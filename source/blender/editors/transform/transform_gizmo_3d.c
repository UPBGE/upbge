/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 *
 * \name 3D Transform Gizmo
 *
 * Used for 3D View
 */

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "DNA_armature_types.h"
#include "DNA_curve_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_lattice_types.h"
#include "DNA_meta_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "BLI_array_utils.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BKE_action.h"
#include "BKE_armature.h"
#include "BKE_context.h"
#include "BKE_curve.h"
#include "BKE_editmesh.h"
#include "BKE_global.h"
#include "BKE_gpencil.h"
#include "BKE_layer.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_pointcache.h"
#include "BKE_scene.h"

#include "DEG_depsgraph.h"

#include "WM_api.h"
#include "WM_message.h"
#include "WM_types.h"

#include "ED_armature.h"
#include "ED_gizmo_library.h"
#include "ED_gizmo_utils.h"
#include "ED_gpencil.h"
#include "ED_object.h"
#include "ED_particle.h"
#include "ED_screen.h"
#include "ED_view3d.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"
#include "RNA_define.h"

/* local module include */
#include "transform.h"
#include "transform_convert.h"
#include "transform_snap.h"

#include "MEM_guardedalloc.h"

#include "GPU_state.h"

static void gizmo_refresh_from_matrix(wmGizmoGroup *gzgroup,
                                      const float twmat[4][4],
                                      const float scale[3]);

/* return codes for select, and drawing flags */

#define MAN_TRANS_X (1 << 0)
#define MAN_TRANS_Y (1 << 1)
#define MAN_TRANS_Z (1 << 2)
#define MAN_TRANS_C (MAN_TRANS_X | MAN_TRANS_Y | MAN_TRANS_Z)

#define MAN_ROT_X (1 << 3)
#define MAN_ROT_Y (1 << 4)
#define MAN_ROT_Z (1 << 5)
#define MAN_ROT_C (MAN_ROT_X | MAN_ROT_Y | MAN_ROT_Z)

#define MAN_SCALE_X (1 << 8)
#define MAN_SCALE_Y (1 << 9)
#define MAN_SCALE_Z (1 << 10)
#define MAN_SCALE_C (MAN_SCALE_X | MAN_SCALE_Y | MAN_SCALE_Z)

/* threshold for testing view aligned gizmo axis */
static struct {
  float min, max;
} g_tw_axis_range[2] = {
    /* Regular range */
    {0.02f, 0.1f},
    /* Use a different range because we flip the dot product,
     * also the view aligned planes are harder to see so hiding early is preferred. */
    {0.175f, 0.25f},
};

/* axes as index */
enum {
  MAN_AXIS_TRANS_X = 0,
  MAN_AXIS_TRANS_Y,
  MAN_AXIS_TRANS_Z,
  MAN_AXIS_TRANS_C,

  MAN_AXIS_TRANS_XY,
  MAN_AXIS_TRANS_YZ,
  MAN_AXIS_TRANS_ZX,
#define MAN_AXIS_RANGE_TRANS_START MAN_AXIS_TRANS_X
#define MAN_AXIS_RANGE_TRANS_END (MAN_AXIS_TRANS_ZX + 1)

  MAN_AXIS_ROT_X,
  MAN_AXIS_ROT_Y,
  MAN_AXIS_ROT_Z,
  MAN_AXIS_ROT_C,
  MAN_AXIS_ROT_T, /* trackball rotation */
#define MAN_AXIS_RANGE_ROT_START MAN_AXIS_ROT_X
#define MAN_AXIS_RANGE_ROT_END (MAN_AXIS_ROT_T + 1)

  MAN_AXIS_SCALE_X,
  MAN_AXIS_SCALE_Y,
  MAN_AXIS_SCALE_Z,
  MAN_AXIS_SCALE_C,
  MAN_AXIS_SCALE_XY,
  MAN_AXIS_SCALE_YZ,
  MAN_AXIS_SCALE_ZX,
#define MAN_AXIS_RANGE_SCALE_START MAN_AXIS_SCALE_X
#define MAN_AXIS_RANGE_SCALE_END (MAN_AXIS_SCALE_ZX + 1)

  MAN_AXIS_LAST = MAN_AXIS_SCALE_ZX + 1,
};

/* axis types */
enum {
  MAN_AXES_ALL = 0,
  MAN_AXES_TRANSLATE,
  MAN_AXES_ROTATE,
  MAN_AXES_SCALE,
};

typedef struct GizmoGroup {
  bool all_hidden;
  int twtype;

  /* Users may change the twtype, detect changes to re-setup gizmo options. */
  int twtype_init;
  int twtype_prev;
  int use_twtype_refresh;

  /* Only for view orientation. */
  struct {
    float viewinv_m3[3][3];
  } prev;

  /* Only for Rotate operator. */
  float rotation;

  struct wmGizmo *gizmos[MAN_AXIS_LAST];
} GizmoGroup;

/* -------------------------------------------------------------------- */
/** \name Utilities
 * \{ */

/* loop over axes */
#define MAN_ITER_AXES_BEGIN(axis, axis_idx) \
  { \
    wmGizmo *axis; \
    int axis_idx; \
    for (axis_idx = 0; axis_idx < MAN_AXIS_LAST; axis_idx++) { \
      axis = gizmo_get_axis_from_index(ggd, axis_idx);

#define MAN_ITER_AXES_END \
  } \
  } \
  ((void)0)

static wmGizmo *gizmo_get_axis_from_index(const GizmoGroup *ggd, const short axis_idx)
{
  BLI_assert(IN_RANGE_INCL(axis_idx, (float)MAN_AXIS_TRANS_X, (float)MAN_AXIS_LAST));
  return ggd->gizmos[axis_idx];
}

static short gizmo_get_axis_type(const int axis_idx)
{
  if (axis_idx >= MAN_AXIS_RANGE_TRANS_START && axis_idx < MAN_AXIS_RANGE_TRANS_END) {
    return MAN_AXES_TRANSLATE;
  }
  if (axis_idx >= MAN_AXIS_RANGE_ROT_START && axis_idx < MAN_AXIS_RANGE_ROT_END) {
    return MAN_AXES_ROTATE;
  }
  if (axis_idx >= MAN_AXIS_RANGE_SCALE_START && axis_idx < MAN_AXIS_RANGE_SCALE_END) {
    return MAN_AXES_SCALE;
  }
  BLI_assert(0);
  return -1;
}

static uint gizmo_orientation_axis(const int axis_idx, bool *r_is_plane)
{
  switch (axis_idx) {
    case MAN_AXIS_TRANS_YZ:
    case MAN_AXIS_SCALE_YZ:
      if (r_is_plane) {
        *r_is_plane = true;
      }
      ATTR_FALLTHROUGH;
    case MAN_AXIS_TRANS_X:
    case MAN_AXIS_ROT_X:
    case MAN_AXIS_SCALE_X:
      return 0;

    case MAN_AXIS_TRANS_ZX:
    case MAN_AXIS_SCALE_ZX:
      if (r_is_plane) {
        *r_is_plane = true;
      }
      ATTR_FALLTHROUGH;
    case MAN_AXIS_TRANS_Y:
    case MAN_AXIS_ROT_Y:
    case MAN_AXIS_SCALE_Y:
      return 1;

    case MAN_AXIS_TRANS_XY:
    case MAN_AXIS_SCALE_XY:
      if (r_is_plane) {
        *r_is_plane = true;
      }
      ATTR_FALLTHROUGH;
    case MAN_AXIS_TRANS_Z:
    case MAN_AXIS_ROT_Z:
    case MAN_AXIS_SCALE_Z:
      return 2;
  }
  return 3;
}

static bool gizmo_is_axis_visible(const RegionView3D *rv3d,
                                  const int twtype,
                                  const float idot[3],
                                  const int axis_type,
                                  const int axis_idx)
{
  if ((axis_idx >= MAN_AXIS_RANGE_ROT_START && axis_idx < MAN_AXIS_RANGE_ROT_END) == 0) {
    bool is_plane = false;
    const uint aidx_norm = gizmo_orientation_axis(axis_idx, &is_plane);
    /* don't draw axis perpendicular to the view */
    if (aidx_norm < 3) {
      float idot_axis = idot[aidx_norm];
      if (is_plane) {
        idot_axis = 1.0f - idot_axis;
      }
      if (idot_axis < g_tw_axis_range[is_plane].min) {
        return false;
      }
    }
  }

  if ((axis_type == MAN_AXES_TRANSLATE && !(twtype & V3D_GIZMO_SHOW_OBJECT_TRANSLATE)) ||
      (axis_type == MAN_AXES_ROTATE && !(twtype & V3D_GIZMO_SHOW_OBJECT_ROTATE)) ||
      (axis_type == MAN_AXES_SCALE && !(twtype & V3D_GIZMO_SHOW_OBJECT_SCALE))) {
    return false;
  }

  switch (axis_idx) {
    case MAN_AXIS_TRANS_X:
      return (rv3d->twdrawflag & MAN_TRANS_X);
    case MAN_AXIS_TRANS_Y:
      return (rv3d->twdrawflag & MAN_TRANS_Y);
    case MAN_AXIS_TRANS_Z:
      return (rv3d->twdrawflag & MAN_TRANS_Z);
    case MAN_AXIS_TRANS_C:
      return (rv3d->twdrawflag & MAN_TRANS_C);
    case MAN_AXIS_ROT_X:
      return (rv3d->twdrawflag & MAN_ROT_X);
    case MAN_AXIS_ROT_Y:
      return (rv3d->twdrawflag & MAN_ROT_Y);
    case MAN_AXIS_ROT_Z:
      return (rv3d->twdrawflag & MAN_ROT_Z);
    case MAN_AXIS_ROT_C:
    case MAN_AXIS_ROT_T:
      return (rv3d->twdrawflag & MAN_ROT_C);
    case MAN_AXIS_SCALE_X:
      return (rv3d->twdrawflag & MAN_SCALE_X);
    case MAN_AXIS_SCALE_Y:
      return (rv3d->twdrawflag & MAN_SCALE_Y);
    case MAN_AXIS_SCALE_Z:
      return (rv3d->twdrawflag & MAN_SCALE_Z);
    case MAN_AXIS_SCALE_C:
      return (rv3d->twdrawflag & MAN_SCALE_C && (twtype & V3D_GIZMO_SHOW_OBJECT_TRANSLATE) == 0);
    case MAN_AXIS_TRANS_XY:
      return (rv3d->twdrawflag & MAN_TRANS_X && rv3d->twdrawflag & MAN_TRANS_Y &&
              (twtype & V3D_GIZMO_SHOW_OBJECT_ROTATE) == 0);
    case MAN_AXIS_TRANS_YZ:
      return (rv3d->twdrawflag & MAN_TRANS_Y && rv3d->twdrawflag & MAN_TRANS_Z &&
              (twtype & V3D_GIZMO_SHOW_OBJECT_ROTATE) == 0);
    case MAN_AXIS_TRANS_ZX:
      return (rv3d->twdrawflag & MAN_TRANS_Z && rv3d->twdrawflag & MAN_TRANS_X &&
              (twtype & V3D_GIZMO_SHOW_OBJECT_ROTATE) == 0);
    case MAN_AXIS_SCALE_XY:
      return (rv3d->twdrawflag & MAN_SCALE_X && rv3d->twdrawflag & MAN_SCALE_Y &&
              (twtype & V3D_GIZMO_SHOW_OBJECT_TRANSLATE) == 0 &&
              (twtype & V3D_GIZMO_SHOW_OBJECT_ROTATE) == 0);
    case MAN_AXIS_SCALE_YZ:
      return (rv3d->twdrawflag & MAN_SCALE_Y && rv3d->twdrawflag & MAN_SCALE_Z &&
              (twtype & V3D_GIZMO_SHOW_OBJECT_TRANSLATE) == 0 &&
              (twtype & V3D_GIZMO_SHOW_OBJECT_ROTATE) == 0);
    case MAN_AXIS_SCALE_ZX:
      return (rv3d->twdrawflag & MAN_SCALE_Z && rv3d->twdrawflag & MAN_SCALE_X &&
              (twtype & V3D_GIZMO_SHOW_OBJECT_TRANSLATE) == 0 &&
              (twtype & V3D_GIZMO_SHOW_OBJECT_ROTATE) == 0);
  }
  return false;
}

