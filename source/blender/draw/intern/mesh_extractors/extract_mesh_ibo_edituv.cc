/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup draw
 */

#include "BLI_bitmap.h"

#include "extract_mesh.hh"

#include "draw_subdivision.h"

namespace blender::draw {
/* ---------------------------------------------------------------------- */
/** \name Extract Edit UV Triangles Indices
 * \{ */

struct MeshExtract_EditUvElem_Data {
  GPUIndexBufBuilder elb;
  bool sync_selection;
};

static void extract_edituv_tris_init(const MeshRenderData *mr,
                                     MeshBatchCache *UNUSED(cache),
                                     void *UNUSED(ibo),
                                     void *tls_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(tls_data);
  GPU_indexbuf_init(&data->elb, GPU_PRIM_TRIS, mr->tri_len, mr->loop_len);
  data->sync_selection = (mr->toolsettings->uv_flag & UV_SYNC_SELECTION) != 0;
}

BLI_INLINE void edituv_tri_add(
    MeshExtract_EditUvElem_Data *data, bool hidden, bool selected, int v1, int v2, int v3)
{
  if (!hidden && (data->sync_selection || selected)) {
    GPU_indexbuf_add_tri_verts(&data->elb, v1, v2, v3);
  }
}

static void extract_edituv_tris_iter_looptri_bm(const MeshRenderData *UNUSED(mr),
                                                BMLoop **elt,
                                                const int UNUSED(elt_index),
                                                void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  edituv_tri_add(data,
                 BM_elem_flag_test(elt[0]->f, BM_ELEM_HIDDEN),
                 BM_elem_flag_test(elt[0]->f, BM_ELEM_SELECT),
                 BM_elem_index_get(elt[0]),
                 BM_elem_index_get(elt[1]),
                 BM_elem_index_get(elt[2]));
}

static void extract_edituv_tris_iter_looptri_mesh(const MeshRenderData *mr,
                                                  const MLoopTri *mlt,
                                                  const int UNUSED(elt_index),
                                                  void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  const BMFace *efa = bm_original_face_get(mr, mlt->poly);
  const bool mp_hidden = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_HIDDEN) : true;
  const bool mp_select = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_SELECT) : false;

  edituv_tri_add(data, mp_hidden, mp_select, mlt->tri[0], mlt->tri[1], mlt->tri[2]);
}

static void extract_edituv_tris_finish(const MeshRenderData *UNUSED(mr),
                                       MeshBatchCache *UNUSED(cache),
                                       void *buf,
                                       void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  GPUIndexBuf *ibo = static_cast<GPUIndexBuf *>(buf);
  GPU_indexbuf_build_in_place(&data->elb, ibo);
}

static void extract_edituv_tris_init_subdiv(const DRWSubdivCache *subdiv_cache,
                                            const MeshRenderData *mr,
                                            MeshBatchCache *UNUSED(cache),
                                            void *UNUSED(buf),
                                            void *tls_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(tls_data);
  GPU_indexbuf_init(&data->elb,
                    GPU_PRIM_TRIS,
                    subdiv_cache->num_subdiv_triangles,
                    subdiv_cache->num_subdiv_loops);
  data->sync_selection = (mr->toolsettings->uv_flag & UV_SYNC_SELECTION) != 0;
}

static void extract_edituv_tris_iter_subdiv_bm(const DRWSubdivCache *UNUSED(subdiv_cache),
                                               const MeshRenderData *UNUSED(mr),
                                               void *_data,
                                               uint subdiv_quad_index,
                                               const BMFace *coarse_quad)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  const uint loop_idx = subdiv_quad_index * 4;

  edituv_tri_add(data,
                 BM_elem_flag_test(coarse_quad, BM_ELEM_HIDDEN) != 0,
                 BM_elem_flag_test(coarse_quad, BM_ELEM_SELECT) != 0,
                 loop_idx,
                 loop_idx + 1,
                 loop_idx + 2);

  edituv_tri_add(data,
                 BM_elem_flag_test(coarse_quad, BM_ELEM_HIDDEN) != 0,
                 BM_elem_flag_test(coarse_quad, BM_ELEM_SELECT) != 0,
                 loop_idx,
                 loop_idx + 2,
                 loop_idx + 3);
}

