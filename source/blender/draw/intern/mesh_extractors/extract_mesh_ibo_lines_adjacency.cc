/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup draw
 */

#include "BLI_edgehash.h"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "draw_subdivision.h"
#include "extract_mesh.hh"

namespace blender::draw {

/* ---------------------------------------------------------------------- */
/** \name Extract Line Adjacency Indices
 * \{ */

#define NO_EDGE INT_MAX

struct MeshExtract_LineAdjacency_Data {
  GPUIndexBufBuilder elb;
  EdgeHash *eh;
  bool is_manifold;
  /* Array to convert vert index to any loop index of this vert. */
  uint *vert_to_loop;
};

static void line_adjacency_data_init(MeshExtract_LineAdjacency_Data *data,
                                     uint vert_len,
                                     uint loop_len,
                                     uint tess_edge_len)
{
  data->vert_to_loop = static_cast<uint *>(MEM_callocN(sizeof(uint) * vert_len, __func__));

  GPU_indexbuf_init(&data->elb, GPU_PRIM_LINES_ADJ, tess_edge_len, loop_len);
  data->eh = BLI_edgehash_new_ex(__func__, tess_edge_len);
  data->is_manifold = true;
}

static void extract_lines_adjacency_init(const MeshRenderData *mr,
                                         MeshBatchCache *UNUSED(cache),
                                         void *UNUSED(buf),
                                         void *tls_data)
{
  /* Similar to poly_to_tri_count().
   * There is always (loop + triangle - 1) edges inside a polygon.
   * Accumulate for all polys and you get : */
  uint tess_edge_len = mr->loop_len + mr->tri_len - mr->poly_len;

  MeshExtract_LineAdjacency_Data *data = static_cast<MeshExtract_LineAdjacency_Data *>(tls_data);
  line_adjacency_data_init(data, mr->vert_len, mr->loop_len, tess_edge_len);
}

BLI_INLINE void lines_adjacency_triangle(
    uint v1, uint v2, uint v3, uint l1, uint l2, uint l3, MeshExtract_LineAdjacency_Data *data)
{
  GPUIndexBufBuilder *elb = &data->elb;
  /* Iterate around the triangle's edges. */
  for (int e = 0; e < 3; e++) {
    SHIFT3(uint, v3, v2, v1);
    SHIFT3(uint, l3, l2, l1);

    bool inv_indices = (v2 > v3);
    void **pval;
    bool value_is_init = BLI_edgehash_ensure_p(data->eh, v2, v3, &pval);
    int v_data = POINTER_AS_INT(*pval);
    if (!value_is_init || v_data == NO_EDGE) {
      /* Save the winding order inside the sign bit. Because the
       * Edge-hash sort the keys and we need to compare winding later. */
      int value = (int)l1 + 1; /* 0 cannot be signed so add one. */
      *pval = POINTER_FROM_INT((inv_indices) ? -value : value);
      /* Store loop indices for remaining non-manifold edges. */
      data->vert_to_loop[v2] = l2;
      data->vert_to_loop[v3] = l3;
    }
    else {
      /* HACK Tag as not used. Prevent overhead of BLI_edgehash_remove. */
      *pval = POINTER_FROM_INT(NO_EDGE);
      bool inv_opposite = (v_data < 0);
      uint l_opposite = (uint)abs(v_data) - 1;
      /* TODO: Make this part thread-safe. */
      if (inv_opposite == inv_indices) {
        /* Don't share edge if triangles have non matching winding. */
        GPU_indexbuf_add_line_adj_verts(elb, l1, l2, l3, l1);
        GPU_indexbuf_add_line_adj_verts(elb, l_opposite, l2, l3, l_opposite);
        data->is_manifold = false;
      }
      else {
        GPU_indexbuf_add_line_adj_verts(elb, l1, l2, l3, l_opposite);
      }
    }
  }
}

static void extract_lines_adjacency_iter_looptri_bm(const MeshRenderData *UNUSED(mr),
                                                    BMLoop **elt,
                                                    const int UNUSED(elt_index),
                                                    void *_data)
{
  MeshExtract_LineAdjacency_Data *data = static_cast<MeshExtract_LineAdjacency_Data *>(_data);
  if (!BM_elem_flag_test(elt[0]->f, BM_ELEM_HIDDEN)) {
    lines_adjacency_triangle(BM_elem_index_get(elt[0]->v),
                             BM_elem_index_get(elt[1]->v),
                             BM_elem_index_get(elt[2]->v),
                             BM_elem_index_get(elt[0]),
                             BM_elem_index_get(elt[1]),
                             BM_elem_index_get(elt[2]),
                             data);
  }
}

static void extract_lines_adjacency_iter_looptri_mesh(const MeshRenderData *mr,
                                                      const MLoopTri *mlt,
                                                      const int UNUSED(elt_index),
                                                      void *_data)
{
  MeshExtract_LineAdjacency_Data *data = static_cast<MeshExtract_LineAdjacency_Data *>(_data);
  const bool hidden = mr->use_hide && mr->hide_poly && mr->hide_poly[mlt->poly];
  if (hidden) {
    return;
  }
  lines_adjacency_triangle(mr->mloop[mlt->tri[0]].v,
                           mr->mloop[mlt->tri[1]].v,
                           mr->mloop[mlt->tri[2]].v,
                           mlt->tri[0],
                           mlt->tri[1],
                           mlt->tri[2],
                           data);
}

static void extract_lines_adjacency_finish(const MeshRenderData *UNUSED(mr),
                                           MeshBatchCache *cache,
                                           void *buf,
                                           void *_data)
{
  GPUIndexBuf *ibo = static_cast<GPUIndexBuf *>(buf);
  MeshExtract_LineAdjacency_Data *data = static_cast<MeshExtract_LineAdjacency_Data *>(_data);
  /* Create edges for remaining non manifold edges. */
  EdgeHashIterator *ehi = BLI_edgehashIterator_new(data->eh);
  for (; !BLI_edgehashIterator_isDone(ehi); BLI_edgehashIterator_step(ehi)) {
    uint v2, v3, l1, l2, l3;
    int v_data = POINTER_AS_INT(BLI_edgehashIterator_getValue(ehi));
    if (v_data != NO_EDGE) {
      BLI_edgehashIterator_getKey(ehi, &v2, &v3);
      l1 = (uint)abs(v_data) - 1;
      if (v_data < 0) { /* `inv_opposite`. */
        SWAP(uint, v2, v3);
      }
      l2 = data->vert_to_loop[v2];
      l3 = data->vert_to_loop[v3];
      GPU_indexbuf_add_line_adj_verts(&data->elb, l1, l2, l3, l1);
      data->is_manifold = false;
    }
  }
  BLI_edgehashIterator_free(ehi);
  BLI_edgehash_free(data->eh, nullptr);

  cache->is_manifold = data->is_manifold;

  GPU_indexbuf_build_in_place(&data->elb, ibo);
  MEM_freeN(data->vert_to_loop);
}

static void extract_lines_adjacency_init_subdiv(const DRWSubdivCache *subdiv_cache,
                                                const MeshRenderData *UNUSED(mr),
                                                MeshBatchCache *UNUSED(cache),
                                                void *UNUSED(buf),
                                                void *_data)
{
  MeshExtract_LineAdjacency_Data *data = static_cast<MeshExtract_LineAdjacency_Data *>(_data);

  /* For each polygon there is (loop + triangle - 1) edges. Since we only have quads, and a quad
   * is split into 2 triangles, we have (loop + 2 - 1) = (loop + 1) edges for each quad, or in
   * total: (number_of_loops + number_of_quads). */
  const uint tess_len = subdiv_cache->num_subdiv_loops + subdiv_cache->num_subdiv_quads;
  line_adjacency_data_init(
      data, subdiv_cache->num_subdiv_verts, subdiv_cache->num_subdiv_loops, tess_len);
}

static void extract_lines_adjacency_iter_subdiv(const DRWSubdivCache *subdiv_cache,
                                                const MeshRenderData *UNUSED(mr),
                                                void *_data,
                                                uint subdiv_quad_index)
{
  MeshExtract_LineAdjacency_Data *data = static_cast<MeshExtract_LineAdjacency_Data *>(_data);

  const uint loop_index = subdiv_quad_index * 4;
  const uint l0 = loop_index + 0;
  const uint l1 = loop_index + 1;
  const uint l2 = loop_index + 2;
  const uint l3 = loop_index + 3;

  const uint v0 = subdiv_cache->subdiv_loop_subdiv_vert_index[l0];
  const uint v1 = subdiv_cache->subdiv_loop_subdiv_vert_index[l1];
  const uint v2 = subdiv_cache->subdiv_loop_subdiv_vert_index[l2];
  const uint v3 = subdiv_cache->subdiv_loop_subdiv_vert_index[l3];

  lines_adjacency_triangle(v0, v1, v2, l0, l1, l2, data);
  lines_adjacency_triangle(v0, v2, v3, l0, l2, l3, data);
}

static void extract_lines_adjacency_iter_subdiv_bm(const DRWSubdivCache *subdiv_cache,
                                                   const MeshRenderData *mr,
                                                   void *_data,
                                                   uint subdiv_quad_index,
                                                   const BMFace *UNUSED(coarse_quad))
{
  extract_lines_adjacency_iter_subdiv(subdiv_cache, mr, _data, subdiv_quad_index);
}

static void extract_lines_adjacency_iter_subdiv_mesh(const DRWSubdivCache *subdiv_cache,
                                                     const MeshRenderData *mr,
                                                     void *_data,
                                                     uint subdiv_quad_index,
                                                     const MPoly *UNUSED(coarse_quad))
{
  extract_lines_adjacency_iter_subdiv(subdiv_cache, mr, _data, subdiv_quad_index);
}

static void extract_lines_adjacency_finish_subdiv(const DRWSubdivCache *UNUSED(subdiv_cache),
                                                  const MeshRenderData *mr,
                                                  MeshBatchCache *cache,
                                                  void *buf,
                                                  void *_data)
{
  extract_lines_adjacency_finish(mr, cache, buf, _data);
}

#undef NO_EDGE

constexpr MeshExtract create_extractor_lines_adjacency()
{
  MeshExtract extractor = {nullptr};
  extractor.init = extract_lines_adjacency_init;
  extractor.iter_looptri_bm = extract_lines_adjacency_iter_looptri_bm;
  extractor.iter_looptri_mesh = extract_lines_adjacency_iter_looptri_mesh;
  extractor.finish = extract_lines_adjacency_finish;
  extractor.init_subdiv = extract_lines_adjacency_init_subdiv;
  extractor.iter_subdiv_bm = extract_lines_adjacency_iter_subdiv_bm;
  extractor.iter_subdiv_mesh = extract_lines_adjacency_iter_subdiv_mesh;
  extractor.finish_subdiv = extract_lines_adjacency_finish_subdiv;
  extractor.data_type = MR_DATA_NONE;
  extractor.data_size = sizeof(MeshExtract_LineAdjacency_Data);
  extractor.use_threading = false;
  extractor.mesh_buffer_offset = offsetof(MeshBufferList, ibo.lines_adjacency);
  return extractor;
}

/** \} */

}  // namespace blender::draw

const MeshExtract extract_lines_adjacency = blender::draw::create_extractor_lines_adjacency();