static void gizmo_get_axis_color(const int axis_idx,
                                 const float idot[3],
                                 float r_col[4],
                                 float r_col_hi[4])
{
  /* alpha values for normal/highlighted states */
  const float alpha = 0.6f;
  const float alpha_hi = 1.0f;
  float alpha_fac;

  if (axis_idx >= MAN_AXIS_RANGE_ROT_START && axis_idx < MAN_AXIS_RANGE_ROT_END) {
    /* Never fade rotation rings. */
    /* trackball rotation axis is a special case, we only draw a slight overlay */
    alpha_fac = (axis_idx == MAN_AXIS_ROT_T) ? 0.05f : 1.0f;
  }
  else {
    bool is_plane = false;
    const int axis_idx_norm = gizmo_orientation_axis(axis_idx, &is_plane);
    /* Get alpha fac based on axis angle,
     * to fade axis out when hiding it because it points towards view. */
    if (axis_idx_norm < 3) {
      const float idot_min = g_tw_axis_range[is_plane].min;
      const float idot_max = g_tw_axis_range[is_plane].max;
      float idot_axis = idot[axis_idx_norm];
      if (is_plane) {
        idot_axis = 1.0f - idot_axis;
      }
      alpha_fac = ((idot_axis > idot_max) ? 1.0f :
                   (idot_axis < idot_min) ? 0.0f :
                                            ((idot_axis - idot_min) / (idot_max - idot_min)));
    }
    else {
      alpha_fac = 1.0f;
    }
  }

  switch (axis_idx) {
    case MAN_AXIS_TRANS_X:
    case MAN_AXIS_ROT_X:
    case MAN_AXIS_SCALE_X:
    case MAN_AXIS_TRANS_YZ:
    case MAN_AXIS_SCALE_YZ:
      UI_GetThemeColor4fv(TH_AXIS_X, r_col);
      break;
    case MAN_AXIS_TRANS_Y:
    case MAN_AXIS_ROT_Y:
    case MAN_AXIS_SCALE_Y:
    case MAN_AXIS_TRANS_ZX:
    case MAN_AXIS_SCALE_ZX:
      UI_GetThemeColor4fv(TH_AXIS_Y, r_col);
      break;
    case MAN_AXIS_TRANS_Z:
    case MAN_AXIS_ROT_Z:
    case MAN_AXIS_SCALE_Z:
    case MAN_AXIS_TRANS_XY:
    case MAN_AXIS_SCALE_XY:
      UI_GetThemeColor4fv(TH_AXIS_Z, r_col);
      break;
    case MAN_AXIS_TRANS_C:
    case MAN_AXIS_ROT_C:
    case MAN_AXIS_SCALE_C:
    case MAN_AXIS_ROT_T:
      UI_GetThemeColor4fv(TH_GIZMO_VIEW_ALIGN, r_col);
      break;
  }

  copy_v4_v4(r_col_hi, r_col);

  r_col[3] = alpha * alpha_fac;
  r_col_hi[3] = alpha_hi * alpha_fac;
}

static void gizmo_get_axis_constraint(const int axis_idx, bool r_axis[3])
{
  ARRAY_SET_ITEMS(r_axis, 0, 0, 0);

  switch (axis_idx) {
    case MAN_AXIS_TRANS_X:
    case MAN_AXIS_ROT_X:
    case MAN_AXIS_SCALE_X:
      r_axis[0] = 1;
      break;
    case MAN_AXIS_TRANS_Y:
    case MAN_AXIS_ROT_Y:
    case MAN_AXIS_SCALE_Y:
      r_axis[1] = 1;
      break;
    case MAN_AXIS_TRANS_Z:
    case MAN_AXIS_ROT_Z:
    case MAN_AXIS_SCALE_Z:
      r_axis[2] = 1;
      break;
    case MAN_AXIS_TRANS_XY:
    case MAN_AXIS_SCALE_XY:
      r_axis[0] = r_axis[1] = 1;
      break;
    case MAN_AXIS_TRANS_YZ:
    case MAN_AXIS_SCALE_YZ:
      r_axis[1] = r_axis[2] = 1;
      break;
    case MAN_AXIS_TRANS_ZX:
    case MAN_AXIS_SCALE_ZX:
      r_axis[2] = r_axis[0] = 1;
      break;
    default:
      break;
  }
}

/* **************** Preparation Stuff **************** */

static void reset_tw_center(struct TransformBounds *tbounds)
{
  INIT_MINMAX(tbounds->min, tbounds->max);
  zero_v3(tbounds->center);

  for (int i = 0; i < 3; i++) {
    tbounds->axis_min[i] = +FLT_MAX;
    tbounds->axis_max[i] = -FLT_MAX;
  }
}

/* transform widget center calc helper for below */
static void calc_tw_center(struct TransformBounds *tbounds, const float co[3])
{
  minmax_v3v3_v3(tbounds->min, tbounds->max, co);
  add_v3_v3(tbounds->center, co);

  for (int i = 0; i < 3; i++) {
    const float d = dot_v3v3(tbounds->axis[i], co);
    tbounds->axis_min[i] = min_ff(d, tbounds->axis_min[i]);
    tbounds->axis_max[i] = max_ff(d, tbounds->axis_max[i]);
  }
}

static void calc_tw_center_with_matrix(struct TransformBounds *tbounds,
                                       const float co[3],
                                       const bool use_matrix,
                                       const float matrix[4][4])
{
  float co_world[3];
  if (use_matrix) {
    mul_v3_m4v3(co_world, matrix, co);
    co = co_world;
  }
  calc_tw_center(tbounds, co);
}

static void protectflag_to_drawflags(short protectflag, short *drawflags)
{
  if (protectflag & OB_LOCK_LOCX) {
    *drawflags &= ~MAN_TRANS_X;
  }
  if (protectflag & OB_LOCK_LOCY) {
    *drawflags &= ~MAN_TRANS_Y;
  }
  if (protectflag & OB_LOCK_LOCZ) {
    *drawflags &= ~MAN_TRANS_Z;
  }

  if (protectflag & OB_LOCK_ROTX) {
    *drawflags &= ~MAN_ROT_X;
  }
  if (protectflag & OB_LOCK_ROTY) {
    *drawflags &= ~MAN_ROT_Y;
  }
  if (protectflag & OB_LOCK_ROTZ) {
    *drawflags &= ~MAN_ROT_Z;
  }

  if (protectflag & OB_LOCK_SCALEX) {
    *drawflags &= ~MAN_SCALE_X;
  }
  if (protectflag & OB_LOCK_SCALEY) {
    *drawflags &= ~MAN_SCALE_Y;
  }
  if (protectflag & OB_LOCK_SCALEZ) {
    *drawflags &= ~MAN_SCALE_Z;
  }
}

/* for pose mode */
static void protectflag_to_drawflags_pchan(RegionView3D *rv3d,
                                           const bPoseChannel *pchan,
                                           short orientation_index)
{
  /* Protect-flags apply to local space in pose mode, so only let them influence axis
   * visibility if we show the global orientation, otherwise it's confusing. */
  if (ELEM(orientation_index, V3D_ORIENT_LOCAL, V3D_ORIENT_GIMBAL)) {
    protectflag_to_drawflags(pchan->protectflag, &rv3d->twdrawflag);
  }
}

/* For editmode. */
static void protectflag_to_drawflags_ebone(RegionView3D *rv3d, const EditBone *ebo)
{
  if (ebo->flag & BONE_EDITMODE_LOCKED) {
    protectflag_to_drawflags(OB_LOCK_LOC | OB_LOCK_ROT | OB_LOCK_SCALE, &rv3d->twdrawflag);
  }
}

/* could move into BLI_math however this is only useful for display/editing purposes */
static void axis_angle_to_gimbal_axis(float gmat[3][3], const float axis[3], const float angle)
{
  /* X/Y are arbitrary axes, most importantly Z is the axis of rotation. */

  float cross_vec[3];
  float quat[4];

  /* this is an un-scientific method to get a vector to cross with
   * XYZ intentionally YZX */
  cross_vec[0] = axis[1];
  cross_vec[1] = axis[2];
  cross_vec[2] = axis[0];

  /* X-axis */
  cross_v3_v3v3(gmat[0], cross_vec, axis);
  normalize_v3(gmat[0]);
  axis_angle_to_quat(quat, axis, angle);
  mul_qt_v3(quat, gmat[0]);

  /* Y-axis */
  axis_angle_to_quat(quat, axis, M_PI_2);
  copy_v3_v3(gmat[1], gmat[0]);
  mul_qt_v3(quat, gmat[1]);

  /* Z-axis */
  copy_v3_v3(gmat[2], axis);

  normalize_m3(gmat);
}

static bool test_rotmode_euler(short rotmode)
{
  return (ELEM(rotmode, ROT_MODE_AXISANGLE, ROT_MODE_QUAT)) ? 0 : 1;
}

bool gimbal_axis_pose(Object *ob, const bPoseChannel *pchan, float gmat[3][3])
{
  float mat[3][3], tmat[3][3], obmat[3][3];
  if (test_rotmode_euler(pchan->rotmode)) {
    eulO_to_gimbal_axis(mat, pchan->eul, pchan->rotmode);
  }
  else if (pchan->rotmode == ROT_MODE_AXISANGLE) {
    axis_angle_to_gimbal_axis(mat, pchan->rotAxis, pchan->rotAngle);
  }
  else { /* quat */
    return 0;
  }

  /* apply bone transformation */
  mul_m3_m3m3(tmat, pchan->bone->bone_mat, mat);

  if (pchan->parent) {
    float parent_mat[3][3];

    copy_m3_m4(parent_mat,
               (pchan->bone->flag & BONE_HINGE) ? pchan->parent->bone->arm_mat :
                                                  pchan->parent->pose_mat);
    mul_m3_m3m3(mat, parent_mat, tmat);

    /* needed if object transformation isn't identity */
    copy_m3_m4(obmat, ob->obmat);
    mul_m3_m3m3(gmat, obmat, mat);
  }
  else {
    /* needed if object transformation isn't identity */
    copy_m3_m4(obmat, ob->obmat);
    mul_m3_m3m3(gmat, obmat, tmat);
  }

  normalize_m3(gmat);
  return true;
}

bool gimbal_axis_object(Object *ob, float gmat[3][3])
{
  if (test_rotmode_euler(ob->rotmode)) {
    eulO_to_gimbal_axis(gmat, ob->rot, ob->rotmode);
  }
  else if (ob->rotmode == ROT_MODE_AXISANGLE) {
    axis_angle_to_gimbal_axis(gmat, ob->rotAxis, ob->rotAngle);
  }
  else { /* quat */
    return 0;
  }

  if (ob->parent) {
    float parent_mat[3][3];
    copy_m3_m4(parent_mat, ob->parent->obmat);
    normalize_m3(parent_mat);
    mul_m3_m3m3(gmat, parent_mat, gmat);
  }
  return 1;
}