static void extract_edituv_tris_iter_subdiv_mesh(const DRWSubdivCache *UNUSED(subdiv_cache),
                                                 const MeshRenderData *mr,
                                                 void *_data,
                                                 uint subdiv_quad_index,
                                                 const MPoly *coarse_quad)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  const uint loop_idx = subdiv_quad_index * 4;

  const BMFace *efa = bm_original_face_get(mr, coarse_quad - mr->mpoly);
  const bool mp_hidden = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_HIDDEN) : true;
  const bool mp_select = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_SELECT) : false;

  edituv_tri_add(data, mp_hidden, mp_select, loop_idx, loop_idx + 1, loop_idx + 2);
  edituv_tri_add(data, mp_hidden, mp_select, loop_idx, loop_idx + 2, loop_idx + 3);
}

static void extract_edituv_tris_finish_subdiv(const struct DRWSubdivCache *UNUSED(subdiv_cache),
                                              const MeshRenderData *UNUSED(mr),
                                              MeshBatchCache *UNUSED(cache),
                                              void *buf,
                                              void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  GPUIndexBuf *ibo = static_cast<GPUIndexBuf *>(buf);
  GPU_indexbuf_build_in_place(&data->elb, ibo);
}

constexpr MeshExtract create_extractor_edituv_tris()
{
  MeshExtract extractor = {nullptr};
  extractor.init = extract_edituv_tris_init;
  extractor.iter_looptri_bm = extract_edituv_tris_iter_looptri_bm;
  extractor.iter_looptri_mesh = extract_edituv_tris_iter_looptri_mesh;
  extractor.finish = extract_edituv_tris_finish;
  extractor.init_subdiv = extract_edituv_tris_init_subdiv;
  extractor.iter_subdiv_bm = extract_edituv_tris_iter_subdiv_bm;
  extractor.iter_subdiv_mesh = extract_edituv_tris_iter_subdiv_mesh;
  extractor.finish_subdiv = extract_edituv_tris_finish_subdiv;
  extractor.data_type = MR_DATA_NONE;
  extractor.data_size = sizeof(MeshExtract_EditUvElem_Data);
  extractor.use_threading = false;
  extractor.mesh_buffer_offset = offsetof(MeshBufferList, ibo.edituv_tris);
  return extractor;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Extract Edit UV Line Indices around faces
 * \{ */

static void extract_edituv_lines_init(const MeshRenderData *mr,
                                      MeshBatchCache *UNUSED(cache),
                                      void *UNUSED(ibo),
                                      void *tls_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(tls_data);
  GPU_indexbuf_init(&data->elb, GPU_PRIM_LINES, mr->loop_len, mr->loop_len);
  data->sync_selection = (mr->toolsettings->uv_flag & UV_SYNC_SELECTION) != 0;
}

BLI_INLINE void edituv_edge_add(
    MeshExtract_EditUvElem_Data *data, bool hidden, bool selected, int v1, int v2)
{
  if (!hidden && (data->sync_selection || selected)) {
    GPU_indexbuf_add_line_verts(&data->elb, v1, v2);
  }
}

static void extract_edituv_lines_iter_poly_bm(const MeshRenderData *UNUSED(mr),
                                              const BMFace *f,
                                              const int UNUSED(f_index),
                                              void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  BMLoop *l_iter, *l_first;
  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    const int l_index = BM_elem_index_get(l_iter);

    edituv_edge_add(data,
                    BM_elem_flag_test_bool(f, BM_ELEM_HIDDEN),
                    BM_elem_flag_test_bool(f, BM_ELEM_SELECT),
                    l_index,
                    BM_elem_index_get(l_iter->next));
  } while ((l_iter = l_iter->next) != l_first);
}

static void extract_edituv_lines_iter_poly_mesh(const MeshRenderData *mr,
                                                const MPoly *mp,
                                                const int mp_index,
                                                void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  const MLoop *mloop = mr->mloop;
  const int ml_index_end = mp->loopstart + mp->totloop;

  const BMFace *efa = bm_original_face_get(mr, mp_index);
  const bool mp_hidden = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_HIDDEN) : true;
  const bool mp_select = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_SELECT) : false;

  for (int ml_index = mp->loopstart; ml_index < ml_index_end; ml_index += 1) {
    const MLoop *ml = &mloop[ml_index];

    const int ml_index_last = mp->totloop + mp->loopstart - 1;
    const int ml_index_next = (ml_index == ml_index_last) ? mp->loopstart : (ml_index + 1);
    const bool real_edge = (mr->e_origindex == nullptr ||
                            mr->e_origindex[ml->e] != ORIGINDEX_NONE);
    edituv_edge_add(data, mp_hidden || !real_edge, mp_select, ml_index, ml_index_next);
  }
}

