/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup gpu
 */

#include "gpu_shader_create_info.hh"

GPU_SHADER_INTERFACE_INFO(text_iface, "")
    .flat(Type::VEC4, "color_flat")
    .no_perspective(Type::VEC2, "texCoord_interp")
    .flat(Type::INT, "glyph_offset")
    .flat(Type::IVEC2, "glyph_dim")
    .flat(Type::INT, "interp_size");

GPU_SHADER_CREATE_INFO(gpu_shader_text)
    .vertex_in(0, Type::VEC4, "pos")
    .vertex_in(1, Type::VEC4, "col")
    .vertex_in(2, Type ::IVEC2, "glyph_size")
    .vertex_in(3, Type ::INT, "offset")
    .vertex_out(text_iface)
    .fragment_out(0, Type::VEC4, "fragColor")
    .push_constant(Type::MAT4, "ModelViewProjectionMatrix")
    .sampler(0, ImageType::FLOAT_2D, "glyph", Frequency::PASS)
    .vertex_source("gpu_shader_text_vert.glsl")
    .fragment_source("gpu_shader_text_frag.glsl")
    .additional_info("gpu_srgb_to_framebuffer_space")
    .do_static_compilation(true);
