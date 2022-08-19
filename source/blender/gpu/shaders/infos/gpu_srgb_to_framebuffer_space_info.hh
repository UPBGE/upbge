/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup gpu
 */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(gpu_srgb_to_framebuffer_space)
    .push_constant(Type::BOOL, "srgbTarget")
    .define("blender_srgb_to_framebuffer_space(a) a");