static void extract_edituv_lines_finish(const MeshRenderData *UNUSED(mr),
                                        MeshBatchCache *UNUSED(cache),
                                        void *buf,
                                        void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  GPUIndexBuf *ibo = static_cast<GPUIndexBuf *>(buf);
  GPU_indexbuf_build_in_place(&data->elb, ibo);
}

static void extract_edituv_lines_init_subdiv(const DRWSubdivCache *subdiv_cache,
                                             const MeshRenderData *mr,
                                             MeshBatchCache *UNUSED(cache),
                                             void *UNUSED(buf),
                                             void *tls_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(tls_data);
  GPU_indexbuf_init(
      &data->elb, GPU_PRIM_LINES, subdiv_cache->num_subdiv_loops, subdiv_cache->num_subdiv_loops);
  data->sync_selection = (mr->toolsettings->uv_flag & UV_SYNC_SELECTION) != 0;
}

static void extract_edituv_lines_iter_subdiv_bm(const DRWSubdivCache *subdiv_cache,
                                                const MeshRenderData *mr,
                                                void *_data,
                                                uint subdiv_quad_index,
                                                const BMFace *coarse_poly)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  int *subdiv_loop_edge_index = (int *)GPU_vertbuf_get_data(subdiv_cache->edges_orig_index);

  const bool mp_hidden = BM_elem_flag_test_bool(coarse_poly, BM_ELEM_HIDDEN);
  const bool mp_select = BM_elem_flag_test_bool(coarse_poly, BM_ELEM_SELECT);

  uint start_loop_idx = subdiv_quad_index * 4;
  uint end_loop_idx = (subdiv_quad_index + 1) * 4;
  for (uint loop_idx = start_loop_idx; loop_idx < end_loop_idx; loop_idx++) {
    const int edge_origindex = subdiv_loop_edge_index[loop_idx];
    const bool real_edge = (edge_origindex != -1 &&
                            (mr->e_origindex == nullptr ||
                             mr->e_origindex[edge_origindex] != ORIGINDEX_NONE));
    edituv_edge_add(data,
                    mp_hidden || !real_edge,
                    mp_select,
                    loop_idx,
                    (loop_idx + 1 == end_loop_idx) ? start_loop_idx : (loop_idx + 1));
  }
}

static void extract_edituv_lines_iter_subdiv_mesh(const DRWSubdivCache *subdiv_cache,
                                                  const MeshRenderData *mr,
                                                  void *_data,
                                                  uint subdiv_quad_index,
                                                  const MPoly *coarse_poly)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  int *subdiv_loop_edge_index = (int *)GPU_vertbuf_get_data(subdiv_cache->edges_orig_index);

  const BMFace *efa = bm_original_face_get(mr, coarse_poly - mr->mpoly);
  const bool mp_hidden = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_HIDDEN) : true;
  const bool mp_select = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_SELECT) : false;

  uint start_loop_idx = subdiv_quad_index * 4;
  uint end_loop_idx = (subdiv_quad_index + 1) * 4;
  for (uint loop_idx = start_loop_idx; loop_idx < end_loop_idx; loop_idx++) {
    const int edge_origindex = subdiv_loop_edge_index[loop_idx];
    const bool real_edge = (edge_origindex != -1 &&
                            (mr->e_origindex == nullptr ||
                             mr->e_origindex[edge_origindex] != ORIGINDEX_NONE));
    edituv_edge_add(data,
                    mp_hidden || !real_edge,
                    mp_select,
                    loop_idx,
                    (loop_idx + 1 == end_loop_idx) ? start_loop_idx : (loop_idx + 1));
  }
}

