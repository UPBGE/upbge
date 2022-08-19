/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup edtransform
 */

#include "DNA_anim_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_mask_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_rect.h"

#include "BKE_context.h"
#include "BKE_gpencil.h"
#include "BKE_key.h"
#include "BKE_mask.h"
#include "BKE_nla.h"

#include "ED_anim_api.h"
#include "ED_keyframes_edit.h"
#include "ED_markers.h"

#include "WM_api.h"
#include "WM_types.h"

#include "transform.h"
#include "transform_snap.h"

#include "transform_convert.h"

/* helper struct for gp-frame transforms */
typedef struct tGPFtransdata {
  union {
    float val;    /* where transdata writes transform */
    float loc[3]; /* #td->val and #td->loc share the same pointer. */
  };
  int *sdata; /* pointer to gpf->framenum */
} tGPFtransdata;

/* -------------------------------------------------------------------- */
/** \name Action Transform Creation
 * \{ */

/* fully select selected beztriples, but only include if it's on the right side of cfra */
static int count_fcurve_keys(FCurve *fcu, char side, float cfra, bool is_prop_edit)
{
  BezTriple *bezt;
  int i, count = 0, count_all = 0;

  if (ELEM(NULL, fcu, fcu->bezt)) {
    return count;
  }

  /* only include points that occur on the right side of cfra */
  for (i = 0, bezt = fcu->bezt; i < fcu->totvert; i++, bezt++) {
    if (FrameOnMouseSide(side, bezt->vec[1][0], cfra)) {
      /* no need to adjust the handle selection since they are assumed
       * selected (like graph editor with SIPO_NOHANDLES) */
      if (bezt->f2 & SELECT) {
        count++;
      }

      count_all++;
    }
  }

  if (is_prop_edit && count > 0) {
    return count_all;
  }
  return count;
}

/* fully select selected beztriples, but only include if it's on the right side of cfra */
static int count_gplayer_frames(bGPDlayer *gpl, char side, float cfra, bool is_prop_edit)
{
  bGPDframe *gpf;
  int count = 0, count_all = 0;

  if (gpl == NULL) {
    return count;
  }

  /* only include points that occur on the right side of cfra */
  for (gpf = gpl->frames.first; gpf; gpf = gpf->next) {
    if (FrameOnMouseSide(side, (float)gpf->framenum, cfra)) {
      if (gpf->flag & GP_FRAME_SELECT) {
        count++;
      }
      count_all++;
    }
  }

  if (is_prop_edit && count > 0) {
    return count_all;
  }
  return count;
}

/* fully select selected beztriples, but only include if it's on the right side of cfra */
static int count_masklayer_frames(MaskLayer *masklay, char side, float cfra, bool is_prop_edit)
{
  MaskLayerShape *masklayer_shape;
  int count = 0, count_all = 0;

  if (masklay == NULL) {
    return count;
  }

  /* only include points that occur on the right side of cfra */
  for (masklayer_shape = masklay->splines_shapes.first; masklayer_shape;
       masklayer_shape = masklayer_shape->next) {
    if (FrameOnMouseSide(side, (float)masklayer_shape->frame, cfra)) {
      if (masklayer_shape->flag & MASK_SHAPE_SELECT) {
        count++;
      }
      count_all++;
    }
  }

  if (is_prop_edit && count > 0) {
    return count_all;
  }
  return count;
}

/* This function assigns the information to transdata */
static void TimeToTransData(
    TransData *td, TransData2D *td2d, BezTriple *bezt, AnimData *adt, float ypos)
{
  float *time = bezt->vec[1];

  /* Setup #TransData2D. */
  td2d->loc[0] = *time;
  td2d->loc2d = time;
  td2d->h1 = bezt->vec[0];
  td2d->h2 = bezt->vec[2];
  copy_v2_v2(td2d->ih1, td2d->h1);
  copy_v2_v2(td2d->ih2, td2d->h2);

  /* Setup #TransData. */

  /* Usually #td2d->loc is used here.
   * But this is for when the original location is not float[3]. */
  td->loc = time;

  copy_v3_v3(td->iloc, td->loc);
  td->val = time;
  td->ival = *(time);
  td->center[0] = td->ival;
  td->center[1] = ypos;

  /* Store the AnimData where this keyframe exists as a keyframe of the
   * active action as #td->extra. */
  td->extra = adt;

  if (bezt->f2 & SELECT) {
    td->flag |= TD_SELECTED;
  }

  /* Set flags to move handles as necessary. */
  td->flag |= TD_MOVEHANDLE1 | TD_MOVEHANDLE2;
}