int ED_transform_calc_gizmo_stats(const bContext *C,
                                  const struct TransformCalcParams *params,
                                  struct TransformBounds *tbounds)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  Scene *scene = CTX_data_scene(C);
  /* TODO(sergey): This function is used from operator's modal() and from gizmo's refresh().
   * Is it fine to possibly evaluate dependency graph here? */
  Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = area->spacedata.first;
  RegionView3D *rv3d = region->regiondata;
  Base *base;
  bGPdata *gpd = CTX_data_gpencil_data(C);
  const bool is_gp_edit = GPENCIL_ANY_MODE(gpd);
  const bool is_curve_edit = GPENCIL_CURVE_EDIT_SESSIONS_ON(gpd);
  int a, totsel = 0;

  const int pivot_point = scene->toolsettings->transform_pivot_point;
  const short orient_index = params->orientation_index ?
                                 (params->orientation_index - 1) :
                                 BKE_scene_orientation_get_index(scene, SCE_ORIENT_DEFAULT);

  Object *ob = OBACT(view_layer);
  Object *obedit = OBEDIT_FROM_OBACT(ob);
  if (ob && ob->mode & OB_MODE_WEIGHT_PAINT) {
    Object *obpose = BKE_object_pose_armature_get(ob);
    if (obpose != NULL) {
      ob = obpose;
    }
  }

  tbounds->use_matrix_space = false;

  /* transform widget matrix */
  unit_m4(rv3d->twmat);

  unit_m3(rv3d->tw_axis_matrix);
  zero_v3(rv3d->tw_axis_min);
  zero_v3(rv3d->tw_axis_max);

  rv3d->twdrawflag = 0xFFFF;

  /* global, local or normal orientation?
   * if we could check 'totsel' now, this should be skipped with no selection. */
  if (ob) {
    float mat[3][3];
    ED_transform_calc_orientation_from_type_ex(
        scene, view_layer, v3d, rv3d, ob, obedit, orient_index, pivot_point, mat);
    copy_m4_m3(rv3d->twmat, mat);
  }

  /* transform widget centroid/center */
  reset_tw_center(tbounds);

  copy_m3_m4(tbounds->axis, rv3d->twmat);
  if (params->use_local_axis && (ob && ob->mode & (OB_MODE_EDIT | OB_MODE_POSE))) {
    float diff_mat[3][3];
    copy_m3_m4(diff_mat, ob->obmat);
    normalize_m3(diff_mat);
    invert_m3(diff_mat);
    mul_m3_m3m3(tbounds->axis, tbounds->axis, diff_mat);
    normalize_m3(tbounds->axis);

    tbounds->use_matrix_space = true;
    copy_m4_m4(tbounds->matrix_space, ob->obmat);
  }

  if (is_gp_edit) {
    float diff_mat[4][4];
    const bool use_mat_local = true;
    LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
      /* only editable and visible layers are considered */

      if (BKE_gpencil_layer_is_editable(gpl) && (gpl->actframe != NULL)) {

        /* calculate difference matrix */
        BKE_gpencil_layer_transform_matrix_get(depsgraph, ob, gpl, diff_mat);

        LISTBASE_FOREACH (bGPDstroke *, gps, &gpl->actframe->strokes) {
          /* skip strokes that are invalid for current view */
          if (ED_gpencil_stroke_can_use(C, gps) == false) {
            continue;
          }

          if (is_curve_edit) {
            if (gps->editcurve == NULL) {
              continue;
            }

            bGPDcurve *gpc = gps->editcurve;
            if (gpc->flag & GP_CURVE_SELECT) {
              for (uint32_t i = 0; i < gpc->tot_curve_points; i++) {
                bGPDcurve_point *gpc_pt = &gpc->curve_points[i];
                BezTriple *bezt = &gpc_pt->bezt;
                if (gpc_pt->flag & GP_CURVE_POINT_SELECT) {
                  for (uint32_t j = 0; j < 3; j++) {
                    if (BEZT_ISSEL_IDX(bezt, j)) {
                      calc_tw_center_with_matrix(tbounds, bezt->vec[j], use_mat_local, diff_mat);
                      totsel++;
                    }
                  }
                }
              }
            }
          }
          else {
            /* we're only interested in selected points here... */
            if (gps->flag & GP_STROKE_SELECT) {
              bGPDspoint *pt;
              int i;

              /* Change selection status of all points, then make the stroke match */
              for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
                if (pt->flag & GP_SPOINT_SELECT) {
                  calc_tw_center_with_matrix(tbounds, &pt->x, use_mat_local, diff_mat);
                  totsel++;
                }
              }
            }
          }
        }
      }
    }

    /* selection center */
    if (totsel) {
      mul_v3_fl(tbounds->center, 1.0f / (float)totsel); /* centroid! */
    }
  }
  else if (obedit) {

#define FOREACH_EDIT_OBJECT_BEGIN(ob_iter, use_mat_local) \
  { \
    invert_m4_m4(obedit->imat, obedit->obmat); \
    uint objects_len = 0; \
    Object **objects = BKE_view_layer_array_from_objects_in_edit_mode( \
        view_layer, CTX_wm_view3d(C), &objects_len); \
    for (uint ob_index = 0; ob_index < objects_len; ob_index++) { \
      Object *ob_iter = objects[ob_index]; \
      const bool use_mat_local = (ob_iter != obedit);

#define FOREACH_EDIT_OBJECT_END() \
  } \
  MEM_freeN(objects); \
  } \
  ((void)0)

    ob = obedit;
    if (obedit->type == OB_MESH) {
      FOREACH_EDIT_OBJECT_BEGIN (ob_iter, use_mat_local) {
        BMEditMesh *em_iter = BKE_editmesh_from_object(ob_iter);
        BMesh *bm = em_iter->bm;

        if (bm->totvertsel == 0) {
          continue;
        }

        BMVert *eve;
        BMIter iter;

        float mat_local[4][4];
        if (use_mat_local) {
          mul_m4_m4m4(mat_local, obedit->imat, ob_iter->obmat);
        }

        BM_ITER_MESH (eve, &iter, bm, BM_VERTS_OF_MESH) {
          if (!BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
            if (BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
              calc_tw_center_with_matrix(tbounds, eve->co, use_mat_local, mat_local);
              totsel++;
            }
          }
        }
      }
      FOREACH_EDIT_OBJECT_END();
    } /* end editmesh */
    else if (obedit->type == OB_ARMATURE) {
      FOREACH_EDIT_OBJECT_BEGIN (ob_iter, use_mat_local) {
        bArmature *arm = ob_iter->data;

        float mat_local[4][4];
        if (use_mat_local) {
          mul_m4_m4m4(mat_local, obedit->imat, ob_iter->obmat);
        }
        LISTBASE_FOREACH (EditBone *, ebo, arm->edbo) {
          if (EBONE_VISIBLE(arm, ebo)) {
            if (ebo->flag & BONE_TIPSEL) {
              calc_tw_center_with_matrix(tbounds, ebo->tail, use_mat_local, mat_local);
              totsel++;
            }
            if ((ebo->flag & BONE_ROOTSEL) &&
                /* don't include same point multiple times */
                ((ebo->flag & BONE_CONNECTED) && (ebo->parent != NULL) &&
                 (ebo->parent->flag & BONE_TIPSEL) && EBONE_VISIBLE(arm, ebo->parent)) == 0) {
              calc_tw_center_with_matrix(tbounds, ebo->head, use_mat_local, mat_local);
              totsel++;
            }
            if (ebo->flag & (BONE_SELECTED | BONE_ROOTSEL | BONE_TIPSEL)) {
              protectflag_to_drawflags_ebone(rv3d, ebo);
            }
          }
        }
      }
      FOREACH_EDIT_OBJECT_END();
    }
    else if (ELEM(obedit->type, OB_CURVES_LEGACY, OB_SURF)) {
      FOREACH_EDIT_OBJECT_BEGIN (ob_iter, use_mat_local) {
        Curve *cu = ob_iter->data;
        Nurb *nu;
        BezTriple *bezt;
        BPoint *bp;
        ListBase *nurbs = BKE_curve_editNurbs_get(cu);

        float mat_local[4][4];
        if (use_mat_local) {
          mul_m4_m4m4(mat_local, obedit->imat, ob_iter->obmat);
        }

        nu = nurbs->first;
        while (nu) {
          if (nu->type == CU_BEZIER) {
            bezt = nu->bezt;
            a = nu->pntsu;
            while (a--) {
              /* exceptions
               * if handles are hidden then only check the center points.
               * If the center knot is selected then only use this as the center point.
               */
              if (v3d->overlay.handle_display == CURVE_HANDLE_NONE) {
                if (bezt->f2 & SELECT) {
                  calc_tw_center_with_matrix(tbounds, bezt->vec[1], use_mat_local, mat_local);
                  totsel++;
                }
              }
              else if (bezt->f2 & SELECT) {
                calc_tw_center_with_matrix(tbounds, bezt->vec[1], use_mat_local, mat_local);
                totsel++;
              }
              else {
                if (bezt->f1 & SELECT) {
                  const float *co = bezt->vec[(pivot_point == V3D_AROUND_LOCAL_ORIGINS) ? 1 : 0];
                  calc_tw_center_with_matrix(tbounds, co, use_mat_local, mat_local);
                  totsel++;
                }
                if (bezt->f3 & SELECT) {
                  const float *co = bezt->vec[(pivot_point == V3D_AROUND_LOCAL_ORIGINS) ? 1 : 2];
                  calc_tw_center_with_matrix(tbounds, co, use_mat_local, mat_local);
                  totsel++;
                }
              }
              bezt++;
            }
          }
          else {
            bp = nu->bp;
            a = nu->pntsu * nu->pntsv;
            while (a--) {
              if (bp->f1 & SELECT) {
                calc_tw_center_with_matrix(tbounds, bp->vec, use_mat_local, mat_local);
                totsel++;
              }
              bp++;
            }
          }
          nu = nu->next;
        }
      }
      FOREACH_EDIT_OBJECT_END();
    }
    else if (obedit->type == OB_MBALL) {
      FOREACH_EDIT_OBJECT_BEGIN (ob_iter, use_mat_local) {
        MetaBall *mb = (MetaBall *)ob_iter->data;

        float mat_local[4][4];
        if (use_mat_local) {
          mul_m4_m4m4(mat_local, obedit->imat, ob_iter->obmat);
        }

        LISTBASE_FOREACH (MetaElem *, ml, mb->editelems) {
          if (ml->flag & SELECT) {
            calc_tw_center_with_matrix(tbounds, &ml->x, use_mat_local, mat_local);
            totsel++;
          }
        }
      }
      FOREACH_EDIT_OBJECT_END();
    }
    else if (obedit->type == OB_LATTICE) {
      FOREACH_EDIT_OBJECT_BEGIN (ob_iter, use_mat_local) {
        Lattice *lt = ((Lattice *)ob_iter->data)->editlatt->latt;
        BPoint *bp = lt->def;
        a = lt->pntsu * lt->pntsv * lt->pntsw;

        float mat_local[4][4];
        if (use_mat_local) {
          mul_m4_m4m4(mat_local, obedit->imat, ob_iter->obmat);
        }

        while (a--) {
          if (bp->f1 & SELECT) {
            calc_tw_center_with_matrix(tbounds, bp->vec, use_mat_local, mat_local);
            totsel++;
          }
          bp++;
        }
      }
      FOREACH_EDIT_OBJECT_END();
    }

#undef FOREACH_EDIT_OBJECT_BEGIN
#undef FOREACH_EDIT_OBJECT_END

    /* selection center */
    if (totsel) {
      mul_v3_fl(tbounds->center, 1.0f / (float)totsel); /* centroid! */
      mul_m4_v3(obedit->obmat, tbounds->center);
      mul_m4_v3(obedit->obmat, tbounds->min);
      mul_m4_v3(obedit->obmat, tbounds->max);
    }
  }
  else if (ob && (ob->mode & OB_MODE_POSE)) {
    invert_m4_m4(ob->imat, ob->obmat);

    uint objects_len = 0;
    Object **objects = BKE_object_pose_array_get(view_layer, v3d, &objects_len);

    for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
      Object *ob_iter = objects[ob_index];
      const bool use_mat_local = (ob_iter != ob);
      /* mislead counting bones... bah. We don't know the gizmo mode, could be mixed */
      const int mode = TFM_ROTATION;

      transform_convert_pose_transflags_update(ob_iter, mode, V3D_AROUND_CENTER_BOUNDS);

      float mat_local[4][4];
      if (use_mat_local) {
        mul_m4_m4m4(mat_local, ob->imat, ob_iter->obmat);
      }

      /* Use channels to get stats. */
      LISTBASE_FOREACH (bPoseChannel *, pchan, &ob_iter->pose->chanbase) {
        if (!(pchan->bone->flag & BONE_TRANSFORM)) {
          continue;
        }
        calc_tw_center_with_matrix(tbounds, pchan->pose_head, use_mat_local, mat_local);
        protectflag_to_drawflags_pchan(rv3d, pchan, orient_index);
        totsel++;
      }
    }
    MEM_freeN(objects);

    if (totsel) {
      mul_v3_fl(tbounds->center, 1.0f / (float)totsel); /* centroid! */
      mul_m4_v3(ob->obmat, tbounds->center);
      mul_m4_v3(ob->obmat, tbounds->min);
      mul_m4_v3(ob->obmat, tbounds->max);
    }
  }
  else if (ob && (ob->mode & OB_MODE_ALL_PAINT)) {
    if (ob->mode & OB_MODE_SCULPT) {
      totsel = 1;
      calc_tw_center_with_matrix(tbounds, ob->sculpt->pivot_pos, false, ob->obmat);
      mul_m4_v3(ob->obmat, tbounds->center);
      mul_m4_v3(ob->obmat, tbounds->min);
      mul_m4_v3(ob->obmat, tbounds->max);
    }
  }
  else if (ob && ob->mode & OB_MODE_PARTICLE_EDIT) {
    PTCacheEdit *edit = PE_get_current(depsgraph, scene, ob);
    PTCacheEditPoint *point;
    PTCacheEditKey *ek;
    int k;

    if (edit) {
      point = edit->points;
      for (a = 0; a < edit->totpoint; a++, point++) {
        if (point->flag & PEP_HIDE) {
          continue;
        }

        for (k = 0, ek = point->keys; k < point->totkey; k++, ek++) {
          if (ek->flag & PEK_SELECT) {
            calc_tw_center(tbounds, (ek->flag & PEK_USE_WCO) ? ek->world_co : ek->co);
            totsel++;
          }
        }
      }

      /* selection center */
      if (totsel) {
        mul_v3_fl(tbounds->center, 1.0f / (float)totsel); /* centroid! */
      }
    }
  }
  else {

    /* we need the one selected object, if its not active */
    base = BASACT(view_layer);
    ob = OBACT(view_layer);
    if (base && ((base->flag & BASE_SELECTED) == 0)) {
      ob = NULL;
    }

    for (base = view_layer->object_bases.first; base; base = base->next) {
      if (!BASE_SELECTED_EDITABLE(v3d, base)) {
        continue;
      }
      if (ob == NULL) {
        ob = base->object;
      }

      /* Get the boundbox out of the evaluated object. */
      const BoundBox *bb = NULL;
      if (params->use_only_center == false) {
        bb = BKE_object_boundbox_get(base->object);
      }

      if (params->use_only_center || (bb == NULL)) {
        calc_tw_center(tbounds, base->object->obmat[3]);
      }
      else {
        for (uint j = 0; j < 8; j++) {
          float co[3];
          mul_v3_m4v3(co, base->object->obmat, bb->vec[j]);
          calc_tw_center(tbounds, co);
        }
      }

      if (orient_index == V3D_ORIENT_GLOBAL) {
        /* Protect-flags apply to world space in object mode,
         * so only let them influence axis visibility if we show the global orientation,
         * otherwise it's confusing. */
        protectflag_to_drawflags(base->object->protectflag & OB_LOCK_LOC, &rv3d->twdrawflag);
      }
      else if (ELEM(orient_index, V3D_ORIENT_LOCAL, V3D_ORIENT_GIMBAL)) {
        protectflag_to_drawflags(base->object->protectflag, &rv3d->twdrawflag);
      }
      totsel++;
    }

    /* selection center */
    if (totsel) {
      mul_v3_fl(tbounds->center, 1.0f / (float)totsel); /* centroid! */
    }
  }

  if (totsel == 0) {
    unit_m4(rv3d->twmat);
  }
  else {
    copy_v3_v3(rv3d->tw_axis_min, tbounds->axis_min);
    copy_v3_v3(rv3d->tw_axis_max, tbounds->axis_max);
    copy_m3_m3(rv3d->tw_axis_matrix, tbounds->axis);
  }

  return totsel;
}

