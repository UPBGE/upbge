/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edobj
 */

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_collection_types.h"
#include "DNA_light_types.h"
#include "DNA_modifier_types.h"
#include "DNA_property_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_rand.h"
#include "BLI_string_utils.hh"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_particle.h"
#include "BKE_property.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_types.hh"

#include "ED_armature.hh"
#include "ED_keyframing.hh"
#include "ED_object.hh"
#include "ED_outliner.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"

#include "ANIM_armature.hh"
#include "ANIM_bone_collections.hh"
#include "ANIM_keyingsets.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "object_intern.hh"

namespace blender::ed::object {

/* -------------------------------------------------------------------- */
/** \name Public Object Selection API
 * \{ */

void base_select(Base *base, eObjectSelect_Mode mode)
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

void base_active_refresh(Main *bmain, Scene *scene, ViewLayer *view_layer)
{
  WM_main_add_notifier(NC_SCENE | ND_OB_ACTIVE, scene);
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  wmMsgBus *mbus = ((wmWindowManager *)bmain->wm.first)->message_bus;
  if (mbus != nullptr) {
    WM_msg_publish_rna_prop(mbus, &scene->id, view_layer, LayerObjects, active);
  }
}

void base_activate(bContext *C, Base *base)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  view_layer->basact = base;
  base_active_refresh(CTX_data_main(C), scene, view_layer);
}

void base_activate_with_mode_exit_if_needed(bContext *C, Base *base)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  /* Currently we only need to be concerned with edit-mode. */
  BKE_view_layer_synced_ensure(scene, view_layer);
  Object *obedit = BKE_view_layer_edit_object_get(view_layer);
  if (obedit) {
    Object *ob = base->object;
    if (((ob->mode & OB_MODE_EDIT) == 0) || (obedit->type != ob->type)) {
      Main *bmain = CTX_data_main(C);
      editmode_exit_multi_ex(bmain, scene, view_layer, EM_FREEDATA);
    }
  }
  base_activate(C, base);
}

