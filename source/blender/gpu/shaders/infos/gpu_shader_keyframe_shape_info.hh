/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup gpu
 */

#include "gpu_shader_create_info.hh"

GPU_SHADER_INTERFACE_INFO(keyframe_shape_iface, "")
    .flat(Type::VEC4, "finalColor")
    .flat(Type::VEC4, "finalOutlineColor")
    .flat(Type::VEC4, "radii")
    .flat(Type::VEC4, "thresholds")
    .flat(Type::INT, "finalFlags");

GPU_SHADER_CREATE_INFO(gpu_shader_keyframe_shape)
    .vertex_in(0, Type::VEC4, "color")
    .vertex_in(1, Type::VEC4, "outlineColor")
    .vertex_in(2, Type::VEC2, "pos")
    .vertex_in(3, Type::FLOAT, "size")
    .vertex_in(4, Type ::INT, "flags")
    .vertex_out(keyframe_shape_iface)
    .fragment_out(0, Type::VEC4, "fragColor")
    .push_constant(Type::MAT4, "ModelViewProjectionMatrix")
    .push_constant(Type::VEC2, "ViewportSize")
    .push_constant(Type::FLOAT, "outline_scale")
    .vertex_source("gpu_shader_keyframe_shape_vert.glsl")
    .fragment_source("gpu_shader_keyframe_shape_frag.glsl")
    .do_static_compilation(true);
