/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edanimation
 */

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "DNA_anim_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_action.h"
#include "BKE_curve.h"
#include "BKE_fcurve.h"
#include "BKE_main.h"
#include "BKE_report.h"

#include "RNA_access.h"
#include "RNA_enum_types.h"
#include "RNA_path.h"

#include "ED_anim_api.h"
#include "ED_keyframes_edit.h"
#include "ED_keyframing.h"

/* This file contains code for various keyframe-editing tools which are 'destructive'
 * (i.e. they will modify the order of the keyframes, and change the size of the array).
 * While some of these tools may eventually be moved out into blenkernel, for now, it is
 * fine to have these calls here.
 *
 * There are also a few tools here which cannot be easily coded for in the other system (yet).
 * These may also be moved around at some point, but for now, they are best added here.
 *
 * - Joshua Leung, Dec 2008
 */

/* **************************************************** */

bool duplicate_fcurve_keys(FCurve *fcu)
{
  bool changed = false;

  /* this can only work when there is an F-Curve, and also when there are some BezTriples */
  if (ELEM(NULL, fcu, fcu->bezt)) {
    return changed;
  }

  for (int i = 0; i < fcu->totvert; i++) {
    /* If a key is selected */
    if (fcu->bezt[i].f2 & SELECT) {
      /* Expand the list */
      BezTriple *newbezt = MEM_callocN(sizeof(BezTriple) * (fcu->totvert + 1), "beztriple");

      memcpy(newbezt, fcu->bezt, sizeof(BezTriple) * (i + 1));
      memcpy(newbezt + i + 1, fcu->bezt + i, sizeof(BezTriple));
      memcpy(newbezt + i + 2, fcu->bezt + i + 1, sizeof(BezTriple) * (fcu->totvert - (i + 1)));
      fcu->totvert++;
      changed = true;
      /* reassign pointers... (free old, and add new) */
      MEM_freeN(fcu->bezt);
      fcu->bezt = newbezt;

      /* Unselect the current key */
      BEZT_DESEL_ALL(&fcu->bezt[i]);
      i++;

      /* Select the copied key */
      BEZT_SEL_ALL(&fcu->bezt[i]);
    }
  }
  return changed;
}

/* **************************************************** */
/* Various Tools */

void clean_fcurve(struct bAnimContext *ac, bAnimListElem *ale, float thresh, bool cleardefault)
{
  FCurve *fcu = (FCurve *)ale->key_data;
  BezTriple *old_bezts, *bezt, *beztn;
  BezTriple *lastb;
  int totCount, i;

  /* Check if any points. */
  if ((fcu == NULL) || (fcu->bezt == NULL) || (fcu->totvert == 0) ||
      (!cleardefault && fcu->totvert == 1)) {
    return;
  }

  /* make a copy of the old BezTriples, and clear F-Curve */
  old_bezts = fcu->bezt;
  totCount = fcu->totvert;
  fcu->bezt = NULL;
  fcu->totvert = 0;

  /* now insert first keyframe, as it should be ok */
  bezt = old_bezts;
  insert_bezt_fcurve(fcu, bezt, 0);
  if (!(bezt->f2 & SELECT)) {
    lastb = fcu->bezt;
    lastb->f1 = lastb->f2 = lastb->f3 = 0;
  }

  /* Loop through BezTriples, comparing them. Skip any that do
   * not fit the criteria for "ok" points.
   */
  for (i = 1; i < totCount; i++) {
    float prev[2], cur[2], next[2];

    /* get BezTriples and their values */
    if (i < (totCount - 1)) {
      beztn = (old_bezts + (i + 1));
      next[0] = beztn->vec[1][0];
      next[1] = beztn->vec[1][1];
    }
    else {
      beztn = NULL;
      next[0] = next[1] = 0.0f;
    }
    lastb = (fcu->bezt + (fcu->totvert - 1));
    bezt = (old_bezts + i);

    /* get references for quicker access */
    prev[0] = lastb->vec[1][0];
    prev[1] = lastb->vec[1][1];
    cur[0] = bezt->vec[1][0];
    cur[1] = bezt->vec[1][1];

    if (!(bezt->f2 & SELECT)) {
      insert_bezt_fcurve(fcu, bezt, 0);
      lastb = (fcu->bezt + (fcu->totvert - 1));
      lastb->f1 = lastb->f2 = lastb->f3 = 0;
      continue;
    }

    /* check if current bezt occurs at same time as last ok */
    if (IS_EQT(cur[0], prev[0], thresh)) {
      /* If there is a next beztriple, and if occurs at the same time, only insert
       * if there is a considerable distance between the points, and also if the
       * current is further away than the next one is to the previous.
       */
      if (beztn && (IS_EQT(cur[0], next[0], thresh)) && (IS_EQT(next[1], prev[1], thresh) == 0)) {
        /* only add if current is further away from previous */
        if (cur[1] > next[1]) {
          if (IS_EQT(cur[1], prev[1], thresh) == 0) {
            /* add new keyframe */
            insert_bezt_fcurve(fcu, bezt, 0);
          }
        }
      }
      else {
        /* only add if values are a considerable distance apart */
        if (IS_EQT(cur[1], prev[1], thresh) == 0) {
          /* add new keyframe */
          insert_bezt_fcurve(fcu, bezt, 0);
        }
      }
    }
    else {
      /* checks required are dependent on whether this is last keyframe or not */
      if (beztn) {
        /* does current have same value as previous and next? */
        if (IS_EQT(cur[1], prev[1], thresh) == 0) {
          /* add new keyframe */
          insert_bezt_fcurve(fcu, bezt, 0);
        }
        else if (IS_EQT(cur[1], next[1], thresh) == 0) {
          /* add new keyframe */
          insert_bezt_fcurve(fcu, bezt, 0);
        }
      }
      else {
        /* add if value doesn't equal that of previous */
        if (IS_EQT(cur[1], prev[1], thresh) == 0) {
          /* add new keyframe */
          insert_bezt_fcurve(fcu, bezt, 0);
        }
      }
    }
  }

  /* now free the memory used by the old BezTriples */
  if (old_bezts) {
    MEM_freeN(old_bezts);
  }

  /* final step, if there is just one key in fcurve, check if it's
   * the default value and if is, remove fcurve completely. */
  if (cleardefault && fcu->totvert == 1) {
    float default_value = 0.0f;
    PointerRNA id_ptr, ptr;
    PropertyRNA *prop;
    RNA_id_pointer_create(ale->id, &id_ptr);

    /* get property to read from, and get value as appropriate */
    if (RNA_path_resolve_property(&id_ptr, fcu->rna_path, &ptr, &prop)) {
      if (RNA_property_type(prop) == PROP_FLOAT) {
        default_value = RNA_property_float_get_default_index(&ptr, prop, fcu->array_index);
      }
    }

    if (fcu->bezt->vec[1][1] == default_value) {
      BKE_fcurve_delete_keys_all(fcu);

      /* check if curve is really unused and if it is, return signal for deletion */
      if (BKE_fcurve_is_empty(fcu)) {
        AnimData *adt = ale->adt;
        ANIM_fcurve_delete_from_animdata(ac, adt, fcu);
        ale->key_data = NULL;
      }
    }
  }
}