static void extract_edituv_lines_finish_subdiv(const struct DRWSubdivCache *UNUSED(subdiv_cache),
                                               const MeshRenderData *UNUSED(mr),
                                               MeshBatchCache *UNUSED(cache),
                                               void *buf,
                                               void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  GPUIndexBuf *ibo = static_cast<GPUIndexBuf *>(buf);
  GPU_indexbuf_build_in_place(&data->elb, ibo);
}

constexpr MeshExtract create_extractor_edituv_lines()
{
  MeshExtract extractor = {nullptr};
  extractor.init = extract_edituv_lines_init;
  extractor.iter_poly_bm = extract_edituv_lines_iter_poly_bm;
  extractor.iter_poly_mesh = extract_edituv_lines_iter_poly_mesh;
  extractor.finish = extract_edituv_lines_finish;
  extractor.init_subdiv = extract_edituv_lines_init_subdiv;
  extractor.iter_subdiv_bm = extract_edituv_lines_iter_subdiv_bm;
  extractor.iter_subdiv_mesh = extract_edituv_lines_iter_subdiv_mesh;
  extractor.finish_subdiv = extract_edituv_lines_finish_subdiv;
  extractor.data_type = MR_DATA_NONE;
  extractor.data_size = sizeof(MeshExtract_EditUvElem_Data);
  extractor.use_threading = false;
  extractor.mesh_buffer_offset = offsetof(MeshBufferList, ibo.edituv_lines);
  return extractor;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Extract Edit UV Points Indices
 * \{ */

static void extract_edituv_points_init(const MeshRenderData *mr,
                                       MeshBatchCache *UNUSED(cache),
                                       void *UNUSED(ibo),
                                       void *tls_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(tls_data);
  GPU_indexbuf_init(&data->elb, GPU_PRIM_POINTS, mr->loop_len, mr->loop_len);
  data->sync_selection = (mr->toolsettings->uv_flag & UV_SYNC_SELECTION) != 0;
}

BLI_INLINE void edituv_point_add(MeshExtract_EditUvElem_Data *data,
                                 bool hidden,
                                 bool selected,
                                 int v1)
{
  if (!hidden && (data->sync_selection || selected)) {
    GPU_indexbuf_add_point_vert(&data->elb, v1);
  }
}

static void extract_edituv_points_iter_poly_bm(const MeshRenderData *UNUSED(mr),
                                               const BMFace *f,
                                               const int UNUSED(f_index),
                                               void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  BMLoop *l_iter, *l_first;
  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    const int l_index = BM_elem_index_get(l_iter);

    edituv_point_add(
        data, BM_elem_flag_test(f, BM_ELEM_HIDDEN), BM_elem_flag_test(f, BM_ELEM_SELECT), l_index);
  } while ((l_iter = l_iter->next) != l_first);
}

static void extract_edituv_points_iter_poly_mesh(const MeshRenderData *mr,
                                                 const MPoly *mp,
                                                 const int mp_index,
                                                 void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);

  const BMFace *efa = bm_original_face_get(mr, mp_index);
  const bool mp_hidden = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_HIDDEN) : true;
  const bool mp_select = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_SELECT) : false;

  const MLoop *mloop = mr->mloop;
  const int ml_index_end = mp->loopstart + mp->totloop;
  for (int ml_index = mp->loopstart; ml_index < ml_index_end; ml_index += 1) {
    const MLoop *ml = &mloop[ml_index];

    const bool real_vert = !mr->v_origindex || mr->v_origindex[ml->v] != ORIGINDEX_NONE;
    edituv_point_add(data, mp_hidden || !real_vert, mp_select, ml_index);
  }
}

static void extract_edituv_points_finish(const MeshRenderData *UNUSED(mr),
                                         MeshBatchCache *UNUSED(cache),
                                         void *buf,
                                         void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  GPUIndexBuf *ibo = static_cast<GPUIndexBuf *>(buf);
  GPU_indexbuf_build_in_place(&data->elb, ibo);
}

