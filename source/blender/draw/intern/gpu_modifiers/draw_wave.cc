/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * GPU-accelerated Wave modifier manager (skeleton + simple image-less compute).
 *
 * This implementation provides a minimal working compute-path for Wave that
 * does not sample any images. It supports vertex-group weighting and the
 * core wave math (amplitude, speed, width, narrowness, falloff, lifetime).
 * The structure mirrors `draw_displace.cc` to ease later feature parity.
 */

#include "draw_wave.hh"

#include "BLI_hash.h"

#include "BKE_deform.hh"
#include "BKE_image.hh"

#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "DEG_depsgraph_query.hh"

#include "draw_cache_extract.hh"

#include "GPU_compute.hh"

#include "MOD_util.hh"

#include "draw_modifier_gpu_helpers.hh"
#include "../gpu/gpu_deform_common/gpu_shader_common_texture_lib.hh"
#include "../gpu/gpu_deform_common/gpu_shader_common_normal_lib.hh"
#include "../blenkernel/intern/mesh_gpu_cache.hh"

namespace blender {
namespace draw {

struct WaveManager::Impl {
  struct MeshModifierKey {
    Mesh *mesh;
    uint32_t modifier_uid;

    uint64_t hash() const { return (uint64_t(reinterpret_cast<uintptr_t>(mesh)) << 32) | uint64_t(modifier_uid); }
    bool operator==(const MeshModifierKey &other) const { return mesh == other.mesh && modifier_uid == other.modifier_uid; }
  };

  struct MeshStaticData {
    std::vector<float> vgroup_weights;
    std::vector<float3> tex_coords;
    int verts_num = 0;

    Object *deformed = nullptr;

    uint32_t last_verified_hash = 0;
    bool tex_is_byte = true;
    bool tex_is_float = false;
    int tex_channels = 4;
    uint32_t colorband_hash = 0;
    bool tex_metadata_cached = false;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
};

/* Shader source getter for Wave compute shader. Placed here to keep shader text
 * separate and cacheable similar to draw_displace. */
static std::string get_wave_compute_src(bool image_only = false)
{
  using namespace gpu;
  /* Provide different common texture helpers depending on whether the shader
   * is image-only (no procedural texture code) or full. This mirrors
   * draw_displace and keeps shader variants consistent. */
  const std::string common = image_only ? get_common_texture_image_lib_glsl() : get_common_texture_lib_glsl();
  /* Normal helpers required for vertex-normal based displacement. */
  const std::string normal_lib = get_common_normal_lib_glsl();

  const std::string body = R"GLSL(

#define MOD_WAVE_X (1 << 1)
#define MOD_WAVE_Y (1 << 2)

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= deformed_positions.length()) return;

  vec4 ip = input_positions[v];
  vec3 co = ip.xyz;

  /* match CPU early-out when lifefac == 0.0f */
  if (u_lifefac == 0.0) {
    deformed_positions[v] = ip;
    return;
  }

  float ctime = u_time;

  /* Precompute falloff inverse like CPU */
  const float falloff = u_falloff;
  const float falloff_inv = (falloff != 0.0) ? 1.0 / falloff : 1.0;

