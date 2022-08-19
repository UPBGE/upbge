/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_anim_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BKE_animsys.h"
#include "BKE_context.h"
#include "BKE_fcurve.h"
#include "BKE_fcurve_driver.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_nla.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "ED_keyframing.h"

#include "UI_interface.h"

#include "RNA_access.h"
#include "RNA_path.h"

#include "WM_api.h"
#include "WM_types.h"

#include "interface_intern.h"

static FCurve *ui_but_get_fcurve(
    uiBut *but, AnimData **adt, bAction **action, bool *r_driven, bool *r_special)
{
  /* for entire array buttons we check the first component, it's not perfect
   * but works well enough in typical cases */
  const int rnaindex = (but->rnaindex == -1) ? 0 : but->rnaindex;

  return BKE_fcurve_find_by_rna_context_ui(static_cast<bContext *>(but->block->evil_C),
                                           &but->rnapoin,
                                           but->rnaprop,
                                           rnaindex,
                                           adt,
                                           action,
                                           r_driven,
                                           r_special);
}

void ui_but_anim_flag(uiBut *but, const AnimationEvalContext *anim_eval_context)
{
  AnimData *adt;
  bAction *act;
  FCurve *fcu;
  bool driven;
  bool special;

  but->flag &= ~(UI_BUT_ANIMATED | UI_BUT_ANIMATED_KEY | UI_BUT_DRIVEN);
  but->drawflag &= ~UI_BUT_ANIMATED_CHANGED;

  /* NOTE: "special" is reserved for special F-Curves stored on the animation data
   *        itself (which are used to animate properties of the animation data).
   *        We count those as "animated" too for now
   */
  fcu = ui_but_get_fcurve(but, &adt, &act, &driven, &special);

  if (fcu) {
    if (!driven) {
      /* Empty curves are ignored by the animation evaluation system. */
      if (BKE_fcurve_is_empty(fcu)) {
        return;
      }

      but->flag |= UI_BUT_ANIMATED;

      /* T41525 - When the active action is a NLA strip being edited,
       * we need to correct the frame number to "look inside" the
       * remapped action
       */
      float cfra = anim_eval_context->eval_time;
      if (adt) {
        cfra = BKE_nla_tweakedit_remap(adt, cfra, NLATIME_CONVERT_UNMAP);
      }

      if (fcurve_frame_has_keyframe(fcu, cfra, 0)) {
        but->flag |= UI_BUT_ANIMATED_KEY;
      }

      /* XXX: this feature is totally broken and useless with NLA */
      if (adt == nullptr || adt->nla_tracks.first == nullptr) {
        const AnimationEvalContext remapped_context = BKE_animsys_eval_context_construct_at(
            anim_eval_context, cfra);
        if (fcurve_is_changed(but->rnapoin, but->rnaprop, fcu, &remapped_context)) {
          but->drawflag |= UI_BUT_ANIMATED_CHANGED;
        }
      }
    }
    else {
      but->flag |= UI_BUT_DRIVEN;
    }
  }
}

static uiBut *ui_but_anim_decorate_find_attached_button(uiButDecorator *but_decorate)
{
  uiBut *but_iter = nullptr;

  BLI_assert(UI_but_is_decorator(&but_decorate->but));
  BLI_assert(but_decorate->rnapoin.data && but_decorate->rnaprop);

  LISTBASE_CIRCULAR_BACKWARD_BEGIN (
      uiBut *, &but_decorate->but.block->buttons, but_iter, but_decorate->but.prev) {
    if (but_iter != (uiBut *)but_decorate &&
        ui_but_rna_equals_ex(
            but_iter, &but_decorate->rnapoin, but_decorate->rnaprop, but_decorate->rnaindex)) {
      return but_iter;
    }
  }
  LISTBASE_CIRCULAR_BACKWARD_END(
      uiBut *, &but_decorate->but.block->buttons, but_iter, but_decorate->but.prev);

  return nullptr;
}

