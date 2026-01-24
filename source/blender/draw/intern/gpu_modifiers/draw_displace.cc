/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
/** \file
 * \ingroup draw
 *
 * GPU-accelerated Displace modifier implementation.
 */

#include "draw_displace.hh"

#include "BLI_hash.h"

#include "BKE_deform.hh"
#include "BKE_image.hh"

#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "draw_cache_extract.hh"

#include "DEG_depsgraph_query.hh"

#include "GPU_compute.hh"
#include "GPU_texture.hh"

#include "MOD_util.hh"

#include "draw_modifier_gpu_helpers.hh"
#include "../gpu/gpu_deform_common/gpu_shader_common_normal_lib.hh"  /* Common normal calculation functions */
#include "../gpu/gpu_deform_common/gpu_shader_common_texture_lib.hh" /* Common texture sampling functions */
#include "../blenkernel/intern/mesh_gpu_cache.hh"

namespace blender {
namespace draw {

/* -------------------------------------------------------------------- */
/** \name Internal Implementation Data
 * \{ */

struct draw::DisplaceManager::Impl {
/* Composite key: (Mesh*, modifier UID) to support multiple Displace modifiers per mesh */
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
    std::vector<float> vgroup_weights;       /* per-vertex weight (0.0-1.0) */
    std::vector<float3> tex_coords; /* per-vertex texture coordinates */
    int verts_num = 0;

    Object *deformed = nullptr;

    uint32_t last_verified_hash = 0;
    /* Texture/ImBuf derived flags (cached in msd to avoid repeated ImBuf acquisition). */
    bool tex_is_byte = true;
    bool tex_is_float = false;
    int tex_channels = 4;
    /* Cached colorband hash to avoid redundant UBO updates. */
    uint32_t colorband_hash = 0;
    
    /* OPTIMIZATION: Cache texture metadata to avoid repeated ImBuf queries.
     * For animated sources (SEQUENCE/MOVIE), format stays constant across frames. */
    bool tex_metadata_cached = false;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Displace Compute Shader (GPU port of MOD_displace.cc)
 * \{ */

/* GPU Displace Compute Shader - Split into several parts to avoid 16380 char limit */


static std::string get_displace_shader_part1(bool image_only = false)
{
  using namespace gpu;
  /* Define position buffer macro before including libs */
  if (image_only) {
    return "#define POSITION_BUFFER input_positions\n" +
           gpu::get_common_texture_image_lib_glsl() + /* Image-only texture helpers */
           gpu::get_common_normal_lib_glsl();
  }

  return "#define POSITION_BUFFER input_positions\n" +
         gpu::get_common_texture_lib_glsl() +  /* ColorBand + boxsample + do_2d_mapping() */
         gpu::get_common_normal_lib_glsl();   /* Normal calculation functions */
}

/* Part 2: Main function body (texture sampling + displacement logic)
 * Note: imagewrap() is displacement-specific and remains here (uses shader uniforms) */
static std::string get_displace_shader_part2()
{
  return R"GLSL(
void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= deformed_positions.length()) {
    return;
  }

  vec4 co_in = input_positions[v];
  vec3 co = co_in.xyz;

  /* Get vertex group weight */
  float vgroup_weight = 1.0;
  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    vgroup_weight = vgroup_weights[v];
  }

  /* Early exit if weight is zero (match CPU behavior) */
  if (vgroup_weight < 1e-6) {
    deformed_positions[v] = co_in;
    return;
  }

  /* Compute delta (displacement amount) */
  float delta;
  
#ifdef HAS_TEXTURE
/* Use shared helper to perform mapping + sampling. Fills `texres` and
 * returns intensity (0..1). This relies on macros from `get_texture_params_glsl()`
 * so the `tex_params` UBO is used for mapping flags and talpha. */
TexResult_tex texres;
float tex_int = BKE_texture_get_value(texres, texture_coords[v].xyz, input_positions[v], int(v));

float s = u_strength * vgroup_weight;
delta = (tex_int - u_midlevel) * s;
#else
  /* Fixed delta (no texture) */
  delta = (1.0 - u_midlevel) * u_strength * vgroup_weight;
#endif
  
