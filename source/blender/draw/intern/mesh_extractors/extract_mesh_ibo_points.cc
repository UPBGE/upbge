/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup draw
 */

#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "draw_subdivision.h"
#include "extract_mesh.hh"

namespace blender::draw {

/* ---------------------------------------------------------------------- */
/** \name Extract Point Indices
 * \{ */

static void extract_points_init(const MeshRenderData *mr,
                                MeshBatchCache *UNUSED(cache),
                                void *UNUSED(buf),
                                void *tls_data)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(tls_data);
  GPU_indexbuf_init(elb, GPU_PRIM_POINTS, mr->vert_len, mr->loop_len + mr->loop_loose_len);
}

BLI_INLINE void vert_set_bm(GPUIndexBufBuilder *elb, const BMVert *eve, int l_index)
{
  const int v_index = BM_elem_index_get(eve);
  if (!BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
    GPU_indexbuf_set_point_vert(elb, v_index, l_index);
  }
  else {
    GPU_indexbuf_set_point_restart(elb, v_index);
  }
}

BLI_INLINE void vert_set_mesh(GPUIndexBufBuilder *elb,
                              const MeshRenderData *mr,
                              const int v_index,
                              const int l_index)
{
  const bool hidden = mr->use_hide && mr->hide_vert && mr->hide_vert[v_index];

  if (!(hidden || ((mr->v_origindex) && (mr->v_origindex[v_index] == ORIGINDEX_NONE)))) {
    GPU_indexbuf_set_point_vert(elb, v_index, l_index);
  }
  else {
    GPU_indexbuf_set_point_restart(elb, v_index);
  }
}

static void extract_points_iter_poly_bm(const MeshRenderData *UNUSED(mr),
                                        const BMFace *f,
                                        const int UNUSED(f_index),
                                        void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  BMLoop *l_iter, *l_first;
  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    const int l_index = BM_elem_index_get(l_iter);

    vert_set_bm(elb, l_iter->v, l_index);
  } while ((l_iter = l_iter->next) != l_first);
}

static void extract_points_iter_poly_mesh(const MeshRenderData *mr,
                                          const MPoly *mp,
                                          const int UNUSED(mp_index),
                                          void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  const MLoop *mloop = mr->mloop;
  const int ml_index_end = mp->loopstart + mp->totloop;
  for (int ml_index = mp->loopstart; ml_index < ml_index_end; ml_index += 1) {
    const MLoop *ml = &mloop[ml_index];
    vert_set_mesh(elb, mr, ml->v, ml_index);
  }
}

static void extract_points_iter_ledge_bm(const MeshRenderData *mr,
                                         const BMEdge *eed,
                                         const int ledge_index,
                                         void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  vert_set_bm(elb, eed->v1, mr->loop_len + (ledge_index * 2));
  vert_set_bm(elb, eed->v2, mr->loop_len + (ledge_index * 2) + 1);
}

static void extract_points_iter_ledge_mesh(const MeshRenderData *mr,
                                           const MEdge *med,
                                           const int ledge_index,
                                           void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  vert_set_mesh(elb, mr, med->v1, mr->loop_len + (ledge_index * 2));
  vert_set_mesh(elb, mr, med->v2, mr->loop_len + (ledge_index * 2) + 1);
}

static void extract_points_iter_lvert_bm(const MeshRenderData *mr,
                                         const BMVert *eve,
                                         const int lvert_index,
                                         void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  const int offset = mr->loop_len + (mr->edge_loose_len * 2);
  vert_set_bm(elb, eve, offset + lvert_index);
}

static void extract_points_iter_lvert_mesh(const MeshRenderData *mr,
                                           const MVert *UNUSED(mv),
                                           const int lvert_index,
                                           void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  const int offset = mr->loop_len + (mr->edge_loose_len * 2);
  vert_set_mesh(elb, mr, mr->lverts[lvert_index], offset + lvert_index);
}

static void extract_points_task_reduce(void *_userdata_to, void *_userdata_from)
{
  GPUIndexBufBuilder *elb_to = static_cast<GPUIndexBufBuilder *>(_userdata_to);
  GPUIndexBufBuilder *elb_from = static_cast<GPUIndexBufBuilder *>(_userdata_from);
  GPU_indexbuf_join(elb_to, elb_from);
}