void ui_but_anim_decorate_update_from_flag(uiButDecorator *decorator_but)
{
  if (!decorator_but->rnapoin.data || !decorator_but->rnaprop) {
    /* Nothing to do. */
    return;
  }

  const uiBut *but_anim = ui_but_anim_decorate_find_attached_button(decorator_but);
  uiBut *but = &decorator_but->but;

  if (!but_anim) {
    printf("Could not find button with matching property to decorate (%s.%s)\n",
           RNA_struct_identifier(decorator_but->rnapoin.type),
           RNA_property_identifier(decorator_but->rnaprop));
    return;
  }

  const int flag = but_anim->flag;

  if (flag & UI_BUT_DRIVEN) {
    but->icon = ICON_DECORATE_DRIVER;
  }
  else if (flag & UI_BUT_ANIMATED_KEY) {
    but->icon = ICON_DECORATE_KEYFRAME;
  }
  else if (flag & UI_BUT_ANIMATED) {
    but->icon = ICON_DECORATE_ANIMATE;
  }
  else if (flag & UI_BUT_OVERRIDDEN) {
    but->icon = ICON_DECORATE_OVERRIDE;
  }
  else {
    but->icon = ICON_DECORATE;
  }

  const int flag_copy = (UI_BUT_DISABLED | UI_BUT_INACTIVE);
  but->flag = (but->flag & ~flag_copy) | (flag & flag_copy);
}

bool ui_but_anim_expression_get(uiBut *but, char *str, size_t maxlen)
{
  FCurve *fcu;
  ChannelDriver *driver;
  bool driven, special;

  fcu = ui_but_get_fcurve(but, nullptr, nullptr, &driven, &special);

  if (fcu && driven) {
    driver = fcu->driver;

    if (driver && driver->type == DRIVER_TYPE_PYTHON) {
      if (str) {
        BLI_strncpy(str, driver->expression, maxlen);
      }
      return true;
    }
  }

  return false;
}

bool ui_but_anim_expression_set(uiBut *but, const char *str)
{
  FCurve *fcu;
  ChannelDriver *driver;
  bool driven, special;

  fcu = ui_but_get_fcurve(but, nullptr, nullptr, &driven, &special);

  if (fcu && driven) {
    driver = fcu->driver;

    if (driver && (driver->type == DRIVER_TYPE_PYTHON)) {
      bContext *C = static_cast<bContext *>(but->block->evil_C);

      BLI_strncpy_utf8(driver->expression, str, sizeof(driver->expression));

      /* tag driver as needing to be recompiled */
      BKE_driver_invalidate_expression(driver, true, false);

      /* clear invalid flags which may prevent this from working */
      driver->flag &= ~DRIVER_FLAG_INVALID;
      fcu->flag &= ~FCURVE_DISABLED;

      /* this notifier should update the Graph Editor and trigger depsgraph refresh? */
      WM_event_add_notifier(C, NC_ANIMATION | ND_KEYFRAME, nullptr);

      DEG_relations_tag_update(CTX_data_main(C));

      return true;
    }
  }

  return false;
}