  /* vertex group weight */
  float def_weight = 1.0;
  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    def_weight = vgroup_weights[v];
    if (def_weight == 0.0) {
      deformed_positions[v] = ip;
      return;
    }
  }
  if (u_lifefac != 0.0) {
    /* local coords relative to start position */
    float x = co.x - u_startx;
    float y = co.y - u_starty;

    /* initial amplitude depending on axis */
    float amplit = 0.0;
    int axis = u_axis; /* bitflags: MOD_WAVE_X, MOD_WAVE_Y */
    /* Mirror CPU switch(wmd_axis) */
    switch (axis) {
      case MOD_WAVE_X | MOD_WAVE_Y:
        amplit = sqrt(x * x + y * y);
        break;
      case MOD_WAVE_X:
        amplit = x;
        break;
      case MOD_WAVE_Y:
        amplit = y;
        break;
      default:
        amplit = 0.0;
        break;
    }

    /* propagate wave over time */
    amplit -= (ctime - u_timeoffs) * u_speed;

    /* cyclic wrapping (match CPU fmodf behaviour) */
    if (u_cyclic != 0) {
      float tmp = amplit - u_width;
      float denom = 2.0 * u_width;
      amplit = tmp - denom * trunc(tmp / denom) + u_width;
    }
    /* falloff calculation */
    float falloff_fac = 1.0;
    if (falloff != 0.0) {
      float dist = 0.0;
      /* Use same switch logic as CPU */
      switch (axis) {
        case MOD_WAVE_X | MOD_WAVE_Y:
          dist = sqrt(x * x + y * y);
          break;
        case MOD_WAVE_X:
          dist = abs(x);
          break;
        case MOD_WAVE_Y:
          dist = abs(y);
          break;
        default:
          dist = 0.0;
          break;
      }

      falloff_fac = 1.0 - (dist * falloff_inv);
      falloff_fac = clamp(falloff_fac, 0.0, 1.0);
    }

    /* gaussian range check + shaping */
    if ((falloff_fac != 0.0) && (amplit > -u_width) && (amplit < u_width)) {
      /* shape amplitude */
      amplit = amplit * u_narrow;
      amplit = (1.0 / exp(amplit * amplit) - u_minfac);

  #ifdef HAS_TEXTURE
      /* texture sampling (if compiled with texture support) */
      TexResult_tex texres;
      float tex_int = BKE_texture_get_value(texres, texture_coords[v].xyz, input_positions[v], int(v));
      amplit *= tex_int;
  #endif

      /* apply vertex-group weight and falloff */
      amplit *= def_weight * falloff_fac;

      /* determine normal or axis displacement */
      vec3 n = vec3(0.0, 0.0, 1.0);
      if (u_use_normal != 0) {
        vec3 n_mesh = vec3(0.0);
        n_mesh = compute_vertex_normal_smooth(int(v));
        n = vec3(0.0);
        if (u_use_normal_x != 0) n.x = n_mesh.x;
        if (u_use_normal_y != 0) n.y = n_mesh.y;
        if (u_use_normal_z != 0) n.z = n_mesh.z;
      }

      vec3 disp = vec3(0.0);
      if (u_use_normal != 0) {
        if (u_use_normal_x != 0) disp.x = u_lifefac * amplit * n.x;
        if (u_use_normal_y != 0) disp.y = u_lifefac * amplit * n.y;
        if (u_use_normal_z != 0) disp.z = u_lifefac * amplit * n.z;
      }
      else {
        disp.z = u_lifefac * amplit;
      }

      co += disp;
      deformed_positions[v] = vec4(co, 1.0);
      return;
    }
  }

  /* no change */
  deformed_positions[v] = ip;
}
)GLSL";

  /* POSITION_BUFFER macro is required by normal helpers to reference the
   * input positions buffer when computing normals from topology. */
  return std::string("#define POSITION_BUFFER input_positions\n") + common + normal_lib + body;
}

WaveManager &WaveManager::instance()
{
  static WaveManager manager;
  return manager;
}

WaveManager::WaveManager() : impl_(new Impl()) {}
WaveManager::~WaveManager() { delete impl_; }

