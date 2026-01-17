#include "draw_modifier_gpu_helpers.hh"

#include "BLI_hash.h"
#include "BLI_math_vector.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"

#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "IMB_imbuf.hh"

#include "GPU_texture.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_uniform_buffer.hh"

#include "../gpu/gpu_modifiers_common/gpu_texture_helpers.hh"

#include "DEG_depsgraph_query.hh"

#include "MEM_guardedalloc.h"

namespace blender {
namespace draw {
namespace modifier_gpu_helpers {

gpu::StorageBuf *modifier_gpu_helpers::ensure_vgroup_ssbo(Mesh *mesh_owner,
                                                          Object *deformed_eval,
                                                          const std::string &key_vgroup,
                                                          const std::vector<float> &weights,
                                                          int verts_num)
{
  gpu::StorageBuf *ssbo_vgroup = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_vgroup);

  size_t count = (weights.empty()) ? size_t((verts_num > 0) ? verts_num : 1) : weights.size();
  const size_t size_vgroup = count * sizeof(float);

  if (!ssbo_vgroup) {
    ssbo_vgroup = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_vgroup, size_vgroup);
    if (ssbo_vgroup) {
      if (weights.empty()) {
        std::vector<float> dummy(count, 1.0f);
        GPU_storagebuf_update(ssbo_vgroup, dummy.data());
      }
      else {
        GPU_storagebuf_update(ssbo_vgroup, weights.data());
      }
    }
  }

  return ssbo_vgroup;
}

/* Local copy of colorband hashing to avoid dependency on draw_displace's static helper. */
static uint32_t colorband_hash_from_coba(const ColorBand *coba)
{
  if (!coba) {
    return 0;
  }

  uint32_t hash = 0;

  /* Hash basic integer fields */
  hash = BLI_hash_int_2d(hash, uint32_t(coba->tot));
  hash = BLI_hash_int_2d(hash, uint32_t(coba->cur));
  hash = BLI_hash_int_2d(hash, uint32_t(coba->ipotype));
  hash = BLI_hash_int_2d(hash, uint32_t(coba->ipotype_hue));
  hash = BLI_hash_int_2d(hash, uint32_t(coba->color_mode));

  int tot = coba->tot;
  if (tot < 0) tot = 0;
  if (tot > 32) tot = 32;

  for (int i = 0; i < tot; ++i) {
    const auto &stop = coba->data[i];
    uint32_t v;
    memcpy(&v, &stop.r, sizeof(v)); hash = BLI_hash_int_2d(hash, v);
    memcpy(&v, &stop.g, sizeof(v)); hash = BLI_hash_int_2d(hash, v);
    memcpy(&v, &stop.b, sizeof(v)); hash = BLI_hash_int_2d(hash, v);
    memcpy(&v, &stop.a, sizeof(v)); hash = BLI_hash_int_2d(hash, v);
    memcpy(&v, &stop.pos, sizeof(v)); hash = BLI_hash_int_2d(hash, v);
    hash = BLI_hash_int_2d(hash, uint32_t(stop.cur));
  }

  return hash;
}

gpu::Texture *modifier_gpu_helpers::prepare_gpu_texture_and_texcoords(
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
    gpu::StorageBuf **r_ssbo_texcoords,
    bool is_uv_mapping,
    bool create_dummy_if_missing)
{
  if (!tex) {
    return nullptr;
  }

  gpu::Texture *gpu_texture = nullptr;
  Image *ima = tex->ima; /* may be nullptr for procedural textures */
  ImageUser iuser = tex->iuser;

  if (ima && ELEM(ima->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE)) {
    Scene *scene = DEG_get_evaluated_scene(depsgraph);
    if (scene) {
      BKE_image_user_frame_calc(ima, &iuser, int(scene->r.cfra));
    }
  }

  const bool is_non_color = (ima && ima->colorspace_settings.name[0] != '\0' &&
                             STREQ(ima->colorspace_settings.name, "Non-Color"));

  if (ima) {
    if (is_non_color) {
      gpu_texture = BKE_image_get_gpu_texture(ima, &iuser);
      if (gpu_texture && !r_tex_metadata_cached) {
        r_tex_is_float = GPU_texture_has_float_format(gpu_texture);
        r_tex_is_byte = !r_tex_is_float;
        r_tex_channels = GPU_texture_component_len(GPU_texture_format(gpu_texture));
        r_tex_metadata_cached = true;
      }
    }
    else {
      const std::string key_texture = key_prefix + "texture_" + std::to_string(reinterpret_cast<uintptr_t>(ima)) + "_" +
                                      std::to_string(iuser.framenr);
      gpu_texture = bke::BKE_mesh_gpu_internal_texture_get(mesh_owner, key_texture);

      const bool is_animated = (ima && ELEM(ima->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE));
      if (!gpu_texture || (is_animated && !gpu_texture)) {
        ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &iuser, nullptr);
        if (ibuf && (ibuf->float_buffer.data || ibuf->byte_buffer.data)) {
          gpu::TextureFormat format = ibuf->float_buffer.data ?
              gpu::TextureFormat::SFLOAT_16_16_16_16 : gpu::TextureFormat::UNORM_8_8_8_8;

          gpu_texture = GPU_texture_create_2d("modifier_tex_raw", ibuf->x, ibuf->y, 1, format,
                                             GPU_TEXTURE_USAGE_SHADER_READ, nullptr);
          if (gpu_texture) {
            gpu::displace_upload_ibuf_to_texture(gpu_texture, ibuf, ima->colorspace_settings.name);
            bke::BKE_mesh_gpu_internal_texture_ensure(mesh_owner, deformed_eval, key_texture, gpu_texture);

            if (!r_tex_metadata_cached) {
              r_tex_is_byte = (ibuf->byte_buffer.data != nullptr);
              r_tex_is_float = (ibuf->float_buffer.data != nullptr);
              r_tex_channels = ibuf->channels;
              r_tex_metadata_cached = true;
            }
          }
        }
        if (ibuf) {
          BKE_image_release_ibuf(ima, ibuf, nullptr);
        }
      }
    }
  }

