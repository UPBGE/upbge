/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

namespace blender {
namespace gpu {

const std::string get_texture_typedefs_glsl();
const std::string get_texture_params_glsl();
const std::string &get_common_texture_lib_glsl();
const std::string &get_common_texture_image_lib_glsl();

}  // namespace gpu
}  // namespace blender
