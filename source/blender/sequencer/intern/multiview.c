/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved.
 *           2003-2009 Blender Foundation.
 *           2005-2006 Peter Schlaile <peter [at] schlaile [dot] de> */

/** \file
 * \ingroup bke
 */

#include "DNA_scene_types.h"

#include "BLI_string.h"

#include "BKE_scene.h"

#include "IMB_imbuf.h"

#include "multiview.h"

void seq_anim_add_suffix(Scene *scene, struct anim *anim, const int view_id)
{
  const char *suffix = BKE_scene_multiview_view_id_suffix_get(&scene->r, view_id);
  IMB_suffix_anim(anim, suffix);
}

int seq_num_files(Scene *scene, char views_format, const bool is_multiview)
{
  if (!is_multiview) {
    return 1;
  }
  if (views_format == R_IMF_VIEWS_STEREO_3D) {
    return 1;
  }
  /* R_IMF_VIEWS_INDIVIDUAL */

  return BKE_scene_multiview_num_views_get(&scene->r);
}

void seq_multiview_name(Scene *scene,
                        const int view_id,
                        const char *prefix,
                        const char *ext,
                        char *r_path,
                        size_t r_size)
{
  const char *suffix = BKE_scene_multiview_view_id_suffix_get(&scene->r, view_id);
  BLI_assert(ext != NULL && suffix != NULL && prefix != NULL);
  BLI_snprintf(r_path, r_size, "%s%s%s", prefix, suffix, ext);
}