  /* Clamp delta to prevent extreme deformations */
  delta = clamp(delta, -10000.0, 10000.0);

  /* Apply displacement based on direction */
  if (u_direction == MOD_DISP_DIR_X) {
    if (u_use_global) {
      /* Global X axis */
      co += delta * vec3(u_local_mat[0][0], u_local_mat[1][0], u_local_mat[2][0]);
    } else {
      /* Local X axis */
      co.x += delta;
    }
  }
  else if (u_direction == MOD_DISP_DIR_Y) {
    if (u_use_global) {
      /* Global Y axis */
      co += delta * vec3(u_local_mat[0][1], u_local_mat[1][1], u_local_mat[2][1]);
    } else {
      /* Local Y axis */
      co.y += delta;
    }
  }
  else if (u_direction == MOD_DISP_DIR_Z) {
    if (u_use_global) {
      /* Global Z axis */
      co += delta * vec3(u_local_mat[0][2], u_local_mat[1][2], u_local_mat[2][2]);
    } else {
      /* Local Z axis */
      co.z += delta;
    }
  }
  else if (u_direction == MOD_DISP_DIR_NOR) {
    vec3 n_mesh = compute_vertex_normal_smooth(int(v));
    /* Displacement along vertex normal
     * This matches CPU behavior and is acceptable for most use cases. */
    co += delta * math_normalize(n_mesh);
  }
  else if (u_direction == MOD_DISP_DIR_CLNOR) {
    /* Displacement along custom loop normals (Simplification -> same than DISP_DIR_NOR) */
    vec3 n_mesh = compute_vertex_normal_smooth(int(v));
    co += delta * math_normalize(n_mesh);
  }
  else if (u_direction == MOD_DISP_DIR_RGB_XYZ) {
    /* Displacement using RGB as (X, Y, Z) vector
     * Each RGB component controls displacement along its respective axis
     * R → X displacement, G → Y displacement, B → Z displacement */
#ifdef HAS_TEXTURE
    /* Match CPU: (tex - u_midlevel) * u_strength * weight, then optional global transform. */
    vec3 local_vec = (texres.trgba.rgb - vec3(u_midlevel)) * (u_strength * vgroup_weight);

    if (u_use_global) {
      /* mul_transposed_mat3_m4_v3 equivalent: multiply by column vectors. */
      vec3 global_disp = vec3(
        dot(local_vec, vec3(u_local_mat[0][0], u_local_mat[1][0], u_local_mat[2][0])),
        dot(local_vec, vec3(u_local_mat[0][1], u_local_mat[1][1], u_local_mat[2][1])),
        dot(local_vec, vec3(u_local_mat[0][2], u_local_mat[1][2], u_local_mat[2][2]))
      );
      co += global_disp;
    }
    else {
      co += local_vec;
    }
#else
    /* No texture: cannot use RGB_XYZ mode, fallback to no displacement */
    /* (This matches CPU behavior: RGB_XYZ requires texture) */
#endif
  }

  deformed_positions[v] = vec4(co, 1.0);
}
)GLSL";
}

