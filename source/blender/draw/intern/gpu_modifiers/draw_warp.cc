/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * Minimal GPU manager for Warp modifier. Starts as a thin wrapper reusing the
 * WaveManager pattern. Current compute shader is a passthrough; real warp
 * math will be added later. This file intentionally keeps the API similar to
 * draw_wave to ease future porting.
 */

#include "draw_warp.hh"

#include "BLI_hash.h"
#include "BLI_math_matrix.h"
#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "BKE_colortools.hh"
#include "BKE_deform.hh"
#include "BKE_image.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "DEG_depsgraph_query.hh"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_texture.hh"

#include "draw_cache_extract.hh"
#include "IMB_imbuf.hh"
#include "../gpu/gpu_modifiers_common/gpu_shader_common_texture_lib.hh"
#include "../gpu/gpu_modifiers_common/gpu_texture_helpers.hh"
#include "MEM_guardedalloc.h"

#include "MOD_util.hh"

namespace blender {
namespace draw {

struct WarpManager::Impl {
  struct MeshModifierKey {
    Mesh *mesh;
    uint32_t modifier_uid;

    uint64_t hash() const
    {
      return (uint64_t(reinterpret_cast<uintptr_t>(mesh)) << 32) | uint64_t(modifier_uid);
    }
    bool operator==(const MeshModifierKey &other) const
    {
      return mesh == other.mesh && modifier_uid == other.modifier_uid;
    }
  };

  struct MeshStaticData {
    std::vector<float> vgroup_weights;
    std::vector<float3> tex_coords;
    std::vector<float> falloff_curve_lut;
    int verts_num = 0;

    Object *deformed = nullptr;

    uint32_t last_verified_hash = 0;
    bool tex_is_byte = true;
    bool tex_is_float = false;
    int tex_channels = 4;
    bool tex_metadata_cached = false;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
};

/* Shader source getter for Warp compute shader. Mirrors draw_wave pattern but
 * without normal helpers. Returns common texture helpers + an (empty) main
 * body with Warp falloff enum defines placed above the main function. */
static std::string get_warp_compute_src(bool image_only = false)
{
  using namespace gpu;
  const std::string common = image_only ? get_common_texture_image_lib_glsl() : get_common_texture_lib_glsl();

  const std::string body = R"GLSL(
#define eWarp_Falloff_None 0
#define eWarp_Falloff_Curve 1
#define eWarp_Falloff_Sharp 2
#define eWarp_Falloff_Smooth 3
#define eWarp_Falloff_Root 4
#define eWarp_Falloff_Linear 5
#define eWarp_Falloff_Const 6
#define eWarp_Falloff_Sphere 7
#define eWarp_Falloff_InvSquare 8

/* Evaluate falloff curve using precomputed LUT with linear interpolation */
float eval_curve_falloff(float t) {
  if (falloff_curve_lut.length() == 0) {
    return t;
  }

  t = clamp(t, 0.0, 1.0);
  int lut_size = int(falloff_curve_lut.length());

  /* Map t to LUT index with sub-pixel precision */
  float idx_f = t * float(lut_size - 1);
  int idx0 = int(floor(idx_f));
  int idx1 = min(idx0 + 1, lut_size - 1);
  float frac = idx_f - float(idx0);

  /* Linear interpolation between two LUT samples for smooth curve */
  float v0 = falloff_curve_lut[idx0];
  float v1 = falloff_curve_lut[idx1];
  return mix(v0, v1, frac);
}

/* Compute warp falloff factor based on distance. Mirrors CPU MOD_warp logic
 * but does not multiply by any external 'force' — caller applies weights. */
float warp_falloff_factor(float len_sq) {
  if (len_sq > falloff_sq) {
    return 0.0;
  }

  if (len_sq > 0.0) {
    float fac;

    if (falloff_type == eWarp_Falloff_Const) {
      fac = 1.0;
      return fac;
    }
    else if (falloff_type == eWarp_Falloff_InvSquare) {
      fac = 1.0 - (len_sq / falloff_sq);
      return fac;
    }

    /* For other types, compute normalized distance */
    fac = 1.0 - (sqrt(len_sq) / falloff_radius);

    switch (falloff_type) {
      case eWarp_Falloff_Curve:
        fac = eval_curve_falloff(fac);
        break;
      case eWarp_Falloff_Sharp:
        fac = fac * fac;
        break;
      case eWarp_Falloff_Smooth:
        fac = 3.0 * fac * fac - 2.0 * fac * fac * fac;
        break;
      case eWarp_Falloff_Root:
        fac = sqrt(fac);
        break;
      case eWarp_Falloff_Linear:
        /* Already linear, do nothing */
        break;
      case eWarp_Falloff_Sphere:
        fac = sqrt(2.0 * fac - fac * fac);
        break;
      default:
        break;
    }

    return fac;
  }
  else {
    return 1.0;
  }
}

void main() {
  /* empty for now */
}

)GLSL";

