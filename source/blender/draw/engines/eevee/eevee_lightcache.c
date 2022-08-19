/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2016 Blender Foundation. */

/** \file
 * \ingroup draw_engine
 *
 * Eevee's indirect lighting cache.
 */

#include "DRW_render.h"

#include "BKE_global.h"

#include "BLI_endian_switch.h"
#include "BLI_threads.h"

#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

#include "BKE_object.h"

#include "DNA_collection_types.h"
#include "DNA_lightprobe_types.h"

#include "PIL_time.h"

#include "eevee_lightcache.h"
#include "eevee_private.h"

#include "GPU_capabilities.h"
#include "GPU_context.h"

#include "WM_api.h"
#include "WM_types.h"

#include "BLO_read_write.h"

#include "wm_window.h"

/* Rounded to nearest PowerOfTwo */
#if defined(IRRADIANCE_SH_L2)
#  define IRRADIANCE_SAMPLE_SIZE_X 4 /* 3 in reality */
#  define IRRADIANCE_SAMPLE_SIZE_Y 4 /* 3 in reality */
#elif defined(IRRADIANCE_HL2)
#  define IRRADIANCE_SAMPLE_SIZE_X 4 /* 3 in reality */
#  define IRRADIANCE_SAMPLE_SIZE_Y 2
#endif

#ifdef IRRADIANCE_SH_L2
/* we need a signed format for Spherical Harmonics */
#  define IRRADIANCE_FORMAT GPU_RGBA16F
#else
#  define IRRADIANCE_FORMAT GPU_RGBA8
#endif

/* OpenGL 3.3 core requirement, can be extended but it's already very big */
#define IRRADIANCE_MAX_POOL_LAYER 256
#define IRRADIANCE_MAX_POOL_SIZE 1024
#define MAX_IRRADIANCE_SAMPLES \
  (IRRADIANCE_MAX_POOL_SIZE / IRRADIANCE_SAMPLE_SIZE_X) * \
      (IRRADIANCE_MAX_POOL_SIZE / IRRADIANCE_SAMPLE_SIZE_Y)

/* TODO: should be replace by a more elegant alternative. */
extern void DRW_opengl_context_enable(void);
extern void DRW_opengl_context_disable(void);

extern void DRW_opengl_render_context_enable(void *re_gl_context);
extern void DRW_opengl_render_context_disable(void *re_gl_context);
extern void DRW_gpu_render_context_enable(void *re_gpu_context);
extern void DRW_gpu_render_context_disable(void *re_gpu_context);

typedef struct EEVEE_LightBake {
  Depsgraph *depsgraph;
  ViewLayer *view_layer;
  ViewLayer *view_layer_input;
  LightCache *lcache;
  Scene *scene;
  struct Main *bmain;
  EEVEE_ViewLayerData *sldata;

  /** Current probe being rendered. */
  LightProbe **probe;
  /** Target cube color texture. */
  GPUTexture *rt_color;
  /** Target cube depth texture. */
  GPUTexture *rt_depth;
  /** Target cube frame-buffers. */
  GPUFrameBuffer *rt_fb[6];
  /** Storage frame-buffer. */
  GPUFrameBuffer *store_fb;
  /** Cube render target resolution. */
  int rt_res;

  /* Shared */
  /** Target layer to store the data to. */
  int layer;
  /** Sample count for the convolution. */
  float samples_count, invsamples_count;
  /** Sampling bias during convolution step. */
  float lod_factor;
  /** Max cube-map LOD to sample when convolving. */
  float lod_max;
  /** Number of probes to render + world probe. */
  int cube_len, grid_len;

  /* Irradiance grid */
  /** Current probe being rendered (UBO data). */
  EEVEE_LightGrid *grid;
  /** Target cube-map at MIP 0. */
  int irr_cube_res;
  /** Size of the irradiance texture. */
  int irr_size[3];
  /** Total for all grids */
  int total_irr_samples;
  /** Nth sample of the current grid being rendered. */
  int grid_sample;
  /** Total number of samples for the current grid. */
  int grid_sample_len;
  /** Nth grid in the cache being rendered. */
  int grid_curr;
  /** The current light bounce being evaluated. */
  int bounce_curr, bounce_len;
  /** Resolution of the Visibility shadow-map. */
  float vis_res;
  /** Result of previous light bounce. */
  GPUTexture *grid_prev;
  /** Pointer to the owner_id of the probe object. */
  LightProbe **grid_prb;

  /* Reflection probe */
  /** Current probe being rendered (UBO data). */
  EEVEE_LightProbe *cube;
  /** Target cube-map at MIP 0. */
  int ref_cube_res;
  /** Index of the current cube. */
  int cube_offset;
  /** Pointer to the owner_id of the probe object. */
  LightProbe **cube_prb;

  /* Dummy Textures */
  struct GPUTexture *dummy_color, *dummy_depth;
  struct GPUTexture *dummy_layer_color;

  int total, done; /* to compute progress */
  short *stop, *do_update;
  float *progress;

  /** For only handling the resources. */
  bool resource_only;
  bool own_resources;
  /** If the light-cache was created for baking, it's first owned by the baker. */
  bool own_light_cache;
  /** ms. delay the start of the baking to not slowdown interactions (TODO: remove). */
  int delay;
  /** Scene frame to bake. */
  int frame;

  /** If running in parallel (in a separate thread), use this context. */
  void *gl_context, *gpu_context;

  ThreadMutex *mutex;
} EEVEE_LightBake;

/* -------------------------------------------------------------------- */
/** \name Light Cache
 * \{ */

/* Return memory footprint in bytes. */
static uint eevee_lightcache_memsize_get(LightCache *lcache)
{
  uint size = 0;
  if (lcache->grid_tx.data) {
    size += MEM_allocN_len(lcache->grid_tx.data);
  }
  if (lcache->cube_tx.data) {
    size += MEM_allocN_len(lcache->cube_tx.data);
    for (int mip = 0; mip < lcache->mips_len; mip++) {
      size += MEM_allocN_len(lcache->cube_mips[mip].data);
    }
  }
  return size;
}

static bool eevee_lightcache_version_check(const LightCache *lcache)
{
  switch (lcache->type) {
    case LIGHTCACHE_TYPE_STATIC:
      return lcache->version == LIGHTCACHE_STATIC_VERSION;
    default:
      return false;
  }
}

static bool eevee_lightcache_can_be_saved(LightCache *lcache)
{
  if (lcache->grid_tx.data) {
    if (MEM_allocN_len(lcache->grid_tx.data) >= INT_MAX) {
      return false;
    }
  }
  if (lcache->cube_tx.data) {
    if (MEM_allocN_len(lcache->cube_tx.data) >= INT_MAX) {
      return false;
    }
  }
  return true;
}

static int eevee_lightcache_irradiance_sample_count(LightCache *lcache)
{
  int total_irr_samples = 0;

  for (int i = 1; i < lcache->grid_len; i++) {
    EEVEE_LightGrid *egrid = lcache->grid_data + i;
    total_irr_samples += egrid->resolution[0] * egrid->resolution[1] * egrid->resolution[2];
  }
  return total_irr_samples;
}

