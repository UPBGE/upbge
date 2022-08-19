/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006-2007 Blender Foundation. All rights reserved. */

#pragma once

/** \file
 * \ingroup bke
 *
 * Studio lighting for the 3dview
 */

#include "BLI_sys_types.h"

#include "BLI_path_util.h"

#include "DNA_userdef_types.h" /* for #SolidLight */

#ifdef __cplusplus
extern "C" {
#endif

struct ImBuf;

/*
 * These defines are the indexes in the StudioLight.diffuse_light
 * X_POS means the light that is traveling towards the positive X
 * So Light direction.
 */
#define STUDIOLIGHT_X_POS 0
#define STUDIOLIGHT_X_NEG 1
#define STUDIOLIGHT_Y_POS 2
#define STUDIOLIGHT_Y_NEG 3
#define STUDIOLIGHT_Z_POS 4
#define STUDIOLIGHT_Z_NEG 5

#define STUDIOLIGHT_ICON_ID_TYPE_RADIANCE (1 << 0)
#define STUDIOLIGHT_ICON_ID_TYPE_IRRADIANCE (1 << 1)
#define STUDIOLIGHT_ICON_ID_TYPE_MATCAP (1 << 2)
#define STUDIOLIGHT_ICON_ID_TYPE_MATCAP_FLIPPED (1 << 3)

#define STUDIOLIGHT_MAX_LIGHT 4

#define STUDIOLIGHT_ICON_SIZE 96

/* Only 1 - 5 is supported */
#define STUDIOLIGHT_SH_BANDS 2

#define STUDIOLIGHT_SH_COEFS_LEN (STUDIOLIGHT_SH_BANDS * STUDIOLIGHT_SH_BANDS)

#if STUDIOLIGHT_SH_BANDS > 3
/* Bypass L3 */
#  define STUDIOLIGHT_SH_EFFECTIVE_COEFS_LEN (STUDIOLIGHT_SH_COEFS_LEN - 7)
#else
#  define STUDIOLIGHT_SH_EFFECTIVE_COEFS_LEN STUDIOLIGHT_SH_COEFS_LEN
#endif

struct GPUTexture;
struct StudioLight;

/** #StudioLight.flag */
enum StudioLightFlag {
  STUDIOLIGHT_SPHERICAL_HARMONICS_COEFFICIENTS_CALCULATED = (1 << 0),
  /*  STUDIOLIGHT_LIGHT_DIRECTION_CALCULATED                  = (1 << 1), */
  STUDIOLIGHT_INTERNAL = (1 << 2),
  STUDIOLIGHT_EXTERNAL_FILE = (1 << 3),
  STUDIOLIGHT_TYPE_STUDIO = (1 << 4),
  STUDIOLIGHT_TYPE_WORLD = (1 << 5),
  STUDIOLIGHT_TYPE_MATCAP = (1 << 6),
  STUDIOLIGHT_EXTERNAL_IMAGE_LOADED = (1 << 7),
  STUDIOLIGHT_EQUIRECT_IRRADIANCE_IMAGE_CALCULATED = (1 << 8),
  STUDIOLIGHT_EQUIRECT_RADIANCE_GPUTEXTURE = (1 << 9),
  STUDIOLIGHT_EQUIRECT_IRRADIANCE_GPUTEXTURE = (1 << 10),
  STUDIOLIGHT_RADIANCE_BUFFERS_CALCULATED = (1 << 11),
  STUDIOLIGHT_USER_DEFINED = (1 << 12),
  STUDIOLIGHT_UI_EXPANDED = (1 << 13),

  STUDIOLIGHT_MATCAP_DIFFUSE_GPUTEXTURE = (1 << 14),
  STUDIOLIGHT_MATCAP_SPECULAR_GPUTEXTURE = (1 << 15),
  /* Is set for studio lights and matcaps with specular highlight pass. */
  STUDIOLIGHT_SPECULAR_HIGHLIGHT_PASS = (1 << 16),
};

#define STUDIOLIGHT_FLAG_ALL (STUDIOLIGHT_INTERNAL | STUDIOLIGHT_EXTERNAL_FILE)
#define STUDIOLIGHT_FLAG_ORIENTATIONS \
  (STUDIOLIGHT_TYPE_STUDIO | STUDIOLIGHT_TYPE_WORLD | STUDIOLIGHT_TYPE_MATCAP)
#define STUDIOLIGHT_ORIENTATIONS_MATERIAL_MODE (STUDIOLIGHT_TYPE_WORLD)
#define STUDIOLIGHT_ORIENTATIONS_SOLID (STUDIOLIGHT_INTERNAL | STUDIOLIGHT_TYPE_STUDIO)

typedef void StudioLightFreeFunction(struct StudioLight *, void *data);

typedef struct StudioLightImage {
  struct ImBuf *ibuf;
  struct GPUTexture *gputexture;
} StudioLightImage;

typedef struct StudioLight {
  struct StudioLight *next, *prev;

  int index;
  int flag;
  char name[FILE_MAXFILE];
  char path[FILE_MAX];
  char *path_irr_cache;
  char *path_sh_cache;
  int icon_id_irradiance;
  int icon_id_radiance;
  int icon_id_matcap;
  int icon_id_matcap_flipped;
  float spherical_harmonics_coefs[STUDIOLIGHT_SH_EFFECTIVE_COEFS_LEN][3];
  float light_direction[3];
  StudioLightImage matcap_diffuse;
  StudioLightImage matcap_specular;
  struct ImBuf *equirect_radiance_buffer;
  struct ImBuf *equirect_irradiance_buffer;
  struct ImBuf *radiance_cubemap_buffers[6];
  struct GPUTexture *equirect_radiance_gputexture;
  struct GPUTexture *equirect_irradiance_gputexture;
  SolidLight light[STUDIOLIGHT_MAX_LIGHT];
  float light_ambient[3];

  /*
   * Free function to clean up the running icons previews (wmJob) the usage is in
   * interface_icons. Please be aware that this was build to handle only one free function
   * that cleans up all icons. just to keep the code simple.
   */
  StudioLightFreeFunction *free_function;
  void *free_function_data;
} StudioLight;

/* API */

void BKE_studiolight_init(void);
void BKE_studiolight_free(void);
void BKE_studiolight_default(SolidLight lights[4], float light_ambient[3]);
struct StudioLight *BKE_studiolight_find(const char *name, int flag);
struct StudioLight *BKE_studiolight_findindex(int index, int flag);
struct StudioLight *BKE_studiolight_find_default(int flag);
void BKE_studiolight_preview(uint *icon_buffer, StudioLight *sl, int icon_id_type);
struct ListBase *BKE_studiolight_listbase(void);
/**
 * Ensure state of studio-lights.
 */
void BKE_studiolight_ensure_flag(StudioLight *sl, int flag);
void BKE_studiolight_refresh(void);
StudioLight *BKE_studiolight_load(const char *path, int type);
StudioLight *BKE_studiolight_create(const char *path,
                                    const SolidLight light[4],
                                    const float light_ambient[3]);
/**
 * Only useful for workbench while editing the user-preferences.
 */
StudioLight *BKE_studiolight_studio_edit_get(void);
void BKE_studiolight_remove(StudioLight *sl);
void BKE_studiolight_set_free_function(StudioLight *sl,
                                       StudioLightFreeFunction *free_function,
                                       void *data);
void BKE_studiolight_unset_icon_id(StudioLight *sl, int icon_id);

#ifdef __cplusplus
}
#endif