/**
 * Find the first segment of consecutive selected curve points, starting from \a start_index.
 * Keys that have BEZT_FLAG_IGNORE_TAG set are treated as unselected.
 * \param r_segment_start_idx: returns the start index of the segment.
 * \param r_segment_len: returns the number of curve points in the segment.
 * \return whether such a segment was found or not.
 */
static bool find_fcurve_segment(FCurve *fcu,
                                const int start_index,
                                int *r_segment_start_idx,
                                int *r_segment_len)
{
  *r_segment_start_idx = 0;
  *r_segment_len = 0;

  bool in_segment = false;

  for (int i = start_index; i < fcu->totvert; i++) {
    const bool point_is_selected = fcu->bezt[i].f2 & SELECT;
    const bool point_is_ignored = fcu->bezt[i].f2 & BEZT_FLAG_IGNORE_TAG;

    if (point_is_selected && !point_is_ignored) {
      if (!in_segment) {
        *r_segment_start_idx = i;
        in_segment = true;
      }
      (*r_segment_len)++;
    }
    else if (in_segment) {
      /* If the curve point is not selected then we have reached the end of the selected curve
       * segment. */
      return true; /* Segment found. */
    }
  }

  /* If the last curve point was in the segment, `r_segment_len` and `r_segment_start_idx`
   * are already updated and true is returned. */
  return in_segment;
}

ListBase find_fcurve_segments(FCurve *fcu)
{
  ListBase segments = {NULL, NULL};
  int segment_start_idx = 0;
  int segment_len = 0;
  int current_index = 0;

  while (find_fcurve_segment(fcu, current_index, &segment_start_idx, &segment_len)) {
    FCurveSegment *segment;
    segment = MEM_callocN(sizeof(*segment), "FCurveSegment");
    segment->start_index = segment_start_idx;
    segment->length = segment_len;
    BLI_addtail(&segments, segment);
    current_index = segment_start_idx + segment_len;
  }
  return segments;
}

static BezTriple fcurve_segment_start_get(FCurve *fcu, int index)
{
  BezTriple start_bezt = index - 1 >= 0 ? fcu->bezt[index - 1] : fcu->bezt[index];
  return start_bezt;
}

static BezTriple fcurve_segment_end_get(FCurve *fcu, int index)
{
  BezTriple end_bezt = index < fcu->totvert ? fcu->bezt[index] : fcu->bezt[index - 1];
  return end_bezt;
}

/* ---------------- */

void blend_to_neighbor_fcurve_segment(FCurve *fcu, FCurveSegment *segment, const float factor)
{
  const float blend_factor = fabs(factor * 2 - 1);
  BezTriple target_bezt;
  /* Find which key to blend towards. */
  if (factor < 0.5f) {
    target_bezt = fcurve_segment_start_get(fcu, segment->start_index);
  }
  else {
    target_bezt = fcurve_segment_end_get(fcu, segment->start_index + segment->length);
  }
  /* Blend each key individually. */
  for (int i = segment->start_index; i < segment->start_index + segment->length; i++) {
    fcu->bezt[i].vec[1][1] = interpf(target_bezt.vec[1][1], fcu->bezt[i].vec[1][1], blend_factor);
  }
}

/* ---------------- */

float get_default_rna_value(FCurve *fcu, PropertyRNA *prop, PointerRNA *ptr)
{
  const int len = RNA_property_array_length(ptr, prop);

  float default_value = 0;
  /* Find the default value of that property. */
  switch (RNA_property_type(prop)) {
    case PROP_BOOLEAN:
      if (len) {
        default_value = RNA_property_boolean_get_default_index(ptr, prop, fcu->array_index);
      }
      else {
        default_value = RNA_property_boolean_get_default(ptr, prop);
      }
      break;
    case PROP_INT:
      if (len) {
        default_value = RNA_property_int_get_default_index(ptr, prop, fcu->array_index);
      }
      else {
        default_value = RNA_property_int_get_default(ptr, prop);
      }
      break;
    case PROP_FLOAT:
      if (len) {
        default_value = RNA_property_float_get_default_index(ptr, prop, fcu->array_index);
      }
      else {
        default_value = RNA_property_float_get_default(ptr, prop);
      }
      break;

    default:
      break;
  }
  return default_value;
}