/* This function advances the address to which td points to, so it must return
 * the new address so that the next time new transform data is added, it doesn't
 * overwrite the existing ones...  i.e.   td = IcuToTransData(td, icu, ob, side, cfra);
 *
 * The 'side' argument is needed for the extend mode. 'B' = both sides, 'R'/'L' mean only data
 * on the named side are used.
 */
static TransData *ActionFCurveToTransData(TransData *td,
                                          TransData2D **td2dv,
                                          FCurve *fcu,
                                          AnimData *adt,
                                          char side,
                                          float cfra,
                                          bool is_prop_edit,
                                          float ypos)
{
  BezTriple *bezt;
  TransData2D *td2d = *td2dv;
  int i;

  if (ELEM(NULL, fcu, fcu->bezt)) {
    return td;
  }

  for (i = 0, bezt = fcu->bezt; i < fcu->totvert; i++, bezt++) {
    /* only add selected keyframes (for now, proportional edit is not enabled) */
    if (is_prop_edit || (bezt->f2 & SELECT)) { /* note this MUST match count_fcurve_keys(),
                                                * so can't use BEZT_ISSEL_ANY() macro */
      /* only add if on the right 'side' of the current frame */
      if (FrameOnMouseSide(side, bezt->vec[1][0], cfra)) {
        TimeToTransData(td, td2d, bezt, adt, ypos);

        td++;
        td2d++;
      }
    }
  }

  *td2dv = td2d;

  return td;
}

/**
 * This function advances the address to which td points to, so it must return
 * the new address so that the next time new transform data is added, it doesn't
 * overwrite the existing ones: e.g. `td += GPLayerToTransData(td, ...);`
 *
 * \param side: is needed for the extend mode. 'B' = both sides,
 * 'R'/'L' mean only data on the named side are used.
 */
static int GPLayerToTransData(TransData *td,
                              tGPFtransdata *tfd,
                              bGPDlayer *gpl,
                              char side,
                              float cfra,
                              bool is_prop_edit,
                              float ypos)
{
  bGPDframe *gpf;
  int count = 0;

  /* check for select frames on right side of current frame */
  for (gpf = gpl->frames.first; gpf; gpf = gpf->next) {
    if (is_prop_edit || (gpf->flag & GP_FRAME_SELECT)) {
      if (FrameOnMouseSide(side, (float)gpf->framenum, cfra)) {
        tfd->val = (float)gpf->framenum;
        tfd->sdata = &gpf->framenum;

        td->val = td->loc = &tfd->val;
        td->ival = td->iloc[0] = tfd->val;

        td->center[0] = td->ival;
        td->center[1] = ypos;

        /* Advance `td` now. */
        td++;
        tfd++;
        count++;
      }
    }
  }

  return count;
}

/* refer to comment above #GPLayerToTransData, this is the same but for masks */
static int MaskLayerToTransData(TransData *td,
                                tGPFtransdata *tfd,
                                MaskLayer *masklay,
                                char side,
                                float cfra,
                                bool is_prop_edit,
                                float ypos)
{
  MaskLayerShape *masklay_shape;
  int count = 0;

  /* check for select frames on right side of current frame */
  for (masklay_shape = masklay->splines_shapes.first; masklay_shape;
       masklay_shape = masklay_shape->next) {
    if (is_prop_edit || (masklay_shape->flag & MASK_SHAPE_SELECT)) {
      if (FrameOnMouseSide(side, (float)masklay_shape->frame, cfra)) {
        tfd->val = (float)masklay_shape->frame;
        tfd->sdata = &masklay_shape->frame;

        td->val = td->loc = &tfd->val;
        td->ival = td->iloc[0] = tfd->val;

        td->center[0] = td->ival;
        td->center[1] = ypos;

        /* advance td now */
        td++;
        tfd++;
        count++;
      }
    }
  }

  return count;
}

