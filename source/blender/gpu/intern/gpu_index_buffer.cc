/* SPDX-FileCopyrightText: 2016 by Mike Erwin. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * GPU element list (AKA index buffer)
 */

#include "MEM_guardedalloc.h"

#include "BLI_array_utils.hh"
#include "BLI_math_base.h"
#include "BLI_utildefines.h"

#include "gpu_backend.hh"

#include "GPU_index_buffer.hh"

#include "GPU_capabilities.hh"
#include "GPU_compute.hh"
#include "GPU_platform.hh"
#include "GPU_state.hh"

#include <algorithm> /* For `min/max`. */
#include <cstring>

/* -------------------------------------------------------------------- */
/** \name IndexBufBuilder
 * \{ */

using namespace blender;
using namespace blender::gpu;

void GPU_indexbuf_init_ex(GPUIndexBufBuilder *builder,
                          GPUPrimType prim_type,
                          uint index_len,
                          uint vertex_len)
{
  builder->max_allowed_index = vertex_len - 1;
  builder->max_index_len = index_len;
  builder->index_len = 0;  // start empty
  builder->index_min = UINT32_MAX;
  builder->index_max = 0;
  builder->prim_type = prim_type;

#ifdef __APPLE__
  /* Only encode restart indices for restart-compatible primitive types.
   * Resolves out-of-bounds read error on macOS. Using 0-index will ensure
   * degenerative primitives when skipping primitives is required and will
   * incur no additional performance cost for rendering. */
  if (GPU_type_matches_ex(GPU_DEVICE_ANY, GPU_OS_MAC, GPU_DRIVER_ANY, GPU_BACKEND_METAL)) {
    /* We will still use restart-indices for point primitives and then
     * patch these during IndexBuf::init, as we cannot benefit from degenerative
     * primitives to eliminate these. */
    builder->restart_index_value = (is_restart_compatible(prim_type) ||
                                    prim_type == GPU_PRIM_POINTS) ?
                                       RESTART_INDEX :
                                       0;
  }
  else {
    builder->restart_index_value = RESTART_INDEX;
  }
#else
  builder->restart_index_value = RESTART_INDEX;
#endif
  builder->uses_restart_indices = false;
  builder->data = MEM_malloc_arrayN<uint>(builder->max_index_len, "IndexBuf data");
}

void GPU_indexbuf_init(GPUIndexBufBuilder *builder,
                       GPUPrimType prim_type,
                       uint prim_len,
                       uint vertex_len)
{
  int verts_per_prim = GPU_indexbuf_primitive_len(prim_type);
  BLI_assert(verts_per_prim != -1);
  GPU_indexbuf_init_ex(builder, prim_type, prim_len * uint(verts_per_prim), vertex_len);
}

IndexBuf *GPU_indexbuf_build_on_device(uint index_len)
{
  IndexBuf *elem_ = GPU_indexbuf_calloc();
  GPU_indexbuf_init_build_on_device(elem_, index_len);
  return elem_;
}

void GPU_indexbuf_init_build_on_device(IndexBuf *elem, uint index_len)
{
  IndexBuf *elem_ = elem;
  elem_->init_build_on_device(index_len);
}

blender::MutableSpan<uint32_t> GPU_indexbuf_get_data(GPUIndexBufBuilder *builder)
{
  return {builder->data, builder->max_index_len};
}

void GPU_indexbuf_join(GPUIndexBufBuilder *builder_to, const GPUIndexBufBuilder *builder_from)
{
  BLI_assert(builder_to->data == builder_from->data);
  builder_to->index_len = max_uu(builder_to->index_len, builder_from->index_len);
  builder_to->index_min = min_uu(builder_to->index_min, builder_from->index_min);
  builder_to->index_max = max_uu(builder_to->index_max, builder_from->index_max);
}

void GPU_indexbuf_add_generic_vert(GPUIndexBufBuilder *builder, uint v)
{
  BLI_assert(builder->data != nullptr);
  BLI_assert(builder->index_len < builder->max_index_len);
  BLI_assert(v <= builder->max_allowed_index);
  builder->data[builder->index_len++] = v;
  builder->index_min = std::min(builder->index_min, v);
  builder->index_max = std::max(builder->index_max, v);
}

