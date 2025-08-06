/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include <cfloat>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_blender_user_menu.hh"
#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

/* -------------------------------------------------------------------- */
/** \name Internal Utilities
 * \{ */

static const char *screen_menu_context_string(const bContext *C, const SpaceLink *sl)
{
  if (sl->spacetype == SPACE_NODE) {
    const SpaceNode *snode = (const SpaceNode *)sl;
    return snode->tree_idname;
  }
  return CTX_data_mode_string(C);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Menu Type
 * \{ */

bUserMenu **ED_screen_user_menus_find(const bContext *C, uint *r_len)
{
  SpaceLink *sl = CTX_wm_space_data(C);

  if (sl == nullptr) {
    *r_len = 0;
    return nullptr;
  }

  const char *context_mode = CTX_data_mode_string(C);
  const char *context = screen_menu_context_string(C, sl);
  uint array_len = 3;
  bUserMenu **um_array = static_cast<bUserMenu **>(
      MEM_calloc_arrayN(array_len, sizeof(*um_array), __func__));
  um_array[0] = BKE_blender_user_menu_find(&U.user_menus, sl->spacetype, context);
  um_array[1] = (sl->spacetype != SPACE_TOPBAR) ?
                    BKE_blender_user_menu_find(&U.user_menus, SPACE_TOPBAR, context_mode) :
                    nullptr;
  um_array[2] = (sl->spacetype == SPACE_VIEW3D) ?
                    BKE_blender_user_menu_find(&U.user_menus, SPACE_PROPERTIES, context_mode) :
                    nullptr;

  *r_len = array_len;
  return um_array;
}

bUserMenu *ED_screen_user_menu_ensure(bContext *C)
{
  SpaceLink *sl = CTX_wm_space_data(C);
  const char *context = screen_menu_context_string(C, sl);
  return BKE_blender_user_menu_ensure(&U.user_menus, sl->spacetype, context);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Menu Item
 * \{ */

bUserMenuItem_Op *ED_screen_user_menu_item_find_operator(ListBase *lb,
                                                         const wmOperatorType *ot,
                                                         IDProperty *prop,
                                                         const char *op_prop_enum,
                                                         blender::wm::OpCallContext opcontext)
{
  LISTBASE_FOREACH (bUserMenuItem *, umi, lb) {
    if (umi->type == USER_MENU_TYPE_OPERATOR) {
      bUserMenuItem_Op *umi_op = (bUserMenuItem_Op *)umi;
      const bool ok_idprop = prop ? IDP_EqualsProperties(prop, umi_op->prop) : true;
      const bool ok_prop_enum = (umi_op->op_prop_enum[0] != '\0') ?
                                    STREQ(umi_op->op_prop_enum, op_prop_enum) :
                                    true;
      if (STREQ(ot->idname, umi_op->op_idname) &&
          (opcontext == blender::wm::OpCallContext(umi_op->opcontext)) && ok_idprop &&
          ok_prop_enum)
      {
        return umi_op;
      }
    }
  }
  return nullptr;
}

bUserMenuItem_Menu *ED_screen_user_menu_item_find_menu(ListBase *lb, const MenuType *mt)
{
  LISTBASE_FOREACH (bUserMenuItem *, umi, lb) {
    if (umi->type == USER_MENU_TYPE_MENU) {
      bUserMenuItem_Menu *umi_mt = (bUserMenuItem_Menu *)umi;
      if (STREQ(mt->idname, umi_mt->mt_idname)) {
        return umi_mt;
      }
    }
  }
  return nullptr;
}

bUserMenuItem_Prop *ED_screen_user_menu_item_find_prop(ListBase *lb,
                                                       const char *context_data_path,
                                                       const char *prop_id,
                                                       int prop_index)
{
  LISTBASE_FOREACH (bUserMenuItem *, umi, lb) {
    if (umi->type == USER_MENU_TYPE_PROP) {
      bUserMenuItem_Prop *umi_pr = (bUserMenuItem_Prop *)umi;
      if (STREQ(context_data_path, umi_pr->context_data_path) && STREQ(prop_id, umi_pr->prop_id) &&
          (prop_index == umi_pr->prop_index))
      {
        return umi_pr;
      }
    }
  }
  return nullptr;
}

void ED_screen_user_menu_item_add_operator(ListBase *lb,
                                           const char *ui_name,
                                           const wmOperatorType *ot,
                                           const IDProperty *prop,
                                           const char *op_prop_enum,
                                           blender::wm::OpCallContext opcontext)
{
  bUserMenuItem_Op *umi_op = (bUserMenuItem_Op *)BKE_blender_user_menu_item_add(
      lb, USER_MENU_TYPE_OPERATOR);
  umi_op->opcontext = int8_t(opcontext);
  if (!STREQ(ui_name, ot->name)) {
    STRNCPY_UTF8(umi_op->item.ui_name, ui_name);
  }
  STRNCPY_UTF8(umi_op->op_idname, ot->idname);
  STRNCPY_UTF8(umi_op->op_prop_enum, op_prop_enum);
  umi_op->prop = prop ? IDP_CopyProperty(prop) : nullptr;
}

void ED_screen_user_menu_item_add_menu(ListBase *lb, const char *ui_name, const MenuType *mt)
{
  bUserMenuItem_Menu *umi_mt = (bUserMenuItem_Menu *)BKE_blender_user_menu_item_add(
      lb, USER_MENU_TYPE_MENU);
  if (!STREQ(ui_name, mt->label)) {
    STRNCPY_UTF8(umi_mt->item.ui_name, ui_name);
  }
  STRNCPY_UTF8(umi_mt->mt_idname, mt->idname);
}

void ED_screen_user_menu_item_add_prop(ListBase *lb,
                                       const char *ui_name,
                                       const char *context_data_path,
                                       const char *prop_id,
                                       int prop_index)
{
  bUserMenuItem_Prop *umi_pr = (bUserMenuItem_Prop *)BKE_blender_user_menu_item_add(
      lb, USER_MENU_TYPE_PROP);
  STRNCPY_UTF8(umi_pr->item.ui_name, ui_name);
  STRNCPY_UTF8(umi_pr->context_data_path, context_data_path);
  STRNCPY_UTF8(umi_pr->prop_id, prop_id);
  umi_pr->prop_index = prop_index;
}

void ED_screen_user_menu_item_remove(ListBase *lb, bUserMenuItem *umi)
{
  BLI_remlink(lb, umi);
  BKE_blender_user_menu_item_free(umi);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Menu Definition
 * \{ */

static void screen_user_menu_draw(const bContext *C, Menu *menu)
{
  using namespace blender;
  /* Enable when we have the ability to edit menus. */
  const bool show_missing = false;
  char label[512];

  uint um_array_len;
  bUserMenu **um_array = ED_screen_user_menus_find(C, &um_array_len);
  bool is_empty = true;
  for (int um_index = 0; um_index < um_array_len; um_index++) {
    bUserMenu *um = um_array[um_index];
    if (um == nullptr) {
      continue;
    }
    LISTBASE_FOREACH (bUserMenuItem *, umi, &um->items) {
      std::optional<StringRefNull> ui_name = umi->ui_name[0] ?
                                                 std::make_optional<StringRefNull>(umi->ui_name) :
                                                 std::nullopt;
      if (umi->type == USER_MENU_TYPE_OPERATOR) {
        bUserMenuItem_Op *umi_op = (bUserMenuItem_Op *)umi;
        if (wmOperatorType *ot = WM_operatortype_find(umi_op->op_idname, false)) {
          if (ui_name) {
            ui_name = CTX_IFACE_(ot->translation_context, ui_name->c_str());
          }
          if (umi_op->op_prop_enum[0] == '\0') {
            PointerRNA ptr = menu->layout->op(ot,
                                              ui_name,
                                              ICON_NONE,
                                              blender::wm::OpCallContext(umi_op->opcontext),
                                              UI_ITEM_NONE);
            if (umi_op->prop) {
              IDP_CopyPropertyContent(ptr.data_as<IDProperty>(), umi_op->prop);
            }
          }
          else {
            /* umi_op->prop could be used to set other properties but it's currently unsupported.
             */
            menu->layout->op_menu_enum(C, ot, umi_op->op_prop_enum, ui_name, ICON_NONE);
          }
          is_empty = false;
        }
        else {
          if (show_missing) {
            SNPRINTF_UTF8(label, RPT_("Missing: %s"), umi_op->op_idname);
            menu->layout->label(label, ICON_NONE);
          }
        }
      }
      else if (umi->type == USER_MENU_TYPE_MENU) {
        bUserMenuItem_Menu *umi_mt = (bUserMenuItem_Menu *)umi;
        MenuType *mt = WM_menutype_find(umi_mt->mt_idname, false);
        if (mt != nullptr) {
          menu->layout->menu(mt, ui_name, ICON_NONE);
          is_empty = false;
        }
        else {
          if (show_missing) {
            SNPRINTF_UTF8(label, RPT_("Missing: %s"), umi_mt->mt_idname);
            menu->layout->label(label, ICON_NONE);
          }
        }
      }
      else if (umi->type == USER_MENU_TYPE_PROP) {
        bUserMenuItem_Prop *umi_pr = (bUserMenuItem_Prop *)umi;

        char *data_path = strchr(umi_pr->context_data_path, '.');
        if (data_path) {
          *data_path = '\0';
        }
        PointerRNA ptr = CTX_data_pointer_get(C, umi_pr->context_data_path);
        if (ptr.type == nullptr) {
          PointerRNA ctx_ptr = RNA_pointer_create_discrete(nullptr, &RNA_Context, (void *)C);
          if (!RNA_path_resolve_full(&ctx_ptr, umi_pr->context_data_path, &ptr, nullptr, nullptr))
          {
            ptr.type = nullptr;
          }
        }
        if (data_path) {
          *data_path = '.';
          data_path += 1;
        }

        bool ok = false;
        if (ptr.type != nullptr) {
          PropertyRNA *prop = nullptr;
          PointerRNA prop_ptr = ptr;
          if ((data_path == nullptr) ||
              RNA_path_resolve_full(&ptr, data_path, &prop_ptr, nullptr, nullptr))
          {
            prop = RNA_struct_find_property(&prop_ptr, umi_pr->prop_id);
            if (prop) {
              ok = true;
              menu->layout->prop(
                  &prop_ptr, prop, umi_pr->prop_index, 0, UI_ITEM_NONE, ui_name, ICON_NONE);
              is_empty = false;
            }
          }
        }
        if (!ok) {
          if (show_missing) {
            SNPRINTF_UTF8(
                label, RPT_("Missing: %s.%s"), umi_pr->context_data_path, umi_pr->prop_id);
            menu->layout->label(label, ICON_NONE);
          }
        }
      }
      else if (umi->type == USER_MENU_TYPE_SEP) {
        menu->layout->separator();
      }
    }
  }
  if (um_array) {
    MEM_freeN(um_array);
  }

  if (is_empty) {
    menu->layout->label(RPT_("No menu items found"), ICON_NONE);
    menu->layout->label(RPT_("Right click on buttons to add them to this menu"), ICON_NONE);
  }
}

void ED_screen_user_menu_register()
{
  MenuType *mt = MEM_callocN<MenuType>(__func__);
  STRNCPY_UTF8(mt->idname, "SCREEN_MT_user_menu");
  STRNCPY_UTF8(mt->label, N_("Quick Favorites"));
  STRNCPY_UTF8(mt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  mt->draw = screen_user_menu_draw;
  WM_menutype_add(mt);
}

/** \} */
