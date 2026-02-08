/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * GPU-accelerated Dynamic Paint 2 modifier implementation.
 *
 * Design:
 *   - One compute shader compiled per modifier instance (not per brush).
 *   - N dispatches per frame (one per brush), changing only uniforms:
 *     origin position, ray direction, ray length, radius, intensity,
 *     falloff type, and optionally a texture binding + falloff curve LUT.
 *   - Vertex displacement is accumulated: each brush dispatch reads
 *     from the current position buffer and writes to the output buffer,
 *     then the buffers are swapped for the next brush.
 *   - Falloff curve LUT follows draw_hook.cc pattern (1024 samples).
 *   - Texture modulation follows draw_wave.cc pattern (image-based).
 */

#include "draw_dynamicpaint2.hh"

#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_math_vector.hh"

#include "BKE_colortools.hh"
#include "BKE_image.hh"

#include "DNA_modifier_types.h"
#include "DNA_dynamicpaint2gpu_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "DEG_depsgraph_query.hh"

#include "draw_cache_extract.hh"

#include "GPU_compute.hh"

#include "draw_modifier_gpu_helpers.hh"
#include "../gpu/gpu_deform_common/gpu_shader_common_texture_lib.hh"
#include "../gpu/gpu_deform_common/gpu_shader_common_normal_lib.hh"
#include "../blenkernel/intern/mesh_gpu_cache.hh"

namespace blender::draw {

/* -------------------------------------------------------------------- */
/** \name Internal data
 * \{ */

struct DynamicPaint2GpuManager::Impl {
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

  /* Per-brush cached data uploaded to GPU once (reused every frame). */
  struct BrushStaticData {
    std::vector<float> falloff_curve_lut; /* 1024 samples for curve falloff */
  };

  struct MeshStaticData {
    int verts_num = 0;
    Object *deformed = nullptr;
    uint32_t last_verified_hash = 0;

    /* One entry per brush (indexed by position in ListBase). */
    std::vector<BrushStaticData> brush_data;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compute shader source
 * \{ */

static std::string get_dp2gpu_compute_src(bool image_only = false)
{
  using namespace gpu;
  const std::string common = image_only ? get_common_texture_image_lib_glsl()
                                        : get_common_texture_lib_glsl();
  const std::string normal_lib = get_common_normal_lib_glsl();

  const std::string body = R"GLSL(

/* Falloff type constants (match eDynPaint2Gpu_Falloff) */
#define FALLOFF_NONE      0
#define FALLOFF_CURVE     1
#define FALLOFF_SHARP     2
#define FALLOFF_SMOOTH    3
#define FALLOFF_ROOT      4
#define FALLOFF_LINEAR    5
#define FALLOFF_CONST     6
#define FALLOFF_SPHERE    7
#define FALLOFF_INVSQUARE 8

/* Evaluate falloff curve from precomputed LUT with linear interpolation */
float eval_curve_falloff(float t) {
  if (falloff_curve_lut.length() == 0) {
    return t;
  }
  t = clamp(t, 0.0, 1.0);
  int lut_size = int(falloff_curve_lut.length());
  float idx_f = t * float(lut_size - 1);
  int idx0 = int(floor(idx_f));
  int idx1 = min(idx0 + 1, lut_size - 1);
  float frac = idx_f - float(idx0);
  return mix(falloff_curve_lut[idx0], falloff_curve_lut[idx1], frac);
}

/* Compute falloff factor from normalized distance [0..1] */
float compute_falloff(float norm_dist) {
  /* norm_dist: 0 = on ray, 1 = at falloff_radius edge */
  float fac = 1.0 - norm_dist;

  switch (u_falloff_type) {
    case FALLOFF_NONE:
      return u_intensity;
    case FALLOFF_CURVE:
      fac = eval_curve_falloff(fac);
      break;
    case FALLOFF_SHARP:
      fac = fac * fac;
      break;
    case FALLOFF_SMOOTH:
      fac = 3.0 * fac * fac - 2.0 * fac * fac * fac;
      break;
    case FALLOFF_ROOT:
      fac = sqrt(max(fac, 0.0));
      break;
    case FALLOFF_LINEAR:
      /* already linear */
      break;
    case FALLOFF_CONST:
      fac = 1.0;
      break;
    case FALLOFF_SPHERE:
      fac = sqrt(max(2.0 * fac - fac * fac, 0.0));
      break;
    case FALLOFF_INVSQUARE:
      /* Use squared distance directly for inverse-square */
      fac = 1.0 - (norm_dist * norm_dist);
      break;
  }

  return fac * u_intensity;
}

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= input_positions.length()) return;

  vec4 ip = input_positions[v];
  vec3 co = ip.xyz;

  /* Vector from brush origin to vertex */
  vec3 to_vertex = co - u_origin;

  /* Project onto ray direction to get parametric t */
  float t = dot(to_vertex, u_ray_dir);

  /* Reject vertices behind origin or beyond ray length */
  if (t < 0.0) {
    deformed_positions[v] = ip;
    return;
  }
  if (u_ray_length > 0.0 && t > u_ray_length) {
    deformed_positions[v] = ip;
    return;
  }

  /* Closest point on ray and perpendicular distance */
  vec3 closest = u_origin + u_ray_dir * t;
  float dist = length(co - closest);

  /* Reject vertices outside radius */
  if (dist > u_falloff_radius) {
    deformed_positions[v] = ip;
    return;
  }

  /* Normalized distance for falloff [0 = on ray, 1 = at edge] */
  float norm_dist = (u_falloff_radius > 0.0) ? (dist / u_falloff_radius) : 0.0;

  /* Compute intensity with falloff */
  float fac = compute_falloff(norm_dist);

#ifdef HAS_TEXTURE
  /* Modulate by texture using the common texture infrastructure */
  if (texture_coords.length() > 0 && v < texture_coords.length()) {
    TexResult_tex texres;
    float tex_int = BKE_texture_get_value(texres, texture_coords[v].xyz, input_positions[v], int(v));
    fac *= tex_int;
  }
#endif

  if (fac <= 0.0) {
    deformed_positions[v] = ip;
    return;
  }

  /* Displacement direction: ray direction or vertex normal */
  vec3 disp_dir = u_ray_dir;

  co += disp_dir * fac;
  deformed_positions[v] = vec4(co, 1.0);
}
)GLSL";