/* This function blends the selected keyframes to the default value of the property the fcurve
 * drives. */
void blend_to_default_fcurve(PointerRNA *id_ptr, FCurve *fcu, const float factor)
{
  PointerRNA ptr;
  PropertyRNA *prop;

  /* Check if path is valid. */
  if (!RNA_path_resolve_property(id_ptr, fcu->rna_path, &ptr, &prop)) {
    return;
  }

  const float default_value = get_default_rna_value(fcu, prop, &ptr);

  /* Blend selected keys to default */
  for (int i = 0; i < fcu->totvert; i++) {
    if (fcu->bezt[i].f2 & SELECT) {
      fcu->bezt[i].vec[1][1] = interpf(default_value, fcu->bezt[i].vec[1][1], factor);
    }
  }
}

/* ---------------- */

void breakdown_fcurve_segment(FCurve *fcu, FCurveSegment *segment, const float factor)
{
  BezTriple left_bezt = fcurve_segment_start_get(fcu, segment->start_index);
  BezTriple right_bezt = fcurve_segment_end_get(fcu, segment->start_index + segment->length);

  for (int i = segment->start_index; i < segment->start_index + segment->length; i++) {
    fcu->bezt[i].vec[1][1] = interpf(right_bezt.vec[1][1], left_bezt.vec[1][1], factor);
  }
}

/* ---------------- */

/* Check if the keyframe interpolation type is supported */
static bool prepare_for_decimate(FCurve *fcu, int i)
{
  switch (fcu->bezt[i].ipo) {
    case BEZT_IPO_BEZ:
      /* We do not need to do anything here as the keyframe already has the required setting.
       */
      return true;
    case BEZT_IPO_LIN:
      /* Convert to a linear bezt curve to be able to use the decimation algorithm. */
      fcu->bezt[i].ipo = BEZT_IPO_BEZ;
      fcu->bezt[i].h1 = HD_FREE;
      fcu->bezt[i].h2 = HD_FREE;

      if (i != 0) {
        float h1[3];
        sub_v3_v3v3(h1, fcu->bezt[i - 1].vec[1], fcu->bezt[i].vec[1]);
        mul_v3_fl(h1, 1.0f / 3.0f);
        add_v3_v3(h1, fcu->bezt[i].vec[1]);
        copy_v3_v3(fcu->bezt[i].vec[0], h1);
      }

      if (i + 1 != fcu->totvert) {
        float h2[3];
        sub_v3_v3v3(h2, fcu->bezt[i + 1].vec[1], fcu->bezt[i].vec[1]);
        mul_v3_fl(h2, 1.0f / 3.0f);
        add_v3_v3(h2, fcu->bezt[i].vec[1]);
        copy_v3_v3(fcu->bezt[i].vec[2], h2);
      }
      return true;
    default:
      /* These are unsupported. */
      return false;
  }
}

/* Decimate the given curve segment. */
static void decimate_fcurve_segment(FCurve *fcu,
                                    int bezt_segment_start_idx,
                                    int bezt_segment_len,
                                    float remove_ratio,
                                    float error_sq_max)
{
  int selected_len = bezt_segment_len;

  /* Make sure that we can remove the start/end point of the segment if they
   * are not the start/end point of the curve. BKE_curve_decimate_bezt_array
   * has a check that prevents removal of the first and last index in the
   * passed array. */
  if (bezt_segment_len + bezt_segment_start_idx != fcu->totvert &&
      prepare_for_decimate(fcu, bezt_segment_len + bezt_segment_start_idx)) {
    bezt_segment_len++;
  }
  if (bezt_segment_start_idx != 0 && prepare_for_decimate(fcu, bezt_segment_start_idx - 1)) {
    bezt_segment_start_idx--;
    bezt_segment_len++;
  }

  const int target_fcurve_verts = ceil(bezt_segment_len - selected_len * remove_ratio);

  BKE_curve_decimate_bezt_array(&fcu->bezt[bezt_segment_start_idx],
                                bezt_segment_len,
                                12, /* The actual resolution displayed in the viewport is dynamic
                                     * so we just pick a value that preserves the curve shape. */
                                false,
                                SELECT,
                                BEZT_FLAG_TEMP_TAG,
                                error_sq_max,
                                target_fcurve_verts);
}

bool decimate_fcurve(bAnimListElem *ale, float remove_ratio, float error_sq_max)
{
  FCurve *fcu = (FCurve *)ale->key_data;
  /* Check if the curve actually has any points. */
  if (fcu == NULL || fcu->bezt == NULL || fcu->totvert == 0) {
    return true;
  }

  BezTriple *old_bezts = fcu->bezt;

  bool can_decimate_all_selected = true;

  for (int i = 0; i < fcu->totvert; i++) {
    /* Ignore keyframes that are not supported. */
    if (!prepare_for_decimate(fcu, i)) {
      can_decimate_all_selected = false;
      fcu->bezt[i].f2 |= BEZT_FLAG_IGNORE_TAG;
    }
    /* Make sure that the temp flag is unset as we use it to determine what to remove. */
    fcu->bezt[i].f2 &= ~BEZT_FLAG_TEMP_TAG;
  }

  ListBase segments = find_fcurve_segments(fcu);
  LISTBASE_FOREACH (FCurveSegment *, segment, &segments) {
    decimate_fcurve_segment(
        fcu, segment->start_index, segment->length, remove_ratio, error_sq_max);
  }
  BLI_freelistN(&segments);

  uint old_totvert = fcu->totvert;
  fcu->bezt = NULL;
  fcu->totvert = 0;

  for (int i = 0; i < old_totvert; i++) {
    BezTriple *bezt = (old_bezts + i);
    bezt->f2 &= ~BEZT_FLAG_IGNORE_TAG;
    if ((bezt->f2 & BEZT_FLAG_TEMP_TAG) == 0) {
      insert_bezt_fcurve(fcu, bezt, 0);
    }
  }
  /* now free the memory used by the old BezTriples */
  if (old_bezts) {
    MEM_freeN(old_bezts);
  }

  return can_decimate_all_selected;
}