void EEVEE_lightcache_info_update(SceneEEVEE *eevee)
{
  LightCache *lcache = eevee->light_cache_data;

  if (lcache != NULL) {
    if (!eevee_lightcache_version_check(lcache)) {
      BLI_strncpy(eevee->light_cache_info,
                  TIP_("Incompatible Light cache version, please bake again"),
                  sizeof(eevee->light_cache_info));
      return;
    }

    if (lcache->cube_tx.tex_size[2] > GPU_max_texture_layers()) {
      BLI_strncpy(eevee->light_cache_info,
                  TIP_("Error: Light cache is too big for the GPU to be loaded"),
                  sizeof(eevee->light_cache_info));
      return;
    }

    if (lcache->flag & LIGHTCACHE_INVALID) {
      BLI_strncpy(eevee->light_cache_info,
                  TIP_("Error: Light cache dimensions not supported by the GPU"),
                  sizeof(eevee->light_cache_info));
      return;
    }

    if (lcache->flag & LIGHTCACHE_BAKING) {
      BLI_strncpy(
          eevee->light_cache_info, TIP_("Baking light cache"), sizeof(eevee->light_cache_info));
      return;
    }

    if (!eevee_lightcache_can_be_saved(lcache)) {
      BLI_strncpy(eevee->light_cache_info,
                  TIP_("Error: LightCache is too large and will not be saved to disk"),
                  sizeof(eevee->light_cache_info));
      return;
    }

    char formatted_mem[15];
    BLI_str_format_byte_unit(formatted_mem, eevee_lightcache_memsize_get(lcache), false);

    int irr_samples = eevee_lightcache_irradiance_sample_count(lcache);

    BLI_snprintf(eevee->light_cache_info,
                 sizeof(eevee->light_cache_info),
                 TIP_("%d Ref. Cubemaps, %d Irr. Samples (%s in memory)"),
                 lcache->cube_len - 1,
                 irr_samples,
                 formatted_mem);
  }
  else {
    BLI_strncpy(eevee->light_cache_info,
                TIP_("No light cache in this scene"),
                sizeof(eevee->light_cache_info));
  }
}

static void irradiance_pool_size_get(int visibility_size, int total_samples, int r_size[3])
{
  /* Compute how many irradiance samples we can store per visibility sample. */
  int irr_per_vis = (visibility_size / IRRADIANCE_SAMPLE_SIZE_X) *
                    (visibility_size / IRRADIANCE_SAMPLE_SIZE_Y);

  /* The irradiance itself take one layer, hence the +1 */
  int layer_count = MIN2(irr_per_vis + 1, IRRADIANCE_MAX_POOL_LAYER);

  int texel_count = (int)ceilf((float)total_samples / (float)(layer_count - 1));
  r_size[0] = visibility_size *
              max_ii(1, min_ii(texel_count, (IRRADIANCE_MAX_POOL_SIZE / visibility_size)));
  r_size[1] = visibility_size *
              max_ii(1, (texel_count / (IRRADIANCE_MAX_POOL_SIZE / visibility_size)));
  r_size[2] = layer_count;
}

static bool EEVEE_lightcache_validate(const LightCache *light_cache,
                                      const int cube_len,
                                      const int cube_res,
                                      const int grid_len,
                                      const int irr_size[3])
{
  if (light_cache == NULL) {
    return false;
  }
  if (!eevee_lightcache_version_check(light_cache)) {
    return false;
  }

  if (!(light_cache->flag & LIGHTCACHE_INVALID)) {
    /* See if we need the same amount of texture space. */
    if ((irr_size[0] == light_cache->grid_tx.tex_size[0]) &&
        (irr_size[1] == light_cache->grid_tx.tex_size[1]) &&
        (irr_size[2] == light_cache->grid_tx.tex_size[2]) && (grid_len == light_cache->grid_len)) {
      int mip_len = log2_floor_u(cube_res) - MIN_CUBE_LOD_LEVEL;
      if ((cube_res == light_cache->cube_tx.tex_size[0]) &&
          (cube_len == light_cache->cube_tx.tex_size[2] / 6) &&
          (cube_len == light_cache->cube_len) && (mip_len == light_cache->mips_len)) {
        return true;
      }
    }
  }
  return false;
}

LightCache *EEVEE_lightcache_create(const int grid_len,
                                    const int cube_len,
                                    const int cube_size,
                                    const int vis_size,
                                    const int irr_size[3])
{
  LightCache *light_cache = MEM_callocN(sizeof(LightCache), "LightCache");

  light_cache->version = LIGHTCACHE_STATIC_VERSION;
  light_cache->type = LIGHTCACHE_TYPE_STATIC;

  light_cache->cube_data = MEM_callocN(sizeof(EEVEE_LightProbe) * cube_len, "EEVEE_LightProbe");
  light_cache->grid_data = MEM_callocN(sizeof(EEVEE_LightGrid) * grid_len, "EEVEE_LightGrid");

  light_cache->grid_tx.tex = DRW_texture_create_2d_array(
      irr_size[0], irr_size[1], irr_size[2], IRRADIANCE_FORMAT, DRW_TEX_FILTER, NULL);
  light_cache->grid_tx.tex_size[0] = irr_size[0];
  light_cache->grid_tx.tex_size[1] = irr_size[1];
  light_cache->grid_tx.tex_size[2] = irr_size[2];

  int mips_len = log2_floor_u(cube_size) - MIN_CUBE_LOD_LEVEL;

  /* Try to create a cubemap array. */
  DRWTextureFlag cube_texflag = DRW_TEX_FILTER | DRW_TEX_MIPMAP;
  light_cache->cube_tx.tex = DRW_texture_create_cube_array(
      cube_size, cube_len, GPU_R11F_G11F_B10F, cube_texflag, NULL);
  if (light_cache->cube_tx.tex == NULL) {
    /* Try fallback to 2D array. */
    light_cache->cube_tx.tex = DRW_texture_create_2d_array(
        cube_size, cube_size, cube_len * 6, GPU_R11F_G11F_B10F, cube_texflag, NULL);
  }

  light_cache->cube_tx.tex_size[0] = cube_size;
  light_cache->cube_tx.tex_size[1] = cube_size;
  light_cache->cube_tx.tex_size[2] = cube_len * 6;

  light_cache->mips_len = mips_len;
  light_cache->vis_res = vis_size;
  light_cache->ref_res = cube_size;

  light_cache->cube_mips = MEM_callocN(sizeof(LightCacheTexture) * light_cache->mips_len,
                                       "LightCacheTexture");

  if (light_cache->grid_tx.tex == NULL || light_cache->cube_tx.tex == NULL) {
    /* We could not create the requested textures size. Stop baking and do not use the cache. */
    light_cache->flag = LIGHTCACHE_INVALID;
  }
  else {
    light_cache->flag = LIGHTCACHE_UPDATE_WORLD | LIGHTCACHE_UPDATE_CUBE | LIGHTCACHE_UPDATE_GRID;

    for (int mip = 0; mip < light_cache->mips_len; mip++) {
      GPU_texture_get_mipmap_size(
          light_cache->cube_tx.tex, mip + 1, light_cache->cube_mips[mip].tex_size);
    }
  }

  return light_cache;
}

