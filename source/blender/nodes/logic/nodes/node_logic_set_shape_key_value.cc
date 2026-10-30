/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "MEM_guardedalloc.h"

#include "BLI_string.h"

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_tree_update.hh"

#include "DNA_key_types.h"
#include "DNA_logic_node_binding_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"

#include "ED_undo.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_string_search.hh"

namespace blender::nodes::node_logic_set_shape_key_value_cc {
namespace decl = blender::nodes::logic::decl;

struct ShapeKeySearchData {
  int32_t node_id;
  char display[256];
};

static char shape_key_clear_item;

static const bNodeSocket *find_input(const bNode &node, const char *identifier)
{
  for (const bNodeSocket *socket = static_cast<const bNodeSocket *>(node.inputs.first);
       socket != nullptr;
       socket = socket->next)
  {
    if (STREQ(socket->identifier, identifier)) {
      return socket;
    }
  }
  return nullptr;
}

static bNodeSocket *find_input(bNode &node, const char *identifier)
{
  return const_cast<bNodeSocket *>(find_input(const_cast<const bNode &>(node), identifier));
}

static const char *shape_key_name(const bNode &node)
{
  const bNodeSocket *socket = find_input(node, "Shape Key");
  if (socket == nullptr || socket->type != SOCK_STRING || socket->default_value == nullptr) {
    return "";
  }
  return static_cast<const bNodeSocketValueString *>(socket->default_value)->value;
}

static bool tree_is_bound_to_object(const bNodeTree &tree, const Object &object)
{
  for (const LogicNodeBinding *binding =
           static_cast<const LogicNodeBinding *>(object.logic_node_bindings.first);
       binding != nullptr;
       binding = binding->next)
  {
    if (binding->tree == &tree) {
      return true;
    }
  }
  return false;
}

static Object *selected_object(const bNodeTree &tree, const bNode &node, const bContext &C)
{
  const bNodeSocket *socket = find_input(node, "Object");
  if (socket == nullptr || socket->is_directly_linked()) {
    return nullptr;
  }
  if (socket->type == SOCK_OBJECT && socket->default_value != nullptr) {
    if (Object *object = static_cast<const bNodeSocketValueObject *>(socket->default_value)->value)
    {
      return object;
    }
  }

  Object *active_object = CTX_data_active_object(&C);
  return active_object != nullptr && tree_is_bound_to_object(tree, *active_object) ?
             active_object :
             nullptr;
}

static Key *relative_shape_keys(Object *object)
{
  if (object == nullptr || object->type != OB_MESH || object->data == nullptr) {
    return nullptr;
  }
  Mesh *mesh = reinterpret_cast<Mesh *>(object->data);
  return mesh->key != nullptr && mesh->key->type == KEY_RELATIVE ? mesh->key : nullptr;
}

static KeyBlock *find_assignable_shape_key(Key &key, const char *name)
{
  if (name == nullptr || name[0] == '\0') {
    return nullptr;
  }
  for (KeyBlock *key_block = static_cast<KeyBlock *>(key.block.first); key_block != nullptr;
       key_block = key_block->next)
  {
    if (key_block != key.refkey && STREQ(key_block->name, name)) {
      return key_block;
    }
  }
  return nullptr;
}

static bool has_assignable_shape_key(const Key &key)
{
  for (const KeyBlock *key_block = static_cast<const KeyBlock *>(key.block.first);
       key_block != nullptr;
       key_block = key_block->next)
  {
    if (key_block != key.refkey) {
      return true;
    }
  }
  return false;
}

static bool search_context(const bContext &C,
                           const ShapeKeySearchData &data,
                           bNodeTree *&r_tree,
                           bNode *&r_node)
{
  SpaceNode *space = CTX_wm_space_node(&C);
  r_tree = space != nullptr ? space->edittree : nullptr;
  r_node = nullptr;
  if (r_tree == nullptr || r_tree->type != NTREE_LOGIC) {
    return false;
  }
  r_tree->ensure_topology_cache();
  r_node = r_tree->node_by_id(data.node_id);
  return r_node != nullptr && STREQ(r_node->idname, "LogicNativeSetShapeKeyValue");
}

static void search_update(const bContext *C,
                          void *data_v,
                          const char *search_string,
                          ui::SearchItems *items,
                          const bool is_first)
{
  ShapeKeySearchData &data = *static_cast<ShapeKeySearchData *>(data_v);
  bNodeTree *tree = nullptr;
  bNode *node = nullptr;
  if (C == nullptr || !search_context(*C, data, tree, node)) {
    return;
  }

  const bNodeSocket *socket = find_input(*node, "Shape Key");
  if (socket == nullptr || socket->is_directly_linked()) {
    return;
  }
  const StringRef query = is_first ? StringRef() : StringRef(search_string);
  if (query.is_empty() &&
      !ui::search_item_add(items, "None", &shape_key_clear_item, ICON_X, 0, 0))
  {
    return;
  }

  Key *key = relative_shape_keys(selected_object(*tree, *node, *C));
  if (key == nullptr) {
    return;
  }

  ui::string_search::StringSearch<KeyBlock> search;
  for (KeyBlock *key_block = static_cast<KeyBlock *>(key->block.first); key_block != nullptr;
       key_block = key_block->next)
  {
    if (key_block != key->refkey) {
      search.add(key_block->name, key_block);
    }
  }
  for (KeyBlock *key_block : search.query(query)) {
    if (!ui::search_item_add(items, key_block->name, key_block, ICON_SHAPEKEY_DATA, 0, 0)) {
      break;
    }
  }
}

static void search_exec(bContext *C, void *data_v, void *item_v)
{
  if (C == nullptr || item_v == nullptr) {
    return;
  }
  ShapeKeySearchData &data = *static_cast<ShapeKeySearchData *>(data_v);
  bNodeTree *tree = nullptr;
  bNode *node = nullptr;
  if (!search_context(*C, data, tree, node)) {
    return;
  }

  bNodeSocket *socket = find_input(*node, "Shape Key");
  if (socket == nullptr || socket->is_directly_linked() || socket->type != SOCK_STRING ||
      socket->default_value == nullptr)
  {
    return;
  }

  const char *selected_name = "";
  if (item_v != &shape_key_clear_item) {
    Key *key = relative_shape_keys(selected_object(*tree, *node, *C));
    KeyBlock *selected = nullptr;
    if (key != nullptr) {
      for (KeyBlock *key_block = static_cast<KeyBlock *>(key->block.first); key_block != nullptr;
           key_block = key_block->next)
      {
        if (key_block == item_v && key_block != key->refkey) {
          selected = key_block;
          break;
        }
      }
    }
    if (selected == nullptr) {
      return;
    }
    selected_name = selected->name;
  }

  bNodeSocketValueString &value = *static_cast<bNodeSocketValueString *>(socket->default_value);
  if (STREQ(value.value, selected_name)) {
    return;
  }
  STRNCPY(value.value, selected_name);
  BKE_ntree_update_tag_node_property(tree, node);
  if (Main *bmain = CTX_data_main(C)) {
    BKE_main_ensure_invariants(*bmain, tree->id);
  }
  ED_undo_push(C, "Select Shape Key");
}

static ui::Block *search_popup(bContext *C, ARegion *region, void *data_v)
{
  static char search[256] = "";
  search[0] = '\0';

  ShapeKeySearchData *search_data = MEM_new<ShapeKeySearchData>(
      __func__, *static_cast<const ShapeKeySearchData *>(data_v));
  const int width = ui::searchbox_size_x_guess(C, search_update, search_data);
  const int height = ui::searchbox_size_y();
  const int search_height = UI_UNIT_Y - 1.0f * UI_SCALE_FAC;

  ui::Block *block = ui::block_begin(C, region, "_popup", ui::EmbossType::Emboss);
  ui::block_flag_enable(block, ui::BLOCK_LOOP | ui::BLOCK_SEARCH_MENU);
  ui::block_theme_style_set(block, ui::BLOCK_THEME_STYLE_POPUP);
  uiDefBut(block,
           ui::ButtonType::Label,
           "",
           0,
           search_height,
           width,
           height - UI_SEARCHBOX_BOUNDS,
           nullptr,
           0,
           0,
           std::nullopt);
  ui::Button *search_button = uiDefSearchBut(block,
                                             search,
                                             ICON_VIEWZOOM,
                                             sizeof(search),
                                             0,
                                             0,
                                             width,
                                             search_height,
                                             "Search relative shape keys");
  button_placeholder_set(search_button, "Search...");
  button_func_search_set_results_are_suggestions(search_button, true);
  button_func_search_set(search_button,
                         nullptr,
                         search_update,
                         search_data,
                         true,
                         nullptr,
                         search_exec,
                         nullptr);
  ui::block_bounds_set_normal(block, UI_SEARCHBOX_BOUNDS);
  ui::block_direction_set(block, ui::UI_DIR_DOWN);
  ui::button_focus_on_enter_event(CTX_wm_window(C), search_button);
  return block;
}

static void draw_shape_key(CustomSocketDrawParams &params)
{
  if (params.socket.is_directly_linked()) {
    params.draw_standard(params.layout);
    return;
  }

  Object *object = selected_object(params.tree, params.node, params.C);
  if (object == nullptr) {
    params.draw_standard(params.layout);
    return;
  }

  Key *key = relative_shape_keys(object);
  const char *stored = shape_key_name(params.node);
  KeyBlock *selected = key != nullptr ? find_assignable_shape_key(*key, stored) : nullptr;

  ShapeKeySearchData *data = MEM_new_zeroed<ShapeKeySearchData>(__func__);
  data->node_id = params.node.identifier;
  BLI_strncpy(data->display,
              selected != nullptr ? selected->name :
              stored[0] != '\0' ? stored :
                                  "Select Shape Key",
              sizeof(data->display));

  params.layout.alignment_set(ui::LayoutAlign::Expand);
  ui::Layout &row = params.layout.row(true);
  row.label("Shape Key:", ICON_NONE);
  ui::Button *button = uiDefBlockButN(row.block(),
                                      search_popup,
                                      data,
                                      data->display,
                                      0,
                                      0,
                                      10 * UI_UNIT_X,
                                      UI_UNIT_Y,
                                      "Select a relative shape key; Basis cannot be assigned");
  button_drawflag_enable(button, ui::BUT_TEXT_LEFT);
  if (stored[0] != '\0' && selected == nullptr) {
    button_flag_enable(button, ui::BUT_REDALERT);
  }
  row.enabled_set((key != nullptr && has_assignable_shape_key(*key)) || stored[0] != '\0');
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Shape Key"_ustr)
      .description("Relative shape key name; an unlinked static target provides a searchable picker")
      .custom_draw(draw_shape_key);
  b.add_input<decl::Float>("Value"_ustr).default_value(0.0f);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetShapeKeyValue"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_shape_key_value_cc
