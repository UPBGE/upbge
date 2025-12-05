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
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_vector.hh"

#include "BKE_deform.hh"
#include "BKE_image.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_object.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_texture_types.h"

#include "../modifiers/intern/MOD_util.hh"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_texture.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

#include "DRW_render.hh"
#include "draw_cache_impl.hh"

#include "DEG_depsgraph_query.hh"

#include "MEM_guardedalloc.h"

using namespace blender::draw;

/* -------------------------------------------------------------------- */
/** \name Internal Implementation Data
 * \{ */

struct blender::draw::DisplaceManager::Impl {
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
    std::vector<float> vgroup_weights; /* per-vertex weight (0.0-1.0) */
    std::vector<blender::float3> tex_coords; /* per-vertex texture coordinates */
    int verts_num = 0;

    Object *deformed = nullptr;

    bool pending_gpu_setup = false;
    int gpu_setup_attempts = 0;
    uint32_t last_verified_hash = 0;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Displace Compute Shader (GPU port of MOD_displace.cc)
 * \{ */

static const char *displace_compute_src = R"GLSL(
/* GPU Displace Modifier Compute Shader v2.0 */
/* Displace direction modes (matching DisplaceModifierDirection enum) */
#define MOD_DISP_DIR_X 0
#define MOD_DISP_DIR_Y 1
#define MOD_DISP_DIR_Z 2
#define MOD_DISP_DIR_NOR 3
#define MOD_DISP_DIR_RGB_XYZ 4
#define MOD_DISP_DIR_CLNOR 5

/* Displace space modes (matching DisplaceModifierSpace enum) */
#define MOD_DISP_SPACE_LOCAL 0
#define MOD_DISP_SPACE_GLOBAL 1

/* Texture extend modes (matching DNA_texture_types.h) */
#define TEX_EXTEND 0
#define TEX_CLIP 1
#define TEX_REPEAT 2
#define TEX_CLIPCUBE 4
#define TEX_CHECKER 8

/* Apply texture extend mode to UV coordinates
 * Matches Blender's texture wrapping behavior */
vec2 apply_texture_extend(vec2 uv, int extend_mode) {
  if (extend_mode == TEX_REPEAT) {
    /* Repeat: fract() wraps to [0,1] */
    return fract(uv);
  }
  else if (extend_mode == TEX_EXTEND) {
    /* Extend: clamp to [0,1] edges */
    return clamp(uv, 0.0, 1.0);
  }
  else if (extend_mode == TEX_CLIP) {
    /* Clip: mark out-of-bounds for black return */
    return uv;  /* Will be checked after sampling */
  }
  else if (extend_mode == TEX_CHECKER) {
    /* Checker: alternate between texture and inverted on grid */
    vec2 check = floor(uv);
    float checker = mod(check.x + check.y, 2.0);
    return fract(uv);  /* Will invert color if checker == 1 */
  }
  else {
    /* Default: repeat */
    return fract(uv);
  }
}

/* Note: displacement_texture sampler is declared by ShaderCreateInfo, no manual uniform needed */

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
  if (vgroup_weight == 0.0) {
    deformed_positions[v] = co_in;
    return;
  }

  /* Compute delta (displacement amount) */
  float delta;
  
#ifdef HAS_TEXTURE
/* Sample texture using texture coordinates from MOD_get_texture_coords() */
vec3 tex_coord = texture_coords[v].xyz;
  
  /* FLAT mapping: remap from world/local space to [0,1] UV space
   * This matches CPU behavior in do_2d_mapping() (texture_procedural.cc) */
  vec2 uv;
  uv.x = (tex_coord.x + 1.0) * 0.5;
  uv.y = (tex_coord.y + 1.0) * 0.5;
  
  /* Apply flip axis (TEX_IMAROT): swap X and Y coordinates */
  if (tex_flip_axis) {
    float temp = uv.x;
    uv.x = uv.y;
    uv.y = temp;
  }
  
  /* Apply crop/recadrage: remap UV to cropped region
   * tex_crop = (xmin, ymin, xmax, ymax) defines the visible region
   * Example: crop = (0.25, 0.25, 0.75, 0.75) uses only center 50% of texture */
  vec2 crop_min = tex_crop.xy;
  vec2 crop_max = tex_crop.zw;
  vec2 crop_size = crop_max - crop_min;
  
  /* Remap UV from [0,1] to [crop_min, crop_max] */
  uv = crop_min + uv * crop_size;
  
  /* Apply repeat scaling (xrepeat, yrepeat)
   * Multiplies UV coordinates to repeat the texture */
  uv *= vec2(tex_repeat);
  
  /* Apply texture extend mode (repeat, clamp, clip, checker) */
  vec2 uv_wrapped = apply_texture_extend(uv, tex_extend);
  
  /* Apply mirror flags (flip on odd tiles) */
  if (tex_xmir || tex_ymir) {
    vec2 tile = floor(uv);  /* Which tile are we in? */
    if (tex_xmir && mod(tile.x, 2.0) != 0.0) {
      uv_wrapped.x = 1.0 - fract(uv.x);  /* Mirror X on odd tiles */
    }
    if (tex_ymir && mod(tile.y, 2.0) != 0.0) {
      uv_wrapped.y = 1.0 - fract(uv.y);  /* Mirror Y on odd tiles */
    }
  }
  
  /* Check if out-of-bounds for CLIP mode */
  bool is_clipped = false;
  if (tex_extend == TEX_CLIP || tex_extend == TEX_CLIPCUBE) {
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
      is_clipped = true;
    }
  }
  