/* ---------------- */

/* temp struct used for smooth_fcurve */
typedef struct tSmooth_Bezt {
  float *h1, *h2, *h3; /* bezt->vec[0,1,2][1] */
  float y1, y2, y3;    /* averaged before/new/after y-values */
} tSmooth_Bezt;

void smooth_fcurve(FCurve *fcu)
{
  int totSel = 0;

  if (fcu->bezt == NULL) {
    return;
  }

  /* first loop through - count how many verts are selected */
  BezTriple *bezt = fcu->bezt;
  for (int i = 0; i < fcu->totvert; i++, bezt++) {
    if (BEZT_ISSEL_ANY(bezt)) {
      totSel++;
    }
  }

  /* if any points were selected, allocate tSmooth_Bezt points to work on */
  if (totSel >= 3) {
    tSmooth_Bezt *tarray, *tsb;

    /* allocate memory in one go */
    tsb = tarray = MEM_callocN(totSel * sizeof(tSmooth_Bezt), "tSmooth_Bezt Array");

    /* populate tarray with data of selected points */
    bezt = fcu->bezt;
    for (int i = 0, x = 0; (i < fcu->totvert) && (x < totSel); i++, bezt++) {
      if (BEZT_ISSEL_ANY(bezt)) {
        /* tsb simply needs pointer to vec, and index */
        tsb->h1 = &bezt->vec[0][1];
        tsb->h2 = &bezt->vec[1][1];
        tsb->h3 = &bezt->vec[2][1];

        /* advance to the next tsb to populate */
        if (x < totSel - 1) {
          tsb++;
        }
        else {
          break;
        }
      }
    }

    /* calculate the new smoothed F-Curve's with weighted averages:
     * - this is done with two passes to avoid progressive corruption errors
     * - uses 5 points for each operation (which stores in the relevant handles)
     * -   previous: w/a ratio = 3:5:2:1:1
     * -   next: w/a ratio = 1:1:2:5:3
     */

    /* round 1: calculate smoothing deltas and new values */
    tsb = tarray;
    for (int i = 0; i < totSel; i++, tsb++) {
      /* Don't touch end points (otherwise, curves slowly explode,
       * as we don't have enough data there). */
      if (ELEM(i, 0, (totSel - 1)) == 0) {
        const tSmooth_Bezt *tP1 = tsb - 1;
        const tSmooth_Bezt *tP2 = (i - 2 > 0) ? (tsb - 2) : (NULL);
        const tSmooth_Bezt *tN1 = tsb + 1;
        const tSmooth_Bezt *tN2 = (i + 2 < totSel) ? (tsb + 2) : (NULL);

        const float p1 = *tP1->h2;
        const float p2 = (tP2) ? (*tP2->h2) : (*tP1->h2);
        const float c1 = *tsb->h2;
        const float n1 = *tN1->h2;
        const float n2 = (tN2) ? (*tN2->h2) : (*tN1->h2);

        /* calculate previous and next, then new position by averaging these */
        tsb->y1 = (3 * p2 + 5 * p1 + 2 * c1 + n1 + n2) / 12;
        tsb->y3 = (p2 + p1 + 2 * c1 + 5 * n1 + 3 * n2) / 12;

        tsb->y2 = (tsb->y1 + tsb->y3) / 2;
      }
    }

    /* round 2: apply new values */
    tsb = tarray;
    for (int i = 0; i < totSel; i++, tsb++) {
      /* don't touch end points, as their values weren't touched above */
      if (ELEM(i, 0, (totSel - 1)) == 0) {
        /* y2 takes the average of the 2 points */
        *tsb->h2 = tsb->y2;

        /* handles are weighted between their original values and the averaged values */
        *tsb->h1 = ((*tsb->h1) * 0.7f) + (tsb->y1 * 0.3f);
        *tsb->h3 = ((*tsb->h3) * 0.7f) + (tsb->y3 * 0.3f);
      }
    }

    /* free memory required for tarray */
    MEM_freeN(tarray);
  }

  /* recalculate handles */
  BKE_fcurve_handles_recalc(fcu);
}

/* ---------------- */

/* little cache for values... */
typedef struct TempFrameValCache {
  float frame, val;
} TempFrameValCache;

