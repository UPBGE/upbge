/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Debug print
 *
 * Allows print() function to have logging support inside shaders.
 * \{ */

GPU_SHADER_CREATE_INFO(draw_debug_print)
    .typedef_source("draw_shader_shared.h")
    .storage_buf(7, Qualifier::READ_WRITE, "uint", "drw_debug_print_buf[]");

GPU_SHADER_INTERFACE_INFO(draw_debug_print_display_iface, "").flat(Type::UINT, "char_index");

GPU_SHADER_CREATE_INFO(draw_debug_print_display)
    .do_static_compilation(true)
    .typedef_source("draw_shader_shared.h")
    .storage_buf(7, Qualifier::READ, "uint", "drw_debug_print_buf[]")
    .vertex_out(draw_debug_print_display_iface)
    .fragment_out(0, Type::VEC4, "out_color")
    .vertex_source("draw_debug_print_display_vert.glsl")
    .fragment_source("draw_debug_print_display_frag.glsl")
    .additional_info("draw_view");

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug draw shapes
 *
 * Allows to draw lines and points just like the DRW_debug module functions.
 * \{ */

GPU_SHADER_CREATE_INFO(draw_debug_draw)
    .typedef_source("draw_shader_shared.h")
    .storage_buf(6, Qualifier::READ_WRITE, "DRWDebugVert", "drw_debug_verts_buf[]");

GPU_SHADER_INTERFACE_INFO(draw_debug_draw_display_iface, "interp").flat(Type::VEC4, "color");

GPU_SHADER_CREATE_INFO(draw_debug_draw_display)
    .do_static_compilation(true)
    .typedef_source("draw_shader_shared.h")
    .storage_buf(6, Qualifier::READ, "DRWDebugVert", "drw_debug_verts_buf[]")
    .vertex_out(draw_debug_draw_display_iface)
    .fragment_out(0, Type::VEC4, "out_color")
    .push_constant(Type::MAT4, "persmat")
    .vertex_source("draw_debug_draw_display_vert.glsl")
    .fragment_source("draw_debug_draw_display_frag.glsl")
    .additional_info("draw_view");

/** \} */
