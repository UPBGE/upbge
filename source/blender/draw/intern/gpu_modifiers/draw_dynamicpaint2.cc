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
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_string.h"

#include "BKE_collection.hh"
#include "BKE_colortools.hh"
#include "BKE_image.hh"
#include "BKE_modifier.hh"

#include "DNA_collection_types.h"

#include "DNA_modifier_types.h"
#include "DNA_dynamicpaint2gpu_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "DEG_depsgraph_query.hh"

#include "draw_cache_extract.hh"

#include "GPU_compute.hh"

#include "draw_modifier_gpu_helpers.hh"
#include "MOD_util.hh"
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
    std::vector<float3> tex_coords;      /* per-vertex texture coordinates */
    bool tex_is_byte = true;
    bool tex_is_float = false;
    int tex_channels = 4;
    bool tex_metadata_cached = false;
    uint32_t colorband_hash = 0;
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
/** \name Brush collection helper
 * \{ */

/**
 * Collect all brush settings pointers that a canvas modifier should process.
 * This includes local brushes on the modifier itself (typically empty on a
 * canvas) plus the first brush from each object in brush_collection —
 * following the original Dynamic Paint convention of one brush per object.
 *
 * The BrushEntry pairs each brush with its owner object so that the
 * dispatch loop can use the owner's transform as the default ray origin.
 * For functions that don't need the owner (hash, static resources), the
 * owner_ob field can be ignored.
 */
struct BrushEntry {
  const DynamicPaint2GpuBrushSettings *brush;
  Object *owner_ob;
};