uint32_t WaveManager::compute_wave_hash(const Mesh *mesh_orig, const WaveModifierData *wmd)
{
  if (!mesh_orig || !wmd) {
    return 0;
  }

  uint32_t hash = 0;
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);
  hash = BLI_hash_int_2d(hash, int(wmd->flag));
  hash = BLI_hash_int_2d(hash, int(wmd->texmapping));
  if (wmd->defgrp_name[0] != '\0') {
    hash = BLI_hash_int_2d(hash, BLI_hash_string(wmd->defgrp_name));
  }

  /* Include objectcenter pointer to detect changes in the referenced object */
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(wmd->objectcenter)));

  /* Include map_object pointer (for OBJECT mapping mode) */
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(wmd->map_object)));

  /* Texture-related metadata that affect sampling/result (similar to Displace). */
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(wmd->texture)));

  if (wmd->texture) {
    hash = BLI_hash_int_2d(hash, uint32_t(wmd->texture->type));
    if (wmd->texture->ima) {
      Image *ima = wmd->texture->ima;
      hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(ima)));
      hash = BLI_hash_int_2d(hash, uint32_t(ima->source));
      hash = BLI_hash_int_2d(hash, uint32_t(wmd->texture->iuser.tile));
      hash = BLI_hash_int_2d(hash, uint32_t(wmd->texture->iuser.framenr));
      hash = BLI_hash_int_2d(hash, uint32_t(wmd->texture->imaflag));

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

  /* Hash deform_verts pointer (detects vertex group changes) */
  Span<MDeformVert> dverts = mesh_orig->deform_verts();
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(dverts.data())));

  /* Do not include runtime parameters like speed/height here. */
  return hash;
}