  /* Sample texture with interpolation mode
   * If tex_interpol is false, use nearest neighbor (texelFetch would be ideal,
   * but we approximate with texture() since we don't have texel coords) */
  vec4 tex_color;
  if (tex_interpol) {
    /* Bilinear interpolation (standard)
     * Note: filtersize is used by CPU for other effects, not direct mipmap bias in sampling */
    tex_color = texture(displacement_texture, uv_wrapped);
  }
  else {
    /* Nearest neighbor approximation: snap to texel center */
    ivec2 tex_size = textureSize(displacement_texture, 0);
    vec2 uv_snapped = (floor(uv_wrapped * vec2(tex_size)) + 0.5) / vec2(tex_size);
    tex_color = texture(displacement_texture, uv_snapped);
  }
  
  /* Apply CLIP mode (return black if out of bounds) */
  if (is_clipped) {
    tex_color = vec4(0.0);
  }
  
  /* Apply CHECKER mode (invert color on alternating tiles) */
  if (tex_extend == TEX_CHECKER) {
    vec2 check = floor(uv);
    float checker = mod(check.x + check.y, 2.0);
    
    /* Apply checker odd/even filter */
    bool show_tile = true;
    if (tex_checker_odd && tex_checker_even) {
      show_tile = true;  /* Both enabled = show all */
    }
    else if (tex_checker_odd && !tex_checker_even) {
      show_tile = (checker > 0.5);  /* Only odd tiles */
    }
    else if (!tex_checker_odd && tex_checker_even) {
      show_tile = (checker < 0.5);  /* Only even tiles */
    }
    else {
      show_tile = false;  /* Neither enabled = show none */
    }
    
    if (!show_tile) {
      tex_color = vec4(0.0);  /* Hide this tile */
    }
    else if (checker > 0.5) {
      tex_color.rgb = vec3(1.0) - tex_color.rgb;  /* Invert odd tiles */
    }
  }
  
vec3 rgb = tex_color.rgb;
float alpha = tex_color.a;
  
  /* Step 1: De-pre-multiply alpha (texture_image.cc lines 272-279) */
  if (use_talpha && alpha != 1.0 && alpha > 1e-4) {
    float inv_alpha = 1.0 / alpha;
    rgb *= inv_alpha;
  }
  