static Vector<BrushEntry> collect_all_brushes(
    const DynamicPaint2GpuModifierData *pmd,
    Object *canvas_ob)
{
  Vector<BrushEntry> all_brushes;

  /* Local brushes on this modifier (typically non-empty on brush objects,
   * typically empty on canvas objects). */
  for (const DynamicPaint2GpuBrushSettings *brush =
           static_cast<const DynamicPaint2GpuBrushSettings *>(pmd->brushes.first);
       brush;
       brush = brush->next)
  {
    all_brushes.append({brush, canvas_ob});
  }

  /* Canvas mode: collect one brush per object in brush_collection. */
  if (pmd->type == MOD_DYNAMICPAINT2GPU_TYPE_CANVAS && pmd->brush_collection) {
    Collection *coll = pmd->brush_collection;
    FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (coll, brush_ob) {
      if (brush_ob == canvas_ob) {
        continue;
      }
      ModifierData *brush_md = BKE_modifiers_findby_type(
          brush_ob, eModifierType_DynamicPaint2Gpu);
      if (!brush_md || !(brush_md->mode & eModifierMode_Realtime)) {
        continue;
      }
      DynamicPaint2GpuModifierData *brush_pmd =
          reinterpret_cast<DynamicPaint2GpuModifierData *>(brush_md);
      if (brush_pmd->type != MOD_DYNAMICPAINT2GPU_TYPE_BRUSH) {
        continue;
      }
      const DynamicPaint2GpuBrushSettings *first_brush =
          static_cast<const DynamicPaint2GpuBrushSettings *>(brush_pmd->brushes.first);
      if (first_brush) {
        all_brushes.append({first_brush, brush_ob});
      }
    }
    FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
  }

  return all_brushes;
}

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

  /* Collect all brushes (local + from brush_collection). */
  Vector<BrushEntry> all_brushes = collect_all_brushes(pmd, nullptr);

  uint32_t hash = 0;
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);

  int brush_index = 0;
  for (const BrushEntry &be : all_brushes) {
    const DynamicPaint2GpuBrushSettings *brush = be.brush;
    hash = BLI_hash_int_2d(hash, uint32_t(brush_index));
    hash = BLI_hash_int_2d(hash, uint32_t(brush->direction_mode));
    hash = BLI_hash_int_2d(hash, uint32_t(brush->use_vertex_normals));
    hash = BLI_hash_int_2d(hash, uint32_t(brush->falloff_type));
    hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(brush->origin)));
    hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(brush->target)));
    hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(brush->mask_texture)));
    hash = BLI_hash_int_2d(hash, uint32_t(brush->texmapping));
    hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(brush->map_object)));
    if (brush->curfalloff) {
      hash = BLI_hash_int_2d(hash, brush->curfalloff->changed_timestamp);
    }
    /* Also hash the owner object pointer — different brush objects produce
     * different origins even if brush settings are identical. */
    hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(be.owner_ob)));
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

  /* Collect all brushes (local + from brush_collection). */
  Vector<BrushEntry> all_brushes = collect_all_brushes(pmd, deform_ob);

  /* Build per-brush static data (falloff curve LUTs, texture coordinates). */
  msd.brush_data.clear();
  for (const BrushEntry &be : all_brushes) {
    const DynamicPaint2GpuBrushSettings *brush = be.brush;
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

    /* Extract texture coordinates for this brush (if it has a mask texture) */
    bsd.tex_coords.clear();
    bsd.tex_metadata_cached = false;
    if (brush->mask_texture) {
      const int verts_num = orig_mesh->verts_num;
      float(*tex_co)[3] = MEM_new_array_uninitialized<float[3]>(verts_num, "dp2gpu_tex_coords");

      MappingInfoModifierData mapping_info = {};
      mapping_info.texture = const_cast<Tex *>(brush->mask_texture);
      mapping_info.map_object = const_cast<Object *>(brush->map_object);
      mapping_info.texmapping = brush->texmapping;
      if (brush->uvlayer_name[0] != '\0') {
        BLI_strncpy(mapping_info.uvlayer_name, brush->uvlayer_name, sizeof(mapping_info.uvlayer_name));
      }

      MOD_get_texture_coords(&mapping_info,
                             nullptr,
                             deform_ob,
                             orig_mesh,
                             nullptr,
                             tex_co);

      bsd.tex_coords.resize(verts_num);
      for (int v = 0; v < verts_num; ++v) {
        bsd.tex_coords[v] = float3(tex_co[v]);
      }

      MEM_delete(tex_co);
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

  /* Collect all brushes using the shared helper. */
  Vector<BrushEntry> all_brushes = collect_all_brushes(pmd, deformed_eval);
  if (all_brushes.is_empty()) {
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

  /* Second output SSBO for ping-pong when there are 2+ brushes.
   * Without this, the second brush dispatch would overwrite ssbo_in
   * (the pipeline's original input), corrupting it for later modifiers. */
  const std::string key_out2 = key_prefix + "output2";
  gpu::StorageBuf *ssbo_out2 = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_out2);
  if (!ssbo_out2 && all_brushes.size() > 1) {
    ssbo_out2 = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_out2, size_out);
  }

  /* --- Scene time for texture params --- */
  Scene *scene = DEG_get_evaluated_scene(depsgraph);
  int scene_frame = scene ? int(scene->r.cfra) : 0;

  /* --- Determine if any brush uses a texture --- */
  const bool any_has_texture = [&]() {
    for (const BrushEntry &be : all_brushes) {
      if (be.brush->mask_texture) {
        return true;
      }
    }
    return false;
  }();

  bool image_only_compile = false;
  if (any_has_texture) {
    /* Check if ALL textures are image-only */
    image_only_compile = true;
    for (const BrushEntry &be : all_brushes) {
      if (be.brush->mask_texture && be.brush->mask_texture->type != TEX_IMAGE) {
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
    info.storage_buf(3, Qualifier::read, "vec4", "texture_coords[]");

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

  /* Ping-pong between ssbo_out and ssbo_out2 so we never write to ssbo_in.
   * First brush: reads ssbo_in, writes ssbo_out.
   * Second brush: reads ssbo_out, writes ssbo_out2.
   * Third brush: reads ssbo_out2, writes ssbo_out. etc. */
  gpu::StorageBuf *current_in = ssbo_in;
  gpu::StorageBuf *current_out = ssbo_out;
  gpu::StorageBuf *ping = ssbo_out;
  gpu::StorageBuf *pong = ssbo_out2 ? ssbo_out2 : ssbo_out;

  const gpu::shader::SpecializationConstants *constants =
      &GPU_shader_get_default_constant_state(shader);

  /* --- Per-brush dispatch loop --- */
  int brush_idx = 0;
  for (const BrushEntry &be : all_brushes)
  {
    const DynamicPaint2GpuBrushSettings *brush = be.brush;

    /* Compute brush origin world position.
     * Priority: brush->origin > brush owner object > canvas object.
     * For external brushes (from brush_collection), the owner object IS
     * the brush, so its position is the natural default origin — matching
     * how original Dynamic Paint treats each object in brush_group. */
    float3 origin_pos;
    if (brush->origin) {
      Object *eval_origin = DEG_get_evaluated(depsgraph, brush->origin);
      origin_pos = eval_origin ? float3(eval_origin->object_to_world().location())
                               : float3(be.owner_ob->object_to_world().location());
    }
    else {
      Object *eval_owner = DEG_get_evaluated(depsgraph, be.owner_ob);
      origin_pos = eval_owner ? float3(eval_owner->object_to_world().location())
                              : float3(deformed_eval->object_to_world().location());
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

    /* Transform origin and ray direction from world space into the mesh's
     * local/object space, since vertex positions in the SSBO are local. */
    {
      float4x4 world_to_local = math::invert(deformed_eval->object_to_world());
      origin_pos = math::transform_point(world_to_local, origin_pos);
      ray_dir = math::normalize(math::transform_direction(world_to_local, ray_dir));
    }

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

    /* Prepare per-brush texture coordinate SSBO + GPU texture upload */
    const std::string key_colorband = key_prefix + "colorband_" + std::to_string(brush_idx);

    /* Per-brush texture coordinate SSBO + GPU texture upload.
     * Always call prepare_gpu_texture_and_texcoords even when brush has
     * no texture — it creates a dummy 1x1 texture, matching the pattern
     * used by draw_displace.cc and draw_wave.cc. */
    gpu::StorageBuf *ssbo_texcoords = nullptr;
    gpu::Texture *brush_gpu_texture = nullptr;

    const bool brush_has_texture = (brush->mask_texture != nullptr);
    const std::string brush_key_prefix = key_prefix + "brush_" + std::to_string(brush_idx) + "_";

    {
      /* Resolve per-brush static data; for brushes without a texture we
       * pass empty coords and nullptr tex — the helper creates a dummy. */
      std::vector<float3> empty_coords;
      bool bsd_tex_is_byte = true, bsd_tex_is_float = false;
      int bsd_tex_channels = 4;
      bool bsd_tex_metadata_cached = false;

      std::vector<float3> *tex_coords_ptr = &empty_coords;
      Tex *brush_tex = nullptr;
      bool is_uv_mapping = false;

      if (brush_has_texture && brush_idx < int(msd.brush_data.size())) {
        Impl::BrushStaticData &bsd = msd.brush_data[brush_idx];
        tex_coords_ptr = &bsd.tex_coords;
        bsd_tex_is_byte = bsd.tex_is_byte;
        bsd_tex_is_float = bsd.tex_is_float;
        bsd_tex_channels = bsd.tex_channels;
        bsd_tex_metadata_cached = bsd.tex_metadata_cached;
        brush_tex = const_cast<Tex *>(brush->mask_texture);
        is_uv_mapping = (brush->texmapping == MOD_DISP_MAP_UV);
      }

      brush_gpu_texture = modifier_gpu_helpers::prepare_gpu_texture_and_texcoords(
          mesh_owner,
          deformed_eval,
          depsgraph,
          brush_tex,
          *tex_coords_ptr,
          bsd_tex_is_byte,
          bsd_tex_is_float,
          bsd_tex_channels,
          bsd_tex_metadata_cached,
          brush_key_prefix,
          &ssbo_texcoords,
          is_uv_mapping);

      /* Write back cached metadata for real brushes. */
      if (brush_has_texture && brush_idx < int(msd.brush_data.size())) {
        Impl::BrushStaticData &bsd = msd.brush_data[brush_idx];
        bsd.tex_is_byte = bsd_tex_is_byte;
        bsd.tex_is_float = bsd_tex_is_float;
        bsd.tex_channels = bsd_tex_channels;
        bsd.tex_metadata_cached = bsd_tex_metadata_cached;
      }
    }

    /* Ensure texcoords SSBO is always valid (Vulkan requires all declared
     * storage buffers to be bound). When prepare_gpu_texture_and_texcoords
     * skips the texcoords upload (empty coords), create a dummy SSBO. */
    if (!ssbo_texcoords) {
      const std::string key_dummy_tc = brush_key_prefix + "dummy_texcoords";
      ssbo_texcoords = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_dummy_tc);
      if (!ssbo_texcoords) {
        ssbo_texcoords = bke::BKE_mesh_gpu_internal_ssbo_ensure(
            mesh_owner, deformed_eval, key_dummy_tc, sizeof(float4));
        if (ssbo_texcoords) {
          float4 dummy_tc(0.0f, 0.0f, 0.0f, 1.0f);
          GPU_storagebuf_update(ssbo_texcoords, &dummy_tc);
        }
      }
    }

    /* Bind per-brush texture coords and GPU texture (unconditionally) */
    if (ssbo_texcoords) {
      GPU_storagebuf_bind(ssbo_texcoords, 3);
    }
    if (brush_gpu_texture) {
      GPU_texture_bind(brush_gpu_texture, 0);
    }

    gpu::UniformBuf *ubo_colorband = modifier_gpu_helpers::ensure_colorband_ubo(
        mesh_owner, deformed_eval, key_colorband, const_cast<Tex *>(brush->mask_texture),
        const_cast<uint32_t &>(msd.last_verified_hash));

    bool tex_is_byte = true, tex_is_float = false;
    int tex_channels = 4;
    if (brush_idx < int(msd.brush_data.size())) {
      tex_is_byte = msd.brush_data[brush_idx].tex_is_byte;
      tex_is_float = msd.brush_data[brush_idx].tex_is_float;
      tex_channels = msd.brush_data[brush_idx].tex_channels;
    }
    const std::string key_tex_params = key_prefix + "texparams_" + std::to_string(brush_idx);
    gpu::UniformBuf *ubo_tex_params = modifier_gpu_helpers::ensure_texture_params_ubo(
        mesh_owner, deformed_eval, key_tex_params,
        const_cast<Tex *>(brush->mask_texture),
        const_cast<ModifierData *>(reinterpret_cast<const ModifierData *>(pmd)),
        scene_frame, tex_is_byte, tex_is_float, tex_channels,
        brush_has_texture && brush_idx < int(msd.brush_data.size()) &&
            !msd.brush_data[brush_idx].tex_coords.empty());

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
    if (brush_gpu_texture) {
      GPU_texture_unbind(brush_gpu_texture);
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
    if (ubo_tex_params) {
      GPU_uniformbuf_unbind(ubo_tex_params);
    }

    /* Advance ping-pong: result is now in current_out. */
    current_in = current_out;
    current_out = (current_in == ping) ? pong : ping;
    brush_idx++;
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
