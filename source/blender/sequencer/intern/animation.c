/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup sequencer
 */

#include <string.h>

#include "DNA_anim_types.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "BKE_animsys.h"
#include "BKE_fcurve.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_string.h"

#include "DEG_depsgraph.h"

#include "SEQ_animation.h"

static bool seq_animation_curves_exist(Scene *scene)
{
  if (scene->adt == NULL || scene->adt->action == NULL ||
      BLI_listbase_is_empty(&scene->adt->action->curves)) {
    return false;
  }
  return true;
}

/* r_prefix + [" + escaped_name + "] + \0 */
#define SEQ_RNAPATH_MAXSTR ((30 + 2 + (SEQ_NAME_MAXSTR * 2) + 2) + 1)

static size_t sequencer_rna_path_prefix(char str[SEQ_RNAPATH_MAXSTR], const char *name)
{
  char name_esc[SEQ_NAME_MAXSTR * 2];

  BLI_str_escape(name_esc, name, sizeof(name_esc));
  return BLI_snprintf_rlen(
      str, SEQ_RNAPATH_MAXSTR, "sequence_editor.sequences_all[\"%s\"]", name_esc);
}

GSet *SEQ_fcurves_by_strip_get(const Sequence *seq, ListBase *fcurve_base)
{
  char rna_path[SEQ_RNAPATH_MAXSTR];
  size_t rna_path_len = sequencer_rna_path_prefix(rna_path, seq->name + 2);

  /* Only allocate `fcurves` if it's needed as it's possible there is no animation for `seq`. */
  GSet *fcurves = NULL;
  LISTBASE_FOREACH (FCurve *, fcurve, fcurve_base) {
    if (STREQLEN(fcurve->rna_path, rna_path, rna_path_len)) {
      if (fcurves == NULL) {
        fcurves = BLI_gset_ptr_new(__func__);
      }
      BLI_gset_add(fcurves, fcurve);
    }
  }

  return fcurves;
}

#undef SEQ_RNAPATH_MAXSTR

void SEQ_offset_animdata(Scene *scene, Sequence *seq, int ofs)
{
  if (!seq_animation_curves_exist(scene) || ofs == 0) {
    return;
  }
  GSet *fcurves = SEQ_fcurves_by_strip_get(seq, &scene->adt->action->curves);
  if (fcurves == NULL) {
    return;
  }

  GSET_FOREACH_BEGIN (FCurve *, fcu, fcurves) {
    unsigned int i;
    if (fcu->bezt) {
      for (i = 0; i < fcu->totvert; i++) {
        BezTriple *bezt = &fcu->bezt[i];
        bezt->vec[0][0] += ofs;
        bezt->vec[1][0] += ofs;
        bezt->vec[2][0] += ofs;
      }
    }
    if (fcu->fpt) {
      for (i = 0; i < fcu->totvert; i++) {
        FPoint *fpt = &fcu->fpt[i];
        fpt->vec[0] += ofs;
      }
    }
  }
  GSET_FOREACH_END();
  BLI_gset_free(fcurves, NULL);

  DEG_id_tag_update(&scene->adt->action->id, ID_RECALC_ANIMATION);
}

void SEQ_free_animdata(Scene *scene, Sequence *seq)
{
  if (!seq_animation_curves_exist(scene)) {
    return;
  }
  GSet *fcurves = SEQ_fcurves_by_strip_get(seq, &scene->adt->action->curves);
  if (fcurves == NULL) {
    return;
  }

  GSET_FOREACH_BEGIN (FCurve *, fcu, fcurves) {
    BLI_remlink(&scene->adt->action->curves, fcu);
    BKE_fcurve_free(fcu);
  }
  GSET_FOREACH_END();
  BLI_gset_free(fcurves, NULL);
}

void SEQ_animation_backup_original(Scene *scene, ListBase *list)
{
  if (scene->adt == NULL || scene->adt->action == NULL ||
      BLI_listbase_is_empty(&scene->adt->action->curves)) {
    return;
  }

  BLI_movelisttolist(list, &scene->adt->action->curves);
}

void SEQ_animation_restore_original(Scene *scene, ListBase *list)
{
  if (scene->adt == NULL || scene->adt->action == NULL || BLI_listbase_is_empty(list)) {
    return;
  }

  BLI_movelisttolist(&scene->adt->action->curves, list);
}

void SEQ_animation_duplicate(Scene *scene, Sequence *seq, ListBase *list)
{
  if (BLI_listbase_is_empty(list)) {
    return;
  }

  if (seq->type == SEQ_TYPE_META) {
    LISTBASE_FOREACH (Sequence *, meta_child, &seq->seqbase) {
      SEQ_animation_duplicate(scene, meta_child, list);
    }
  }

  GSet *fcurves = SEQ_fcurves_by_strip_get(seq, list);
  if (fcurves == NULL) {
    return;
  }

  GSET_FOREACH_BEGIN (FCurve *, fcu, fcurves) {
    FCurve *fcu_cpy = BKE_fcurve_copy(fcu);
    BLI_addtail(&scene->adt->action->curves, fcu_cpy);
  }
  GSET_FOREACH_END();
  BLI_gset_free(fcurves, NULL);
}