void sample_fcurve(FCurve *fcu)
{
  BezTriple *bezt, *start = NULL, *end = NULL;
  TempFrameValCache *value_cache, *fp;
  int sfra, range;
  int i, n;

  if (fcu->bezt == NULL) { /* ignore baked */
    return;
  }

  /* Find selected keyframes... once pair has been found, add keyframes. */
  for (i = 0, bezt = fcu->bezt; i < fcu->totvert; i++, bezt++) {
    /* check if selected, and which end this is */
    if (BEZT_ISSEL_ANY(bezt)) {
      if (start) {
        /* If next bezt is also selected, don't start sampling yet,
         * but instead wait for that one to reconsider, to avoid
         * changing the curve when sampling consecutive segments
         * (T53229)
         */
        if (i < fcu->totvert - 1) {
          BezTriple *next = &fcu->bezt[i + 1];
          if (BEZT_ISSEL_ANY(next)) {
            continue;
          }
        }

        /* set end */
        end = bezt;

        /* cache values then add keyframes using these values, as adding
         * keyframes while sampling will affect the outcome...
         * - only start sampling+adding from index=1, so that we don't overwrite original keyframe
         */
        range = (int)(ceil(end->vec[1][0] - start->vec[1][0]));
        sfra = (int)(floor(start->vec[1][0]));

        if (range) {
          value_cache = MEM_callocN(sizeof(TempFrameValCache) * range, "IcuFrameValCache");

          /* sample values */
          for (n = 1, fp = value_cache; n < range && fp; n++, fp++) {
            fp->frame = (float)(sfra + n);
            fp->val = evaluate_fcurve(fcu, fp->frame);
          }

          /* add keyframes with these, tagging as 'breakdowns' */
          for (n = 1, fp = value_cache; n < range && fp; n++, fp++) {
            insert_vert_fcurve(fcu, fp->frame, fp->val, BEZT_KEYTYPE_BREAKDOWN, 1);
          }

          /* free temp cache */
          MEM_freeN(value_cache);

          /* as we added keyframes, we need to compensate so that bezt is at the right place */
          bezt = fcu->bezt + i + range - 1;
          i += (range - 1);
        }

        /* the current selection island has ended, so start again from scratch */
        start = NULL;
        end = NULL;
      }
      else {
        /* just set start keyframe */
        start = bezt;
        end = NULL;
      }
    }
  }

  /* recalculate channel's handles? */
  BKE_fcurve_handles_recalc(fcu);
}

/* **************************************************** */
/* Copy/Paste Tools:
 * - The copy/paste buffer currently stores a set of temporary F-Curves containing only the
 *   keyframes that were selected in each of the original F-Curves.
 * - All pasted frames are offset by the same amount.
 *   This is calculated as the difference in the times of the current frame and the
 *   'first keyframe' (i.e. the earliest one in all channels).
 * - The earliest frame is calculated per copy operation.
 */

/* globals for copy/paste data (like for other copy/paste buffers) */
static ListBase animcopybuf = {NULL, NULL};
static float animcopy_firstframe = 999999999.0f;
static float animcopy_lastframe = -999999999.0f;
static float animcopy_cfra = 0.0;

/* datatype for use in copy/paste buffer */
typedef struct tAnimCopybufItem {
  struct tAnimCopybufItem *next, *prev;

  ID *id;            /* ID which owns the curve */
  bActionGroup *grp; /* Action Group */
  char *rna_path;    /* RNA-Path */
  int array_index;   /* array index */

  int totvert;     /* number of keyframes stored for this channel */
  BezTriple *bezt; /* keyframes in buffer */

  short id_type; /* Result of `GS(id->name)`. */
  bool is_bone;  /* special flag for armature bones */
} tAnimCopybufItem;

void ANIM_fcurves_copybuf_free(void)
{
  tAnimCopybufItem *aci, *acn;

  /* free each buffer element */
  for (aci = animcopybuf.first; aci; aci = acn) {
    acn = aci->next;

    /* free keyframes */
    if (aci->bezt) {
      MEM_freeN(aci->bezt);
    }

    /* free RNA-path */
    if (aci->rna_path) {
      MEM_freeN(aci->rna_path);
    }

    /* free ourself */
    BLI_freelinkN(&animcopybuf, aci);
  }

  /* restore initial state */
  BLI_listbase_clear(&animcopybuf);
  animcopy_firstframe = 999999999.0f;
  animcopy_lastframe = -999999999.0f;
}

/* ------------------- */