static void gizmo_get_idot(RegionView3D *rv3d, float r_idot[3])
{
  float view_vec[3], axis_vec[3];
  ED_view3d_global_to_vector(rv3d, rv3d->twmat[3], view_vec);
  for (int i = 0; i < 3; i++) {
    normalize_v3_v3(axis_vec, rv3d->twmat[i]);
    r_idot[i] = 1.0f - fabsf(dot_v3v3(view_vec, axis_vec));
  }
}

static void gizmo_prepare_mat(const bContext *C,
                              RegionView3D *rv3d,
                              const struct TransformBounds *tbounds)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  switch (scene->toolsettings->transform_pivot_point) {
    case V3D_AROUND_CENTER_BOUNDS:
    case V3D_AROUND_ACTIVE: {
      mid_v3_v3v3(rv3d->twmat[3], tbounds->min, tbounds->max);

      if (scene->toolsettings->transform_pivot_point == V3D_AROUND_ACTIVE) {
        bGPdata *gpd = CTX_data_gpencil_data(C);
        if (gpd && (gpd->flag & GP_DATA_STROKE_EDITMODE)) {
          /* pass */
        }
        else {
          Object *ob = OBACT(view_layer);
          if (ob != NULL) {
            if ((ob->mode & OB_MODE_ALL_SCULPT) && ob->sculpt) {
              SculptSession *ss = ob->sculpt;
              copy_v3_v3(rv3d->twmat[3], ss->pivot_pos);
            }
            else {
              ED_object_calc_active_center(ob, false, rv3d->twmat[3]);
            }
          }
        }
      }
      break;
    }
    case V3D_AROUND_LOCAL_ORIGINS:
    case V3D_AROUND_CENTER_MEDIAN:
      copy_v3_v3(rv3d->twmat[3], tbounds->center);
      break;
    case V3D_AROUND_CURSOR:
      copy_v3_v3(rv3d->twmat[3], scene->cursor.location);
      break;
  }
}

/**
 * Sets up \a r_start and \a r_len to define arrow line range.
 * Needed to adjust line drawing for combined gizmo axis types.
 */
static void gizmo_line_range(const int twtype, const short axis_type, float *r_start, float *r_len)
{
  const float ofs = 0.2f;

  *r_start = 0.2f;
  *r_len = 1.0f;

  switch (axis_type) {
    case MAN_AXES_TRANSLATE:
      if (twtype & V3D_GIZMO_SHOW_OBJECT_SCALE) {
        *r_start = *r_len - ofs + 0.075f;
      }
      if (twtype & V3D_GIZMO_SHOW_OBJECT_ROTATE) {
        *r_len += ofs;
      }
      break;
    case MAN_AXES_SCALE:
      if (twtype & (V3D_GIZMO_SHOW_OBJECT_TRANSLATE | V3D_GIZMO_SHOW_OBJECT_ROTATE)) {
        *r_len -= ofs + 0.025f;
      }
      break;
  }

  *r_len -= *r_start;
}

static void gizmo_xform_message_subscribe(wmGizmoGroup *gzgroup,
                                          struct wmMsgBus *mbus,
                                          Scene *scene,
                                          bScreen *screen,
                                          ScrArea *area,
                                          ARegion *region,
                                          const void *type_fn)
{
  /* Subscribe to view properties */
  wmMsgSubscribeValue msg_sub_value_gz_tag_refresh = {
      .owner = region,
      .user_data = gzgroup->parent_gzmap,
      .notify = WM_gizmo_do_msg_notify_tag_refresh,
  };

  int orient_flag = 0;
  if (type_fn == VIEW3D_GGT_xform_gizmo) {
    GizmoGroup *ggd = gzgroup->customdata;
    orient_flag = ggd->twtype_init;
  }
  else if (type_fn == VIEW3D_GGT_xform_cage) {
    orient_flag = V3D_GIZMO_SHOW_OBJECT_SCALE;
    /* pass */
  }
  else if (type_fn == VIEW3D_GGT_xform_shear) {
    orient_flag = V3D_GIZMO_SHOW_OBJECT_ROTATE;
  }
  TransformOrientationSlot *orient_slot = BKE_scene_orientation_slot_get_from_flag(scene,
                                                                                   orient_flag);
  PointerRNA orient_ref_ptr;
  RNA_pointer_create(&scene->id, &RNA_TransformOrientationSlot, orient_slot, &orient_ref_ptr);
  const ToolSettings *ts = scene->toolsettings;

  PointerRNA scene_ptr;
  RNA_id_pointer_create(&scene->id, &scene_ptr);
  {
    const PropertyRNA *props[] = {
        &rna_Scene_transform_orientation_slots,
    };
    for (int i = 0; i < ARRAY_SIZE(props); i++) {
      WM_msg_subscribe_rna(mbus, &scene_ptr, props[i], &msg_sub_value_gz_tag_refresh, __func__);
    }
  }

  if ((ts->transform_pivot_point == V3D_AROUND_CURSOR) ||
      (orient_slot->type == V3D_ORIENT_CURSOR)) {
    /* We could be more specific here, for now subscribe to any cursor change. */
    PointerRNA cursor_ptr;
    RNA_pointer_create(&scene->id, &RNA_View3DCursor, &scene->cursor, &cursor_ptr);
    WM_msg_subscribe_rna(mbus, &cursor_ptr, NULL, &msg_sub_value_gz_tag_refresh, __func__);
  }

  {
    const PropertyRNA *props[] = {
        &rna_TransformOrientationSlot_type,
        &rna_TransformOrientationSlot_use,
    };
    for (int i = 0; i < ARRAY_SIZE(props); i++) {
      if (props[i]) {
        WM_msg_subscribe_rna(
            mbus, &orient_ref_ptr, props[i], &msg_sub_value_gz_tag_refresh, __func__);
      }
    }
  }

  PointerRNA toolsettings_ptr;
  RNA_pointer_create(&scene->id, &RNA_ToolSettings, scene->toolsettings, &toolsettings_ptr);

  if (ELEM(type_fn, VIEW3D_GGT_xform_gizmo, VIEW3D_GGT_xform_shear)) {
    const PropertyRNA *props[] = {
        &rna_ToolSettings_transform_pivot_point,
    };
    for (int i = 0; i < ARRAY_SIZE(props); i++) {
      WM_msg_subscribe_rna(
          mbus, &toolsettings_ptr, props[i], &msg_sub_value_gz_tag_refresh, __func__);
    }
  }

  {
    const PropertyRNA *props[] = {
        &rna_ToolSettings_workspace_tool_type,
    };
    for (int i = 0; i < ARRAY_SIZE(props); i++) {
      WM_msg_subscribe_rna(
          mbus, &toolsettings_ptr, props[i], &msg_sub_value_gz_tag_refresh, __func__);
    }
  }

  PointerRNA view3d_ptr;
  RNA_pointer_create(&screen->id, &RNA_SpaceView3D, area->spacedata.first, &view3d_ptr);

  if (type_fn == VIEW3D_GGT_xform_gizmo) {
    GizmoGroup *ggd = gzgroup->customdata;
    if (ggd->use_twtype_refresh) {
      const PropertyRNA *props[] = {
          &rna_SpaceView3D_show_gizmo_object_translate,
          &rna_SpaceView3D_show_gizmo_object_rotate,
          &rna_SpaceView3D_show_gizmo_object_scale,
      };
      for (int i = 0; i < ARRAY_SIZE(props); i++) {
        WM_msg_subscribe_rna(mbus, &view3d_ptr, props[i], &msg_sub_value_gz_tag_refresh, __func__);
      }
    }
  }
  else if (type_fn == VIEW3D_GGT_xform_cage) {
    /* pass */
  }
  else if (type_fn == VIEW3D_GGT_xform_shear) {
    /* pass */
  }
  else {
    BLI_assert(0);
  }

  WM_msg_subscribe_rna_anon_prop(mbus, Window, view_layer, &msg_sub_value_gz_tag_refresh);
  WM_msg_subscribe_rna_anon_prop(mbus, EditBone, lock, &msg_sub_value_gz_tag_refresh);
}