  return common + body;
}

WarpManager &WarpManager::instance()
{
  static WarpManager manager;
  return manager;
}

WarpManager::WarpManager() : impl_(new Impl()) {}
WarpManager::~WarpManager()
{
  delete impl_;
}

uint32_t WarpManager::compute_warp_hash(const Mesh *mesh_orig, const WarpModifierData *wmd)
{
  if (!mesh_orig || !wmd) {
    return 0;
  }

  /* If required objects are not set, consider this pipeline invalid. */
  if (!(wmd->object_from && wmd->object_to)) {
    return 0;
  }

  uint32_t hash = 0;
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);
  if (wmd->defgrp_name[0] != '\0') {
    hash = BLI_hash_int_2d(hash, BLI_hash_string(wmd->defgrp_name));
  }
  /* Basic flags that may affect behavior */
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(wmd->texture)));
  /* Mapping related fields that affect sampling */
  hash = BLI_hash_int_2d(hash, int(wmd->texmapping));
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(wmd->map_object)));
  if (wmd->map_bone[0] != '\0') {
    hash = BLI_hash_int_2d(hash, BLI_hash_string(wmd->map_bone));
  }
  if (wmd->uvlayer_name[0] != '\0') {
    hash = BLI_hash_int_2d(hash, BLI_hash_string(wmd->uvlayer_name));
  }

  /* Include object pointers so changes to referenced objects invalidate cache. */
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(wmd->object_from)));
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(wmd->object_to)));

  if (wmd->texture) {
    hash = BLI_hash_int_2d(hash, uint32_t(wmd->texture->type));
    if (wmd->texture->ima) {
      Image *ima = wmd->texture->ima;
      hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(ima)));
      hash = BLI_hash_int_2d(hash, uint32_t(ima->source));
      hash = BLI_hash_int_2d(hash, uint32_t(wmd->texture->iuser.tile));
      hash = BLI_hash_int_2d(hash, uint32_t(wmd->texture->iuser.framenr));
      hash = BLI_hash_int_2d(hash, uint32_t(wmd->texture->imaflag));
      hash = BLI_hash_int_2d(hash, uint32_t(wmd->texture->extend));

      /* Mix Image generation flags/values (use actual values, not addresses). */
      hash = BLI_hash_int_2d(hash, uint32_t(ima->alpha_mode));

      /* Hash the colorspace name string into the running hash. */
      if (ima->colorspace_settings.name[0] != '\0') {
        hash = BLI_hash_int_2d(hash, BLI_hash_string(ima->colorspace_settings.name));
      }
      else {
        hash = BLI_hash_int_2d(hash, 0);
      }

      ImageTile *tile = BKE_image_get_tile(ima, wmd->texture->iuser.tile);
      if (tile) {
        hash = BLI_hash_int_2d(hash, uint32_t(tile->gen_flag));
        hash = BLI_hash_int_2d(hash, uint32_t(tile->gen_type));
        hash = BLI_hash_int_2d(hash, uint32_t(tile->gen_depth));
      }
    }
  }

  /* Hash curve changed_timestamp to detect falloff curve edits (if any). */
  if (wmd->curfalloff) {
    hash = BLI_hash_int_2d(hash, wmd->curfalloff->changed_timestamp);
  }

  return hash;
}

