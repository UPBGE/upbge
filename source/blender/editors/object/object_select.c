/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup edobj
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_collection_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_modifier_types.h"
#include "DNA_property_types.h"
#include "DNA_scene_types.h"
#include "DNA_workspace_types.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_math_bits.h"
#include "BLI_rand.h"
#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_action.h"
#include "BKE_armature.h"
#include "BKE_collection.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_particle.h"
#include "BKE_property.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_workspace.h"

#include "DEG_depsgraph.h"

#include "WM_api.h"
#include "WM_message.h"
#include "WM_types.h"

#include "ED_armature.h"
#include "ED_keyframing.h"
#include "ED_object.h"
#include "ED_outliner.h"
#include "ED_screen.h"
#include "ED_select_utils.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "object_intern.h"

/* -------------------------------------------------------------------- */
/** \name Public Object Selection API
 * \{ */

void ED_object_base_select(Base *base, eObjectSelect_Mode mode)
{
  if (mode == BA_INVERT) {
    mode = (base->flag & BASE_SELECTED) != 0 ? BA_DESELECT : BA_SELECT;
  }

  if (base) {
    switch (mode) {
      case BA_SELECT:
        if ((base->flag & BASE_SELECTABLE) != 0) {
          base->flag |= BASE_SELECTED;
        }
        break;
      case BA_DESELECT:
        base->flag &= ~BASE_SELECTED;
        break;
      case BA_INVERT:
        /* Never happens. */
        break;
    }
    BKE_scene_object_base_flag_sync_from_base(base);
  }
}

void ED_object_base_active_refresh(Main *bmain, Scene *scene, ViewLayer *view_layer)
{
  WM_main_add_notifier(NC_SCENE | ND_OB_ACTIVE, scene);
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  struct wmMsgBus *mbus = ((wmWindowManager *)bmain->wm.first)->message_bus;
  if (mbus != NULL) {
    WM_msg_publish_rna_prop(mbus, &scene->id, view_layer, LayerObjects, active);
  }
}

void ED_object_base_activate(bContext *C, Base *base)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  view_layer->basact = base;
  ED_object_base_active_refresh(CTX_data_main(C), scene, view_layer);
}

void ED_object_base_activate_with_mode_exit_if_needed(bContext *C, Base *base)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);

  /* Currently we only need to be concerned with edit-mode. */
  Object *obedit = OBEDIT_FROM_VIEW_LAYER(view_layer);
  if (obedit) {
    Object *ob = base->object;
    if (((ob->mode & OB_MODE_EDIT) == 0) || (obedit->type != ob->type)) {
      Main *bmain = CTX_data_main(C);
      Scene *scene = CTX_data_scene(C);
      ED_object_editmode_exit_multi_ex(bmain, scene, view_layer, EM_FREEDATA);
    }
  }
  ED_object_base_activate(C, base);
}

bool ED_object_base_deselect_all_ex(ViewLayer *view_layer,
                                    View3D *v3d,
                                    int action,
                                    bool *r_any_visible)
{
  if (action == SEL_TOGGLE) {
    action = SEL_SELECT;
    FOREACH_VISIBLE_BASE_BEGIN (view_layer, v3d, base) {
      if (v3d && ((v3d->object_type_exclude_select & (1 << base->object->type)) != 0)) {
        continue;
      }
      if ((base->flag & BASE_SELECTED) != 0) {
        action = SEL_DESELECT;
        break;
      }
    }
    FOREACH_VISIBLE_BASE_END;
  }

  bool any_visible = false;
  bool changed = false;
  FOREACH_VISIBLE_BASE_BEGIN (view_layer, v3d, base) {
    if (v3d && ((v3d->object_type_exclude_select & (1 << base->object->type)) != 0)) {
      continue;
    }
    switch (action) {
      case SEL_SELECT:
        if ((base->flag & BASE_SELECTED) == 0) {
          ED_object_base_select(base, BA_SELECT);
          changed = true;
        }
        break;
      case SEL_DESELECT:
        if ((base->flag & BASE_SELECTED) != 0) {
          ED_object_base_select(base, BA_DESELECT);
          changed = true;
        }
        break;
      case SEL_INVERT:
        if ((base->flag & BASE_SELECTED) != 0) {
          ED_object_base_select(base, BA_DESELECT);
          changed = true;
        }
        else {
          ED_object_base_select(base, BA_SELECT);
          changed = true;
        }
        break;
    }
    any_visible = true;
  }
  FOREACH_VISIBLE_BASE_END;
  if (r_any_visible) {
    *r_any_visible = any_visible;
  }
  return changed;
}