static void gizmo_3d_dial_matrixbasis_calc(const ARegion *region,
                                           float axis[3],
                                           float center_global[3],
                                           float mval_init[2],
                                           float r_mat_basis[4][4])
{
  copy_v3_v3(r_mat_basis[2], axis);
  copy_v3_v3(r_mat_basis[3], center_global);
  r_mat_basis[2][3] = -dot_v3v3(axis, center_global);

  if (ED_view3d_win_to_3d_on_plane(region, axis, mval_init, false, r_mat_basis[1])) {
    sub_v3_v3(r_mat_basis[1], center_global);
    normalize_v3(r_mat_basis[1]);
    cross_v3_v3v3(r_mat_basis[0], r_mat_basis[1], r_mat_basis[2]);
  }
  else {
    /* The plane and the mouse direction are parallel.
     * Calculate a matrix orthogonal to the axis. */
    ortho_basis_v3v3_v3(r_mat_basis[0], r_mat_basis[1], r_mat_basis[2]);
  }

  r_mat_basis[0][3] = 0.0f;
  r_mat_basis[1][3] = 0.0f;
  r_mat_basis[2][3] = 0.0f;
  r_mat_basis[3][3] = 1.0f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform Gizmo
 * \{ */

/** Scale of the two-axis planes. */
#define MAN_AXIS_SCALE_PLANE_SCALE 0.07f
/** Offset of the two-axis planes, depends on the gizmos scale. Define to avoid repeating. */
#define MAN_AXIS_SCALE_PLANE_OFFSET 7.0f

static void rotation_get_fn(const wmGizmo *UNUSED(gz), wmGizmoProperty *gz_prop, void *value)
{
  const GizmoGroup *ggd = (const GizmoGroup *)gz_prop->custom_func.user_data;
  *(float *)value = ggd->rotation;
}

static void rotation_set_fn(const wmGizmo *UNUSED(gz), wmGizmoProperty *gz_prop, const void *value)
{
  GizmoGroup *ggd = (GizmoGroup *)gz_prop->custom_func.user_data;
  ggd->rotation = *(const float *)value;
}

static GizmoGroup *gizmogroup_init(wmGizmoGroup *gzgroup)
{
  GizmoGroup *ggd;

  ggd = MEM_callocN(sizeof(GizmoGroup), "gizmo_data");

  const wmGizmoType *gzt_arrow = WM_gizmotype_find("GIZMO_GT_arrow_3d", true);
  const wmGizmoType *gzt_dial = WM_gizmotype_find("GIZMO_GT_dial_3d", true);
  const wmGizmoType *gzt_prim = WM_gizmotype_find("GIZMO_GT_primitive_3d", true);

  wmGizmoPropertyFnParams params = {
      .value_get_fn = rotation_get_fn, .value_set_fn = rotation_set_fn, .user_data = ggd};

#define GIZMO_NEW_ARROW(v, draw_style) \
  { \
    ggd->gizmos[v] = WM_gizmo_new_ptr(gzt_arrow, gzgroup, NULL); \
    RNA_enum_set(ggd->gizmos[v]->ptr, "draw_style", draw_style); \
  } \
  ((void)0)
#define GIZMO_NEW_DIAL(v, draw_options) \
  { \
    ggd->gizmos[v] = WM_gizmo_new_ptr(gzt_dial, gzgroup, NULL); \
    RNA_enum_set(ggd->gizmos[v]->ptr, "draw_options", draw_options); \
    WM_gizmo_target_property_def_func(ggd->gizmos[v], "offset", &params); \
  } \
  ((void)0)
#define GIZMO_NEW_PRIM(v, draw_style) \
  { \
    ggd->gizmos[v] = WM_gizmo_new_ptr(gzt_prim, gzgroup, NULL); \
    RNA_enum_set(ggd->gizmos[v]->ptr, "draw_style", draw_style); \
  } \
  ((void)0)

  /* add/init widgets - order matters! */
  GIZMO_NEW_DIAL(MAN_AXIS_ROT_T, ED_GIZMO_DIAL_DRAW_FLAG_FILL);

  GIZMO_NEW_DIAL(MAN_AXIS_SCALE_C, ED_GIZMO_DIAL_DRAW_FLAG_FILL_SELECT);

  GIZMO_NEW_ARROW(MAN_AXIS_SCALE_X, ED_GIZMO_ARROW_STYLE_BOX);
  GIZMO_NEW_ARROW(MAN_AXIS_SCALE_Y, ED_GIZMO_ARROW_STYLE_BOX);
  GIZMO_NEW_ARROW(MAN_AXIS_SCALE_Z, ED_GIZMO_ARROW_STYLE_BOX);

  GIZMO_NEW_PRIM(MAN_AXIS_SCALE_XY, ED_GIZMO_PRIMITIVE_STYLE_PLANE);
  GIZMO_NEW_PRIM(MAN_AXIS_SCALE_YZ, ED_GIZMO_PRIMITIVE_STYLE_PLANE);
  GIZMO_NEW_PRIM(MAN_AXIS_SCALE_ZX, ED_GIZMO_PRIMITIVE_STYLE_PLANE);

  GIZMO_NEW_DIAL(MAN_AXIS_ROT_X, ED_GIZMO_DIAL_DRAW_FLAG_CLIP);
  GIZMO_NEW_DIAL(MAN_AXIS_ROT_Y, ED_GIZMO_DIAL_DRAW_FLAG_CLIP);
  GIZMO_NEW_DIAL(MAN_AXIS_ROT_Z, ED_GIZMO_DIAL_DRAW_FLAG_CLIP);

  /* init screen aligned widget last here, looks better, behaves better */
  GIZMO_NEW_DIAL(MAN_AXIS_ROT_C, ED_GIZMO_DIAL_DRAW_FLAG_NOP);

  GIZMO_NEW_DIAL(MAN_AXIS_TRANS_C, ED_GIZMO_DIAL_DRAW_FLAG_FILL_SELECT);

  GIZMO_NEW_ARROW(MAN_AXIS_TRANS_X, ED_GIZMO_ARROW_STYLE_NORMAL);
  GIZMO_NEW_ARROW(MAN_AXIS_TRANS_Y, ED_GIZMO_ARROW_STYLE_NORMAL);
  GIZMO_NEW_ARROW(MAN_AXIS_TRANS_Z, ED_GIZMO_ARROW_STYLE_NORMAL);

  GIZMO_NEW_PRIM(MAN_AXIS_TRANS_XY, ED_GIZMO_PRIMITIVE_STYLE_PLANE);
  GIZMO_NEW_PRIM(MAN_AXIS_TRANS_YZ, ED_GIZMO_PRIMITIVE_STYLE_PLANE);
  GIZMO_NEW_PRIM(MAN_AXIS_TRANS_ZX, ED_GIZMO_PRIMITIVE_STYLE_PLANE);

  ggd->gizmos[MAN_AXIS_ROT_T]->flag |= WM_GIZMO_SELECT_BACKGROUND;

  /* Prevent axis gizmos overlapping the center point, see: T63744. */
  ggd->gizmos[MAN_AXIS_TRANS_C]->select_bias = 2.0f;

  ggd->gizmos[MAN_AXIS_SCALE_C]->select_bias = -2.0f;

  /* Use 1/6 since this is '0.2' if the main scale is 1.2. */
  RNA_float_set(ggd->gizmos[MAN_AXIS_SCALE_C]->ptr, "arc_inner_factor", 1.0 / 6.0);

  return ggd;
}

/**
 * Custom handler for gizmo widgets
 */
static int gizmo_modal(bContext *C,
                       wmGizmo *widget,
                       const wmEvent *event,
                       eWM_GizmoFlagTweak UNUSED(tweak_flag))
{
  /* Avoid unnecessary updates, partially address: T55458. */
  if (ELEM(event->type, TIMER, INBETWEEN_MOUSEMOVE)) {
    return OPERATOR_RUNNING_MODAL;
  }

  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = region->regiondata;
  wmGizmoGroup *gzgroup = widget->parent_gzgroup;

  /* Recalculating the orientation has two problems.
   * - The matrix calculated based on the transformed selection may not match the matrix
   *   that was set when transform started.
   * - Inspecting the selection for every update is expensive (for *every* redraw).
   *
   * Instead, use #transform_apply_matrix to transform `rv3d->twmat` or the final scale value
   * when scaling.
   */
  if (false) {
    struct TransformBounds tbounds;

    if (ED_transform_calc_gizmo_stats(C,
                                      &(struct TransformCalcParams){
                                          .use_only_center = true,
                                      },
                                      &tbounds)) {
      gizmo_prepare_mat(C, rv3d, &tbounds);
      for (wmGizmo *gz = gzgroup->gizmos.first; gz; gz = gz->next) {
        WM_gizmo_set_matrix_location(gz, rv3d->twmat[3]);
      }
    }
  }
  else {
    GizmoGroup *ggd = gzgroup->customdata;

    short axis_type = 0;
    MAN_ITER_AXES_BEGIN (axis, axis_idx) {
      if (axis == widget) {
        axis_type = gizmo_get_axis_type(axis_idx);
        break;
      }
    }
    MAN_ITER_AXES_END;

    /* Showing axes which aren't being manipulated doesn't always work so well.
     *
     * For rotate: global axis will reset after finish.
     * Also, gimbal axis isn't properly recalculated while transforming.
     */
    if (axis_type == MAN_AXES_ROTATE) {
      MAN_ITER_AXES_BEGIN (axis, axis_idx) {
        if (axis == widget) {
          continue;
        }

        bool is_plane_dummy;
        const uint aidx_norm = gizmo_orientation_axis(axis_idx, &is_plane_dummy);
        /* Always show the axis-aligned handle as it's distracting when it's disabled. */
        if (aidx_norm == 3) {
          continue;
        }
        WM_gizmo_set_flag(axis, WM_GIZMO_HIDDEN, true);
      }
      MAN_ITER_AXES_END;
    }

    wmWindow *win = CTX_wm_window(C);
    wmOperator *op = NULL;
    for (int i = 0; i < widget->op_data_len; i++) {
      wmGizmoOpElem *gzop = WM_gizmo_operator_get(widget, i);
      op = WM_operator_find_modal_by_type(win, gzop->type);
      if (op != NULL) {
        break;
      }
    }

    if (op != NULL) {
      float twmat[4][4];
      float scale_buf[3];
      float *scale = NULL;
      bool update = false;
      copy_m4_m4(twmat, rv3d->twmat);

      if (axis_type == MAN_AXES_SCALE) {
        scale = scale_buf;
        transform_final_value_get(op->customdata, scale, 3);
        update = true;
      }
      else if (axis_type == MAN_AXES_ROTATE) {
        transform_final_value_get(op->customdata, &ggd->rotation, 1);
        if (widget != ggd->gizmos[MAN_AXIS_ROT_C]) {
          ggd->rotation *= -1;
        }
        RNA_float_set(
            widget->ptr, "incremental_angle", transform_snap_increment_get(op->customdata));
      }
      else if (transform_apply_matrix(op->customdata, twmat)) {
        update = true;
      }

      if (update) {
        gizmo_refresh_from_matrix(gzgroup, twmat, scale);
        ED_region_tag_redraw_editor_overlays(region);
      }
    }
  }

  return OPERATOR_RUNNING_MODAL;
}

static void gizmogroup_init_properties_from_twtype(wmGizmoGroup *gzgroup)
{
  struct {
    wmOperatorType *translate, *rotate, *trackball, *resize;
  } ot_store = {NULL};
  GizmoGroup *ggd = gzgroup->customdata;

  MAN_ITER_AXES_BEGIN (axis, axis_idx) {
    const short axis_type = gizmo_get_axis_type(axis_idx);
    bool constraint_axis[3] = {1, 0, 0};
    PointerRNA *ptr = NULL;

    gizmo_get_axis_constraint(axis_idx, constraint_axis);

    /* custom handler! */
    WM_gizmo_set_fn_custom_modal(axis, gizmo_modal);

    switch (axis_idx) {
      case MAN_AXIS_TRANS_X:
      case MAN_AXIS_TRANS_Y:
      case MAN_AXIS_TRANS_Z:
      case MAN_AXIS_SCALE_X:
      case MAN_AXIS_SCALE_Y:
      case MAN_AXIS_SCALE_Z:
        if (axis_idx >= MAN_AXIS_RANGE_TRANS_START && axis_idx < MAN_AXIS_RANGE_TRANS_END) {
          int draw_options = 0;
          if ((ggd->twtype & (V3D_GIZMO_SHOW_OBJECT_ROTATE | V3D_GIZMO_SHOW_OBJECT_SCALE)) == 0) {
            draw_options |= ED_GIZMO_ARROW_DRAW_FLAG_STEM;
          }
          RNA_enum_set(axis->ptr, "draw_options", draw_options);
        }

        WM_gizmo_set_line_width(axis, GIZMO_AXIS_LINE_WIDTH);
        break;
      case MAN_AXIS_ROT_X:
      case MAN_AXIS_ROT_Y:
      case MAN_AXIS_ROT_Z:
        /* increased line width for better display */
        WM_gizmo_set_line_width(axis, GIZMO_AXIS_LINE_WIDTH + 1.0f);
        WM_gizmo_set_flag(axis, WM_GIZMO_DRAW_VALUE, true);
        break;
      case MAN_AXIS_TRANS_XY:
      case MAN_AXIS_TRANS_YZ:
      case MAN_AXIS_TRANS_ZX:
      case MAN_AXIS_SCALE_XY:
      case MAN_AXIS_SCALE_YZ:
      case MAN_AXIS_SCALE_ZX: {
        const float ofs[3] = {MAN_AXIS_SCALE_PLANE_OFFSET, MAN_AXIS_SCALE_PLANE_OFFSET, 0.0f};
        WM_gizmo_set_scale(axis, MAN_AXIS_SCALE_PLANE_SCALE);
        WM_gizmo_set_matrix_offset_location(axis, ofs);
        WM_gizmo_set_flag(axis, WM_GIZMO_DRAW_OFFSET_SCALE, true);
        break;
      }
      case MAN_AXIS_TRANS_C:
      case MAN_AXIS_ROT_C:
      case MAN_AXIS_SCALE_C:
      case MAN_AXIS_ROT_T:
        WM_gizmo_set_line_width(axis, GIZMO_AXIS_LINE_WIDTH);
        if (axis_idx == MAN_AXIS_ROT_T) {
          WM_gizmo_set_flag(axis, WM_GIZMO_DRAW_HOVER, true);
        }
        else if (axis_idx == MAN_AXIS_ROT_C) {
          WM_gizmo_set_flag(axis, WM_GIZMO_DRAW_VALUE, true);
          WM_gizmo_set_scale(axis, 1.2f);
        }
        else if (axis_idx == MAN_AXIS_SCALE_C) {
          WM_gizmo_set_scale(axis, 1.2f);
        }
        else {
          WM_gizmo_set_scale(axis, 0.2f);
        }
        break;
    }

    switch (axis_type) {
      case MAN_AXES_TRANSLATE:
        if (ot_store.translate == NULL) {
          ot_store.translate = WM_operatortype_find("TRANSFORM_OT_translate", true);
        }
        ptr = WM_gizmo_operator_set(axis, 0, ot_store.translate, NULL);
        break;
      case MAN_AXES_ROTATE: {
        wmOperatorType *ot_rotate;
        if (axis_idx == MAN_AXIS_ROT_T) {
          if (ot_store.trackball == NULL) {
            ot_store.trackball = WM_operatortype_find("TRANSFORM_OT_trackball", true);
          }
          ot_rotate = ot_store.trackball;
        }
        else {
          if (ot_store.rotate == NULL) {
            ot_store.rotate = WM_operatortype_find("TRANSFORM_OT_rotate", true);
          }
          ot_rotate = ot_store.rotate;
        }
        ptr = WM_gizmo_operator_set(axis, 0, ot_rotate, NULL);
        break;
      }
      case MAN_AXES_SCALE: {
        if (ot_store.resize == NULL) {
          ot_store.resize = WM_operatortype_find("TRANSFORM_OT_resize", true);
        }
        ptr = WM_gizmo_operator_set(axis, 0, ot_store.resize, NULL);
        break;
      }
    }

    if (ptr) {
      PropertyRNA *prop;
      if (ELEM(true, UNPACK3(constraint_axis))) {
        if ((prop = RNA_struct_find_property(ptr, "constraint_axis"))) {
          RNA_property_boolean_set_array(ptr, prop, constraint_axis);
        }
      }

      RNA_boolean_set(ptr, "release_confirm", 1);
    }
  }
  MAN_ITER_AXES_END;
}

static void WIDGETGROUP_gizmo_setup(const bContext *C, wmGizmoGroup *gzgroup)
{
  GizmoGroup *ggd = gizmogroup_init(gzgroup);

  gzgroup->customdata = ggd;

  {
    ScrArea *area = CTX_wm_area(C);
    const bToolRef *tref = area->runtime.tool;

    ggd->twtype = 0;
    if (tref && STREQ(tref->idname, "builtin.move")) {
      ggd->twtype |= V3D_GIZMO_SHOW_OBJECT_TRANSLATE;
    }
    else if (tref && STREQ(tref->idname, "builtin.rotate")) {
      ggd->twtype |= V3D_GIZMO_SHOW_OBJECT_ROTATE;
    }
    else if (tref && STREQ(tref->idname, "builtin.scale")) {
      ggd->twtype |= V3D_GIZMO_SHOW_OBJECT_SCALE;
    }
    else if (tref && STREQ(tref->idname, "builtin.transform")) {
      ggd->twtype = V3D_GIZMO_SHOW_OBJECT_TRANSLATE | V3D_GIZMO_SHOW_OBJECT_ROTATE |
                    V3D_GIZMO_SHOW_OBJECT_SCALE;
    }
    else {
      /* This is also correct logic for 'builtin.transform', no special check needed. */
      /* Setup all gizmos, they can be toggled via 'ToolSettings.gizmo_flag' */
      ggd->twtype = V3D_GIZMO_SHOW_OBJECT_TRANSLATE | V3D_GIZMO_SHOW_OBJECT_ROTATE |
                    V3D_GIZMO_SHOW_OBJECT_SCALE;
      ggd->use_twtype_refresh = true;
    }
    BLI_assert(ggd->twtype != 0);
    ggd->twtype_init = ggd->twtype;
  }

  /* *** set properties for axes *** */
  gizmogroup_init_properties_from_twtype(gzgroup);
}

/**
 * Set properties for axes.
 *
 * \param twmat: The transform matrix (typically #RegionView3D.twmat).
 * \param scale: Optional scale, to show scale while modally dragging the scale handles.
 */
static void gizmo_refresh_from_matrix(wmGizmoGroup *gzgroup,
                                      const float twmat[4][4],
                                      const float scale[3])
{
  GizmoGroup *ggd = gzgroup->customdata;

  MAN_ITER_AXES_BEGIN (axis, axis_idx) {
    const short axis_type = gizmo_get_axis_type(axis_idx);
    const int aidx_norm = gizmo_orientation_axis(axis_idx, NULL);

    WM_gizmo_set_matrix_location(axis, twmat[3]);
    switch (axis_idx) {
      case MAN_AXIS_TRANS_X:
      case MAN_AXIS_TRANS_Y:
      case MAN_AXIS_TRANS_Z:
      case MAN_AXIS_SCALE_X:
      case MAN_AXIS_SCALE_Y:
      case MAN_AXIS_SCALE_Z: {
        float start_co[3] = {0.0f, 0.0f, 0.0f};
        float len;

        gizmo_line_range(ggd->twtype, axis_type, &start_co[2], &len);

        const float *z_axis = twmat[aidx_norm];
        if (axis_type == MAN_AXES_SCALE) {
          /* Scale handles are cubes that don't look right when not aligned with other axes.
           * This is noticeable when the axis is rotated to something besides the global-axis. */
          const int aidx_norm_y = (aidx_norm + 2) % 3;
          const float *y_axis = twmat[aidx_norm_y];
          WM_gizmo_set_matrix_rotation_from_yz_axis(axis, y_axis, z_axis);
        }
        else {
          WM_gizmo_set_matrix_rotation_from_z_axis(axis, z_axis);
        }

        if (axis_idx >= MAN_AXIS_RANGE_TRANS_START && axis_idx < MAN_AXIS_RANGE_TRANS_END) {
          if (ggd->twtype & V3D_GIZMO_SHOW_OBJECT_ROTATE) {
            /* Avoid rotate and translate arrows overlap. */
            start_co[2] += 0.215f;
          }
        }

        if (scale) {
          if (axis_type == MAN_AXES_SCALE) {
            len = ((start_co[2] + len) * scale[aidx_norm]) - start_co[2];
          }
        }

        RNA_float_set(axis->ptr, "length", len);

        WM_gizmo_set_matrix_offset_location(axis, start_co);

        WM_gizmo_set_flag(axis, WM_GIZMO_DRAW_OFFSET_SCALE, true);

        break;
      }
      case MAN_AXIS_ROT_X:
      case MAN_AXIS_ROT_Y:
      case MAN_AXIS_ROT_Z:
      case MAN_AXIS_ROT_C: {
        if (axis_idx != MAN_AXIS_ROT_C) {
          WM_gizmo_set_matrix_rotation_from_z_axis(axis, twmat[aidx_norm]);
        }

        /* Remove #ED_GIZMO_DIAL_DRAW_FLAG_ANGLE_VALUE. It is used only for modal drawing. */
        PropertyRNA *prop = RNA_struct_find_property(axis->ptr, "draw_options");
        RNA_property_enum_set(axis->ptr,
                              prop,
                              RNA_property_enum_get(axis->ptr, prop) &
                                  ~ED_GIZMO_DIAL_DRAW_FLAG_ANGLE_VALUE);
      } break;
      case MAN_AXIS_TRANS_XY:
      case MAN_AXIS_TRANS_YZ:
      case MAN_AXIS_TRANS_ZX:
      case MAN_AXIS_SCALE_XY:
      case MAN_AXIS_SCALE_YZ:
      case MAN_AXIS_SCALE_ZX: {
        const int aidx_norm_x = (aidx_norm + 1) % 3;
        const int aidx_norm_y = (aidx_norm + 2) % 3;
        const float *y_axis = twmat[aidx_norm_y];
        const float *z_axis = twmat[aidx_norm];
        WM_gizmo_set_matrix_rotation_from_yz_axis(axis, y_axis, z_axis);

        if (axis_type == MAN_AXES_SCALE) {
          float ofs[3] = {MAN_AXIS_SCALE_PLANE_OFFSET, MAN_AXIS_SCALE_PLANE_OFFSET, 0.0f};
          if (scale) {
            ofs[0] *= scale[aidx_norm_x];
            ofs[1] *= scale[aidx_norm_y];
          }
          WM_gizmo_set_matrix_offset_location(axis, ofs);
        }
        break;
      }
    }
  }
  MAN_ITER_AXES_END;

  /* Ensure rotate disks don't overlap scale arrows, especially in ortho view. */
  float rotate_select_bias = 0.0f;
  if ((ggd->twtype & V3D_GIZMO_SHOW_OBJECT_SCALE) && ggd->twtype & V3D_GIZMO_SHOW_OBJECT_ROTATE) {
    rotate_select_bias = -2.0f;
  }
  for (int i = MAN_AXIS_RANGE_ROT_START; i < MAN_AXIS_RANGE_ROT_END; i++) {
    ggd->gizmos[i]->select_bias = rotate_select_bias;
  }
}

static void WIDGETGROUP_gizmo_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  ARegion *region = CTX_wm_region(C);

  {
    wmGizmo *gz = WM_gizmomap_get_modal(region->gizmo_map);
    if (gz && gz->parent_gzgroup == gzgroup) {
      return;
    }
  }

  GizmoGroup *ggd = gzgroup->customdata;
  Scene *scene = CTX_data_scene(C);
  ScrArea *area = CTX_wm_area(C);
  View3D *v3d = area->spacedata.first;
  RegionView3D *rv3d = region->regiondata;
  struct TransformBounds tbounds;

  if (ggd->use_twtype_refresh) {
    ggd->twtype = v3d->gizmo_show_object & ggd->twtype_init;
    if (ggd->twtype != ggd->twtype_prev) {
      ggd->twtype_prev = ggd->twtype;
      gizmogroup_init_properties_from_twtype(gzgroup);
    }
  }

  const int orient_index = BKE_scene_orientation_get_index_from_flag(scene, ggd->twtype_init);

  /* skip, we don't draw anything anyway */
  if ((ggd->all_hidden = (ED_transform_calc_gizmo_stats(C,
                                                        &(struct TransformCalcParams){
                                                            .use_only_center = true,
                                                            .orientation_index = orient_index + 1,
                                                        },
                                                        &tbounds) == 0))) {
    return;
  }

  gizmo_prepare_mat(C, rv3d, &tbounds);

  gizmo_refresh_from_matrix(gzgroup, rv3d->twmat, NULL);
}

static void WIDGETGROUP_gizmo_message_subscribe(const bContext *C,
                                                wmGizmoGroup *gzgroup,
                                                struct wmMsgBus *mbus)
{
  Scene *scene = CTX_data_scene(C);
  bScreen *screen = CTX_wm_screen(C);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  gizmo_xform_message_subscribe(
      gzgroup, mbus, scene, screen, area, region, VIEW3D_GGT_xform_gizmo);
}

static void WIDGETGROUP_gizmo_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  GizmoGroup *ggd = gzgroup->customdata;
  // ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  // View3D *v3d = area->spacedata.first;
  RegionView3D *rv3d = region->regiondata;
  float viewinv_m3[3][3];
  copy_m3_m4(viewinv_m3, rv3d->viewinv);
  float idot[3];

  /* Re-calculate hidden unless modal. */
  bool is_modal = false;
  {
    wmGizmo *gz = WM_gizmomap_get_modal(region->gizmo_map);
    if (gz && gz->parent_gzgroup == gzgroup) {
      is_modal = true;
    }
  }

  /* when looking through a selected camera, the gizmo can be at the
   * exact same position as the view, skip so we don't break selection */
  if (ggd->all_hidden || fabsf(ED_view3d_pixel_size(rv3d, rv3d->twmat[3])) < 5e-7f) {
    MAN_ITER_AXES_BEGIN (axis, axis_idx) {
      if (!is_modal) {
        WM_gizmo_set_flag(axis, WM_GIZMO_HIDDEN, true);
      }
    }
    MAN_ITER_AXES_END;
    return;
  }
  gizmo_get_idot(rv3d, idot);

  /* *** set properties for axes *** */
  MAN_ITER_AXES_BEGIN (axis, axis_idx) {
    const short axis_type = gizmo_get_axis_type(axis_idx);
    /* XXX maybe unset _HIDDEN flag on redraw? */
    if (gizmo_is_axis_visible(rv3d, ggd->twtype, idot, axis_type, axis_idx)) {
      if (!is_modal) {
        WM_gizmo_set_flag(axis, WM_GIZMO_HIDDEN, false);
      }
    }
    else {
      if (!is_modal) {
        WM_gizmo_set_flag(axis, WM_GIZMO_HIDDEN, true);
      }
      continue;
    }

    float color[4], color_hi[4];
    gizmo_get_axis_color(axis_idx, idot, color, color_hi);
    WM_gizmo_set_color(axis, color);
    WM_gizmo_set_color_highlight(axis, color_hi);

    if (!is_modal) {
      switch (axis_idx) {
        case MAN_AXIS_TRANS_C:
        case MAN_AXIS_ROT_C:
        case MAN_AXIS_SCALE_C:
        case MAN_AXIS_ROT_T:
          WM_gizmo_set_matrix_rotation_from_z_axis(axis, rv3d->viewinv[2]);
          break;
      }
    }
  }
  MAN_ITER_AXES_END;

  /* Refresh handled above when using view orientation. */
  if (!equals_m3m3(viewinv_m3, ggd->prev.viewinv_m3)) {
    {
      Scene *scene = CTX_data_scene(C);
      const TransformOrientationSlot *orient_slot = BKE_scene_orientation_slot_get_from_flag(
          scene, ggd->twtype_init);
      switch (orient_slot->type) {
        case V3D_ORIENT_VIEW: {
          WIDGETGROUP_gizmo_refresh(C, gzgroup);
          break;
        }
      }
    }
    copy_m3_m4(ggd->prev.viewinv_m3, rv3d->viewinv);
  }
}