void WaveManager::ensure_static_resources(const WaveModifierData *wmd, Object *deform_ob, Mesh *orig_mesh, uint32_t pipeline_hash)
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
        const bool invert_vgroup = (wmd->flag & MOD_WAVE_INVERT_VGROUP) != 0;
        for (int v = 0; v < orig_mesh->verts_num; ++v) {
          const MDeformVert &dvert = dverts[v];
          float weight = BKE_defvert_find_weight(&dvert, defgrp_index);
          msd.vgroup_weights[v] = invert_vgroup ? 1.0f - weight : weight;
        }
      }
    }
  }

  /* Extract texture coordinates (if texture is present) */
  msd.tex_coords.clear();
  if (wmd->texture) {
    const int verts_num = orig_mesh->verts_num;
    float(*tex_co)[3] = MEM_malloc_arrayN<float[3]>(verts_num, "wave_tex_coords");

    MOD_get_texture_coords(
        reinterpret_cast<MappingInfoModifierData *>(const_cast<WaveModifierData *>(wmd)),
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

  /* If no vertex group was found or specified, use default weight = 1.0 per-vertex.
   * This simplifies later SSBO handling: we always have a per-vertex buffer to upload. */
  if (msd.vgroup_weights.empty()) {
    if (orig_mesh->verts_num > 0) {
      msd.vgroup_weights.resize(orig_mesh->verts_num, 1.0f);
    }
    else {
      /* Ensure at least one element to avoid zero-size allocations later. */
      msd.vgroup_weights.resize(1, 1.0f);
    }
  }
}

gpu::StorageBuf *WaveManager::dispatch_deform(const WaveModifierData *wmd,
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

  /* Scene time: use DEG_get_ctime to match CPU modifier behavior (MOD_wave). */
  float ctime = DEG_get_ctime(depsgraph);
  Scene *scene = DEG_get_evaluated_scene(depsgraph);
  int scene_frame = 0;
  if (scene) {
    scene_frame = int(scene->r.cfra);
  }

  /* Create unique keys and SSBOs similar to Displace pattern */
  const std::string key_prefix = "wave_" + std::to_string(key.hash()) + "_";
  const std::string key_vgroup = key_prefix + "vgroup_weights";
  const std::string key_out = key_prefix + "output";

  /* Ensure vgroup SSBO using helper (get -> ensure + upload when created). */
  gpu::StorageBuf *ssbo_vgroup = modifier_gpu_helpers::ensure_vgroup_ssbo(
      mesh_owner, deformed_eval, key_vgroup, msd.vgroup_weights, msd.verts_num);

  /* Upload texture coordinates SSBO (if available) and prepare texture binding. */
  const std::string key_texcoords = key_prefix + "tex_coords";
  gpu::StorageBuf *ssbo_texcoords = nullptr;
  gpu::Texture *gpu_texture = nullptr;

  /* Use shared helper to prepare texture and texcoords (handles image user frame, ImBuf upload and caching). */
  if (wmd->texture) {
    const bool create_dummy = (wmd->texture->type != TEX_IMAGE);
    const bool is_uv_mapping = (wmd->texmapping == MOD_DISP_MAP_UV);
    gpu_texture = modifier_gpu_helpers::prepare_gpu_texture_and_texcoords(
        mesh_owner,
        deformed_eval,
        depsgraph,
        wmd->texture,
        msd.tex_coords,
        msd.tex_is_byte,
        msd.tex_is_float,
        msd.tex_channels,
        msd.tex_metadata_cached,
        key_prefix,
        &ssbo_texcoords,
        is_uv_mapping,
        create_dummy);
  }

  /* Create output SSBO (use get -> ensure pattern to avoid unnecessary allocations). */
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  gpu::StorageBuf *ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_out);
  if (!ssbo_out) {
    ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, deformed_eval, key_out, size_out);
    if (!ssbo_out) {
      return nullptr;
    }
  }

  /* Upload ColorBand UBO if texture has colorband enabled */
  const std::string key_colorband = key_prefix + "colorband";
  gpu::UniformBuf *ubo_colorband = nullptr;
  /* Pass msd.colorband_hash directly so helper can update it when uploading a real colorband. */
  ubo_colorband = modifier_gpu_helpers::ensure_colorband_ubo(
      mesh_owner, deformed_eval, key_colorband, wmd->texture, msd.colorband_hash);

  /* TextureParams UBO */
  const std::string key_tex_params = key_prefix + "texture_params";
  gpu::UniformBuf *ubo_texture_params = blender::draw::modifier_gpu_helpers::ensure_texture_params_ubo(
      mesh_owner,
      deformed_eval,
      key_tex_params,
      wmd->texture,
      (ModifierData *)wmd,
      scene_frame,
      msd.tex_is_byte,
      msd.tex_is_float,
      msd.tex_channels,
      !msd.tex_coords.empty());

  /* Noise tables (shared helpers) - create or get cached textures holding
   * permutation/gradient/point data used by procedural noise GLSL helpers. */
  const std::string key_hash = key_prefix + "hash_perm";
  const std::string key_hashvect = key_prefix + "hash_vectf";
  const std::string key_hashpnt = key_prefix + "hash_pntf3";

  gpu::Texture *tex_hash = blender::gpu::get_noise_hash_texture(mesh_owner, deformed_eval, key_hash);
  gpu::Texture *tex_hashvect = blender::gpu::get_noise_hashvect_texture(mesh_owner, deformed_eval, key_hashvect);
  gpu::Texture *tex_hashpnt = blender::gpu::get_noise_hashpnt_texture(mesh_owner, deformed_eval, key_hashpnt);

  /* Create shader (image-less compute) */
  bool image_only_compile = false;
  if (wmd->texture) {
    image_only_compile = (wmd->texture->type == TEX_IMAGE);
  }

  const std::string shader_key = std::string("wave_compute_v1") + (image_only_compile ? "_image" : "_full");
  gpu::Shader *shader = bke::BKE_mesh_gpu_internal_shader_get(mesh_owner, shader_key);
  Mesh *mesh_eval = id_cast<Mesh *>(deformed_eval->data);
  bke::MeshGpuData *mesh_gpu_data = bke::BKE_mesh_gpu_ensure_data(mesh_owner, mesh_eval);
  bool shader_has_texture = (wmd->texture != nullptr);
  if (!shader) {
    using namespace gpu::shader;
    ShaderCreateInfo info("pyGPU_Shader");
    info.local_group_size(256, 1, 1);

    std::string shader_src;
    if (shader_has_texture) {
      shader_src += "#define HAS_TEXTURE\n";
    }
    shader_src += get_wave_compute_src(image_only_compile);

    /* Mesh topology accessors are required by normal helpers. Ensure we have
     * mesh GPU data available and concatenate topology GLSL before the shader. */
    
    std::string glsl_accessors = bke::BKE_mesh_gpu_topology_glsl_accessors_string(mesh_gpu_data->topology);

    /* Use shared typedefs when texture sampling / params are required. */
    info.typedef_source_generated = gpu::get_texture_typedefs_glsl();
    info.compute_source_generated = gpu::get_texture_params_glsl() + glsl_accessors + shader_src;

    info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
    info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
    info.storage_buf(2, Qualifier::read, "float", "vgroup_weights[]");
    if (shader_has_texture) {
      info.storage_buf(3, Qualifier::read, "vec4", "texture_coords[]");
      info.sampler(0, ImageType::Float2D, "displacement_texture");
    }
    /* Noise/gradient permutation buffers used by GLSL noise helpers. */
    info.sampler(1, ImageType::Float1D, "u_hash_buf");
    info.sampler(2, ImageType::Float1D, "u_hashvectf_buf");
    info.sampler(3, ImageType::Float1D, "u_hashpntf3_buf");
    /* Topology SSBO (binding 15) required by normal helpers */
    info.storage_buf(15, Qualifier::read, "int", "topo[]");
    /* ColorBand UBO (binding 4) */
    info.uniform_buf(4, "ColorBand", "tex_colorband");
    /* TextureParams UBO (binding 5) */
    info.uniform_buf(5, "TextureParams", "tex_params");

    /* Push constants / uniforms expected by shader */
    info.push_constant(Type::float_t, "u_startx");
    info.push_constant(Type::float_t, "u_starty");
    info.push_constant(Type::float_t, "u_time");
    info.push_constant(Type::float_t, "u_timeoffs");
    info.push_constant(Type::float_t, "u_speed");
    info.push_constant(Type::float_t, "u_width");
    info.push_constant(Type::float_t, "u_narrow");
    info.push_constant(Type::float_t, "u_minfac");
    info.push_constant(Type::float_t, "u_falloff");
    info.push_constant(Type::float_t, "u_lifefac");
    info.push_constant(Type::int_t, "u_axis");
    info.push_constant(Type::int_t, "u_cyclic");
    info.push_constant(Type::int_t, "u_use_normal");
    /* Per-axis normal enable flags (X/Y/Z) */
    info.push_constant(Type::int_t, "u_use_normal_x");
    info.push_constant(Type::int_t, "u_use_normal_y");
    info.push_constant(Type::int_t, "u_use_normal_z");

    /* Add specialization constants for topology if present */
    bke::BKE_mesh_gpu_topology_add_specialization_constants(info, mesh_gpu_data->topology);
    shader = bke::BKE_mesh_gpu_internal_shader_ensure(mesh_owner, deformed_eval, shader_key, info);
  }
  if (!shader) {
    return nullptr;
  }

  /* Bind and dispatch */
  const gpu::shader::SpecializationConstants *constants = &GPU_shader_get_default_constant_state(shader);
  GPU_shader_bind(shader, constants);

  GPU_storagebuf_bind(ssbo_out, 0);
  GPU_storagebuf_bind(ssbo_in, 1);
  if (ssbo_vgroup) {
    GPU_storagebuf_bind(ssbo_vgroup, 2);
  }
  if (ssbo_texcoords) {
    GPU_storagebuf_bind(ssbo_texcoords, 3);
  }
  if (gpu_texture) {
    GPU_texture_bind(gpu_texture, 0);
  }
  /* Bind shared noise textures (units must match shader sampler bindings). */
  if (tex_hash) {
    GPU_texture_bind(tex_hash, 1);
  }
  if (tex_hashvect) {
    GPU_texture_bind(tex_hashvect, 2);
  }
  if (tex_hashpnt) {
    GPU_texture_bind(tex_hashpnt, 3);
  }
  /* Bind topology SSBO required by normal helpers */
  GPU_storagebuf_bind(mesh_gpu_data->topology.ssbo, 15);
  /* Bind ColorBand and TextureParams UBOs */
  if (ubo_colorband) {
    GPU_uniformbuf_bind(ubo_colorband, 4);
  }
  if (ubo_texture_params) {
    GPU_uniformbuf_bind(ubo_texture_params, 5);
  }

  /* Set uniforms (push constants) */
  GPU_shader_uniform_1f(shader, "u_startx", wmd->startx);
  GPU_shader_uniform_1f(shader, "u_starty", wmd->starty);
  GPU_shader_uniform_1f(shader, "u_time", ctime);
  GPU_shader_uniform_1f(shader, "u_timeoffs", wmd->timeoffs);
  GPU_shader_uniform_1f(shader, "u_speed", wmd->speed);
  GPU_shader_uniform_1f(shader, "u_width", wmd->width);
  GPU_shader_uniform_1f(shader, "u_narrow", wmd->narrow);
  float minfac = float(1.0 / exp(wmd->width * wmd->narrow * wmd->width * wmd->narrow));
  GPU_shader_uniform_1f(shader, "u_minfac", minfac);
  GPU_shader_uniform_1f(shader, "u_falloff", wmd->falloff);
  /* Compute lifefac to match CPU behavior in MOD_wave:
   * lifefac starts as height. If lifetime is enabled and time passed exceeds
   * lifetime, lifefac transitions to 0 over `damp` using sqrt easing.
   * If damp == 0 on the modifier, use default 10.0f (CPU sets this on demand).
   */
  float lifefac = wmd->height;
  float damp = (wmd->damp == 0.0f) ? 10.0f : wmd->damp;
  if (wmd->lifetime != 0.0f) {
    float x = ctime - wmd->timeoffs;

    if (x > wmd->lifetime) {
      lifefac = x - wmd->lifetime;

      if (lifefac > damp) {
        lifefac = 0.0f;
      }
      else {
        lifefac = (wmd->height * (1.0f - sqrtf(lifefac / damp)));
      }
    }
  }
  GPU_shader_uniform_1f(shader, "u_lifefac", lifefac);

  const int wmd_axis = wmd->flag & (MOD_WAVE_X | MOD_WAVE_Y);
  GPU_shader_uniform_1i(shader, "u_axis", wmd_axis);
  GPU_shader_uniform_1i(shader, "u_cyclic", (wmd->flag & MOD_WAVE_CYCL) ? 1 : 0);
  GPU_shader_uniform_1i(shader, "u_use_normal", (wmd->flag & MOD_WAVE_NORM) ? 1 : 0);
  
  /* Per-axis normal flags */
  GPU_shader_uniform_1i(shader, "u_use_normal_x", (wmd->flag & MOD_WAVE_NORM_X) ? 1 : 0);
  GPU_shader_uniform_1i(shader, "u_use_normal_y", (wmd->flag & MOD_WAVE_NORM_Y) ? 1 : 0);
  GPU_shader_uniform_1i(shader, "u_use_normal_z", (wmd->flag & MOD_WAVE_NORM_Z) ? 1 : 0);

  const int group_size = 256;
  int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1, constants);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_TEXTURE_FETCH);
  GPU_shader_unbind();

  /* Unbind texture and UBOs */
  if (gpu_texture) {
    GPU_texture_unbind(gpu_texture);
  }
  if (tex_hash) {
    GPU_texture_unbind(tex_hash);
  }
  if (tex_hashvect) {
    GPU_texture_unbind(tex_hashvect);
  }
  if (tex_hashpnt) {
    GPU_texture_unbind(tex_hashpnt);
  }
  if (ubo_colorband) {
    GPU_uniformbuf_unbind(ubo_colorband);
  }
  if (ubo_texture_params) {
    GPU_uniformbuf_unbind(ubo_texture_params);
  }
  return ssbo_out;
}

void WaveManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) return;
  Vector<Impl::MeshModifierKey> keys_to_remove;
  for (const auto &item : impl_->static_map.items()) {
    if (item.key.mesh == mesh) {
      keys_to_remove.append(item.key);
    }
  }
  for (const Impl::MeshModifierKey &key : keys_to_remove) {
    impl_->static_map.remove(key);
  }
}

void WaveManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) return;
  bke::BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);
}

void WaveManager::free_all()
{
  impl_->static_map.clear();
}

} // namespace draw
} // namespace blender