/* Final assembly function - concatenates both parts */
/* Final assembly function - concatenates both parts */
static std::string get_displace_compute_src(bool image_only = false)
{
  return get_displace_shader_part1(image_only) + get_displace_shader_part2();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name DisplaceManager Public API
 * \{ */

DisplaceManager &DisplaceManager::instance()
{
  static DisplaceManager manager;
  return manager;
}

DisplaceManager::DisplaceManager() : impl_(new Impl()) {}
DisplaceManager::~DisplaceManager() {}

uint32_t DisplaceManager::compute_displace_hash(const Mesh *mesh_orig,
                                                const DisplaceModifierData *dmd)
{
  if (!mesh_orig || !dmd) {
    return 0;
  }

  uint32_t hash = 0;

  /* Hash vertex count */
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);

  /* Hash direction mode */
  hash = BLI_hash_int_2d(hash, int(dmd->direction));

  /* Hash space mode */
  hash = BLI_hash_int_2d(hash, int(dmd->space));

  /* Hash vertex group name (mix into existing hash) */
  if (dmd->defgrp_name[0] != '\0') {
    hash = BLI_hash_int_2d(hash, BLI_hash_string(dmd->defgrp_name));
  }

  /* Hash invert flag */
  hash = BLI_hash_int_2d(hash, int(dmd->flag & MOD_DISP_INVERT_VGROUP));

  /* Hash texture mapping mode */
  hash = BLI_hash_int_2d(hash, int(dmd->texmapping));

  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(dmd->texture)));

  if (dmd->texture) {
    hash = BLI_hash_int_2d(hash, uint32_t(dmd->texture->type));
    if (dmd->texture->ima) {
      hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(dmd->texture->ima)));
      hash = BLI_hash_int_2d(hash, uint32_t(dmd->texture->ima->source));
      hash = BLI_hash_int_2d(hash, uint32_t(dmd->texture->iuser.tile));
      hash = BLI_hash_int_2d(hash, uint32_t(dmd->texture->iuser.framenr));
      hash = BLI_hash_int_2d(hash, uint32_t(dmd->texture->imaflag));

      /* Mix Image generation flags/values (use actual values, not addresses). */
      hash = BLI_hash_int_2d(hash, uint32_t(dmd->texture->ima->alpha_mode));

      /* Hash the colorspace name string into the running hash. */
      if (dmd->texture->ima->colorspace_settings.name[0] != '\0') {
        hash = BLI_hash_int_2d(hash, BLI_hash_string(dmd->texture->ima->colorspace_settings.name));
      }
      else {
        hash = BLI_hash_int_2d(hash, 0);
      }
      ImageTile *tile = BKE_image_get_tile(dmd->texture->ima, dmd->texture->iuser.tile);
      if (tile) {
        /* Tile generation color may be a small array/value; mix the numeric
         * flags/types/depth which indicate tile changes. */
        hash = BLI_hash_int_2d(hash, uint32_t(tile->gen_flag));
        hash = BLI_hash_int_2d(hash, uint32_t(tile->gen_type));
        hash = BLI_hash_int_2d(hash, uint32_t(tile->gen_depth));
      }
    }
  }

  /* Hash deform_verts pointer (detects vertex group changes) */
  Span<MDeformVert> dverts = mesh_orig->deform_verts();
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(dverts.data())));

  /* Note: strength and midlevel are runtime uniforms, not hashed */

  return hash;
}

