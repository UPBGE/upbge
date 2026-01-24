/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

namespace blender {
namespace gpu {
class Texture;
}  // namespace gpu
}  // namespace blender

namespace blender {
struct Mesh;
struct Object;
}  // namespace blender

namespace blender {
namespace gpu {

/* Helper getters that create or return cached GPU textures for Blender noise tables.
 * Implemented in `gpu_shader_common_texture_lib.cc`. These mirror the signatures used
 * by legacy `draw_displace` so callers can obtain textures that wrap the internal
 * hash/gradient tables. */
Texture *get_noise_hash_texture(Mesh *mesh_owner, Object *deformed_eval, const std::string &key);
Texture *get_noise_hashvect_texture(Mesh *mesh_owner, Object *deformed_eval, const std::string &key);
Texture *get_noise_hashpnt_texture(Mesh *mesh_owner, Object *deformed_eval, const std::string &key);

const std::string get_texture_typedefs_glsl();
const std::string get_texture_params_glsl();
const std::string &get_common_texture_lib_glsl();
const std::string &get_common_texture_image_lib_glsl();

}  // namespace gpu
}  // namespace blender