void GPU_indexbuf_add_primitive_restart(GPUIndexBufBuilder *builder)
{
  BLI_assert(builder->data != nullptr);
  BLI_assert(builder->index_len < builder->max_index_len);
  builder->data[builder->index_len++] = builder->restart_index_value;
  builder->uses_restart_indices = true;
}

void GPU_indexbuf_add_point_vert(GPUIndexBufBuilder *builder, uint v)
{
  BLI_assert(builder->prim_type == GPU_PRIM_POINTS);
  GPU_indexbuf_add_generic_vert(builder, v);
}

void GPU_indexbuf_add_line_verts(GPUIndexBufBuilder *builder, uint v1, uint v2)
{
  BLI_assert(builder->prim_type == GPU_PRIM_LINES);
  BLI_assert(v1 != v2);
  GPU_indexbuf_add_generic_vert(builder, v1);
  GPU_indexbuf_add_generic_vert(builder, v2);
}

void GPU_indexbuf_add_tri_verts(GPUIndexBufBuilder *builder, uint v1, uint v2, uint v3)
{
  BLI_assert(builder->prim_type == GPU_PRIM_TRIS);
  BLI_assert(v1 != v2 && v2 != v3 && v3 != v1);
  GPU_indexbuf_add_generic_vert(builder, v1);
  GPU_indexbuf_add_generic_vert(builder, v2);
  GPU_indexbuf_add_generic_vert(builder, v3);
}

void GPU_indexbuf_add_line_adj_verts(
    GPUIndexBufBuilder *builder, uint v1, uint v2, uint v3, uint v4)
{
  BLI_assert(builder->prim_type == GPU_PRIM_LINES_ADJ);
  BLI_assert(v2 != v3); /* only the line need diff indices */
  GPU_indexbuf_add_generic_vert(builder, v1);
  GPU_indexbuf_add_generic_vert(builder, v2);
  GPU_indexbuf_add_generic_vert(builder, v3);
  GPU_indexbuf_add_generic_vert(builder, v4);
}

void GPU_indexbuf_set_point_vert(GPUIndexBufBuilder *builder, uint elem, uint v1)
{
  BLI_assert(builder->prim_type == GPU_PRIM_POINTS);
  BLI_assert(elem < builder->max_index_len);
  builder->data[elem++] = v1;
  builder->index_min = std::min(builder->index_min, v1);
  builder->index_max = std::max(builder->index_max, v1);
  builder->index_len = std::max(builder->index_len, elem);
}

void GPU_indexbuf_set_line_verts(GPUIndexBufBuilder *builder, uint elem, uint v1, uint v2)
{
  BLI_assert(builder->prim_type == GPU_PRIM_LINES);
  BLI_assert(v1 != v2);
  BLI_assert(v1 <= builder->max_allowed_index);
  BLI_assert(v2 <= builder->max_allowed_index);
  BLI_assert((elem + 1) * 2 <= builder->max_index_len);
  uint idx = elem * 2;
  builder->data[idx++] = v1;
  builder->data[idx++] = v2;
  builder->index_min = std::min({builder->index_min, v1, v2});
  builder->index_max = std::max({builder->index_max, v1, v2});
  builder->index_len = std::max(builder->index_len, idx);
}

void GPU_indexbuf_set_tri_verts(GPUIndexBufBuilder *builder, uint elem, uint v1, uint v2, uint v3)
{
  BLI_assert(builder->prim_type == GPU_PRIM_TRIS);
  BLI_assert(v1 != v2 && v2 != v3 && v3 != v1);
  BLI_assert(v1 <= builder->max_allowed_index);
  BLI_assert(v2 <= builder->max_allowed_index);
  BLI_assert(v3 <= builder->max_allowed_index);
  BLI_assert((elem + 1) * 3 <= builder->max_index_len);
  uint idx = elem * 3;
  builder->data[idx++] = v1;
  builder->data[idx++] = v2;
  builder->data[idx++] = v3;

  builder->index_min = std::min({builder->index_min, v1, v2, v3});
  builder->index_max = std::max({builder->index_max, v1, v2, v3});
  builder->index_len = std::max(builder->index_len, idx);
}

