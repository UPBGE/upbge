/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
/** \file
 * \ingroup draw
 *
 * GPU-accelerated Displace modifier implementation.
 */

#pragma once

#include "draw_displace.hh"

#include "BLI_hash.h"
#include "BLI_noise.h"
#include "BLI_map.hh"
#include "BLI_rand.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_vector.hh"

#include "BKE_action.hh" /* BKE_pose_channel_find_name for MAP_OBJECT/bone */
#include "BKE_colorband.hh"
#include "BKE_deform.hh"
#include "BKE_image.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_object.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "../modifiers/intern/MOD_util.hh"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include <cstdio>
#include <cstring>
#include <cstdint>

#include "../gpu/intern/gpu_shader_create_info.hh"
#include "../gpu/gpu_modifiers_common/gpu_shader_common_normal_lib.hh"  /* Common normal calculation functions */
#include "../gpu/gpu_modifiers_common/gpu_shader_common_texture_lib.hh" /* Common texture sampling functions */
#include "../gpu/gpu_modifiers_common/gpu_texture_helpers.hh"

#include "DRW_render.hh"
#include "draw_cache_impl.hh"
#include "draw_cache_extract.hh"

#include "DEG_depsgraph_query.hh"

#include "../blenkernel/intern/mesh_gpu_cache.hh"

#include "MEM_guardedalloc.h"

namespace blender {
namespace draw {

/* -------------------------------------------------------------------- */
/** \name Colorspace Management Helpers
 * \{ */

/**
 * Check if we need manual colorspace handling for this image.
 * Returns true if the image uses a non-"Non-Color" colorspace.
 */
static bool displace_needs_manual_colorspace(Image *ima)
{
  if (!ima) {
    return false;
  }

  /* Check if colorspace is NOT "Non-Color" */
  if (ima->colorspace_settings.name[0] != '\0') {
    return !STREQ(ima->colorspace_settings.name, "Non-Color");
  }

  /* Default: assume sRGB if no colorspace specified */
  return true;
}

/**
 * Upload ImBuf data to GPU texture WITHOUT colorspace conversion.
 * For displacement, we want raw byte values (matching CPU behavior).
 * The CPU reads bytes directly without sRGB→linear conversion, so GPU must do the same.
 */
static void displace_upload_ibuf_to_texture(gpu::Texture *tex,
                                            ImBuf *ibuf,
                                            const char * /*colorspace_name*/)
{
  if (!tex || !ibuf) {
    return;
  }

  const int width = ibuf->x;
  const int height = ibuf->y;

  /* For displacement, we want raw values WITHOUT colorspace conversion.
   * CPU reads bytes directly (no sRGB decode), so GPU must match.
   * Always upload as RGBA float (converted from bytes if needed). */
  float *upload_data = MEM_malloc_arrayN<float>(width * height * 4, "displace_tex_upload");

  if (ibuf->float_buffer.data) {
    /* Float buffer: copy directly */
    for (int i = 0; i < width * height; i++) {
      if (ibuf->channels == 4) {
        upload_data[4 * i + 0] = ibuf->float_buffer.data[4 * i + 0];
        upload_data[4 * i + 1] = ibuf->float_buffer.data[4 * i + 1];
        upload_data[4 * i + 2] = ibuf->float_buffer.data[4 * i + 2];
        upload_data[4 * i + 3] = ibuf->float_buffer.data[4 * i + 3];
      }
      else if (ibuf->channels == 3) {
        upload_data[4 * i + 0] = ibuf->float_buffer.data[3 * i + 0];
        upload_data[4 * i + 1] = ibuf->float_buffer.data[3 * i + 1];
        upload_data[4 * i + 2] = ibuf->float_buffer.data[3 * i + 2];
        upload_data[4 * i + 3] = 1.0f;
      }
      else {
        /* Single channel */
        float val = ibuf->float_buffer.data[i];
        upload_data[4 * i + 0] = val;
        upload_data[4 * i + 1] = val;
        upload_data[4 * i + 2] = val;
        upload_data[4 * i + 3] = 1.0f;
      }
    }
  }
  else if (ibuf->byte_buffer.data) {
    /* Byte buffer: convert to float WITHOUT sRGB decoding
     * This matches CPU behavior which reads bytes directly as linear values */
    for (int i = 0; i < width * height; i++) {
      const uchar *rect = &ibuf->byte_buffer.data[4 * i];
      upload_data[4 * i + 0] = float(rect[0]) * (1.0f / 255.0f);
      upload_data[4 * i + 1] = float(rect[1]) * (1.0f / 255.0f);
      upload_data[4 * i + 2] = float(rect[2]) * (1.0f / 255.0f);
      upload_data[4 * i + 3] = float(rect[3]) * (1.0f / 255.0f);
    }
  }
  else {
    /* No valid buffer */
    MEM_freeN(upload_data);
    return;
  }

  /* Upload to GPU */
  GPU_texture_update(tex, GPU_DATA_FLOAT, upload_data);

  /* Free temp buffer */
  MEM_freeN(upload_data);
}

/** \} */

/* Compute a stable 32-bit hash for a ColorBand to detect changes. Uses FNV-1a. */
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