  /* Step 2: Apply BRICONTRGB (brightness, contrast, RGB factors) */
  rgb.r = tex_rfac * ((rgb.r - 0.5) * tex_contrast + tex_bright - 0.5);
  rgb.g = tex_gfac * ((rgb.g - 0.5) * tex_contrast + tex_bright - 0.5);
  rgb.b = tex_bfac * ((rgb.b - 0.5) * tex_contrast + tex_bright - 0.5);
  
  /* Clamp negative values only if !TEX_NO_CLAMP */
  if (!tex_no_clamp) {
    rgb = max(rgb, vec3(0.0));
  }
  
  /* Step 3: Apply saturation (if needed) */
  if (tex_saturation != 1.0) {
    /* RGB to HSV */
    float cmax = max(max(rgb.r, rgb.g), rgb.b);
    float cmin = min(min(rgb.r, rgb.g), rgb.b);
    float delta_hsv = cmax - cmin;
    
    float h = 0.0, s = 0.0, v = cmax;
    
    if (delta_hsv > 1e-20) {
      s = delta_hsv / (cmax + 1e-20);
      
      if (rgb.r >= cmax) {
        h = (rgb.g - rgb.b) / delta_hsv;
      } else if (rgb.g >= cmax) {
        h = 2.0 + (rgb.b - rgb.r) / delta_hsv;
      } else {
        h = 4.0 + (rgb.r - rgb.g) / delta_hsv;
      }
      
      h /= 6.0;
      if (h < 0.0) h += 1.0;
    }
    
    /* Scale saturation */
    s *= tex_saturation;
    
    /* HSV to RGB (Blender's algorithm) */
    float nr = abs(h * 6.0 - 3.0) - 1.0;
    float ng = 2.0 - abs(h * 6.0 - 2.0);
    float nb = 2.0 - abs(h * 6.0 - 4.0);
    
    nr = clamp(nr, 0.0, 1.0);
    ng = clamp(ng, 0.0, 1.0);
    nb = clamp(nb, 0.0, 1.0);
    
    rgb.r = ((nr - 1.0) * s + 1.0) * v;
    rgb.g = ((ng - 1.0) * s + 1.0) * v;
    rgb.b = ((nb - 1.0) * s + 1.0) * v;
    
    /* Clamp again if saturation > 1.0 and !TEX_NO_CLAMP */
    if (tex_saturation > 1.0 && !tex_no_clamp) {
      rgb = max(rgb, vec3(0.0));
    }
  }
  
  /* Step 4: Convert linear → sRGB (Blender's linearrgb_to_srgb formula)
   * This is CRITICAL: GPU textures are in linear space, but CPU computes
   * displacement from sRGB values. Formula from math_color.cc line 649-654:
   *   if (c < 0.0031308f) return max(c * 12.92f, 0.0f);
   *   else return 1.055f * pow(c, 1.0f/2.4f) - 0.055f;
   */
  vec3 srgb_rgb;
  srgb_rgb.r = (rgb.r < 0.0031308) ? max(rgb.r * 12.92, 0.0) : (1.055 * pow(rgb.r, 1.0 / 2.4) - 0.055);
  srgb_rgb.g = (rgb.g < 0.0031308) ? max(rgb.g * 12.92, 0.0) : (1.055 * pow(rgb.g, 1.0 / 2.4) - 0.055);
  srgb_rgb.b = (rgb.b < 0.0031308) ? max(rgb.b * 12.92, 0.0) : (1.055 * pow(rgb.b, 1.0 / 2.4) - 0.055);
  
  /* Step 5: Compute intensity as average of sRGB values
   * This matches BKE_texture_get_value_ex() (texture.cc line 630) */
  float tex_value = (srgb_rgb.r + srgb_rgb.g + srgb_rgb.b) * (1.0 / 3.0);
  
  /* Apply TEX_FLIPBLEND: invert gradient (1.0 - value)
   * Used for reversing blend/gradient textures */
  if (tex_flipblend) {
    tex_value = 1.0 - tex_value;
  }