bool ED_object_base_deselect_all(ViewLayer *view_layer, View3D *v3d, int action)
{
  return ED_object_base_deselect_all_ex(view_layer, v3d, action, NULL);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Jump To Object Utilities
 * \{ */

static int get_base_select_priority(Base *base)
{
  if (base->flag & BASE_VISIBLE_DEPSGRAPH) {
    if (base->flag & BASE_SELECTABLE) {
      return 3;
    }
    return 2;
  }
  return 1;
}

Base *ED_object_find_first_by_data_id(ViewLayer *view_layer, ID *id)
{
  BLI_assert(OB_DATA_SUPPORT_ID(GS(id->name)));

  /* Try active object. */
  Base *basact = view_layer->basact;

  if (basact && basact->object && basact->object->data == id) {
    return basact;
  }

  /* Try all objects. */
  Base *base_best = NULL;
  int priority_best = 0;

  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    if (base->object && base->object->data == id) {
      if (base->flag & BASE_SELECTED) {
        return base;
      }

      int priority_test = get_base_select_priority(base);

      if (priority_test > priority_best) {
        priority_best = priority_test;
        base_best = base;
      }
    }
  }

  return base_best;
}

bool ED_object_jump_to_object(bContext *C, Object *ob, const bool UNUSED(reveal_hidden))
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  Base *base = BKE_view_layer_base_find(view_layer, ob);

  if (base == NULL) {
    return false;
  }

  /* TODO: use 'reveal_hidden', as is done with bones. */

  if (view_layer->basact != base || !(base->flag & BASE_SELECTED)) {
    /* Select if not selected. */
    if (!(base->flag & BASE_SELECTED)) {
      ED_object_base_deselect_all(view_layer, v3d, SEL_DESELECT);

      if (BASE_VISIBLE(v3d, base)) {
        ED_object_base_select(base, BA_SELECT);
      }

      WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, CTX_data_scene(C));
    }

    /* Make active if not active. */
    ED_object_base_activate(C, base);
  }

  return true;
}

bool ED_object_jump_to_bone(bContext *C,
                            Object *ob,
                            const char *bone_name,
                            const bool reveal_hidden)
{
  /* Verify it's a valid armature object. */
  if (ob == NULL || ob->type != OB_ARMATURE) {
    return false;
  }

  bArmature *arm = ob->data;

  /* Activate the armature object. */
  if (!ED_object_jump_to_object(C, ob, reveal_hidden)) {
    return false;
  }

  /* Switch to pose mode from object mode. */
  if (!ELEM(ob->mode, OB_MODE_EDIT, OB_MODE_POSE)) {
    ED_object_mode_set(C, OB_MODE_POSE);
  }

  if (ob->mode == OB_MODE_EDIT && arm->edbo != NULL) {
    /* In Edit mode select and activate the target Edit-Bone. */
    EditBone *ebone = ED_armature_ebone_find_name(arm->edbo, bone_name);
    if (ebone != NULL) {
      if (reveal_hidden) {
        /* Unhide the bone. */
        ebone->flag &= ~BONE_HIDDEN_A;

        if ((arm->layer & ebone->layer) == 0) {
          arm->layer |= 1U << bitscan_forward_uint(ebone->layer);
        }
      }

      /* Select it. */
      ED_armature_edit_deselect_all(ob);

      if (EBONE_SELECTABLE(arm, ebone)) {
        ED_armature_ebone_select_set(ebone, true);
        ED_armature_edit_sync_selection(arm->edbo);
      }

      arm->act_edbone = ebone;

      ED_pose_bone_select_tag_update(ob);
      return true;
    }
  }
  else if (ob->mode == OB_MODE_POSE && ob->pose != NULL) {
    /* In Pose mode select and activate the target Bone/Pose-Channel. */
    bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose, bone_name);
    if (pchan != NULL) {
      if (reveal_hidden) {
        /* Unhide the bone. */
        pchan->bone->flag &= ~BONE_HIDDEN_P;

        if ((arm->layer & pchan->bone->layer) == 0) {
          arm->layer |= 1U << bitscan_forward_uint(pchan->bone->layer);
        }
      }

      /* Select it. */
      ED_pose_deselect_all(ob, SEL_DESELECT, true);
      ED_pose_bone_select(ob, pchan, true);

      arm->act_bone = pchan->bone;

      ED_pose_bone_select_tag_update(ob);
      return true;
    }
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Operator Utils
 * \{ */

static bool objects_selectable_poll(bContext *C)
{
  /* we don't check for linked scenes here, selection is
   * still allowed then for inspection of scene */
  Object *obact = CTX_data_active_object(C);

  if (CTX_data_edit_object(C)) {
    return 0;
  }
  if (obact && obact->mode) {
    return 0;
  }

  return 1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select by Type
 * \{ */

static int object_select_by_type_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  short obtype, extend;

  obtype = RNA_enum_get(op->ptr, "type");
  extend = RNA_boolean_get(op->ptr, "extend");

  if (extend == 0) {
    ED_object_base_deselect_all(view_layer, v3d, SEL_DESELECT);
  }

  CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
    if (base->object->type == obtype) {
      ED_object_base_select(base, BA_SELECT);
    }
  }
  CTX_DATA_END;

  Scene *scene = CTX_data_scene(C);
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);

  ED_outliner_select_sync_from_object_tag(C);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_select_by_type(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select by Type";
  ot->description = "Select all visible objects that are of a type";
  ot->idname = "OBJECT_OT_select_by_type";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = object_select_by_type_exec;
  ot->poll = objects_selectable_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna,
                  "extend",
                  false,
                  "Extend",
                  "Extend selection instead of deselecting everything first");
  ot->prop = RNA_def_enum(ot->srna, "type", rna_enum_object_type_items, 1, "Type", "");
  RNA_def_property_translation_context(ot->prop, BLT_I18NCONTEXT_ID_ID);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Selection by Links
 * \{ */

enum {
  OBJECT_SELECT_LINKED_IPO = 1,
  OBJECT_SELECT_LINKED_OBDATA,
  OBJECT_SELECT_LINKED_MATERIAL,
  OBJECT_SELECT_LINKED_DUPGROUP,
  OBJECT_SELECT_LINKED_PARTICLE,
  OBJECT_SELECT_LINKED_LIBRARY,
  OBJECT_SELECT_LINKED_LIBRARY_OBDATA,
};

static const EnumPropertyItem prop_select_linked_types[] = {
    /* XXX deprecated animation system stuff. */
    // {OBJECT_SELECT_LINKED_IPO, "IPO", 0, "Object IPO", ""},
    {OBJECT_SELECT_LINKED_OBDATA, "OBDATA", 0, "Object Data", ""},
    {OBJECT_SELECT_LINKED_MATERIAL, "MATERIAL", 0, "Material", ""},
    {OBJECT_SELECT_LINKED_DUPGROUP, "DUPGROUP", 0, "Instanced Collection", ""},
    {OBJECT_SELECT_LINKED_PARTICLE, "PARTICLE", 0, "Particle System", ""},
    {OBJECT_SELECT_LINKED_LIBRARY, "LIBRARY", 0, "Library", ""},
    {OBJECT_SELECT_LINKED_LIBRARY_OBDATA, "LIBRARY_OBDATA", 0, "Library (Object Data)", ""},
    {0, NULL, 0, NULL, NULL},
};

static bool object_select_all_by_obdata(bContext *C, void *obdata)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
    if (((base->flag & BASE_SELECTED) == 0) && ((base->flag & BASE_SELECTABLE) != 0)) {
      if (base->object->data == obdata) {
        ED_object_base_select(base, BA_SELECT);
        changed = true;
      }
    }
  }
  CTX_DATA_END;

  return changed;
}

static bool object_select_all_by_material(bContext *C, Material *mat)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
    if (((base->flag & BASE_SELECTED) == 0) && ((base->flag & BASE_SELECTABLE) != 0)) {
      Object *ob = base->object;
      Material *mat1;
      int a;

      for (a = 1; a <= ob->totcol; a++) {
        mat1 = BKE_object_material_get(ob, a);

        if (mat1 == mat) {
          ED_object_base_select(base, BA_SELECT);
          changed = true;
        }
      }
    }
  }
  CTX_DATA_END;

  return changed;
}