  /* Hash only the active stops (up to 32). For floats, hash their bit pattern. */
  int tot = coba->tot;
  if (tot < 0) {
    tot = 0;
  }
  if (tot > 32) {
    tot = 32;
  }

  for (int i = 0; i < tot; ++i) {
    const auto &stop = coba->data[i];

    uint32_t v;
    memcpy(&v, &stop.r, sizeof(v));
    hash = BLI_hash_int_2d(hash, v);
    memcpy(&v, &stop.g, sizeof(v));
    hash = BLI_hash_int_2d(hash, v);
    memcpy(&v, &stop.b, sizeof(v));
    hash = BLI_hash_int_2d(hash, v);
    memcpy(&v, &stop.a, sizeof(v));
    hash = BLI_hash_int_2d(hash, v);
    memcpy(&v, &stop.pos, sizeof(v));
    hash = BLI_hash_int_2d(hash, v);
    hash = BLI_hash_int_2d(hash, uint32_t(stop.cur));
  }

  return hash;
}

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
/* GPU port of Blender's texture sampling pipeline (texture_procedural.cc + texture_image.cc)
 * Flow: MOD_get_texture_coords() → do_2d_mapping() → imagewrap() → BRICONTRGB
 * This replicates the EXACT CPU path for pixel-perfect GPU/CPU match. */

/* Sample texture using MOD_get_texture_coords() or input_positions when requested */
vec3 tex_coord = texture_coords[v].xyz;

if (u_mapping_use_input_positions) {
  vec3 in_pos = input_positions[v].xyz;
  if (u_tex_mapping == 0) { //MOD_DISP_MAP_LOCAL
    tex_coord = in_pos;
  } else if (u_tex_mapping == 1) { //MOD_DISP_MAP_GLOBAL
    vec4 w = u_object_to_world_mat * vec4(in_pos, 1.0);
    tex_coord = w.xyz;
  } else if (u_tex_mapping == 2) { //MOD_DISP_MAP_OBJECT
    vec4 w = u_object_to_world_mat * vec4(in_pos, 1.0);
    vec4 o = u_mapref_imat * w;
    tex_coord = o.xyz;
  } else {
    /* Fallback to precomputed coords (covers UV case and others) */
    tex_coord = texture_coords[v].xyz;
  }
}
else {
  tex_coord = texture_coords[v].xyz;
}

/* Sample texture (CPU uses boxsample for interpolation) */
TexResult_tex texres;
texres.trgba = vec4(0.0);
texres.talpha = u_use_talpha;  /* From CPU line 211-213 */
texres.tin = 0.0;

/* Pass tex_coord as-is (CPU procedural textures use raw mapped coords; IMAGE handles its own remap). */
 vec3 mapped_coord = tex_coord;
 int retval = multitex(mapped_coord, texres, int(v));
  /* texres.trgba and texres.tin are filled/processed by imagewrap()/multitex() to match CPU pipeline */
  vec3 rgb = texres.trgba.rgb;

  if ((retval & TEX_RGB) != 0) {
    texres.tin = (rgb.r + rgb.g + rgb.b) / 3.0;
  }
  else {
    texres.trgba.rgb = vec3(texres.tin);
  }

  float s = u_strength * vgroup_weight;
  delta = (texres.tin - u_midlevel) * s;
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
      hash = BLI_hash_int_2d(hash, uint32_t(dmd->texture->extend));

      /* Mix Image generation flags/values (use actual values, not addresses). */
      hash = BLI_hash_int_2d(hash, uint32_t(dmd->texture->ima->gen_flag));
      hash = BLI_hash_int_2d(hash, uint32_t(dmd->texture->ima->gen_depth));
      hash = BLI_hash_int_2d(hash, uint32_t(dmd->texture->ima->gen_type));
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

