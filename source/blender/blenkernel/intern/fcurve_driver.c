/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation, Joshua Leung. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

#include "DNA_anim_types.h"
#include "DNA_constraint_types.h"
#include "DNA_object_types.h"

#include "BLI_alloca.h"
#include "BLI_expr_pylike_eval.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string_utils.h"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_action.h"
#include "BKE_animsys.h"
#include "BKE_armature.h"
#include "BKE_constraint.h"
#include "BKE_fcurve_driver.h"
#include "BKE_global.h"
#include "BKE_object.h"

#include "RNA_access.h"
#include "RNA_path.h"

#include "atomic_ops.h"

#include "CLG_log.h"

#ifdef WITH_PYTHON
#  include "BPY_extern.h"
#endif

#ifdef WITH_PYTHON
static ThreadMutex python_driver_lock = BLI_MUTEX_INITIALIZER;
#endif

static CLG_LogRef LOG = {"bke.fcurve"};

/* -------------------------------------------------------------------- */
/** \name Driver Variables
 * \{ */

/* TypeInfo for Driver Variables (dvti) */
typedef struct DriverVarTypeInfo {
  /* Evaluation callback. */
  float (*get_value)(ChannelDriver *driver, DriverVar *dvar);

  /* Allocation of target slots. */
  int num_targets;                              /* Number of target slots required. */
  const char *target_names[MAX_DRIVER_TARGETS]; /* UI names that should be given to the slots. */
  short target_flags[MAX_DRIVER_TARGETS]; /* Flags defining the requirements for each slot. */
} DriverVarTypeInfo;

/* Macro to begin definitions */
#define BEGIN_DVAR_TYPEDEF(type) {

/* Macro to end definitions */
#define END_DVAR_TYPEDEF }

/** \} */

/* -------------------------------------------------------------------- */
/** \name Driver Target Utilities
 * \{ */

/**
 * Helper function to obtain a value using RNA from the specified source
 * (for evaluating drivers).
 */
static float dtar_get_prop_val(ChannelDriver *driver, DriverTarget *dtar)
{
  PointerRNA id_ptr, ptr;
  PropertyRNA *prop;
  ID *id;
  int index = -1;
  float value = 0.0f;

  /* Sanity check. */
  if (ELEM(NULL, driver, dtar)) {
    return 0.0f;
  }

  id = dtar->id;

  /* Error check for missing pointer. */
  if (id == NULL) {
    if (G.debug & G_DEBUG) {
      CLOG_ERROR(&LOG, "driver has an invalid target to use (path = %s)", dtar->rna_path);
    }

    driver->flag |= DRIVER_FLAG_INVALID;
    dtar->flag |= DTAR_FLAG_INVALID;
    return 0.0f;
  }

  /* Get RNA-pointer for the ID-block given in target. */
  RNA_id_pointer_create(id, &id_ptr);

  /* Get property to read from, and get value as appropriate. */
  if (!RNA_path_resolve_property_full(&id_ptr, dtar->rna_path, &ptr, &prop, &index)) {
    /* Path couldn't be resolved. */
    if (G.debug & G_DEBUG) {
      CLOG_ERROR(&LOG,
                 "Driver Evaluation Error: cannot resolve target for %s -> %s",
                 id->name,
                 dtar->rna_path);
    }

    driver->flag |= DRIVER_FLAG_INVALID;
    dtar->flag |= DTAR_FLAG_INVALID;
    return 0.0f;
  }

  if (RNA_property_array_check(prop)) {
    /* Array. */
    if (index < 0 || index >= RNA_property_array_length(&ptr, prop)) {
      /* Out of bounds. */
      if (G.debug & G_DEBUG) {
        CLOG_ERROR(&LOG,
                   "Driver Evaluation Error: array index is out of bounds for %s -> %s (%d)",
                   id->name,
                   dtar->rna_path,
                   index);
      }

      driver->flag |= DRIVER_FLAG_INVALID;
      dtar->flag |= DTAR_FLAG_INVALID;
      return 0.0f;
    }

    switch (RNA_property_type(prop)) {
      case PROP_BOOLEAN:
        value = (float)RNA_property_boolean_get_index(&ptr, prop, index);
        break;
      case PROP_INT:
        value = (float)RNA_property_int_get_index(&ptr, prop, index);
        break;
      case PROP_FLOAT:
        value = RNA_property_float_get_index(&ptr, prop, index);
        break;
      default:
        break;
    }
  }
  else {
    /* Not an array. */
    switch (RNA_property_type(prop)) {
      case PROP_BOOLEAN:
        value = (float)RNA_property_boolean_get(&ptr, prop);
        break;
      case PROP_INT:
        value = (float)RNA_property_int_get(&ptr, prop);
        break;
      case PROP_FLOAT:
        value = RNA_property_float_get(&ptr, prop);
        break;
      case PROP_ENUM:
        value = (float)RNA_property_enum_get(&ptr, prop);
        break;
      default:
        break;
    }
  }

  /* If we're still here, we should be ok. */
  dtar->flag &= ~DTAR_FLAG_INVALID;
  return value;
}