static bool object_select_all_by_instance_collection(bContext *C, Object *ob)
{
  bool changed = false;
  Collection *instance_collection = (ob->transflag & OB_DUPLICOLLECTION) ?
                                        ob->instance_collection :
                                        NULL;

  CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
    if (((base->flag & BASE_SELECTED) == 0) && ((base->flag & BASE_SELECTABLE) != 0)) {
      Collection *instance_collection_other = (base->object->transflag & OB_DUPLICOLLECTION) ?
                                                  base->object->instance_collection :
                                                  NULL;
      if (instance_collection == instance_collection_other) {
        ED_object_base_select(base, BA_SELECT);
        changed = true;
      }
    }
  }
  CTX_DATA_END;

  return changed;
}

static bool object_select_all_by_particle(bContext *C, Object *ob)
{
  ParticleSystem *psys_act = psys_get_current(ob);
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
    if (((base->flag & BASE_SELECTED) == 0) && ((base->flag & BASE_SELECTABLE) != 0)) {
      /* Loop through other particles. */
      ParticleSystem *psys;

      for (psys = base->object->particlesystem.first; psys; psys = psys->next) {
        if (psys->part == psys_act->part) {
          ED_object_base_select(base, BA_SELECT);
          changed = true;
          break;
        }

        if (base->flag & BASE_SELECTED) {
          break;
        }
      }
    }
  }
  CTX_DATA_END;

  return changed;
}

static bool object_select_all_by_library(bContext *C, Library *lib)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
    if (((base->flag & BASE_SELECTED) == 0) && ((base->flag & BASE_SELECTABLE) != 0)) {
      if (lib == base->object->id.lib) {
        ED_object_base_select(base, BA_SELECT);
        changed = true;
      }
    }
  }
  CTX_DATA_END;

  return changed;
}