void DisplaceManager::ensure_static_resources(const DisplaceModifierData *dmd,
                                              Object *deform_ob,
                                              Mesh *orig_mesh,
                                              uint32_t pipeline_hash)
{
  if (!orig_mesh || !dmd) {
    return;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple Displace modifiers per mesh */
  Impl::MeshModifierKey key{orig_mesh, uint32_t(dmd->modifier.persistent_uid)};
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
  if (dmd->defgrp_name[0] != '\0') {
    const int defgrp_index = BKE_id_defgroup_name_index(&orig_mesh->id, dmd->defgrp_name);
    if (defgrp_index != -1) {
      Span<MDeformVert> dverts = orig_mesh->deform_verts();

      /* Check if dverts is empty to prevent crash
       * When ALL vertex groups are deleted, dverts.data() == nullptr.
       * Accessing dverts[v] would crash with Access Violation. */
      if (!dverts.is_empty()) {
        msd.vgroup_weights.resize(orig_mesh->verts_num, 0.0f);
        const bool invert_vgroup = (dmd->flag & MOD_DISP_INVERT_VGROUP) != 0;

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
  if (dmd->texture) {
    /* Use the same MOD_get_texture_coords() function as the CPU modifier
     * to guarantee identical behavior for all mapping modes (LOCAL/GLOBAL/OBJECT/UV) */
    const int verts_num = orig_mesh->verts_num;
    float(*tex_co)[3] = MEM_malloc_arrayN<float[3]>(verts_num, "displace_tex_coords");

    MOD_get_texture_coords(
        reinterpret_cast<MappingInfoModifierData *>(const_cast<DisplaceModifierData *>(dmd)),
        nullptr,  // ctx (not needed for coordinate calculation)
        deform_ob,
        orig_mesh,
        nullptr,  // cos (use original positions)
        tex_co);

    /* Copy to msd.tex_coords vector */
    msd.tex_coords.resize(verts_num);
    for (int v = 0; v < verts_num; ++v) {
      msd.tex_coords[v] = float3(tex_co[v]);
    }

    MEM_freeN(tex_co);
  }
}

gpu::StorageBuf *DisplaceManager::dispatch_deform(const DisplaceModifierData *dmd,
                                                           Depsgraph *depsgraph,
                                                           Object *deformed_eval,
                                                           MeshBatchCache *cache,
                                                           gpu::StorageBuf *ssbo_in)
{
  if (!dmd || !ssbo_in) {
    return nullptr;
  }

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return nullptr;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple Displace modifiers per mesh */
  Impl::MeshModifierKey key{mesh_owner, uint32_t(dmd->modifier.persistent_uid)};
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(key);
  if (!msd_ptr) {
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  /* Current scene frame for animated RNG/textures. Use evaluated scene to
   * match CPU evaluator frame calculation. */
  Scene *scene = DEG_get_evaluated_scene(depsgraph);
  int scene_frame = 0;
  if (scene) {
    scene_frame = int(scene->r.cfra);
  }

  /* Create unique buffer keys per modifier instance using composite key hash */
  const std::string key_prefix = "displace_" + std::to_string(key.hash()) + "_";
  const std::string key_vgroup = key_prefix + "vgroup_weights";
  const std::string key_out = key_prefix + "output";

  /* Ensure vgroup SSBO using helper (get -> ensure + upload when created). */
  gpu::StorageBuf *ssbo_vgroup = blender::draw::modifier_gpu_helpers::ensure_vgroup_ssbo(
      mesh_owner, deformed_eval, key_vgroup, msd.vgroup_weights, msd.verts_num);

  /* Upload texture coordinates SSBO and prepare texture binding */
  const std::string key_texcoords = key_prefix + "tex_coords";
  gpu::StorageBuf *ssbo_texcoords = nullptr;
  gpu::Texture *gpu_texture = nullptr;

  if (dmd->texture) {
    const bool create_dummy = (dmd->texture->type != TEX_IMAGE);
    const bool is_uv_mapping = (dmd->texmapping == MOD_DISP_MAP_UV);
    gpu_texture = blender::draw::modifier_gpu_helpers::prepare_gpu_texture_and_texcoords(
        mesh_owner,
        deformed_eval,
        depsgraph,
        dmd->texture,
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

  /* Shader-level flag: indicates a Tex is present (image or procedural).
   * Separate from `has_texture` which historically meant image+coords.
   * This controls which shader code paths are compiled and which push
   * constants are emitted. */
  bool shader_has_texture = (dmd->texture != nullptr);

  /* Upload ColorBand UBO if texture has colorband enabled (TEX_COLORBAND flag) */
  const std::string key_colorband = key_prefix + "colorband";
  gpu::UniformBuf *ubo_colorband = nullptr;
  /* Pass msd.colorband_hash so helper updates the cached hash when uploading. */
  ubo_colorband = blender::draw::modifier_gpu_helpers::ensure_colorband_ubo(
      mesh_owner, deformed_eval, key_colorband, dmd->texture, msd.colorband_hash);

  std::string key_hash = key_prefix + "hash_perm";
  std::string key_hashvect = key_prefix + "hash_vectf";
  std::string key_hashpnt = key_prefix + "hash_pntf3";

  /* Use shared helpers to create or retrieve cached noise textures (perm, gradients, points).
   * These functions wrap creation/upload and lifetime management via the mesh internal texture cache. */
  gpu::Texture *tex_hash = blender::gpu::get_noise_hash_texture(mesh_owner, deformed_eval, key_hash);
  gpu::Texture *tex_hashvect = blender::gpu::get_noise_hashvect_texture(mesh_owner, deformed_eval, key_hashvect);
  gpu::Texture *tex_hashpnt = blender::gpu::get_noise_hashpnt_texture(mesh_owner, deformed_eval, key_hashpnt);

  /* Create/Update TextureParams UBO (use helper) */
  const std::string key_tex_params = key_prefix + "texture_params";
  gpu::UniformBuf *ubo_texture_params = blender::draw::modifier_gpu_helpers::ensure_texture_params_ubo(
      mesh_owner,
      deformed_eval,
      key_tex_params,
      dmd->texture,
      (ModifierData *)dmd,
      scene_frame,
      msd.tex_is_byte,
      msd.tex_is_float,
      msd.tex_channels,
      !msd.tex_coords.empty());
  /* Create output SSBO (use get -> ensure pattern to avoid unnecessary allocations). */
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  gpu::StorageBuf *ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_out);
  if (!ssbo_out) {
    ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, deformed_eval, key_out, size_out);
    if (!ssbo_out) {
      return nullptr;
    }
  }

  /* Compute transformation matrix (for global space) */
  float local_mat[4][4];
  const bool use_global = (dmd->space == MOD_DISP_SPACE_GLOBAL);
  if (use_global) {
    copy_m4_m4(local_mat, deformed_eval->object_to_world().ptr());
  }
  else {
    unit_m4(local_mat);
  }

  /* Create shader */
  Mesh *deformed_mesh = id_cast<Mesh *>(deformed_eval->data);
  bke::MeshGpuData *mesh_gpu_data = bke::BKE_mesh_gpu_ensure_data(mesh_owner, deformed_mesh);
  /* Decide whether to compile an image-only shader variant (skip procedural noise code).
   * This reduces compile time when the texture is a simple image. Use distinct shader keys
   * to cache both variants separately. */
  bool image_only_compile = false;
  if (dmd->texture) {
    image_only_compile = (dmd->texture->type == TEX_IMAGE);
  }

  const std::string shader_key = std::string("displace_compute_v2") + (image_only_compile ? "_image" : "_full");
  gpu::Shader *shader = bke::BKE_mesh_gpu_internal_shader_get(mesh_owner, shader_key);
  if (!shader) {
    using namespace gpu::shader;
    ShaderCreateInfo info("pyGPU_Shader");
    info.local_group_size(256, 1, 1);

    /* Build shader source with conditional texture support */
    std::string shader_src;
    if (shader_has_texture) {
      shader_src += "#define HAS_TEXTURE\n";
    }
    shader_src += get_displace_compute_src(image_only_compile);
    std::string glsl_accessors = bke::BKE_mesh_gpu_topology_glsl_accessors_string(
        mesh_gpu_data->topology);

    /* Use shared typedefs (ColorBand + TextureParams) from common lib. */
    info.typedef_source_generated = gpu::get_texture_typedefs_glsl();
    /* Ensure texture params macros are available before any included GLSL
     * accessors (they may reference legacy `u_tex_*` identifiers). Prepend
     * `get_texture_params_glsl()` so macros map to the `tex_params` UBO
     * before other generated sources are concatenated. */
    info.compute_source_generated = gpu::get_texture_params_glsl() + glsl_accessors + shader_src;

    /* Bindings */
    info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
    info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
    info.storage_buf(2, Qualifier::read, "float", "vgroup_weights[]");
    if (shader_has_texture) {
      info.storage_buf(3, Qualifier::read, "vec4", "texture_coords[]");
      info.sampler(0, ImageType::Float2D, "displacement_texture");
      /* Noise/gradient permutation buffers used by GLSL noise helpers. */
      info.sampler(1, ImageType::Float1D, "u_hash_buf");
      info.sampler(2, ImageType::Float1D, "u_hashvectf_buf");
      info.sampler(3, ImageType::Float1D, "u_hashpntf3_buf");
    }
    /* ColorBand UBO (binding 4) - added for TEX_COLORBAND support */
    info.uniform_buf(4, "ColorBand", "tex_colorband");
    /* TextureParams UBO (binding 5) - contains packed texture parameters */
    info.uniform_buf(5, "TextureParams", "tex_params");
    /* Topology SSBO (binding 4) - parser automatically generates declaration before typedef */
    info.storage_buf(15, Qualifier::read, "int", "topo[]");

    /* Push constants */
    info.push_constant(Type::float4x4_t, "u_local_mat");
    info.push_constant(Type::float_t, "u_strength");
    info.push_constant(Type::float_t, "u_midlevel");
    info.push_constant(Type::int_t, "u_direction");
    info.push_constant(Type::bool_t, "u_use_global");
    /* ColorBand enable flag moved into TextureParams UBO (tex_misc2.z).
     * Avoid using a push-constant for this boolean to reduce push-constant usage. */

    /* Texture processing parameters are mostly packed in the `TextureParams` UBO.
     * Only a minimal set of runtime push constants are declared above; detailed
     * mapping and sampling flags live inside the UBO so shaders can access them
     * via `tex_params`. This avoids large push-constant usage and keeps shaders
     * compatible with legacy `u_tex_*` identifiers through macros in the
     * common texture library. */
    bke::BKE_mesh_gpu_topology_add_specialization_constants(info, mesh_gpu_data->topology);

    shader = bke::BKE_mesh_gpu_internal_shader_ensure(mesh_owner, deformed_eval, shader_key, info);
  }
  if (!shader) {
    return nullptr;
  }

  /* Bind and dispatch */
  const gpu::shader::SpecializationConstants *constants =
      &GPU_shader_get_default_constant_state(shader);
  GPU_shader_bind(shader, constants);

  GPU_storagebuf_bind(ssbo_out, 0);
  GPU_storagebuf_bind(ssbo_in, 1);
  if (ssbo_vgroup) {
    GPU_storagebuf_bind(ssbo_vgroup, 2);
  }

  /* Note: vertex normals SSBO removed — shader computes vertex normal from topology. */

  /* Bind texture coordinates and texture (if present) */
  if (shader_has_texture) {
    if (ssbo_texcoords) {
      GPU_storagebuf_bind(ssbo_texcoords, 3);
    }
    if (gpu_texture) {
      GPU_texture_bind(gpu_texture, 0);
    }
    /* Bind shared noise textures (if available) to matching units. */
    if (tex_hash) {
      GPU_texture_bind(tex_hash, 1);
    }
    if (tex_hashvect) {
      GPU_texture_bind(tex_hashvect, 2);
    }
    if (tex_hashpnt) {
      GPU_texture_bind(tex_hashpnt, 3);
    }
  }
  GPU_storagebuf_bind(mesh_gpu_data->topology.ssbo, 15);

  /* Bind ColorBand UBO (binding 4) */
  if (ubo_colorband) {
    GPU_uniformbuf_bind(ubo_colorband, 4);
  }
  /* Bind TextureParams UBO (binding 5) */
  if (ubo_texture_params) {
    GPU_uniformbuf_bind(ubo_texture_params, 5);
  }

  /* Set uniforms (runtime parameters) */
  GPU_shader_uniform_mat4(shader, "u_local_mat", (const float(*)[4])local_mat);
  GPU_shader_uniform_1f(shader, "u_strength", dmd->strength);
  GPU_shader_uniform_1f(shader, "u_midlevel", dmd->midlevel);
  GPU_shader_uniform_1i(shader, "u_direction", int(dmd->direction));
  GPU_shader_uniform_1b(shader, "u_use_global", use_global);

  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1, constants);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_TEXTURE_FETCH);
  GPU_shader_unbind();

  /* Unbind texture */
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

void DisplaceManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  /* Remove all entries for this mesh (may be multiple Displace modifiers) */
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

void DisplaceManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  /* Free all GPU resources (SSBOs + shaders) for this mesh */
  bke::BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);
}

void DisplaceManager::free_all()
{
  impl_->static_map.clear();
}

/** \} */

}  // namespace draw
}  // namespace blender