bool ui_but_anim_expression_create(uiBut *but, const char *str)
{
  bContext *C = static_cast<bContext *>(but->block->evil_C);
  ID *id;
  FCurve *fcu;
  char *path;
  bool ok = false;

  /* button must have RNA-pointer to a numeric-capable property */
  if (ELEM(nullptr, but->rnapoin.data, but->rnaprop)) {
    if (G.debug & G_DEBUG) {
      printf("ERROR: create expression failed - button has no RNA info attached\n");
    }
    return false;
  }

  if (RNA_property_array_check(but->rnaprop) != 0) {
    if (but->rnaindex == -1) {
      if (G.debug & G_DEBUG) {
        printf("ERROR: create expression failed - can't create expression for entire array\n");
      }
      return false;
    }
  }

  /* make sure we have animdata for this */
  /* FIXME: until materials can be handled by depsgraph,
   * don't allow drivers to be created for them */
  id = but->rnapoin.owner_id;
  if ((id == nullptr) || (GS(id->name) == ID_MA) || (GS(id->name) == ID_TE)) {
    if (G.debug & G_DEBUG) {
      printf("ERROR: create expression failed - invalid data-block for adding drivers (%p)\n", id);
    }
    return false;
  }

  /* get path */
  path = RNA_path_from_ID_to_property(&but->rnapoin, but->rnaprop);
  if (path == nullptr) {
    return false;
  }

  /* create driver */
  fcu = verify_driver_fcurve(id, path, but->rnaindex, DRIVER_FCURVE_KEYFRAMES);
  if (fcu) {
    ChannelDriver *driver = fcu->driver;

    if (driver) {
      /* set type of driver */
      driver->type = DRIVER_TYPE_PYTHON;

      /* set the expression */
      /* TODO: need some way of identifying variables used */
      BLI_strncpy_utf8(driver->expression, str, sizeof(driver->expression));

      /* updates */
      BKE_driver_invalidate_expression(driver, true, false);
      DEG_relations_tag_update(CTX_data_main(C));
      WM_event_add_notifier(C, NC_ANIMATION | ND_KEYFRAME, nullptr);
      ok = true;
    }
  }

  MEM_freeN(path);

  return ok;
}

void ui_but_anim_autokey(bContext *C, uiBut *but, Scene *scene, float cfra)
{
  ED_autokeyframe_property(C, scene, &but->rnapoin, but->rnaprop, but->rnaindex, cfra, true);
}

void ui_but_anim_copy_driver(bContext *C)
{
  /* this operator calls UI_context_active_but_prop_get */
  WM_operator_name_call(C, "ANIM_OT_copy_driver_button", WM_OP_INVOKE_DEFAULT, nullptr, nullptr);
}

void ui_but_anim_paste_driver(bContext *C)
{
  /* this operator calls UI_context_active_but_prop_get */
  WM_operator_name_call(C, "ANIM_OT_paste_driver_button", WM_OP_INVOKE_DEFAULT, nullptr, nullptr);
}

void ui_but_anim_decorate_cb(bContext *C, void *arg_but, void *UNUSED(arg_dummy))
{
  wmWindowManager *wm = CTX_wm_manager(C);
  uiButDecorator *but_decorate = static_cast<uiButDecorator *>(arg_but);
  uiBut *but_anim = ui_but_anim_decorate_find_attached_button(but_decorate);

  if (!but_anim) {
    return;
  }

  /* FIXME(@campbellbarton): swapping active pointer is weak. */
  SWAP(struct uiHandleButtonData *, but_anim->active, but_decorate->but.active);
  wm->op_undo_depth++;

  if (but_anim->flag & UI_BUT_DRIVEN) {
    /* pass */
    /* TODO: report? */
  }
  else if (but_anim->flag & UI_BUT_ANIMATED_KEY) {
    PointerRNA props_ptr;
    wmOperatorType *ot = WM_operatortype_find("ANIM_OT_keyframe_delete_button", false);
    WM_operator_properties_create_ptr(&props_ptr, ot);
    RNA_boolean_set(&props_ptr, "all", but_anim->rnaindex == -1);
    WM_operator_name_call_ptr(C, ot, WM_OP_INVOKE_DEFAULT, &props_ptr, nullptr);
    WM_operator_properties_free(&props_ptr);
  }
  else {
    PointerRNA props_ptr;
    wmOperatorType *ot = WM_operatortype_find("ANIM_OT_keyframe_insert_button", false);
    WM_operator_properties_create_ptr(&props_ptr, ot);
    RNA_boolean_set(&props_ptr, "all", but_anim->rnaindex == -1);
    WM_operator_name_call_ptr(C, ot, WM_OP_INVOKE_DEFAULT, &props_ptr, nullptr);
    WM_operator_properties_free(&props_ptr);
  }

  SWAP(struct uiHandleButtonData *, but_anim->active, but_decorate->but.active);
  wm->op_undo_depth--;
}