static bool eevee_lightcache_static_load(LightCache *lcache)
{
  /* We use fallback if a texture is not setup and there is no data to restore it. */
  if ((!lcache->grid_tx.tex && !lcache->grid_tx.data) || !lcache->grid_data ||
      (!lcache->cube_tx.tex && !lcache->cube_tx.data) || !lcache->cube_data) {
    return false;
  }
  /* If cache is too big for this GPU. */
  if (lcache->cube_tx.tex_size[2] > GPU_max_texture_layers()) {
    return false;
  }

  if (lcache->grid_tx.tex == NULL) {
    lcache->grid_tx.tex = GPU_texture_create_2d_array(
        "lightcache_irradiance", UNPACK3(lcache->grid_tx.tex_size), 1, IRRADIANCE_FORMAT, NULL);
    GPU_texture_update(lcache->grid_tx.tex, GPU_DATA_UBYTE, lcache->grid_tx.data);

    if (lcache->grid_tx.tex == NULL) {
      lcache->flag |= LIGHTCACHE_NOT_USABLE;
      return false;
    }

    GPU_texture_filter_mode(lcache->grid_tx.tex, true);
  }

  if (lcache->cube_tx.tex == NULL) {
    /* Try to create a cubemap array. */
    lcache->cube_tx.tex = GPU_texture_create_cube_array("lightcache_cubemaps",
                                                        lcache->cube_tx.tex_size[0],
                                                        lcache->cube_tx.tex_size[2] / 6,
                                                        lcache->mips_len + 1,
                                                        GPU_R11F_G11F_B10F,
                                                        NULL);

    if (lcache->cube_tx.tex == NULL) {
      /* Try fallback to 2D array. */
      lcache->cube_tx.tex = GPU_texture_create_2d_array("lightcache_cubemaps_fallback",
                                                        UNPACK3(lcache->cube_tx.tex_size),
                                                        lcache->mips_len + 1,
                                                        GPU_R11F_G11F_B10F,
                                                        NULL);
    }

    if (lcache->cube_tx.tex == NULL) {
      lcache->flag |= LIGHTCACHE_NOT_USABLE;
      return false;
    }

    for (int mip = 0; mip <= lcache->mips_len; mip++) {
      const void *data = (mip == 0) ? lcache->cube_tx.data : lcache->cube_mips[mip - 1].data;
      GPU_texture_update_mipmap(lcache->cube_tx.tex, mip, GPU_DATA_10_11_11_REV, data);
    }
    GPU_texture_mipmap_mode(lcache->cube_tx.tex, true, true);
  }
  return true;
}

bool EEVEE_lightcache_load(LightCache *lcache)
{
  if (lcache == NULL) {
    return false;
  }

  if (!eevee_lightcache_version_check(lcache)) {
    return false;
  }

  if (lcache->flag & (LIGHTCACHE_INVALID | LIGHTCACHE_NOT_USABLE)) {
    return false;
  }

  switch (lcache->type) {
    case LIGHTCACHE_TYPE_STATIC:
      return eevee_lightcache_static_load(lcache);
    default:
      return false;
  }
}

static void eevee_lightbake_readback_irradiance(LightCache *lcache)
{
  MEM_SAFE_FREE(lcache->grid_tx.data);
  lcache->grid_tx.data = GPU_texture_read(lcache->grid_tx.tex, GPU_DATA_UBYTE, 0);
  lcache->grid_tx.data_type = LIGHTCACHETEX_BYTE;
  lcache->grid_tx.components = 4;
}

static void eevee_lightbake_readback_reflections(LightCache *lcache)
{
  MEM_SAFE_FREE(lcache->cube_tx.data);
  lcache->cube_tx.data = GPU_texture_read(lcache->cube_tx.tex, GPU_DATA_10_11_11_REV, 0);
  lcache->cube_tx.data_type = LIGHTCACHETEX_UINT;
  lcache->cube_tx.components = 1;

  for (int mip = 0; mip < lcache->mips_len; mip++) {
    LightCacheTexture *cube_mip = lcache->cube_mips + mip;
    MEM_SAFE_FREE(cube_mip->data);
    GPU_texture_get_mipmap_size(lcache->cube_tx.tex, mip + 1, cube_mip->tex_size);

    cube_mip->data = GPU_texture_read(lcache->cube_tx.tex, GPU_DATA_10_11_11_REV, mip + 1);
    cube_mip->data_type = LIGHTCACHETEX_UINT;
    cube_mip->components = 1;
  }
}

void EEVEE_lightcache_free(LightCache *lcache)
{
  DRW_TEXTURE_FREE_SAFE(lcache->cube_tx.tex);
  MEM_SAFE_FREE(lcache->cube_tx.data);
  DRW_TEXTURE_FREE_SAFE(lcache->grid_tx.tex);
  MEM_SAFE_FREE(lcache->grid_tx.data);

  if (lcache->cube_mips) {
    for (int i = 0; i < lcache->mips_len; i++) {
      MEM_SAFE_FREE(lcache->cube_mips[i].data);
    }
    MEM_SAFE_FREE(lcache->cube_mips);
  }

  MEM_SAFE_FREE(lcache->cube_data);
  MEM_SAFE_FREE(lcache->grid_data);
  MEM_freeN(lcache);
}

static void write_lightcache_texture(BlendWriter *writer, LightCacheTexture *tex)
{
  if (tex->data) {
    size_t data_size = tex->components * tex->tex_size[0] * tex->tex_size[1] * tex->tex_size[2];
    if (tex->data_type == LIGHTCACHETEX_FLOAT) {
      data_size *= sizeof(float);
    }
    else if (tex->data_type == LIGHTCACHETEX_UINT) {
      data_size *= sizeof(uint);
    }

    /* FIXME: We can't save more than what 32bit systems can handle.
     * The solution would be to split the texture but it is too late for 2.90. (see T78529) */
    if (data_size < INT_MAX) {
      BLO_write_raw(writer, data_size, tex->data);
    }
  }
}

void EEVEE_lightcache_blend_write(BlendWriter *writer, LightCache *cache)
{
  write_lightcache_texture(writer, &cache->grid_tx);
  write_lightcache_texture(writer, &cache->cube_tx);

  if (cache->cube_mips) {
    BLO_write_struct_array(writer, LightCacheTexture, cache->mips_len, cache->cube_mips);
    for (int i = 0; i < cache->mips_len; i++) {
      write_lightcache_texture(writer, &cache->cube_mips[i]);
    }
  }

  BLO_write_struct_array(writer, LightGridCache, cache->grid_len, cache->grid_data);
  BLO_write_struct_array(writer, LightProbeCache, cache->cube_len, cache->cube_data);
}

static void direct_link_lightcache_texture(BlendDataReader *reader, LightCacheTexture *lctex)
{
  lctex->tex = NULL;

  if (lctex->data) {
    BLO_read_data_address(reader, &lctex->data);
    if (lctex->data && BLO_read_requires_endian_switch(reader)) {
      int data_size = lctex->components * lctex->tex_size[0] * lctex->tex_size[1] *
                      lctex->tex_size[2];

      if (lctex->data_type == LIGHTCACHETEX_FLOAT) {
        BLI_endian_switch_float_array((float *)lctex->data, data_size * sizeof(float));
      }
      else if (lctex->data_type == LIGHTCACHETEX_UINT) {
        BLI_endian_switch_uint32_array((uint *)lctex->data, data_size * sizeof(uint));
      }
    }
  }

  if (lctex->data == NULL) {
    zero_v3_int(lctex->tex_size);
  }
}