void WarpManager::ensure_static_resources(const WarpModifierData *wmd,
                                          Object *deform_ob,
                                          Mesh *orig_mesh,
                                          uint32_t pipeline_hash)
{
  if (!orig_mesh || !wmd) {
    return;
  }

  Impl::MeshModifierKey key{orig_mesh, uint32_t(wmd->modifier.persistent_uid)};
  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(key);

  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);

  if (!first_time && !hash_changed) {
    return;
  }

  msd.last_verified_hash = pipeline_hash;
  msd.verts_num = orig_mesh->verts_num;
  msd.deformed = deform_ob;

  /* Extract vertex group weights */
  msd.vgroup_weights.clear();
  if (wmd->defgrp_name[0] != '\0') {
    const int defgrp_index = BKE_id_defgroup_name_index(&orig_mesh->id, wmd->defgrp_name);
    if (defgrp_index != -1) {
      Span<MDeformVert> dverts = orig_mesh->deform_verts();
      if (!dverts.is_empty()) {
        msd.vgroup_weights.resize(orig_mesh->verts_num, 0.0f);
        const bool invert_vgroup = (wmd->flag & MOD_WARP_INVERT_VGROUP) != 0;
        for (int v = 0; v < orig_mesh->verts_num; ++v) {
          const MDeformVert &dvert = dverts[v];
          float weight = BKE_defvert_find_weight(&dvert, defgrp_index);
          msd.vgroup_weights[v] = invert_vgroup ? 1.0f - weight : weight;
        }
      }
    }
  }

  /* If no vertex group was found or specified, use default weight = 1.0 per-vertex. */
  if (msd.vgroup_weights.empty()) {
    if (orig_mesh->verts_num > 0) {
      msd.vgroup_weights.resize(orig_mesh->verts_num, 1.0f);
    }
    else {
      msd.vgroup_weights.resize(1, 1.0f);
    }
  }

  /* Extract falloff curve LUT if using curve falloff */
  msd.falloff_curve_lut.clear();
  if (wmd->falloff_type == eWarp_Falloff_Curve) {
    if (wmd->curfalloff) {
      BKE_curvemapping_init(wmd->curfalloff);
      const int LUT_SIZE = 1024;
      msd.falloff_curve_lut.resize(LUT_SIZE);
      for (int i = 0; i < LUT_SIZE; ++i) {
        float t = float(i) / float(LUT_SIZE - 1);
        msd.falloff_curve_lut[i] = BKE_curvemapping_evaluateF(wmd->curfalloff, 0, t);
      }
    }
  }

  /* Extract texture coordinates (if texture is present). Use MOD_get_texture_coords
   * to ensure identical mapping behavior with CPU modifier (supports OBJECT/UV/etc). */
  msd.tex_coords.clear();
  if (wmd->texture) {
    const int verts_num = orig_mesh->verts_num;
    float(*tex_co)[3] = MEM_malloc_arrayN<float[3]>(verts_num, "warp_tex_coords");

    MOD_get_texture_coords(
        reinterpret_cast<MappingInfoModifierData *>(const_cast<WarpModifierData *>(wmd)),
        nullptr, /* ctx (not needed for coordinate calculation) */
        deform_ob,
        orig_mesh,
        nullptr, /* cos (use original positions) */
        tex_co);

    msd.tex_coords.resize(verts_num);
    for (int v = 0; v < verts_num; ++v) {
      msd.tex_coords[v] = float3(tex_co[v]);
    }

    MEM_freeN(tex_co);
  }
}

