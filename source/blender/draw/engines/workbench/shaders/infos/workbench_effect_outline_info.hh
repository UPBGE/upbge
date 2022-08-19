/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(workbench_effect_outline)
    .typedef_source("workbench_shader_shared.h")
    .fragment_source("workbench_effect_outline_frag.glsl")
    .sampler(0, ImageType::UINT_2D, "objectIdBuffer")
    .uniform_buf(4, "WorldData", "world_data", Frequency::PASS)
    .fragment_out(0, Type::VEC4, "fragColor")
    .additional_info("draw_fullscreen")
    .do_static_compilation(true);
