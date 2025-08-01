/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_bokeh_blur_variable_size)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float, base_size)
PUSH_CONSTANT(int, search_radius)
SAMPLER(0, sampler2D, input_tx)
SAMPLER(1, sampler2D, weights_tx)
SAMPLER(2, sampler2D, size_tx)
SAMPLER(3, sampler2D, mask_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, output_img)
COMPUTE_SOURCE("compositor_bokeh_blur_variable_size.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