bool driver_get_variable_property(ChannelDriver *driver,
                                  DriverTarget *dtar,
                                  PointerRNA *r_ptr,
                                  PropertyRNA **r_prop,
                                  int *r_index)
{
  PointerRNA id_ptr;
  PointerRNA ptr;
  PropertyRNA *prop;
  ID *id;
  int index = -1;

  /* Sanity check. */
  if (ELEM(NULL, driver, dtar)) {
    return false;
  }

  id = dtar->id;

  /* Error check for missing pointer. */
  if (id == NULL) {
    if (G.debug & G_DEBUG) {
      CLOG_ERROR(&LOG, "driver has an invalid target to use (path = %s)", dtar->rna_path);
    }

    driver->flag |= DRIVER_FLAG_INVALID;
    dtar->flag |= DTAR_FLAG_INVALID;
    return false;
  }

  /* Get RNA-pointer for the ID-block given in target. */
  RNA_id_pointer_create(id, &id_ptr);

  /* Get property to read from, and get value as appropriate. */
  if (dtar->rna_path == NULL || dtar->rna_path[0] == '\0') {
    ptr = PointerRNA_NULL;
    prop = NULL; /* OK. */
  }
  else if (RNA_path_resolve_full(&id_ptr, dtar->rna_path, &ptr, &prop, &index)) {
    /* OK. */
  }
  else {
    /* Path couldn't be resolved. */
    if (G.debug & G_DEBUG) {
      CLOG_ERROR(&LOG,
                 "Driver Evaluation Error: cannot resolve target for %s -> %s",
                 id->name,
                 dtar->rna_path);
    }

    ptr = PointerRNA_NULL;
    *r_prop = NULL;
    *r_index = -1;

    driver->flag |= DRIVER_FLAG_INVALID;
    dtar->flag |= DTAR_FLAG_INVALID;
    return false;
  }

  *r_ptr = ptr;
  *r_prop = prop;
  *r_index = index;

  /* If we're still here, we should be ok. */
  dtar->flag &= ~DTAR_FLAG_INVALID;
  return true;
}