static void WIDGETGROUP_gizmo_invoke_prepare(const bContext *C,
                                             wmGizmoGroup *gzgroup,
                                             wmGizmo *gz,
                                             const wmEvent *event)
{

  GizmoGroup *ggd = gzgroup->customdata;

  /* Support gizmo specific orientation. */
  if (gz != ggd->gizmos[MAN_AXIS_ROT_T]) {
    Scene *scene = CTX_data_scene(C);
    wmGizmoOpElem *gzop = WM_gizmo_operator_get(gz, 0);
    PointerRNA *ptr = &gzop->ptr;
    PropertyRNA *prop_orient_type = RNA_struct_find_property(ptr, "orient_type");
    const TransformOrientationSlot *orient_slot = BKE_scene_orientation_slot_get_from_flag(
        scene, ggd->twtype_init);
    if ((gz == ggd->gizmos[MAN_AXIS_ROT_C]) ||
        (orient_slot == &scene->orientation_slots[SCE_ORIENT_DEFAULT])) {
      /* #MAN_AXIS_ROT_C always uses the #V3D_ORIENT_VIEW orientation,
       * optionally we could set this orientation instead of unset the property. */
      RNA_property_unset(ptr, prop_orient_type);
    }
    else {
      /* TODO: APIfunction */
      int index = BKE_scene_orientation_slot_get_index(orient_slot);
      RNA_property_enum_set(ptr, prop_orient_type, index);
    }
  }

  /* Support shift click to constrain axis. */
  const int axis_idx = BLI_array_findindex(ggd->gizmos, ARRAY_SIZE(ggd->gizmos), &gz);
  int axis = -1;
  switch (axis_idx) {
    case MAN_AXIS_TRANS_X:
    case MAN_AXIS_TRANS_Y:
    case MAN_AXIS_TRANS_Z:
      axis = axis_idx - MAN_AXIS_TRANS_X;
      break;
    case MAN_AXIS_SCALE_X:
    case MAN_AXIS_SCALE_Y:
    case MAN_AXIS_SCALE_Z:
      axis = axis_idx - MAN_AXIS_SCALE_X;
      break;
  }

  if (axis != -1) {
    /* Swap single axis for two-axis constraint. */
    const bool flip = (event->modifier & KM_SHIFT) != 0;
    BLI_assert(axis_idx != -1);
    const short axis_type = gizmo_get_axis_type(axis_idx);
    if (axis_type != MAN_AXES_ROTATE) {
      wmGizmoOpElem *gzop = WM_gizmo_operator_get(gz, 0);
      PointerRNA *ptr = &gzop->ptr;
      PropertyRNA *prop_constraint_axis = RNA_struct_find_property(ptr, "constraint_axis");
      if (prop_constraint_axis) {
        bool constraint[3] = {false};
        constraint[axis] = true;
        if (flip) {
          for (int i = 0; i < ARRAY_SIZE(constraint); i++) {
            constraint[i] = !constraint[i];
          }
        }
        RNA_property_boolean_set_array(ptr, prop_constraint_axis, constraint);
      }
    }
  }
  else if (ELEM(axis_idx, MAN_AXIS_ROT_X, MAN_AXIS_ROT_Y, MAN_AXIS_ROT_Z, MAN_AXIS_ROT_C)) {
    gizmo_3d_dial_matrixbasis_calc(CTX_wm_region(C),
                                   gz->matrix_basis[2],
                                   gz->matrix_basis[3],
                                   (float[2]){UNPACK2(event->mval)},
                                   gz->matrix_basis);
    PropertyRNA *prop = RNA_struct_find_property(gz->ptr, "draw_options");
    RNA_property_enum_set(
        gz->ptr, prop, RNA_property_enum_get(gz->ptr, prop) | ED_GIZMO_DIAL_DRAW_FLAG_ANGLE_VALUE);
    RNA_float_set(gz->ptr, "incremental_angle", 0.0f);
  }
}

