/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee (UPBGE)
 */

#pragma once
#include "GPU_vertex_buffer.hh"
struct Mesh;

namespace blender::draw {
gpu::VertBuf *ensure_prev_pos_vbo(Mesh *me, int verts_num, const GPUVertFormat *format);
gpu::VertBuf *get_prev_pos_vbo(Mesh *me);
void free_prev_pos_vbo(Mesh *me);
/* GPU copy helper: copy src vertbuf -> dst vertbuf (GPU only). */
void copy_vertbuf_to_vertbuf(gpu::VertBuf *dst, gpu::VertBuf *src, int verts);
} // namespace blender::draw
