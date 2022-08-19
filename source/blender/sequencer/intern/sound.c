/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved.
 *           2003-2009 Blender Foundation.
 *           2005-2006 Peter Schlaile <peter [at] schlaile [dot] de> */

/** \file
 * \ingroup bke
 */

#include <math.h>

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_sound_types.h"

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "BKE_main.h"
#include "BKE_scene.h"
#include "BKE_sound.h"

#include "SEQ_sound.h"
#include "SEQ_time.h"

#include "sequencer.h"
#include "strip_time.h"

/* Unlike _update_sound_ funcs, these ones take info from audaspace to update sequence length! */
#ifdef WITH_AUDASPACE
static bool sequencer_refresh_sound_length_recursive(Main *bmain, Scene *scene, ListBase *seqbase)
{
  Sequence *seq;
  bool changed = false;

  for (seq = seqbase->first; seq; seq = seq->next) {
    if (seq->type == SEQ_TYPE_META) {
      if (sequencer_refresh_sound_length_recursive(bmain, scene, &seq->seqbase)) {
        changed = true;
      }
    }
    else if (seq->type == SEQ_TYPE_SOUND_RAM && seq->sound) {
      SoundInfo info;
      if (!BKE_sound_info_get(bmain, seq->sound, &info)) {
        continue;
      }

      int old = seq->len;
      float fac;

      seq->len = MAX2(1, round((info.length - seq->sound->offset_time) * FPS));
      fac = (float)seq->len / (float)old;
      old = seq->startofs;
      seq->startofs *= fac;
      seq->endofs *= fac;
      seq->start += (old - seq->startofs); /* So that visual/"real" start frame does not change! */

      changed = true;
    }
  }
  return changed;
}
#endif

void SEQ_sound_update_length(Main *bmain, Scene *scene)
{
#ifdef WITH_AUDASPACE
  if (scene->ed) {
    sequencer_refresh_sound_length_recursive(bmain, scene, &scene->ed->seqbase);
  }
#else
  UNUSED_VARS(bmain, scene);
#endif
}

void SEQ_sound_update_bounds_all(Scene *scene)
{
  Editing *ed = scene->ed;

  if (ed) {
    Sequence *seq;

    for (seq = ed->seqbase.first; seq; seq = seq->next) {
      if (seq->type == SEQ_TYPE_META) {
        seq_update_sound_bounds_recursive(scene, seq);
      }
      else if (ELEM(seq->type, SEQ_TYPE_SOUND_RAM, SEQ_TYPE_SCENE)) {
        SEQ_sound_update_bounds(scene, seq);
      }
    }
  }
}

void SEQ_sound_update_bounds(Scene *scene, Sequence *seq)
{
  if (seq->type == SEQ_TYPE_SCENE) {
    if (seq->scene && seq->scene_sound) {
      /* We have to take into account start frame of the sequence's scene! */
      int startofs = seq->startofs + seq->anim_startofs + seq->scene->r.sfra;

      BKE_sound_move_scene_sound(scene,
                                 seq->scene_sound,
                                 SEQ_time_left_handle_frame_get(scene, seq),
                                 SEQ_time_right_handle_frame_get(scene, seq),
                                 startofs,
                                 0.0);
    }
  }
  else {
    BKE_sound_move_scene_sound_defaults(scene, seq);
  }
  /* mute is set in seq_update_muting_recursive */
}

static void seq_update_sound_recursive(Scene *scene, ListBase *seqbasep, bSound *sound)
{
  Sequence *seq;

  for (seq = seqbasep->first; seq; seq = seq->next) {
    if (seq->type == SEQ_TYPE_META) {
      seq_update_sound_recursive(scene, &seq->seqbase, sound);
    }
    else if (seq->type == SEQ_TYPE_SOUND_RAM) {
      if (seq->scene_sound && sound == seq->sound) {
        BKE_sound_update_scene_sound(seq->scene_sound, sound);
      }
    }
  }
}

void SEQ_sound_update(Scene *scene, bSound *sound)
{
  if (scene->ed) {
    seq_update_sound_recursive(scene, &scene->ed->seqbase, sound);
  }
}

float SEQ_sound_pitch_get(const Scene *scene, const Sequence *seq)
{
  Sequence *meta_parent = seq_sequence_lookup_meta_by_seq(scene, seq);
  if (meta_parent != NULL) {
    return seq->speed_factor * SEQ_sound_pitch_get(scene, meta_parent);
  }
  return seq->speed_factor;
}