short copy_animedit_keys(bAnimContext *ac, ListBase *anim_data)
{
  bAnimListElem *ale;
  Scene *scene = ac->scene;

  /* clear buffer first */
  ANIM_fcurves_copybuf_free();

  /* assume that each of these is an F-Curve */
  for (ale = anim_data->first; ale; ale = ale->next) {
    FCurve *fcu = (FCurve *)ale->key_data;
    tAnimCopybufItem *aci;
    BezTriple *bezt, *nbezt, *newbuf;
    int i;

    /* firstly, check if F-Curve has any selected keyframes
     * - skip if no selected keyframes found (so no need to create unnecessary copy-buffer data)
     * - this check should also eliminate any problems associated with using sample-data
     */
    if (ANIM_fcurve_keyframes_loop(
            NULL, fcu, NULL, ANIM_editkeyframes_ok(BEZT_OK_SELECTED), NULL) == 0) {
      continue;
    }

    /* init copybuf item info */
    aci = MEM_callocN(sizeof(tAnimCopybufItem), "AnimCopybufItem");
    aci->id = ale->id;
    aci->id_type = GS(ale->id->name);
    aci->grp = fcu->grp;
    aci->rna_path = MEM_dupallocN(fcu->rna_path);
    aci->array_index = fcu->array_index;

    /* Detect if this is a bone. We do that here rather than during pasting because ID pointers
     * will get invalidated if we undo.
     * Storing the relevant information here helps avoiding crashes if we undo-repaste. */
    if ((aci->id_type == ID_OB) && (((Object *)aci->id)->type == OB_ARMATURE) && aci->rna_path) {
      Object *ob = (Object *)aci->id;

      bPoseChannel *pchan;
      char bone_name[sizeof(pchan->name)];
      if (BLI_str_quoted_substr(aci->rna_path, "pose.bones[", bone_name, sizeof(bone_name))) {
        pchan = BKE_pose_channel_find_name(ob->pose, bone_name);
        if (pchan) {
          aci->is_bone = true;
        }
      }
    }

    BLI_addtail(&animcopybuf, aci);

    /* add selected keyframes to buffer */
    /* TODO: currently, we resize array every time we add a new vert -
     * this works ok as long as it is assumed only a few keys are copied */
    for (i = 0, bezt = fcu->bezt; i < fcu->totvert; i++, bezt++) {
      if (BEZT_ISSEL_ANY(bezt)) {
        /* add to buffer */
        newbuf = MEM_callocN(sizeof(BezTriple) * (aci->totvert + 1), "copybuf beztriple");

        /* assume that since we are just re-sizing the array, just copy all existing data across */
        if (aci->bezt) {
          memcpy(newbuf, aci->bezt, sizeof(BezTriple) * (aci->totvert));
        }

        /* copy current beztriple across too */
        nbezt = &newbuf[aci->totvert];
        *nbezt = *bezt;

        /* ensure copy buffer is selected so pasted keys are selected */
        BEZT_SEL_ALL(nbezt);

        /* free old array and set the new */
        if (aci->bezt) {
          MEM_freeN(aci->bezt);
        }
        aci->bezt = newbuf;
        aci->totvert++;

        /* check if this is the earliest frame encountered so far */
        if (bezt->vec[1][0] < animcopy_firstframe) {
          animcopy_firstframe = bezt->vec[1][0];
        }
        if (bezt->vec[1][0] > animcopy_lastframe) {
          animcopy_lastframe = bezt->vec[1][0];
        }
      }
    }
  }

  /* check if anything ended up in the buffer */
  if (ELEM(NULL, animcopybuf.first, animcopybuf.last)) {
    return -1;
  }

  /* in case 'relative' paste method is used */
  animcopy_cfra = scene->r.cfra;

  /* everything went fine */
  return 0;
}

static void flip_names(tAnimCopybufItem *aci, char **r_name)
{
  if (aci->is_bone) {
    int ofs_start;
    int ofs_end;

    if (BLI_str_quoted_substr_range(aci->rna_path, "pose.bones[", &ofs_start, &ofs_end)) {
      char *str_start = aci->rna_path + ofs_start;
      const char *str_end = aci->rna_path + ofs_end;

      /* Swap out the name.
       * Note that there is no need to un-escape the string to flip it. */
      char bname_new[MAX_VGROUP_NAME];
      char *str_iter;
      int length, prefix_l, postfix_l;

      prefix_l = str_start - aci->rna_path;

      length = str_end - str_start;
      postfix_l = strlen(str_end);

      /* Temporary substitute with NULL terminator. */
      BLI_assert(str_start[length] == '\"');
      str_start[length] = 0;
      BLI_string_flip_side_name(bname_new, str_start, false, sizeof(bname_new));
      str_start[length] = '\"';

      str_iter = *r_name = MEM_mallocN(sizeof(char) * (prefix_l + postfix_l + length + 1),
                                       "flipped_path");

      BLI_strncpy(str_iter, aci->rna_path, prefix_l + 1);
      str_iter += prefix_l;
      BLI_strncpy(str_iter, bname_new, length + 1);
      str_iter += length;
      BLI_strncpy(str_iter, str_end, postfix_l + 1);
      str_iter[postfix_l] = '\0';
    }
  }
}

/* ------------------- */

/* most strict method: exact matches only */
static tAnimCopybufItem *pastebuf_match_path_full(FCurve *fcu,
                                                  const short from_single,
                                                  const short to_simple,
                                                  bool flip)
{
  tAnimCopybufItem *aci;

  for (aci = animcopybuf.first; aci; aci = aci->next) {
    if (to_simple || (aci->rna_path && fcu->rna_path)) {
      if (!to_simple && flip && aci->is_bone && fcu->rna_path) {
        if ((from_single) || (aci->array_index == fcu->array_index)) {
          char *name = NULL;
          flip_names(aci, &name);
          if (STREQ(name, fcu->rna_path)) {
            MEM_freeN(name);
            break;
          }
          MEM_freeN(name);
        }
      }
      else if (to_simple || STREQ(aci->rna_path, fcu->rna_path)) {
        if ((from_single) || (aci->array_index == fcu->array_index)) {
          break;
        }
      }
    }
  }

  return aci;
}