  float s = strength * vgroup_weight;
  
  /* For RGB_XYZ mode, we need the full RGB vector, not just intensity */
  vec3 rgb_displacement = (srgb_rgb - vec3(midlevel)) * s;
  
  delta = (tex_value - midlevel) * s;
#else
  /* Fixed delta (no texture) */
  delta = (1.0 - midlevel) * strength * vgroup_weight;
  vec3 rgb_displacement = vec3(0.0);  /* Not used without texture */
#endif
  
  /* Clamp delta to prevent extreme deformations */
  delta = clamp(delta, -10000.0, 10000.0);

  /* Apply displacement based on direction */
  if (direction == MOD_DISP_DIR_X) {
    if (use_global) {
      /* Global X axis */
      co += delta * vec3(local_mat[0][0], local_mat[1][0], local_mat[2][0]);
    } else {
      /* Local X axis */
      co.x += delta;
    }
  }
  else if (direction == MOD_DISP_DIR_Y) {
    if (use_global) {
      /* Global Y axis */
      co += delta * vec3(local_mat[0][1], local_mat[1][1], local_mat[2][1]);
    } else {
      /* Local Y axis */
      co.y += delta;
    }
  }
  else if (direction == MOD_DISP_DIR_Z) {
    if (use_global) {
      /* Global Z axis */
      co += delta * vec3(local_mat[0][2], local_mat[1][2], local_mat[2][2]);
    } else {
      /* Local Z axis */
      co.z += delta;
    }
  }
  else if (direction == MOD_DISP_DIR_NOR) {
    /* Displacement along vertex normal
     * Use pre-computed normals from cache (NOT updated during GPU playback!)
     * This matches CPU behavior and is acceptable for most use cases. */
    vec3 normal = vert_normals[v].xyz;
    co += delta * normalize(normal);
  }
  else if (direction == MOD_DISP_DIR_CLNOR) {
    /* Displacement along custom loop normals (corner normals averaged to vertices)
     * Use pre-computed normals from cache (NOT updated during GPU playback!)
     * Respects sharp edges and custom normals, unlike MOD_DISP_DIR_NOR. */
    vec3 normal = vert_normals[v].xyz;
    co += delta * normalize(normal);
  }
  else if (direction == MOD_DISP_DIR_RGB_XYZ) {
    /* Displacement using RGB as (X, Y, Z) vector
     * Each RGB component controls displacement along its respective axis
     * R → X displacement, G → Y displacement, B → Z displacement */
#ifdef HAS_TEXTURE
    if (use_global) {
      /* Transform local displacement vector to global space */
      vec3 global_disp = vec3(
        dot(vec3(local_mat[0][0], local_mat[0][1], local_mat[0][2]), rgb_displacement),
        dot(vec3(local_mat[1][0], local_mat[1][1], local_mat[1][2]), rgb_displacement),
        dot(vec3(local_mat[2][0], local_mat[2][1], local_mat[2][2]), rgb_displacement)
      );
      co += global_disp;
    } else {
      /* Local space: directly apply RGB as (X, Y, Z) */
      co += rgb_displacement;
    }
#else
    /* No texture: cannot use RGB_XYZ mode, fallback to no displacement */
    /* (This matches CPU behavior: RGB_XYZ requires texture) */
#endif
  }