gpu::StorageBuf *WarpManager::dispatch_deform(const WarpModifierData *wmd,
                                              Depsgraph *depsgraph,
                                              Object *deformed_eval,
                                              MeshBatchCache *cache,
                                              gpu::StorageBuf *ssbo_in)
{
  if (!wmd || !ssbo_in || !cache) {
    return nullptr;
  }

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return nullptr;
  }

  Impl::MeshModifierKey key{mesh_owner, uint32_t(wmd->modifier.persistent_uid)};
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(key);
  if (!msd_ptr) {
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  const std::string key_prefix = "warp_" + std::to_string(key.hash()) + "_";
  const std::string key_vgroup = key_prefix + "vgroup_weights";
  const std::string key_texcoords = key_prefix + "tex_coords";
  gpu::StorageBuf *ssbo_texcoords = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_texcoords);

  /* Ensure vgroup SSBO */
  gpu::StorageBuf *ssbo_vgroup = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_vgroup);
  if (!msd.vgroup_weights.empty()) {
    if (!ssbo_vgroup) {
      const size_t size_vgroup = msd.vgroup_weights.size() * sizeof(float);
      ssbo_vgroup = bke::BKE_mesh_gpu_internal_ssbo_ensure(
          mesh_owner, deformed_eval, key_vgroup, size_vgroup);
      if (ssbo_vgroup) {
        GPU_storagebuf_update(ssbo_vgroup, msd.vgroup_weights.data());
      }
    }
  }

  /* Ensure texcoord SSBO */
  if (!msd.vgroup_weights.empty() && ssbo_texcoords) {
    const size_t size_texcoords = msd.vgroup_weights.size() * sizeof(float) * 2;
    ssbo_texcoords = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_texcoords, size_texcoords);
  }

  /* Upload falloff curve LUT SSBO (if available) */
  const std::string key_curve = key_prefix + "falloff_curve_lut";
  gpu::StorageBuf *ssbo_curve = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_curve);
  if (!msd.falloff_curve_lut.empty()) {
    if (!ssbo_curve) {
      const size_t size_curve = msd.falloff_curve_lut.size() * sizeof(float);
      ssbo_curve = bke::BKE_mesh_gpu_internal_ssbo_ensure(
          mesh_owner, deformed_eval, key_curve, size_curve);
      if (ssbo_curve) {
        GPU_storagebuf_update(ssbo_curve, msd.falloff_curve_lut.data());
      }
    }
  }
  else {
    if (!ssbo_curve) {
      ssbo_curve = bke::BKE_mesh_gpu_internal_ssbo_ensure(
          mesh_owner, deformed_eval, key_curve, sizeof(float));
      if (ssbo_curve) {
        float dummy = 1.0f;
        GPU_storagebuf_update(ssbo_curve, &dummy);
      }
    }
  }

  /* Create output SSBO (use get -> ensure pattern to avoid unnecessary allocations). */
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  const std::string key_out = key_prefix + "output";
  gpu::StorageBuf *ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_out);
  if (!ssbo_out) {
    ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, deformed_eval, key_out, size_out);
    if (!ssbo_out) {
      return nullptr;
    }
  }

  const Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  const bool is_seq_or_movie = ELEM(wmd->texture->ima->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE);

  ImageUser iuser = wmd->texture->iuser; /* copy */
  /* Update frame for animated images */
  if (is_seq_or_movie && scene_eval) {
    BKE_image_user_frame_calc(wmd->texture->ima, &iuser, int(scene_eval->r.cfra));
  }

  /* Prepare GPU texture (if a Tex/Image is present). Follow Displace pattern: */
  gpu::Texture *gpu_texture = nullptr;
  bool shader_has_texture = (wmd->texture != nullptr);
  if (shader_has_texture && wmd->texture && wmd->texture->ima) {
    Image *ima = wmd->texture->ima;
    Tex *tex = wmd->texture;

    const bool is_non_color = (ima && ima->colorspace_settings.name[0] != '\0' &&
                               STREQ(ima->colorspace_settings.name, "Non-Color"));

    if (is_non_color) {
      gpu_texture = BKE_image_get_gpu_texture(ima, &iuser);
      if (gpu_texture && !msd.tex_metadata_cached) {
        msd.tex_is_float = GPU_texture_has_float_format(gpu_texture);
        msd.tex_is_byte = !msd.tex_is_float;
        msd.tex_channels = GPU_texture_component_len(GPU_texture_format(gpu_texture));
        msd.tex_metadata_cached = true;
      }
    }
    else {
      /* Slow path: manual ImBuf upload with no colorspace decode (raw bytes). */
      const std::string key_texture = key_prefix + "texture_" +
                                     std::to_string(reinterpret_cast<uintptr_t>(ima)) + "_" +
                                     std::to_string(iuser.framenr);

      gpu_texture = bke::BKE_mesh_gpu_internal_texture_get(mesh_owner, key_texture);

      const bool is_animated = ELEM(ima->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE);
      if (!gpu_texture || (is_animated && !gpu_texture)) {
        ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &iuser, nullptr);
        if (ibuf && (ibuf->float_buffer.data || ibuf->byte_buffer.data)) {
          gpu::TextureFormat format = ibuf->float_buffer.data ?
                                          gpu::TextureFormat::SFLOAT_16_16_16_16 :
                                          gpu::TextureFormat::UNORM_8_8_8_8;

          gpu_texture = GPU_texture_create_2d(
              "warp_tex_raw", ibuf->x, ibuf->y, 1, format, GPU_TEXTURE_USAGE_SHADER_READ, nullptr);

          if (gpu_texture) {
            /* Upload WITHOUT colorspace conversion (raw bytes/floats). Use shared helper
             * to match Displace behavior and avoid duplicating byte/float upload logic. */
            gpu::displace_upload_ibuf_to_texture(gpu_texture, ibuf, ima->colorspace_settings.name);

            bke::BKE_mesh_gpu_internal_texture_ensure(mesh_owner, deformed_eval, key_texture, gpu_texture);

            if (!msd.tex_metadata_cached) {
              msd.tex_is_byte = (ibuf->byte_buffer.data != nullptr);
              msd.tex_is_float = (ibuf->float_buffer.data != nullptr);
              msd.tex_channels = ibuf->channels;
              msd.tex_metadata_cached = true;
            }
          }
        }
        if (ibuf) {
          BKE_image_release_ibuf(ima, ibuf, nullptr);
        }
      }
    }
  }

  /* If shader expects a texture but we have none, create a 1x1 dummy texture to satisfy binding. */
  if (shader_has_texture && !gpu_texture) {
    const std::string key_dummy = key_prefix + "dummy_warp_tex";
    gpu_texture = bke::BKE_mesh_gpu_internal_texture_get(mesh_owner, key_dummy);
    if (!gpu_texture) {
      unsigned char pixel[4] = {128, 128, 128, 255};
      gpu_texture = GPU_texture_create_2d(
          "warp_dummy_tex", 1, 1, 1, gpu::TextureFormat::UNORM_8_8_8_8, GPU_TEXTURE_USAGE_SHADER_READ, nullptr);
      if (gpu_texture) {
        GPU_texture_update(gpu_texture, GPU_DATA_UBYTE, pixel);
        bke::BKE_mesh_gpu_internal_texture_ensure(mesh_owner, deformed_eval, key_dummy, gpu_texture);
      }
    }
    if (gpu_texture && !msd.tex_metadata_cached) {
      msd.tex_is_byte = true;
      msd.tex_is_float = false;
      msd.tex_channels = GPU_texture_component_len(GPU_texture_format(gpu_texture));
      msd.tex_metadata_cached = true;
    }
  }

  /* Simple passthrough compute shader: copy input to output. */
  const std::string shader_key = std::string("warp_compute_v1");
  gpu::Shader *shader = bke::BKE_mesh_gpu_internal_shader_get(mesh_owner, shader_key);
  Mesh *mesh_eval = id_cast<Mesh *>(deformed_eval->data);
  bke::MeshGpuData *mesh_gpu_data = bke::BKE_mesh_gpu_ensure_data(mesh_owner, mesh_eval);
  if (!shader) {
    using namespace gpu::shader;
    ShaderCreateInfo info("pyGPU_Shader");
    info.local_group_size(256, 1, 1);

    std::string shader_src;
    if (wmd->texture) {
      shader_src += "#define HAS_TEXTURE\n";
    }
    shader_src += get_warp_compute_src(wmd->texture && wmd->texture->type == TEX_IMAGE);

    info.typedef_source_generated = gpu::get_texture_typedefs_glsl();
    info.compute_source_generated = gpu::get_texture_params_glsl() + shader_src;

    info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
    info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
    /* vgroup binding available */
    info.storage_buf(2, Qualifier::read, "float", "vgroup_weights[]");
    if (wmd->texture) {
      info.storage_buf(3, Qualifier::read, "vec4", "texture_coords[]");
      info.sampler(0, ImageType::Float2D, "warp_texture");
    }
    /* Falloff curve LUT (binding 4) */
    info.storage_buf(4, Qualifier::read, "float", "falloff_curve_lut[]");
    shader = bke::BKE_mesh_gpu_internal_shader_ensure(mesh_owner, deformed_eval, shader_key, info);
  }
  if (!shader) {
    return nullptr;
  }

  const gpu::shader::SpecializationConstants *constants = &GPU_shader_get_default_constant_state(
      shader);
  GPU_shader_bind(shader, constants);

  GPU_storagebuf_bind(ssbo_out, 0);
  GPU_storagebuf_bind(ssbo_in, 1);
  if (ssbo_vgroup) {
    GPU_storagebuf_bind(ssbo_vgroup, 2);
  }
  if (ssbo_texcoords) {
    GPU_storagebuf_bind(ssbo_texcoords, 3);
  }
  if (ssbo_curve) {
    GPU_storagebuf_bind(ssbo_curve, 4);
  }
  if (gpu_texture) {
    GPU_texture_bind(gpu_texture, 0);
  }

  const int group_size = 256;
  int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1, constants);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_TEXTURE_FETCH);
  GPU_shader_unbind();

  return ssbo_out;
}

void WarpManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh)
    return;
  Vector<Impl::MeshModifierKey> keys_to_remove;
  for (const auto &item : impl_->static_map.items()) {
    if (item.key.mesh == mesh) {
      keys_to_remove.append(item.key);
    }
  }
}

void WarpManager::invalidate_all(Mesh *mesh)
{
  if (!mesh)
    return;
  bke::BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);
}

void WarpManager::free_all()
{
  impl_->static_map.clear();
}

}  // namespace draw
}  // namespace blender