static bool object_select_all_by_library_obdata(bContext *C, Library *lib)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
    if (((base->flag & BASE_SELECTED) == 0) && ((base->flag & BASE_SELECTABLE) != 0)) {
      if (base->object->data && lib == ((ID *)base->object->data)->lib) {
        ED_object_base_select(base, BA_SELECT);
        changed = true;
      }
    }
  }
  CTX_DATA_END;

  return changed;
}

void ED_object_select_linked_by_id(bContext *C, ID *id)
{
  int idtype = GS(id->name);
  bool changed = false;

  if (OB_DATA_SUPPORT_ID(idtype)) {
    changed = object_select_all_by_obdata(C, id);
  }
  else if (idtype == ID_MA) {
    changed = object_select_all_by_material(C, (Material *)id);
  }
  else if (idtype == ID_LI) {
    changed = object_select_all_by_library(C, (Library *)id);
  }

  if (changed) {
    Scene *scene = CTX_data_scene(C);
    DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  }
}

static int object_select_linked_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  Object *ob;
  int nr = RNA_enum_get(op->ptr, "type");
  bool changed = false, extend;

  extend = RNA_boolean_get(op->ptr, "extend");

  if (extend == 0) {
    ED_object_base_deselect_all(view_layer, v3d, SEL_DESELECT);
  }

  ob = OBACT(view_layer);
  if (ob == NULL) {
    BKE_report(op->reports, RPT_ERROR, "No active object");
    return OPERATOR_CANCELLED;
  }

  if (nr == OBJECT_SELECT_LINKED_IPO) {
    /* XXX old animation system */
    // if (ob->ipo == 0) return OPERATOR_CANCELLED;
    // object_select_all_by_ipo(C, ob->ipo)
    return OPERATOR_CANCELLED;
  }
  if (nr == OBJECT_SELECT_LINKED_OBDATA) {
    if (ob->data == NULL) {
      return OPERATOR_CANCELLED;
    }

    changed = object_select_all_by_obdata(C, ob->data);
  }
  else if (nr == OBJECT_SELECT_LINKED_MATERIAL) {
    Material *mat = NULL;

    mat = BKE_object_material_get(ob, ob->actcol);
    if (mat == NULL) {
      return OPERATOR_CANCELLED;
    }

    changed = object_select_all_by_material(C, mat);
  }
  else if (nr == OBJECT_SELECT_LINKED_DUPGROUP) {
    if (ob->instance_collection == NULL) {
      return OPERATOR_CANCELLED;
    }

    changed = object_select_all_by_instance_collection(C, ob);
  }
  else if (nr == OBJECT_SELECT_LINKED_PARTICLE) {
    if (BLI_listbase_is_empty(&ob->particlesystem)) {
      return OPERATOR_CANCELLED;
    }

    changed = object_select_all_by_particle(C, ob);
  }
  else if (nr == OBJECT_SELECT_LINKED_LIBRARY) {
    /* do nothing */
    changed = object_select_all_by_library(C, ob->id.lib);
  }
  else if (nr == OBJECT_SELECT_LINKED_LIBRARY_OBDATA) {
    if (ob->data == NULL) {
      return OPERATOR_CANCELLED;
    }

    changed = object_select_all_by_library_obdata(C, ((ID *)ob->data)->lib);
  }
  else {
    return OPERATOR_CANCELLED;
  }

  if (changed) {
    DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
    ED_outliner_select_sync_from_object_tag(C);
    return OPERATOR_FINISHED;
  }

  return OPERATOR_CANCELLED;
}

