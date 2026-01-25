#pragma once

#include <vector>
#include "BLI_math_vector_types.hh"

namespace blender {
struct Depsgraph;
struct Mesh;
struct Object;
struct Tex;
struct ModifierData;
}  // namespace blender

namespace blender {
namespace gpu {
class StorageBuf;
class Texture;
class UniformBuf;
}  // namespace gpu
}  // namespace blender

namespace blender {
namespace draw {
namespace modifier_gpu_helpers {

/* Ensure vertex-group SSBO. If `weights` is empty, a dummy buffer of size
 * `verts_num` filled with 1.0f will be created. The function returns the
 * SSBO (or nullptr on failure). */
blender::gpu::StorageBuf *ensure_vgroup_ssbo(Mesh *mesh_owner,
                                             Object *deformed_eval,
                                             const std::string &key_vgroup,
                                             const std::vector<float> &weights,
                                             int verts_num);

/* Prepare GPU texture from `Tex`/Image and optionally upload tex_coords SSBO.
 * Updates provided metadata flags and may create/cache the GPU texture.
 * Returns the GPU texture (or nullptr). If `r_ssbo_texcoords` is non-nullptr
 * it will be set to the created SSBO (or left null if none). */
blender::gpu::Texture *prepare_gpu_texture_and_texcoords(
    Mesh *mesh_owner,
    Object *deformed_eval,
    Depsgraph *depsgraph,
    Tex *tex,
    std::vector<float3> &tex_coords,
    bool &r_tex_is_byte,
    bool &r_tex_is_float,
    int &r_tex_channels,
    bool &r_tex_metadata_cached,
    const std::string &key_prefix,
    blender::gpu::StorageBuf **r_ssbo_texcoords,
    bool is_uv_mapping);

/* Ensure ColorBand UBO (creates dummy when missing). Returns the UBO and
 * updates `out_hash` when a real colorband was uploaded. */
blender::gpu::UniformBuf *ensure_colorband_ubo(Mesh *mesh_owner,
                                               Object *deformed_eval,
                                               const std::string &key_colorband,
                                               Tex *tex,
                                               uint32_t &colorband_hash);

/* Ensure TextureParams UBO using existing helper to fill params. */
blender::gpu::UniformBuf *ensure_texture_params_ubo(Mesh *mesh_owner,
                                                    Object *deformed_eval,
                                                    const std::string &key_tex_params,
                                                    Tex *tex,
                                                    ModifierData *md,
                                                    int scene_frame,
                                                    bool tex_is_byte,
                                                    bool tex_is_float,
                                                    int tex_channels,
                                                    bool has_texcoords);

}  // namespace modifier_gpu_helpers
}  // namespace draw
}  // namespace blender
