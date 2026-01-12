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
#include "gpu_shader_common_normal_lib.hh"  /* Common normal calculation functions */
#include "gpu_shader_common_texture_lib.hh" /* Common texture sampling functions */

#include "DRW_render.hh"
#include "draw_cache_impl.hh"

#include "DEG_depsgraph_query.hh"

#include "MEM_guardedalloc.h"

namespace blender::draw {

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


static std::string get_vertex_normals()
{
  using namespace gpu;
  /* Define position buffer macro before including libs */
  return "#define POSITION_BUFFER input_positions\n" + 
         blender::gpu::get_common_texture_lib_glsl() +  /* ColorBand + boxsample + do_2d_mapping() */
         blender::gpu::get_common_normal_lib_glsl();   /* Normal calculation functions */
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

if (mapping_use_input_positions) {
  vec3 in_pos = input_positions[v].xyz;
  if (tex_mapping == 0) { //MOD_DISP_MAP_LOCAL
    tex_coord = in_pos;
  } else if (tex_mapping == 1) { //MOD_DISP_MAP_GLOBAL
    vec4 w = object_to_world_mat * vec4(in_pos, 1.0);
    tex_coord = w.xyz;
  } else if (tex_mapping == 2) { //MOD_DISP_MAP_OBJECT
    vec4 w = object_to_world_mat * vec4(in_pos, 1.0);
    vec4 o = mapref_imat * w;
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
texres.talpha = use_talpha;  /* From CPU line 211-213 */
texres.tin = 0.0;

/* Step 1: FLAT mapping (normalize [-1,1] → [0,1]) */
float fx = (tex_coord.x + 1.0) / 2.0;
float fy = (tex_coord.y + 1.0) / 2.0;

/* Step 3: Apply imagewrap() - handles all wrapping, filtering, and sampling
 * This now includes CLIPCUBE check, coordinate wrapping, and texture sampling */
  vec3 mapped_coord = vec3(fx, fy, tex_coord.z);
  int retval = multitex_components(mapped_coord, texres.trgba, texres.tin, texres.talpha, int(v));
  /* texres.trgba and texres.tin are filled/processed by imagewrap() to match CPU pipeline */
  vec3 rgb = texres.trgba.rgb;
  
  /* Use texres.tin for intensity to match CPU naming convention (imagewrap.cc line 244-253)
   * If the sampled result contained RGB data (retval & TEX_RGB) compute intensity from RGB.
   * Otherwise propagate the intensity into the color channels (CPU copies tin to trgba). */
  if ((retval & TEX_RGB) != 0) {
    texres.tin = (rgb.r + rgb.g + rgb.b) * (1.0 / 3.0);
  }
  else {
    texres.trgba.rgb = vec3(texres.tin);
    rgb = vec3(texres.tin);
  }

  if (tex_flipblend) {
    texres.tin = 1.0 - texres.tin;
  }

  float s = strength * vgroup_weight;
  vec3 rgb_displacement = (rgb - vec3(midlevel)) * s;
  delta = (texres.tin - midlevel) * s;
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
    vec3 n_mesh = compute_vertex_normal_smooth(int(v));
    /* Displacement along vertex normal
     * This matches CPU behavior and is acceptable for most use cases. */
    co += delta * math_normalize(n_mesh);
  }
  else if (direction == MOD_DISP_DIR_CLNOR) {
    /* Displacement along custom loop normals (Simplification -> same than DISP_DIR_NOR) */
    vec3 n_mesh = compute_vertex_normal_smooth(int(v));
    co += delta * math_normalize(n_mesh);
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
}

/* Final assembly function - concatenates both parts */
static std::string get_displace_compute_src()
{
  return get_vertex_normals() + get_displace_shader_part2();
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
    if (dmd->texture->type == TEX_IMAGE && dmd->texture->ima) {
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
  if (dmd->texture && dmd->texture->type == TEX_IMAGE) {
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
              displace_upload_ibuf_to_texture(gpu_texture, ibuf, ima->colorspace_settings.name);

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
  bool use_colorband = false;

  /* ColorBand UBO layout (std140, vec4-aligned):
   * struct CBData {
   *   vec4 rgba;         // r, g, b, a packed in vec4 (16 bytes)
   *   vec4 pos_cur_pad;  // pos, cur (as float), pad[2] (16 bytes)
   * };  // Total: 32 bytes per stop
   *
   * struct ColorBand {
   *   ivec4 tot_cur_ipotype_hue;  // tot, cur, ipotype, ipotype_hue (16 bytes)
   *   ivec4 color_mode_pad;       // color_mode, pad[3] (16 bytes)
   *   CBData data[32];            // 32 * 32 bytes = 1024 bytes
   * };  // Total: 1056 bytes */

  struct GPUCBData {
    float rgba[4];        /* r, g, b, a */
    float pos_cur_pad[4]; /* pos, cur (as float), pad[2] */
  };

  struct GPUColorBand {
    int32_t tot_cur_ipotype_hue[4]; /* tot, cur, ipotype, ipotype_hue */
    int32_t color_mode_pad[4];      /* color_mode, pad[3] */
    GPUCBData data[32];
  };

  const size_t size_colorband = sizeof(GPUColorBand);

  /* Check if UBO already exists in cache */
  ubo_colorband = bke::BKE_mesh_gpu_internal_ubo_get(mesh_owner, key_colorband);

  if (!ubo_colorband) {
    /* Create and upload ColorBand data */
    if (shader_has_texture && dmd->texture->coba && (dmd->texture->flag & TEX_COLORBAND)) {
      use_colorband = true;

      GPUColorBand gpu_coba = {};
      ColorBand *coba = dmd->texture->coba;

      gpu_coba.tot_cur_ipotype_hue[0] = coba->tot;
      gpu_coba.tot_cur_ipotype_hue[1] = coba->cur;
      gpu_coba.tot_cur_ipotype_hue[2] = coba->ipotype;
      gpu_coba.tot_cur_ipotype_hue[3] = coba->ipotype_hue;
      gpu_coba.color_mode_pad[0] = coba->color_mode;

      for (int i = 0; i < 32; i++) {
        gpu_coba.data[i].rgba[0] = coba->data[i].r;
        gpu_coba.data[i].rgba[1] = coba->data[i].g;
        gpu_coba.data[i].rgba[2] = coba->data[i].b;
        gpu_coba.data[i].rgba[3] = coba->data[i].a;
        gpu_coba.data[i].pos_cur_pad[0] = coba->data[i].pos;
        gpu_coba.data[i].pos_cur_pad[1] = float(coba->data[i].cur);
      }

      ubo_colorband = bke::BKE_mesh_gpu_internal_ubo_ensure(
          mesh_owner, deformed_eval, key_colorband, size_colorband);
      if (ubo_colorband) {
        GPU_uniformbuf_update(ubo_colorband, &gpu_coba);
      }
    }
    else {
      /* No colorband: create dummy UBO to avoid binding errors */
      GPUColorBand dummy_coba = {};
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
  const std::string shader_key = "displace_compute_v2";
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
    shader_src += get_displace_compute_src();
    std::string glsl_accessors = BKE_mesh_gpu_topology_glsl_accessors_string(
        mesh_gpu_data->topology);

    /* Build typedef header with ColorBand structure (vec4-aligned for UBO std140 layout) */
    std::string typedef_header = R"GLSL(
struct CBData {
  vec4 rgba;         /* r, g, b, a packed in vec4 */
  vec4 pos_cur_pad;  /* pos, cur (as float), pad[2] */
};

struct ColorBand {
  ivec4 tot_cur_ipotype_hue;  /* tot, cur, ipotype, ipotype_hue */
  ivec4 color_mode_pad;       /* color_mode, pad[3] */
  CBData data[32];
};
)GLSL";

    info.typedef_source_generated = typedef_header;
    info.compute_source_generated = glsl_accessors + shader_src;

    /* Bindings */
    info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
    info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
    info.storage_buf(2, Qualifier::read, "float", "vgroup_weights[]");
    if (shader_has_texture) {
      info.storage_buf(3, Qualifier::read, "vec4", "texture_coords[]");
      info.sampler(0, ImageType::Float2D, "displacement_texture");
    }
    /* ColorBand UBO (binding 5) - added for TEX_COLORBAND support */
    info.uniform_buf(4, "ColorBand", "tex_colorband");
    /* Topology SSBO (binding 4) - parser automatically generates declaration before typedef */
    info.storage_buf(15, Qualifier::read, "int", "topo[]");

    /* Push constants */
    info.push_constant(Type::float4x4_t, "local_mat");
    info.push_constant(Type::float_t, "strength");
    info.push_constant(Type::float_t, "midlevel");
    info.push_constant(Type::int_t, "direction");
    info.push_constant(Type::bool_t, "use_global");
    info.push_constant(Type::bool_t, "use_colorband"); /* ColorBand enable flag */

    /* Texture processing parameters (for BRICONTRGB and de-premultiply) */
    if (shader_has_texture) {
      info.push_constant(Type::bool_t, "use_talpha");      /* Enable de-premultiply */
      info.push_constant(Type::bool_t, "tex_calcalpha");   /* TEX_CALCALPHA */
      info.push_constant(Type::bool_t, "tex_negalpha");    /* TEX_NEGALPHA */
      info.push_constant(Type::float_t, "tex_bright");     /* Tex->bright */
      info.push_constant(Type::float_t, "tex_contrast");   /* Tex->contrast */
      info.push_constant(Type::float_t, "tex_saturation"); /* Tex->saturation */
      info.push_constant(Type::float_t, "tex_rfac");       /* Tex->rfac */
      info.push_constant(Type::float_t, "tex_gfac");       /* Tex->gfac */
      info.push_constant(Type::float_t, "tex_bfac");       /* Tex->bfac */
      info.push_constant(Type::bool_t, "tex_no_clamp");    /* Tex->flag & TEX_NO_CLAMP */
      info.push_constant(Type::int_t, "tex_extend");       /* Tex->extend (wrap mode) */
      info.push_constant(Type::float4_t,
                         "tex_crop"); /* (cropxmin, cropymin, cropxmax, cropymax) */
      info.push_constant(Type::float2_t, "tex_repeat");     /* (xrepeat, yrepeat) */
      info.push_constant(Type::bool_t, "tex_xmir");         /* TEX_REPEAT_XMIR */
      info.push_constant(Type::bool_t, "tex_ymir");         /* TEX_REPEAT_YMIR */
      info.push_constant(Type::bool_t, "tex_interpol");     /* TEX_INTERPOL */
      info.push_constant(Type::float_t, "tex_filtersize");  /* Tex->filtersize for boxsample */
      info.push_constant(Type::bool_t, "tex_checker_odd");  /* TEX_CHECKER_ODD */
      info.push_constant(Type::bool_t, "tex_checker_even"); /* TEX_CHECKER_EVEN */
      info.push_constant(Type::float_t, "tex_checkerdist"); /* Tex->checkerdist */
      info.push_constant(Type::bool_t, "tex_flipblend");    /* TEX_FLIPBLEND */
      info.push_constant(Type::bool_t, "tex_flip_axis");    /* TEX_IMAROT (flip X/Y) */
      /* Texture transformation parameters (rotation/scale/offset) */
      info.push_constant(Type::float3_t, "tex_size_param");  /* Tex->size (scale X/Y/Z) */
      info.push_constant(Type::float3_t, "tex_ofs");         /* Tex->ofs (offset X/Y/Z) */
      info.push_constant(Type::float_t, "tex_rot");          /* Tex->rot (rotation Z in radians) */
      /* Mapping controls (when mapping_use_input_positions==true shader will
       * compute texture coords from input_positions[] instead of using
       * precomputed texture_coords[]). UV mapping remains CPU-side. */
      info.push_constant(Type::int_t, "tex_mapping");
      info.push_constant(Type::bool_t, "mapping_use_input_positions");
      info.push_constant(Type::float4x4_t, "object_to_world_mat");
      info.push_constant(Type::float4x4_t, "mapref_imat");
      info.push_constant(Type::bool_t, "tex_is_byte"); /* Image data originally bytes (needs premultiply) */
      info.push_constant(Type::bool_t, "tex_is_float"); /* ImBuf had float data */
      info.push_constant(Type::int_t, "tex_channels");  /* number of channels in ImBuf (1/3/4) */
      info.push_constant(Type::int_t, "mtex_mapto");    /* MTex.mapto flags (MAP_COL etc.) */
      /* Texture subtype and flags to match CPU Tex struct (Tex->stype, Tex->flag) */
      info.push_constant(Type::int_t, "tex_stype");
      info.push_constant(Type::int_t, "tex_flag");
      info.push_constant(Type::int_t, "tex_type");
      info.push_constant(Type::int_t, "tex_noisebasis");
      info.push_constant(Type::int_t, "tex_noisebasis2");
      info.push_constant(Type::float_t, "tex_noisesize");
      info.push_constant(Type::float_t, "tex_turbul");
      info.push_constant(Type::int_t, "tex_noisetype");
      info.push_constant(Type::int_t, "tex_noisedepth");
      info.push_constant(Type::int_t, "tex_frame"); /* Current frame for animated textures */

    }
    BKE_mesh_gpu_topology_add_specialization_constants(info, mesh_gpu_data->topology);

    shader = bke::BKE_mesh_gpu_internal_shader_ensure(mesh_owner, deformed_eval, shader_key, info);
  }
  if (!shader) {
    return nullptr;
  }

  if (ubo_colorband && use_colorband) {
    /* Update UBO only when colorband content changed to avoid redundant uploads. */
    ColorBand *coba = dmd->texture->coba;
    uint32_t new_hash = colorband_hash_from_coba(coba);
    if (new_hash != msd.colorband_hash) {
      GPUColorBand gpu_coba = {};

      gpu_coba.tot_cur_ipotype_hue[0] = coba->tot;
      gpu_coba.tot_cur_ipotype_hue[1] = coba->cur;
      gpu_coba.tot_cur_ipotype_hue[2] = coba->ipotype;
      gpu_coba.tot_cur_ipotype_hue[3] = coba->ipotype_hue;
      gpu_coba.color_mode_pad[0] = coba->color_mode;

      for (int i = 0; i < 32; i++) {
        gpu_coba.data[i].rgba[0] = coba->data[i].r;
        gpu_coba.data[i].rgba[1] = coba->data[i].g;
        gpu_coba.data[i].rgba[2] = coba->data[i].b;
        gpu_coba.data[i].rgba[3] = coba->data[i].a;
        gpu_coba.data[i].pos_cur_pad[0] = coba->data[i].pos;
        gpu_coba.data[i].pos_cur_pad[1] = float(coba->data[i].cur);
      }

      GPU_uniformbuf_update(ubo_colorband, &gpu_coba);
      msd.colorband_hash = new_hash;
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

  /* Set uniforms (runtime parameters) */
  GPU_shader_uniform_mat4(shader, "local_mat", (const float(*)[4])local_mat);
  GPU_shader_uniform_1f(shader, "strength", dmd->strength);
  GPU_shader_uniform_1f(shader, "midlevel", dmd->midlevel);
  GPU_shader_uniform_1i(shader, "direction", int(dmd->direction));
  GPU_shader_uniform_1b(shader, "use_global", use_global);
  GPU_shader_uniform_1b(shader, "use_colorband", use_colorband); /* ColorBand enable flag */

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

    GPU_shader_uniform_1b(shader, "use_talpha", use_talpha);
    GPU_shader_uniform_1b(shader, "tex_calcalpha", (tex->imaflag & TEX_CALCALPHA) != 0);
    GPU_shader_uniform_1b(shader, "tex_negalpha", (tex->flag & TEX_NEGALPHA) != 0);
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
    GPU_shader_uniform_1b(shader, "tex_checker_odd", (tex->flag & TEX_CHECKER_ODD) == 0);
    GPU_shader_uniform_1b(shader, "tex_checker_even", (tex->flag & TEX_CHECKER_EVEN) == 0);
    GPU_shader_uniform_1b(shader, "tex_flipblend", (tex->flag & TEX_FLIPBLEND) != 0);
    GPU_shader_uniform_1b(shader, "tex_flip_axis", (tex->imaflag & TEX_IMAROT) != 0);
    GPU_shader_uniform_1f(shader, "tex_filtersize", tex->filtersize);

    /* Checker pattern scaling parameter */
    GPU_shader_uniform_1f(shader, "tex_checkerdist", tex->checkerdist);

    /* NEW: Texture transformation parameters (rotation/scale/offset)
     * NOTE: These fields (size/ofs/rot) may not exist in all Tex versions.
     * For now, use default values (no transformation). Future versions can enable these. */
    float tex_size_default[3] = {1.0f, 1.0f, 1.0f};
    float tex_ofs_default[3] = {0.0f, 0.0f, 0.0f};
    float tex_rot_default = 0.0f;
    
    GPU_shader_uniform_3f(shader, "tex_size_param", tex_size_default[0], tex_size_default[1], tex_size_default[2]);
    GPU_shader_uniform_3f(shader, "tex_ofs", tex_ofs_default[0], tex_ofs_default[1], tex_ofs_default[2]);
    GPU_shader_uniform_1f(shader, "tex_rot", tex_rot_default);

    /* Use metadata from MeshStaticData (filled during texture upload) */
    GPU_shader_uniform_1b(shader, "tex_is_byte", msd.tex_is_byte);
    GPU_shader_uniform_1b(shader, "tex_is_float", msd.tex_is_float);
    msd.tex_channels = GPU_texture_component_len(GPU_texture_format(gpu_texture));
    GPU_shader_uniform_1i(shader, "tex_channels", msd.tex_channels);

    /* Texture subtype and flag (match CPU Tex struct) */
    GPU_shader_uniform_1i(shader, "tex_stype", int(tex->stype));
    GPU_shader_uniform_1i(shader, "tex_flag", int(tex->flag));

    /* Pass mtex->mapto to shader so it can decide whether to apply scene color conversion
     * (MAP_COL flag). If no mtex is used, this will be 0. */
    int mtex_mapto = 0; /* default: none */
    GPU_shader_uniform_1i(shader, "mtex_mapto", mtex_mapto);

    /* Mapping controls: replicate CPU logic from MOD_get_texture_coords()
     * If MOD_DISP_MAP_OBJECT but no map_object, fallback to LOCAL.
     * If UV mapping, use precomputed coords (mapping_use_input_positions = false).
     * Otherwise compute coords from input_positions in shader. */
    int tex_mapping = int(dmd->texmapping);

    /* Replicate CPU fallback: if OBJECT mapping but no map_object, use LOCAL */
    if (tex_mapping == MOD_DISP_MAP_OBJECT && dmd->map_object == nullptr) {
      tex_mapping = MOD_DISP_MAP_LOCAL;
    }

    /* When no texture coordinates were uploaded (procedural textures)
     * force the shader to compute coords from input_positions[]. This
     * prevents reading an unbound/empty SSBO when using NON-UV mapping. */
    bool mapping_use_input_positions = (tex_mapping != MOD_DISP_MAP_UV) || msd.tex_coords.empty();
    GPU_shader_uniform_1i(shader, "tex_mapping", tex_mapping);
    GPU_shader_uniform_1b(shader, "mapping_use_input_positions", mapping_use_input_positions);

    /* Pass object->world matrix (fast copy) */
    float obj2w[4][4];
    memcpy(obj2w, deformed_eval->object_to_world().ptr(), sizeof(obj2w));
    GPU_shader_uniform_mat4(shader, "object_to_world_mat", obj2w);

    /* mapref_imat: compute inverse map reference for MOD_DISP_MAP_OBJECT when possible.
     * Falls back to identity when no map_object is set. This mirrors logic from
     * MOD_get_texture_coords(). */
    float mapref_imat[4][4];
    if (dmd->texmapping == MOD_DISP_MAP_OBJECT && dmd->map_object != nullptr) {
      Object *map_object = dmd->map_object;
      if (dmd->map_bone[0] != '\0') {
        bPoseChannel *pchan = BKE_pose_channel_find_name(map_object->pose, dmd->map_bone);
        if (pchan) {
          float mat_bone_world[4][4];
          mul_m4_m4m4(mat_bone_world, map_object->object_to_world().ptr(), pchan->pose_mat);
          invert_m4_m4(mapref_imat, mat_bone_world);
        }
        else {
          invert_m4_m4(mapref_imat, map_object->object_to_world().ptr());
        }
      }
      else {
        invert_m4_m4(mapref_imat, map_object->object_to_world().ptr());
      }
    }
    else {
      unit_m4(mapref_imat);
    }
    GPU_shader_uniform_mat4(shader, "mapref_imat", mapref_imat);
    /* Multitex / noise uniforms: ensure these are uploaded when present. */
    GPU_shader_uniform_1i(shader, "tex_type", int(tex->type));
    GPU_shader_uniform_1i(shader, "tex_noisebasis", int(tex->noisebasis));
    GPU_shader_uniform_1i(shader, "tex_noisebasis2", int(tex->noisebasis2));
    GPU_shader_uniform_1f(shader, "tex_noisesize", float(tex->noisesize));
    GPU_shader_uniform_1f(shader, "tex_turbul", float(tex->turbul));
    GPU_shader_uniform_1i(shader, "tex_noisetype", int(tex->noisetype));
    GPU_shader_uniform_1i(shader, "tex_noisedepth", int(tex->noisedepth));
    GPU_shader_uniform_1i(shader, "tex_frame", scene_frame);
  }

  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1, constants);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  /* Unbind texture */
  if (gpu_texture) {
    GPU_texture_unbind(gpu_texture);
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

}  // namespace blender::draw
