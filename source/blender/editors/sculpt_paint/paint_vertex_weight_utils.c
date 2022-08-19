/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Intended for use by `paint_vertex.c` & `paint_vertex_weight_ops.c`.
 */

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string_utils.h"

#include "DNA_armature_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_action.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_mesh.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_object_deform.h"
#include "BKE_report.h"

#include "DEG_depsgraph_build.h"

/* Only for blend modes. */
#include "IMB_imbuf.h"

#include "WM_api.h"
#include "WM_types.h"

#include "paint_intern.h" /* own include */

/* -------------------------------------------------------------------- */
/** \name Weight Paint Sanity Checks
 * \{ */

bool ED_wpaint_ensure_data(bContext *C,
                           struct ReportList *reports,
                           enum eWPaintFlag flag,
                           struct WPaintVGroupIndex *vgroup_index)
{
  Object *ob = CTX_data_active_object(C);
  Mesh *me = BKE_mesh_from_object(ob);

  if (vgroup_index) {
    vgroup_index->active = -1;
    vgroup_index->mirror = -1;
  }

  if (BKE_object_is_in_editmode(ob)) {
    return false;
  }

  if (me == NULL || me->totpoly == 0) {
    return false;
  }

  /* if nothing was added yet, we make dverts and a vertex deform group */
  if (!me->dvert) {
    BKE_object_defgroup_data_create(&me->id);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, me);
  }

  const ListBase *defbase = BKE_object_defgroup_list(ob);

  /* this happens on a Bone select, when no vgroup existed yet */
  if (me->vertex_group_active_index <= 0) {
    Object *modob;
    if ((modob = BKE_modifiers_is_deformed_by_armature(ob))) {
      Bone *actbone = ((bArmature *)modob->data)->act_bone;
      if (actbone) {
        bPoseChannel *pchan = BKE_pose_channel_find_name(modob->pose, actbone->name);

        if (pchan) {
          bDeformGroup *dg = BKE_object_defgroup_find_name(ob, pchan->name);
          if (dg == NULL) {
            dg = BKE_object_defgroup_add_name(ob, pchan->name); /* sets actdef */
            DEG_relations_tag_update(CTX_data_main(C));
          }
          else {

            int actdef = 1 + BLI_findindex(defbase, dg);
            BLI_assert(actdef >= 0);
            me->vertex_group_active_index = actdef;
          }
        }
      }
    }
  }
  if (BLI_listbase_is_empty(defbase)) {
    BKE_object_defgroup_add(ob);
    DEG_relations_tag_update(CTX_data_main(C));
  }

  /* ensure we don't try paint onto an invalid group */
  if (me->vertex_group_active_index <= 0) {
    BKE_report(reports, RPT_WARNING, "No active vertex group for painting, aborting");
    return false;
  }

  if (vgroup_index) {
    vgroup_index->active = me->vertex_group_active_index - 1;
  }

  if (flag & WPAINT_ENSURE_MIRROR) {
    if (ME_USING_MIRROR_X_VERTEX_GROUPS(me)) {
      int mirror = ED_wpaint_mirror_vgroup_ensure(ob, me->vertex_group_active_index - 1);
      if (vgroup_index) {
        vgroup_index->mirror = mirror;
      }
    }
  }

  return true;
}

/** \} */

int ED_wpaint_mirror_vgroup_ensure(Object *ob, const int vgroup_active)
{
  const ListBase *defbase = BKE_object_defgroup_list(ob);
  bDeformGroup *defgroup = BLI_findlink(defbase, vgroup_active);

  if (defgroup) {
    int mirrdef;
    char name_flip[MAXBONENAME];

    BLI_string_flip_side_name(name_flip, defgroup->name, false, sizeof(name_flip));
    mirrdef = BKE_object_defgroup_name_index(ob, name_flip);
    if (mirrdef == -1) {
      if (BKE_object_defgroup_new(ob, name_flip)) {
        mirrdef = BLI_listbase_count(defbase) - 1;
      }
    }

    /* curdef should never be NULL unless this is
     * a  light and BKE_object_defgroup_add_name fails */
    return mirrdef;
  }

  return -1;
}

/* -------------------------------------------------------------------- */
/** \name Weight Blending Modes
 * \{ */

BLI_INLINE float wval_blend(const float weight, const float paintval, const float alpha)
{
  const float talpha = min_ff(alpha, 1.0f); /* blending with values over 1 doesn't make sense */
  return (paintval * talpha) + (weight * (1.0f - talpha));
}
BLI_INLINE float wval_add(const float weight, const float paintval, const float alpha)
{
  return weight + (paintval * alpha);
}
BLI_INLINE float wval_sub(const float weight, const float paintval, const float alpha)
{
  return weight - (paintval * alpha);
}
BLI_INLINE float wval_mul(const float weight, const float paintval, const float alpha)
{ /* first mul, then blend the fac */
  return ((1.0f - alpha) + (alpha * paintval)) * weight;
}
BLI_INLINE float wval_lighten(const float weight, const float paintval, const float alpha)
{
  return (weight < paintval) ? wval_blend(weight, paintval, alpha) : weight;
}
BLI_INLINE float wval_darken(const float weight, const float paintval, const float alpha)
{
  return (weight > paintval) ? wval_blend(weight, paintval, alpha) : weight;
}