  deformed_positions[v] = vec4(co, 1.0);
}
)GLSL";

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

  /* Hash vertex group name */
  if (dmd->defgrp_name[0] != '\0') {
    hash = BLI_hash_string(dmd->defgrp_name);
  }

  /* Hash invert flag */
  hash = BLI_hash_int_2d(hash, int(dmd->flag & MOD_DISP_INVERT_VGROUP));

  /* Hash texture mapping mode */
  hash = BLI_hash_int_2d(hash, int(dmd->texmapping));

  /* Hash texture pointer (if present) */
  if (dmd->texture && dmd->texture->type == TEX_IMAGE) {
    hash = BLI_hash_int_2d(hash, int(reinterpret_cast<uintptr_t>(dmd->texture)));
  }

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
  const bool gpu_invalidated = msd.pending_gpu_setup;

  if (!first_time && !hash_changed && !gpu_invalidated) {
    return;
  }

  msd.last_verified_hash = pipeline_hash;
  msd.verts_num = orig_mesh->verts_num;
  msd.deformed = deform_ob;

  if (first_time || hash_changed) {
    msd.pending_gpu_setup = true;
    msd.gpu_setup_attempts = 0;
  }

  /* Extract vertex group weights */
  msd.vgroup_weights.clear();
  if (dmd->defgrp_name[0] != '\0') {
    const int defgrp_index = BKE_id_defgroup_name_index(&orig_mesh->id, dmd->defgrp_name);
    if (defgrp_index != -1) {
      blender::Span<MDeformVert> dverts = orig_mesh->deform_verts();
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
  if (dmd->texture && dmd->texture->type == TEX_IMAGE) {
    /* Use the same MOD_get_texture_coords() function as the CPU modifier
     * to guarantee identical behavior for all mapping modes (LOCAL/GLOBAL/OBJECT/UV) */
    const int verts_num = orig_mesh->verts_num;
    float (*tex_co)[3] = MEM_malloc_arrayN<float[3]>(verts_num, "displace_tex_coords");

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
      msd.tex_coords[v] = blender::float3(tex_co[v]);
    }
    
    MEM_freeN(tex_co);
  }
}