static short driver_check_valid_targets(ChannelDriver *driver, DriverVar *dvar)
{
  short valid_targets = 0;

  DRIVER_TARGETS_USED_LOOPER_BEGIN (dvar) {
    Object *ob = (Object *)dtar->id;

    /* Check if this target has valid data. */
    if ((ob == NULL) || (GS(ob->id.name) != ID_OB)) {
      /* Invalid target, so will not have enough targets. */
      driver->flag |= DRIVER_FLAG_INVALID;
      dtar->flag |= DTAR_FLAG_INVALID;
    }
    else {
      /* Target seems to be OK now. */
      dtar->flag &= ~DTAR_FLAG_INVALID;
      valid_targets++;
    }
  }
  DRIVER_TARGETS_LOOPER_END;

  return valid_targets;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Driver Variable Utilities
 * \{ */

/* Evaluate 'single prop' driver variable. */
static float dvar_eval_singleProp(ChannelDriver *driver, DriverVar *dvar)
{
  /* Just evaluate the first target slot. */
  return dtar_get_prop_val(driver, &dvar->targets[0]);
}

/* Evaluate 'rotation difference' driver variable. */
static float dvar_eval_rotDiff(ChannelDriver *driver, DriverVar *dvar)
{
  short valid_targets = driver_check_valid_targets(driver, dvar);

  /* Make sure we have enough valid targets to use - all or nothing for now. */
  if (driver_check_valid_targets(driver, dvar) != 2) {
    if (G.debug & G_DEBUG) {
      CLOG_WARN(&LOG,
                "RotDiff DVar: not enough valid targets (n = %d) (a = %p, b = %p)",
                valid_targets,
                dvar->targets[0].id,
                dvar->targets[1].id);
    }
    return 0.0f;
  }

  float(*mat[2])[4];

  /* NOTE: for now, these are all just world-space. */
  for (int i = 0; i < 2; i++) {
    /* Get pointer to loc values to store in. */
    DriverTarget *dtar = &dvar->targets[i];
    Object *ob = (Object *)dtar->id;
    bPoseChannel *pchan;

    /* After the checks above, the targets should be valid here. */
    BLI_assert((ob != NULL) && (GS(ob->id.name) == ID_OB));

    /* Try to get pose-channel. */
    pchan = BKE_pose_channel_find_name(ob->pose, dtar->pchan_name);

    /* Check if object or bone. */
    if (pchan) {
      /* Bone. */
      mat[i] = pchan->pose_mat;
    }
    else {
      /* Object. */
      mat[i] = ob->obmat;
    }
  }

  float q1[4], q2[4], quat[4], angle;

  /* Use the final posed locations. */
  mat4_to_quat(q1, mat[0]);
  mat4_to_quat(q2, mat[1]);

  invert_qt_normalized(q1);
  mul_qt_qtqt(quat, q1, q2);
  angle = 2.0f * (saacos(quat[0]));
  angle = fabsf(angle);

  return (angle > (float)M_PI) ? (float)((2.0f * (float)M_PI) - angle) : (float)(angle);
}

/**
 * Evaluate 'location difference' driver variable.
 *
 * TODO: this needs to take into account space conversions.
 */
static float dvar_eval_locDiff(ChannelDriver *driver, DriverVar *dvar)
{
  float loc1[3] = {0.0f, 0.0f, 0.0f};
  float loc2[3] = {0.0f, 0.0f, 0.0f};
  short valid_targets = driver_check_valid_targets(driver, dvar);

  /* Make sure we have enough valid targets to use - all or nothing for now. */
  if (valid_targets < dvar->num_targets) {
    if (G.debug & G_DEBUG) {
      CLOG_WARN(&LOG,
                "LocDiff DVar: not enough valid targets (n = %d) (a = %p, b = %p)",
                valid_targets,
                dvar->targets[0].id,
                dvar->targets[1].id);
    }
    return 0.0f;
  }

  /* SECOND PASS: get two location values */
  /* NOTE: for now, these are all just world-space */
  DRIVER_TARGETS_USED_LOOPER_BEGIN (dvar) {
    /* Get pointer to loc values to store in. */
    Object *ob = (Object *)dtar->id;
    bPoseChannel *pchan;
    float tmp_loc[3];

    /* After the checks above, the targets should be valid here. */
    BLI_assert((ob != NULL) && (GS(ob->id.name) == ID_OB));

    /* Try to get pose-channel. */
    pchan = BKE_pose_channel_find_name(ob->pose, dtar->pchan_name);

    /* Check if object or bone. */
    if (pchan) {
      /* Bone. */
      if (dtar->flag & DTAR_FLAG_LOCALSPACE) {
        if (dtar->flag & DTAR_FLAG_LOCAL_CONSTS) {
          float mat[4][4];

          /* Extract transform just like how the constraints do it! */
          copy_m4_m4(mat, pchan->pose_mat);
          BKE_constraint_mat_convertspace(
              ob, pchan, NULL, mat, CONSTRAINT_SPACE_POSE, CONSTRAINT_SPACE_LOCAL, false);

          /* ... and from that, we get our transform. */
          copy_v3_v3(tmp_loc, mat[3]);
        }
        else {
          /* Transform space (use transform values directly). */
          copy_v3_v3(tmp_loc, pchan->loc);
        }
      }
      else {
        /* Convert to world-space. */
        copy_v3_v3(tmp_loc, pchan->pose_head);
        mul_m4_v3(ob->obmat, tmp_loc);
      }
    }
    else {
      /* Object. */
      if (dtar->flag & DTAR_FLAG_LOCALSPACE) {
        if (dtar->flag & DTAR_FLAG_LOCAL_CONSTS) {
          /* XXX: this should practically be the same as transform space. */
          float mat[4][4];

          /* Extract transform just like how the constraints do it! */
          copy_m4_m4(mat, ob->obmat);
          BKE_constraint_mat_convertspace(
              ob, NULL, NULL, mat, CONSTRAINT_SPACE_WORLD, CONSTRAINT_SPACE_LOCAL, false);

          /* ... and from that, we get our transform. */
          copy_v3_v3(tmp_loc, mat[3]);
        }
        else {
          /* Transform space (use transform values directly). */
          copy_v3_v3(tmp_loc, ob->loc);
        }
      }
      else {
        /* World-space. */
        copy_v3_v3(tmp_loc, ob->obmat[3]);
      }
    }

    /* Copy the location to the right place. */
    if (tarIndex) {
      copy_v3_v3(loc2, tmp_loc);
    }
    else {
      copy_v3_v3(loc1, tmp_loc);
    }
  }
  DRIVER_TARGETS_LOOPER_END;

  /* If we're still here, there should now be two targets to use,
   * so just take the length of the vector between these points. */
  return len_v3v3(loc1, loc2);
}

/**
 * Evaluate 'transform channel' driver variable.
 */
static float dvar_eval_transChan(ChannelDriver *driver, DriverVar *dvar)
{
  DriverTarget *dtar = &dvar->targets[0];
  Object *ob = (Object *)dtar->id;
  bPoseChannel *pchan;
  float mat[4][4];
  float oldEul[3] = {0.0f, 0.0f, 0.0f};
  bool use_eulers = false;
  short rot_order = ROT_MODE_EUL;

  /* Check if this target has valid data. */
  if ((ob == NULL) || (GS(ob->id.name) != ID_OB)) {
    /* Invalid target, so will not have enough targets. */
    driver->flag |= DRIVER_FLAG_INVALID;
    dtar->flag |= DTAR_FLAG_INVALID;
    return 0.0f;
  }

  /* Target should be valid now. */
  dtar->flag &= ~DTAR_FLAG_INVALID;

  /* Try to get pose-channel. */
  pchan = BKE_pose_channel_find_name(ob->pose, dtar->pchan_name);

  /* Check if object or bone, and get transform matrix accordingly:
   * - "use_eulers" code is used to prevent the problems associated with non-uniqueness
   *   of euler decomposition from matrices T20870.
   * - "local-space" is for T21384, where parent results are not wanted
   *   but #DTAR_FLAG_LOCAL_CONSTS is for all the common "corrective-shapes-for-limbs" situations.
   */
  if (pchan) {
    /* Bone. */
    if (pchan->rotmode > 0) {
      copy_v3_v3(oldEul, pchan->eul);
      rot_order = pchan->rotmode;
      use_eulers = true;
    }

    if (dtar->flag & DTAR_FLAG_LOCALSPACE) {
      if (dtar->flag & DTAR_FLAG_LOCAL_CONSTS) {
        /* Just like how the constraints do it! */
        copy_m4_m4(mat, pchan->pose_mat);
        BKE_constraint_mat_convertspace(
            ob, pchan, NULL, mat, CONSTRAINT_SPACE_POSE, CONSTRAINT_SPACE_LOCAL, false);
      }
      else {
        /* Specially calculate local matrix, since chan_mat is not valid
         * since it stores delta transform of pose_mat so that deforms work
         * so it cannot be used here for "transform" space. */
        BKE_pchan_to_mat4(pchan, mat);
      }
    }
    else {
      /* World-space matrix. */
      mul_m4_m4m4(mat, ob->obmat, pchan->pose_mat);
    }
  }
  else {
    /* Object. */
    if (ob->rotmode > 0) {
      copy_v3_v3(oldEul, ob->rot);
      rot_order = ob->rotmode;
      use_eulers = true;
    }

    if (dtar->flag & DTAR_FLAG_LOCALSPACE) {
      if (dtar->flag & DTAR_FLAG_LOCAL_CONSTS) {
        /* Just like how the constraints do it! */
        copy_m4_m4(mat, ob->obmat);
        BKE_constraint_mat_convertspace(
            ob, NULL, NULL, mat, CONSTRAINT_SPACE_WORLD, CONSTRAINT_SPACE_LOCAL, false);
      }
      else {
        /* Transforms to matrix. */
        BKE_object_to_mat4(ob, mat);
      }
    }
    else {
      /* World-space matrix - just the good-old one. */
      copy_m4_m4(mat, ob->obmat);
    }
  }

  /* Check which transform. */
  if (dtar->transChan >= MAX_DTAR_TRANSCHAN_TYPES) {
    /* Not valid channel. */
    return 0.0f;
  }
  if (dtar->transChan == DTAR_TRANSCHAN_SCALE_AVG) {
    /* Cubic root of the change in volume, equal to the geometric mean
     * of scale over all three axes unless the matrix includes shear. */
    return cbrtf(mat4_to_volume_scale(mat));
  }
  if (ELEM(dtar->transChan, DTAR_TRANSCHAN_SCALEX, DTAR_TRANSCHAN_SCALEY, DTAR_TRANSCHAN_SCALEZ)) {
    /* Extract scale, and choose the right axis,
     * inline 'mat4_to_size'. */
    return len_v3(mat[dtar->transChan - DTAR_TRANSCHAN_SCALEX]);
  }
  if (dtar->transChan >= DTAR_TRANSCHAN_ROTX) {
    /* Extract rotation as eulers (if needed)
     * - definitely if rotation order isn't eulers already
     * - if eulers, then we have 2 options:
     *     a) decompose transform matrix as required, then try to make eulers from
     *        there compatible with original values
     *     b) [NOT USED] directly use the original values (no decomposition)
     *         - only an option for "transform space", if quality is really bad with a)
     */
    float quat[4];
    int channel;

    if (dtar->transChan == DTAR_TRANSCHAN_ROTW) {
      channel = 0;
    }
    else {
      channel = 1 + dtar->transChan - DTAR_TRANSCHAN_ROTX;
      BLI_assert(channel < 4);
    }

    BKE_driver_target_matrix_to_rot_channels(
        mat, rot_order, dtar->rotation_mode, channel, false, quat);

    if (use_eulers && dtar->rotation_mode == DTAR_ROTMODE_AUTO) {
      compatible_eul(quat + 1, oldEul);
    }

    return quat[channel];
  }

  /* Extract location and choose right axis. */
  return mat[3][dtar->transChan];
}

/* Convert a quaternion to pseudo-angles representing the weighted amount of rotation. */
static void quaternion_to_angles(float quat[4], int channel)
{
  if (channel < 0) {
    quat[0] = 2.0f * saacosf(quat[0]);

    for (int i = 1; i < 4; i++) {
      quat[i] = 2.0f * saasinf(quat[i]);
    }
  }
  else if (channel == 0) {
    quat[0] = 2.0f * saacosf(quat[0]);
  }
  else {
    quat[channel] = 2.0f * saasinf(quat[channel]);
  }
}

void BKE_driver_target_matrix_to_rot_channels(
    float mat[4][4], int auto_order, int rotation_mode, int channel, bool angles, float r_buf[4])
{
  float *const quat = r_buf;
  float *const eul = r_buf + 1;

  zero_v4(r_buf);

  if (rotation_mode == DTAR_ROTMODE_AUTO) {
    mat4_to_eulO(eul, auto_order, mat);
  }
  else if (rotation_mode >= DTAR_ROTMODE_EULER_MIN && rotation_mode <= DTAR_ROTMODE_EULER_MAX) {
    mat4_to_eulO(eul, rotation_mode, mat);
  }
  else if (rotation_mode == DTAR_ROTMODE_QUATERNION) {
    mat4_to_quat(quat, mat);

    /* For Transformation constraint convenience, convert to pseudo-angles. */
    if (angles) {
      quaternion_to_angles(quat, channel);
    }
  }
  else if (rotation_mode >= DTAR_ROTMODE_SWING_TWIST_X &&
           rotation_mode <= DTAR_ROTMODE_SWING_TWIST_Z) {
    int axis = rotation_mode - DTAR_ROTMODE_SWING_TWIST_X;
    float raw_quat[4], twist;

    mat4_to_quat(raw_quat, mat);

    if (channel == axis + 1) {
      /* If only the twist angle is needed, skip computing swing. */
      twist = quat_split_swing_and_twist(raw_quat, axis, NULL, NULL);
    }
    else {
      twist = quat_split_swing_and_twist(raw_quat, axis, quat, NULL);

      quaternion_to_angles(quat, channel);
    }

    quat[axis + 1] = twist;
  }
  else {
    BLI_assert(false);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Driver Variable Type Info
 * \{ */

/* Table of Driver Variable Type Info Data */
static DriverVarTypeInfo dvar_types[MAX_DVAR_TYPES] = {
    BEGIN_DVAR_TYPEDEF(DVAR_TYPE_SINGLE_PROP) dvar_eval_singleProp, /* Eval callback. */
    1,                                                              /* Number of targets used. */
    {"Property"},                                                   /* UI names for targets */
    {0}                                                             /* Flags. */
    END_DVAR_TYPEDEF,

    BEGIN_DVAR_TYPEDEF(DVAR_TYPE_ROT_DIFF) dvar_eval_rotDiff, /* Eval callback. */
    2,                                                        /* Number of targets used. */
    {"Object/Bone 1", "Object/Bone 2"},                       /* UI names for targets */
    {DTAR_FLAG_STRUCT_REF | DTAR_FLAG_ID_OB_ONLY,
     DTAR_FLAG_STRUCT_REF | DTAR_FLAG_ID_OB_ONLY} /* Flags. */
    END_DVAR_TYPEDEF,

    BEGIN_DVAR_TYPEDEF(DVAR_TYPE_LOC_DIFF) dvar_eval_locDiff, /* Eval callback. */
    2,                                                        /* Number of targets used. */
    {"Object/Bone 1", "Object/Bone 2"},                       /* UI names for targets */
    {DTAR_FLAG_STRUCT_REF | DTAR_FLAG_ID_OB_ONLY,
     DTAR_FLAG_STRUCT_REF | DTAR_FLAG_ID_OB_ONLY} /* Flags. */
    END_DVAR_TYPEDEF,

    BEGIN_DVAR_TYPEDEF(DVAR_TYPE_TRANSFORM_CHAN) dvar_eval_transChan, /* Eval callback. */
    1,                                                                /* Number of targets used. */
    {"Object/Bone"},                                                  /* UI names for targets */
    {DTAR_FLAG_STRUCT_REF | DTAR_FLAG_ID_OB_ONLY}                     /* Flags. */
    END_DVAR_TYPEDEF,
};

/* Get driver variable typeinfo */
static const DriverVarTypeInfo *get_dvar_typeinfo(int type)
{
  /* Check if valid type. */
  if ((type >= 0) && (type < MAX_DVAR_TYPES)) {
    return &dvar_types[type];
  }

  return NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Driver API
 * \{ */

void driver_free_variable(ListBase *variables, DriverVar *dvar)
{
  /* Sanity checks. */
  if (dvar == NULL) {
    return;
  }

  /* Free target vars:
   * - need to go over all of them, not just up to the ones that are used
   *   currently, since there may be some lingering RNA paths from
   *   previous users needing freeing
   */
  DRIVER_TARGETS_LOOPER_BEGIN (dvar) {
    /* Free RNA path if applicable. */
    if (dtar->rna_path) {
      MEM_freeN(dtar->rna_path);
    }
  }
  DRIVER_TARGETS_LOOPER_END;

  /* Remove the variable from the driver. */
  BLI_freelinkN(variables, dvar);
}

void driver_free_variable_ex(ChannelDriver *driver, DriverVar *dvar)
{
  /* Remove and free the driver variable. */
  driver_free_variable(&driver->variables, dvar);

  /* Since driver variables are cached, the expression needs re-compiling too. */
  BKE_driver_invalidate_expression(driver, false, true);
}

void driver_variables_copy(ListBase *dst_vars, const ListBase *src_vars)
{
  BLI_assert(BLI_listbase_is_empty(dst_vars));
  BLI_duplicatelist(dst_vars, src_vars);

  LISTBASE_FOREACH (DriverVar *, dvar, dst_vars) {
    /* Need to go over all targets so that we don't leave any dangling paths. */
    DRIVER_TARGETS_LOOPER_BEGIN (dvar) {
      /* Make a copy of target's rna path if available. */
      if (dtar->rna_path) {
        dtar->rna_path = MEM_dupallocN(dtar->rna_path);
      }
    }
    DRIVER_TARGETS_LOOPER_END;
  }
}

void driver_change_variable_type(DriverVar *dvar, int type)
{
  const DriverVarTypeInfo *dvti = get_dvar_typeinfo(type);

  /* Sanity check. */
  if (ELEM(NULL, dvar, dvti)) {
    return;
  }

  /* Set the new settings. */
  dvar->type = type;
  dvar->num_targets = dvti->num_targets;

  /* Make changes to the targets based on the defines for these types.
   * NOTE: only need to make sure the ones we're using here are valid. */
  DRIVER_TARGETS_USED_LOOPER_BEGIN (dvar) {
    short flags = dvti->target_flags[tarIndex];

    /* Store the flags. */
    dtar->flag = flags;

    /* Object ID types only, or idtype not yet initialized. */
    if ((flags & DTAR_FLAG_ID_OB_ONLY) || (dtar->idtype == 0)) {
      dtar->idtype = ID_OB;
    }
  }
  DRIVER_TARGETS_LOOPER_END;
}

void driver_variable_name_validate(DriverVar *dvar)
{
  /* Special character blacklist */
  const char special_char_blacklist[] = {
      '~', '`', '!', '@', '#', '$', '%', '^', '&', '*', '+', '=', '-',  '/',  '\\',
      '?', ':', ';', '<', '>', '{', '}', '[', ']', '|', ' ', '.', '\t', '\n', '\r',
  };

  /* Sanity checks. */
  if (dvar == NULL) {
    return;
  }

  /* Clear all invalid-name flags. */
  dvar->flag &= ~DVAR_ALL_INVALID_FLAGS;

  /* 0) Zero-length identifiers are not allowed */
  if (dvar->name[0] == '\0') {
    dvar->flag |= DVAR_FLAG_INVALID_EMPTY;
  }

  /* 1) Must start with a letter */
  /* XXX: We assume that valid unicode letters in other languages are ok too,
   * hence the blacklisting. */
  if (IN_RANGE_INCL(dvar->name[0], '0', '9')) {
    dvar->flag |= DVAR_FLAG_INVALID_START_NUM;
  }
  else if (dvar->name[0] == '_') {
    /* NOTE: We don't allow names to start with underscores
     * (i.e. it helps when ruling out security risks) */
    dvar->flag |= DVAR_FLAG_INVALID_START_CHAR;
  }

  /* 2) Must not contain invalid stuff in the middle of the string */
  if (strchr(dvar->name, ' ')) {
    dvar->flag |= DVAR_FLAG_INVALID_HAS_SPACE;
  }
  if (strchr(dvar->name, '.')) {
    dvar->flag |= DVAR_FLAG_INVALID_HAS_DOT;
  }

  /* 3) Check for special characters - Either at start, or in the middle */
  for (int i = 0; i < sizeof(special_char_blacklist); i++) {
    char *match = strchr(dvar->name, special_char_blacklist[i]);

    if (match == dvar->name) {
      dvar->flag |= DVAR_FLAG_INVALID_START_CHAR;
    }
    else if (match != NULL) {
      dvar->flag |= DVAR_FLAG_INVALID_HAS_SPECIAL;
    }
  }

  /* 4) Check if the name is a reserved keyword
   * NOTE: These won't confuse Python, but it will be impossible to use the variable
   *       in an expression without Python misinterpreting what these are for
   */
#ifdef WITH_PYTHON
  if (BPY_string_is_keyword(dvar->name)) {
    dvar->flag |= DVAR_FLAG_INVALID_PY_KEYWORD;
  }
#endif

  /* If any these conditions match, the name is invalid */
  if (dvar->flag & DVAR_ALL_INVALID_FLAGS) {
    dvar->flag |= DVAR_FLAG_INVALID_NAME;
  }
}

void driver_variable_unique_name(DriverVar *dvar)
{
  ListBase variables = BLI_listbase_from_link((Link *)dvar);
  BLI_uniquename(&variables, dvar, dvar->name, '_', offsetof(DriverVar, name), sizeof(dvar->name));
}

DriverVar *driver_add_new_variable(ChannelDriver *driver)
{
  DriverVar *dvar;

  /* Sanity checks. */
  if (driver == NULL) {
    return NULL;
  }

  /* Make a new variable. */
  dvar = MEM_callocN(sizeof(DriverVar), "DriverVar");
  BLI_addtail(&driver->variables, dvar);

  /* Give the variable a 'unique' name. */
  strcpy(dvar->name, CTX_DATA_(BLT_I18NCONTEXT_ID_ACTION, "var"));
  BLI_uniquename(&driver->variables,
                 dvar,
                 CTX_DATA_(BLT_I18NCONTEXT_ID_ACTION, "var"),
                 '_',
                 offsetof(DriverVar, name),
                 sizeof(dvar->name));

  /* Set the default type to 'single prop'. */
  driver_change_variable_type(dvar, DVAR_TYPE_SINGLE_PROP);

  /* Since driver variables are cached, the expression needs re-compiling too. */
  BKE_driver_invalidate_expression(driver, false, true);

  /* Return the target. */
  return dvar;
}

void fcurve_free_driver(FCurve *fcu)
{
  ChannelDriver *driver;
  DriverVar *dvar, *dvarn;

  /* Sanity checks. */
  if (ELEM(NULL, fcu, fcu->driver)) {
    return;
  }
  driver = fcu->driver;

  /* Free driver targets. */
  for (dvar = driver->variables.first; dvar; dvar = dvarn) {
    dvarn = dvar->next;
    driver_free_variable_ex(driver, dvar);
  }

#ifdef WITH_PYTHON
  /* Free compiled driver expression. */
  if (driver->expr_comp) {
    BPY_DECREF(driver->expr_comp);
  }
#endif

  BLI_expr_pylike_free(driver->expr_simple);

  /* Free driver itself, then set F-Curve's point to this to NULL
   * (as the curve may still be used). */
  MEM_freeN(driver);
  fcu->driver = NULL;
}

ChannelDriver *fcurve_copy_driver(const ChannelDriver *driver)
{
  ChannelDriver *ndriver;

  /* Sanity checks. */
  if (driver == NULL) {
    return NULL;
  }

  /* Copy all data. */
  ndriver = MEM_dupallocN(driver);
  ndriver->expr_comp = NULL;
  ndriver->expr_simple = NULL;

  /* Copy variables. */

  /* To get rid of refs to non-copied data (that's still used on original). */
  BLI_listbase_clear(&ndriver->variables);
  driver_variables_copy(&ndriver->variables, &driver->variables);

  /* Return the new driver. */
  return ndriver;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Driver Expression Evaluation
 * \{ */

/* Index constants for the expression parameter array. */
enum {
  /* Index of the 'frame' variable. */
  VAR_INDEX_FRAME = 0,
  /* Index of the first user-defined driver variable. */
  VAR_INDEX_CUSTOM
};

static ExprPyLike_Parsed *driver_compile_simple_expr_impl(ChannelDriver *driver)
{
  /* Prepare parameter names. */
  int names_len = BLI_listbase_count(&driver->variables);
  const char **names = BLI_array_alloca(names, names_len + VAR_INDEX_CUSTOM);
  int i = VAR_INDEX_CUSTOM;

  names[VAR_INDEX_FRAME] = "frame";

  LISTBASE_FOREACH (DriverVar *, dvar, &driver->variables) {
    names[i++] = dvar->name;
  }

  return BLI_expr_pylike_parse(driver->expression, names, names_len + VAR_INDEX_CUSTOM);
}

static bool driver_check_simple_expr_depends_on_time(ExprPyLike_Parsed *expr)
{
  /* Check if the 'frame' parameter is actually used. */
  return BLI_expr_pylike_is_using_param(expr, VAR_INDEX_FRAME);
}

static bool driver_evaluate_simple_expr(ChannelDriver *driver,
                                        ExprPyLike_Parsed *expr,
                                        float *result,
                                        float time)
{
  /* Prepare parameter values. */
  int vars_len = BLI_listbase_count(&driver->variables);
  double *vars = BLI_array_alloca(vars, vars_len + VAR_INDEX_CUSTOM);
  int i = VAR_INDEX_CUSTOM;

  vars[VAR_INDEX_FRAME] = time;

  LISTBASE_FOREACH (DriverVar *, dvar, &driver->variables) {
    vars[i++] = driver_get_variable_value(driver, dvar);
  }

  /* Evaluate expression. */
  double result_val;
  eExprPyLike_EvalStatus status = BLI_expr_pylike_eval(
      expr, vars, vars_len + VAR_INDEX_CUSTOM, &result_val);
  const char *message;

  switch (status) {
    case EXPR_PYLIKE_SUCCESS:
      if (isfinite(result_val)) {
        *result = (float)result_val;
      }
      return true;

    case EXPR_PYLIKE_DIV_BY_ZERO:
    case EXPR_PYLIKE_MATH_ERROR:
      message = (status == EXPR_PYLIKE_DIV_BY_ZERO) ? "Division by Zero" : "Math Domain Error";
      CLOG_ERROR(&LOG, "%s in Driver: '%s'", message, driver->expression);

      driver->flag |= DRIVER_FLAG_INVALID;
      return true;

    default:
      /* Arriving here means a bug, not user error. */
      CLOG_ERROR(&LOG, "simple driver expression evaluation failed: '%s'", driver->expression);
      return false;
  }
}

/* Compile and cache the driver expression if necessary, with thread safety. */
static bool driver_compile_simple_expr(ChannelDriver *driver)
{
  if (driver->expr_simple != NULL) {
    return true;
  }

  if (driver->type != DRIVER_TYPE_PYTHON) {
    return false;
  }

  /* It's safe to parse in multiple threads; at worst it'll
   * waste some effort, but in return avoids mutex contention. */
  ExprPyLike_Parsed *expr = driver_compile_simple_expr_impl(driver);

  /* Store the result if the field is still NULL, or discard
   * it if another thread got here first. */
  if (atomic_cas_ptr((void **)&driver->expr_simple, NULL, expr) != NULL) {
    BLI_expr_pylike_free(expr);
  }

  return true;
}

/* Try using the simple expression evaluator to compute the result of the driver.
 * On success, stores the result and returns true; on failure result is set to 0. */
static bool driver_try_evaluate_simple_expr(ChannelDriver *driver,
                                            ChannelDriver *driver_orig,
                                            float *result,
                                            float time)
{
  *result = 0.0f;

  return driver_compile_simple_expr(driver_orig) &&
         BLI_expr_pylike_is_valid(driver_orig->expr_simple) &&
         driver_evaluate_simple_expr(driver, driver_orig->expr_simple, result, time);
}

bool BKE_driver_has_simple_expression(ChannelDriver *driver)
{
  return driver_compile_simple_expr(driver) && BLI_expr_pylike_is_valid(driver->expr_simple);
}

/* TODO(sergey): This is somewhat weak, but we don't want neither false-positive
 * time dependencies nor special exceptions in the depsgraph evaluation. */
static bool python_driver_exression_depends_on_time(const char *expression)
{
  if (expression[0] == '\0') {
    /* Empty expression depends on nothing. */
    return false;
  }
  if (strchr(expression, '(') != NULL) {
    /* Function calls are considered dependent on a time. */
    return true;
  }
  if (strstr(expression, "frame") != NULL) {
    /* Variable `frame` depends on time. */
    /* TODO(sergey): This is a bit weak, but not sure about better way of handling this. */
    return true;
  }
  /* Possible indirect time relation s should be handled via variable targets. */
  return false;
}

bool BKE_driver_expression_depends_on_time(ChannelDriver *driver)
{
  if (driver->type != DRIVER_TYPE_PYTHON) {
    return false;
  }

  if (BKE_driver_has_simple_expression(driver)) {
    /* Simple expressions can be checked exactly. */
    return driver_check_simple_expr_depends_on_time(driver->expr_simple);
  }

  /* Otherwise, heuristically scan the expression string for certain patterns. */
  return python_driver_exression_depends_on_time(driver->expression);
}

void BKE_driver_invalidate_expression(ChannelDriver *driver,
                                      bool expr_changed,
                                      bool varname_changed)
{
  if (expr_changed || varname_changed) {
    BLI_expr_pylike_free(driver->expr_simple);
    driver->expr_simple = NULL;
  }

#ifdef WITH_PYTHON
  if (expr_changed) {
    driver->flag |= DRIVER_FLAG_RECOMPILE;
  }

  if (varname_changed) {
    driver->flag |= DRIVER_FLAG_RENAMEVAR;
  }
#endif
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Driver Evaluation
 * \{ */

float driver_get_variable_value(ChannelDriver *driver, DriverVar *dvar)
{
  const DriverVarTypeInfo *dvti;

  /* Sanity check. */
  if (ELEM(NULL, driver, dvar)) {
    return 0.0f;
  }

  /* Call the relevant callbacks to get the variable value
   * using the variable type info, storing the obtained value
   * in `dvar->curval` so that drivers can be debugged. */
  dvti = get_dvar_typeinfo(dvar->type);

  if (dvti && dvti->get_value) {
    dvar->curval = dvti->get_value(driver, dvar);
  }
  else {
    dvar->curval = 0.0f;
  }

  return dvar->curval;
}

static void evaluate_driver_sum(ChannelDriver *driver)
{
  DriverVar *dvar;

  /* Check how many variables there are first (i.e. just one?). */
  if (BLI_listbase_is_single(&driver->variables)) {
    /* Just one target, so just use that. */
    dvar = driver->variables.first;
    driver->curval = driver_get_variable_value(driver, dvar);
    return;
  }

  /* More than one target, so average the values of the targets. */
  float value = 0.0f;
  int tot = 0;

  /* Loop through targets, adding (hopefully we don't get any overflow!). */
  for (dvar = driver->variables.first; dvar; dvar = dvar->next) {
    value += driver_get_variable_value(driver, dvar);
    tot++;
  }

  /* Perform operations on the total if appropriate. */
  if (driver->type == DRIVER_TYPE_AVERAGE) {
    driver->curval = tot ? (value / (float)tot) : 0.0f;
  }
  else {
    driver->curval = value;
  }
}

static void evaluate_driver_min_max(ChannelDriver *driver)
{
  DriverVar *dvar;
  float value = 0.0f;

  /* Loop through the variables, getting the values and comparing them to existing ones. */
  for (dvar = driver->variables.first; dvar; dvar = dvar->next) {
    /* Get value. */
    float tmp_val = driver_get_variable_value(driver, dvar);

    /* Store this value if appropriate. */
    if (dvar->prev) {
      /* Check if greater/smaller than the baseline. */
      if (driver->type == DRIVER_TYPE_MAX) {
        /* Max? */
        if (tmp_val > value) {
          value = tmp_val;
        }
      }
      else {
        /* Min? */
        if (tmp_val < value) {
          value = tmp_val;
        }
      }
    }
    else {
      /* First item - make this the baseline for comparisons. */
      value = tmp_val;
    }
  }

  /* Store value in driver. */
  driver->curval = value;
}

static void evaluate_driver_python(PathResolvedRNA *anim_rna,
                                   ChannelDriver *driver,
                                   ChannelDriver *driver_orig,
                                   const AnimationEvalContext *anim_eval_context)
{
  /* Check for empty or invalid expression. */
  if ((driver_orig->expression[0] == '\0') || (driver_orig->flag & DRIVER_FLAG_INVALID)) {
    driver->curval = 0.0f;
  }
  else if (!driver_try_evaluate_simple_expr(
               driver, driver_orig, &driver->curval, anim_eval_context->eval_time)) {
#ifdef WITH_PYTHON
    /* This evaluates the expression using Python, and returns its result:
     * - on errors it reports, then returns 0.0f. */
    BLI_mutex_lock(&python_driver_lock);

    driver->curval = BPY_driver_exec(anim_rna, driver, driver_orig, anim_eval_context);

    BLI_mutex_unlock(&python_driver_lock);
#else  /* WITH_PYTHON */
    UNUSED_VARS(anim_rna, anim_eval_context);
#endif /* WITH_PYTHON */
  }
}

float evaluate_driver(PathResolvedRNA *anim_rna,
                      ChannelDriver *driver,
                      ChannelDriver *driver_orig,
                      const AnimationEvalContext *anim_eval_context)
{
  /* Check if driver can be evaluated. */
  if (driver_orig->flag & DRIVER_FLAG_INVALID) {
    return 0.0f;
  }

  switch (driver->type) {
    case DRIVER_TYPE_AVERAGE: /* Average values of driver targets. */
    case DRIVER_TYPE_SUM:     /* Sum values of driver targets. */
      evaluate_driver_sum(driver);
      break;
    case DRIVER_TYPE_MIN: /* Smallest value. */
    case DRIVER_TYPE_MAX: /* Largest value. */
      evaluate_driver_min_max(driver);
      break;
    case DRIVER_TYPE_PYTHON: /* Expression. */
      evaluate_driver_python(anim_rna, driver, driver_orig, anim_eval_context);
      break;
    default:
      /* Special 'hack' - just use stored value
       * This is currently used as the mechanism which allows animated settings to be able
       * to be changed via the UI. */
      break;
  }

  /* Return value for driver. */
  return driver->curval;
}

/** \} */