/* mainly for color */
BLI_INLINE float wval_colordodge(float weight, float paintval, float fac)
{
  float mfac, temp;
  if (fac == 0.0f) {
    return weight;
  }
  mfac = 1.0f - fac;
  temp = (paintval == 1.0f) ? 1.0f :
                              min_ff((weight * (225.0f / 255.0f)) / (1.0f - paintval), 1.0f);
  return mfac * weight + temp * fac;
}
BLI_INLINE float wval_difference(float weight, float paintval, float fac)
{
  float mfac, temp;
  if (fac == 0.0f) {
    return weight;
  }
  mfac = 1.0f - fac;
  temp = fabsf(weight - paintval);
  return mfac * weight + temp * fac;
}
BLI_INLINE float wval_screen(float weight, float paintval, float fac)
{
  float mfac, temp;
  if (fac == 0.0f) {
    return weight;
  }
  mfac = 1.0f - fac;
  temp = max_ff(1.0f - (((1.0f - weight) * (1.0f - paintval))), 0);
  return mfac * weight + temp * fac;
}
BLI_INLINE float wval_hardlight(float weight, float paintval, float fac)
{
  float mfac, temp;
  if (fac == 0.0f) {
    return weight;
  }
  mfac = 1.0f - fac;
  if (paintval > 0.5f) {
    temp = 1.0f - ((1.0f - 2.0f * (paintval - 0.5f)) * (1.0f - weight));
  }
  else {
    temp = (2.0f * paintval * weight);
  }
  return mfac * weight + temp * fac;
}
BLI_INLINE float wval_overlay(float weight, float paintval, float fac)
{
  float mfac, temp;
  if (fac == 0.0f) {
    return weight;
  }
  mfac = 1.0f - fac;
  if (weight > 0.5f) {
    temp = 1.0f - ((1.0f - 2.0f * (weight - 0.5f)) * (1.0f - paintval));
  }
  else {
    temp = (2.0f * paintval * weight);
  }
  return mfac * weight + temp * fac;
}
BLI_INLINE float wval_softlight(float weight, float paintval, float fac)
{
  float mfac, temp;
  if (fac == 0.0f) {
    return weight;
  }
  mfac = 1.0f - fac;
  if (weight < 0.5f) {
    temp = ((2.0f * ((paintval / 2.0f) + 0.25f)) * weight);
  }
  else {
    temp = 1.0f - (2.0f * (1.0f - ((paintval / 2.0f) + 0.25f)) * (1.0f - weight));
  }
  return temp * fac + weight * mfac;
}
BLI_INLINE float wval_exclusion(float weight, float paintval, float fac)
{
  float mfac, temp;
  if (fac == 0.0f) {
    return weight;
  }
  mfac = 1.0f - fac;
  temp = 0.5f - ((2.0f * (weight - 0.5f) * (paintval - 0.5f)));
  return temp * fac + weight * mfac;
}

float ED_wpaint_blend_tool(const int tool,
                           const float weight,
                           const float paintval,
                           const float alpha)
{
  switch ((IMB_BlendMode)tool) {
    case IMB_BLEND_MIX:
      return wval_blend(weight, paintval, alpha);
    case IMB_BLEND_ADD:
      return wval_add(weight, paintval, alpha);
    case IMB_BLEND_SUB:
      return wval_sub(weight, paintval, alpha);
    case IMB_BLEND_MUL:
      return wval_mul(weight, paintval, alpha);
    case IMB_BLEND_LIGHTEN:
      return wval_lighten(weight, paintval, alpha);
    case IMB_BLEND_DARKEN:
      return wval_darken(weight, paintval, alpha);
    /* Mostly make sense for color: support anyway. */
    case IMB_BLEND_COLORDODGE:
      return wval_colordodge(weight, paintval, alpha);
    case IMB_BLEND_DIFFERENCE:
      return wval_difference(weight, paintval, alpha);
    case IMB_BLEND_SCREEN:
      return wval_screen(weight, paintval, alpha);
    case IMB_BLEND_HARDLIGHT:
      return wval_hardlight(weight, paintval, alpha);
    case IMB_BLEND_OVERLAY:
      return wval_overlay(weight, paintval, alpha);
    case IMB_BLEND_SOFTLIGHT:
      return wval_softlight(weight, paintval, alpha);
    case IMB_BLEND_EXCLUSION:
      return wval_exclusion(weight, paintval, alpha);
    /* Only for color: just use blend. */
    default:
      return wval_blend(weight, paintval, alpha);
  }
}

/** \} */
