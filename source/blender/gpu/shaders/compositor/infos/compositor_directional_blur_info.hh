/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_directional_blur)
    .local_group_size(16, 16)
    .push_constant(Type::INT, "iterations")
    .push_constant(Type::MAT4, "inverse_transformation")
    .sampler(0, ImageType::FLOAT_2D, "input_tx")
    .image(0, GPU_RGBA16F, Qualifier::WRITE, ImageType::FLOAT_2D, "output_img")
    .compute_source("compositor_directional_blur.glsl")
    .do_static_compilation(true);