/* medium match strictness: path match only (i.e. ignore ID) */
static tAnimCopybufItem *pastebuf_match_path_property(Main *bmain,
                                                      FCurve *fcu,
                                                      const short from_single,
                                                      const short UNUSED(to_simple))
{
  tAnimCopybufItem *aci;

  for (aci = animcopybuf.first; aci; aci = aci->next) {
    /* check that paths exist */
    if (aci->rna_path && fcu->rna_path) {
      /* find the property of the fcurve and compare against the end of the tAnimCopybufItem
       * more involved since it needs to do path lookups.
       * This is not 100% reliable since the user could be editing the curves on a path that won't
       * resolve, or a bone could be renamed after copying for eg. but in normal copy & paste
       * this should work out ok.
       */
      if (BLI_findindex(which_libbase(bmain, aci->id_type), aci->id) == -1) {
        /* pedantic but the ID could have been removed, and beats crashing! */
        printf("paste_animedit_keys: error ID has been removed!\n");
      }
      else {
        PointerRNA id_ptr, rptr;
        PropertyRNA *prop;

        RNA_id_pointer_create(aci->id, &id_ptr);

        if (RNA_path_resolve_property(&id_ptr, aci->rna_path, &rptr, &prop)) {
          const char *identifier = RNA_property_identifier(prop);
          int len_id = strlen(identifier);
          int len_path = strlen(fcu->rna_path);
          if (len_id <= len_path) {
            /* NOTE: paths which end with "] will fail with this test - Animated ID Props. */
            if (STREQ(identifier, fcu->rna_path + (len_path - len_id))) {
              if ((from_single) || (aci->array_index == fcu->array_index)) {
                break;
              }
            }
          }
        }
        else {
          printf("paste_animedit_keys: failed to resolve path id:%s, '%s'!\n",
                 aci->id->name,
                 aci->rna_path);
        }
      }
    }
  }

  return aci;
}

/* least strict matching heuristic: indices only */
static tAnimCopybufItem *pastebuf_match_index_only(FCurve *fcu,
                                                   const short from_single,
                                                   const short UNUSED(to_simple))
{
  tAnimCopybufItem *aci;

  for (aci = animcopybuf.first; aci; aci = aci->next) {
    /* check that paths exist */
    if ((from_single) || (aci->array_index == fcu->array_index)) {
      break;
    }
  }

  return aci;
}

/* ................ */

static void do_curve_mirror_flippping(tAnimCopybufItem *aci, BezTriple *bezt)
{
  if (aci->is_bone) {
    const size_t slength = strlen(aci->rna_path);
    bool flip = false;
    if (BLI_strn_endswith(aci->rna_path, "location", slength) && aci->array_index == 0) {
      flip = true;
    }
    else if (BLI_strn_endswith(aci->rna_path, "rotation_quaternion", slength) &&
             ELEM(aci->array_index, 2, 3)) {
      flip = true;
    }
    else if (BLI_strn_endswith(aci->rna_path, "rotation_euler", slength) &&
             ELEM(aci->array_index, 1, 2)) {
      flip = true;
    }
    else if (BLI_strn_endswith(aci->rna_path, "rotation_axis_angle", slength) &&
             ELEM(aci->array_index, 2, 3)) {
      flip = true;
    }

    if (flip) {
      bezt->vec[0][1] = -bezt->vec[0][1];
      bezt->vec[1][1] = -bezt->vec[1][1];
      bezt->vec[2][1] = -bezt->vec[2][1];
    }
  }
}

/* helper for paste_animedit_keys() - performs the actual pasting */
static void paste_animedit_keys_fcurve(
    FCurve *fcu, tAnimCopybufItem *aci, float offset, const eKeyMergeMode merge_mode, bool flip)
{
  BezTriple *bezt;
  int i;

  /* First de-select existing FCurve's keyframes */
  for (i = 0, bezt = fcu->bezt; i < fcu->totvert; i++, bezt++) {
    BEZT_DESEL_ALL(bezt);
  }

  /* mix mode with existing data */
  switch (merge_mode) {
    case KEYFRAME_PASTE_MERGE_MIX:
      /* do-nothing */
      break;

    case KEYFRAME_PASTE_MERGE_OVER:
      /* remove all keys */
      BKE_fcurve_delete_keys_all(fcu);
      break;

    case KEYFRAME_PASTE_MERGE_OVER_RANGE:
    case KEYFRAME_PASTE_MERGE_OVER_RANGE_ALL: {
      float f_min;
      float f_max;

      if (merge_mode == KEYFRAME_PASTE_MERGE_OVER_RANGE) {
        f_min = aci->bezt[0].vec[1][0] + offset;
        f_max = aci->bezt[aci->totvert - 1].vec[1][0] + offset;
      }
      else { /* Entire Range */
        f_min = animcopy_firstframe + offset;
        f_max = animcopy_lastframe + offset;
      }

      /* remove keys in range */
      if (f_min < f_max) {
        /* select verts in range for removal */
        for (i = 0, bezt = fcu->bezt; i < fcu->totvert; i++, bezt++) {
          if ((f_min < bezt[0].vec[1][0]) && (bezt[0].vec[1][0] < f_max)) {
            bezt->f2 |= SELECT;
          }
        }

        /* remove frames in the range */
        BKE_fcurve_delete_keys_selected(fcu);
      }
      break;
    }
  }

  /* just start pasting, with the first keyframe on the current frame, and so on */
  for (i = 0, bezt = aci->bezt; i < aci->totvert; i++, bezt++) {
    /* temporarily apply offset to src beztriple while copying */
    if (flip) {
      do_curve_mirror_flippping(aci, bezt);
    }

    bezt->vec[0][0] += offset;
    bezt->vec[1][0] += offset;
    bezt->vec[2][0] += offset;

    /* insert the keyframe
     * NOTE: we do not want to inherit handles from existing keyframes in this case!
     */

    insert_bezt_fcurve(fcu, bezt, INSERTKEY_OVERWRITE_FULL);

    /* un-apply offset from src beztriple after copying */
    bezt->vec[0][0] -= offset;
    bezt->vec[1][0] -= offset;
    bezt->vec[2][0] -= offset;

    if (flip) {
      do_curve_mirror_flippping(aci, bezt);
    }
  }

  /* recalculate F-Curve's handles? */
  BKE_fcurve_handles_recalc(fcu);
}

