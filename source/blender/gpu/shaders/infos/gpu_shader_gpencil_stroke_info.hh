/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup gpu
 */

#include "gpu_shader_create_info.hh"

GPU_SHADER_INTERFACE_INFO(gpencil_stroke_vert_iface, "geometry_in")
    .smooth(Type::VEC4, "finalColor")
    .smooth(Type::FLOAT, "finalThickness");
GPU_SHADER_INTERFACE_INFO(gpencil_stroke_geom_iface, "geometry_out")
    .smooth(Type::VEC4, "mColor")
    .smooth(Type::VEC2, "mTexCoord");

GPU_SHADER_CREATE_INFO(gpu_shader_gpencil_stroke)
    .vertex_in(0, Type::VEC4, "color")
    .vertex_in(1, Type::VEC3, "pos")
    .vertex_in(2, Type::FLOAT, "thickness")
    .vertex_out(gpencil_stroke_vert_iface)
    .geometry_layout(PrimitiveIn::LINES_ADJACENCY, PrimitiveOut::TRIANGLE_STRIP, 13)
    .geometry_out(gpencil_stroke_geom_iface)
    .fragment_out(0, Type::VEC4, "fragColor")

    .uniform_buf(0, "GPencilStrokeData", "gpencil_stroke_data")

    .push_constant(Type::MAT4, "ModelViewProjectionMatrix")
    .push_constant(Type::MAT4, "ProjectionMatrix")
    .vertex_source("gpu_shader_gpencil_stroke_vert.glsl")
    .geometry_source("gpu_shader_gpencil_stroke_geom.glsl")
    .fragment_source("gpu_shader_gpencil_stroke_frag.glsl")
    .typedef_source("GPU_shader_shared.h")
    .do_static_compilation(true);