void GPU_indexbuf_set_point_restart(GPUIndexBufBuilder *builder, uint elem)
{
  BLI_assert(builder->prim_type == GPU_PRIM_POINTS);
  BLI_assert(elem < builder->max_index_len);
  builder->data[elem++] = builder->restart_index_value;
  builder->index_len = std::max(builder->index_len, elem);
  builder->uses_restart_indices = true;
}

void GPU_indexbuf_set_line_restart(GPUIndexBufBuilder *builder, uint elem)
{
  BLI_assert(builder->prim_type == GPU_PRIM_LINES);
  BLI_assert((elem + 1) * 2 <= builder->max_index_len);
  uint idx = elem * 2;
  builder->data[idx++] = builder->restart_index_value;
  builder->data[idx++] = builder->restart_index_value;
  builder->index_len = std::max(builder->index_len, idx);
  builder->uses_restart_indices = true;
}

void GPU_indexbuf_set_tri_restart(GPUIndexBufBuilder *builder, uint elem)
{
  BLI_assert(builder->prim_type == GPU_PRIM_TRIS);
  BLI_assert((elem + 1) * 3 <= builder->max_index_len);
  uint idx = elem * 3;
  builder->data[idx++] = builder->restart_index_value;
  builder->data[idx++] = builder->restart_index_value;
  builder->data[idx++] = builder->restart_index_value;
  builder->index_len = std::max(builder->index_len, idx);
  builder->uses_restart_indices = true;
}