  /* If requested, create a 1x1 dummy texture when a procedural texture
   * exists (tex != nullptr) but no GPU texture was produced (no Image).
   * Cache the dummy per-mesh+modifier using `key_prefix + "dummy_tex"`. */
  if (!gpu_texture && tex && create_dummy_if_missing) {
    const std::string key_dummy = key_prefix + "dummy_tex";
    gpu_texture = bke::BKE_mesh_gpu_internal_texture_get(mesh_owner, key_dummy);
    if (!gpu_texture) {
      unsigned char pixel[4] = {128, 128, 128, 255};
      gpu_texture = GPU_texture_create_2d("modifier_dummy_tex", 1, 1, 1,
                                         gpu::TextureFormat::UNORM_8_8_8_8,
                                         GPU_TEXTURE_USAGE_SHADER_READ, nullptr);
      if (gpu_texture) {
        GPU_texture_update(gpu_texture, GPU_DATA_UBYTE, pixel);
        bke::BKE_mesh_gpu_internal_texture_ensure(mesh_owner, deformed_eval, key_dummy, gpu_texture);
      }
    }

    if (gpu_texture && !r_tex_metadata_cached) {
      r_tex_is_byte = true;
      r_tex_is_float = false;
      r_tex_channels = GPU_texture_component_len(GPU_texture_format(gpu_texture));
      r_tex_metadata_cached = true;
    }
  }

  /* Upload texcoords SSBO if we have coords and a buffer pointer to fill.
   * Do this after dummy texture creation so procedural textures are covered. */
  if (!tex_coords.empty() && r_ssbo_texcoords) {
    const std::string key_texcoords = key_prefix + "tex_coords";
    gpu::StorageBuf *ssbo_texcoords = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_texcoords);
    if (!ssbo_texcoords) {
      const size_t size_texcoords = tex_coords.size() * sizeof(float4);
      ssbo_texcoords = bke::BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, deformed_eval, key_texcoords, size_texcoords);
      if (ssbo_texcoords) {
        std::vector<float4> padded(tex_coords.size());

        for (size_t i = 0; i < tex_coords.size(); ++i) {
          printf("Uploading texcoord %zu: (%f, %f, %f)\n",
                 i,
                 tex_coords[i].x,
                 tex_coords[i].y,
                 tex_coords[i].z);
          float z = is_uv_mapping ? 0.0f : tex_coords[i].z;
          padded[i] = float4(tex_coords[i].x, tex_coords[i].y, z, 1.0f);
        }

        GPU_storagebuf_update(ssbo_texcoords, padded.data());
      }
    }
    *r_ssbo_texcoords = ssbo_texcoords;
  }

  return gpu_texture;
}