void OBJECT_OT_select_linked(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Linked";
  ot->description = "Select all visible objects that are linked";
  ot->idname = "OBJECT_OT_select_linked";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = object_select_linked_exec;
  ot->poll = objects_selectable_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna,
                  "extend",
                  false,
                  "Extend",
                  "Extend selection instead of deselecting everything first");
  ot->prop = RNA_def_enum(ot->srna, "type", prop_select_linked_types, 0, "Type", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Selected Grouped
 * \{ */

enum {
  OBJECT_GRPSEL_CHILDREN_RECURSIVE = 0,
  OBJECT_GRPSEL_CHILDREN = 1,
  OBJECT_GRPSEL_PARENT = 2,
  OBJECT_GRPSEL_SIBLINGS = 3,
  OBJECT_GRPSEL_TYPE = 4,
  OBJECT_GRPSEL_COLLECTION = 5,
  OBJECT_GRPSEL_HOOK = 7,
  OBJECT_GRPSEL_PASS = 8,
  OBJECT_GRPSEL_COLOR = 9,
  OBJECT_GRPSEL_KEYINGSET = 10,
  OBJECT_GRPSEL_LIGHT_TYPE = 11,
  OBJECT_GRPSEL_PROPERTIES = 12,
};

static const EnumPropertyItem prop_select_grouped_types[] = {
    {OBJECT_GRPSEL_CHILDREN_RECURSIVE, "CHILDREN_RECURSIVE", 0, "Children", ""},
    {OBJECT_GRPSEL_CHILDREN, "CHILDREN", 0, "Immediate Children", ""},
    {OBJECT_GRPSEL_PARENT, "PARENT", 0, "Parent", ""},
    {OBJECT_GRPSEL_SIBLINGS, "SIBLINGS", 0, "Siblings", "Shared parent"},
    {OBJECT_GRPSEL_TYPE, "TYPE", 0, "Type", "Shared object type"},
    {OBJECT_GRPSEL_COLLECTION, "COLLECTION", 0, "Collection", "Shared collection"},
    {OBJECT_GRPSEL_HOOK, "HOOK", 0, "Hook", ""},
    {OBJECT_GRPSEL_PASS, "PASS", 0, "Pass", "Render pass index"},
    {OBJECT_GRPSEL_COLOR, "COLOR", 0, "Color", "Object color"},
    {OBJECT_GRPSEL_PROPERTIES, "PROPERTIES", 0, "Properties", "Game Properties"},
    {OBJECT_GRPSEL_KEYINGSET,
     "KEYINGSET",
     0,
     "Keying Set",
     "Objects included in active Keying Set"},
    {OBJECT_GRPSEL_LIGHT_TYPE, "LIGHT_TYPE", 0, "Light Type", "Matching light types"},
    {0, NULL, 0, NULL, NULL},
};

static bool select_grouped_children(bContext *C, Object *ob, const bool recursive)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    if (ob == base->object->parent) {
      if ((base->flag & BASE_SELECTED) == 0) {
        ED_object_base_select(base, BA_SELECT);
        changed = true;
      }

      if (recursive) {
        changed |= select_grouped_children(C, base->object, 1);
      }
    }
  }
  CTX_DATA_END;
  return changed;
}

static bool select_grouped_parent(bContext *C) /* Makes parent active and de-selected OBACT */
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  Base *baspar, *basact = CTX_data_active_base(C);
  bool changed = false;

  if (!basact || !(basact->object->parent)) {
    return 0; /* we know OBACT is valid */
  }

  baspar = BKE_view_layer_base_find(view_layer, basact->object->parent);

  /* can be NULL if parent in other scene */
  if (baspar && BASE_SELECTABLE(v3d, baspar)) {
    ED_object_base_select(baspar, BA_SELECT);
    ED_object_base_activate(C, baspar);
    changed = true;
  }
  return changed;
}

#define COLLECTION_MENU_MAX 24
/* Select objects in the same group as the active */
static bool select_grouped_collection(bContext *C, Object *ob)
{
  Main *bmain = CTX_data_main(C);
  bool changed = false;
  Collection *collection, *ob_collections[COLLECTION_MENU_MAX];
  int collection_count = 0, i;
  uiPopupMenu *pup;
  uiLayout *layout;

  for (collection = bmain->collections.first;
       collection && (collection_count < COLLECTION_MENU_MAX);
       collection = collection->id.next) {
    if (BKE_collection_has_object(collection, ob)) {
      ob_collections[collection_count] = collection;
      collection_count++;
    }
  }

  if (!collection_count) {
    return 0;
  }
  if (collection_count == 1) {
    collection = ob_collections[0];
    CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
      if (((base->flag & BASE_SELECTED) == 0) && ((base->flag & BASE_SELECTABLE) != 0)) {
        if (BKE_collection_has_object(collection, base->object)) {
          ED_object_base_select(base, BA_SELECT);
          changed = true;
        }
      }
    }
    CTX_DATA_END;
    return changed;
  }

  /* build the menu. */
  pup = UI_popup_menu_begin(C, IFACE_("Select Collection"), ICON_NONE);
  layout = UI_popup_menu_layout(pup);

  for (i = 0; i < collection_count; i++) {
    collection = ob_collections[i];
    uiItemStringO(layout,
                  collection->id.name + 2,
                  0,
                  "OBJECT_OT_select_same_collection",
                  "collection",
                  collection->id.name + 2);
  }

  UI_popup_menu_end(C, pup);
  return changed; /* The operator already handle this! */
}

static bool select_grouped_object_hooks(bContext *C, Object *ob)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);

  bool changed = false;
  Base *base;
  ModifierData *md;
  HookModifierData *hmd;

  for (md = ob->modifiers.first; md; md = md->next) {
    if (md->type == eModifierType_Hook) {
      hmd = (HookModifierData *)md;
      if (hmd->object) {
        base = BKE_view_layer_base_find(view_layer, hmd->object);
        if (base && ((base->flag & BASE_SELECTED) == 0) && (BASE_SELECTABLE(v3d, base))) {
          ED_object_base_select(base, BA_SELECT);
          changed = true;
        }
      }
    }
  }
  return changed;
}

/* Select objects with the same parent as the active (siblings),
 * parent can be NULL also */