static void extract_points_finish(const MeshRenderData *UNUSED(mr),
                                  MeshBatchCache *UNUSED(cache),
                                  void *buf,
                                  void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  GPUIndexBuf *ibo = static_cast<GPUIndexBuf *>(buf);
  GPU_indexbuf_build_in_place(elb, ibo);
}

static void extract_points_init_subdiv(const DRWSubdivCache *subdiv_cache,
                                       const MeshRenderData *mr,
                                       MeshBatchCache *UNUSED(cache),
                                       void *UNUSED(buffer),
                                       void *data)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(data);
  GPU_indexbuf_init(elb,
                    GPU_PRIM_POINTS,
                    mr->vert_len,
                    subdiv_cache->num_subdiv_loops + subdiv_cache->loose_geom.loop_len);
}

static void extract_points_iter_subdiv_common(GPUIndexBufBuilder *elb,
                                              const MeshRenderData *mr,
                                              const DRWSubdivCache *subdiv_cache,
                                              uint subdiv_quad_index,
                                              bool for_bmesh)
{
  int *subdiv_loop_vert_index = (int *)GPU_vertbuf_get_data(subdiv_cache->verts_orig_index);
  uint start_loop_idx = subdiv_quad_index * 4;
  uint end_loop_idx = (subdiv_quad_index + 1) * 4;
  for (uint i = start_loop_idx; i < end_loop_idx; i++) {
    int coarse_vertex_index = subdiv_loop_vert_index[i];

    if (coarse_vertex_index == -1) {
      continue;
    }

    if (mr->v_origindex && mr->v_origindex[coarse_vertex_index] == -1) {
      continue;
    }

    if (for_bmesh) {
      const BMVert *mv = BM_vert_at_index(mr->bm, coarse_vertex_index);
      if (BM_elem_flag_test(mv, BM_ELEM_HIDDEN)) {
        GPU_indexbuf_set_point_restart(elb, coarse_vertex_index);
        continue;
      }
    }
    else {
      if (mr->use_hide && mr->hide_vert && mr->hide_vert[coarse_vertex_index]) {
        GPU_indexbuf_set_point_restart(elb, coarse_vertex_index);
        continue;
      }
    }

    GPU_indexbuf_set_point_vert(elb, coarse_vertex_index, i);
  }
}

static void extract_points_iter_subdiv_bm(const DRWSubdivCache *subdiv_cache,
                                          const MeshRenderData *mr,
                                          void *_data,
                                          uint subdiv_quad_index,
                                          const BMFace *UNUSED(coarse_quad))
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_data);
  extract_points_iter_subdiv_common(elb, mr, subdiv_cache, subdiv_quad_index, true);
}

static void extract_points_iter_subdiv_mesh(const DRWSubdivCache *subdiv_cache,
                                            const MeshRenderData *mr,
                                            void *_data,
                                            uint subdiv_quad_index,
                                            const MPoly *UNUSED(coarse_quad))
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_data);
  extract_points_iter_subdiv_common(elb, mr, subdiv_cache, subdiv_quad_index, false);
}