static bool WIDGETGROUP_gizmo_poll_generic(View3D *v3d)
{
  if (v3d->gizmo_flag & V3D_GIZMO_HIDE) {
    return false;
  }
  if (G.moving & (G_TRANSFORM_OBJ | G_TRANSFORM_EDIT)) {
    return false;
  }
  return true;
}

static bool WIDGETGROUP_gizmo_poll_context(const struct bContext *C,
                                           struct wmGizmoGroupType *UNUSED(gzgt))
{
  ScrArea *area = CTX_wm_area(C);
  View3D *v3d = area->spacedata.first;
  if (!WIDGETGROUP_gizmo_poll_generic(v3d)) {
    return false;
  }

  const bToolRef *tref = area->runtime.tool;
  if (v3d->gizmo_flag & V3D_GIZMO_HIDE_CONTEXT) {
    return false;
  }
  if ((v3d->gizmo_show_object & (V3D_GIZMO_SHOW_OBJECT_TRANSLATE | V3D_GIZMO_SHOW_OBJECT_ROTATE |
                                 V3D_GIZMO_SHOW_OBJECT_SCALE)) == 0) {
    return false;
  }

  /* Don't show if the tool has a gizmo. */
  if (tref && tref->runtime && tref->runtime->gizmo_group[0]) {
    return false;
  }
  return true;
}

static bool WIDGETGROUP_gizmo_poll_tool(const struct bContext *C, struct wmGizmoGroupType *gzgt)
{
  if (!ED_gizmo_poll_or_unlink_delayed_from_tool(C, gzgt)) {
    return false;
  }

  ScrArea *area = CTX_wm_area(C);
  View3D *v3d = area->spacedata.first;
  if (!WIDGETGROUP_gizmo_poll_generic(v3d)) {
    return false;
  }

  if (v3d->gizmo_flag & V3D_GIZMO_HIDE_TOOL) {
    return false;
  }

  return true;
}

/* Expose as multiple gizmos so tools use one, persistent context another.
 * Needed because they use different options which isn't so simple to dynamically update. */

void VIEW3D_GGT_xform_gizmo(wmGizmoGroupType *gzgt)
{
  gzgt->name = "3D View: Transform Gizmo";
  gzgt->idname = "VIEW3D_GGT_xform_gizmo";

  gzgt->flag = WM_GIZMOGROUPTYPE_3D | WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP |
               WM_GIZMOGROUPTYPE_DELAY_REFRESH_FOR_TWEAK;

  gzgt->gzmap_params.spaceid = SPACE_VIEW3D;
  gzgt->gzmap_params.regionid = RGN_TYPE_WINDOW;

  gzgt->poll = WIDGETGROUP_gizmo_poll_tool;
  gzgt->setup = WIDGETGROUP_gizmo_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = WIDGETGROUP_gizmo_refresh;
  gzgt->message_subscribe = WIDGETGROUP_gizmo_message_subscribe;
  gzgt->draw_prepare = WIDGETGROUP_gizmo_draw_prepare;
  gzgt->invoke_prepare = WIDGETGROUP_gizmo_invoke_prepare;

  static const EnumPropertyItem rna_enum_gizmo_items[] = {
      {V3D_GIZMO_SHOW_OBJECT_TRANSLATE, "TRANSLATE", 0, "Move", ""},
      {V3D_GIZMO_SHOW_OBJECT_ROTATE, "ROTATE", 0, "Rotate", ""},
      {V3D_GIZMO_SHOW_OBJECT_SCALE, "SCALE", 0, "Scale", ""},
      {0, "NONE", 0, "None", ""},
      {0, NULL, 0, NULL, NULL},
  };
  RNA_def_enum(gzgt->srna,
               "drag_action",
               rna_enum_gizmo_items,
               V3D_GIZMO_SHOW_OBJECT_TRANSLATE,
               "Drag Action",
               "");
}

void VIEW3D_GGT_xform_gizmo_context(wmGizmoGroupType *gzgt)
{
  gzgt->name = "3D View: Transform Gizmo Context";
  gzgt->idname = "VIEW3D_GGT_xform_gizmo_context";

  gzgt->flag = WM_GIZMOGROUPTYPE_3D | WM_GIZMOGROUPTYPE_PERSISTENT |
               WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP | WM_GIZMOGROUPTYPE_DELAY_REFRESH_FOR_TWEAK;

  gzgt->poll = WIDGETGROUP_gizmo_poll_context;
  gzgt->setup = WIDGETGROUP_gizmo_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = WIDGETGROUP_gizmo_refresh;
  gzgt->message_subscribe = WIDGETGROUP_gizmo_message_subscribe;
  gzgt->draw_prepare = WIDGETGROUP_gizmo_draw_prepare;
  gzgt->invoke_prepare = WIDGETGROUP_gizmo_invoke_prepare;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scale Cage Gizmo
 * \{ */

struct XFormCageWidgetGroup {
  wmGizmo *gizmo;
  /* Only for view orientation. */
  struct {
    float viewinv_m3[3][3];
  } prev;
};

static bool WIDGETGROUP_xform_cage_poll(const bContext *C, wmGizmoGroupType *gzgt)
{
  if (!ED_gizmo_poll_or_unlink_delayed_from_tool(C, gzgt)) {
    return false;
  }
  View3D *v3d = CTX_wm_view3d(C);
  if (v3d->gizmo_flag & (V3D_GIZMO_HIDE | V3D_GIZMO_HIDE_TOOL)) {
    return false;
  }
  if (G.moving & (G_TRANSFORM_OBJ | G_TRANSFORM_EDIT)) {
    return false;
  }
  return true;
}

static void WIDGETGROUP_xform_cage_setup(const bContext *UNUSED(C), wmGizmoGroup *gzgroup)
{
  struct XFormCageWidgetGroup *xgzgroup = MEM_mallocN(sizeof(struct XFormCageWidgetGroup),
                                                      __func__);
  const wmGizmoType *gzt_cage = WM_gizmotype_find("GIZMO_GT_cage_3d", true);
  xgzgroup->gizmo = WM_gizmo_new_ptr(gzt_cage, gzgroup, NULL);
  wmGizmo *gz = xgzgroup->gizmo;

  RNA_enum_set(gz->ptr,
               "transform",
               ED_GIZMO_CAGE2D_XFORM_FLAG_SCALE | ED_GIZMO_CAGE2D_XFORM_FLAG_TRANSLATE);

  gz->color[0] = 1;
  gz->color_hi[0] = 1;

  gzgroup->customdata = xgzgroup;

  {
    wmOperatorType *ot_resize = WM_operatortype_find("TRANSFORM_OT_resize", true);
    PointerRNA *ptr;

    /* assign operator */
    PropertyRNA *prop_release_confirm = NULL;
    PropertyRNA *prop_constraint_axis = NULL;

    int i = ED_GIZMO_CAGE3D_PART_SCALE_MIN_X_MIN_Y_MIN_Z;
    for (int x = 0; x < 3; x++) {
      for (int y = 0; y < 3; y++) {
        for (int z = 0; z < 3; z++) {
          const bool constraint[3] = {x != 1, y != 1, z != 1};
          ptr = WM_gizmo_operator_set(gz, i, ot_resize, NULL);
          if (prop_release_confirm == NULL) {
            prop_release_confirm = RNA_struct_find_property(ptr, "release_confirm");
            prop_constraint_axis = RNA_struct_find_property(ptr, "constraint_axis");
          }
          RNA_property_boolean_set(ptr, prop_release_confirm, true);
          RNA_property_boolean_set_array(ptr, prop_constraint_axis, constraint);
          i++;
        }
      }
    }
  }
}

static void WIDGETGROUP_xform_cage_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = region->regiondata;
  Scene *scene = CTX_data_scene(C);

  struct XFormCageWidgetGroup *xgzgroup = gzgroup->customdata;
  wmGizmo *gz = xgzgroup->gizmo;

  struct TransformBounds tbounds;

  const int orient_index = BKE_scene_orientation_get_index_from_flag(scene, SCE_ORIENT_SCALE);

  if ((ED_transform_calc_gizmo_stats(C,
                                     &(struct TransformCalcParams){
                                         .use_local_axis = true,
                                         .orientation_index = orient_index + 1,
                                     },
                                     &tbounds) == 0) ||
      equals_v3v3(rv3d->tw_axis_min, rv3d->tw_axis_max)) {
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  }
  else {
    if (tbounds.use_matrix_space) {
      copy_m4_m4(gz->matrix_space, tbounds.matrix_space);
    }
    else {
      unit_m4(gz->matrix_space);
    }

    gizmo_prepare_mat(C, rv3d, &tbounds);

    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
    WM_gizmo_set_flag(gz, WM_GIZMO_MOVE_CURSOR, true);

    float dims[3];
    sub_v3_v3v3(dims, rv3d->tw_axis_max, rv3d->tw_axis_min);
    RNA_float_set_array(gz->ptr, "dimensions", dims);
    mul_v3_fl(dims, 0.5f);

    copy_m4_m3(gz->matrix_offset, rv3d->tw_axis_matrix);
    mid_v3_v3v3(gz->matrix_offset[3], rv3d->tw_axis_max, rv3d->tw_axis_min);
    mul_m3_v3(rv3d->tw_axis_matrix, gz->matrix_offset[3]);

    float matrix_offset_global[4][4];
    mul_m4_m4m4(matrix_offset_global, gz->matrix_space, gz->matrix_offset);

    PropertyRNA *prop_center_override = NULL;
    float center[3];
    float center_global[3];
    int i = ED_GIZMO_CAGE3D_PART_SCALE_MIN_X_MIN_Y_MIN_Z;
    for (int x = 0; x < 3; x++) {
      center[0] = (float)(1 - x) * dims[0];
      for (int y = 0; y < 3; y++) {
        center[1] = (float)(1 - y) * dims[1];
        for (int z = 0; z < 3; z++) {
          center[2] = (float)(1 - z) * dims[2];
          struct wmGizmoOpElem *gzop = WM_gizmo_operator_get(gz, i);
          if (prop_center_override == NULL) {
            prop_center_override = RNA_struct_find_property(&gzop->ptr, "center_override");
          }
          mul_v3_m4v3(center_global, matrix_offset_global, center);
          RNA_property_float_set_array(&gzop->ptr, prop_center_override, center_global);
          i++;
        }
      }
    }
  }

  /* Needed to test view orientation changes. */
  copy_m3_m4(xgzgroup->prev.viewinv_m3, rv3d->viewinv);
}

static void WIDGETGROUP_xform_cage_message_subscribe(const bContext *C,
                                                     wmGizmoGroup *gzgroup,
                                                     struct wmMsgBus *mbus)
{
  Scene *scene = CTX_data_scene(C);
  bScreen *screen = CTX_wm_screen(C);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  gizmo_xform_message_subscribe(gzgroup, mbus, scene, screen, area, region, VIEW3D_GGT_xform_cage);
}

static void WIDGETGROUP_xform_cage_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  struct XFormCageWidgetGroup *xgzgroup = gzgroup->customdata;

  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  {
    Scene *scene = CTX_data_scene(C);
    const TransformOrientationSlot *orient_slot = BKE_scene_orientation_slot_get(scene,
                                                                                 SCE_ORIENT_SCALE);
    switch (orient_slot->type) {
      case V3D_ORIENT_VIEW: {
        float viewinv_m3[3][3];
        copy_m3_m4(viewinv_m3, rv3d->viewinv);
        if (!equals_m3m3(viewinv_m3, xgzgroup->prev.viewinv_m3)) {
          /* Take care calling refresh from draw_prepare,
           * this should be OK because it's only adjusting the cage orientation. */
          WIDGETGROUP_xform_cage_refresh(C, gzgroup);
        }
        break;
      }
    }
  }
}