static bool select_grouped_siblings(bContext *C, Object *ob)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    if ((base->object->parent == ob->parent) && ((base->flag & BASE_SELECTED) == 0)) {
      ED_object_base_select(base, BA_SELECT);
      changed = true;
    }
  }
  CTX_DATA_END;
  return changed;
}
static bool select_grouped_lighttype(bContext *C, Object *ob)
{
  Light *la = ob->data;

  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    if (base->object->type == OB_LAMP) {
      Light *la_test = base->object->data;
      if ((la->type == la_test->type) && ((base->flag & BASE_SELECTED) == 0)) {
        ED_object_base_select(base, BA_SELECT);
        changed = true;
      }
    }
  }
  CTX_DATA_END;
  return changed;
}
static bool select_grouped_type(bContext *C, Object *ob)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    if ((base->object->type == ob->type) && ((base->flag & BASE_SELECTED) == 0)) {
      ED_object_base_select(base, BA_SELECT);
      changed = true;
    }
  }
  CTX_DATA_END;
  return changed;
}

static bool select_grouped_index_object(bContext *C, Object *ob)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    if ((base->object->index == ob->index) && ((base->flag & BASE_SELECTED) == 0)) {
      ED_object_base_select(base, BA_SELECT);
      changed = true;
    }
  }
  CTX_DATA_END;
  return changed;
}

static bool select_grouped_color(bContext *C, Object *ob)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    if (((base->flag & BASE_SELECTED) == 0) &&
        (compare_v3v3(base->object->color, ob->color, 0.005f))) {
      ED_object_base_select(base, BA_SELECT);
      changed = true;
    }
  }
  CTX_DATA_END;
  return changed;
}

static bool objects_share_gameprop(Object *a, Object *b)
{
  bProperty *prop;

  for (prop = a->prop.first; prop; prop = prop->next) {
    if (BKE_bproperty_object_get(b, prop->name)) {
      return 1;
    }
  }
  return 0;
}

static bool select_grouped_gameprops(bContext *C, Object *ob)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    if (((base->flag & BASE_SELECTED) == 0) && (objects_share_gameprop(base->object, ob))) {
      ED_object_base_select(base, BA_SELECT);
      changed = true;
    }
  }
  CTX_DATA_END;
  return changed;
}

static bool select_grouped_keyingset(bContext *C, Object *UNUSED(ob), ReportList *reports)
{
  KeyingSet *ks = ANIM_scene_get_active_keyingset(CTX_data_scene(C));
  bool changed = false;

  /* firstly, validate KeyingSet */
  if (ks == NULL) {
    BKE_report(reports, RPT_ERROR, "No active Keying Set to use");
    return false;
  }
  if (ANIM_validate_keyingset(C, NULL, ks) != 0) {
    if (ks->paths.first == NULL) {
      if ((ks->flag & KEYINGSET_ABSOLUTE) == 0) {
        BKE_report(reports,
                   RPT_ERROR,
                   "Use another Keying Set, as the active one depends on the currently "
                   "selected objects or cannot find any targets due to unsuitable context");
      }
      else {
        BKE_report(reports, RPT_ERROR, "Keying Set does not contain any paths");
      }
    }
    return false;
  }

  /* select each object that Keying Set refers to */
  /* TODO: perhaps to be more in line with the rest of these, we should only take objects
   * if the passed in object is included in this too */
  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    /* only check for this object if it isn't selected already, to limit time wasted */
    if ((base->flag & BASE_SELECTED) == 0) {
      KS_Path *ksp;

      /* this is the slow way... we could end up with > 500 items here,
       * with none matching, but end up doing this on 1000 objects...
       */
      for (ksp = ks->paths.first; ksp; ksp = ksp->next) {
        /* if id matches, select then stop looping (match found) */
        if (ksp->id == (ID *)base->object) {
          ED_object_base_select(base, BA_SELECT);
          changed = true;
          break;
        }
      }
    }
  }
  CTX_DATA_END;

  return changed;
}

static int object_select_grouped_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  Object *ob;
  const int type = RNA_enum_get(op->ptr, "type");
  bool changed = false, extend;

  extend = RNA_boolean_get(op->ptr, "extend");

  if (extend == 0) {
    changed = ED_object_base_deselect_all(view_layer, v3d, SEL_DESELECT);
  }

  ob = OBACT(view_layer);
  if (ob == NULL) {
    BKE_report(op->reports, RPT_ERROR, "No active object");
    return OPERATOR_CANCELLED;
  }

  switch (type) {
    case OBJECT_GRPSEL_CHILDREN_RECURSIVE:
      changed |= select_grouped_children(C, ob, true);
      break;
    case OBJECT_GRPSEL_CHILDREN:
      changed |= select_grouped_children(C, ob, false);
      break;
    case OBJECT_GRPSEL_PARENT:
      changed |= select_grouped_parent(C);
      break;
    case OBJECT_GRPSEL_SIBLINGS:
      changed |= select_grouped_siblings(C, ob);
      break;
    case OBJECT_GRPSEL_TYPE:
      changed |= select_grouped_type(C, ob);
      break;
    case OBJECT_GRPSEL_COLLECTION:
      changed |= select_grouped_collection(C, ob);
      break;
    case OBJECT_GRPSEL_HOOK:
      changed |= select_grouped_object_hooks(C, ob);
      break;
    case OBJECT_GRPSEL_PASS:
      changed |= select_grouped_index_object(C, ob);
      break;
    case OBJECT_GRPSEL_COLOR:
      changed |= select_grouped_color(C, ob);
      break;
    case OBJECT_GRPSEL_PROPERTIES:
      changed |= select_grouped_gameprops(C, ob);
      break;
    case OBJECT_GRPSEL_KEYINGSET:
      changed |= select_grouped_keyingset(C, ob, op->reports);
      break;
    case OBJECT_GRPSEL_LIGHT_TYPE:
      if (ob->type != OB_LAMP) {
        BKE_report(op->reports, RPT_ERROR, "Active object must be a light");
        break;
      }
      changed |= select_grouped_lighttype(C, ob);
      break;
    default:
      break;
  }

  if (changed) {
    DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
    ED_outliner_select_sync_from_object_tag(C);
    return OPERATOR_FINISHED;
  }

  return OPERATOR_CANCELLED;
}