bool base_deselect_all_ex(
    const Scene *scene, ViewLayer *view_layer, View3D *v3d, int action, bool *r_any_visible)
{
  if (action == SEL_TOGGLE) {
    action = SEL_SELECT;
    FOREACH_VISIBLE_BASE_BEGIN (scene, view_layer, v3d, base) {
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
  FOREACH_VISIBLE_BASE_BEGIN (scene, view_layer, v3d, base) {
    if (v3d && ((v3d->object_type_exclude_select & (1 << base->object->type)) != 0)) {
      continue;
    }
    switch (action) {
      case SEL_SELECT:
        if ((base->flag & BASE_SELECTED) == 0) {
          base_select(base, BA_SELECT);
          changed = true;
        }
        break;
      case SEL_DESELECT:
        if ((base->flag & BASE_SELECTED) != 0) {
          base_select(base, BA_DESELECT);
          changed = true;
        }
        break;
      case SEL_INVERT:
        if ((base->flag & BASE_SELECTED) != 0) {
          base_select(base, BA_DESELECT);
          changed = true;
        }
        else {
          base_select(base, BA_SELECT);
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

bool base_deselect_all(const Scene *scene, ViewLayer *view_layer, View3D *v3d, int action)
{
  return base_deselect_all_ex(scene, view_layer, v3d, action, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Jump To Object Utilities
 * \{ */

static int get_base_select_priority(Base *base)
{
  if (base->flag & BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT) {
    if (base->flag & BASE_SELECTABLE) {
      return 3;
    }
    return 2;
  }
  return 1;
}

Base *find_first_by_data_id(const Scene *scene, ViewLayer *view_layer, ID *id)
{
  BLI_assert(OB_DATA_SUPPORT_ID(GS(id->name)));

  /* Try active object. */
  BKE_view_layer_synced_ensure(scene, view_layer);
  Base *basact = BKE_view_layer_active_base_get(view_layer);

  if (basact && basact->object && basact->object->data == id) {
    return basact;
  }

  /* Try all objects. */
  Base *base_best = nullptr;
  int priority_best = 0;

  LISTBASE_FOREACH (Base *, base, BKE_view_layer_object_bases_get(view_layer)) {
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

bool jump_to_object(bContext *C, Object *ob, const bool /*reveal_hidden*/)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  BKE_view_layer_synced_ensure(scene, view_layer);
  Base *base = BKE_view_layer_base_find(view_layer, ob);

  if (base == nullptr) {
    return false;
  }

  /* TODO: use 'reveal_hidden', as is done with bones. */

  if (BKE_view_layer_active_base_get(view_layer) != base || !(base->flag & BASE_SELECTED)) {
    /* Select if not selected. */
    if (!(base->flag & BASE_SELECTED)) {
      base_deselect_all(scene, view_layer, v3d, SEL_DESELECT);

      if (BASE_VISIBLE(v3d, base)) {
        base_select(base, BA_SELECT);
      }

      WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, CTX_data_scene(C));
    }

    /* Make active if not active. */
    base_activate(C, base);
  }

  return true;
}

bool jump_to_bone(bContext *C, Object *ob, const char *bone_name, const bool reveal_hidden)
{
  /* Verify it's a valid armature object. */
  if (ob == nullptr || ob->type != OB_ARMATURE) {
    return false;
  }

  bArmature *arm = static_cast<bArmature *>(ob->data);

  /* Activate the armature object. */
  if (!jump_to_object(C, ob, reveal_hidden)) {
    return false;
  }

  /* Switch to pose mode from object mode. */
  if (!ELEM(ob->mode, OB_MODE_EDIT, OB_MODE_POSE)) {
    mode_set(C, OB_MODE_POSE);
  }

  if (ob->mode == OB_MODE_EDIT && arm->edbo != nullptr) {
    /* In Edit mode select and activate the target Edit-Bone. */
    EditBone *ebone = ED_armature_ebone_find_name(arm->edbo, bone_name);
    if (ebone != nullptr) {
      if (reveal_hidden) {
        /* Unhide the bone. */
        ebone->flag &= ~BONE_HIDDEN_A;
        ANIM_armature_bonecoll_show_from_ebone(arm, ebone);
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
  else if (ob->mode == OB_MODE_POSE && ob->pose != nullptr) {
    /* In Pose mode select and activate the target Bone/Pose-Channel. */
    bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose, bone_name);
    if (pchan != nullptr) {
      if (reveal_hidden) {
        /* Unhide the bone. */
        pchan->bone->flag &= ~BONE_HIDDEN_P;
        ANIM_armature_bonecoll_show_from_pchan(arm, pchan);
      }

      /* Select it. */
      ED_pose_deselect_all(ob, SEL_DESELECT, true);
      ED_pose_bone_select(ob, pchan, true, true);

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
    return false;
  }
  if (obact && obact->mode) {
    return false;
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select by Type
 * \{ */

static wmOperatorStatus object_select_by_type_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  short obtype, extend;

  obtype = RNA_enum_get(op->ptr, "type");
  extend = RNA_boolean_get(op->ptr, "extend");

  if (extend == 0) {
    base_deselect_all(scene, view_layer, v3d, SEL_DESELECT);
  }

  CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
    if (base->object->type == obtype) {
      base_select(base, BA_SELECT);
    }
  }
  CTX_DATA_END;

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

  /* API callbacks. */
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
    {0, nullptr, 0, nullptr, nullptr},
};

static bool object_select_all_by_obdata(bContext *C, void *obdata)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
    if (((base->flag & BASE_SELECTED) == 0) && ((base->flag & BASE_SELECTABLE) != 0)) {
      if (base->object->data == obdata) {
        base_select(base, BA_SELECT);
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
          base_select(base, BA_SELECT);
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
                                        nullptr;

  CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
    if (((base->flag & BASE_SELECTED) == 0) && ((base->flag & BASE_SELECTABLE) != 0)) {
      Collection *instance_collection_other = (base->object->transflag & OB_DUPLICOLLECTION) ?
                                                  base->object->instance_collection :
                                                  nullptr;
      if (instance_collection == instance_collection_other) {
        base_select(base, BA_SELECT);
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
      LISTBASE_FOREACH (ParticleSystem *, psys, &base->object->particlesystem) {
        if (psys->part == psys_act->part) {
          base_select(base, BA_SELECT);
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
        base_select(base, BA_SELECT);
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
        base_select(base, BA_SELECT);
        changed = true;
      }
    }
  }
  CTX_DATA_END;

  return changed;
}

void select_linked_by_id(bContext *C, ID *id)
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

static wmOperatorStatus object_select_linked_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  Object *ob;
  int nr = RNA_enum_get(op->ptr, "type");
  bool changed = false, extend;

  extend = RNA_boolean_get(op->ptr, "extend");

  if (extend == 0) {
    base_deselect_all(scene, view_layer, v3d, SEL_DESELECT);
  }

  BKE_view_layer_synced_ensure(scene, view_layer);
  ob = BKE_view_layer_active_object_get(view_layer);
  if (ob == nullptr) {
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
    if (ob->data == nullptr) {
      return OPERATOR_CANCELLED;
    }

    changed = object_select_all_by_obdata(C, ob->data);
  }
  else if (nr == OBJECT_SELECT_LINKED_MATERIAL) {
    Material *mat = nullptr;

    mat = BKE_object_material_get(ob, ob->actcol);
    if (mat == nullptr) {
      return OPERATOR_CANCELLED;
    }

    changed = object_select_all_by_material(C, mat);
  }
  else if (nr == OBJECT_SELECT_LINKED_DUPGROUP) {
    if (ob->instance_collection == nullptr) {
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
    if (ob->data == nullptr) {
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

  /* API callbacks. */
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
    {0, nullptr, 0, nullptr, nullptr},
};

static bool select_grouped_children(bContext *C, Object *ob, const bool recursive)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    if (ob == base->object->parent) {
      if ((base->flag & BASE_SELECTED) == 0) {
        base_select(base, BA_SELECT);
        changed = true;
      }

      if (recursive) {
        changed |= select_grouped_children(C, base->object, true);
      }
    }
  }
  CTX_DATA_END;
  return changed;
}

/* Makes parent active and de-selected BKE_view_layer_active_object_get. */
static bool select_grouped_parent(bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  Base *baspar, *basact = CTX_data_active_base(C);
  bool changed = false;

  if (!basact || !(basact->object->parent)) {
    /* We know BKE_view_layer_active_object_get is valid. */
    return false;
  }

  BKE_view_layer_synced_ensure(scene, view_layer);
  baspar = BKE_view_layer_base_find(view_layer, basact->object->parent);

  /* can be nullptr if parent in other scene */
  if (baspar && BASE_SELECTABLE(v3d, baspar)) {
    base_select(baspar, BA_SELECT);
    base_activate(C, baspar);
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

  for (collection = static_cast<Collection *>(bmain->collections.first);
       collection && (collection_count < COLLECTION_MENU_MAX);
       collection = static_cast<Collection *>(collection->id.next))
  {
    if (BKE_collection_has_object(collection, ob)) {
      ob_collections[collection_count] = collection;
      collection_count++;
    }
  }

  if (!collection_count) {
    return false;
  }
  if (collection_count == 1) {
    collection = ob_collections[0];
    CTX_DATA_BEGIN (C, Base *, base, visible_bases) {
      if (((base->flag & BASE_SELECTED) == 0) && ((base->flag & BASE_SELECTABLE) != 0)) {
        if (BKE_collection_has_object(collection, base->object)) {
          base_select(base, BA_SELECT);
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
    PointerRNA op_ptr = layout->op(
        "OBJECT_OT_select_same_collection", collection->id.name + 2, ICON_NONE);
    RNA_string_set(&op_ptr, "collection", collection->id.name + 2);
  }

  UI_popup_menu_end(C, pup);
  return changed; /* The operator already handle this! */
}

static bool select_grouped_object_hooks(bContext *C, Object *ob)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);

  bool changed = false;
  Base *base;
  HookModifierData *hmd;

  LISTBASE_FOREACH (ModifierData *, md, &ob->modifiers) {
    if (md->type == eModifierType_Hook) {
      hmd = (HookModifierData *)md;
      if (hmd->object) {
        BKE_view_layer_synced_ensure(scene, view_layer);
        base = BKE_view_layer_base_find(view_layer, hmd->object);
        if (base && ((base->flag & BASE_SELECTED) == 0) && BASE_SELECTABLE(v3d, base)) {
          base_select(base, BA_SELECT);
          changed = true;
        }
      }
    }
  }
  return changed;
}

/* Select objects with the same parent as the active (siblings),
 * parent can be nullptr also */
static bool select_grouped_siblings(bContext *C, Object *ob)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    if ((base->object->parent == ob->parent) && ((base->flag & BASE_SELECTED) == 0)) {
      base_select(base, BA_SELECT);
      changed = true;
    }
  }
  CTX_DATA_END;
  return changed;
}
static bool select_grouped_lighttype(bContext *C, Object *ob)
{
  Light *la = static_cast<Light *>(ob->data);

  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    if (base->object->type == OB_LAMP) {
      Light *la_test = static_cast<Light *>(base->object->data);
      if ((la->type == la_test->type) && ((base->flag & BASE_SELECTED) == 0)) {
        base_select(base, BA_SELECT);
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
      base_select(base, BA_SELECT);
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
      base_select(base, BA_SELECT);
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
        compare_v3v3(base->object->color, ob->color, 0.005f))
    {
      base_select(base, BA_SELECT);
      changed = true;
    }
  }
  CTX_DATA_END;
  return changed;
}

static bool objects_share_gameprop(Object *a, Object *b)
{
  bool changed = false;
  bProperty *prop;

  for (prop = (bProperty *)a->prop.first; prop; prop = prop->next) {
    if (BKE_bproperty_object_get(b, prop->name)) {
      changed = true;
    }
  }
  return changed;
}

static bool select_grouped_gameprops(bContext *C, Object *ob)
{
  bool changed = false;

  CTX_DATA_BEGIN (C, Base *, base, selectable_bases) {
    if (((base->flag & BASE_SELECTED) == 0) && (objects_share_gameprop(base->object, ob))) {
      object::base_select(base, BA_SELECT);
      changed = true;
    }
  }
  CTX_DATA_END;
  return changed;
}

static bool select_grouped_keyingset(bContext *C, Object * /*ob*/, ReportList *reports)
{
  KeyingSet *ks = blender::animrig::scene_get_active_keyingset(CTX_data_scene(C));
  bool changed = false;

  /* firstly, validate KeyingSet */
  if (ks == nullptr) {
    BKE_report(reports, RPT_ERROR, "No active Keying Set to use");
    return false;
  }
  if (blender::animrig::validate_keyingset(C, nullptr, ks) !=
      blender::animrig::ModifyKeyReturn::SUCCESS)
  {
    if (ks->paths.first == nullptr) {
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
      /* This is the slow way... we could end up with > 500 items here,
       * with none matching, but end up doing this on 1000 objects. */
      LISTBASE_FOREACH (KS_Path *, ksp, &ks->paths) {
        /* if id matches, select then stop looping (match found) */
        if (ksp->id == (ID *)base->object) {
          base_select(base, BA_SELECT);
          changed = true;
          break;
        }
      }
    }
  }
  CTX_DATA_END;

  return changed;
}

static wmOperatorStatus object_select_grouped_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  Object *ob;
  const int type = RNA_enum_get(op->ptr, "type");
  bool changed = false, extend;

  extend = RNA_boolean_get(op->ptr, "extend");

  if (extend == 0) {
    changed = base_deselect_all(scene, view_layer, v3d, SEL_DESELECT);
  }

  BKE_view_layer_synced_ensure(scene, view_layer);
  ob = BKE_view_layer_active_object_get(view_layer);
  if (ob == nullptr) {
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

  /* API callbacks. */
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

static wmOperatorStatus object_select_all_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  int action = RNA_enum_get(op->ptr, "action");
  bool any_visible = false;

  bool changed = base_deselect_all_ex(scene, view_layer, v3d, action, &any_visible);

  if (changed) {
    DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);

    ED_outliner_select_sync_from_object_tag(C);

    return OPERATOR_FINISHED;
  }
  if (any_visible == false) {
    /* TODO(@ideasman42): Looks like we could remove this,
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

  /* API callbacks. */
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

static wmOperatorStatus object_select_same_collection_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Collection *collection;
  char collection_name[MAX_ID_NAME - 2];

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
        base_select(base, BA_SELECT);
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

  /* API callbacks. */
  ot->exec = object_select_same_collection_exec;
  ot->poll = objects_selectable_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_string(ot->srna,
                 "collection",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Collection",
                 "Name of the collection to select");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Mirror
 * \{ */

static wmOperatorStatus object_select_mirror_exec(bContext *C, wmOperator *op)
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
        BKE_view_layer_synced_ensure(scene, view_layer);
        Base *secbase = BKE_view_layer_base_find(view_layer, ob);

        if (secbase) {
          base_select(secbase, BA_SELECT);
        }
      }
    }

    if (extend == false) {
      base_select(primbase, BA_DESELECT);
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

  /* API callbacks. */
  ot->exec = object_select_mirror_exec;
  ot->poll = objects_selectable_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "extend",
                  false,
                  "Extend",
                  "Extend selection instead of deselecting everything first");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select More/Less
 * \{ */

static bool object_select_more_less(bContext *C, const bool select)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  BKE_view_layer_synced_ensure(scene, view_layer);
  LISTBASE_FOREACH (Base *, base, BKE_view_layer_object_bases_get(view_layer)) {
    Object *ob = base->object;
    ob->flag &= ~OB_DONE;
    ob->id.tag &= ~ID_TAG_DOIT;
    /* parent may be in another scene */
    if (ob->parent) {
      ob->parent->flag &= ~OB_DONE;
      ob->parent->id.tag &= ~ID_TAG_DOIT;
    }
  }

  Vector<PointerRNA> ctx_base_list;
  CTX_data_selectable_bases(C, &ctx_base_list);

  CTX_DATA_BEGIN (C, Object *, ob, selected_objects) {
    ob->flag |= OB_DONE;
  }
  CTX_DATA_END;

  for (PointerRNA &ptr : ctx_base_list) {
    Object *ob = ((Base *)ptr.data)->object;
    if (ob->parent) {
      if ((ob->flag & OB_DONE) != (ob->parent->flag & OB_DONE)) {
        ob->id.tag |= ID_TAG_DOIT;
        ob->parent->id.tag |= ID_TAG_DOIT;
      }
    }
  }

  bool changed = false;
  const short select_mode = select ? BA_SELECT : BA_DESELECT;
  const short select_flag = select ? BASE_SELECTED : 0;

  for (PointerRNA &ptr : ctx_base_list) {
    Base *base = static_cast<Base *>(ptr.data);
    Object *ob = base->object;
    if ((ob->id.tag & ID_TAG_DOIT) && ((base->flag & BASE_SELECTED) != select_flag)) {
      base_select(base, eObjectSelect_Mode(select_mode));
      changed = true;
    }
  }

  return changed;
}

static wmOperatorStatus object_select_more_exec(bContext *C, wmOperator * /*op*/)
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

  /* API callbacks. */
  ot->exec = object_select_more_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus object_select_less_exec(bContext *C, wmOperator * /*op*/)
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

  /* API callbacks. */
  ot->exec = object_select_less_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Random
 * \{ */

static wmOperatorStatus object_select_random_exec(bContext *C, wmOperator *op)
{
  const bool select = (RNA_enum_get(op->ptr, "action") == SEL_SELECT);
  const float randfac = RNA_float_get(op->ptr, "ratio");
  const int seed = WM_operator_properties_select_random_seed_increment_get(op);

  Vector<PointerRNA> ctx_data_list;
  CTX_data_selectable_bases(C, &ctx_data_list);
  int elem_map_len = 0;
  Base **elem_map = static_cast<Base **>(
      MEM_mallocN(sizeof(*elem_map) * ctx_data_list.size(), __func__));

  for (PointerRNA &ptr : ctx_data_list) {
    elem_map[elem_map_len++] = static_cast<Base *>(ptr.data);
  }

  BLI_array_randomize(elem_map, sizeof(*elem_map), elem_map_len, seed);
  const int count_select = elem_map_len * randfac;
  for (int i = 0; i < count_select; i++) {
    base_select(elem_map[i], eObjectSelect_Mode(select));
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
  ot->description = "Select or deselect random visible objects";
  ot->idname = "OBJECT_OT_select_random";

  /* API callbacks. */
  // ot->invoke = object_select_random_invoke; /* TODO: need a number popup. */
  ot->exec = object_select_random_exec;
  ot->poll = objects_selectable_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  WM_operator_properties_select_random(ot);
}

/** \} */

}  // namespace blender::ed::object