static void extract_points_loose_geom_subdiv(const DRWSubdivCache *subdiv_cache,
                                             const MeshRenderData *mr,
                                             void *UNUSED(buffer),
                                             void *data)
{
  const DRWSubdivLooseGeom &loose_geom = subdiv_cache->loose_geom;
  const int loop_loose_len = loose_geom.loop_len;
  if (loop_loose_len == 0) {
    return;
  }

  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(data);

  uint offset = subdiv_cache->num_subdiv_loops;

  if (mr->extract_type != MR_EXTRACT_BMESH) {
    blender::Span<DRWSubdivLooseEdge> loose_edges = draw_subdiv_cache_get_loose_edges(
        subdiv_cache);

    for (const DRWSubdivLooseEdge &loose_edge : loose_edges) {
      const DRWSubdivLooseVertex &v1 = loose_geom.verts[loose_edge.loose_subdiv_v1_index];
      const DRWSubdivLooseVertex &v2 = loose_geom.verts[loose_edge.loose_subdiv_v2_index];
      if (v1.coarse_vertex_index != -1u) {
        vert_set_mesh(elb, mr, v1.coarse_vertex_index, offset);
      }
      if (v2.coarse_vertex_index != -1u) {
        vert_set_mesh(elb, mr, v2.coarse_vertex_index, offset + 1);
      }

      offset += 2;
    }
    blender::Span<DRWSubdivLooseVertex> loose_verts = draw_subdiv_cache_get_loose_verts(
        subdiv_cache);

    for (const DRWSubdivLooseVertex &loose_vert : loose_verts) {
      vert_set_mesh(elb, mr, loose_vert.coarse_vertex_index, offset);
      offset += 1;
    }
  }
  else {
    blender::Span<DRWSubdivLooseEdge> loose_edges = draw_subdiv_cache_get_loose_edges(
        subdiv_cache);

    for (const DRWSubdivLooseEdge &loose_edge : loose_edges) {
      const DRWSubdivLooseVertex &v1 = loose_geom.verts[loose_edge.loose_subdiv_v1_index];
      const DRWSubdivLooseVertex &v2 = loose_geom.verts[loose_edge.loose_subdiv_v2_index];
      if (v1.coarse_vertex_index != -1u) {
        BMVert *eve = mr->v_origindex ? bm_original_vert_get(mr, v1.coarse_vertex_index) :
                                        BM_vert_at_index(mr->bm, v1.coarse_vertex_index);
        vert_set_bm(elb, eve, offset);
      }
      if (v2.coarse_vertex_index != -1u) {
        BMVert *eve = mr->v_origindex ? bm_original_vert_get(mr, v2.coarse_vertex_index) :
                                        BM_vert_at_index(mr->bm, v2.coarse_vertex_index);
        vert_set_bm(elb, eve, offset + 1);
      }

      offset += 2;
    }
    blender::Span<DRWSubdivLooseVertex> loose_verts = draw_subdiv_cache_get_loose_verts(
        subdiv_cache);

    for (const DRWSubdivLooseVertex &loose_vert : loose_verts) {
      BMVert *eve = mr->v_origindex ? bm_original_vert_get(mr, loose_vert.coarse_vertex_index) :
                                      BM_vert_at_index(mr->bm, loose_vert.coarse_vertex_index);
      vert_set_bm(elb, eve, offset);
      offset += 1;
    }
  }
}

static void extract_points_finish_subdiv(const DRWSubdivCache *UNUSED(subdiv_cache),
                                         const MeshRenderData *UNUSED(mr),
                                         MeshBatchCache *UNUSED(cache),
                                         void *buf,
                                         void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  GPUIndexBuf *ibo = static_cast<GPUIndexBuf *>(buf);
  GPU_indexbuf_build_in_place(elb, ibo);
}

constexpr MeshExtract create_extractor_points()
{
  MeshExtract extractor = {nullptr};
  extractor.init = extract_points_init;
  extractor.iter_poly_bm = extract_points_iter_poly_bm;
  extractor.iter_poly_mesh = extract_points_iter_poly_mesh;
  extractor.iter_ledge_bm = extract_points_iter_ledge_bm;
  extractor.iter_ledge_mesh = extract_points_iter_ledge_mesh;
  extractor.iter_lvert_bm = extract_points_iter_lvert_bm;
  extractor.iter_lvert_mesh = extract_points_iter_lvert_mesh;
  extractor.task_reduce = extract_points_task_reduce;
  extractor.finish = extract_points_finish;
  extractor.init_subdiv = extract_points_init_subdiv;
  extractor.iter_subdiv_bm = extract_points_iter_subdiv_bm;
  extractor.iter_subdiv_mesh = extract_points_iter_subdiv_mesh;
  extractor.iter_loose_geom_subdiv = extract_points_loose_geom_subdiv;
  extractor.finish_subdiv = extract_points_finish_subdiv;
  extractor.use_threading = true;
  extractor.data_type = MR_DATA_NONE;
  extractor.data_size = sizeof(GPUIndexBufBuilder);
  extractor.mesh_buffer_offset = offsetof(MeshBufferList, ibo.points);
  return extractor;
}

/** \} */

}  // namespace blender::draw

const MeshExtract extract_points = blender::draw::create_extractor_points();