const EnumPropertyItem rna_enum_keyframe_paste_offset_items[] = {
    {KEYFRAME_PASTE_OFFSET_CFRA_START,
     "START",
     0,
     "Frame Start",
     "Paste keys starting at current frame"},
    {KEYFRAME_PASTE_OFFSET_CFRA_END, "END", 0, "Frame End", "Paste keys ending at current frame"},
    {KEYFRAME_PASTE_OFFSET_CFRA_RELATIVE,
     "RELATIVE",
     0,
     "Frame Relative",
     "Paste keys relative to the current frame when copying"},
    {KEYFRAME_PASTE_OFFSET_NONE, "NONE", 0, "No Offset", "Paste keys from original time"},
    {0, NULL, 0, NULL, NULL},
};

const EnumPropertyItem rna_enum_keyframe_paste_merge_items[] = {
    {KEYFRAME_PASTE_MERGE_MIX, "MIX", 0, "Mix", "Overlay existing with new keys"},
    {KEYFRAME_PASTE_MERGE_OVER, "OVER_ALL", 0, "Overwrite All", "Replace all keys"},
    {KEYFRAME_PASTE_MERGE_OVER_RANGE,
     "OVER_RANGE",
     0,
     "Overwrite Range",
     "Overwrite keys in pasted range"},
    {KEYFRAME_PASTE_MERGE_OVER_RANGE_ALL,
     "OVER_RANGE_ALL",
     0,
     "Overwrite Entire Range",
     "Overwrite keys in pasted range, using the range of all copied keys"},
    {0, NULL, 0, NULL, NULL},
};

eKeyPasteError paste_animedit_keys(bAnimContext *ac,
                                   ListBase *anim_data,
                                   const eKeyPasteOffset offset_mode,
                                   const eKeyMergeMode merge_mode,
                                   bool flip)
{
  bAnimListElem *ale;

  const Scene *scene = (ac->scene);

  const bool from_single = BLI_listbase_is_single(&animcopybuf);
  const bool to_simple = BLI_listbase_is_single(anim_data);

  float offset = 0.0f;
  int pass;

  /* check if buffer is empty */
  if (BLI_listbase_is_empty(&animcopybuf)) {
    return KEYFRAME_PASTE_NOTHING_TO_PASTE;
  }

  if (BLI_listbase_is_empty(anim_data)) {
    return KEYFRAME_PASTE_NOWHERE_TO_PASTE;
  }

  /* methods of offset */
  switch (offset_mode) {
    case KEYFRAME_PASTE_OFFSET_CFRA_START:
      offset = (float)(scene->r.cfra - animcopy_firstframe);
      break;
    case KEYFRAME_PASTE_OFFSET_CFRA_END:
      offset = (float)(scene->r.cfra - animcopy_lastframe);
      break;
    case KEYFRAME_PASTE_OFFSET_CFRA_RELATIVE:
      offset = (float)(scene->r.cfra - animcopy_cfra);
      break;
    case KEYFRAME_PASTE_OFFSET_NONE:
      offset = 0.0f;
      break;
  }

  if (from_single && to_simple) {
    /* 1:1 match, no tricky checking, just paste */
    FCurve *fcu;
    tAnimCopybufItem *aci;

    ale = anim_data->first;
    fcu = (FCurve *)ale->data; /* destination F-Curve */
    aci = animcopybuf.first;

    paste_animedit_keys_fcurve(fcu, aci, offset, merge_mode, false);
    ale->update |= ANIM_UPDATE_DEFAULT;
  }
  else {
    /* from selected channels
     * This "passes" system aims to try to find "matching" channels to paste keyframes
     * into with increasingly loose matching heuristics. The process finishes when at least
     * one F-Curve has been pasted into.
     */
    for (pass = 0; pass < 3; pass++) {
      uint totmatch = 0;

      for (ale = anim_data->first; ale; ale = ale->next) {
        /* Find buffer item to paste from:
         * - If names don't matter (i.e. only 1 channel in buffer), don't check id/group
         * - If names do matter, only check if id-type is ok for now
         *   (group check is not that important).
         * - Most importantly, rna-paths should match (array indices are unimportant for now)
         */
        AnimData *adt = ANIM_nla_mapping_get(ac, ale);
        FCurve *fcu = (FCurve *)ale->data; /* destination F-Curve */
        tAnimCopybufItem *aci = NULL;

        switch (pass) {
          case 0:
            /* most strict, must be exact path match data_path & index */
            aci = pastebuf_match_path_full(fcu, from_single, to_simple, flip);
            break;

          case 1:
            /* less strict, just compare property names */
            aci = pastebuf_match_path_property(ac->bmain, fcu, from_single, to_simple);
            break;

          case 2:
            /* Comparing properties gave no results, so just do index comparisons */
            aci = pastebuf_match_index_only(fcu, from_single, to_simple);
            break;
        }

        /* copy the relevant data from the matching buffer curve */
        if (aci) {
          totmatch++;

          if (adt) {
            ANIM_nla_mapping_apply_fcurve(adt, ale->key_data, 0, 0);
            paste_animedit_keys_fcurve(fcu, aci, offset, merge_mode, flip);
            ANIM_nla_mapping_apply_fcurve(adt, ale->key_data, 1, 0);
          }
          else {
            paste_animedit_keys_fcurve(fcu, aci, offset, merge_mode, flip);
          }
        }

        ale->update |= ANIM_UPDATE_DEFAULT;
      }

      /* don't continue if some fcurves were pasted */
      if (totmatch) {
        break;
      }
    }
  }

  ANIM_animdata_update(ac, anim_data);

  return KEYFRAME_PASTE_OK;
}

/* **************************************************** */