  /* Upload vertex group weights SSBO */
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
  else {
    /* No vertex group: create a per-vertex buffer filled with 1.0f.
     * This avoids backend-dependent behavior when using a single-float
     * dummy (which can lead to incorrect reads on OpenGL). If the mesh has
     * zero vertices allocate a single float to satisfy minimum buffer size. */
    if (!ssbo_vgroup) {
      const size_t count = (msd.verts_num > 0) ? size_t(msd.verts_num) : size_t(1);
      const size_t size_vgroup = count * sizeof(float);
      ssbo_vgroup = bke::BKE_mesh_gpu_internal_ssbo_ensure(
          mesh_owner, deformed_eval, key_vgroup, size_vgroup);
      if (ssbo_vgroup) {
        std::vector<float> dummy(count, 1.0f);
        GPU_storagebuf_update(ssbo_vgroup, dummy.data());
      }
    }
  }

  /* Upload texture coordinates SSBO and prepare texture binding */
  const std::string key_texcoords = key_prefix + "tex_coords";
  gpu::StorageBuf *ssbo_texcoords = nullptr;
  gpu::Texture *gpu_texture = nullptr;
  bool has_texture = false;
  if (dmd->texture && dmd->texture->ima) {
    Image *ima = dmd->texture->ima;
    Tex *tex = dmd->texture;

    /* Setup ImageUser with correct frame for ImageSequence/Movies
     * CRITICAL: ImageUser.framenr must be updated from scene frame for animation!
     * The CPU path (MOD_init_texture) calls BKE_texture_fetch_images_for_pool() which
     * updates iuser.framenr. We must replicate this for GPU. */
    if (ima && ima->runtime && tex) {
      ImageUser iuser = tex->iuser; /* Start with texture's ImageUser */

      /* For animated textures, update frame number from current scene
       * This is CRITICAL for ImageSequence/Movie playback! */
      if (ELEM(ima->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE)) {
        /* Get scene from depsgraph (same as CPU modifier evaluator) and compute
         * the correct image user frame using the shared utility which handles
         * offsets, cycling and ranges. */
        Scene *scene = DEG_get_evaluated_scene(depsgraph);
        if (scene) {
          BKE_image_user_frame_calc(ima, &iuser, int(scene->r.cfra));
        }
      }

      /* CRITICAL: For displacement, we MUST upload textures WITHOUT colorspace auto-decode!
       * CPU reads bytes directly (no decode), GPU must match.
       * 
       * OPTIMIZATION: For "Non-Color" images, BKE_image_get_gpu_texture() already uses
       * UNORM_8_8_8_8 (no sRGB decode), so we can use it directly (fast path).
       * For other colorspaces (sRGB, Rec2020, etc.), we need manual upload with UNORM_8_8_8_8.
       * 
       * Cache texture persistently for static images (FILE/GENERATED/VIEWER).
       * Only animated sources (SEQUENCE/MOVIE) need per-frame updates. */
      
      const bool is_non_color = STREQ(ima->colorspace_settings.name, "Non-Color");
      
      /* Fast path for Non-Color: use GPU cache directly (already UNORM_8_8_8_8) */
      if (is_non_color) {
        gpu_texture = BKE_image_get_gpu_texture(ima, &iuser);
        
        if (gpu_texture && !msd.tex_metadata_cached) {
          /* Cache metadata on first load */
          msd.tex_is_float = GPU_texture_has_float_format(gpu_texture);
          msd.tex_is_byte = !msd.tex_is_float;
          msd.tex_channels = GPU_texture_component_len(GPU_texture_format(gpu_texture));
          msd.tex_metadata_cached = true;
        }
      }
      else {
        /* Slow path for sRGB/Rec2020/etc: manual upload with UNORM_8_8_8_8 */
        const std::string key_texture = key_prefix + "texture_" +
                                       std::to_string(reinterpret_cast<uintptr_t>(ima)) + "_" +
                                       std::to_string(iuser.framenr);

        /* Check if texture already exists in cache (optimization for static images) */
        gpu_texture = bke::BKE_mesh_gpu_internal_texture_get(mesh_owner, key_texture);

        /* Only re-upload if texture doesn't exist OR if image source is animated */
        const bool is_animated = ELEM(ima->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE);
        
        if (!gpu_texture || (is_animated && !gpu_texture)) {
          ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &iuser, nullptr);

          if (ibuf && (ibuf->float_buffer.data || ibuf->byte_buffer.data)) {
            /* Use UNORM_8_8_8_8 for bytes (no sRGB auto-decode), RGBA16F for floats */
            gpu::TextureFormat format;
            if (ibuf->float_buffer.data) {
              format = gpu::TextureFormat::SFLOAT_16_16_16_16;
            }
            else {
              /* CRITICAL: UNORM_8_8_8_8 = no sRGB decode, bytes stay raw */
              format = gpu::TextureFormat::UNORM_8_8_8_8;
            }

            gpu_texture = GPU_texture_create_2d("displace_tex_raw",
                                               ibuf->x,
                                               ibuf->y,
                                               1,
                                               format,
                                               GPU_TEXTURE_USAGE_SHADER_READ,
                                               nullptr);

            if (gpu_texture) {
              /* Upload WITHOUT colorspace conversion (raw bytes) */
              gpu::displace_upload_ibuf_to_texture(gpu_texture, ibuf, ima->colorspace_settings.name);

              /* Cache texture for reuse (static images will persist across frames) */
              bke::BKE_mesh_gpu_internal_texture_ensure(
                  mesh_owner, deformed_eval, key_texture, gpu_texture);

              /* Cache metadata to avoid repeated ImBuf acquisition
               * OPTIMIZATION: For animated sources, metadata stays constant across frames */
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
        /* else: texture already cached, reuse it! (optimization for static images)
         * Metadata already cached in msd, no need to acquire ImBuf */
      }

      if (gpu_texture && !msd.tex_coords.empty()) {
        has_texture = true;

        /* Upload texture coordinates SSBO */
        ssbo_texcoords = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_texcoords);

        if (!ssbo_texcoords) {
          const size_t size_texcoords = msd.tex_coords.size() * sizeof(float4);
          ssbo_texcoords = bke::BKE_mesh_gpu_internal_ssbo_ensure(
              mesh_owner, deformed_eval, key_texcoords, size_texcoords);
          if (ssbo_texcoords) {
            /* Pad float3 to float4 for GPU alignment */
            std::vector<float4> padded_texcoords(msd.tex_coords.size());
            for (size_t i = 0; i < msd.tex_coords.size(); ++i) {
              padded_texcoords[i] = float4(
                  msd.tex_coords[i].x, msd.tex_coords[i].y, msd.tex_coords[i].z, 1.0f);
            }
            GPU_storagebuf_update(ssbo_texcoords, padded_texcoords.data());
          }
        }
      }
    }
  }

  /* Shader-level flag: indicates a Tex is present (image or procedural).
   * Separate from `has_texture` which historically meant image+coords.
   * This controls which shader code paths are compiled and which push
   * constants are emitted. */
  bool shader_has_texture = (dmd->texture != nullptr);

  /* If shader expects a displacement sampler but no image texture was provided
   * (procedural Tex only), create or reuse a 1x1 dummy texture so the sampler
   * binding is valid on all backends. Cache it per-mesh+modifier like other
   * internal textures. */
  if (shader_has_texture && !gpu_texture) {
    const std::string key_dummy = key_prefix + "dummy_displacement";
    gpu_texture = bke::BKE_mesh_gpu_internal_texture_get(mesh_owner, key_dummy);
    if (!gpu_texture) {
      /* Create a 1x1 UNORM texture with a neutral value (midlevel) */
      unsigned char pixel[4] = {128, 128, 128, 255};
      gpu_texture = GPU_texture_create_2d(
          "displace_dummy_tex",
          1,
          1,
          1,
          gpu::TextureFormat::UNORM_8_8_8_8,
          GPU_TEXTURE_USAGE_SHADER_READ,
          nullptr);
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

  /* Upload ColorBand UBO if texture has colorband enabled (TEX_COLORBAND flag)
   * GPU port of: if (tex->flag & TEX_COLORBAND) { ... } */
  const std::string key_colorband = key_prefix + "colorband";
  gpu::UniformBuf *ubo_colorband = nullptr;
  const size_t size_colorband = sizeof(gpu::GPUColorBand);

  /* Check if UBO already exists in cache */
  ubo_colorband = bke::BKE_mesh_gpu_internal_ubo_get(mesh_owner, key_colorband);

  bool use_colorband = false;
  if (!ubo_colorband) {
    /* Create and upload ColorBand data */
    if (shader_has_texture && dmd->texture->coba && (dmd->texture->flag & TEX_COLORBAND)) {
      use_colorband = true;

      ColorBand *coba = dmd->texture->coba;
      gpu::GPUColorBand gpu_coba = {};
      if (blender::gpu::fill_gpu_colorband_from_colorband(gpu_coba, coba)) {
        ubo_colorband = bke::BKE_mesh_gpu_internal_ubo_ensure(
            mesh_owner, deformed_eval, key_colorband, size_colorband);
        if (ubo_colorband) {
          GPU_uniformbuf_update(ubo_colorband, &gpu_coba);
          /* Cache initial colorband hash to avoid redundant uploads. */
          msd.colorband_hash = colorband_hash_from_coba(coba);
        }
      }
    }
    else {
      /* No colorband: create dummy UBO to avoid binding errors */
      gpu::GPUColorBand dummy_coba = {};
      dummy_coba.tot_cur_ipotype_hue[0] = 0; /* 0 stops = disabled */

      ubo_colorband = bke::BKE_mesh_gpu_internal_ubo_ensure(
          mesh_owner, deformed_eval, key_colorband, size_colorband);
      if (ubo_colorband) {
        GPU_uniformbuf_update(ubo_colorband, &dummy_coba);
      }
    }
  }
  else {
    /* UBO exists, check if we need to enable colorband */
    use_colorband = (shader_has_texture && dmd->texture->coba && (dmd->texture->flag & TEX_COLORBAND));
  }

  /* Create/Update TextureParams UBO (similar pattern to ColorBand) */
  const std::string key_tex_params = key_prefix + "texture_params";
  gpu::UniformBuf *ubo_texture_params = bke::BKE_mesh_gpu_internal_ubo_get(mesh_owner, key_tex_params);
  gpu::GPUTextureParams gpu_tex_params = {};

  if (shader_has_texture && dmd->texture) {
    Tex *tex = dmd->texture;

    /* Use centralized helper to fill all TextureParams fields. */
    blender::gpu::fill_texture_params_from_tex(gpu_tex_params,
                                               tex,
                                               (ModifierData *)dmd,
                                               deformed_eval,
                                               scene_frame,
                                               msd.tex_is_byte,
                                               msd.tex_is_float,
                                               msd.tex_channels,
                                               !msd.tex_coords.empty());

    /* `fill_texture_params_from_tex()` has already populated the remaining
     * texture parameter fields (flags, misc3, noise, noisesize/turbul,
     * misc2, distamount, object/world matrices and mapref_imat). Keep a
     * single source of truth to avoid duplicated logic. */
  }

  const size_t size_tex_params = sizeof(gpu::GPUTextureParams);
  if (!ubo_texture_params) {
    ubo_texture_params = bke::BKE_mesh_gpu_internal_ubo_ensure(
        mesh_owner, deformed_eval, key_tex_params, size_tex_params);
  }
  if (ubo_texture_params) {
    GPU_uniformbuf_update(ubo_texture_params, &gpu_tex_params);
  }

  /* Create output SSBO */
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  gpu::StorageBuf *ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, deformed_eval, key_out, size_out);
  if (!ssbo_out) {
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

  if (ubo_colorband && use_colorband) {
    ColorBand *coba = dmd->texture->coba;
    uint32_t new_hash = colorband_hash_from_coba(coba);
    if (new_hash != msd.colorband_hash) {
      gpu::GPUColorBand gpu_coba = {};
      if (blender::gpu::fill_gpu_colorband_from_colorband(gpu_coba, coba)) {
        GPU_uniformbuf_update(ubo_colorband, &gpu_coba);
        msd.colorband_hash = new_hash;
      }
    }
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
  GPU_shader_uniform_1b(shader, "u_use_colorband", use_colorband); /* ColorBand enable flag */

  /* Set texture processing parameters (if a Tex is present) */
  if (shader_has_texture) {
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

    /* Many texture-related parameters are packed in the `TextureParams` UBO
     * and were uploaded earlier via `GPU_uniformbuf_update(ubo_texture_params, ...)`.
     * Legacy `u_tex_*` identifiers are mapped to UBO fields in the GLSL via
     * macros defined in `gpu_shader_common_texture_lib.hh`, so no additional
     * runtime uniforms are required here. */
  }

  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1, constants);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_TEXTURE_FETCH);
  GPU_shader_unbind();

  /* Unbind texture */
  if (gpu_texture) {
    GPU_texture_unbind(gpu_texture);
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
