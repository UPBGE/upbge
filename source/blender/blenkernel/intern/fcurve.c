/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation, Joshua Leung. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_anim_types.h"
#include "DNA_object_types.h"
#include "DNA_text_types.h"

#include "BLI_blenlib.h"
#include "BLI_easing.h"
#include "BLI_ghash.h"
#include "BLI_math.h"
#include "BLI_sort_utils.h"

#include "BKE_anim_data.h"
#include "BKE_animsys.h"
#include "BKE_context.h"
#include "BKE_curve.h"
#include "BKE_fcurve.h"
#include "BKE_fcurve_driver.h"
#include "BKE_global.h"
#include "BKE_idprop.h"
#include "BKE_lib_query.h"
#include "BKE_nla.h"

#include "BLO_read_write.h"

#include "RNA_access.h"
#include "RNA_path.h"

#include "CLG_log.h"

#define SMALL -1.0e-10
#define SELECT 1

static CLG_LogRef LOG = {"bke.fcurve"};

/* -------------------------------------------------------------------- */
/** \name F-Curve Data Create
 * \{ */

FCurve *BKE_fcurve_create(void)
{
  FCurve *fcu = MEM_callocN(sizeof(FCurve), __func__);
  return fcu;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name F-Curve Data Free
 * \{ */

void BKE_fcurve_free(FCurve *fcu)
{
  if (fcu == NULL) {
    return;
  }

  /* Free curve data. */
  MEM_SAFE_FREE(fcu->bezt);
  MEM_SAFE_FREE(fcu->fpt);

  /* Free RNA-path, as this were allocated when getting the path string. */
  MEM_SAFE_FREE(fcu->rna_path);

  /* Free extra data - i.e. modifiers, and driver. */
  fcurve_free_driver(fcu);
  free_fmodifiers(&fcu->modifiers);

  /* Free the f-curve itself. */
  MEM_freeN(fcu);
}

void BKE_fcurves_free(ListBase *list)
{
  FCurve *fcu, *fcn;

  /* Sanity check. */
  if (list == NULL) {
    return;
  }

  /* Free data - no need to call remlink before freeing each curve,
   * as we store reference to next, and freeing only touches the curve
   * it's given.
   */
  for (fcu = list->first; fcu; fcu = fcn) {
    fcn = fcu->next;
    BKE_fcurve_free(fcu);
  }

  /* Clear pointers just in case. */
  BLI_listbase_clear(list);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name F-Curve Data Copy
 * \{ */

FCurve *BKE_fcurve_copy(const FCurve *fcu)
{
  FCurve *fcu_d;

  /* Sanity check. */
  if (fcu == NULL) {
    return NULL;
  }

  /* Make a copy. */
  fcu_d = MEM_dupallocN(fcu);

  fcu_d->next = fcu_d->prev = NULL;
  fcu_d->grp = NULL;

  /* Copy curve data. */
  fcu_d->bezt = MEM_dupallocN(fcu_d->bezt);
  fcu_d->fpt = MEM_dupallocN(fcu_d->fpt);

  /* Copy rna-path. */
  fcu_d->rna_path = MEM_dupallocN(fcu_d->rna_path);

  /* Copy driver. */
  fcu_d->driver = fcurve_copy_driver(fcu_d->driver);

  /* Copy modifiers. */
  copy_fmodifiers(&fcu_d->modifiers, &fcu->modifiers);

  /* Return new data. */
  return fcu_d;
}

void BKE_fcurves_copy(ListBase *dst, ListBase *src)
{
  FCurve *dfcu, *sfcu;

  /* Sanity checks. */
  if (ELEM(NULL, dst, src)) {
    return;
  }

  /* Clear destination list first. */
  BLI_listbase_clear(dst);

  /* Copy one-by-one. */
  for (sfcu = src->first; sfcu; sfcu = sfcu->next) {
    dfcu = BKE_fcurve_copy(sfcu);
    BLI_addtail(dst, dfcu);
  }
}

void BKE_fcurve_foreach_id(FCurve *fcu, LibraryForeachIDData *data)
{
  ChannelDriver *driver = fcu->driver;

  if (driver != NULL) {
    LISTBASE_FOREACH (DriverVar *, dvar, &driver->variables) {
      /* only used targets */
      DRIVER_TARGETS_USED_LOOPER_BEGIN (dvar) {
        BKE_LIB_FOREACHID_PROCESS_ID(data, dtar->id, IDWALK_CB_NOP);
      }
      DRIVER_TARGETS_LOOPER_END;
    }
  }

  LISTBASE_FOREACH (FModifier *, fcm, &fcu->modifiers) {
    switch (fcm->type) {
      case FMODIFIER_TYPE_PYTHON: {
        FMod_Python *fcm_py = (FMod_Python *)fcm->data;
        BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, fcm_py->script, IDWALK_CB_NOP);

        BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
            data,
            IDP_foreach_property(fcm_py->prop,
                                 IDP_TYPE_FILTER_ID,
                                 BKE_lib_query_idpropertiesForeachIDLink_callback,
                                 data));
        break;
      }
      default:
        break;
    }
  }
}

/* ----------------- Finding F-Curves -------------------------- */

FCurve *id_data_find_fcurve(
    ID *id, void *data, StructRNA *type, const char *prop_name, int index, bool *r_driven)
{
  /* Anim vars */
  AnimData *adt = BKE_animdata_from_id(id);
  FCurve *fcu = NULL;

  /* Rna vars */
  PointerRNA ptr;
  PropertyRNA *prop;
  char *path;

  if (r_driven) {
    *r_driven = false;
  }

  /* Only use the current action ??? */
  if (ELEM(NULL, adt, adt->action)) {
    return NULL;
  }

  RNA_pointer_create(id, type, data, &ptr);
  prop = RNA_struct_find_property(&ptr, prop_name);
  if (prop == NULL) {
    return NULL;
  }

  path = RNA_path_from_ID_to_property(&ptr, prop);
  if (path == NULL) {
    return NULL;
  }

  /* FIXME: The way drivers are handled here (always NULL-ifying `fcu`) is very weird, this needs
   * to be re-checked I think?. */
  bool is_driven = false;
  fcu = BKE_animadata_fcurve_find_by_rna_path(adt, path, index, NULL, &is_driven);
  if (is_driven) {
    if (r_driven != NULL) {
      *r_driven = is_driven;
    }
    fcu = NULL;
  }

  MEM_freeN(path);

  return fcu;
}

FCurve *BKE_fcurve_find(ListBase *list, const char rna_path[], const int array_index)
{
  FCurve *fcu;

  /* Sanity checks. */
  if (ELEM(NULL, list, rna_path) || (array_index < 0)) {
    return NULL;
  }

  /* Check paths of curves, then array indices... */
  for (fcu = list->first; fcu; fcu = fcu->next) {
    /* Check indices first, much cheaper than a string comparison. */
    /* Simple string-compare (this assumes that they have the same root...) */
    if (UNLIKELY(fcu->array_index == array_index && fcu->rna_path &&
                 fcu->rna_path[0] == rna_path[0] && STREQ(fcu->rna_path, rna_path))) {
      return fcu;
    }
  }

  return NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name FCurve Iteration
 * \{ */

FCurve *BKE_fcurve_iter_step(FCurve *fcu_iter, const char rna_path[])
{
  FCurve *fcu;

  /* Sanity checks. */
  if (ELEM(NULL, fcu_iter, rna_path)) {
    return NULL;
  }

  /* Check paths of curves, then array indices... */
  for (fcu = fcu_iter; fcu; fcu = fcu->next) {
    /* Simple string-compare (this assumes that they have the same root...) */
    if (fcu->rna_path && STREQ(fcu->rna_path, rna_path)) {
      return fcu;
    }
  }

  return NULL;
}

int BKE_fcurves_filter(ListBase *dst, ListBase *src, const char *dataPrefix, const char *dataName)
{
  FCurve *fcu;
  int matches = 0;

  /* Sanity checks. */
  if (ELEM(NULL, dst, src, dataPrefix, dataName)) {
    return 0;
  }
  if ((dataPrefix[0] == 0) || (dataName[0] == 0)) {
    return 0;
  }

  const size_t quotedName_size = strlen(dataName) + 1;
  char *quotedName = alloca(quotedName_size);

  /* Search each F-Curve one by one. */
  for (fcu = src->first; fcu; fcu = fcu->next) {
    /* Check if quoted string matches the path. */
    if (fcu->rna_path == NULL) {
      continue;
    }
    /* Skipping names longer than `quotedName_size` is OK since we're after an exact match. */
    if (!BLI_str_quoted_substr(fcu->rna_path, dataPrefix, quotedName, quotedName_size)) {
      continue;
    }
    if (!STREQ(quotedName, dataName)) {
      continue;
    }

    /* Check if the quoted name matches the required name. */
    LinkData *ld = MEM_callocN(sizeof(LinkData), __func__);

    ld->data = fcu;
    BLI_addtail(dst, ld);

    matches++;
  }
  /* Return the number of matches. */
  return matches;
}

FCurve *BKE_animadata_fcurve_find_by_rna_path(
    AnimData *animdata, const char *rna_path, int rna_index, bAction **r_action, bool *r_driven)
{
  if (r_driven != NULL) {
    *r_driven = false;
  }
  if (r_action != NULL) {
    *r_action = NULL;
  }

  const bool has_action_fcurves = animdata->action != NULL &&
                                  !BLI_listbase_is_empty(&animdata->action->curves);
  const bool has_drivers = !BLI_listbase_is_empty(&animdata->drivers);

  /* Animation takes priority over drivers. */
  if (has_action_fcurves) {
    FCurve *fcu = BKE_fcurve_find(&animdata->action->curves, rna_path, rna_index);

    if (fcu != NULL) {
      if (r_action != NULL) {
        *r_action = animdata->action;
      }
      return fcu;
    }
  }

  /* If not animated, check if driven. */
  if (has_drivers) {
    FCurve *fcu = BKE_fcurve_find(&animdata->drivers, rna_path, rna_index);

    if (fcu != NULL) {
      if (r_driven != NULL) {
        *r_driven = true;
      }
      return fcu;
    }
  }

  return NULL;
}

FCurve *BKE_fcurve_find_by_rna(PointerRNA *ptr,
                               PropertyRNA *prop,
                               int rnaindex,
                               AnimData **r_adt,
                               bAction **r_action,
                               bool *r_driven,
                               bool *r_special)
{
  return BKE_fcurve_find_by_rna_context_ui(
      NULL, ptr, prop, rnaindex, r_adt, r_action, r_driven, r_special);
}

FCurve *BKE_fcurve_find_by_rna_context_ui(bContext *UNUSED(C),
                                          const PointerRNA *ptr,
                                          PropertyRNA *prop,
                                          int rnaindex,
                                          AnimData **r_animdata,
                                          bAction **r_action,
                                          bool *r_driven,
                                          bool *r_special)
{
  if (r_animdata != NULL) {
    *r_animdata = NULL;
  }
  if (r_action != NULL) {
    *r_action = NULL;
  }
  if (r_driven != NULL) {
    *r_driven = false;
  }
  if (r_special) {
    *r_special = false;
  }

  /* Special case for NLA Control Curves... */
  if (BKE_nlastrip_has_curves_for_property(ptr, prop)) {
    NlaStrip *strip = ptr->data;

    /* Set the special flag, since it cannot be a normal action/driver
     * if we've been told to start looking here...
     */
    if (r_special) {
      *r_special = true;
    }

    *r_driven = false;
    if (r_animdata) {
      *r_animdata = NULL;
    }
    if (r_action) {
      *r_action = NULL;
    }

    /* The F-Curve either exists or it doesn't here... */
    return BKE_fcurve_find(&strip->fcurves, RNA_property_identifier(prop), rnaindex);
  }

  /* There must be some RNA-pointer + property combo. */
  if (!prop || !ptr->owner_id || !RNA_property_animateable(ptr, prop)) {
    return NULL;
  }

  AnimData *adt = BKE_animdata_from_id(ptr->owner_id);
  if (adt == NULL) {
    return NULL;
  }

  /* XXX This function call can become a performance bottleneck. */
  char *rna_path = RNA_path_from_ID_to_property(ptr, prop);
  if (rna_path == NULL) {
    return NULL;
  }

  /* Standard F-Curve from animdata - Animation (Action) or Drivers. */
  FCurve *fcu = BKE_animadata_fcurve_find_by_rna_path(adt, rna_path, rnaindex, r_action, r_driven);

  if (fcu != NULL && r_animdata != NULL) {
    *r_animdata = adt;
  }

  MEM_freeN(rna_path);
  return fcu;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Finding Keyframes/Extents
 * \{ */

/* Binary search algorithm for finding where to insert BezTriple,
 * with optional argument for precision required.
 * Returns the index to insert at (data already at that index will be offset if replace is 0)
 */
static int BKE_fcurve_bezt_binarysearch_index_ex(const BezTriple array[],
                                                 const float frame,
                                                 const int arraylen,
                                                 const float threshold,
                                                 bool *r_replace)
{
  int start = 0, end = arraylen;
  int loopbreaker = 0, maxloop = arraylen * 2;

  /* Initialize replace-flag first. */
  *r_replace = false;

  /* Sneaky optimizations (don't go through searching process if...):
   * - Keyframe to be added is to be added out of current bounds.
   * - Keyframe to be added would replace one of the existing ones on bounds.
   */
  if ((arraylen <= 0) || (array == NULL)) {
    CLOG_WARN(&LOG, "encountered invalid array");
    return 0;
  }

  /* Check whether to add before/after/on. */
  float framenum;

  /* 'First' Keyframe (when only one keyframe, this case is used) */
  framenum = array[0].vec[1][0];
  if (IS_EQT(frame, framenum, threshold)) {
    *r_replace = true;
    return 0;
  }
  if (frame < framenum) {
    return 0;
  }

  /* 'Last' Keyframe */
  framenum = array[(arraylen - 1)].vec[1][0];
  if (IS_EQT(frame, framenum, threshold)) {
    *r_replace = true;
    return (arraylen - 1);
  }
  if (frame > framenum) {
    return arraylen;
  }

  /* Most of the time, this loop is just to find where to put it
   * 'loopbreaker' is just here to prevent infinite loops.
   */
  for (loopbreaker = 0; (start <= end) && (loopbreaker < maxloop); loopbreaker++) {
    /* Compute and get midpoint. */

    /* We calculate the midpoint this way to avoid int overflows... */
    int mid = start + ((end - start) / 2);

    float midfra = array[mid].vec[1][0];

    /* Check if exactly equal to midpoint. */
    if (IS_EQT(frame, midfra, threshold)) {
      *r_replace = true;
      return mid;
    }

    /* Repeat in upper/lower half. */
    if (frame > midfra) {
      start = mid + 1;
    }
    else if (frame < midfra) {
      end = mid - 1;
    }
  }

  /* Print error if loop-limit exceeded. */
  if (loopbreaker == (maxloop - 1)) {
    CLOG_ERROR(&LOG, "search taking too long");

    /* Include debug info. */
    CLOG_ERROR(&LOG,
               "\tround = %d: start = %d, end = %d, arraylen = %d",
               loopbreaker,
               start,
               end,
               arraylen);
  }

  /* Not found, so return where to place it. */
  return start;
}

int BKE_fcurve_bezt_binarysearch_index(const BezTriple array[],
                                       const float frame,
                                       const int arraylen,
                                       bool *r_replace)
{
  /* This is just a wrapper which uses the default threshold. */
  return BKE_fcurve_bezt_binarysearch_index_ex(
      array, frame, arraylen, BEZT_BINARYSEARCH_THRESH, r_replace);
}

/* ...................................... */

/* Helper for calc_fcurve_* functions -> find first and last BezTriple to be used. */
static short get_fcurve_end_keyframes(FCurve *fcu,
                                      BezTriple **first,
                                      BezTriple **last,
                                      const bool do_sel_only)
{
  bool found = false;

  /* Init outputs. */
  *first = NULL;
  *last = NULL;

  /* Sanity checks. */
  if (fcu->bezt == NULL) {
    return found;
  }

  /* Only include selected items? */
  if (do_sel_only) {
    BezTriple *bezt;

    /* Find first selected. */
    bezt = fcu->bezt;
    for (int i = 0; i < fcu->totvert; bezt++, i++) {
      if (BEZT_ISSEL_ANY(bezt)) {
        *first = bezt;
        found = true;
        break;
      }
    }

    /* Find last selected. */
    bezt = ARRAY_LAST_ITEM(fcu->bezt, BezTriple, fcu->totvert);
    for (int i = 0; i < fcu->totvert; bezt--, i++) {
      if (BEZT_ISSEL_ANY(bezt)) {
        *last = bezt;
        found = true;
        break;
      }
    }
  }
  else {
    /* Use the whole array. */
    *first = fcu->bezt;
    *last = ARRAY_LAST_ITEM(fcu->bezt, BezTriple, fcu->totvert);
    found = true;
  }

  return found;
}

bool BKE_fcurve_calc_bounds(FCurve *fcu,
                            float *xmin,
                            float *xmax,
                            float *ymin,
                            float *ymax,
                            const bool do_sel_only,
                            const bool include_handles)
{
  float xminv = 999999999.0f, xmaxv = -999999999.0f;
  float yminv = 999999999.0f, ymaxv = -999999999.0f;
  bool foundvert = false;

  if (fcu->totvert) {
    if (fcu->bezt) {
      BezTriple *bezt_first = NULL, *bezt_last = NULL;

      if (xmin || xmax) {
        /* Get endpoint keyframes. */
        foundvert = get_fcurve_end_keyframes(fcu, &bezt_first, &bezt_last, do_sel_only);

        if (bezt_first) {
          BLI_assert(bezt_last != NULL);

          if (include_handles) {
            xminv = min_fff(xminv, bezt_first->vec[0][0], bezt_first->vec[1][0]);
            xmaxv = max_fff(xmaxv, bezt_last->vec[1][0], bezt_last->vec[2][0]);
          }
          else {
            xminv = min_ff(xminv, bezt_first->vec[1][0]);
            xmaxv = max_ff(xmaxv, bezt_last->vec[1][0]);
          }
        }
      }

      /* Only loop over keyframes to find extents for values if needed. */
      if (ymin || ymax) {
        BezTriple *bezt, *prevbezt = NULL;

        int i;
        for (bezt = fcu->bezt, i = 0; i < fcu->totvert; prevbezt = bezt, bezt++, i++) {
          if ((do_sel_only == false) || BEZT_ISSEL_ANY(bezt)) {
            /* Keyframe itself. */
            yminv = min_ff(yminv, bezt->vec[1][1]);
            ymaxv = max_ff(ymaxv, bezt->vec[1][1]);

            if (include_handles) {
              /* Left handle - only if applicable.
               * NOTE: for the very first keyframe,
               * the left handle actually has no bearings on anything. */
              if (prevbezt && (prevbezt->ipo == BEZT_IPO_BEZ)) {
                yminv = min_ff(yminv, bezt->vec[0][1]);
                ymaxv = max_ff(ymaxv, bezt->vec[0][1]);
              }

              /* Right handle - only if applicable. */
              if (bezt->ipo == BEZT_IPO_BEZ) {
                yminv = min_ff(yminv, bezt->vec[2][1]);
                ymaxv = max_ff(ymaxv, bezt->vec[2][1]);
              }
            }

            foundvert = true;
          }
        }
      }
    }
    else if (fcu->fpt) {
      /* Frame range can be directly calculated from end verts. */
      if (xmin || xmax) {
        xminv = min_ff(xminv, fcu->fpt[0].vec[0]);
        xmaxv = max_ff(xmaxv, fcu->fpt[fcu->totvert - 1].vec[0]);
      }

      /* Only loop over keyframes to find extents for values if needed. */
      if (ymin || ymax) {
        FPoint *fpt;
        int i;

        for (fpt = fcu->fpt, i = 0; i < fcu->totvert; fpt++, i++) {
          if (fpt->vec[1] < yminv) {
            yminv = fpt->vec[1];
          }
          if (fpt->vec[1] > ymaxv) {
            ymaxv = fpt->vec[1];
          }

          foundvert = true;
        }
      }
    }
  }

  if (foundvert) {
    if (xmin) {
      *xmin = xminv;
    }
    if (xmax) {
      *xmax = xmaxv;
    }

    if (ymin) {
      *ymin = yminv;
    }
    if (ymax) {
      *ymax = ymaxv;
    }
  }
  else {
    if (G.debug & G_DEBUG) {
      printf("F-Curve calc bounds didn't find anything, so assuming minimum bounds of 1.0\n");
    }

    if (xmin) {
      *xmin = 0.0f;
    }
    if (xmax) {
      *xmax = 1.0f;
    }

    if (ymin) {
      *ymin = 0.0f;
    }
    if (ymax) {
      *ymax = 1.0f;
    }
  }

  return foundvert;
}

bool BKE_fcurve_calc_range(
    FCurve *fcu, float *start, float *end, const bool do_sel_only, const bool do_min_length)
{
  float min = 999999999.0f, max = -999999999.0f;
  bool foundvert = false;

  if (fcu->totvert) {
    if (fcu->bezt) {
      BezTriple *bezt_first = NULL, *bezt_last = NULL;

      /* Get endpoint keyframes. */
      get_fcurve_end_keyframes(fcu, &bezt_first, &bezt_last, do_sel_only);

      if (bezt_first) {
        BLI_assert(bezt_last != NULL);

        min = min_ff(min, bezt_first->vec[1][0]);
        max = max_ff(max, bezt_last->vec[1][0]);

        foundvert = true;
      }
    }
    else if (fcu->fpt) {
      min = min_ff(min, fcu->fpt[0].vec[0]);
      max = max_ff(max, fcu->fpt[fcu->totvert - 1].vec[0]);

      foundvert = true;
    }
  }

  if (foundvert == false) {
    min = max = 0.0f;
  }

  if (do_min_length) {
    /* Minimum length is 1 frame. */
    if (min == max) {
      max += 1.0f;
    }
  }

  *start = min;
  *end = max;

  return foundvert;
}

float *BKE_fcurves_calc_keyed_frames_ex(FCurve **fcurve_array,
                                        int fcurve_array_len,
                                        const float interval,
                                        int *r_frames_len)
{
  /* Use `1e-3f` as the smallest possible value since these are converted to integers
   * and we can be sure `MAXFRAME / 1e-3f < INT_MAX` as it's around half the size. */
  const double interval_db = max_ff(interval, 1e-3f);
  GSet *frames_unique = BLI_gset_int_new(__func__);
  for (int fcurve_index = 0; fcurve_index < fcurve_array_len; fcurve_index++) {
    const FCurve *fcu = fcurve_array[fcurve_index];
    for (int i = 0; i < fcu->totvert; i++) {
      const BezTriple *bezt = &fcu->bezt[i];
      const double value = round((double)bezt->vec[1][0] / interval_db);
      BLI_assert(value > INT_MIN && value < INT_MAX);
      BLI_gset_add(frames_unique, POINTER_FROM_INT((int)value));
    }
  }

  const size_t frames_len = BLI_gset_len(frames_unique);
  float *frames = MEM_mallocN(sizeof(*frames) * frames_len, __func__);

  GSetIterator gs_iter;
  int i = 0;
  GSET_ITER_INDEX (gs_iter, frames_unique, i) {
    const int value = POINTER_AS_INT(BLI_gsetIterator_getKey(&gs_iter));
    frames[i] = (double)value * interval_db;
  }
  BLI_gset_free(frames_unique, NULL);

  qsort(frames, frames_len, sizeof(*frames), BLI_sortutil_cmp_float);
  *r_frames_len = frames_len;
  return frames;
}

float *BKE_fcurves_calc_keyed_frames(FCurve **fcurve_array,
                                     int fcurve_array_len,
                                     int *r_frames_len)
{
  return BKE_fcurves_calc_keyed_frames_ex(fcurve_array, fcurve_array_len, 1.0f, r_frames_len);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Active Keyframe
 * \{ */

void BKE_fcurve_active_keyframe_set(FCurve *fcu, const BezTriple *active_bezt)
{
  if (active_bezt == NULL) {
    fcu->active_keyframe_index = FCURVE_ACTIVE_KEYFRAME_NONE;
    return;
  }

  /* Gracefully handle out-of-bounds pointers. Ideally this would do a BLI_assert() as well, but
   * then the unit tests would break in debug mode. */
  ptrdiff_t offset = active_bezt - fcu->bezt;
  if (offset < 0 || offset >= fcu->totvert) {
    fcu->active_keyframe_index = FCURVE_ACTIVE_KEYFRAME_NONE;
    return;
  }

  /* The active keyframe should always be selected. */
  BLI_assert_msg(BEZT_ISSEL_ANY(active_bezt), "active keyframe must be selected");

  fcu->active_keyframe_index = (int)offset;
}

int BKE_fcurve_active_keyframe_index(const FCurve *fcu)
{
  const int active_keyframe_index = fcu->active_keyframe_index;

  /* Array access boundary checks. */
  if ((fcu->bezt == NULL) || (active_keyframe_index >= fcu->totvert) ||
      (active_keyframe_index < 0)) {
    return FCURVE_ACTIVE_KEYFRAME_NONE;
  }

  const BezTriple *active_bezt = &fcu->bezt[active_keyframe_index];
  if (((active_bezt->f1 | active_bezt->f2 | active_bezt->f3) & SELECT) == 0) {
    /* The active keyframe should always be selected. If it's not selected, it can't be active. */
    return FCURVE_ACTIVE_KEYFRAME_NONE;
  }

  return active_keyframe_index;
}

/** \} */

void BKE_fcurve_keyframe_move_value_with_handles(struct BezTriple *keyframe, const float new_value)
{
  const float value_delta = new_value - keyframe->vec[1][1];
  keyframe->vec[0][1] += value_delta;
  keyframe->vec[1][1] = new_value;
  keyframe->vec[2][1] += value_delta;
}

/* -------------------------------------------------------------------- */
/** \name Status Checks
 * \{ */

bool BKE_fcurve_are_keyframes_usable(FCurve *fcu)
{
  /* F-Curve must exist. */
  if (fcu == NULL) {
    return false;
  }

  /* F-Curve must not have samples - samples are mutually exclusive of keyframes. */
  if (fcu->fpt) {
    return false;
  }

  /* If it has modifiers, none of these should "drastically" alter the curve. */
  if (fcu->modifiers.first) {
    FModifier *fcm;

    /* Check modifiers from last to first, as last will be more influential. */
    /* TODO: optionally, only check modifier if it is the active one... (Joshua Leung 2010) */
    for (fcm = fcu->modifiers.last; fcm; fcm = fcm->prev) {
      /* Ignore if muted/disabled. */
      if (fcm->flag & (FMODIFIER_FLAG_DISABLED | FMODIFIER_FLAG_MUTED)) {
        continue;
      }

      /* Type checks. */
      switch (fcm->type) {
        /* Clearly harmless - do nothing. */
        case FMODIFIER_TYPE_CYCLES:
        case FMODIFIER_TYPE_STEPPED:
        case FMODIFIER_TYPE_NOISE:
          break;

        /* Sometimes harmful - depending on whether they're "additive" or not. */
        case FMODIFIER_TYPE_GENERATOR: {
          FMod_Generator *data = (FMod_Generator *)fcm->data;

          if ((data->flag & FCM_GENERATOR_ADDITIVE) == 0) {
            return false;
          }
          break;
        }
        case FMODIFIER_TYPE_FN_GENERATOR: {
          FMod_FunctionGenerator *data = (FMod_FunctionGenerator *)fcm->data;

          if ((data->flag & FCM_GENERATOR_ADDITIVE) == 0) {
            return false;
          }
          break;
        }
        /* Always harmful - cannot allow. */
        default:
          return false;
      }
    }
  }

  /* Keyframes are usable. */
  return true;
}

bool BKE_fcurve_is_protected(FCurve *fcu)
{
  return ((fcu->flag & FCURVE_PROTECTED) || ((fcu->grp) && (fcu->grp->flag & AGRP_PROTECTED)));
}

bool BKE_fcurve_is_keyframable(FCurve *fcu)
{
  /* F-Curve's keyframes must be "usable" (i.e. visible + have an effect on final result) */
  if (BKE_fcurve_are_keyframes_usable(fcu) == 0) {
    return false;
  }

  /* F-Curve must currently be editable too. */
  if (BKE_fcurve_is_protected(fcu)) {
    return false;
  }

  /* F-Curve is keyframable. */
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Keyframe Column Tools
 * \{ */

static void UNUSED_FUNCTION(bezt_add_to_cfra_elem)(ListBase *lb, BezTriple *bezt)
{
  CfraElem *ce, *cen;

  for (ce = lb->first; ce; ce = ce->next) {
    /* Double key? */
    if (IS_EQT(ce->cfra, bezt->vec[1][0], BEZT_BINARYSEARCH_THRESH)) {
      if (bezt->f2 & SELECT) {
        ce->sel = bezt->f2;
      }
      return;
    }
    /* Should key be inserted before this column? */
    if (ce->cfra > bezt->vec[1][0]) {
      break;
    }
  }

  /* Create a new column */
  cen = MEM_callocN(sizeof(CfraElem), "add_to_cfra_elem");
  if (ce) {
    BLI_insertlinkbefore(lb, ce, cen);
  }
  else {
    BLI_addtail(lb, cen);
  }

  cen->cfra = bezt->vec[1][0];
  cen->sel = bezt->f2;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Samples Utilities
 * \{ */

/* Some utilities for working with FPoints (i.e. 'sampled' animation curve data, such as
 * data imported from BVH/motion-capture files), which are specialized for use with high density
 * datasets, which BezTriples/Keyframe data are ill equipped to do. */

float fcurve_samplingcb_evalcurve(FCurve *fcu, void *UNUSED(data), float evaltime)
{
  /* Assume any interference from drivers on the curve is intended... */
  return evaluate_fcurve(fcu, evaltime);
}

void fcurve_store_samples(FCurve *fcu, void *data, int start, int end, FcuSampleFunc sample_cb)
{
  FPoint *fpt, *new_fpt;
  int cfra;

  /* Sanity checks. */
  /* TODO: make these tests report errors using reports not CLOG's (Joshua Leung 2009) */
  if (ELEM(NULL, fcu, sample_cb)) {
    CLOG_ERROR(&LOG, "No F-Curve with F-Curve Modifiers to Bake");
    return;
  }
  if (start > end) {
    CLOG_ERROR(&LOG, "Error: Frame range for Sampled F-Curve creation is inappropriate");
    return;
  }

  /* Set up sample data. */
  fpt = new_fpt = MEM_callocN(sizeof(FPoint) * (end - start + 1), "FPoint Samples");

  /* Use the sampling callback at 1-frame intervals from start to end frames. */
  for (cfra = start; cfra <= end; cfra++, fpt++) {
    fpt->vec[0] = (float)cfra;
    fpt->vec[1] = sample_cb(fcu, data, (float)cfra);
  }

  /* Free any existing sample/keyframe data on curve. */
  if (fcu->bezt) {
    MEM_freeN(fcu->bezt);
  }
  if (fcu->fpt) {
    MEM_freeN(fcu->fpt);
  }

  /* Store the samples. */
  fcu->bezt = NULL;
  fcu->fpt = new_fpt;
  fcu->totvert = end - start + 1;
}

static void init_unbaked_bezt_data(BezTriple *bezt)
{
  bezt->f1 = bezt->f2 = bezt->f3 = SELECT;
  /* Baked FCurve points always use linear interpolation. */
  bezt->ipo = BEZT_IPO_LIN;
  bezt->h1 = bezt->h2 = HD_AUTO_ANIM;
}

void fcurve_samples_to_keyframes(FCurve *fcu, const int start, const int end)
{

  /* Sanity checks. */
  /* TODO: make these tests report errors using reports not CLOG's (Joshua Leung 2009). */
  if (fcu == NULL) {
    CLOG_ERROR(&LOG, "No F-Curve with F-Curve Modifiers to Un-Bake");
    return;
  }

  if (start > end) {
    CLOG_ERROR(&LOG, "Error: Frame range to unbake F-Curve is inappropriate");
    return;
  }

  if (fcu->fpt == NULL) {
    /* No data to unbake. */
    CLOG_ERROR(&LOG, "Error: Curve contains no baked keyframes");
    return;
  }

  /* Free any existing sample/keyframe data on the curve. */
  if (fcu->bezt) {
    MEM_freeN(fcu->bezt);
  }

  BezTriple *bezt;
  FPoint *fpt = fcu->fpt;
  int keyframes_to_insert = end - start;
  int sample_points = fcu->totvert;

  bezt = fcu->bezt = MEM_callocN(sizeof(*fcu->bezt) * (size_t)keyframes_to_insert, __func__);
  fcu->totvert = keyframes_to_insert;

  /* Get first sample point to 'copy' as keyframe. */
  for (; sample_points && (fpt->vec[0] < start); fpt++, sample_points--) {
    /* pass */
  }

  /* Current position in the timeline. */
  int cur_pos = start;

  /* Add leading dummy flat points if needed. */
  for (; keyframes_to_insert && (fpt->vec[0] > start); cur_pos++, bezt++, keyframes_to_insert--) {
    init_unbaked_bezt_data(bezt);
    bezt->vec[1][0] = (float)cur_pos;
    bezt->vec[1][1] = fpt->vec[1];
  }

  /* Copy actual sample points. */
  for (; keyframes_to_insert && sample_points;
       cur_pos++, bezt++, keyframes_to_insert--, fpt++, sample_points--) {
    init_unbaked_bezt_data(bezt);
    copy_v2_v2(bezt->vec[1], fpt->vec);
  }

  /* Add trailing dummy flat points if needed. */
  for (fpt--; keyframes_to_insert; cur_pos++, bezt++, keyframes_to_insert--) {
    init_unbaked_bezt_data(bezt);
    bezt->vec[1][0] = (float)cur_pos;
    bezt->vec[1][1] = fpt->vec[1];
  }

  MEM_SAFE_FREE(fcu->fpt);

  /* Not strictly needed since we use linear interpolation, but better be consistent here. */
  BKE_fcurve_handles_recalc(fcu);
}

/* ***************************** F-Curve Sanity ********************************* */
/* The functions here are used in various parts of Blender, usually after some editing
 * of keyframe data has occurred. They ensure that keyframe data is properly ordered and
 * that the handles are correct.
 */

eFCU_Cycle_Type BKE_fcurve_get_cycle_type(FCurve *fcu)
{
  FModifier *fcm = fcu->modifiers.first;

  if (!fcm || fcm->type != FMODIFIER_TYPE_CYCLES) {
    return FCU_CYCLE_NONE;
  }

  if (fcm->flag & (FMODIFIER_FLAG_DISABLED | FMODIFIER_FLAG_MUTED)) {
    return FCU_CYCLE_NONE;
  }

  if (fcm->flag & (FMODIFIER_FLAG_RANGERESTRICT | FMODIFIER_FLAG_USEINFLUENCE)) {
    return FCU_CYCLE_NONE;
  }

  FMod_Cycles *data = (FMod_Cycles *)fcm->data;

  if (data && data->after_cycles == 0 && data->before_cycles == 0) {
    if (data->before_mode == FCM_EXTRAPOLATE_CYCLIC &&
        data->after_mode == FCM_EXTRAPOLATE_CYCLIC) {
      return FCU_CYCLE_PERFECT;
    }

    if (ELEM(data->before_mode, FCM_EXTRAPOLATE_CYCLIC, FCM_EXTRAPOLATE_CYCLIC_OFFSET) &&
        ELEM(data->after_mode, FCM_EXTRAPOLATE_CYCLIC, FCM_EXTRAPOLATE_CYCLIC_OFFSET)) {
      return FCU_CYCLE_OFFSET;
    }
  }

  return FCU_CYCLE_NONE;
}

bool BKE_fcurve_is_cyclic(FCurve *fcu)
{
  return BKE_fcurve_get_cycle_type(fcu) != FCU_CYCLE_NONE;
}

/* Shifts 'in' by the difference in coordinates between 'to' and 'from',
 * using 'out' as the output buffer.
 * When 'to' and 'from' are end points of the loop, this moves the 'in' point one loop cycle.
 */
static BezTriple *cycle_offset_triple(
    bool cycle, BezTriple *out, const BezTriple *in, const BezTriple *from, const BezTriple *to)
{
  if (!cycle) {
    return NULL;
  }

  memcpy(out, in, sizeof(BezTriple));

  float delta[3];
  sub_v3_v3v3(delta, to->vec[1], from->vec[1]);

  for (int i = 0; i < 3; i++) {
    add_v3_v3(out->vec[i], delta);
  }

  return out;
}

void BKE_fcurve_handles_recalc_ex(FCurve *fcu, eBezTriple_Flag handle_sel_flag)
{
  BezTriple *bezt, *prev, *next;
  int a = fcu->totvert;

  /* Error checking:
   * - Need at least two points.
   * - Need bezier keys.
   * - Only bezier-interpolation has handles (for now).
   */
  if (ELEM(NULL, fcu, fcu->bezt) || (a < 2) /*|| ELEM(fcu->ipo, BEZT_IPO_CONST, BEZT_IPO_LIN) */) {
    return;
  }

  /* If the first modifier is Cycles, smooth the curve through the cycle. */
  BezTriple *first = &fcu->bezt[0], *last = &fcu->bezt[fcu->totvert - 1];
  BezTriple tmp;

  bool cycle = BKE_fcurve_is_cyclic(fcu) && BEZT_IS_AUTOH(first) && BEZT_IS_AUTOH(last);

  /* Get initial pointers. */
  bezt = fcu->bezt;
  prev = cycle_offset_triple(cycle, &tmp, &fcu->bezt[fcu->totvert - 2], last, first);
  next = (bezt + 1);

  /* Loop over all beztriples, adjusting handles. */
  while (a--) {
    /* Clamp timing of handles to be on either side of beztriple. */
    if (bezt->vec[0][0] > bezt->vec[1][0]) {
      bezt->vec[0][0] = bezt->vec[1][0];
    }
    if (bezt->vec[2][0] < bezt->vec[1][0]) {
      bezt->vec[2][0] = bezt->vec[1][0];
    }

    /* Calculate auto-handles. */
    BKE_nurb_handle_calc_ex(bezt, prev, next, handle_sel_flag, true, fcu->auto_smoothing);

    /* For automatic ease in and out. */
    if (BEZT_IS_AUTOH(bezt) && !cycle) {
      /* Only do this on first or last beztriple. */
      if (ELEM(a, 0, fcu->totvert - 1)) {
        /* Set both handles to have same horizontal value as keyframe. */
        if (fcu->extend == FCURVE_EXTRAPOLATE_CONSTANT) {
          bezt->vec[0][1] = bezt->vec[2][1] = bezt->vec[1][1];
          /* Remember that these keyframes are special, they don't need to be adjusted. */
          bezt->auto_handle_type = HD_AUTOTYPE_LOCKED_FINAL;
        }
      }
    }

    /* Avoid total smoothing failure on duplicate keyframes (can happen during grab). */
    if (prev && prev->vec[1][0] >= bezt->vec[1][0]) {
      prev->auto_handle_type = bezt->auto_handle_type = HD_AUTOTYPE_LOCKED_FINAL;
    }

    /* Advance pointers for next iteration. */
    prev = bezt;

    if (a == 1) {
      next = cycle_offset_triple(cycle, &tmp, &fcu->bezt[1], first, last);
    }
    else {
      next++;
    }

    bezt++;
  }

  /* If cyclic extrapolation and Auto Clamp has triggered, ensure it is symmetric. */
  if (cycle && (first->auto_handle_type != HD_AUTOTYPE_NORMAL ||
                last->auto_handle_type != HD_AUTOTYPE_NORMAL)) {
    first->vec[0][1] = first->vec[2][1] = first->vec[1][1];
    last->vec[0][1] = last->vec[2][1] = last->vec[1][1];
    first->auto_handle_type = last->auto_handle_type = HD_AUTOTYPE_LOCKED_FINAL;
  }

  /* Do a second pass for auto handle: compute the handle to have 0 acceleration step. */
  if (fcu->auto_smoothing != FCURVE_SMOOTH_NONE) {
    BKE_nurb_handle_smooth_fcurve(fcu->bezt, fcu->totvert, cycle);
  }
}

void BKE_fcurve_handles_recalc(FCurve *fcu)
{
  BKE_fcurve_handles_recalc_ex(fcu, SELECT);
}

void testhandles_fcurve(FCurve *fcu, eBezTriple_Flag sel_flag, const bool use_handle)
{
  BezTriple *bezt;
  unsigned int a;

  /* Only beztriples have handles (bpoints don't though). */
  if (ELEM(NULL, fcu, fcu->bezt)) {
    return;
  }

  /* Loop over beztriples. */
  for (a = 0, bezt = fcu->bezt; a < fcu->totvert; a++, bezt++) {
    BKE_nurb_bezt_handle_test(bezt, sel_flag, use_handle, false);
  }

  /* Recalculate handles. */
  BKE_fcurve_handles_recalc_ex(fcu, sel_flag);
}

void sort_time_fcurve(FCurve *fcu)
{
  if (fcu->bezt == NULL) {
    return;
  }

  /* Keep adjusting order of beztriples until nothing moves (bubble-sort). */
  BezTriple *bezt;
  uint a;

  bool ok = true;
  while (ok) {
    ok = 0;
    /* Currently, will only be needed when there are beztriples. */

    /* Loop over ALL points to adjust position in array and recalculate handles. */
    for (a = 0, bezt = fcu->bezt; a < fcu->totvert; a++, bezt++) {
      /* Check if thee's a next beztriple which we could try to swap with current. */
      if (a < (fcu->totvert - 1)) {
        /* Swap if one is after the other (and indicate that order has changed). */
        if (bezt->vec[1][0] > (bezt + 1)->vec[1][0]) {
          SWAP(BezTriple, *bezt, *(bezt + 1));
          ok = 1;
        }
      }
    }
  }

  for (a = 0, bezt = fcu->bezt; a < fcu->totvert; a++, bezt++) {
    /* If either one of both of the points exceeds crosses over the keyframe time... */
    if ((bezt->vec[0][0] > bezt->vec[1][0]) && (bezt->vec[2][0] < bezt->vec[1][0])) {
      /* Swap handles if they have switched sides for some reason. */
      swap_v2_v2(bezt->vec[0], bezt->vec[2]);
    }
    else {
      /* Clamp handles. */
      CLAMP_MAX(bezt->vec[0][0], bezt->vec[1][0]);
      CLAMP_MIN(bezt->vec[2][0], bezt->vec[1][0]);
    }
  }
}

bool test_time_fcurve(FCurve *fcu)
{
  unsigned int a;

  /* Sanity checks. */
  if (fcu == NULL) {
    return false;
  }

  /* Currently, only need to test beztriples. */
  if (fcu->bezt) {
    BezTriple *bezt;

    /* Loop through all BezTriples, stopping when one exceeds the one after it. */
    for (a = 0, bezt = fcu->bezt; a < (fcu->totvert - 1); a++, bezt++) {
      if (bezt->vec[1][0] > (bezt + 1)->vec[1][0]) {
        return true;
      }
    }
  }
  else if (fcu->fpt) {
    FPoint *fpt;

    /* Loop through all FPoints, stopping when one exceeds the one after it. */
    for (a = 0, fpt = fcu->fpt; a < (fcu->totvert - 1); a++, fpt++) {
      if (fpt->vec[0] > (fpt + 1)->vec[0]) {
        return true;
      }
    }
  }

  /* None need any swapping. */
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name F-Curve Calculations
 * \{ */

void BKE_fcurve_correct_bezpart(const float v1[2], float v2[2], float v3[2], const float v4[2])
{
  float h1[2], h2[2], len1, len2, len, fac;

  /* Calculate handle deltas. */
  h1[0] = v1[0] - v2[0];
  h1[1] = v1[1] - v2[1];

  h2[0] = v4[0] - v3[0];
  h2[1] = v4[1] - v3[1];

  /* Calculate distances:
   * - len  = Span of time between keyframes.
   * - len1 = Length of handle of start key.
   * - len2 = Length of handle of end key.
   */
  len = v4[0] - v1[0];
  len1 = fabsf(h1[0]);
  len2 = fabsf(h2[0]);

  /* If the handles have no length, no need to do any corrections. */
  if ((len1 + len2) == 0.0f) {
    return;
  }

  /* To prevent looping or rewinding, handles cannot
   * exceed the adjacent key-frames time position. */
  if (len1 > len) {
    fac = len / len1;
    v2[0] = (v1[0] - fac * h1[0]);
    v2[1] = (v1[1] - fac * h1[1]);
  }

  if (len2 > len) {
    fac = len / len2;
    v3[0] = (v4[0] - fac * h2[0]);
    v3[1] = (v4[1] - fac * h2[1]);
  }
}

/**
 * Find roots of cubic equation (c0 x^3 + c1 x^2 + c2 x + c3)
 * \return number of roots in `o`.
 *
 * \note it is up to the caller to allocate enough memory for `o`.
 */
static int solve_cubic(double c0, double c1, double c2, double c3, float *o)
{
  double a, b, c, p, q, d, t, phi;
  int nr = 0;

  if (c3 != 0.0) {
    a = c2 / c3;
    b = c1 / c3;
    c = c0 / c3;
    a = a / 3;

    p = b / 3 - a * a;
    q = (2 * a * a * a - a * b + c) / 2;
    d = q * q + p * p * p;

    if (d > 0.0) {
      t = sqrt(d);
      o[0] = (float)(sqrt3d(-q + t) + sqrt3d(-q - t) - a);

      if ((o[0] >= (float)SMALL) && (o[0] <= 1.000001f)) {
        return 1;
      }
      return 0;
    }

    if (d == 0.0) {
      t = sqrt3d(-q);
      o[0] = (float)(2 * t - a);

      if ((o[0] >= (float)SMALL) && (o[0] <= 1.000001f)) {
        nr++;
      }
      o[nr] = (float)(-t - a);

      if ((o[nr] >= (float)SMALL) && (o[nr] <= 1.000001f)) {
        return nr + 1;
      }
      return nr;
    }

    phi = acos(-q / sqrt(-(p * p * p)));
    t = sqrt(-p);
    p = cos(phi / 3);
    q = sqrt(3 - 3 * p * p);
    o[0] = (float)(2 * t * p - a);

    if ((o[0] >= (float)SMALL) && (o[0] <= 1.000001f)) {
      nr++;
    }
    o[nr] = (float)(-t * (p + q) - a);

    if ((o[nr] >= (float)SMALL) && (o[nr] <= 1.000001f)) {
      nr++;
    }
    o[nr] = (float)(-t * (p - q) - a);

    if ((o[nr] >= (float)SMALL) && (o[nr] <= 1.000001f)) {
      return nr + 1;
    }
    return nr;
  }
  a = c2;
  b = c1;
  c = c0;

  if (a != 0.0) {
    /* Discriminant */
    p = b * b - 4 * a * c;

    if (p > 0) {
      p = sqrt(p);
      o[0] = (float)((-b - p) / (2 * a));

      if ((o[0] >= (float)SMALL) && (o[0] <= 1.000001f)) {
        nr++;
      }
      o[nr] = (float)((-b + p) / (2 * a));

      if ((o[nr] >= (float)SMALL) && (o[nr] <= 1.000001f)) {
        return nr + 1;
      }
      return nr;
    }

    if (p == 0) {
      o[0] = (float)(-b / (2 * a));
      if ((o[0] >= (float)SMALL) && (o[0] <= 1.000001f)) {
        return 1;
      }
    }

    return 0;
  }

  if (b != 0.0) {
    o[0] = (float)(-c / b);

    if ((o[0] >= (float)SMALL) && (o[0] <= 1.000001f)) {
      return 1;
    }
    return 0;
  }

  if (c == 0.0) {
    o[0] = 0.0;
    return 1;
  }

  return 0;
}

/* Find root(s) ('zero') of a Bezier curve. */
static int findzero(float x, float q0, float q1, float q2, float q3, float *o)
{
  const double c0 = q0 - x;
  const double c1 = 3.0f * (q1 - q0);
  const double c2 = 3.0f * (q0 - 2.0f * q1 + q2);
  const double c3 = q3 - q0 + 3.0f * (q1 - q2);

  return solve_cubic(c0, c1, c2, c3, o);
}

static void berekeny(float f1, float f2, float f3, float f4, float *o, int b)
{
  float t, c0, c1, c2, c3;
  int a;

  c0 = f1;
  c1 = 3.0f * (f2 - f1);
  c2 = 3.0f * (f1 - 2.0f * f2 + f3);
  c3 = f4 - f1 + 3.0f * (f2 - f3);

  for (a = 0; a < b; a++) {
    t = o[a];
    o[a] = c0 + t * c1 + t * t * c2 + t * t * t * c3;
  }
}

static void fcurve_bezt_free(FCurve *fcu)
{
  MEM_SAFE_FREE(fcu->bezt);
  fcu->totvert = 0;
}

bool BKE_fcurve_bezt_subdivide_handles(struct BezTriple *bezt,
                                       struct BezTriple *prev,
                                       struct BezTriple *next,
                                       float *r_pdelta)
{
  /* The four points that make up this section of the Bezier curve. */
  const float *prev_coords = prev->vec[1];
  float *prev_handle_right = prev->vec[2];
  float *next_handle_left = next->vec[0];
  const float *next_coords = next->vec[1];

  float *new_handle_left = bezt->vec[0];
  const float *new_coords = bezt->vec[1];
  float *new_handle_right = bezt->vec[2];

  if (new_coords[0] <= prev_coords[0] || new_coords[0] >= next_coords[0]) {
    /* The new keyframe is outside the (prev_coords, next_coords) range. */
    return false;
  }

  /* Apply evaluation-time limits and compute the effective curve. */
  BKE_fcurve_correct_bezpart(prev_coords, prev_handle_right, next_handle_left, next_coords);
  float roots[4];
  if (!findzero(new_coords[0],
                prev_coords[0],
                prev_handle_right[0],
                next_handle_left[0],
                next_coords[0],
                roots)) {
    return false;
  }

  const float t = roots[0]; /* Percentage of the curve at which the split should occur. */
  if (t <= 0.0f || t >= 1.0f) {
    /* The split would occur outside the curve, which isn't possible. */
    return false;
  }

  /* De Casteljau split, requires three iterations of splitting.
   * See https://pomax.github.io/bezierinfo/#decasteljau */
  float split1[3][2], split2[2][2], split3[2];
  interp_v2_v2v2(split1[0], prev_coords, prev_handle_right, t);
  interp_v2_v2v2(split1[1], prev_handle_right, next_handle_left, t);
  interp_v2_v2v2(split1[2], next_handle_left, next_coords, t);
  interp_v2_v2v2(split2[0], split1[0], split1[1], t);
  interp_v2_v2v2(split2[1], split1[1], split1[2], t);
  interp_v2_v2v2(split3, split2[0], split2[1], t);

  /* Update the existing handles. */
  copy_v2_v2(prev_handle_right, split1[0]);
  copy_v2_v2(next_handle_left, split1[2]);

  float diff_coords[2];
  sub_v2_v2v2(diff_coords, new_coords, split3);
  add_v2_v2v2(new_handle_left, split2[0], diff_coords);
  add_v2_v2v2(new_handle_right, split2[1], diff_coords);

  *r_pdelta = diff_coords[1];
  return true;
}

void BKE_fcurve_delete_key(FCurve *fcu, int index)
{
  /* sanity check */
  if (fcu == NULL) {
    return;
  }

  /* verify the index:
   * 1) cannot be greater than the number of available keyframes
   * 2) negative indices are for specifying a value from the end of the array
   */
  if (abs(index) >= fcu->totvert) {
    return;
  }
  if (index < 0) {
    index += fcu->totvert;
  }

  /* Delete this keyframe */
  memmove(
      &fcu->bezt[index], &fcu->bezt[index + 1], sizeof(BezTriple) * (fcu->totvert - index - 1));
  fcu->totvert--;

  /* Free the array of BezTriples if there are not keyframes */
  if (fcu->totvert == 0) {
    fcurve_bezt_free(fcu);
  }
}

bool BKE_fcurve_delete_keys_selected(FCurve *fcu)
{
  bool changed = false;

  if (fcu->bezt == NULL) { /* ignore baked curves */
    return false;
  }

  /* Delete selected BezTriples */
  for (int i = 0; i < fcu->totvert; i++) {
    if (fcu->bezt[i].f2 & SELECT) {
      if (i == fcu->active_keyframe_index) {
        BKE_fcurve_active_keyframe_set(fcu, NULL);
      }
      memmove(&fcu->bezt[i], &fcu->bezt[i + 1], sizeof(BezTriple) * (fcu->totvert - i - 1));
      fcu->totvert--;
      i--;
      changed = true;
    }
  }

  /* Free the array of BezTriples if there are not keyframes */
  if (fcu->totvert == 0) {
    fcurve_bezt_free(fcu);
  }

  return changed;
}

void BKE_fcurve_delete_keys_all(FCurve *fcu)
{
  fcurve_bezt_free(fcu);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name F-Curve Evaluation
 * \{ */

static float fcurve_eval_keyframes_extrapolate(
    FCurve *fcu, BezTriple *bezts, float evaltime, int endpoint_offset, int direction_to_neighbor)
{
  BezTriple *endpoint_bezt = bezts + endpoint_offset; /* The first/last keyframe. */
  BezTriple *neighbor_bezt = endpoint_bezt +
                             direction_to_neighbor; /* The second (to last) keyframe. */

  if (endpoint_bezt->ipo == BEZT_IPO_CONST || fcu->extend == FCURVE_EXTRAPOLATE_CONSTANT ||
      (fcu->flag & FCURVE_DISCRETE_VALUES) != 0) {
    /* Constant (BEZT_IPO_HORIZ) extrapolation or constant interpolation, so just extend the
     * endpoint's value. */
    return endpoint_bezt->vec[1][1];
  }

  if (endpoint_bezt->ipo == BEZT_IPO_LIN) {
    /* Use the next center point instead of our own handle for linear interpolated extrapolate. */
    if (fcu->totvert == 1) {
      return endpoint_bezt->vec[1][1];
    }

    float dx = endpoint_bezt->vec[1][0] - evaltime;
    float fac = neighbor_bezt->vec[1][0] - endpoint_bezt->vec[1][0];

    /* Prevent division by zero. */
    if (fac == 0.0f) {
      return endpoint_bezt->vec[1][1];
    }

    fac = (neighbor_bezt->vec[1][1] - endpoint_bezt->vec[1][1]) / fac;
    return endpoint_bezt->vec[1][1] - (fac * dx);
  }

  /* Use the gradient of the second handle (later) of neighbor to calculate the gradient and thus
   * the value of the curve at evaluation time. */
  int handle = direction_to_neighbor > 0 ? 0 : 2;
  float dx = endpoint_bezt->vec[1][0] - evaltime;
  float fac = endpoint_bezt->vec[1][0] - endpoint_bezt->vec[handle][0];

  /* Prevent division by zero. */
  if (fac == 0.0f) {
    return endpoint_bezt->vec[1][1];
  }

  fac = (endpoint_bezt->vec[1][1] - endpoint_bezt->vec[handle][1]) / fac;
  return endpoint_bezt->vec[1][1] - (fac * dx);
}

static float fcurve_eval_keyframes_interpolate(FCurve *fcu, BezTriple *bezts, float evaltime)
{
  const float eps = 1.e-8f;
  BezTriple *bezt, *prevbezt;
  unsigned int a;

  /* Evaltime occurs somewhere in the middle of the curve. */
  bool exact = false;

  /* Use binary search to find appropriate keyframes...
   *
   * The threshold here has the following constraints:
   * - 0.001 is too coarse:
   *   We get artifacts with 2cm driver movements at 1BU = 1m (see T40332).
   *
   * - 0.00001 is too fine:
   *   Weird errors, like selecting the wrong keyframe range (see T39207), occur.
   *   This lower bound was established in b888a32eee8147b028464336ad2404d8155c64dd.
   */
  a = BKE_fcurve_bezt_binarysearch_index_ex(bezts, evaltime, fcu->totvert, 0.0001, &exact);
  bezt = bezts + a;

  if (exact) {
    /* Index returned must be interpreted differently when it sits on top of an existing keyframe
     * - That keyframe is the start of the segment we need (see action_bug_2.blend in T39207).
     */
    return bezt->vec[1][1];
  }

  /* Index returned refers to the keyframe that the eval-time occurs *before*
   * - hence, that keyframe marks the start of the segment we're dealing with.
   */
  prevbezt = (a > 0) ? (bezt - 1) : bezt;

  /* Use if the key is directly on the frame, in rare cases this is needed else we get 0.0 instead.
   * XXX: consult T39207 for examples of files where failure of these checks can cause issues. */
  if (fabsf(bezt->vec[1][0] - evaltime) < eps) {
    return bezt->vec[1][1];
  }

  if (evaltime < prevbezt->vec[1][0] || bezt->vec[1][0] < evaltime) {
    if (G.debug & G_DEBUG) {
      printf("   ERROR: failed eval - p=%f b=%f, t=%f (%f)\n",
             prevbezt->vec[1][0],
             bezt->vec[1][0],
             evaltime,
             fabsf(bezt->vec[1][0] - evaltime));
    }
    return 0.0f;
  }

  /* Evaltime occurs within the interval defined by these two keyframes. */
  const float begin = prevbezt->vec[1][1];
  const float change = bezt->vec[1][1] - prevbezt->vec[1][1];
  const float duration = bezt->vec[1][0] - prevbezt->vec[1][0];
  const float time = evaltime - prevbezt->vec[1][0];
  const float amplitude = prevbezt->amplitude;
  const float period = prevbezt->period;

  /* Value depends on interpolation mode. */
  if ((prevbezt->ipo == BEZT_IPO_CONST) || (fcu->flag & FCURVE_DISCRETE_VALUES) ||
      (duration == 0)) {
    /* Constant (evaltime not relevant, so no interpolation needed). */
    return prevbezt->vec[1][1];
  }

  switch (prevbezt->ipo) {
    /* Interpolation ...................................... */
    case BEZT_IPO_BEZ: {
      float v1[2], v2[2], v3[2], v4[2], opl[32];

      /* Bezier interpolation. */
      /* (v1, v2) are the first keyframe and its 2nd handle. */
      v1[0] = prevbezt->vec[1][0];
      v1[1] = prevbezt->vec[1][1];
      v2[0] = prevbezt->vec[2][0];
      v2[1] = prevbezt->vec[2][1];
      /* (v3, v4) are the last keyframe's 1st handle + the last keyframe. */
      v3[0] = bezt->vec[0][0];
      v3[1] = bezt->vec[0][1];
      v4[0] = bezt->vec[1][0];
      v4[1] = bezt->vec[1][1];

      if (fabsf(v1[1] - v4[1]) < FLT_EPSILON && fabsf(v2[1] - v3[1]) < FLT_EPSILON &&
          fabsf(v3[1] - v4[1]) < FLT_EPSILON) {
        /* Optimization: If all the handles are flat/at the same values,
         * the value is simply the shared value (see T40372 -> F91346).
         */
        return v1[1];
      }
      /* Adjust handles so that they don't overlap (forming a loop). */
      BKE_fcurve_correct_bezpart(v1, v2, v3, v4);

      /* Try to get a value for this position - if failure, try another set of points. */
      if (!findzero(evaltime, v1[0], v2[0], v3[0], v4[0], opl)) {
        if (G.debug & G_DEBUG) {
          printf("    ERROR: findzero() failed at %f with %f %f %f %f\n",
                 evaltime,
                 v1[0],
                 v2[0],
                 v3[0],
                 v4[0]);
        }
        return 0.0;
      }

      berekeny(v1[1], v2[1], v3[1], v4[1], opl, 1);
      return opl[0];
    }
    case BEZT_IPO_LIN:
      /* Linear - simply linearly interpolate between values of the two keyframes. */
      return BLI_easing_linear_ease(time, begin, change, duration);

    /* Easing ............................................ */
    case BEZT_IPO_BACK:
      switch (prevbezt->easing) {
        case BEZT_IPO_EASE_IN:
          return BLI_easing_back_ease_in(time, begin, change, duration, prevbezt->back);
        case BEZT_IPO_EASE_OUT:
          return BLI_easing_back_ease_out(time, begin, change, duration, prevbezt->back);
        case BEZT_IPO_EASE_IN_OUT:
          return BLI_easing_back_ease_in_out(time, begin, change, duration, prevbezt->back);

        default: /* Default/Auto: same as ease out. */
          return BLI_easing_back_ease_out(time, begin, change, duration, prevbezt->back);
      }
      break;

    case BEZT_IPO_BOUNCE:
      switch (prevbezt->easing) {
        case BEZT_IPO_EASE_IN:
          return BLI_easing_bounce_ease_in(time, begin, change, duration);
        case BEZT_IPO_EASE_OUT:
          return BLI_easing_bounce_ease_out(time, begin, change, duration);
        case BEZT_IPO_EASE_IN_OUT:
          return BLI_easing_bounce_ease_in_out(time, begin, change, duration);

        default: /* Default/Auto: same as ease out. */
          return BLI_easing_bounce_ease_out(time, begin, change, duration);
      }
      break;

    case BEZT_IPO_CIRC:
      switch (prevbezt->easing) {
        case BEZT_IPO_EASE_IN:
          return BLI_easing_circ_ease_in(time, begin, change, duration);
        case BEZT_IPO_EASE_OUT:
          return BLI_easing_circ_ease_out(time, begin, change, duration);
        case BEZT_IPO_EASE_IN_OUT:
          return BLI_easing_circ_ease_in_out(time, begin, change, duration);

        default: /* Default/Auto: same as ease in. */
          return BLI_easing_circ_ease_in(time, begin, change, duration);
      }
      break;

    case BEZT_IPO_CUBIC:
      switch (prevbezt->easing) {
        case BEZT_IPO_EASE_IN:
          return BLI_easing_cubic_ease_in(time, begin, change, duration);
        case BEZT_IPO_EASE_OUT:
          return BLI_easing_cubic_ease_out(time, begin, change, duration);
        case BEZT_IPO_EASE_IN_OUT:
          return BLI_easing_cubic_ease_in_out(time, begin, change, duration);

        default: /* Default/Auto: same as ease in. */
          return BLI_easing_cubic_ease_in(time, begin, change, duration);
      }
      break;

    case BEZT_IPO_ELASTIC:
      switch (prevbezt->easing) {
        case BEZT_IPO_EASE_IN:
          return BLI_easing_elastic_ease_in(time, begin, change, duration, amplitude, period);
        case BEZT_IPO_EASE_OUT:
          return BLI_easing_elastic_ease_out(time, begin, change, duration, amplitude, period);
        case BEZT_IPO_EASE_IN_OUT:
          return BLI_easing_elastic_ease_in_out(time, begin, change, duration, amplitude, period);

        default: /* Default/Auto: same as ease out. */
          return BLI_easing_elastic_ease_out(time, begin, change, duration, amplitude, period);
      }
      break;

    case BEZT_IPO_EXPO:
      switch (prevbezt->easing) {
        case BEZT_IPO_EASE_IN:
          return BLI_easing_expo_ease_in(time, begin, change, duration);
        case BEZT_IPO_EASE_OUT:
          return BLI_easing_expo_ease_out(time, begin, change, duration);
        case BEZT_IPO_EASE_IN_OUT:
          return BLI_easing_expo_ease_in_out(time, begin, change, duration);

        default: /* Default/Auto: same as ease in. */
          return BLI_easing_expo_ease_in(time, begin, change, duration);
      }
      break;

    case BEZT_IPO_QUAD:
      switch (prevbezt->easing) {
        case BEZT_IPO_EASE_IN:
          return BLI_easing_quad_ease_in(time, begin, change, duration);
        case BEZT_IPO_EASE_OUT:
          return BLI_easing_quad_ease_out(time, begin, change, duration);
        case BEZT_IPO_EASE_IN_OUT:
          return BLI_easing_quad_ease_in_out(time, begin, change, duration);

        default: /* Default/Auto: same as ease in. */
          return BLI_easing_quad_ease_in(time, begin, change, duration);
      }
      break;

    case BEZT_IPO_QUART:
      switch (prevbezt->easing) {
        case BEZT_IPO_EASE_IN:
          return BLI_easing_quart_ease_in(time, begin, change, duration);
        case BEZT_IPO_EASE_OUT:
          return BLI_easing_quart_ease_out(time, begin, change, duration);
        case BEZT_IPO_EASE_IN_OUT:
          return BLI_easing_quart_ease_in_out(time, begin, change, duration);

        default: /* Default/Auto: same as ease in. */
          return BLI_easing_quart_ease_in(time, begin, change, duration);
      }
      break;

    case BEZT_IPO_QUINT:
      switch (prevbezt->easing) {
        case BEZT_IPO_EASE_IN:
          return BLI_easing_quint_ease_in(time, begin, change, duration);
        case BEZT_IPO_EASE_OUT:
          return BLI_easing_quint_ease_out(time, begin, change, duration);
        case BEZT_IPO_EASE_IN_OUT:
          return BLI_easing_quint_ease_in_out(time, begin, change, duration);

        default: /* Default/Auto: same as ease in. */
          return BLI_easing_quint_ease_in(time, begin, change, duration);
      }
      break;

    case BEZT_IPO_SINE:
      switch (prevbezt->easing) {
        case BEZT_IPO_EASE_IN:
          return BLI_easing_sine_ease_in(time, begin, change, duration);
        case BEZT_IPO_EASE_OUT:
          return BLI_easing_sine_ease_out(time, begin, change, duration);
        case BEZT_IPO_EASE_IN_OUT:
          return BLI_easing_sine_ease_in_out(time, begin, change, duration);

        default: /* Default/Auto: same as ease in. */
          return BLI_easing_sine_ease_in(time, begin, change, duration);
      }
      break;

    default:
      return prevbezt->vec[1][1];
  }

  return 0.0f;
}

/* Calculate F-Curve value for 'evaltime' using #BezTriple keyframes. */
static float fcurve_eval_keyframes(FCurve *fcu, BezTriple *bezts, float evaltime)
{
  if (evaltime <= bezts->vec[1][0]) {
    return fcurve_eval_keyframes_extrapolate(fcu, bezts, evaltime, 0, +1);
  }

  BezTriple *lastbezt = bezts + fcu->totvert - 1;
  if (lastbezt->vec[1][0] <= evaltime) {
    return fcurve_eval_keyframes_extrapolate(fcu, bezts, evaltime, fcu->totvert - 1, -1);
  }

  return fcurve_eval_keyframes_interpolate(fcu, bezts, evaltime);
}

/* Calculate F-Curve value for 'evaltime' using #FPoint samples. */
static float fcurve_eval_samples(FCurve *fcu, FPoint *fpts, float evaltime)
{
  FPoint *prevfpt, *lastfpt, *fpt;
  float cvalue = 0.0f;

  /* Get pointers. */
  prevfpt = fpts;
  lastfpt = prevfpt + fcu->totvert - 1;

  /* Evaluation time at or past endpoints? */
  if (prevfpt->vec[0] >= evaltime) {
    /* Before or on first sample, so just extend value. */
    cvalue = prevfpt->vec[1];
  }
  else if (lastfpt->vec[0] <= evaltime) {
    /* After or on last sample, so just extend value. */
    cvalue = lastfpt->vec[1];
  }
  else {
    float t = fabsf(evaltime - floorf(evaltime));

    /* Find the one on the right frame (assume that these are spaced on 1-frame intervals). */
    fpt = prevfpt + ((int)evaltime - (int)prevfpt->vec[0]);

    /* If not exactly on the frame, perform linear interpolation with the next one. */
    if ((t != 0.0f) && (t < 1.0f)) {
      cvalue = interpf(fpt->vec[1], (fpt + 1)->vec[1], 1.0f - t);
    }
    else {
      cvalue = fpt->vec[1];
    }
  }

  return cvalue;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name F-Curve - Evaluation
 * \{ */

/* Evaluate and return the value of the given F-Curve at the specified frame ("evaltime")
 * NOTE: this is also used for drivers.
 */
static float evaluate_fcurve_ex(FCurve *fcu, float evaltime, float cvalue)
{
  float devaltime;

  /* Evaluate modifiers which modify time to evaluate the base curve at. */
  FModifiersStackStorage storage;
  storage.modifier_count = BLI_listbase_count(&fcu->modifiers);
  storage.size_per_modifier = evaluate_fmodifiers_storage_size_per_modifier(&fcu->modifiers);
  storage.buffer = alloca(storage.modifier_count * storage.size_per_modifier);

  devaltime = evaluate_time_fmodifiers(&storage, &fcu->modifiers, fcu, cvalue, evaltime);

  /* Evaluate curve-data
   * - 'devaltime' instead of 'evaltime', as this is the time that the last time-modifying
   *   F-Curve modifier on the stack requested the curve to be evaluated at.
   */
  if (fcu->bezt) {
    cvalue = fcurve_eval_keyframes(fcu, fcu->bezt, devaltime);
  }
  else if (fcu->fpt) {
    cvalue = fcurve_eval_samples(fcu, fcu->fpt, devaltime);
  }

  /* Evaluate modifiers. */
  evaluate_value_fmodifiers(&storage, &fcu->modifiers, fcu, &cvalue, devaltime);

  /* If curve can only have integral values, perform truncation (i.e. drop the decimal part)
   * here so that the curve can be sampled correctly.
   */
  if (fcu->flag & FCURVE_INT_VALUES) {
    cvalue = floorf(cvalue + 0.5f);
  }

  return cvalue;
}

float evaluate_fcurve(FCurve *fcu, float evaltime)
{
  BLI_assert(fcu->driver == NULL);

  return evaluate_fcurve_ex(fcu, evaltime, 0.0);
}

float evaluate_fcurve_only_curve(FCurve *fcu, float evaltime)
{
  /* Can be used to evaluate the (key-framed) f-curve only.
   * Also works for driver-f-curves when the driver itself is not relevant.
   * E.g. when inserting a keyframe in a driver f-curve. */
  return evaluate_fcurve_ex(fcu, evaltime, 0.0);
}

float evaluate_fcurve_driver(PathResolvedRNA *anim_rna,
                             FCurve *fcu,
                             ChannelDriver *driver_orig,
                             const AnimationEvalContext *anim_eval_context)
{
  BLI_assert(fcu->driver != NULL);
  float cvalue = 0.0f;
  float evaltime = anim_eval_context->eval_time;

  /* If there is a driver (only if this F-Curve is acting as 'driver'),
   * evaluate it to find value to use as "evaltime" since drivers essentially act as alternative
   * input (i.e. in place of 'time') for F-Curves. */
  if (fcu->driver) {
    /* Evaltime now serves as input for the curve. */
    evaltime = evaluate_driver(anim_rna, fcu->driver, driver_orig, anim_eval_context);

    /* Only do a default 1-1 mapping if it's unlikely that anything else will set a value... */
    if (fcu->totvert == 0) {
      FModifier *fcm;
      bool do_linear = true;

      /* Out-of-range F-Modifiers will block, as will those which just plain overwrite the values
       * XXX: additive is a bit more dicey; it really depends then if things are in range or not...
       */
      for (fcm = fcu->modifiers.first; fcm; fcm = fcm->next) {
        /* If there are range-restrictions, we must definitely block T36950. */
        if ((fcm->flag & FMODIFIER_FLAG_RANGERESTRICT) == 0 ||
            ((fcm->sfra <= evaltime) && (fcm->efra >= evaltime))) {
          /* Within range: here it probably doesn't matter,
           * though we'd want to check on additive. */
        }
        else {
          /* Outside range: modifier shouldn't contribute to the curve here,
           * though it does in other areas, so neither should the driver! */
          do_linear = false;
        }
      }

      /* Only copy over results if none of the modifiers disagreed with this. */
      if (do_linear) {
        cvalue = evaltime;
      }
    }
  }

  return evaluate_fcurve_ex(fcu, evaltime, cvalue);
}

bool BKE_fcurve_is_empty(FCurve *fcu)
{
  return (fcu->totvert == 0) && (fcu->driver == NULL) &&
         !list_has_suitable_fmodifier(&fcu->modifiers, 0, FMI_TYPE_GENERATE_CURVE);
}

float calculate_fcurve(PathResolvedRNA *anim_rna,
                       FCurve *fcu,
                       const AnimationEvalContext *anim_eval_context)
{
  /* Only calculate + set curval (overriding the existing value) if curve has
   * any data which warrants this...
   */
  if (BKE_fcurve_is_empty(fcu)) {
    return 0.0f;
  }

  /* Calculate and set curval (evaluates driver too if necessary). */
  float curval;
  if (fcu->driver) {
    curval = evaluate_fcurve_driver(anim_rna, fcu, fcu->driver, anim_eval_context);
  }
  else {
    curval = evaluate_fcurve(fcu, anim_eval_context->eval_time);
  }
  fcu->curval = curval; /* Debug display only, not thread safe! */
  return curval;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name F-Curve - .blend file API
 * \{ */

void BKE_fmodifiers_blend_write(BlendWriter *writer, ListBase *fmodifiers)
{
  /* Write all modifiers first (for faster reloading) */
  BLO_write_struct_list(writer, FModifier, fmodifiers);

  /* Modifiers */
  LISTBASE_FOREACH (FModifier *, fcm, fmodifiers) {
    const FModifierTypeInfo *fmi = fmodifier_get_typeinfo(fcm);

    /* Write the specific data */
    if (fmi && fcm->data) {
      /* firstly, just write the plain fmi->data struct */
      BLO_write_struct_by_name(writer, fmi->structName, fcm->data);

      /* do any modifier specific stuff */
      switch (fcm->type) {
        case FMODIFIER_TYPE_GENERATOR: {
          FMod_Generator *data = fcm->data;

          /* write coefficients array */
          if (data->coefficients) {
            BLO_write_float_array(writer, data->arraysize, data->coefficients);
          }

          break;
        }
        case FMODIFIER_TYPE_ENVELOPE: {
          FMod_Envelope *data = fcm->data;

          /* write envelope data */
          if (data->data) {
            BLO_write_struct_array(writer, FCM_EnvelopeData, data->totvert, data->data);
          }

          break;
        }
        case FMODIFIER_TYPE_PYTHON: {
          FMod_Python *data = fcm->data;

          /* Write ID Properties -- and copy this comment EXACTLY for easy finding
           * of library blocks that implement this. */
          IDP_BlendWrite(writer, data->prop);

          break;
        }
      }
    }
  }
}

void BKE_fmodifiers_blend_read_data(BlendDataReader *reader, ListBase *fmodifiers, FCurve *curve)
{
  LISTBASE_FOREACH (FModifier *, fcm, fmodifiers) {
    /* relink general data */
    BLO_read_data_address(reader, &fcm->data);
    fcm->curve = curve;

    /* do relinking of data for specific types */
    switch (fcm->type) {
      case FMODIFIER_TYPE_GENERATOR: {
        FMod_Generator *data = (FMod_Generator *)fcm->data;
        BLO_read_float_array(reader, data->arraysize, &data->coefficients);
        break;
      }
      case FMODIFIER_TYPE_ENVELOPE: {
        FMod_Envelope *data = (FMod_Envelope *)fcm->data;

        BLO_read_data_address(reader, &data->data);

        break;
      }
      case FMODIFIER_TYPE_PYTHON: {
        FMod_Python *data = (FMod_Python *)fcm->data;

        BLO_read_data_address(reader, &data->prop);
        IDP_BlendDataRead(reader, &data->prop);

        break;
      }
    }
  }
}

void BKE_fmodifiers_blend_read_lib(BlendLibReader *reader, ID *id, ListBase *fmodifiers)
{
  LISTBASE_FOREACH (FModifier *, fcm, fmodifiers) {
    /* data for specific modifiers */
    switch (fcm->type) {
      case FMODIFIER_TYPE_PYTHON: {
        FMod_Python *data = (FMod_Python *)fcm->data;
        BLO_read_id_address(reader, id->lib, &data->script);
        break;
      }
    }
  }
}

void BKE_fmodifiers_blend_read_expand(BlendExpander *expander, ListBase *fmodifiers)
{
  LISTBASE_FOREACH (FModifier *, fcm, fmodifiers) {
    /* library data for specific F-Modifier types */
    switch (fcm->type) {
      case FMODIFIER_TYPE_PYTHON: {
        FMod_Python *data = (FMod_Python *)fcm->data;
        BLO_expand(expander, data->script);
        break;
      }
    }
  }
}

void BKE_fcurve_blend_write(BlendWriter *writer, ListBase *fcurves)
{
  BLO_write_struct_list(writer, FCurve, fcurves);
  LISTBASE_FOREACH (FCurve *, fcu, fcurves) {
    /* curve data */
    if (fcu->bezt) {
      BLO_write_struct_array(writer, BezTriple, fcu->totvert, fcu->bezt);
    }
    if (fcu->fpt) {
      BLO_write_struct_array(writer, FPoint, fcu->totvert, fcu->fpt);
    }

    if (fcu->rna_path) {
      BLO_write_string(writer, fcu->rna_path);
    }

    /* driver data */
    if (fcu->driver) {
      ChannelDriver *driver = fcu->driver;

      BLO_write_struct(writer, ChannelDriver, driver);

      /* variables */
      BLO_write_struct_list(writer, DriverVar, &driver->variables);
      LISTBASE_FOREACH (DriverVar *, dvar, &driver->variables) {
        DRIVER_TARGETS_USED_LOOPER_BEGIN (dvar) {
          if (dtar->rna_path) {
            BLO_write_string(writer, dtar->rna_path);
          }
        }
        DRIVER_TARGETS_LOOPER_END;
      }
    }

    /* write F-Modifiers */
    BKE_fmodifiers_blend_write(writer, &fcu->modifiers);
  }
}

void BKE_fcurve_blend_read_data(BlendDataReader *reader, ListBase *fcurves)
{
  /* link F-Curve data to F-Curve again (non ID-libs) */
  LISTBASE_FOREACH (FCurve *, fcu, fcurves) {
    /* curve data */
    BLO_read_data_address(reader, &fcu->bezt);
    BLO_read_data_address(reader, &fcu->fpt);

    /* rna path */
    BLO_read_data_address(reader, &fcu->rna_path);

    /* group */
    BLO_read_data_address(reader, &fcu->grp);

    /* clear disabled flag - allows disabled drivers to be tried again (T32155),
     * but also means that another method for "reviving disabled F-Curves" exists
     */
    fcu->flag &= ~FCURVE_DISABLED;

    /* driver */
    BLO_read_data_address(reader, &fcu->driver);
    if (fcu->driver) {
      ChannelDriver *driver = fcu->driver;

      /* Compiled expression data will need to be regenerated
       * (old pointer may still be set here). */
      driver->expr_comp = NULL;
      driver->expr_simple = NULL;

      /* Give the driver a fresh chance - the operating environment may be different now
       * (addons, etc. may be different) so the driver namespace may be sane now T32155. */
      driver->flag &= ~DRIVER_FLAG_INVALID;

      /* relink variables, targets and their paths */
      BLO_read_list(reader, &driver->variables);
      LISTBASE_FOREACH (DriverVar *, dvar, &driver->variables) {
        DRIVER_TARGETS_LOOPER_BEGIN (dvar) {
          /* only relink the targets being used */
          if (tarIndex < dvar->num_targets) {
            BLO_read_data_address(reader, &dtar->rna_path);
          }
          else {
            dtar->rna_path = NULL;
          }
        }
        DRIVER_TARGETS_LOOPER_END;
      }
    }

    /* modifiers */
    BLO_read_list(reader, &fcu->modifiers);
    BKE_fmodifiers_blend_read_data(reader, &fcu->modifiers, fcu);
  }
}

void BKE_fcurve_blend_read_lib(BlendLibReader *reader, ID *id, ListBase *fcurves)
{
  if (fcurves == NULL) {
    return;
  }

  /* relink ID-block references... */
  LISTBASE_FOREACH (FCurve *, fcu, fcurves) {
    /* driver data */
    if (fcu->driver) {
      ChannelDriver *driver = fcu->driver;
      LISTBASE_FOREACH (DriverVar *, dvar, &driver->variables) {
        DRIVER_TARGETS_LOOPER_BEGIN (dvar) {
          /* only relink if still used */
          if (tarIndex < dvar->num_targets) {
            BLO_read_id_address(reader, id->lib, &dtar->id);
          }
          else {
            dtar->id = NULL;
          }
        }
        DRIVER_TARGETS_LOOPER_END;
      }
    }

    /* modifiers */
    BKE_fmodifiers_blend_read_lib(reader, id, &fcu->modifiers);
  }
}

void BKE_fcurve_blend_read_expand(BlendExpander *expander, ListBase *fcurves)
{
  LISTBASE_FOREACH (FCurve *, fcu, fcurves) {
    /* Driver targets if there is a driver */
    if (fcu->driver) {
      ChannelDriver *driver = fcu->driver;

      LISTBASE_FOREACH (DriverVar *, dvar, &driver->variables) {
        DRIVER_TARGETS_LOOPER_BEGIN (dvar) {
          // TODO: only expand those that are going to get used?
          BLO_expand(expander, dtar->id);
        }
        DRIVER_TARGETS_LOOPER_END;
      }
    }

    /* F-Curve Modifiers */
    BKE_fmodifiers_blend_read_expand(expander, &fcu->modifiers);
  }
}

/** \} */
