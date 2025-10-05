/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "extract_mesh.hh"

namespace blender::draw {

gpu::VertBufPtr extract_orco(const MeshRenderData &mr)
{
  /* Orco is stored per-vertex. If CD_ORCO layer is missing, fall back to vertex positions
   * transformed by BKE_mesh_orco_verts_transform (same approach as tangent extractor). */
  Span<float3> orco_span;
  Array<float3> orco_allocated;

  if (mr.mesh) {
    if (const float3 *orco_ptr = static_cast<const float3 *>(
            CustomData_get_layer(&mr.mesh->vert_data, CD_ORCO)))
    {
      orco_span = Span(orco_ptr, mr.verts_num);
    }
    else {
      /* Fallback: copy vertex positions and compute orco transform. */
      orco_allocated = mr.vert_positions;
      /* This writes into the local array only. */
      BKE_mesh_orco_verts_transform(const_cast<Mesh *>(mr.mesh), orco_allocated, false);
      orco_span = orco_allocated;
    }
  }
  else {
    /* Defensive fallback if no mesh pointer (should not normally happen). */
    orco_allocated = mr.vert_positions;
    orco_span = orco_allocated;
  }

  /* VBO is per-corner (loops). Keep float4 format for compatibility with shaders. */
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "orco", gpu::VertAttrType::SFLOAT_32_32_32_32);

  gpu::VertBufPtr vbo = gpu::VertBufPtr(GPU_vertbuf_create_with_format(format));
  GPU_vertbuf_data_alloc(*vbo, mr.corners_num);
  MutableSpan vbo_data = vbo->data<float4>();

  const int64_t bytes = orco_span.size_in_bytes() + vbo_data.size_in_bytes();
  threading::memory_bandwidth_bound_task(bytes, [&]() {
    if (mr.extract_type == MeshExtractType::BMesh) {
      const BMesh &bm = *mr.bm;
      threading::parallel_for(IndexRange(bm.totface), 2048, [&](const IndexRange range) {
        for (const int face_index : range) {
          const BMFace &face = *BM_face_at_index(&const_cast<BMesh &>(bm), face_index);
          const BMLoop *loop = BM_FACE_FIRST_LOOP(&face);
          for ([[maybe_unused]] const int i : IndexRange(face.len)) {
            const int loop_index = BM_elem_index_get(loop);
            const int vert_index = BM_elem_index_get(loop->v);
            vbo_data[loop_index] = float4(orco_span[vert_index], 0.0f);
            loop = loop->next;
          }
        }
      });
    }
    else {
      const Span<int> corner_verts = mr.corner_verts;
      threading::parallel_for(corner_verts.index_range(), 4096, [&](const IndexRange range) {
        for (const int corner : range) {
          const int vert_index = corner_verts[corner];
          vbo_data[corner] = float4(orco_span[vert_index], 0.0f);
        }
      });
    }
  });
  return vbo;
}

}  // namespace blender::draw