static void extract_edituv_points_init_subdiv(const DRWSubdivCache *subdiv_cache,
                                              const MeshRenderData *mr,
                                              MeshBatchCache *UNUSED(cache),
                                              void *UNUSED(buf),
                                              void *tls_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(tls_data);
  GPU_indexbuf_init(
      &data->elb, GPU_PRIM_POINTS, subdiv_cache->num_subdiv_loops, subdiv_cache->num_subdiv_loops);
  data->sync_selection = (mr->toolsettings->uv_flag & UV_SYNC_SELECTION) != 0;
}

static void extract_edituv_points_iter_subdiv_bm(const DRWSubdivCache *subdiv_cache,
                                                 const MeshRenderData *UNUSED(mr),
                                                 void *_data,
                                                 uint subdiv_quad_index,
                                                 const BMFace *coarse_quad)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  int *subdiv_loop_vert_index = (int *)GPU_vertbuf_get_data(subdiv_cache->verts_orig_index);

  uint start_loop_idx = subdiv_quad_index * 4;
  uint end_loop_idx = (subdiv_quad_index + 1) * 4;
  for (uint i = start_loop_idx; i < end_loop_idx; i++) {
    const int vert_origindex = subdiv_loop_vert_index[i];
    edituv_point_add(data,
                     (BM_elem_flag_test(coarse_quad, BM_ELEM_HIDDEN) || vert_origindex == -1),
                     BM_elem_flag_test(coarse_quad, BM_ELEM_SELECT) != 0,
                     i);
  }
}

static void extract_edituv_points_iter_subdiv_mesh(const DRWSubdivCache *subdiv_cache,
                                                   const MeshRenderData *mr,
                                                   void *_data,
                                                   uint subdiv_quad_index,
                                                   const MPoly *coarse_quad)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  int *subdiv_loop_vert_index = (int *)GPU_vertbuf_get_data(subdiv_cache->verts_orig_index);

  const BMFace *efa = bm_original_face_get(mr, coarse_quad - mr->mpoly);
  const bool mp_hidden = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_HIDDEN) : true;
  const bool mp_select = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_SELECT) : false;

  uint start_loop_idx = subdiv_quad_index * 4;
  uint end_loop_idx = (subdiv_quad_index + 1) * 4;
  for (uint i = start_loop_idx; i < end_loop_idx; i++) {
    const int vert_origindex = subdiv_loop_vert_index[i];
    const bool real_vert = !mr->v_origindex || (vert_origindex != -1 &&
                                                mr->v_origindex[vert_origindex] != ORIGINDEX_NONE);
    edituv_point_add(data, mp_hidden || !real_vert, mp_select, i);
  }
}

static void extract_edituv_points_finish_subdiv(const struct DRWSubdivCache *UNUSED(subdiv_cache),
                                                const MeshRenderData *UNUSED(mr),
                                                MeshBatchCache *UNUSED(cache),
                                                void *buf,
                                                void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  GPUIndexBuf *ibo = static_cast<GPUIndexBuf *>(buf);
  GPU_indexbuf_build_in_place(&data->elb, ibo);
}

