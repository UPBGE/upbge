/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * \file gpu_shader_common_normal_lib.hh
 * \ingroup gpu
 *
 * Common normal calculation functions for GPU shaders (GLSL string generation).
 * Shared between mesh_gpu.cc scatter shader and draw_displace.cc compute shader.
 *
 * Port of CPU functions from:
 * - BLI_math_base.hh (safe_acos_approx)
 * - BLI_math_vector.hh (math::normalize when no arg passed -> fallback on vec3(0.0))
 * - mesh_normals.cc (face_normal_object)
 */

#pragma once

namespace blender {
namespace gpu {

const std::string &get_common_normal_lib_glsl();

}  // namespace gpu
}  // namespace blender