void EEVEE_lightcache_blend_read_data(BlendDataReader *reader, LightCache *cache)
{
  cache->flag &= ~LIGHTCACHE_NOT_USABLE;
  direct_link_lightcache_texture(reader, &cache->cube_tx);
  direct_link_lightcache_texture(reader, &cache->grid_tx);

  if (cache->cube_mips) {
    BLO_read_data_address(reader, &cache->cube_mips);
    for (int i = 0; i < cache->mips_len; i++) {
      direct_link_lightcache_texture(reader, &cache->cube_mips[i]);
    }
  }

  BLO_read_data_address(reader, &cache->cube_data);
  BLO_read_data_address(reader, &cache->grid_data);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Light Bake Context
 * \{ */

static void eevee_lightbake_context_enable(EEVEE_LightBake *lbake)
{
  if (GPU_use_main_context_workaround() && !BLI_thread_is_main()) {
    GPU_context_main_lock();
    DRW_opengl_context_enable();
    return;
  }

  if (lbake->gl_context) {
    DRW_opengl_render_context_enable(lbake->gl_context);
    if (lbake->gpu_context == NULL) {
      lbake->gpu_context = GPU_context_create(NULL);
    }
    DRW_gpu_render_context_enable(lbake->gpu_context);
  }
  else {
    DRW_opengl_context_enable();
  }
}

static void eevee_lightbake_context_disable(EEVEE_LightBake *lbake)
{
  if (GPU_use_main_context_workaround() && !BLI_thread_is_main()) {
    DRW_opengl_context_disable();
    GPU_context_main_unlock();
    return;
  }

  if (lbake->gl_context) {
    DRW_gpu_render_context_disable(lbake->gpu_context);
    DRW_opengl_render_context_disable(lbake->gl_context);
  }
  else {
    DRW_opengl_context_disable();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Light Bake Job
 * \{ */

static void eevee_lightbake_count_probes(EEVEE_LightBake *lbake)
{
  Depsgraph *depsgraph = lbake->depsgraph;

  /* At least one of each for the world */
  lbake->grid_len = lbake->cube_len = lbake->total_irr_samples = 1;

  DEG_OBJECT_ITER_FOR_RENDER_ENGINE_BEGIN (depsgraph, ob) {
    const int ob_visibility = BKE_object_visibility(ob, DAG_EVAL_RENDER);
    if ((ob_visibility & OB_VISIBLE_SELF) == 0) {
      continue;
    }

    if (ob->type == OB_LIGHTPROBE) {
      LightProbe *prb = (LightProbe *)ob->data;

      if (prb->type == LIGHTPROBE_TYPE_GRID) {
        lbake->total_irr_samples += prb->grid_resolution_x * prb->grid_resolution_y *
                                    prb->grid_resolution_z;
        lbake->grid_len++;
      }
      else if (prb->type == LIGHTPROBE_TYPE_CUBE && lbake->cube_len < EEVEE_PROBE_MAX) {
        lbake->cube_len++;
      }
    }
  }
  DEG_OBJECT_ITER_FOR_RENDER_ENGINE_END;
}

static void eevee_lightbake_create_render_target(EEVEE_LightBake *lbake, int rt_res)
{
  lbake->rt_depth = DRW_texture_create_cube(rt_res, GPU_DEPTH_COMPONENT24, 0, NULL);
  lbake->rt_color = DRW_texture_create_cube(
      rt_res, GPU_RGBA16F, DRW_TEX_FILTER | DRW_TEX_MIPMAP, NULL);

  for (int i = 0; i < 6; i++) {
    GPU_framebuffer_ensure_config(&lbake->rt_fb[i],
                                  {GPU_ATTACHMENT_TEXTURE_CUBEFACE(lbake->rt_depth, i),
                                   GPU_ATTACHMENT_TEXTURE_CUBEFACE(lbake->rt_color, i)});
  }

  GPU_framebuffer_ensure_config(&lbake->store_fb, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_NONE});
}

static void eevee_lightbake_create_resources(EEVEE_LightBake *lbake)
{
  Scene *scene_eval = DEG_get_evaluated_scene(lbake->depsgraph);
  SceneEEVEE *eevee = &scene_eval->eevee;

  lbake->bounce_len = eevee->gi_diffuse_bounces;
  lbake->vis_res = eevee->gi_visibility_resolution;
  lbake->rt_res = eevee->gi_cubemap_resolution;

  irradiance_pool_size_get(lbake->vis_res, lbake->total_irr_samples, lbake->irr_size);

  lbake->ref_cube_res = lbake->rt_res;
  lbake->cube_prb = MEM_callocN(sizeof(LightProbe *) * lbake->cube_len, "EEVEE Cube visgroup ptr");
  lbake->grid_prb = MEM_callocN(sizeof(LightProbe *) * lbake->grid_len, "EEVEE Grid visgroup ptr");

  lbake->grid_prev = DRW_texture_create_2d_array(lbake->irr_size[0],
                                                 lbake->irr_size[1],
                                                 lbake->irr_size[2],
                                                 IRRADIANCE_FORMAT,
                                                 DRW_TEX_FILTER,
                                                 NULL);

  /* Ensure Light Cache is ready to accept new data. If not recreate one.
   * WARNING: All the following must be threadsafe. It's currently protected
   * by the DRW mutex. */
  lbake->lcache = eevee->light_cache_data;

  /* TODO: validate irradiance and reflection cache independently... */
  if (!EEVEE_lightcache_validate(
          lbake->lcache, lbake->cube_len, lbake->ref_cube_res, lbake->grid_len, lbake->irr_size)) {
    eevee->light_cache_data = lbake->lcache = NULL;
  }

  if (lbake->lcache == NULL) {
    lbake->lcache = EEVEE_lightcache_create(
        lbake->grid_len, lbake->cube_len, lbake->ref_cube_res, lbake->vis_res, lbake->irr_size);

    lbake->own_light_cache = true;

    eevee->light_cache_data = lbake->lcache;
  }

  EEVEE_lightcache_load(eevee->light_cache_data);

  lbake->lcache->flag |= LIGHTCACHE_BAKING;
  lbake->lcache->cube_len = 1;
}

wmJob *EEVEE_lightbake_job_create(struct wmWindowManager *wm,
                                  struct wmWindow *win,
                                  struct Main *bmain,
                                  struct ViewLayer *view_layer,
                                  struct Scene *scene,
                                  int delay,
                                  int frame)
{
  EEVEE_LightBake *lbake = NULL;

  /* only one render job at a time */
  if (WM_jobs_test(wm, scene, WM_JOB_TYPE_RENDER)) {
    return NULL;
  }

  wmJob *wm_job = WM_jobs_get(wm,
                              win,
                              scene,
                              "Bake Lighting",
                              WM_JOB_EXCL_RENDER | WM_JOB_PRIORITY | WM_JOB_PROGRESS,
                              WM_JOB_TYPE_LIGHT_BAKE);

  /* If job exists do not recreate context and depsgraph. */
  EEVEE_LightBake *old_lbake = (EEVEE_LightBake *)WM_jobs_customdata_get(wm_job);

  if (old_lbake && (old_lbake->view_layer_input == view_layer) && (old_lbake->bmain == bmain)) {
    lbake = MEM_callocN(sizeof(EEVEE_LightBake), "EEVEE_LightBake");
    /* Cannot reuse depsgraph for now because we cannot get the update from the
     * main database directly. TODO: reuse depsgraph and only update positions. */
    /* lbake->depsgraph = old_lbake->depsgraph; */
    lbake->depsgraph = DEG_graph_new(bmain, scene, view_layer, DAG_EVAL_RENDER);

    lbake->mutex = BLI_mutex_alloc();

    BLI_mutex_lock(old_lbake->mutex);
    old_lbake->own_resources = false;

    lbake->scene = scene;
    lbake->bmain = bmain;
    lbake->view_layer_input = view_layer;
    lbake->gl_context = old_lbake->gl_context;
    lbake->own_resources = true;
    lbake->delay = delay;
    lbake->frame = frame;

    if (lbake->gl_context == NULL && !GPU_use_main_context_workaround()) {
      lbake->gl_context = WM_opengl_context_create();
      wm_window_reset_drawable();
    }

    if (old_lbake->stop != NULL) {
      *old_lbake->stop = 1;
    }
    BLI_mutex_unlock(old_lbake->mutex);
  }
  else {
    lbake = EEVEE_lightbake_job_data_alloc(bmain, view_layer, scene, true, frame);
    lbake->delay = delay;
  }

  WM_jobs_customdata_set(wm_job, lbake, EEVEE_lightbake_job_data_free);
  WM_jobs_timer(wm_job, 0.4, NC_SCENE | NA_EDITED, 0);
  WM_jobs_callbacks(
      wm_job, EEVEE_lightbake_job, NULL, EEVEE_lightbake_update, EEVEE_lightbake_update);

  G.is_break = false;

  return wm_job;
}

void *EEVEE_lightbake_job_data_alloc(struct Main *bmain,
                                     struct ViewLayer *view_layer,
                                     struct Scene *scene,
                                     bool run_as_job,
                                     int frame)
{
  BLI_assert(BLI_thread_is_main());

  EEVEE_LightBake *lbake = MEM_callocN(sizeof(EEVEE_LightBake), "EEVEE_LightBake");

  lbake->depsgraph = DEG_graph_new(bmain, scene, view_layer, DAG_EVAL_RENDER);
  lbake->scene = scene;
  lbake->bmain = bmain;
  lbake->view_layer_input = view_layer;
  lbake->own_resources = true;
  lbake->own_light_cache = false;
  lbake->mutex = BLI_mutex_alloc();
  lbake->frame = frame;

  if (run_as_job && !GPU_use_main_context_workaround()) {
    lbake->gl_context = WM_opengl_context_create();
    wm_window_reset_drawable();
  }

  return lbake;
}

void EEVEE_lightbake_job_data_free(void *custom_data)
{
  EEVEE_LightBake *lbake = (EEVEE_LightBake *)custom_data;

  /* TODO: reuse depsgraph. */
  /* if (lbake->own_resources) { */
  DEG_graph_free(lbake->depsgraph);
  /* } */

  MEM_SAFE_FREE(lbake->cube_prb);
  MEM_SAFE_FREE(lbake->grid_prb);

  BLI_mutex_free(lbake->mutex);

  MEM_freeN(lbake);
}

static void eevee_lightbake_delete_resources(EEVEE_LightBake *lbake)
{
  if (!lbake->resource_only) {
    BLI_mutex_lock(lbake->mutex);
  }

  if (lbake->gl_context) {
    DRW_opengl_render_context_enable(lbake->gl_context);
    DRW_gpu_render_context_enable(lbake->gpu_context);
  }
  else if (!lbake->resource_only) {
    DRW_opengl_context_enable();
  }

  /* XXX Free the resources contained in the viewlayer data
   * to be able to free the context before deleting the depsgraph. */
  if (lbake->sldata) {
    EEVEE_view_layer_data_free(lbake->sldata);
  }

  DRW_TEXTURE_FREE_SAFE(lbake->rt_depth);
  DRW_TEXTURE_FREE_SAFE(lbake->rt_color);
  DRW_TEXTURE_FREE_SAFE(lbake->grid_prev);
  GPU_FRAMEBUFFER_FREE_SAFE(lbake->store_fb);
  for (int i = 0; i < 6; i++) {
    GPU_FRAMEBUFFER_FREE_SAFE(lbake->rt_fb[i]);
  }

  if (lbake->gpu_context) {
    DRW_gpu_render_context_disable(lbake->gpu_context);
    DRW_gpu_render_context_enable(lbake->gpu_context);
    GPU_context_discard(lbake->gpu_context);
  }

  if (lbake->gl_context && lbake->own_resources) {
    /* Delete the baking context. */
    DRW_opengl_render_context_disable(lbake->gl_context);
    WM_opengl_context_dispose(lbake->gl_context);
    lbake->gpu_context = NULL;
    lbake->gl_context = NULL;
  }
  else if (lbake->gl_context) {
    DRW_opengl_render_context_disable(lbake->gl_context);
  }
  else if (!lbake->resource_only) {
    DRW_opengl_context_disable();
  }

  if (!lbake->resource_only) {
    BLI_mutex_unlock(lbake->mutex);
  }
}

/* Cache as in draw cache not light cache. */
static void eevee_lightbake_cache_create(EEVEE_Data *vedata, EEVEE_LightBake *lbake)
{
  EEVEE_TextureList *txl = vedata->txl;
  EEVEE_StorageList *stl = vedata->stl;
  EEVEE_FramebufferList *fbl = vedata->fbl;
  EEVEE_ViewLayerData *sldata = EEVEE_view_layer_data_ensure();
  Scene *scene_eval = DEG_get_evaluated_scene(lbake->depsgraph);
  lbake->sldata = sldata;

  /* Disable all effects BUT high bit-depth shadows. */
  scene_eval->eevee.flag &= SCE_EEVEE_SHADOW_HIGH_BITDEPTH;
  scene_eval->eevee.taa_samples = 1;
  scene_eval->eevee.gi_irradiance_smoothing = 0.0f;

  stl->g_data = MEM_callocN(sizeof(*stl->g_data), __func__);
  stl->g_data->background_alpha = 1.0f;
  stl->g_data->render_timesteps = 1;

  /* XXX TODO: remove this. This is in order to make the init functions work. */
  if (DRW_view_default_get() == NULL) {
    float winmat[4][4], viewmat[4][4];
    unit_m4(viewmat);
    unit_m4(winmat);
    negate_v3(winmat[2]);
    DRWView *view = DRW_view_create(viewmat, winmat, NULL, NULL, NULL);
    DRW_view_default_set(view);
    DRW_view_set_active(view);
  }

  /* HACK: set txl->color but unset it before Draw Manager frees it. */
  txl->color = lbake->rt_color;
  const int viewport_size[2] = {
      GPU_texture_width(txl->color),
      GPU_texture_height(txl->color),
  };
  DRW_render_viewport_size_set(viewport_size);

  EEVEE_effects_init(sldata, vedata, NULL, true);
  EEVEE_materials_init(sldata, vedata, stl, fbl);
  EEVEE_shadows_init(sldata);
  EEVEE_lightprobes_init(sldata, vedata);

  EEVEE_effects_cache_init(sldata, vedata);
  EEVEE_materials_cache_init(sldata, vedata);
  EEVEE_subsurface_cache_init(sldata, vedata);
  EEVEE_volumes_cache_init(sldata, vedata);
  EEVEE_lights_cache_init(sldata, vedata);
  EEVEE_lightprobes_cache_init(sldata, vedata);

  EEVEE_lightbake_cache_init(sldata, vedata, lbake->rt_color, lbake->rt_depth);

  if (lbake->probe) {
    EEVEE_LightProbesInfo *pinfo = sldata->probes;
    LightProbe *prb = *lbake->probe;
    pinfo->vis_data.collection = prb->visibility_grp;
    pinfo->vis_data.invert = prb->flag & LIGHTPROBE_FLAG_INVERT_GROUP;
    pinfo->vis_data.cached = false;
  }
  DRW_render_object_iter(vedata, NULL, lbake->depsgraph, EEVEE_render_cache);

  EEVEE_volumes_cache_finish(sldata, vedata);
  EEVEE_materials_cache_finish(sldata, vedata);
  EEVEE_lights_cache_finish(sldata, vedata);
  EEVEE_lightprobes_cache_finish(sldata, vedata);
  EEVEE_shadows_update(sldata, vedata);

  /* Disable volumetrics when baking. */
  stl->effects->enabled_effects &= ~EFFECT_VOLUMETRIC;

  EEVEE_subsurface_draw_init(sldata, vedata);
  EEVEE_effects_draw_init(sldata, vedata);
  EEVEE_volumes_draw_init(sldata, vedata);

  txl->color = NULL;

  DRW_render_instance_buffer_finish();
  DRW_curves_update();
}

static void eevee_lightbake_copy_irradiance(EEVEE_LightBake *lbake, LightCache *lcache)
{
  DRW_TEXTURE_FREE_SAFE(lbake->grid_prev);

  /* Copy texture by reading back and re-uploading it. */
  float *tex = GPU_texture_read(lcache->grid_tx.tex, GPU_DATA_FLOAT, 0);
  lbake->grid_prev = DRW_texture_create_2d_array(lbake->irr_size[0],
                                                 lbake->irr_size[1],
                                                 lbake->irr_size[2],
                                                 IRRADIANCE_FORMAT,
                                                 DRW_TEX_FILTER,
                                                 tex);

  MEM_freeN(tex);
}

static void eevee_lightbake_render_world_sample(void *ved, void *user_data)
{
  EEVEE_Data *vedata = (EEVEE_Data *)ved;
  EEVEE_ViewLayerData *sldata = EEVEE_view_layer_data_ensure();
  EEVEE_LightBake *lbake = (EEVEE_LightBake *)user_data;
  Scene *scene_eval = DEG_get_evaluated_scene(lbake->depsgraph);
  LightCache *lcache = scene_eval->eevee.light_cache_data;
  float clamp = scene_eval->eevee.gi_glossy_clamp;
  float filter_quality = scene_eval->eevee.gi_filter_quality;

  /* TODO: do this once for the whole bake when we have independent DRWManagers. */
  eevee_lightbake_cache_create(vedata, lbake);

  sldata->common_data.ray_type = EEVEE_RAY_GLOSSY;
  sldata->common_data.ray_depth = 1;
  GPU_uniformbuf_update(sldata->common_ubo, &sldata->common_data);
  EEVEE_lightbake_render_world(sldata, vedata, lbake->rt_fb);
  EEVEE_lightbake_filter_glossy(sldata,
                                vedata,
                                lbake->rt_color,
                                lbake->store_fb,
                                0,
                                1.0f,
                                lcache->mips_len,
                                filter_quality,
                                clamp);

  sldata->common_data.ray_type = EEVEE_RAY_DIFFUSE;
  sldata->common_data.ray_depth = 1;
  GPU_uniformbuf_update(sldata->common_ubo, &sldata->common_data);
  EEVEE_lightbake_render_world(sldata, vedata, lbake->rt_fb);
  EEVEE_lightbake_filter_diffuse(sldata, vedata, lbake->rt_color, lbake->store_fb, 0, 1.0f);

  if (lcache->flag & LIGHTCACHE_UPDATE_GRID) {
    /* Clear the cache to avoid white values in the grid. */
    GPU_framebuffer_texture_attach(lbake->store_fb, lbake->grid_prev, 0, 0);
    GPU_framebuffer_bind(lbake->store_fb);
    /* Clear to 1.0f for visibility. */
    GPU_framebuffer_clear_color(lbake->store_fb, ((float[4]){1.0f, 1.0f, 1.0f, 1.0f}));
    DRW_draw_pass(vedata->psl->probe_grid_fill);

    SWAP(GPUTexture *, lbake->grid_prev, lcache->grid_tx.tex);

    /* Make a copy for later. */
    eevee_lightbake_copy_irradiance(lbake, lcache);
  }

  lcache->cube_len = 1;
  lcache->grid_len = lbake->grid_len;

  lcache->flag |= LIGHTCACHE_CUBE_READY | LIGHTCACHE_GRID_READY;
  lcache->flag &= ~LIGHTCACHE_UPDATE_WORLD;
}

static void cell_id_to_grid_loc(EEVEE_LightGrid *egrid, int cell_idx, int r_local_cell[3])
{
  /* Keep in sync with lightprobe_grid_display_vert */
  r_local_cell[2] = cell_idx % egrid->resolution[2];
  r_local_cell[1] = (cell_idx / egrid->resolution[2]) % egrid->resolution[1];
  r_local_cell[0] = cell_idx / (egrid->resolution[2] * egrid->resolution[1]);
}

static void compute_cell_id(EEVEE_LightGrid *egrid,
                            LightProbe *probe,
                            int cell_idx,
                            int *r_final_idx,
                            int r_local_cell[3],
                            int *r_stride)
{
  const int cell_count = probe->grid_resolution_x * probe->grid_resolution_y *
                         probe->grid_resolution_z;

  /* Add one for level 0 */
  int max_lvl = (int)floorf(log2f(
      (float)MAX3(probe->grid_resolution_x, probe->grid_resolution_y, probe->grid_resolution_z)));

  int visited_cells = 0;
  *r_stride = 0;
  *r_final_idx = 0;
  r_local_cell[0] = r_local_cell[1] = r_local_cell[2] = 0;
  for (int lvl = max_lvl; lvl >= 0; lvl--) {
    *r_stride = 1 << lvl;
    int prev_stride = *r_stride << 1;
    for (int i = 0; i < cell_count; i++) {
      *r_final_idx = i;
      cell_id_to_grid_loc(egrid, *r_final_idx, r_local_cell);
      if (((r_local_cell[0] % *r_stride) == 0) && ((r_local_cell[1] % *r_stride) == 0) &&
          ((r_local_cell[2] % *r_stride) == 0)) {
        if (!(((r_local_cell[0] % prev_stride) == 0) && ((r_local_cell[1] % prev_stride) == 0) &&
              ((r_local_cell[2] % prev_stride) == 0)) ||
            ((i == 0) && (lvl == max_lvl))) {
          if (visited_cells == cell_idx) {
            return;
          }

          visited_cells++;
        }
      }
    }
  }

  BLI_assert(0);
}

static void grid_loc_to_world_loc(EEVEE_LightGrid *egrid, const int local_cell[3], float r_pos[3])
{
  copy_v3_v3(r_pos, egrid->corner);
  madd_v3_v3fl(r_pos, egrid->increment_x, local_cell[0]);
  madd_v3_v3fl(r_pos, egrid->increment_y, local_cell[1]);
  madd_v3_v3fl(r_pos, egrid->increment_z, local_cell[2]);
}

static void eevee_lightbake_render_grid_sample(void *ved, void *user_data)
{
  EEVEE_Data *vedata = (EEVEE_Data *)ved;
  EEVEE_ViewLayerData *sldata = EEVEE_view_layer_data_ensure();
  EEVEE_CommonUniformBuffer *common_data = &sldata->common_data;
  EEVEE_LightBake *lbake = (EEVEE_LightBake *)user_data;
  EEVEE_LightGrid *egrid = lbake->grid;
  LightProbe *prb = *lbake->probe;
  Scene *scene_eval = DEG_get_evaluated_scene(lbake->depsgraph);
  LightCache *lcache = scene_eval->eevee.light_cache_data;
  int grid_loc[3], sample_id, sample_offset, stride;
  float pos[3];
  const bool is_last_bounce_sample = ((egrid->offset + lbake->grid_sample) ==
                                      (lbake->total_irr_samples - 1));

  /* No bias for rendering the probe. */
  egrid->level_bias = 1.0f;

  /* Use the previous bounce for rendering this bounce. */
  SWAP(GPUTexture *, lbake->grid_prev, lcache->grid_tx.tex);

  /* TODO: do this once for the whole bake when we have independent DRWManagers.
   * WARNING: Some of the things above require this. */
  eevee_lightbake_cache_create(vedata, lbake);

  /* Compute sample position */
  compute_cell_id(egrid, prb, lbake->grid_sample, &sample_id, grid_loc, &stride);
  sample_offset = egrid->offset + sample_id;

  grid_loc_to_world_loc(egrid, grid_loc, pos);

  /* Disable specular lighting when rendering probes to avoid feedback loops (looks bad). */
  common_data->spec_toggle = false;
  common_data->sss_toggle = false;
  common_data->prb_num_planar = 0;
  common_data->prb_num_render_cube = 0;
  common_data->ray_type = EEVEE_RAY_DIFFUSE;
  common_data->ray_depth = lbake->bounce_curr + 1;
  if (lbake->bounce_curr == 0) {
    common_data->prb_num_render_grid = 0;
  }
  GPU_uniformbuf_update(sldata->common_ubo, &sldata->common_data);

  EEVEE_lightbake_render_scene(sldata, vedata, lbake->rt_fb, pos, prb->clipsta, prb->clipend);

  /* Restore before filtering. */
  SWAP(GPUTexture *, lbake->grid_prev, lcache->grid_tx.tex);

  EEVEE_lightbake_filter_diffuse(
      sldata, vedata, lbake->rt_color, lbake->store_fb, sample_offset, prb->intensity);

  if (lbake->bounce_curr == 0) {
    /* We only need to filter the visibility for the first bounce. */
    EEVEE_lightbake_filter_visibility(sldata,
                                      vedata,
                                      lbake->rt_depth,
                                      lbake->store_fb,
                                      sample_offset,
                                      prb->clipsta,
                                      prb->clipend,
                                      egrid->visibility_range,
                                      prb->vis_blur,
                                      lbake->vis_res);
  }

  /* Update level for progressive update. */
  if (is_last_bounce_sample) {
    egrid->level_bias = 1.0f;
  }
  else if (lbake->bounce_curr == 0) {
    egrid->level_bias = (float)(stride << 1);
  }

  /* Only run this for the last sample of a bounce. */
  if (is_last_bounce_sample) {
    eevee_lightbake_copy_irradiance(lbake, lcache);
  }

  /* If it is the last sample grid sample (and last bounce). */
  if ((lbake->bounce_curr == lbake->bounce_len - 1) && (lbake->grid_curr == lbake->grid_len - 1) &&
      (lbake->grid_sample == lbake->grid_sample_len - 1)) {
    lcache->flag &= ~LIGHTCACHE_UPDATE_GRID;
  }
}

static void eevee_lightbake_render_probe_sample(void *ved, void *user_data)
{
  EEVEE_Data *vedata = (EEVEE_Data *)ved;
  EEVEE_ViewLayerData *sldata = EEVEE_view_layer_data_ensure();
  EEVEE_CommonUniformBuffer *common_data = &sldata->common_data;
  EEVEE_LightBake *lbake = (EEVEE_LightBake *)user_data;
  Scene *scene_eval = DEG_get_evaluated_scene(lbake->depsgraph);
  LightCache *lcache = scene_eval->eevee.light_cache_data;
  EEVEE_LightProbe *eprobe = lbake->cube;
  LightProbe *prb = *lbake->probe;
  float clamp = scene_eval->eevee.gi_glossy_clamp;
  float filter_quality = scene_eval->eevee.gi_filter_quality;

  /* TODO: do this once for the whole bake when we have independent DRWManagers. */
  eevee_lightbake_cache_create(vedata, lbake);

  /* Disable specular lighting when rendering probes to avoid feedback loops (looks bad). */
  common_data->spec_toggle = false;
  common_data->sss_toggle = false;
  common_data->prb_num_planar = 0;
  common_data->prb_num_render_cube = 0;
  common_data->ray_type = EEVEE_RAY_GLOSSY;
  common_data->ray_depth = 1;
  GPU_uniformbuf_update(sldata->common_ubo, &sldata->common_data);

  EEVEE_lightbake_render_scene(
      sldata, vedata, lbake->rt_fb, eprobe->position, prb->clipsta, prb->clipend);
  EEVEE_lightbake_filter_glossy(sldata,
                                vedata,
                                lbake->rt_color,
                                lbake->store_fb,
                                lbake->cube_offset,
                                prb->intensity,
                                lcache->mips_len,
                                filter_quality,
                                clamp);

  lcache->cube_len += 1;

  /* If it's the last probe. */
  if (lbake->cube_offset == lbake->cube_len - 1) {
    lcache->flag &= ~LIGHTCACHE_UPDATE_CUBE;
  }
}

static float eevee_lightbake_grid_influence_volume(EEVEE_LightGrid *grid)
{
  return mat4_to_scale(grid->mat);
}

static float eevee_lightbake_cube_influence_volume(EEVEE_LightProbe *eprb)
{
  return mat4_to_scale(eprb->attenuationmat);
}

static bool eevee_lightbake_grid_comp(EEVEE_LightGrid *grid_a, EEVEE_LightGrid *grid_b)
{
  float vol_a = eevee_lightbake_grid_influence_volume(grid_a);
  float vol_b = eevee_lightbake_grid_influence_volume(grid_b);
  return (vol_a < vol_b);
}

static bool eevee_lightbake_cube_comp(EEVEE_LightProbe *prb_a, EEVEE_LightProbe *prb_b)
{
  float vol_a = eevee_lightbake_cube_influence_volume(prb_a);
  float vol_b = eevee_lightbake_cube_influence_volume(prb_b);
  return (vol_a < vol_b);
}

#define SORT_PROBE(elems_type, prbs, elems, elems_len, comp_fn) \
  { \
    bool sorted = false; \
    while (!sorted) { \
      sorted = true; \
      for (int i = 0; i < (elems_len)-1; i++) { \
        if ((comp_fn)((elems) + i, (elems) + i + 1)) { \
          SWAP(elems_type, (elems)[i], (elems)[i + 1]); \
          SWAP(LightProbe *, (prbs)[i], (prbs)[i + 1]); \
          sorted = false; \
        } \
      } \
    } \
  } \
  ((void)0)

static void eevee_lightbake_gather_probes(EEVEE_LightBake *lbake)
{
  Depsgraph *depsgraph = lbake->depsgraph;
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  LightCache *lcache = scene_eval->eevee.light_cache_data;

  /* At least one for the world */
  int grid_len = 1;
  int cube_len = 1;
  int total_irr_samples = 1;

  /* Convert all lightprobes to tight UBO data from all lightprobes in the scene.
   * This allows a large number of probe to be precomputed (even dupli ones). */
  DEG_OBJECT_ITER_FOR_RENDER_ENGINE_BEGIN (depsgraph, ob) {
    const int ob_visibility = BKE_object_visibility(ob, DAG_EVAL_RENDER);
    if ((ob_visibility & OB_VISIBLE_SELF) == 0) {
      continue;
    }

    if (ob->type == OB_LIGHTPROBE) {
      LightProbe *prb = (LightProbe *)ob->data;

      if (prb->type == LIGHTPROBE_TYPE_GRID) {
        lbake->grid_prb[grid_len] = prb;
        EEVEE_LightGrid *egrid = &lcache->grid_data[grid_len++];
        EEVEE_lightprobes_grid_data_from_object(ob, egrid, &total_irr_samples);
      }
      else if (prb->type == LIGHTPROBE_TYPE_CUBE && cube_len < EEVEE_PROBE_MAX) {
        lbake->cube_prb[cube_len] = prb;
        EEVEE_LightProbe *eprobe = &lcache->cube_data[cube_len++];
        EEVEE_lightprobes_cube_data_from_object(ob, eprobe);
      }
    }
  }
  DEG_OBJECT_ITER_FOR_RENDER_ENGINE_END;

  SORT_PROBE(EEVEE_LightGrid,
             lbake->grid_prb + 1,
             lcache->grid_data + 1,
             lbake->grid_len - 1,
             eevee_lightbake_grid_comp);
  SORT_PROBE(EEVEE_LightProbe,
             lbake->cube_prb + 1,
             lcache->cube_data + 1,
             lbake->cube_len - 1,
             eevee_lightbake_cube_comp);

  lbake->total = lbake->total_irr_samples * lbake->bounce_len + lbake->cube_len;
  lbake->done = 0;
}

void EEVEE_lightbake_update(void *custom_data)
{
  EEVEE_LightBake *lbake = (EEVEE_LightBake *)custom_data;
  Scene *scene_orig = lbake->scene;

  /* If a new light-cache was created, free the old one and reference the new. */
  if (lbake->lcache && scene_orig->eevee.light_cache_data != lbake->lcache) {
    if (scene_orig->eevee.light_cache_data != NULL) {
      EEVEE_lightcache_free(scene_orig->eevee.light_cache_data);
    }
    scene_orig->eevee.light_cache_data = lbake->lcache;
    lbake->own_light_cache = false;
  }

  EEVEE_lightcache_info_update(&lbake->scene->eevee);

  DEG_id_tag_update(&scene_orig->id, ID_RECALC_COPY_ON_WRITE);
}

static bool lightbake_do_sample(EEVEE_LightBake *lbake,
                                void (*render_callback)(void *ved, void *user_data))
{
  if (G.is_break == true || *lbake->stop) {
    return false;
  }

  Depsgraph *depsgraph = lbake->depsgraph;

  /* TODO: make DRW manager instantiable (and only lock on drawing) */
  eevee_lightbake_context_enable(lbake);
  DRW_custom_pipeline(&draw_engine_eevee_type, depsgraph, render_callback, lbake);
  lbake->done += 1;
  *lbake->progress = lbake->done / (float)lbake->total;
  *lbake->do_update = 1;
  eevee_lightbake_context_disable(lbake);

  return true;
}

void EEVEE_lightbake_job(void *custom_data, short *stop, short *do_update, float *progress)
{
  EEVEE_LightBake *lbake = (EEVEE_LightBake *)custom_data;
  Depsgraph *depsgraph = lbake->depsgraph;

  DEG_graph_relations_update(depsgraph);
  DEG_evaluate_on_framechange(depsgraph, lbake->frame);

  lbake->view_layer = DEG_get_evaluated_view_layer(depsgraph);
  lbake->stop = stop;
  lbake->do_update = do_update;
  lbake->progress = progress;

  if (G.background) {
    /* Make sure to init GL capabilities before counting probes. */
    eevee_lightbake_context_enable(lbake);
    eevee_lightbake_context_disable(lbake);
  }

  /* Count lightprobes */
  eevee_lightbake_count_probes(lbake);

  /* We need to create the FBOs in the right context.
   * We cannot do it in the main thread. */
  eevee_lightbake_context_enable(lbake);
  eevee_lightbake_create_resources(lbake);

  /* Resource allocation can fail. Early exit in this case. */
  if (lbake->lcache->flag & LIGHTCACHE_INVALID) {
    *lbake->stop = 1;
    *lbake->do_update = 1;
    lbake->lcache->flag &= ~LIGHTCACHE_BAKING;
    eevee_lightbake_context_disable(lbake);
    eevee_lightbake_delete_resources(lbake);
    return;
  }

  eevee_lightbake_create_render_target(lbake, lbake->rt_res);
  eevee_lightbake_context_disable(lbake);

  /* Gather all probes data */
  eevee_lightbake_gather_probes(lbake);

  LightCache *lcache = lbake->lcache;

  /* HACK: Sleep to delay the first rendering operation
   * that causes a small freeze (caused by VBO generation)
   * because this step is locking at this moment. */
  /* TODO: remove this. */
  if (lbake->delay) {
    PIL_sleep_ms(lbake->delay);
  }

  /* Render world irradiance and reflection first */
  if (lcache->flag & LIGHTCACHE_UPDATE_WORLD) {
    lbake->probe = NULL;
    lightbake_do_sample(lbake, eevee_lightbake_render_world_sample);
  }

  /* Render irradiance grids */
  if (lcache->flag & LIGHTCACHE_UPDATE_GRID) {
    for (lbake->bounce_curr = 0; lbake->bounce_curr < lbake->bounce_len; lbake->bounce_curr++) {
      /* Bypass world, start at 1. */
      lbake->probe = lbake->grid_prb + 1;
      lbake->grid = lcache->grid_data + 1;
      for (lbake->grid_curr = 1; lbake->grid_curr < lbake->grid_len;
           lbake->grid_curr++, lbake->probe++, lbake->grid++) {
        LightProbe *prb = *lbake->probe;
        lbake->grid_sample_len = prb->grid_resolution_x * prb->grid_resolution_y *
                                 prb->grid_resolution_z;
        for (lbake->grid_sample = 0; lbake->grid_sample < lbake->grid_sample_len;
             ++lbake->grid_sample) {
          lightbake_do_sample(lbake, eevee_lightbake_render_grid_sample);
        }
      }
    }
  }

  /* Render reflections */
  if (lcache->flag & LIGHTCACHE_UPDATE_CUBE) {
    /* Bypass world, start at 1. */
    lbake->probe = lbake->cube_prb + 1;
    lbake->cube = lcache->cube_data + 1;
    for (lbake->cube_offset = 1; lbake->cube_offset < lbake->cube_len;
         lbake->cube_offset++, lbake->probe++, lbake->cube++) {
      lightbake_do_sample(lbake, eevee_lightbake_render_probe_sample);
    }
  }

  /* Read the resulting lighting data to save it to file/disk. */
  eevee_lightbake_context_enable(lbake);
  eevee_lightbake_readback_irradiance(lcache);
  eevee_lightbake_readback_reflections(lcache);
  eevee_lightbake_context_disable(lbake);

  lcache->flag |= LIGHTCACHE_BAKED;
  lcache->flag &= ~LIGHTCACHE_BAKING;

  /* Assume that if lbake->gl_context is NULL
   * we are not running in this in a job, so update
   * the scene light-cache pointer before deleting it. */
  if (lbake->gl_context == NULL) {
    BLI_assert(BLI_thread_is_main());
    EEVEE_lightbake_update(lbake);
  }

  eevee_lightbake_delete_resources(lbake);
}

void EEVEE_lightbake_update_world_quick(EEVEE_ViewLayerData *sldata,
                                        EEVEE_Data *vedata,
                                        const Scene *scene)
{
  LightCache *lcache = vedata->stl->g_data->light_cache;
  float clamp = scene->eevee.gi_glossy_clamp;
  float filter_quality = scene->eevee.gi_filter_quality;

  EEVEE_LightBake lbake = {
      .resource_only = true,
  };

  /* Create resources. */
  eevee_lightbake_create_render_target(&lbake, scene->eevee.gi_cubemap_resolution);

  EEVEE_lightbake_cache_init(sldata, vedata, lbake.rt_color, lbake.rt_depth);

  sldata->common_data.ray_type = EEVEE_RAY_GLOSSY;
  sldata->common_data.ray_depth = 1;
  GPU_uniformbuf_update(sldata->common_ubo, &sldata->common_data);
  EEVEE_lightbake_render_world(sldata, vedata, lbake.rt_fb);
  EEVEE_lightbake_filter_glossy(sldata,
                                vedata,
                                lbake.rt_color,
                                lbake.store_fb,
                                0,
                                1.0f,
                                lcache->mips_len,
                                filter_quality,
                                clamp);

  sldata->common_data.ray_type = EEVEE_RAY_DIFFUSE;
  sldata->common_data.ray_depth = 1;
  GPU_uniformbuf_update(sldata->common_ubo, &sldata->common_data);
  EEVEE_lightbake_render_world(sldata, vedata, lbake.rt_fb);
  EEVEE_lightbake_filter_diffuse(sldata, vedata, lbake.rt_color, lbake.store_fb, 0, 1.0f);

  /* Don't hide grids if they are already rendered. */
  lcache->grid_len = max_ii(1, lcache->grid_len);
  lcache->cube_len = 1;

  lcache->flag |= LIGHTCACHE_CUBE_READY | LIGHTCACHE_GRID_READY;
  lcache->flag &= ~LIGHTCACHE_UPDATE_WORLD;

  eevee_lightbake_delete_resources(&lbake);
}

/** \} */
