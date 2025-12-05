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

/* Triangle normal: direct cross product (CPU-accurate for tris) */
vec3 displace_tri_normal(vec3 v0, vec3 v1, vec3 v2) {
  vec3 n = cross(v1 - v0, v2 - v0);
  float len = length(n);
  if (len < 1e-6) {
    return vec3(0.0, 0.0, 1.0);
  }
  return n / len;
}

/* Quad normal: average of two triangles (CPU-accurate for quads)
 * This matches normal_quad_v3() from BLI_math_geom.h */
vec3 displace_quad_normal(vec3 v0, vec3 v1, vec3 v2, vec3 v3) {
  /* Split quad into two triangles and average their normals */
  vec3 n1 = cross(v1 - v0, v2 - v0);
  vec3 n2 = cross(v2 - v0, v3 - v0);
  
  /* Check if the two triangle normals roughly agree (non-twisted quad)
   * If they point in opposite directions, the quad is badly twisted */
  if (dot(n1, n2) < 0.0) {
    /* Twisted quad! Use the longer normal (more reliable) */
    float len1 = length(n1);
    float len2 = length(n2);
    vec3 n = (len1 > len2) ? n1 : n2;
    float len = max(len1, len2);
    if (len < 1e-6) {
      return vec3(0.0, 0.0, 1.0);
    }
    return n / len;
  }
  
  vec3 n = n1 + n2;
  float len = length(n);
  if (len < 1e-6) {
    return vec3(0.0, 0.0, 1.0);
  }
  return n / len;
}

/* Face normal calculation with CPU-accurate tri/quad optimizations
 * Computes face normal from CURRENT deformed positions (after ShapeKeys/Armature/etc.)
 * This ensures normals are coherent with the actual geometry state. */
vec3 displace_newell_face_normal(int f) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  int size = end - beg;
  
  /* Optimized path for triangles (most common case) */
  if (size == 3) {
    vec3 v0 = input_positions[corner_verts(beg)].xyz;
    vec3 v1 = input_positions[corner_verts(beg + 1)].xyz;
    vec3 v2 = input_positions[corner_verts(beg + 2)].xyz;
    return displace_tri_normal(v0, v1, v2);
  }
  
  /* Optimized path for quads (common for subdivided planes) */
  if (size == 4) {
    vec3 v0 = input_positions[corner_verts(beg)].xyz;
    vec3 v1 = input_positions[corner_verts(beg + 1)].xyz;
    vec3 v2 = input_positions[corner_verts(beg + 2)].xyz;
    vec3 v3 = input_positions[corner_verts(beg + 3)].xyz;
    return displace_quad_normal(v0, v1, v2, v3);
  }
  
  /* Newell's method for n-gons (5+ vertices) */
  vec3 n = vec3(0.0);
  int v_prev_idx = corner_verts(end - 1);
  vec3 v_prev = input_positions[v_prev_idx].xyz;
  for (int i = beg; i < end; ++i) {
    int v_curr_idx = corner_verts(i);
    vec3 v_curr = input_positions[v_curr_idx].xyz;
    n += cross(v_prev, v_curr);
    v_prev = v_curr;
  }

  if (length(n) < 1e-4) {
    /* Other axis are already set to zero. */
    n[2] = 1.0f;
  }
  
  /* Robust normalization: handle degenerate faces */
  float len = length(n);
  if (len < 1e-6) {
    return vec3(0.0, 0.0, 1.0);  /* Fallback for degenerate/coplanar face */
  }
  
  return normalize(n);
}

/* Find the corner in face f that uses vertex v */
int find_corner_from_vert(int f, int v) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  for (int i = beg; i < end; ++i) {
    if (corner_verts(i) == v) {
      return i;
    }
  }
  /* Should never happen in valid topology, but guard against it */
  return beg;
}

/* Get previous corner in the face (circular wrapping) */
int face_corner_prev(int f, int corner) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  return (corner == beg) ? (end - 1) : (corner - 1);
}

/* Get next corner in the face (circular wrapping) */
int face_corner_next(int f, int corner) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  return (corner == end - 1) ? beg : (corner + 1);
}

/* Compute vertex normal with angle weighting (CPU-accurate implementation)
 * Uses CURRENT deformed positions for accurate normal calculation.
 * This matches the algorithm in mesh_normals.cc::normals_calc_verts() */