static void createTransActionData(bContext *C, TransInfo *t)
{
  Scene *scene = t->scene;
  TransData *td = NULL;
  TransData2D *td2d = NULL;
  tGPFtransdata *tfd = NULL;

  rcti *mask = &t->region->v2d.mask;
  rctf *datamask = &t->region->v2d.cur;

  float xsize = BLI_rctf_size_x(datamask);
  float ysize = BLI_rctf_size_y(datamask);
  float xmask = BLI_rcti_size_x(mask);
  float ymask = BLI_rcti_size_y(mask);

  bAnimContext ac;
  ListBase anim_data = {NULL, NULL};
  bAnimListElem *ale;
  int filter;
  const bool is_prop_edit = (t->flag & T_PROP_EDIT) != 0;

  int count = 0;
  int gpf_count = 0;
  float cfra;
  float ypos = 1.0f / ((ysize / xsize) * (xmask / ymask)) * BLI_rctf_cent_y(&t->region->v2d.cur);

  /* determine what type of data we are operating on */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return;
  }

  /* filter data */
  filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FOREDIT);
  ANIM_animdata_filter(&ac, &anim_data, filter, ac.data, ac.datatype);

  /* which side of the current frame should be allowed */
  if (t->mode == TFM_TIME_EXTEND) {
    t->frame_side = transform_convert_frame_side_dir_get(t, (float)scene->r.cfra);
  }
  else {
    /* normal transform - both sides of current frame are considered */
    t->frame_side = 'B';
  }

  /* loop 1: fully select F-Curve keys and count how many BezTriples are selected */
  for (ale = anim_data.first; ale; ale = ale->next) {
    AnimData *adt = ANIM_nla_mapping_get(&ac, ale);
    int adt_count = 0;
    /* convert current-frame to action-time (slightly less accurate, especially under
     * higher scaling ratios, but is faster than converting all points)
     */
    if (adt) {
      cfra = BKE_nla_tweakedit_remap(adt, (float)scene->r.cfra, NLATIME_CONVERT_UNMAP);
    }
    else {
      cfra = (float)scene->r.cfra;
    }

    if (ELEM(ale->type, ANIMTYPE_FCURVE, ANIMTYPE_NLACURVE)) {
      adt_count = count_fcurve_keys(ale->key_data, t->frame_side, cfra, is_prop_edit);
    }
    else if (ale->type == ANIMTYPE_GPLAYER) {
      adt_count = count_gplayer_frames(ale->data, t->frame_side, cfra, is_prop_edit);
    }
    else if (ale->type == ANIMTYPE_MASKLAYER) {
      adt_count = count_masklayer_frames(ale->data, t->frame_side, cfra, is_prop_edit);
    }
    else {
      BLI_assert(0);
    }

    if (adt_count > 0) {
      if (ELEM(ale->type, ANIMTYPE_GPLAYER, ANIMTYPE_MASKLAYER)) {
        gpf_count += adt_count;
      }
      count += adt_count;
      ale->tag = true;
    }
  }

  /* stop if trying to build list if nothing selected */
  if (count == 0 && gpf_count == 0) {
    /* cleanup temp list */
    ANIM_animdata_freelist(&anim_data);
    return;
  }

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  /* allocate memory for data */
  tc->data_len = count;

  tc->data = MEM_callocN(tc->data_len * sizeof(TransData), "TransData(Action Editor)");
  tc->data_2d = MEM_callocN(tc->data_len * sizeof(TransData2D), "transdata2d");
  td = tc->data;
  td2d = tc->data_2d;

  if (ELEM(ac.datatype, ANIMCONT_GPENCIL, ANIMCONT_MASK, ANIMCONT_DOPESHEET, ANIMCONT_TIMELINE)) {
    tc->data_gpf_len = gpf_count;
    tc->custom.type.data = tfd = MEM_callocN(sizeof(tGPFtransdata) * gpf_count, "tGPFtransdata");
    tc->custom.type.use_free = true;
  }

  /* loop 2: build transdata array */
  for (ale = anim_data.first; ale; ale = ale->next) {

    if (is_prop_edit && !ale->tag) {
      continue;
    }

    cfra = (float)scene->r.cfra;

    {
      AnimData *adt;
      adt = ANIM_nla_mapping_get(&ac, ale);
      if (adt) {
        cfra = BKE_nla_tweakedit_remap(adt, cfra, NLATIME_CONVERT_UNMAP);
      }
    }

    if (ale->type == ANIMTYPE_GPLAYER) {
      bGPDlayer *gpl = (bGPDlayer *)ale->data;
      int i;

      i = GPLayerToTransData(td, tfd, gpl, t->frame_side, cfra, is_prop_edit, ypos);
      td += i;
      tfd += i;
    }
    else if (ale->type == ANIMTYPE_MASKLAYER) {
      MaskLayer *masklay = (MaskLayer *)ale->data;
      int i;

      i = MaskLayerToTransData(td, tfd, masklay, t->frame_side, cfra, is_prop_edit, ypos);
      td += i;
      tfd += i;
    }
    else {
      AnimData *adt = ANIM_nla_mapping_get(&ac, ale);
      FCurve *fcu = (FCurve *)ale->key_data;

      td = ActionFCurveToTransData(td, &td2d, fcu, adt, t->frame_side, cfra, is_prop_edit, ypos);
    }
  }

  /* calculate distances for proportional editing */
  if (is_prop_edit) {
    td = tc->data;

    for (ale = anim_data.first; ale; ale = ale->next) {
      AnimData *adt;

      /* F-Curve may not have any keyframes */
      if (!ale->tag) {
        continue;
      }

      adt = ANIM_nla_mapping_get(&ac, ale);
      if (adt) {
        cfra = BKE_nla_tweakedit_remap(adt, (float)scene->r.cfra, NLATIME_CONVERT_UNMAP);
      }
      else {
        cfra = (float)scene->r.cfra;
      }

      if (ale->type == ANIMTYPE_GPLAYER) {
        bGPDlayer *gpl = (bGPDlayer *)ale->data;
        bGPDframe *gpf;

        for (gpf = gpl->frames.first; gpf; gpf = gpf->next) {
          if (gpf->flag & GP_FRAME_SELECT) {
            td->dist = td->rdist = 0.0f;
          }
          else {
            bGPDframe *gpf_iter;
            int min = INT_MAX;
            for (gpf_iter = gpl->frames.first; gpf_iter; gpf_iter = gpf_iter->next) {
              if (gpf_iter->flag & GP_FRAME_SELECT) {
                if (FrameOnMouseSide(t->frame_side, (float)gpf_iter->framenum, cfra)) {
                  int val = abs(gpf->framenum - gpf_iter->framenum);
                  if (val < min) {
                    min = val;
                  }
                }
              }
            }
            td->dist = td->rdist = min;
          }
          td++;
        }
      }
      else if (ale->type == ANIMTYPE_MASKLAYER) {
        MaskLayer *masklay = (MaskLayer *)ale->data;
        MaskLayerShape *masklay_shape;

        for (masklay_shape = masklay->splines_shapes.first; masklay_shape;
             masklay_shape = masklay_shape->next) {
          if (FrameOnMouseSide(t->frame_side, (float)masklay_shape->frame, cfra)) {
            if (masklay_shape->flag & MASK_SHAPE_SELECT) {
              td->dist = td->rdist = 0.0f;
            }
            else {
              MaskLayerShape *masklay_iter;
              int min = INT_MAX;
              for (masklay_iter = masklay->splines_shapes.first; masklay_iter;
                   masklay_iter = masklay_iter->next) {
                if (masklay_iter->flag & MASK_SHAPE_SELECT) {
                  if (FrameOnMouseSide(t->frame_side, (float)masklay_iter->frame, cfra)) {
                    int val = abs(masklay_shape->frame - masklay_iter->frame);
                    if (val < min) {
                      min = val;
                    }
                  }
                }
              }
              td->dist = td->rdist = min;
            }
            td++;
          }
        }
      }
      else {
        FCurve *fcu = (FCurve *)ale->key_data;
        BezTriple *bezt;
        int i;

        for (i = 0, bezt = fcu->bezt; i < fcu->totvert; i++, bezt++) {
          if (FrameOnMouseSide(t->frame_side, bezt->vec[1][0], cfra)) {
            if (bezt->f2 & SELECT) {
              td->dist = td->rdist = 0.0f;
            }
            else {
              BezTriple *bezt_iter;
              int j;
              float min = FLT_MAX;
              for (j = 0, bezt_iter = fcu->bezt; j < fcu->totvert; j++, bezt_iter++) {
                if (bezt_iter->f2 & SELECT) {
                  if (FrameOnMouseSide(t->frame_side, (float)bezt_iter->vec[1][0], cfra)) {
                    float val = fabs(bezt->vec[1][0] - bezt_iter->vec[1][0]);
                    if (val < min) {
                      min = val;
                    }
                  }
                }
              }
              td->dist = td->rdist = min;
            }
            td++;
          }
        }
      }
    }
  }

  /* cleanup temp list */
  ANIM_animdata_freelist(&anim_data);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Action Transform Flush
 * \{ */

/* This function helps flush transdata written to tempdata into the gp-frames. */
static void flushTransIntFrameActionData(TransInfo *t)
{
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  tGPFtransdata *tfd = tc->custom.type.data;

  /* flush data!
   * Expects data_gpf_len to be set in the data container. */
  for (int i = 0; i < tc->data_gpf_len; i++, tfd++) {
    *(tfd->sdata) = round_fl_to_int(tfd->val);
  }
}

static void recalcData_actedit(TransInfo *t)
{
  ViewLayer *view_layer = t->view_layer;
  SpaceAction *saction = (SpaceAction *)t->area->spacedata.first;

  bAnimContext ac = {NULL};
  ListBase anim_data = {NULL, NULL};
  bAnimListElem *ale;
  int filter;

  /* initialize relevant anim-context 'context' data from TransInfo data */
  /* NOTE: sync this with the code in ANIM_animdata_get_context() */
  ac.bmain = CTX_data_main(t->context);
  ac.scene = t->scene;
  ac.view_layer = t->view_layer;
  ac.obact = OBACT(view_layer);
  ac.area = t->area;
  ac.region = t->region;
  ac.sl = (t->area) ? t->area->spacedata.first : NULL;
  ac.spacetype = (t->area) ? t->area->spacetype : 0;
  ac.regiontype = (t->region) ? t->region->regiontype : 0;

  ANIM_animdata_context_getdata(&ac);

  /* perform flush */
  if (ELEM(ac.datatype, ANIMCONT_GPENCIL, ANIMCONT_MASK, ANIMCONT_DOPESHEET, ANIMCONT_TIMELINE)) {
    /* flush transform values back to actual coordinates */
    flushTransIntFrameActionData(t);
  }

  /* Flush 2d vector. */
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  const short autosnap = getAnimEdit_SnapMode(t);
  TransData *td;
  TransData2D *td2d;
  int i = 0;
  for (td = tc->data, td2d = tc->data_2d; i < tc->data_len; i++, td++, td2d++) {
    if ((autosnap != SACTSNAP_OFF) && (t->state != TRANS_CANCEL) && !(td->flag & TD_NOTIMESNAP)) {
      transform_snap_anim_flush_data(t, td, autosnap, td->loc);
    }

    /* Constrain Y. */
    td->loc[1] = td->iloc[1];

    transform_convert_flush_handle2D(td, td2d, 0.0f);
  }

  if (ac.datatype != ANIMCONT_MASK) {
    /* Get animdata blocks visible in editor,
     * assuming that these will be the ones where things changed. */
    filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_ANIMDATA);
    ANIM_animdata_filter(&ac, &anim_data, filter, ac.data, ac.datatype);

    /* just tag these animdata-blocks to recalc, assuming that some data there changed
     * BUT only do this if realtime updates are enabled
     */
    if ((saction->flag & SACTION_NOREALTIMEUPDATES) == 0) {
      for (ale = anim_data.first; ale; ale = ale->next) {
        /* set refresh tags for objects using this animation */
        ANIM_list_elem_update(CTX_data_main(t->context), t->scene, ale);
      }
    }

    /* now free temp channels */
    ANIM_animdata_freelist(&anim_data);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Special After Transform Action
 * \{ */

static int masklay_shape_cmp_frame(void *thunk, const void *a, const void *b)
{
  const MaskLayerShape *frame_a = a;
  const MaskLayerShape *frame_b = b;

  if (frame_a->frame < frame_b->frame) {
    return -1;
  }
  if (frame_a->frame > frame_b->frame) {
    return 1;
  }
  *((bool *)thunk) = true;
  /* selected last */
  if ((frame_a->flag & MASK_SHAPE_SELECT) && ((frame_b->flag & MASK_SHAPE_SELECT) == 0)) {
    return 1;
  }
  return 0;
}

static void posttrans_mask_clean(Mask *mask)
{
  MaskLayer *masklay;

  for (masklay = mask->masklayers.first; masklay; masklay = masklay->next) {
    MaskLayerShape *masklay_shape, *masklay_shape_next;
    bool is_double = false;

    BLI_listbase_sort_r(&masklay->splines_shapes, masklay_shape_cmp_frame, &is_double);

    if (is_double) {
      for (masklay_shape = masklay->splines_shapes.first; masklay_shape;
           masklay_shape = masklay_shape_next) {
        masklay_shape_next = masklay_shape->next;
        if (masklay_shape_next && masklay_shape->frame == masklay_shape_next->frame) {
          BKE_mask_layer_shape_unlink(masklay, masklay_shape);
        }
      }
    }

#ifdef DEBUG
    for (masklay_shape = masklay->splines_shapes.first; masklay_shape;
         masklay_shape = masklay_shape->next) {
      BLI_assert(!masklay_shape->next || masklay_shape->frame < masklay_shape->next->frame);
    }
#endif
  }

  WM_main_add_notifier(NC_MASK | NA_EDITED, mask);
}

/* Called by special_aftertrans_update to make sure selected gp-frames replace
 * any other gp-frames which may reside on that frame (that are not selected).
 * It also makes sure gp-frames are still stored in chronological order after
 * transform.
 */
static void posttrans_gpd_clean(bGPdata *gpd)
{
  bGPDlayer *gpl;

  for (gpl = gpd->layers.first; gpl; gpl = gpl->next) {
    bGPDframe *gpf, *gpfn;
    bool is_double = false;

    BKE_gpencil_layer_frames_sort(gpl, &is_double);

    if (is_double) {
      for (gpf = gpl->frames.first; gpf; gpf = gpfn) {
        gpfn = gpf->next;
        if (gpfn && gpf->framenum == gpfn->framenum) {
          BKE_gpencil_layer_frame_delete(gpl, gpf);
        }
      }
    }

#ifdef DEBUG
    for (gpf = gpl->frames.first; gpf; gpf = gpf->next) {
      BLI_assert(!gpf->next || gpf->framenum < gpf->next->framenum);
    }
#endif
  }
  /* set cache flag to dirty */
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);

  WM_main_add_notifier(NC_GPENCIL | NA_EDITED, gpd);
}

/* Called by special_aftertrans_update to make sure selected keyframes replace
 * any other keyframes which may reside on that frame (that is not selected).
 * remake_action_ipos should have already been called
 */
static void posttrans_action_clean(bAnimContext *ac, bAction *act)
{
  ListBase anim_data = {NULL, NULL};
  bAnimListElem *ale;
  int filter;

  /* filter data */
  filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FOREDIT | ANIMFILTER_FCURVESONLY);
  ANIM_animdata_filter(ac, &anim_data, filter, act, ANIMCONT_ACTION);

  /* loop through relevant data, removing keyframes as appropriate
   *      - all keyframes are converted in/out of global time
   */
  for (ale = anim_data.first; ale; ale = ale->next) {
    AnimData *adt = ANIM_nla_mapping_get(ac, ale);

    if (adt) {
      ANIM_nla_mapping_apply_fcurve(adt, ale->key_data, 0, 0);
      posttrans_fcurve_clean(ale->key_data, SELECT, false); /* only use handles in graph editor */
      ANIM_nla_mapping_apply_fcurve(adt, ale->key_data, 1, 0);
    }
    else {
      posttrans_fcurve_clean(ale->key_data, SELECT, false); /* only use handles in graph editor */
    }
  }

  /* free temp data */
  ANIM_animdata_freelist(&anim_data);
}

static void special_aftertrans_update__actedit(bContext *C, TransInfo *t)
{
  SpaceAction *saction = (SpaceAction *)t->area->spacedata.first;
  bAnimContext ac;

  const bool canceled = (t->state == TRANS_CANCEL);
  const bool duplicate = (t->mode == TFM_TIME_DUPLICATE);

  /* initialize relevant anim-context 'context' data */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return;
  }

  Object *ob = ac.obact;

  if (ELEM(ac.datatype, ANIMCONT_DOPESHEET, ANIMCONT_SHAPEKEY, ANIMCONT_TIMELINE)) {
    ListBase anim_data = {NULL, NULL};
    bAnimListElem *ale;
    short filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FOREDIT);

    /* get channels to work on */
    ANIM_animdata_filter(&ac, &anim_data, filter, ac.data, ac.datatype);

    for (ale = anim_data.first; ale; ale = ale->next) {
      switch (ale->datatype) {
        case ALE_GPFRAME:
          ale->id->tag &= ~LIB_TAG_DOIT;
          posttrans_gpd_clean((bGPdata *)ale->id);
          break;

        case ALE_FCURVE: {
          AnimData *adt = ANIM_nla_mapping_get(&ac, ale);
          FCurve *fcu = (FCurve *)ale->key_data;

          /* 3 cases here for curve cleanups:
           * 1) NOTRANSKEYCULL on    -> cleanup of duplicates shouldn't be done
           * 2) canceled == 0        -> user confirmed the transform,
           *                            so duplicates should be removed
           * 3) canceled + duplicate -> user canceled the transform,
           *                            but we made duplicates, so get rid of these
           */
          if ((saction->flag & SACTION_NOTRANSKEYCULL) == 0 && ((canceled == 0) || (duplicate))) {
            if (adt) {
              ANIM_nla_mapping_apply_fcurve(adt, fcu, 0, 0);
              posttrans_fcurve_clean(fcu, SELECT, false); /* only use handles in graph editor */
              ANIM_nla_mapping_apply_fcurve(adt, fcu, 1, 0);
            }
            else {
              posttrans_fcurve_clean(fcu, SELECT, false); /* only use handles in graph editor */
            }
          }
          break;
        }

        default:
          BLI_assert_msg(false, "Keys cannot be transformed into this animation type.");
      }
    }

    /* free temp memory */
    ANIM_animdata_freelist(&anim_data);
  }
  else if (ac.datatype == ANIMCONT_ACTION) { /* TODO: just integrate into the above. */
    /* Depending on the lock status, draw necessary views */
    /* FIXME: some of this stuff is not good. */
    if (ob) {
      if (ob->pose || BKE_key_from_object(ob)) {
        DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);
      }
      else {
        DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
      }
    }

    /* 3 cases here for curve cleanups:
     * 1) NOTRANSKEYCULL on    -> cleanup of duplicates shouldn't be done
     * 2) canceled == 0        -> user confirmed the transform,
     *                            so duplicates should be removed.
     * 3) canceled + duplicate -> user canceled the transform,
     *                            but we made duplicates, so get rid of these.
     */
    if ((saction->flag & SACTION_NOTRANSKEYCULL) == 0 && ((canceled == 0) || (duplicate))) {
      posttrans_action_clean(&ac, (bAction *)ac.data);
    }
  }
  else if (ac.datatype == ANIMCONT_GPENCIL) {
    /* remove duplicate frames and also make sure points are in order! */
    /* 3 cases here for curve cleanups:
     * 1) NOTRANSKEYCULL on    -> cleanup of duplicates shouldn't be done
     * 2) canceled == 0        -> user confirmed the transform,
     *                            so duplicates should be removed
     * 3) canceled + duplicate -> user canceled the transform,
     *                            but we made duplicates, so get rid of these
     */
    if ((saction->flag & SACTION_NOTRANSKEYCULL) == 0 && ((canceled == 0) || (duplicate))) {
      ListBase anim_data = {NULL, NULL};
      const int filter = ANIMFILTER_DATA_VISIBLE;
      ANIM_animdata_filter(&ac, &anim_data, filter, ac.data, ac.datatype);

      LISTBASE_FOREACH (bAnimListElem *, ale, &anim_data) {
        if (ale->datatype == ALE_GPFRAME) {
          ale->id->tag &= ~LIB_TAG_DOIT;
          posttrans_gpd_clean((bGPdata *)ale->id);
        }
      }
      ANIM_animdata_freelist(&anim_data);
    }
  }
  else if (ac.datatype == ANIMCONT_MASK) {
    /* remove duplicate frames and also make sure points are in order! */
    /* 3 cases here for curve cleanups:
     * 1) NOTRANSKEYCULL on:
     *    Cleanup of duplicates shouldn't be done.
     * 2) canceled == 0:
     *    User confirmed the transform, so duplicates should be removed.
     * 3) Canceled + duplicate:
     *    User canceled the transform, but we made duplicates, so get rid of these.
     */
    if ((saction->flag & SACTION_NOTRANSKEYCULL) == 0 && ((canceled == 0) || (duplicate))) {
      ListBase anim_data = {NULL, NULL};
      const int filter = ANIMFILTER_DATA_VISIBLE;
      ANIM_animdata_filter(&ac, &anim_data, filter, ac.data, ac.datatype);

      LISTBASE_FOREACH (bAnimListElem *, ale, &anim_data) {
        if (ale->datatype == ALE_MASKLAY) {
          ale->id->tag &= ~LIB_TAG_DOIT;
          posttrans_mask_clean((Mask *)ale->id);
        }
      }
      ANIM_animdata_freelist(&anim_data);
    }
  }

  /* marker transform, not especially nice but we may want to move markers
   * at the same time as keyframes in the dope sheet.
   */
  if ((saction->flag & SACTION_MARKERS_MOVE) && (canceled == 0)) {
    if (t->mode == TFM_TIME_TRANSLATE) {
#if 0
        if (ELEM(t->frame_side, 'L', 'R')) { /* TFM_TIME_EXTEND */
          /* same as below */
          ED_markers_post_apply_transform(
              ED_context_get_markers(C), t->scene, t->mode, t->values[0], t->frame_side);
        }
        else /* TFM_TIME_TRANSLATE */
#endif
      {
        ED_markers_post_apply_transform(
            ED_context_get_markers(C), t->scene, t->mode, t->values[0], t->frame_side);
      }
    }
    else if (t->mode == TFM_TIME_SCALE) {
      ED_markers_post_apply_transform(
          ED_context_get_markers(C), t->scene, t->mode, t->values[0], t->frame_side);
    }
  }

  /* make sure all F-Curves are set correctly */
  if (!ELEM(ac.datatype, ANIMCONT_GPENCIL)) {
    ANIM_editkeyframes_refresh(&ac);
  }

  /* clear flag that was set for time-slide drawing */
  saction->flag &= ~SACTION_MOVING;
}

/** \} */

TransConvertTypeInfo TransConvertType_Action = {
    /* flags */ (T_POINTS | T_2D_EDIT),
    /* createTransData */ createTransActionData,
    /* recalcData */ recalcData_actedit,
    /* special_aftertrans_update */ special_aftertrans_update__actedit,
};