IndexBuf *GPU_indexbuf_build_curves_on_device(GPUPrimType prim_type,
                                              uint curves_num,
                                              uint verts_per_curve)
{
  uint64_t dispatch_x_dim = verts_per_curve;
  if (ELEM(prim_type, GPU_PRIM_LINE_STRIP, GPU_PRIM_TRI_STRIP)) {
    dispatch_x_dim += 1;
  }
  uint64_t grid_x, grid_y, grid_z;
  uint64_t max_grid_x = GPU_max_work_group_count(0), max_grid_y = GPU_max_work_group_count(1),
           max_grid_z = GPU_max_work_group_count(2);
  grid_x = min_uu(max_grid_x, (dispatch_x_dim + 15) / 16);
  grid_y = (curves_num + 15) / 16;
  if (grid_y <= max_grid_y) {
    grid_z = 1;
  }
  else {
    grid_y = grid_z = uint64_t(ceil(sqrt(double(grid_y))));
    grid_y = min_uu(grid_y, max_grid_y);
    grid_z = min_uu(grid_z, max_grid_z);
  }
  bool tris = (prim_type == GPU_PRIM_TRIS);
  bool lines = (prim_type == GPU_PRIM_LINES);
  blender::gpu::Shader *shader = GPU_shader_get_builtin_shader(
      tris ? GPU_SHADER_INDEXBUF_TRIS :
             (lines ? GPU_SHADER_INDEXBUF_LINES : GPU_SHADER_INDEXBUF_POINTS));
  GPU_shader_bind(shader);
  IndexBuf *ibo = GPU_indexbuf_build_on_device(curves_num * dispatch_x_dim);
  int resolution;
  if (tris) {
    resolution = 6;
  }
  else if (lines) {
    resolution = 2;
  }
  else {
    resolution = 1;
  }
  GPU_shader_uniform_1i(shader, "elements_per_curve", dispatch_x_dim / resolution);
  GPU_shader_uniform_1i(shader, "ncurves", curves_num);
  GPU_indexbuf_bind_as_ssbo(ibo, GPU_shader_get_ssbo_binding(shader, "out_indices"));
  GPU_compute_dispatch(shader, grid_x, grid_y, grid_z);

  GPU_memory_barrier(GPU_BARRIER_ELEMENT_ARRAY);
  GPU_shader_unbind();
  return ibo;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Creation & Deletion
 * \{ */

namespace blender::gpu {

IndexBuf::~IndexBuf()
{
  if (!is_subrange_) {
    MEM_SAFE_FREE(data_);
  }
}

void IndexBuf::init(uint indices_len,
                    uint32_t *indices,
                    uint min_index,
                    uint max_index,
                    GPUPrimType prim_type,
                    bool uses_restart_indices)
{
  is_init_ = true;
  data_ = indices;
  index_start_ = 0;
  index_len_ = indices_len;
  is_empty_ = min_index > max_index;

  /* Patch index buffer to remove restart indices from
   * non-restart-compatible primitive types. Restart indices
   * are situationally added to selectively hide vertices.
   * Metal does not support restart-indices for non-restart-compatible
   * types, as such we should remove these indices.
   *
   * We only need to perform this for point primitives, as
   * line primitives/triangle primitives can use index 0 for all
   * vertices to create a degenerative primitive, where all
   * vertices share the same index and skip rendering via HW
   * culling. */
  if (prim_type == GPU_PRIM_POINTS && uses_restart_indices) {
    this->strip_restart_indices();
  }

#if GPU_TRACK_INDEX_RANGE
  /* Everything remains 32 bit while building to keep things simple.
   * Find min/max after, then convert to smallest index type possible. */
  uint range = min_index < max_index ? max_index - min_index : 0;
  /* count the primitive restart index. */
  range += 1;

  if (range <= 0xFFFF) {
    index_type_ = GPU_INDEX_U16;
    bool do_clamp_indices = false;
#  ifdef __APPLE__
    /* NOTE: For the Metal Backend, we use degenerative primitives to hide vertices
     * which are not restart compatible. When this is done, we need to ensure
     * that compressed index ranges clamp all index values within the valid
     * range, rather than maximally clamping against the USHORT restart index
     * value of 0xFFFFu, as this will cause an out-of-bounds read during
     * vertex assembly. */
    do_clamp_indices = GPU_type_matches_ex(
        GPU_DEVICE_ANY, GPU_OS_MAC, GPU_DRIVER_ANY, GPU_BACKEND_METAL);
#  endif
    this->squeeze_indices_short(min_index, max_index, prim_type, do_clamp_indices);
  }
#endif
}

void IndexBuf::init_build_on_device(uint index_len)
{
  is_init_ = true;
  index_start_ = 0;
  index_len_ = index_len;
  index_type_ = GPU_INDEX_U32;
  data_ = nullptr;
}

void IndexBuf::init_subrange(IndexBuf *elem_src, uint start, uint length)
{
  /* We don't support nested sub-ranges. */
  BLI_assert(elem_src && elem_src->is_subrange_ == false);
  BLI_assert((length == 0) || (start + length <= elem_src->index_len_));

  is_init_ = true;
  is_subrange_ = true;
  src_ = elem_src;
  index_start_ = start;
  index_len_ = length;
  index_base_ = elem_src->index_base_;
  index_type_ = elem_src->index_type_;
}

void IndexBuf::squeeze_indices_short(uint min_idx,
                                     uint max_idx,
                                     GPUPrimType prim_type,
                                     bool clamp_indices_in_range)
{
  /* data will never be *larger* than builder->data...
   * converting in place to avoid extra allocation */
  uint16_t *ushort_idx = (uint16_t *)data_;
  const uint32_t *uint_idx = (uint32_t *)data_;

  if (max_idx >= 0xFFFF) {
    index_base_ = min_idx;
    /* NOTE: When using restart_index=0 for degenerative primitives indices,
     * the compressed index will go below zero and wrap around when min_idx > 0.
     * In order to ensure the resulting index is still within range, we instead
     * clamp index to the maximum within the index range.
     *
     * `clamp_max_idx` represents the maximum possible index to clamp against. If primitive is
     * restart-compatible, we can just clamp against the primitive-restart value, otherwise, we
     * must assign to a valid index within the range.
     *
     * NOTE: For OpenGL we skip this by disabling clamping, as we still need to use
     * restart index values for point primitives to disable rendering. */
    uint16_t clamp_max_idx = (is_restart_compatible(prim_type) || !clamp_indices_in_range) ?
                                 0xFFFFu :
                                 (max_idx - min_idx);
    for (uint i = 0; i < index_len_; i++) {
      ushort_idx[i] = std::min<uint16_t>(clamp_max_idx, uint_idx[i] - min_idx);
    }
  }
  else {
    index_base_ = 0;
    for (uint i = 0; i < index_len_; i++) {
      ushort_idx[i] = uint16_t(uint_idx[i]);
    }
  }
}

}  // namespace blender::gpu

/** \} */

/* -------------------------------------------------------------------- */
/** \name C-API
 * \{ */

IndexBuf *GPU_indexbuf_calloc()
{
  return GPUBackend::get()->indexbuf_alloc();
}

IndexBuf *GPU_indexbuf_build(GPUIndexBufBuilder *builder)
{
  IndexBuf *elem = GPU_indexbuf_calloc();
  GPU_indexbuf_build_in_place(builder, elem);
  return elem;
}

IndexBuf *GPU_indexbuf_create_subrange(IndexBuf *elem_src, uint start, uint length)
{
  IndexBuf *elem = GPU_indexbuf_calloc();
  GPU_indexbuf_create_subrange_in_place(elem, elem_src, start, length);
  return elem;
}

void GPU_indexbuf_build_in_place(GPUIndexBufBuilder *builder, IndexBuf *elem)
{
  BLI_assert(builder->data != nullptr);
  /* Transfer data ownership to IndexBuf.
   * It will be uploaded upon first use. */
  elem->init(builder->index_len,
             builder->data,
             builder->index_min,
             builder->index_max,
             builder->prim_type,
             builder->uses_restart_indices);
  builder->data = nullptr;
}

void GPU_indexbuf_build_in_place_ex(GPUIndexBufBuilder *builder,
                                    const uint index_min,
                                    const uint index_max,
                                    const bool uses_restart_indices,
                                    IndexBuf *elem)
{
  BLI_assert(builder->data != nullptr);
  /* Transfer data ownership to IndexBuf.
   * It will be uploaded upon first use. */
  elem->init(builder->max_index_len,
             builder->data,
             index_min,
             index_max,
             builder->prim_type,
             uses_restart_indices);
  builder->data = nullptr;
}

IndexBuf *GPU_indexbuf_build_ex(GPUIndexBufBuilder *builder,
                                const uint index_min,
                                const uint index_max,
                                const bool uses_restart_indices)
{
  IndexBuf *elem = GPU_indexbuf_calloc();
  GPU_indexbuf_build_in_place_ex(builder, index_min, index_max, uses_restart_indices, elem);
  return elem;
}

IndexBuf *GPU_indexbuf_build_from_memory(const GPUPrimType prim_type,
                                         const uint32_t *data,
                                         const int32_t data_len,
                                         const int32_t index_min,
                                         const int32_t index_max,
                                         const bool uses_restart_indices)
{
  const uint32_t indices_num = data_len * indices_per_primitive(prim_type);
  /* TODO: The need for this copy is meant to be temporary. The data should be uploaded directly to
   * the GPU here rather than copied to an array owned by the IBO first. */
  uint32_t *copy = MEM_malloc_arrayN<uint32_t>(indices_num, __func__);
  threading::memory_bandwidth_bound_task(sizeof(uint32_t) * indices_num * 2, [&]() {
    array_utils::copy(Span(data, indices_num), MutableSpan(copy, indices_num));
  });
  IndexBuf *ibo = GPU_indexbuf_calloc();
  ibo->init(indices_num, copy, index_min, index_max, prim_type, uses_restart_indices);
  return ibo;
}

void GPU_indexbuf_create_subrange_in_place(IndexBuf *elem,
                                           IndexBuf *elem_src,
                                           uint start,
                                           uint length)
{
  elem->init_subrange(elem_src, start, length);
}

void GPU_indexbuf_read(IndexBuf *elem, uint32_t *data)
{
  elem->read(data);
}

void GPU_indexbuf_discard(IndexBuf *elem)
{
  delete elem;
}

bool GPU_indexbuf_is_init(IndexBuf *elem)
{
  return elem->is_init();
}

int GPU_indexbuf_primitive_len(GPUPrimType prim_type)
{
  return indices_per_primitive(prim_type);
}

void GPU_indexbuf_use(IndexBuf *elem)
{
  elem->upload_data();
}

void GPU_indexbuf_bind_as_ssbo(IndexBuf *elem, int binding)
{
  elem->bind_as_ssbo(binding);
}

void GPU_indexbuf_update_sub(IndexBuf *elem, uint start, uint len, const void *data)
{
  elem->update_sub(start, len, data);
}

/** \} */