blender::gpu::StorageBuf *DisplaceManager::dispatch_deform(const DisplaceModifierData *dmd,
                                                           Depsgraph * /*depsgraph*/,
                                                           Object *deformed_eval,
                                                           MeshBatchCache *cache,
                                                           blender::gpu::StorageBuf *ssbo_in)
{
  if (!dmd) {
    return nullptr;
  }

  using namespace blender::draw;

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

  /* GPU setup retry logic */
  const int MAX_ATTEMPTS = 3;
  if (msd.pending_gpu_setup) {
    if (msd.gpu_setup_attempts == 0) {
      msd.gpu_setup_attempts = 1;
      return nullptr;
    }
    if (msd.gpu_setup_attempts >= MAX_ATTEMPTS) {
      msd.pending_gpu_setup = false;
      msd.gpu_setup_attempts = 0;
      return nullptr;
    }
    msd.gpu_setup_attempts++;
  }

  MeshGpuInternalResources *ires = BKE_mesh_gpu_internal_resources_ensure(mesh_owner);
  if (!ires) {
    return nullptr;
  }

  /* Create unique buffer keys per modifier instance using persistent_uid */
  char uid_str[16];
  snprintf(uid_str, sizeof(uid_str), "%u", dmd->modifier.persistent_uid);
  const std::string key_prefix = std::string("displace_") + uid_str + "_";
  const std::string key_vgroup = key_prefix + "vgroup_weights";
  const std::string key_out = key_prefix + "output";

  /* Upload vertex group weights SSBO */
  blender::gpu::StorageBuf *ssbo_vgroup = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_vgroup);

  if (!msd.vgroup_weights.empty()) {
    if (!ssbo_vgroup) {
      const size_t size_vgroup = msd.vgroup_weights.size() * sizeof(float);
      ssbo_vgroup = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_vgroup, size_vgroup);
      if (ssbo_vgroup) {
        GPU_storagebuf_update(ssbo_vgroup, msd.vgroup_weights.data());
      }
    }
  }
  else {
    /* No vertex group: create dummy buffer (length=0 triggers default weight=1.0 in shader) */
    if (!ssbo_vgroup) {
      ssbo_vgroup = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_vgroup, sizeof(float));
      if (ssbo_vgroup) {
        float dummy = 1.0f;
        GPU_storagebuf_update(ssbo_vgroup, &dummy);
      }
    }
  }

  /* Upload texture coordinates SSBO and prepare texture binding */
  const std::string key_texcoords = key_prefix + "tex_coords";
  blender::gpu::StorageBuf *ssbo_texcoords = nullptr;
  blender::gpu::Texture *gpu_texture = nullptr;
  bool has_texture = false;

  if (dmd->texture && dmd->texture->type == TEX_IMAGE && dmd->texture->ima) {
    Image *ima = dmd->texture->ima;
    
    /* Check if GPU texture is loaded */
    if (ima && ima->runtime) {
      ImageUser iuser = {nullptr};

      gpu_texture = BKE_image_get_gpu_texture(ima, &iuser);
      
      if (gpu_texture && !msd.tex_coords.empty()) {
        has_texture = true;
        
        /* Upload texture coordinates SSBO */
        ssbo_texcoords = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_texcoords);
        
        if (!ssbo_texcoords) {
          const size_t size_texcoords = msd.tex_coords.size() * sizeof(blender::float4);
          ssbo_texcoords = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_texcoords, size_texcoords);
          if (ssbo_texcoords) {
            /* Pad float3 to float4 for GPU alignment */
            std::vector<blender::float4> padded_texcoords(msd.tex_coords.size());
            for (size_t i = 0; i < msd.tex_coords.size(); ++i) {
              padded_texcoords[i] = blender::float4(
                  msd.tex_coords[i].x, msd.tex_coords[i].y, msd.tex_coords[i].z, 1.0f);
            }
            GPU_storagebuf_update(ssbo_texcoords, padded_texcoords.data());
          }
        }
      }
    }
  }

  /* Create output SSBO */
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  blender::gpu::StorageBuf *ssbo_out = BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, key_out, size_out);
  if (!ssbo_out || !ssbo_in) {
    return nullptr;
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
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("pyGPU_Shader");
  info.local_group_size(256, 1, 1);
  
  /* Build shader source with conditional texture support */
  std::string shader_src;
  
  if (has_texture) {
    shader_src += "#define HAS_TEXTURE\n";
  }
  shader_src += displace_compute_src;
  
  info.compute_source_generated = shader_src;

  /* Bindings */
  info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
  info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
  info.storage_buf(2, Qualifier::read, "float", "vgroup_weights[]");
  info.storage_buf(3, Qualifier::read, "vec4", "vert_normals[]");  /* Pre-computed normals (NOT updated during GPU playback) */
  
  if (has_texture) {
    info.storage_buf(4, Qualifier::read, "vec4", "texture_coords[]");
    info.sampler(0, ImageType::Float2D, "displacement_texture");
  }

  /* Push constants */
  info.push_constant(Type::float4x4_t, "local_mat");
  info.push_constant(Type::float_t, "strength");
  info.push_constant(Type::float_t, "midlevel");
  info.push_constant(Type::int_t, "direction");
  info.push_constant(Type::bool_t, "use_global");
  
  /* Texture processing parameters (for BRICONTRGB and de-premultiply) */
  if (has_texture) {
    info.push_constant(Type::bool_t, "use_talpha");      /* Enable de-premultiply */
    info.push_constant(Type::float_t, "tex_bright");     /* Tex->bright */
    info.push_constant(Type::float_t, "tex_contrast");   /* Tex->contrast */
    info.push_constant(Type::float_t, "tex_saturation"); /* Tex->saturation */
    info.push_constant(Type::float_t, "tex_rfac");       /* Tex->rfac */
    info.push_constant(Type::float_t, "tex_gfac");       /* Tex->gfac */
    info.push_constant(Type::float_t, "tex_bfac");       /* Tex->bfac */
    info.push_constant(Type::bool_t, "tex_no_clamp");    /* Tex->flag & TEX_NO_CLAMP */
    info.push_constant(Type::int_t, "tex_extend");       /* Tex->extend (wrap mode) */
    info.push_constant(Type::float4_t, "tex_crop");      /* (cropxmin, cropymin, cropxmax, cropymax) */
    info.push_constant(Type::float2_t, "tex_repeat");    /* (xrepeat, yrepeat) */
    info.push_constant(Type::bool_t, "tex_xmir");        /* TEX_REPEAT_XMIR */
    info.push_constant(Type::bool_t, "tex_ymir");        /* TEX_REPEAT_YMIR */
    info.push_constant(Type::bool_t, "tex_interpol");    /* TEX_INTERPOL */
    info.push_constant(Type::bool_t, "tex_checker_odd"); /* TEX_CHECKER_ODD */
    info.push_constant(Type::bool_t, "tex_checker_even");/* TEX_CHECKER_EVEN */
    info.push_constant(Type::bool_t, "tex_flipblend");   /* TEX_FLIPBLEND */
    info.push_constant(Type::bool_t, "tex_flip_axis");   /* TEX_IMAROT (flip X/Y) */
  }

  blender::gpu::Shader *shader = BKE_mesh_gpu_internal_shader_ensure(
      mesh_owner, "displace_compute_v2", info);
  if (!shader) {
    return nullptr;
  }

  /* Bind and dispatch */
  GPU_shader_bind(shader);

  GPU_storagebuf_bind(ssbo_out, 0);
  GPU_storagebuf_bind(ssbo_in, 1);
  if (ssbo_vgroup) {
    GPU_storagebuf_bind(ssbo_vgroup, 2);
  }
  
  /* Bind vertex normals SSBO (binding 3) if MOD_DISP_DIR_NOR or MOD_DISP_DIR_CLNOR
   * WARNING: Normals are computed ONCE from the mesh state before GPU playback starts.
   * They are NOT updated during GPU animation playback (modifier stack is GPU-only).
   * This is acceptable for most use cases (e.g., vertex group deformation with textures). */
  if (dmd->direction == MOD_DISP_DIR_NOR || dmd->direction == MOD_DISP_DIR_CLNOR) {
    const std::string key_normals = key_prefix + "vert_normals";
    blender::gpu::StorageBuf *ssbo_normals = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_normals);
    
    if (!ssbo_normals) {
      /* Get CPU-computed normals and upload them (computed from base mesh state)
       * Use vert_normals_true() for MOD_DISP_DIR_NOR (face normals, ignore sharp edges)
       * Use vert_normals() for MOD_DISP_DIR_CLNOR (corner normals averaged, respect sharp edges) */
      blender::Span<blender::float3> cpu_normals = (dmd->direction == MOD_DISP_DIR_NOR) ?
                                                      mesh_owner->vert_normals_true() :
                                                      mesh_owner->vert_normals();
      
      const size_t size_normals = msd.verts_num * sizeof(blender::float4);
      ssbo_normals = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_normals, size_normals);
      
      if (ssbo_normals && !cpu_normals.is_empty()) {
        /* Pad float3 to float4 for GPU alignment */
        std::vector<blender::float4> padded_normals(msd.verts_num);
        for (int i = 0; i < msd.verts_num; ++i) {
          padded_normals[i] = blender::float4(
              cpu_normals[i].x, cpu_normals[i].y, cpu_normals[i].z, 0.0f);
        }
        GPU_storagebuf_update(ssbo_normals, padded_normals.data());
      }
    }
    
    if (ssbo_normals) {
      GPU_storagebuf_bind(ssbo_normals, 3);
    }
  }
  
  /* Bind texture coordinates and texture (if present) */
  if (has_texture) {
    if (ssbo_texcoords) {
      GPU_storagebuf_bind(ssbo_texcoords, 4);
    }
    if (gpu_texture) {
      GPU_texture_bind(gpu_texture, 0);
    }
  }

  /* Set uniforms (runtime parameters) */
  GPU_shader_uniform_mat4(shader, "local_mat", (const float(*)[4])local_mat);
  GPU_shader_uniform_1f(shader, "strength", dmd->strength);
  GPU_shader_uniform_1f(shader, "midlevel", dmd->midlevel);
  GPU_shader_uniform_1i(shader, "direction", int(dmd->direction));
  GPU_shader_uniform_1b(shader, "use_global", use_global);
  
  /* Set texture processing parameters (if texture is present) */
  if (has_texture && dmd->texture) {
    Tex *tex = dmd->texture;
    Image *ima = tex->ima;
    
    /* Determine if we should use de-premultiply (talpha flag logic from imagewrap)
     * talpha is set when: TEX_USEALPHA && alpha_mode != IGNORE && !TEX_CALCALPHA */
    bool use_talpha = false;
    if ((tex->imaflag & TEX_USEALPHA) && ima && (ima->alpha_mode != IMA_ALPHA_IGNORE)) {
      if ((tex->imaflag & TEX_CALCALPHA) == 0) {
        use_talpha = true;
      }
    }
    
    GPU_shader_uniform_1b(shader, "use_talpha", use_talpha);
    GPU_shader_uniform_1f(shader, "tex_bright", tex->bright);
    GPU_shader_uniform_1f(shader, "tex_contrast", tex->contrast);
    GPU_shader_uniform_1f(shader, "tex_saturation", tex->saturation);
    GPU_shader_uniform_1f(shader, "tex_rfac", tex->rfac);
    GPU_shader_uniform_1f(shader, "tex_gfac", tex->gfac);
    GPU_shader_uniform_1f(shader, "tex_bfac", tex->bfac);
    GPU_shader_uniform_1b(shader, "tex_no_clamp", (tex->flag & TEX_NO_CLAMP) != 0);
    GPU_shader_uniform_1i(shader, "tex_extend", int(tex->extend));
    
    /* Upload crop parameters (xmin, ymin, xmax, ymax) */
    float crop[4] = {tex->cropxmin, tex->cropymin, tex->cropxmax, tex->cropymax};
    GPU_shader_uniform_4f(shader, "tex_crop", crop[0], crop[1], crop[2], crop[3]);
    
    /* Upload repeat/mirror flags */
    GPU_shader_uniform_2f(shader, "tex_repeat", float(tex->xrepeat), float(tex->yrepeat));
    GPU_shader_uniform_1b(shader, "tex_xmir", (tex->flag & TEX_REPEAT_XMIR) != 0);
    GPU_shader_uniform_1b(shader, "tex_ymir", (tex->flag & TEX_REPEAT_YMIR) != 0);
    GPU_shader_uniform_1b(shader, "tex_interpol", (tex->imaflag & TEX_INTERPOL) != 0);
    GPU_shader_uniform_1b(shader, "tex_checker_odd", (tex->flag & TEX_CHECKER_ODD) != 0);
    GPU_shader_uniform_1b(shader, "tex_checker_even", (tex->flag & TEX_CHECKER_EVEN) != 0);
    GPU_shader_uniform_1b(shader, "tex_flipblend", (tex->flag & TEX_FLIPBLEND) != 0);
    GPU_shader_uniform_1b(shader, "tex_flip_axis", (tex->imaflag & TEX_IMAROT) != 0);
  }


  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1);

  /* Unbind texture */
  if (gpu_texture) {
    GPU_texture_unbind(gpu_texture);
  }

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  msd.pending_gpu_setup = false;
  msd.gpu_setup_attempts = 0;

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

  BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);

  /* Invalidate all Displace modifiers for this mesh */
  for (auto item : impl_->static_map.items()) {
    if (item.key.mesh == mesh) {
      /* Lookup again to get mutable reference */
      Impl::MeshStaticData *msd = impl_->static_map.lookup_ptr(item.key);
      if (msd) {
        msd->pending_gpu_setup = true;
        msd->gpu_setup_attempts = 0;
      }
    }
  }
}

void DisplaceManager::free_all()
{
  impl_->static_map.clear();
}

/** \} */