gpu::UniformBuf *modifier_gpu_helpers::ensure_colorband_ubo(Mesh *mesh_owner,
                                                            Object *deformed_eval,
                                                            const std::string &key_colorband,
                                                            Tex *tex,
                                                            uint32_t &colorband_hash)
{
  gpu::UniformBuf *ubo_colorband = bke::BKE_mesh_gpu_internal_ubo_get(mesh_owner, key_colorband);
  const size_t size_colorband = sizeof(blender::gpu::GPUColorBand);

  /* If UBO does not exist, create it (real colorband or dummy). The helper
   * will fill `out_hash` when a real colorband is uploaded so callers can
   * cache it. If UBO exists, ensure its contents match current texture state
   * and update it when the ColorBand changed or was added/removed. */
  if (!ubo_colorband) {
    if (tex && tex->coba && (tex->flag & TEX_COLORBAND)) {
      ColorBand *coba = tex->coba;
      blender::gpu::GPUColorBand gpu_coba = {};
      if (blender::gpu::fill_gpu_colorband_from_colorband(gpu_coba, coba)) {
        ubo_colorband = bke::BKE_mesh_gpu_internal_ubo_ensure(mesh_owner, deformed_eval, key_colorband, size_colorband);
        if (ubo_colorband) {
          GPU_uniformbuf_update(ubo_colorband, &gpu_coba);
          colorband_hash = colorband_hash_from_coba(coba);
        }
      }
    }
    else {
      blender::gpu::GPUColorBand dummy_coba = {};
      dummy_coba.tot_cur_ipotype_hue[0] = 0;
      ubo_colorband = bke::BKE_mesh_gpu_internal_ubo_ensure(mesh_owner, deformed_eval, key_colorband, size_colorband);
      if (ubo_colorband) {
        GPU_uniformbuf_update(ubo_colorband, &dummy_coba);
        colorband_hash = 0;
      }
    }
  }
  else {
    /* UBO exists: verify its contents reflect the current tex/colorband state. */
    if (tex && tex->coba && (tex->flag & TEX_COLORBAND)) {
      ColorBand *coba = tex->coba;
      uint32_t new_hash = colorband_hash_from_coba(coba);
      if (new_hash != colorband_hash) {
        blender::gpu::GPUColorBand gpu_coba = {};
        if (blender::gpu::fill_gpu_colorband_from_colorband(gpu_coba, coba)) {
          GPU_uniformbuf_update(ubo_colorband, &gpu_coba);
          colorband_hash = new_hash;
        }
      }
    }
    else {
      /* No real colorband: only upload dummy if the previously cached hash was
       * non-zero (meaning a real ColorBand was present before). This avoids
       * redundant uniform buffer updates every frame for the dummy UBO. */
      if (colorband_hash != 0) {
        blender::gpu::GPUColorBand dummy_coba = {};
        dummy_coba.tot_cur_ipotype_hue[0] = 0;
        GPU_uniformbuf_update(ubo_colorband, &dummy_coba);
        colorband_hash = 0;
      }
    }
  }

  return ubo_colorband;
}

gpu::UniformBuf *modifier_gpu_helpers::ensure_texture_params_ubo(Mesh *mesh_owner,
                                                                 Object *deformed_eval,
                                                                 const std::string &key_tex_params,
                                                                 Tex *tex,
                                                                 ModifierData *md,
                                                                 int scene_frame,
                                                                 bool tex_is_byte,
                                                                 bool tex_is_float,
                                                                 int tex_channels,
                                                                 bool has_texcoords)
{
  blender::gpu::GPUTextureParams gpu_tex_params = {};
  if (tex) {
    blender::gpu::fill_texture_params_from_tex(gpu_tex_params, tex, md, deformed_eval, scene_frame,
                                                tex_is_byte, tex_is_float, tex_channels, has_texcoords);
  }

  const size_t size_tex_params = sizeof(blender::gpu::GPUTextureParams);
  gpu::UniformBuf *ubo_texture_params = bke::BKE_mesh_gpu_internal_ubo_get(mesh_owner, key_tex_params);
  if (!ubo_texture_params) {
    ubo_texture_params = bke::BKE_mesh_gpu_internal_ubo_ensure(mesh_owner, deformed_eval, key_tex_params, size_tex_params);
  }
  if (ubo_texture_params) {
    GPU_uniformbuf_update(ubo_texture_params, &gpu_tex_params);
  }
  return ubo_texture_params;
}

}  // namespace modifier_gpu_helpers
}  // namespace draw
}  // namespace blender