constexpr MeshExtract create_extractor_edituv_points()
{
  MeshExtract extractor = {nullptr};
  extractor.init = extract_edituv_points_init;
  extractor.iter_poly_bm = extract_edituv_points_iter_poly_bm;
  extractor.iter_poly_mesh = extract_edituv_points_iter_poly_mesh;
  extractor.finish = extract_edituv_points_finish;
  extractor.init_subdiv = extract_edituv_points_init_subdiv;
  extractor.iter_subdiv_bm = extract_edituv_points_iter_subdiv_bm;
  extractor.iter_subdiv_mesh = extract_edituv_points_iter_subdiv_mesh;
  extractor.finish_subdiv = extract_edituv_points_finish_subdiv;
  extractor.data_type = MR_DATA_NONE;
  extractor.data_size = sizeof(MeshExtract_EditUvElem_Data);
  extractor.use_threading = false;
  extractor.mesh_buffer_offset = offsetof(MeshBufferList, ibo.edituv_points);
  return extractor;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Extract Edit UV Face-dots Indices
 * \{ */

static void extract_edituv_fdots_init(const MeshRenderData *mr,
                                      MeshBatchCache *UNUSED(cache),
                                      void *UNUSED(ibo),
                                      void *tls_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(tls_data);
  GPU_indexbuf_init(&data->elb, GPU_PRIM_POINTS, mr->poly_len, mr->poly_len);
  data->sync_selection = (mr->toolsettings->uv_flag & UV_SYNC_SELECTION) != 0;
}

BLI_INLINE void edituv_facedot_add(MeshExtract_EditUvElem_Data *data,
                                   bool hidden,
                                   bool selected,
                                   int face_index)
{
  if (!hidden && (data->sync_selection || selected)) {
    GPU_indexbuf_set_point_vert(&data->elb, face_index, face_index);
  }
  else {
    GPU_indexbuf_set_point_restart(&data->elb, face_index);
  }
}

static void extract_edituv_fdots_iter_poly_bm(const MeshRenderData *UNUSED(mr),
                                              const BMFace *f,
                                              const int f_index,
                                              void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  edituv_facedot_add(data,
                     BM_elem_flag_test_bool(f, BM_ELEM_HIDDEN),
                     BM_elem_flag_test_bool(f, BM_ELEM_SELECT),
                     f_index);
}

static void extract_edituv_fdots_iter_poly_mesh(const MeshRenderData *mr,
                                                const MPoly *mp,
                                                const int mp_index,
                                                void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);

  const BMFace *efa = bm_original_face_get(mr, mp_index);
  const bool mp_hidden = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_HIDDEN) : true;
  const bool mp_select = (efa) ? BM_elem_flag_test_bool(efa, BM_ELEM_SELECT) : false;

  if (mr->use_subsurf_fdots) {
    const BLI_bitmap *facedot_tags = mr->me->runtime.subsurf_face_dot_tags;

    const MLoop *mloop = mr->mloop;
    const int ml_index_end = mp->loopstart + mp->totloop;
    for (int ml_index = mp->loopstart; ml_index < ml_index_end; ml_index += 1) {
      const MLoop *ml = &mloop[ml_index];

      const bool real_fdot = !mr->p_origindex || (mr->p_origindex[mp_index] != ORIGINDEX_NONE);
      const bool subd_fdot = BLI_BITMAP_TEST(facedot_tags, ml->v);
      edituv_facedot_add(data, mp_hidden || !real_fdot || !subd_fdot, mp_select, mp_index);
    }
  }
  else {
    const bool real_fdot = !mr->p_origindex || (mr->p_origindex[mp_index] != ORIGINDEX_NONE);
    edituv_facedot_add(data, mp_hidden || !real_fdot, mp_select, mp_index);
  }
}

static void extract_edituv_fdots_finish(const MeshRenderData *UNUSED(mr),
                                        MeshBatchCache *UNUSED(cache),
                                        void *buf,
                                        void *_data)
{
  MeshExtract_EditUvElem_Data *data = static_cast<MeshExtract_EditUvElem_Data *>(_data);
  GPUIndexBuf *ibo = static_cast<GPUIndexBuf *>(buf);
  GPU_indexbuf_build_in_place(&data->elb, ibo);
}

constexpr MeshExtract create_extractor_edituv_fdots()
{
  MeshExtract extractor = {nullptr};
  extractor.init = extract_edituv_fdots_init;
  extractor.iter_poly_bm = extract_edituv_fdots_iter_poly_bm;
  extractor.iter_poly_mesh = extract_edituv_fdots_iter_poly_mesh;
  extractor.finish = extract_edituv_fdots_finish;
  extractor.data_type = MR_DATA_NONE;
  extractor.data_size = sizeof(MeshExtract_EditUvElem_Data);
  extractor.use_threading = false;
  extractor.mesh_buffer_offset = offsetof(MeshBufferList, ibo.edituv_fdots);
  return extractor;
}

/** \} */

}  // namespace blender::draw

const MeshExtract extract_edituv_tris = blender::draw::create_extractor_edituv_tris();
const MeshExtract extract_edituv_lines = blender::draw::create_extractor_edituv_lines();
const MeshExtract extract_edituv_points = blender::draw::create_extractor_edituv_points();
const MeshExtract extract_edituv_fdots = blender::draw::create_extractor_edituv_fdots();