void OBJECT_OT_select_grouped(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Grouped";
  ot->description = "Select all visible objects grouped by various properties";
  ot->idname = "OBJECT_OT_select_grouped";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = object_select_grouped_exec;
  ot->poll = objects_selectable_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna,
                  "extend",
                  false,
                  "Extend",
                  "Extend selection instead of deselecting everything first");
  ot->prop = RNA_def_enum(ot->srna, "type", prop_select_grouped_types, 0, "Type", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name (De)select All
 * \{ */

static int object_select_all_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  int action = RNA_enum_get(op->ptr, "action");
  bool any_visible = false;

  bool changed = ED_object_base_deselect_all_ex(view_layer, v3d, action, &any_visible);

  if (changed) {
    Scene *scene = CTX_data_scene(C);
    DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);

    ED_outliner_select_sync_from_object_tag(C);

    return OPERATOR_FINISHED;
  }
  if (any_visible == false) {
    /* TODO(@campbellbarton): Looks like we could remove this,
     * if not comment should say why its needed. */
    return OPERATOR_PASS_THROUGH;
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_select_all(wmOperatorType *ot)
{

  /* identifiers */
  ot->name = "(De)select All";
  ot->description = "Change selection of all visible objects in scene";
  ot->idname = "OBJECT_OT_select_all";

  /* api callbacks */
  ot->exec = object_select_all_exec;
  ot->poll = objects_selectable_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  WM_operator_properties_select_all(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select In The Same Collection
 * \{ */

static int object_select_same_collection_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Collection *collection;
  char collection_name[MAX_ID_NAME];

  /* passthrough if no objects are visible */
  if (CTX_DATA_COUNT(C, visible_bases) == 0) {
    return OPERATOR_PASS_THROUGH;
  }

  RNA_string_get(op->ptr, "collection", collection_name);

  collection = (Collection *)BKE_libblock_find_name(bmain, ID_GR, collection_name);

  if (!collection) {
    return OPERATOR_PASS_THROUGH;
  }

  CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
    if (((base->flag & BASE_SELECTED) == 0) && ((base->flag & BASE_SELECTABLE) != 0)) {
      if (BKE_collection_has_object(collection, base->object)) {
        ED_object_base_select(base, BA_SELECT);
      }
    }
  }
  CTX_DATA_END;

  Scene *scene = CTX_data_scene(C);
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);

  ED_outliner_select_sync_from_object_tag(C);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_select_same_collection(wmOperatorType *ot)
{

  /* identifiers */
  ot->name = "Select Same Collection";
  ot->description = "Select object in the same collection";
  ot->idname = "OBJECT_OT_select_same_collection";

  /* api callbacks */
  ot->exec = object_select_same_collection_exec;
  ot->poll = objects_selectable_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_string(
      ot->srna, "collection", NULL, MAX_ID_NAME, "Collection", "Name of the collection to select");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Mirror
 * \{ */

static int object_select_mirror_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  bool extend;

  extend = RNA_boolean_get(op->ptr, "extend");

  CTX_DATA_BEGIN (C, Base *, primbase, selected_bases) {
    char name_flip[MAXBONENAME];

    BLI_string_flip_side_name(name_flip, primbase->object->id.name + 2, true, sizeof(name_flip));

    if (!STREQ(name_flip, primbase->object->id.name + 2)) {
      Object *ob = (Object *)BKE_libblock_find_name(bmain, ID_OB, name_flip);
      if (ob) {
        Base *secbase = BKE_view_layer_base_find(view_layer, ob);

        if (secbase) {
          ED_object_base_select(secbase, BA_SELECT);
        }
      }
    }

    if (extend == false) {
      ED_object_base_select(primbase, BA_DESELECT);
    }
  }
  CTX_DATA_END;

  /* undo? */
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);

  ED_outliner_select_sync_from_object_tag(C);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_select_mirror(wmOperatorType *ot)
{

  /* identifiers */
  ot->name = "Select Mirror";
  ot->description =
      "Select the mirror objects of the selected object e.g. \"L.sword\" and \"R.sword\"";
  ot->idname = "OBJECT_OT_select_mirror";

  /* api callbacks */
  ot->exec = object_select_mirror_exec;
  ot->poll = objects_selectable_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "extend", 0, "Extend", "Extend selection instead of deselecting everything first");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select More/Less
 * \{ */

static bool object_select_more_less(bContext *C, const bool select)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);

  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    Object *ob = base->object;
    ob->flag &= ~OB_DONE;
    ob->id.tag &= ~LIB_TAG_DOIT;
    /* parent may be in another scene */
    if (ob->parent) {
      ob->parent->flag &= ~OB_DONE;
      ob->parent->id.tag &= ~LIB_TAG_DOIT;
    }
  }

  ListBase ctx_base_list;
  CollectionPointerLink *ctx_base;
  CTX_data_selectable_bases(C, &ctx_base_list);

  CTX_DATA_BEGIN (C, Object *, ob, selected_objects) {
    ob->flag |= OB_DONE;
  }
  CTX_DATA_END;

  for (ctx_base = ctx_base_list.first; ctx_base; ctx_base = ctx_base->next) {
    Object *ob = ((Base *)ctx_base->ptr.data)->object;
    if (ob->parent) {
      if ((ob->flag & OB_DONE) != (ob->parent->flag & OB_DONE)) {
        ob->id.tag |= LIB_TAG_DOIT;
        ob->parent->id.tag |= LIB_TAG_DOIT;
      }
    }
  }

  bool changed = false;
  const short select_mode = select ? BA_SELECT : BA_DESELECT;
  const short select_flag = select ? BASE_SELECTED : 0;

  for (ctx_base = ctx_base_list.first; ctx_base; ctx_base = ctx_base->next) {
    Base *base = ctx_base->ptr.data;
    Object *ob = base->object;
    if ((ob->id.tag & LIB_TAG_DOIT) && ((base->flag & BASE_SELECTED) != select_flag)) {
      ED_object_base_select(base, select_mode);
      changed = true;
    }
  }

  BLI_freelistN(&ctx_base_list);

  return changed;
}

static int object_select_more_exec(bContext *C, wmOperator *UNUSED(op))
{
  bool changed = object_select_more_less(C, true);

  if (changed) {
    Scene *scene = CTX_data_scene(C);
    DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);

    ED_outliner_select_sync_from_object_tag(C);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_select_more(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select More";
  ot->idname = "OBJECT_OT_select_more";
  ot->description = "Select connected parent/child objects";

  /* api callbacks */
  ot->exec = object_select_more_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int object_select_less_exec(bContext *C, wmOperator *UNUSED(op))
{
  bool changed = object_select_more_less(C, false);

  if (changed) {
    Scene *scene = CTX_data_scene(C);
    DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);

    ED_outliner_select_sync_from_object_tag(C);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_select_less(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Less";
  ot->idname = "OBJECT_OT_select_less";
  ot->description = "Deselect objects at the boundaries of parent/child relationships";

  /* api callbacks */
  ot->exec = object_select_less_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Random
 * \{ */

static int object_select_random_exec(bContext *C, wmOperator *op)
{
  const bool select = (RNA_enum_get(op->ptr, "action") == SEL_SELECT);
  const float randfac = RNA_float_get(op->ptr, "ratio");
  const int seed = WM_operator_properties_select_random_seed_increment_get(op);

  ListBase ctx_data_list;
  CTX_data_selectable_bases(C, &ctx_data_list);
  const int tot = BLI_listbase_count(&ctx_data_list);
  int elem_map_len = 0;
  Base **elem_map = MEM_mallocN(sizeof(*elem_map) * tot, __func__);

  CollectionPointerLink *ctx_link;
  for (ctx_link = ctx_data_list.first; ctx_link; ctx_link = ctx_link->next) {
    elem_map[elem_map_len++] = ctx_link->ptr.data;
  }
  BLI_freelistN(&ctx_data_list);

  BLI_array_randomize(elem_map, sizeof(*elem_map), elem_map_len, seed);
  const int count_select = elem_map_len * randfac;
  for (int i = 0; i < count_select; i++) {
    ED_object_base_select(elem_map[i], select);
  }
  MEM_freeN(elem_map);

  Scene *scene = CTX_data_scene(C);
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);

  ED_outliner_select_sync_from_object_tag(C);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_select_random(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Random";
  ot->description = "Set select on random visible objects";
  ot->idname = "OBJECT_OT_select_random";

  /* api callbacks */
  // ot->invoke = object_select_random_invoke; /* TODO: need a number popup. */
  ot->exec = object_select_random_exec;
  ot->poll = objects_selectable_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  WM_operator_properties_select_random(ot);
}

/** \} */
