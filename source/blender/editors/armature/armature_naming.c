/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup edarmature
 * Operators and API's for renaming bones both in and out of Edit Mode.
 *
 * This file contains functions/API's for renaming bones and/or working with them.
 */

#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_armature_types.h"
#include "DNA_camera_types.h"
#include "DNA_constraint_types.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_object_types.h"

#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_action.h"
#include "BKE_animsys.h"
#include "BKE_armature.h"
#include "BKE_constraint.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_modifier.h"

#include "DEG_depsgraph.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_armature.h"
#include "ED_screen.h"

#include "armature_intern.h"

/* -------------------------------------------------------------------- */
/** \name Unique Bone Name Utility (Edit Mode)
 * \{ */

/* NOTE: there's a ed_armature_bone_unique_name() too! */
static bool editbone_unique_check(void *arg, const char *name)
{
  struct {
    ListBase *lb;
    void *bone;
  } *data = arg;
  EditBone *dupli = ED_armature_ebone_find_name(data->lb, name);
  return dupli && dupli != data->bone;
}

void ED_armature_ebone_unique_name(ListBase *ebones, char *name, EditBone *bone)
{
  struct {
    ListBase *lb;
    void *bone;
  } data;
  data.lb = ebones;
  data.bone = bone;

  BLI_uniquename_cb(editbone_unique_check, &data, DATA_("Bone"), '.', name, sizeof(bone->name));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Unique Bone Name Utility (Object Mode)
 * \{ */

static bool bone_unique_check(void *arg, const char *name)
{
  return BKE_armature_find_bone_name((bArmature *)arg, name) != NULL;
}

static void ed_armature_bone_unique_name(bArmature *arm, char *name)
{
  BLI_uniquename_cb(
      bone_unique_check, (void *)arm, DATA_("Bone"), '.', name, sizeof(((Bone *)NULL)->name));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bone Renaming (Object & Edit Mode API)
 * \{ */

/* helper call for armature_bone_rename */
static void constraint_bone_name_fix(Object *ob,
                                     ListBase *conlist,
                                     const char *oldname,
                                     const char *newname)
{
  bConstraint *curcon;
  bConstraintTarget *ct;

  for (curcon = conlist->first; curcon; curcon = curcon->next) {
    ListBase targets = {NULL, NULL};

    /* constraint targets */
    if (BKE_constraint_targets_get(curcon, &targets)) {
      for (ct = targets.first; ct; ct = ct->next) {
        if (ct->tar == ob) {
          if (STREQ(ct->subtarget, oldname)) {
            BLI_strncpy(ct->subtarget, newname, MAXBONENAME);
          }
        }
      }

      BKE_constraint_targets_flush(curcon, &targets, 0);
    }

    /* action constraints */
    if (curcon->type == CONSTRAINT_TYPE_ACTION) {
      bActionConstraint *actcon = (bActionConstraint *)curcon->data;
      BKE_action_fix_paths_rename(&ob->id, actcon->act, "pose.bones", oldname, newname, 0, 0, 1);
    }
  }
}

void ED_armature_bone_rename(Main *bmain,
                             bArmature *arm,
                             const char *oldnamep,
                             const char *newnamep)
{
  Object *ob;
  char newname[MAXBONENAME];
  char oldname[MAXBONENAME];

  /* names better differ! */
  if (!STREQLEN(oldnamep, newnamep, MAXBONENAME)) {

    /* we alter newname string... so make copy */
    BLI_strncpy(newname, newnamep, MAXBONENAME);
    /* we use oldname for search... so make copy */
    BLI_strncpy(oldname, oldnamep, MAXBONENAME);

    /* now check if we're in editmode, we need to find the unique name */
    if (arm->edbo) {
      EditBone *eBone = ED_armature_ebone_find_name(arm->edbo, oldname);

      if (eBone) {
        ED_armature_ebone_unique_name(arm->edbo, newname, NULL);
        BLI_strncpy(eBone->name, newname, MAXBONENAME);
      }
      else {
        return;
      }
    }
    else {
      Bone *bone = BKE_armature_find_bone_name(arm, oldname);

      if (bone) {
        ed_armature_bone_unique_name(arm, newname);

        if (arm->bonehash) {
          BLI_assert(BLI_ghash_haskey(arm->bonehash, bone->name));
          BLI_ghash_remove(arm->bonehash, bone->name, NULL, NULL);
        }

        BLI_strncpy(bone->name, newname, MAXBONENAME);

        if (arm->bonehash) {
          BLI_ghash_insert(arm->bonehash, bone->name, bone);
        }
      }
      else {
        return;
      }
    }

    /* force copy on write to update database */
    DEG_id_tag_update(&arm->id, ID_RECALC_COPY_ON_WRITE);

    /* do entire dbase - objects */
    for (ob = bmain->objects.first; ob; ob = ob->id.next) {
      ModifierData *md;

      /* we have the object using the armature */
      if (arm == ob->data) {
        Object *cob;

        /* Rename the pose channel, if it exists */
        if (ob->pose) {
          bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose, oldname);
          if (pchan) {
            GHash *gh = ob->pose->chanhash;

            /* remove the old hash entry, and replace with the new name */
            if (gh) {
              BLI_assert(BLI_ghash_haskey(gh, pchan->name));
              BLI_ghash_remove(gh, pchan->name, NULL, NULL);
            }

            BLI_strncpy(pchan->name, newname, MAXBONENAME);

            if (gh) {
              BLI_ghash_insert(gh, pchan->name, pchan);
            }
          }

          BLI_assert(BKE_pose_channels_is_valid(ob->pose) == true);
        }

        /* Update any object constraints to use the new bone name */
        for (cob = bmain->objects.first; cob; cob = cob->id.next) {
          if (cob->constraints.first) {
            constraint_bone_name_fix(ob, &cob->constraints, oldname, newname);
          }
          if (cob->pose) {
            bPoseChannel *pchan;
            for (pchan = cob->pose->chanbase.first; pchan; pchan = pchan->next) {
              constraint_bone_name_fix(ob, &pchan->constraints, oldname, newname);
            }
          }
        }
      }

      /* See if an object is parented to this armature */
      if (ob->parent && (ob->parent->data == arm)) {
        if (ob->partype == PARBONE) {
          /* bone name in object */
          if (STREQ(ob->parsubstr, oldname)) {
            BLI_strncpy(ob->parsubstr, newname, MAXBONENAME);
          }
        }
      }

      if (BKE_modifiers_uses_armature(ob, arm) && BKE_object_supports_vertex_groups(ob)) {
        bDeformGroup *dg = BKE_object_defgroup_find_name(ob, oldname);
        if (dg) {
          BLI_strncpy(dg->name, newname, MAXBONENAME);
          DEG_id_tag_update(ob->data, ID_RECALC_GEOMETRY);
        }
      }

      /* fix modifiers that might be using this name */
      for (md = ob->modifiers.first; md; md = md->next) {
        switch (md->type) {
          case eModifierType_Hook: {
            HookModifierData *hmd = (HookModifierData *)md;

            if (hmd->object && (hmd->object->data == arm)) {
              if (STREQ(hmd->subtarget, oldname)) {
                BLI_strncpy(hmd->subtarget, newname, MAXBONENAME);
              }
            }
            break;
          }
          case eModifierType_UVWarp: {
            UVWarpModifierData *umd = (UVWarpModifierData *)md;

            if (umd->object_src && (umd->object_src->data == arm)) {
              if (STREQ(umd->bone_src, oldname)) {
                BLI_strncpy(umd->bone_src, newname, MAXBONENAME);
              }
            }
            if (umd->object_dst && (umd->object_dst->data == arm)) {
              if (STREQ(umd->bone_dst, oldname)) {
                BLI_strncpy(umd->bone_dst, newname, MAXBONENAME);
              }
            }
            break;
          }
          default:
            break;
        }
      }

      /* fix camera focus */
      if (ob->type == OB_CAMERA) {
        Camera *cam = (Camera *)ob->data;
        if ((cam->dof.focus_object != NULL) && (cam->dof.focus_object->data == arm)) {
          if (STREQ(cam->dof.focus_subtarget, oldname)) {
            BLI_strncpy(cam->dof.focus_subtarget, newname, MAXBONENAME);
            DEG_id_tag_update(&cam->id, ID_RECALC_COPY_ON_WRITE);
          }
        }
      }

      /* fix grease pencil modifiers and vertex groups */
      if (ob->type == OB_GPENCIL) {

        bGPdata *gpd = (bGPdata *)ob->data;
        LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
          if ((gpl->parent != NULL) && (gpl->parent->data == arm)) {
            if (STREQ(gpl->parsubstr, oldname)) {
              BLI_strncpy(gpl->parsubstr, newname, MAXBONENAME);
            }
          }
        }

        LISTBASE_FOREACH (GpencilModifierData *, gp_md, &ob->greasepencil_modifiers) {
          switch (gp_md->type) {
            case eGpencilModifierType_Armature: {
              ArmatureGpencilModifierData *mmd = (ArmatureGpencilModifierData *)gp_md;
              if (mmd->object && mmd->object->data == arm) {
                bDeformGroup *dg = BKE_object_defgroup_find_name(ob, oldname);
                if (dg) {
                  BLI_strncpy(dg->name, newname, MAXBONENAME);
                  DEG_id_tag_update(ob->data, ID_RECALC_GEOMETRY);
                }
              }
              break;
            }
            case eGpencilModifierType_Hook: {
              HookGpencilModifierData *hgp_md = (HookGpencilModifierData *)gp_md;
              if (hgp_md->object && (hgp_md->object->data == arm)) {
                if (STREQ(hgp_md->subtarget, oldname)) {
                  BLI_strncpy(hgp_md->subtarget, newname, MAXBONENAME);
                }
              }
              break;
            }
            default:
              break;
          }
        }
      }
      DEG_id_tag_update(&ob->id, ID_RECALC_COPY_ON_WRITE);
    }

    /* Fix all animdata that may refer to this bone -
     * we can't just do the ones attached to objects,
     * since other ID-blocks may have drivers referring to this bone T29822. */

    /* XXX: the ID here is for armatures,
     * but most bone drivers are actually on the object instead. */
    {

      BKE_animdata_fix_paths_rename_all(&arm->id, "pose.bones", oldname, newname);
    }

    /* correct view locking */
    {
      bScreen *screen;
      for (screen = bmain->screens.first; screen; screen = screen->id.next) {
        ScrArea *area;
        /* add regions */
        for (area = screen->areabase.first; area; area = area->next) {
          SpaceLink *sl;
          for (sl = area->spacedata.first; sl; sl = sl->next) {
            if (sl->spacetype == SPACE_VIEW3D) {
              View3D *v3d = (View3D *)sl;
              if (v3d->ob_center && v3d->ob_center->data == arm) {
                if (STREQ(v3d->ob_center_bone, oldname)) {
                  BLI_strncpy(v3d->ob_center_bone, newname, MAXBONENAME);
                }
              }
            }
          }
        }
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bone Flipping (Object & Edit Mode API)
 * \{ */

typedef struct BoneFlipNameData {
  struct BoneFlipNameData *next, *prev;
  char *name;
  char name_flip[MAXBONENAME];
} BoneFlipNameData;

void ED_armature_bones_flip_names(Main *bmain,
                                  bArmature *arm,
                                  ListBase *bones_names,
                                  const bool do_strip_numbers)
{
  ListBase bones_names_conflicts = {NULL};
  BoneFlipNameData *bfn;

  /* First pass: generate flip names, and blindly rename.
   * If rename did not yield expected result,
   * store both bone's name and expected flipped one into temp list for second pass. */
  LISTBASE_FOREACH (LinkData *, link, bones_names) {
    char name_flip[MAXBONENAME];
    char *name = link->data;

    /* WARNING: if do_strip_numbers is set, expect completely mismatched names in cases like
     * Bone.R, Bone.R.001, Bone.R.002, etc. */
    BLI_string_flip_side_name(name_flip, name, do_strip_numbers, sizeof(name_flip));

    ED_armature_bone_rename(bmain, arm, name, name_flip);

    if (!STREQ(name, name_flip)) {
      bfn = alloca(sizeof(BoneFlipNameData));
      bfn->name = name;
      BLI_strncpy(bfn->name_flip, name_flip, sizeof(bfn->name_flip));
      BLI_addtail(&bones_names_conflicts, bfn);
    }
  }

  /* Second pass to handle the bones that have naming conflicts with other bones.
   * Note that if the other bone was not selected, its name was not flipped,
   * so conflict remains and that second rename simply generates a new numbered alternative name.
   */
  for (bfn = bones_names_conflicts.first; bfn; bfn = bfn->next) {
    ED_armature_bone_rename(bmain, arm, bfn->name, bfn->name_flip);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Flip Bone Names (Edit Mode Operator)
 * \{ */

static int armature_flip_names_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *ob_active = CTX_data_edit_object(C);

  const bool do_strip_numbers = RNA_boolean_get(op->ptr, "do_strip_numbers");

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);
  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *ob = objects[ob_index];
    bArmature *arm = ob->data;

    /* Paranoia check. */
    if (ob_active->pose == NULL) {
      continue;
    }

    ListBase bones_names = {NULL};

    LISTBASE_FOREACH (EditBone *, ebone, arm->edbo) {
      if (EBONE_VISIBLE(arm, ebone)) {
        if (ebone->flag & BONE_SELECTED) {
          BLI_addtail(&bones_names, BLI_genericNodeN(ebone->name));

          if (arm->flag & ARM_MIRROR_EDIT) {
            EditBone *flipbone = ED_armature_ebone_get_mirrored(arm->edbo, ebone);
            if ((flipbone) && !(flipbone->flag & BONE_SELECTED)) {
              BLI_addtail(&bones_names, BLI_genericNodeN(flipbone->name));
            }
          }
        }
      }
    }

    if (BLI_listbase_is_empty(&bones_names)) {
      continue;
    }

    ED_armature_bones_flip_names(bmain, arm, &bones_names, do_strip_numbers);

    BLI_freelistN(&bones_names);

    /* since we renamed stuff... */
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);

    /* copied from #rna_Bone_update_renamed */
    /* redraw view */
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob->data);

    /* update animation channels */
    WM_event_add_notifier(C, NC_ANIMATION | ND_ANIMCHAN, ob->data);
  }
  MEM_freeN(objects);

  return OPERATOR_FINISHED;
}

void ARMATURE_OT_flip_names(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Flip Names";
  ot->idname = "ARMATURE_OT_flip_names";
  ot->description = "Flips (and corrects) the axis suffixes of the names of selected bones";

  /* api callbacks */
  ot->exec = armature_flip_names_exec;
  ot->poll = ED_operator_editarmature;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "do_strip_numbers",
                  false,
                  "Strip Numbers",
                  "Try to remove right-most dot-number from flipped names.\n"
                  "Warning: May result in incoherent naming in some cases");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bone Auto Side Names (Edit Mode Operator)
 * \{ */

static int armature_autoside_names_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Main *bmain = CTX_data_main(C);
  char newname[MAXBONENAME];
  const short axis = RNA_enum_get(op->ptr, "type");
  bool changed_multi = false;

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);
  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *ob = objects[ob_index];
    bArmature *arm = ob->data;
    bool changed = false;

    /* Paranoia checks. */
    if (ELEM(NULL, ob, ob->pose)) {
      continue;
    }

    LISTBASE_FOREACH (EditBone *, ebone, arm->edbo) {
      if (EBONE_EDITABLE(ebone)) {

        /* We first need to do the flipped bone, then the original one.
         * Otherwise we can't find the flipped one because of the bone name change. */
        if (arm->flag & ARM_MIRROR_EDIT) {
          EditBone *flipbone = ED_armature_ebone_get_mirrored(arm->edbo, ebone);
          if ((flipbone) && !(flipbone->flag & BONE_SELECTED)) {
            BLI_strncpy(newname, flipbone->name, sizeof(newname));
            if (bone_autoside_name(newname, 1, axis, flipbone->head[axis], flipbone->tail[axis])) {
              ED_armature_bone_rename(bmain, arm, flipbone->name, newname);
              changed = true;
            }
          }
        }

        BLI_strncpy(newname, ebone->name, sizeof(newname));
        if (bone_autoside_name(newname, 1, axis, ebone->head[axis], ebone->tail[axis])) {
          ED_armature_bone_rename(bmain, arm, ebone->name, newname);
          changed = true;
        }
      }
    }

    if (!changed) {
      continue;
    }

    changed_multi = true;

    /* Since we renamed stuff... */
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);

    /* NOTE: notifier might evolve. */
    WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);
  }
  MEM_freeN(objects);
  return changed_multi ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void ARMATURE_OT_autoside_names(wmOperatorType *ot)
{
  static const EnumPropertyItem axis_items[] = {
      {0, "XAXIS", 0, "X-Axis", "Left/Right"},
      {1, "YAXIS", 0, "Y-Axis", "Front/Back"},
      {2, "ZAXIS", 0, "Z-Axis", "Top/Bottom"},
      {0, NULL, 0, NULL, NULL},
  };

  /* identifiers */
  ot->name = "Auto-Name by Axis";
  ot->idname = "ARMATURE_OT_autoside_names";
  ot->description =
      "Automatically renames the selected bones according to which side of the target axis they "
      "fall on";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = armature_autoside_names_exec;
  ot->poll = ED_operator_editarmature;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* settings */
  ot->prop = RNA_def_enum(ot->srna, "type", axis_items, 0, "Axis", "Axis tag names with");
}

/** \} */