  return std::string("#define POSITION_BUFFER input_positions\n") + common + normal_lib + body;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Singleton
 * \{ */

DynamicPaint2GpuManager &DynamicPaint2GpuManager::instance()
{
  static DynamicPaint2GpuManager manager;
  return manager;
}

DynamicPaint2GpuManager::DynamicPaint2GpuManager() : impl_(std::make_unique<Impl>()) {}
DynamicPaint2GpuManager::~DynamicPaint2GpuManager() = default;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hash
 * \{ */

uint32_t DynamicPaint2GpuManager::compute_dp2gpu_hash(
    const Mesh *mesh_orig, const DynamicPaint2GpuModifierData *pmd)
{
  if (!mesh_orig || !pmd) {
    return 0;
  }

  uint32_t hash = 0;
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);

  int brush_index = 0;
  for (const DynamicPaint2GpuBrushSettings *brush =
           static_cast<const DynamicPaint2GpuBrushSettings *>(pmd->brushes.first);
       brush;
       brush = brush->next)
  {
    hash = BLI_hash_int_2d(hash, uint32_t(brush_index));
    hash = BLI_hash_int_2d(hash, uint32_t(brush->direction_mode));
    hash = BLI_hash_int_2d(hash, uint32_t(brush->use_vertex_normals));
    hash = BLI_hash_int_2d(hash, uint32_t(brush->falloff_type));
    hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(brush->origin)));
    hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(brush->target)));
    hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(brush->mask_texture)));
    if (brush->curfalloff) {
      hash = BLI_hash_int_2d(hash, brush->curfalloff->changed_timestamp);
    }
    brush_index++;
  }

  hash = BLI_hash_int_2d(hash, uint32_t(brush_index));
  return hash;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ensure_static_resources
 * \{ */

void DynamicPaint2GpuManager::ensure_static_resources(
    const DynamicPaint2GpuModifierData *pmd,
    Object *deform_ob,
    Mesh *orig_mesh,
    uint32_t pipeline_hash)
{
  if (!orig_mesh || !pmd) {
    return;
  }

  Impl::MeshModifierKey key{orig_mesh, uint32_t(pmd->modifier.persistent_uid)};
  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(key);

  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);

  if (!first_time && !hash_changed) {
    return;
  }

  msd.last_verified_hash = pipeline_hash;
  msd.verts_num = orig_mesh->verts_num;
  msd.deformed = deform_ob;

  /* Build per-brush static data (falloff curve LUTs) */
  msd.brush_data.clear();
  for (const DynamicPaint2GpuBrushSettings *brush =
           static_cast<const DynamicPaint2GpuBrushSettings *>(pmd->brushes.first);
       brush;
       brush = brush->next)
  {
    Impl::BrushStaticData bsd;

    /* Build falloff curve LUT if using curve falloff */
    if (brush->falloff_type == DP2GPU_FALLOFF_CURVE && brush->curfalloff) {
      BKE_curvemapping_init(brush->curfalloff);
      constexpr int LUT_SIZE = 1024;
      bsd.falloff_curve_lut.resize(LUT_SIZE);
      for (int i = 0; i < LUT_SIZE; i++) {
        float t = float(i) / float(LUT_SIZE - 1);
        bsd.falloff_curve_lut[i] = BKE_curvemapping_evaluateF(brush->curfalloff, 0, t);
      }
    }

    msd.brush_data.push_back(std::move(bsd));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name dispatch_deform
 * \{ */

gpu::StorageBuf *DynamicPaint2GpuManager::dispatch_deform(
    const DynamicPaint2GpuModifierData *pmd,
    Depsgraph *depsgraph,
    Object *deformed_eval,
    MeshBatchCache *cache,
    gpu::StorageBuf *ssbo_in)
{
  if (!pmd || !ssbo_in || !cache) {
    return nullptr;
  }

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return nullptr;
  }

  Impl::MeshModifierKey key{mesh_owner, uint32_t(pmd->modifier.persistent_uid)};
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(key);
  if (!msd_ptr) {
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  if (BLI_listbase_is_empty(&pmd->brushes)) {
    return ssbo_in;
  }

  /* --- Allocate output SSBO --- */
  const std::string key_prefix = "dp2gpu_" + std::to_string(key.hash()) + "_";
  const std::string key_out = key_prefix + "output";
  const size_t size_out = msd.verts_num * sizeof(float) * 4;

  gpu::StorageBuf *ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_out);
  if (!ssbo_out) {
    ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_out, size_out);
    if (!ssbo_out) {
      return nullptr;
    }
  }

  /* --- Scene time for texture params --- */
  Scene *scene = DEG_get_evaluated_scene(depsgraph);
  int scene_frame = scene ? int(scene->r.cfra) : 0;

  /* --- Determine if any brush uses a texture --- */
  const bool any_has_texture = [&]() {
    for (const DynamicPaint2GpuBrushSettings *b =
             static_cast<const DynamicPaint2GpuBrushSettings *>(pmd->brushes.first);
         b;
         b = b->next)
    {
      if (b->mask_texture) {
        return true;
      }
    }
    return false;
  }();

  bool image_only_compile = false;
  if (any_has_texture) {
    /* Check if ALL textures are image-only */
    image_only_compile = true;
    for (const DynamicPaint2GpuBrushSettings *b =
             static_cast<const DynamicPaint2GpuBrushSettings *>(pmd->brushes.first);
         b;
         b = b->next)
    {
      if (b->mask_texture && b->mask_texture->type != TEX_IMAGE) {
        image_only_compile = false;
        break;
      }
    }
  }

  /* --- Noise tables (shared helpers for procedural textures) --- */
  const std::string key_hash = key_prefix + "hash_perm";
  const std::string key_hashvect = key_prefix + "hash_vectf";
  const std::string key_hashpnt = key_prefix + "hash_pntf3";

  gpu::Texture *tex_hash = gpu::get_noise_hash_texture(
      mesh_owner, deformed_eval, key_hash);
  gpu::Texture *tex_hashvect = gpu::get_noise_hashvect_texture(
      mesh_owner, deformed_eval, key_hashvect);
  gpu::Texture *tex_hashpnt = gpu::get_noise_hashpnt_texture(
      mesh_owner, deformed_eval, key_hashpnt);

  /* --- Mesh GPU topology data --- */
  Mesh *mesh_eval = id_cast<Mesh *>(deformed_eval->data);
  bke::MeshGpuData *mesh_gpu_data = bke::BKE_mesh_gpu_ensure_data(mesh_owner, mesh_eval);

  /* --- Compile shader (once) --- */
  const std::string shader_key = std::string("dp2gpu_compute_v3") +
                                 (image_only_compile ? "_image" : "_full") +
                                 (any_has_texture ? "_tex" : "");
  gpu::Shader *shader = bke::BKE_mesh_gpu_internal_shader_get(mesh_owner, shader_key);
  if (!shader) {
    using namespace gpu::shader;
    ShaderCreateInfo info("pyGPU_Shader");
    info.local_group_size(256, 1, 1);

    std::string src;
    if (any_has_texture) {
      src += "#define HAS_TEXTURE\n";
    }
    src += get_dp2gpu_compute_src(image_only_compile);

    std::string glsl_accessors = bke::BKE_mesh_gpu_topology_glsl_accessors_string(
        mesh_gpu_data->topology);

    info.typedef_source_generated = gpu::get_texture_typedefs_glsl();
    info.compute_source_generated = gpu::get_texture_params_glsl() + glsl_accessors + src;

    info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
    info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
    info.storage_buf(2, Qualifier::read, "float", "falloff_curve_lut[]");

    if (any_has_texture) {
      info.storage_buf(3, Qualifier::read, "vec4", "texture_coords[]");
    }

    /* Texture samplers (same slots as draw_wave) */
    info.sampler(0, ImageType::Float2D, "displacement_texture");
    info.sampler(1, ImageType::Float1D, "u_hash_buf");
    info.sampler(2, ImageType::Float1D, "u_hashvectf_buf");
    info.sampler(3, ImageType::Float1D, "u_hashpntf3_buf");

    /* Topology SSBO (binding 15) required by normal helpers */
    info.storage_buf(15, Qualifier::read, "int", "topo[]");

    /* ColorBand UBO (binding 4) */
    info.uniform_buf(4, "ColorBand", "tex_colorband");
    /* TextureParams UBO (binding 5) */
    info.uniform_buf(5, "TextureParams", "tex_params");

    info.push_constant(Type::float3_t, "u_origin");
    info.push_constant(Type::float3_t, "u_ray_dir");
    info.push_constant(Type::float_t, "u_ray_length");
    info.push_constant(Type::float_t, "u_radius");
    info.push_constant(Type::float_t, "u_intensity");
    info.push_constant(Type::float_t, "u_falloff_radius");
    info.push_constant(Type::int_t, "u_falloff_type");

    bke::BKE_mesh_gpu_topology_add_specialization_constants(info, mesh_gpu_data->topology);

    shader = bke::BKE_mesh_gpu_internal_shader_ensure(
        mesh_owner, deformed_eval, shader_key, info);
  }
  if (!shader) {
    return nullptr;
  }

  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;

  gpu::StorageBuf *current_in = ssbo_in;
  gpu::StorageBuf *current_out = ssbo_out;

  const gpu::shader::SpecializationConstants *constants =
      &GPU_shader_get_default_constant_state(shader);

  /* --- Per-brush dispatch loop --- */
  int brush_idx = 0;
  for (const DynamicPaint2GpuBrushSettings *brush =
           static_cast<const DynamicPaint2GpuBrushSettings *>(pmd->brushes.first);
       brush;
       brush = brush->next, brush_idx++)
  {
    /* Compute brush origin world position.
     * Fallback: use the canvas/modifier owner object position. */
    float3 origin_pos = float3(deformed_eval->object_to_world().location());
    if (brush->origin) {
      Object *eval_origin = DEG_get_evaluated(depsgraph, brush->origin);
      if (eval_origin) {
        origin_pos = float3(eval_origin->object_to_world().location());
      }
    }

    /* Compute ray direction */
    float3 ray_dir = float3(0.0f, 0.0f, -1.0f);
    float ray_len = brush->ray_length;

    switch (brush->direction_mode) {
      case DP2GPU_DIR_X:     ray_dir = float3(1, 0, 0); break;
      case DP2GPU_DIR_NEG_X: ray_dir = float3(-1, 0, 0); break;
      case DP2GPU_DIR_Y:     ray_dir = float3(0, 1, 0); break;
      case DP2GPU_DIR_NEG_Y: ray_dir = float3(0, -1, 0); break;
      case DP2GPU_DIR_Z:     ray_dir = float3(0, 0, 1); break;
      case DP2GPU_DIR_NEG_Z: ray_dir = float3(0, 0, -1); break;
      case DP2GPU_DIR_ORIGIN_TO_TARGET: {
        /* Compute target position.
         * Fallback when no target is set: origin_pos + (0, 0, -10). */
        float3 target_pos = origin_pos + float3(0.0f, 0.0f, -10.0f);
        if (brush->target) {
          Object *eval_target = DEG_get_evaluated(depsgraph, brush->target);
          if (eval_target) {
            target_pos = float3(eval_target->object_to_world().location());
          }
        }
        float3 delta = target_pos - origin_pos;
        float dist = math::length(delta);
        if (dist > 1e-6f) {
          ray_dir = delta / dist;
          if (ray_len <= 0.0f) {
            ray_len = dist;
          }
        }
        break;
      }
      case DP2GPU_DIR_ORIGIN_FORWARD: {
        if (brush->origin) {
          Object *eval_origin = DEG_get_evaluated(depsgraph, brush->origin);
          if (eval_origin) {
            float4x4 mat = eval_origin->object_to_world();
            ray_dir = math::normalize(float3(mat[1][0], mat[1][1], mat[1][2]));
          }
        }
        break;
      }
    }
    ray_dir = math::normalize(ray_dir);

    /* Falloff radius: 0 means use brush radius */
    float falloff_radius = brush->falloff > 0.0f ? brush->falloff : brush->radius;

    /* Upload falloff curve LUT SSBO for this brush */
    const std::string key_curve = key_prefix + "curve_" + std::to_string(brush_idx);
    gpu::StorageBuf *ssbo_curve = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_curve);

    const bool has_curve = (brush_idx < int(msd.brush_data.size()) &&
                            !msd.brush_data[brush_idx].falloff_curve_lut.empty());
    if (has_curve) {
      if (!ssbo_curve) {
        const auto &lut = msd.brush_data[brush_idx].falloff_curve_lut;
        const size_t size_curve = lut.size() * sizeof(float);
        ssbo_curve = bke::BKE_mesh_gpu_internal_ssbo_ensure(
            mesh_owner, deformed_eval, key_curve, size_curve);
        if (ssbo_curve) {
          GPU_storagebuf_update(ssbo_curve, lut.data());
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

    /* Bind and dispatch */
    GPU_shader_bind(shader, constants);

    GPU_storagebuf_bind(current_out, 0);
    GPU_storagebuf_bind(current_in, 1);
    if (ssbo_curve) {
      GPU_storagebuf_bind(ssbo_curve, 2);
    }

    /* Bind noise textures */
    if (tex_hash) {
      GPU_texture_bind(tex_hash, 1);
    }
    if (tex_hashvect) {
      GPU_texture_bind(tex_hashvect, 2);
    }
    if (tex_hashpnt) {
      GPU_texture_bind(tex_hashpnt, 3);
    }

    /* Bind topology SSBO */
    GPU_storagebuf_bind(mesh_gpu_data->topology.ssbo, 15);

    /* Prepare per-brush colorband and texture params UBOs */
    const std::string key_colorband = key_prefix + "colorband_" + std::to_string(brush_idx);
    gpu::UniformBuf *ubo_colorband = modifier_gpu_helpers::ensure_colorband_ubo(
        mesh_owner, deformed_eval, key_colorband, brush->mask_texture,
        const_cast<uint32_t &>(msd.last_verified_hash));

    bool tex_is_byte = true, tex_is_float = false;
    int tex_channels = 4;
    const std::string key_tex_params = key_prefix + "texparams_" + std::to_string(brush_idx);
    gpu::UniformBuf *ubo_tex_params = modifier_gpu_helpers::ensure_texture_params_ubo(
        mesh_owner, deformed_eval, key_tex_params,
        brush->mask_texture,
        const_cast<ModifierData *>(reinterpret_cast<const ModifierData *>(pmd)),
        scene_frame, tex_is_byte, tex_is_float, tex_channels,
        false);

    if (ubo_colorband) {
      GPU_uniformbuf_bind(ubo_colorband, 4);
    }
    if (ubo_tex_params) {
      GPU_uniformbuf_bind(ubo_tex_params, 5);
    }

    GPU_shader_uniform_3fv(shader, "u_origin", origin_pos);
    GPU_shader_uniform_3fv(shader, "u_ray_dir", ray_dir);
    GPU_shader_uniform_1f(shader, "u_ray_length", ray_len);
    GPU_shader_uniform_1f(shader, "u_radius", brush->radius);
    GPU_shader_uniform_1f(shader, "u_intensity", brush->intensity);
    GPU_shader_uniform_1f(shader, "u_falloff_radius", falloff_radius);
    GPU_shader_uniform_1i(shader, "u_falloff_type", int(brush->falloff_type));

    GPU_compute_dispatch(shader, num_groups, 1, 1, constants);
    GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_TEXTURE_FETCH);
    GPU_shader_unbind();

    /* Unbind textures and UBOs */
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
    if (ubo_tex_params) {
      GPU_uniformbuf_unbind(ubo_tex_params);
    }

    std::swap(current_in, current_out);
  }

  return current_in;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resource cleanup
 * \{ */

void DynamicPaint2GpuManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  Vector<Impl::MeshModifierKey> keys_to_remove;
  for (const auto &item : impl_->static_map.items()) {
    if (item.key.mesh == mesh) {
      keys_to_remove.append(item.key);
    }
  }
  for (const Impl::MeshModifierKey &k : keys_to_remove) {
    impl_->static_map.remove(k);
  }
}

void DynamicPaint2GpuManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  bke::BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);
}

void DynamicPaint2GpuManager::free_all()
{
  impl_->static_map.clear();
}

/** \} */

}  // namespace blender::draw