vec3 displace_compute_vertex_normal(int v) {
  int beg = vert_to_face_offsets(v);
  int end = vert_to_face_offsets(v + 1);
  
  /* Handle isolated vertices: use position-based normal like CPU
   * This matches: vert_normals[vert] = math::normalize(positions[vert]); */
  if (beg >= end) {
    vec3 pos = input_positions[v].xyz;
    float len = length(pos);
    return (len > 1e-6) ? (pos / len) : vec3(0.0, 0.0, 1.0);
  }
  
  vec3 n_accum = vec3(0.0);
  vec3 v_pos = input_positions[v].xyz;
  
  /* For each adjacent face, weight its normal by the angle at this vertex's corner */
  for (int i = beg; i < end; ++i) {
    int f = vert_to_face(i);
    
    /* Find the corner for this vertex in this face */
    int corner = find_corner_from_vert(f, v);
    int corner_prev = face_corner_prev(f, corner);
    int corner_next = face_corner_next(f, corner);
    
    int vert_prev = corner_verts(corner_prev);
    int vert_next = corner_verts(corner_next);
    
    /* Compute angle at this corner (angle between the two adjacent edges) */
    vec3 edge_prev = input_positions[vert_prev].xyz - v_pos;
    vec3 edge_next = input_positions[vert_next].xyz - v_pos;
    
    /* Skip degenerate edges (vertices too close after deformation) */
    float len_prev = length(edge_prev);
    float len_next = length(edge_next);
    if (len_prev < 1e-5 || len_next < 1e-5) {
      continue;  /* Skip this face, edges collapsed */
    }
    
    vec3 dir_prev = edge_prev / len_prev;
    vec3 dir_next = edge_next / len_next;
    
    /* Safe acos to avoid NaN from floating point errors */
    float dot_prod = clamp(dot(dir_prev, dir_next), -1.0, 1.0);
    float angle = acos(dot_prod);
    
    /* Skip near-zero angles (colinear edges) */
    if (angle < 1e-4) {
      continue;  /* Degenerate corner, skip */
    }
    
    /* Weight face normal by the angle at this corner
     * This gives more influence to faces with larger angles at this vertex */
    n_accum += displace_newell_face_normal(f) * angle;
  }
  
  /* Robust normalization: handle zero-length accumulation */
  float len = length(n_accum);
  if (len < 1e-6) {
    return vec3(0.0, 0.0, 1.0);  /* Fallback to Z-up if degenerate */
  }
  
  return n_accum / len;
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
  
  /* Sample texture and get linear RGB values */
  vec4 tex_color = texture(displacement_texture, uv);
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

  float s = strength * vgroup_weight;
  delta = (tex_value - midlevel) * s;
#else
  /* Fixed delta (no texture) */
  delta = (1.0 - midlevel) * strength * vgroup_weight;
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
    /* Displacement along vertex normal (recalculated from CURRENT deformed positions)
     * CRITICAL: We use displace_compute_vertex_normal() which calculates normals from the
     * CURRENT frame's deformed positions (after ShapeKeys/Armature/etc.), ensuring
     * coherence with the actual geometry state. This is more accurate than using
     * cached normals from the previous frame. */
    vec3 normal = displace_compute_vertex_normal(int(v));
    co += delta * normal;
  }
  else if (direction == MOD_DISP_DIR_CLNOR) {
    /* MOD_DISP_DIR_CLNOR not supported yet on GPU (requires corner normals averaging)
     * For now, pass-through without displacement to avoid incorrect results. */
    deformed_positions[v] = co_in;
    return;
  }
  /* Note: MOD_DISP_DIR_RGB_XYZ not supported yet */

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
  
  /* Build shader source with topology accessors + conditional texture support */
  std::string shader_src;
  
  /* Always ensure topology exists and add accessors (needed by shader functions)
   * Even if MOD_DISP_DIR_NOR is not active, the shader contains helper functions
   * (displace_newell_face_normal, displace_compute_vertex_normal) that reference topology accessors.
   * These must be defined to avoid compilation errors, even if unused. */
  blender::bke::MeshGPUTopology *topology = BKE_mesh_gpu_get_topology(mesh_owner);
  if (!topology || !topology->ssbo) {
    /* Create and upload topology for this mesh (only once, then cached) */
    blender::bke::MeshGPUTopology temp_topo;
    if (!BKE_mesh_gpu_topology_create(mesh_owner, temp_topo) ||
        !BKE_mesh_gpu_topology_upload(temp_topo))
    {
      return nullptr;
    }
    /* Retrieve pointer again after creation */
    topology = BKE_mesh_gpu_get_topology(mesh_owner);
    if (!topology) {
      return nullptr;
    }
  }
  
  /* Inject topology accessors into shader source */
  shader_src = BKE_mesh_gpu_topology_glsl_accessors_string(*topology);
  
  /* Append main shader code */
  if (has_texture) {
    shader_src += "#define HAS_TEXTURE\n";
  }
  shader_src += displace_compute_src;
  
  info.compute_source_generated = shader_src;

  /* Bindings */
  info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
  info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
  info.storage_buf(2, Qualifier::read, "float", "vgroup_weights[]");
  
  /* Topology buffer (binding 15, always declared, only actively used if MOD_DISP_DIR_NOR) */
  info.storage_buf(15, Qualifier::read, "int", "topo[]");
  
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
  
  /* Bind topology buffer (always, even if only used for MOD_DISP_DIR_NOR) */
  if (topology && topology->ssbo) {
    GPU_storagebuf_bind(topology->ssbo, 15);
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