void VIEW3D_GGT_xform_cage(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Transform Cage";
  gzgt->idname = "VIEW3D_GGT_xform_cage";

  gzgt->flag |= WM_GIZMOGROUPTYPE_3D | WM_GIZMOGROUPTYPE_DRAW_MODAL_EXCLUDE |
                WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP | WM_GIZMOGROUPTYPE_DELAY_REFRESH_FOR_TWEAK;

  gzgt->gzmap_params.spaceid = SPACE_VIEW3D;
  gzgt->gzmap_params.regionid = RGN_TYPE_WINDOW;

  gzgt->poll = WIDGETGROUP_xform_cage_poll;
  gzgt->setup = WIDGETGROUP_xform_cage_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = WIDGETGROUP_xform_cage_refresh;
  gzgt->message_subscribe = WIDGETGROUP_xform_cage_message_subscribe;
  gzgt->draw_prepare = WIDGETGROUP_xform_cage_draw_prepare;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform Shear Gizmo
 * \{ */

struct XFormShearWidgetGroup {
  wmGizmo *gizmo[3][2];
  /** View aligned gizmos. */
  wmGizmo *gizmo_view[4];

  /* Only for view orientation. */
  struct {
    float viewinv_m3[3][3];
  } prev;
};

static bool WIDGETGROUP_xform_shear_poll(const bContext *C, wmGizmoGroupType *gzgt)
{
  if (!ED_gizmo_poll_or_unlink_delayed_from_tool(C, gzgt)) {
    return false;
  }
  View3D *v3d = CTX_wm_view3d(C);
  if (v3d->gizmo_flag & (V3D_GIZMO_HIDE | V3D_GIZMO_HIDE_TOOL)) {
    return false;
  }
  return true;
}

static void WIDGETGROUP_xform_shear_setup(const bContext *UNUSED(C), wmGizmoGroup *gzgroup)
{
  struct XFormShearWidgetGroup *xgzgroup = MEM_mallocN(sizeof(struct XFormShearWidgetGroup),
                                                       __func__);
  const wmGizmoType *gzt_arrow = WM_gizmotype_find("GIZMO_GT_arrow_3d", true);
  wmOperatorType *ot_shear = WM_operatortype_find("TRANSFORM_OT_shear", true);

  float axis_color[3][3];
  for (int i = 0; i < 3; i++) {
    UI_GetThemeColor3fv(TH_AXIS_X + i, axis_color[i]);
  }

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 2; j++) {
      wmGizmo *gz = WM_gizmo_new_ptr(gzt_arrow, gzgroup, NULL);
      RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_BOX);
      const int i_ortho_a = (i + j + 1) % 3;
      const int i_ortho_b = (i + (1 - j) + 1) % 3;
      interp_v3_v3v3(gz->color, axis_color[i_ortho_a], axis_color[i_ortho_b], 0.75f);
      gz->color[3] = 0.5f;
      PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot_shear, NULL);
      RNA_boolean_set(ptr, "release_confirm", 1);
      xgzgroup->gizmo[i][j] = gz;
    }
  }

  for (int i = 0; i < 4; i++) {
    wmGizmo *gz = WM_gizmo_new_ptr(gzt_arrow, gzgroup, NULL);
    RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_BOX);
    RNA_enum_set(gz->ptr, "draw_options", 0); /* No stem. */
    copy_v3_fl(gz->color, 1.0f);
    gz->color[3] = 0.5f;
    WM_gizmo_set_flag(gz, WM_GIZMO_DRAW_OFFSET_SCALE, true);
    PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot_shear, NULL);
    RNA_boolean_set(ptr, "release_confirm", 1);
    xgzgroup->gizmo_view[i] = gz;

    /* Unlike the other gizmos, this never changes so can be set on setup. */
    wmGizmoOpElem *gzop = WM_gizmo_operator_get(gz, 0);
    RNA_enum_set(&gzop->ptr, "orient_type", V3D_ORIENT_VIEW);

    RNA_enum_set(&gzop->ptr, "orient_axis", 2);
    RNA_enum_set(&gzop->ptr, "orient_axis_ortho", ((i % 2) ? 0 : 1));
  }

  gzgroup->customdata = xgzgroup;
}

static void WIDGETGROUP_xform_shear_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = region->regiondata;

  struct XFormShearWidgetGroup *xgzgroup = gzgroup->customdata;
  struct TransformBounds tbounds;

  /* Needed to test view orientation changes. */
  copy_m3_m4(xgzgroup->prev.viewinv_m3, rv3d->viewinv);

  TransformOrientationSlot *orient_slot = BKE_scene_orientation_slot_get_from_flag(
      scene, SCE_ORIENT_ROTATE);
  const int orient_index = BKE_scene_orientation_slot_get_index(orient_slot);

  if (ED_transform_calc_gizmo_stats(C,
                                    &(struct TransformCalcParams){
                                        .use_local_axis = false,
                                        .orientation_index = orient_index + 1,
                                    },
                                    &tbounds) == 0) {
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 2; j++) {
        wmGizmo *gz = xgzgroup->gizmo[i][j];
        WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
      }
    }

    for (int i = 0; i < 4; i++) {
      wmGizmo *gz = xgzgroup->gizmo_view[i];
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
    }
  }
  else {
    gizmo_prepare_mat(C, rv3d, &tbounds);
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 2; j++) {
        wmGizmo *gz = xgzgroup->gizmo[i][j];
        WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
        WM_gizmo_set_flag(gz, WM_GIZMO_MOVE_CURSOR, true);

        wmGizmoOpElem *gzop = WM_gizmo_operator_get(gz, 0);
        const int i_ortho_a = (i + j + 1) % 3;
        const int i_ortho_b = (i + (1 - j) + 1) % 3;
        WM_gizmo_set_matrix_rotation_from_yz_axis(gz, rv3d->twmat[i_ortho_a], rv3d->twmat[i]);
        WM_gizmo_set_matrix_location(gz, rv3d->twmat[3]);

        RNA_float_set_array(&gzop->ptr, "orient_matrix", &tbounds.axis[0][0]);
        RNA_enum_set(&gzop->ptr, "orient_type", orient_slot->type);

        RNA_enum_set(&gzop->ptr, "orient_axis", i_ortho_b);
        RNA_enum_set(&gzop->ptr, "orient_axis_ortho", i_ortho_a);

        mul_v3_fl(gz->matrix_basis[0], 0.5f);
        mul_v3_fl(gz->matrix_basis[1], 6.0f);
      }
    }

    for (int i = 0; i < 4; i++) {
      wmGizmo *gz = xgzgroup->gizmo_view[i];
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
    }
  }
}

static void WIDGETGROUP_xform_shear_message_subscribe(const bContext *C,
                                                      wmGizmoGroup *gzgroup,
                                                      struct wmMsgBus *mbus)
{
  Scene *scene = CTX_data_scene(C);
  bScreen *screen = CTX_wm_screen(C);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  gizmo_xform_message_subscribe(
      gzgroup, mbus, scene, screen, area, region, VIEW3D_GGT_xform_shear);
}

static void WIDGETGROUP_xform_shear_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  struct XFormShearWidgetGroup *xgzgroup = gzgroup->customdata;
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  {
    Scene *scene = CTX_data_scene(C);
    /* Shear is like rotate, use the rotate setting. */
    const TransformOrientationSlot *orient_slot = BKE_scene_orientation_slot_get(
        scene, SCE_ORIENT_ROTATE);
    switch (orient_slot->type) {
      case V3D_ORIENT_VIEW: {
        float viewinv_m3[3][3];
        copy_m3_m4(viewinv_m3, rv3d->viewinv);
        if (!equals_m3m3(viewinv_m3, xgzgroup->prev.viewinv_m3)) {
          /* Take care calling refresh from draw_prepare,
           * this should be OK because it's only adjusting the cage orientation. */
          WIDGETGROUP_xform_shear_refresh(C, gzgroup);
        }
        break;
      }
    }
  }

  for (int i = 0; i < 4; i++) {
    const float outer_thin = 0.3f;
    const float outer_offset = 1.0f / 0.3f;
    wmGizmo *gz = xgzgroup->gizmo_view[i];
    WM_gizmo_set_matrix_rotation_from_yz_axis(
        gz, rv3d->viewinv[(i + 1) % 2], rv3d->viewinv[i % 2]);
    if (i >= 2) {
      negate_v3(gz->matrix_basis[1]);
      negate_v3(gz->matrix_basis[2]);
    }

    /* No need for depth with view aligned gizmos. */
    mul_v3_fl(gz->matrix_basis[0], 0.0f);
    mul_v3_fl(gz->matrix_basis[1], 20.0f + ((1.0f / outer_thin) * 1.8f));
    mul_v3_fl(gz->matrix_basis[2], outer_thin);
    WM_gizmo_set_matrix_location(gz, rv3d->twmat[3]);
    gz->matrix_offset[3][2] = outer_offset;
  }

  /* Basic ordering for drawing only. */
  {
    LISTBASE_FOREACH (wmGizmo *, gz, &gzgroup->gizmos) {
      /* Since we have two pairs of each axis,
       * bias the values so gizmos that are orthogonal to the view get priority.
       * This means we never default to shearing along
       * the view axis in the case of an overlap. */
      float axis_order[3], axis_bias[3];
      copy_v3_v3(axis_order, gz->matrix_basis[2]);
      copy_v3_v3(axis_bias, gz->matrix_basis[1]);
      if (dot_v3v3(axis_bias, rv3d->viewinv[2]) < 0.0f) {
        negate_v3(axis_bias);
      }
      madd_v3_v3fl(axis_order, axis_bias, 0.01f);
      gz->temp.f = dot_v3v3(rv3d->viewinv[2], axis_order);
    }
    BLI_listbase_sort(&gzgroup->gizmos, WM_gizmo_cmp_temp_fl_reverse);
  }
}

void VIEW3D_GGT_xform_shear(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Transform Shear";
  gzgt->idname = "VIEW3D_GGT_xform_shear";

  gzgt->flag |= WM_GIZMOGROUPTYPE_3D | WM_GIZMOGROUPTYPE_DRAW_MODAL_EXCLUDE |
                WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP | WM_GIZMOGROUPTYPE_DELAY_REFRESH_FOR_TWEAK;

  gzgt->gzmap_params.spaceid = SPACE_VIEW3D;
  gzgt->gzmap_params.regionid = RGN_TYPE_WINDOW;

  gzgt->poll = WIDGETGROUP_xform_shear_poll;
  gzgt->setup = WIDGETGROUP_xform_shear_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = WIDGETGROUP_xform_shear_refresh;
  gzgt->message_subscribe = WIDGETGROUP_xform_shear_message_subscribe;
  gzgt->draw_prepare = WIDGETGROUP_xform_shear_draw_prepare;
}

/** \} */
