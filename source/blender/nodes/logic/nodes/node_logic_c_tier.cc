/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "MEM_guardedalloc.h"

#include "BLI_listbase_wrapper.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"

#include "BKE_context.hh"
#include "BKE_icons.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_material.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_node_enum.hh"
#include "BKE_node_tree_update.hh"

#include "DNA_logic_node_binding_types.h"
#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "ED_undo.hh"

#include "RNA_prototypes.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_string_search.hh"

namespace blender::nodes::node_logic_c_tier_cc {
namespace decl = blender::nodes::logic::decl;

static void layout_bone_attribute(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "attribute", UI_ITEM_NONE, "", ICON_NONE);
  const int attribute = RNA_enum_get(ptr, "attribute");
  if (attribute >= 4 && attribute <= 12) {
    layout.prop(ptr, "world_space", UI_ITEM_NONE, "Use World Space", ICON_NONE);
  }
}

static void layout_bone_set_attribute(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "attribute", UI_ITEM_NONE, "", ICON_NONE);
  if (RNA_enum_get(ptr, "attribute") == 2) {
    layout.prop(ptr, "scale_mode", UI_ITEM_NONE, "", ICON_NONE);
  }
}

static void layout_axis(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "axis", UI_ITEM_NONE, "", ICON_NONE);
}

static void layout_resize_vector(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "to_size", UI_ITEM_NONE, "", ICON_NONE);
}

static void layout_matrix_to_xyz(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "output", UI_ITEM_NONE, "", ICON_NONE);
  if (RNA_enum_get(ptr, "output") == 1) {
    layout.prop(ptr, "euler_order", UI_ITEM_NONE, "", ICON_NONE);
  }
}

static void layout_vector_rotate(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
  if (RNA_enum_get(ptr, "mode") == 1) {
    layout.prop(ptr, "axis", UI_ITEM_NONE, "", ICON_NONE);
  }
}

static void layout_vector_to_rotation(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "axis", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "use_up_reference", UI_ITEM_NONE, "Use Up Reference", ICON_NONE);
}

static void layout_draw(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
  const int mode = RNA_enum_get(ptr, "mode");
  if (mode == 3 || mode == 4) {
    layout.prop(ptr, "use_volume_origin", UI_ITEM_NONE, nullptr, ICON_NONE);
  }
}

static void layout_volume_origin(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "use_volume_origin", UI_ITEM_NONE, nullptr, ICON_NONE);
}

static constexpr int set_socket_value_float = 0;
static constexpr int set_socket_value_int = 1;
static constexpr int set_socket_value_bool = 2;
static constexpr int set_socket_value_vector = 3;
static constexpr int set_socket_value_color = 4;
static constexpr int set_socket_value_string = 5;
static constexpr int set_socket_value_material = 6;
static constexpr int set_socket_value_generic = 7;
static constexpr int set_socket_target_per_object_only = 1 << 0;

static bool set_socket_value_type_is_valid(const int value_type)
{
  return value_type >= set_socket_value_float && value_type <= set_socket_value_generic;
}

static bool set_socket_value_type_is_material_parameter_supported(const int value_type)
{
  return value_type == set_socket_value_float || value_type == set_socket_value_int ||
         value_type == set_socket_value_bool || value_type == set_socket_value_vector ||
         value_type == set_socket_value_color || value_type == set_socket_value_generic;
}

static bool set_socket_is_material_parameter_node(const bNode &node)
{
  return STREQ(node.idname, "LogicNativeSetMaterialParameter") ||
         STREQ(node.idname, "LogicNativeGetMaterialParameter");
}

static bool set_socket_per_object_only(const bNode &node)
{
  return (node.custom2 & set_socket_target_per_object_only) != 0;
}

static bool set_socket_supported_material_input(const bNodeSocket &socket)
{
  return socket.default_value != nullptr && (socket.flag & SOCK_UNAVAIL) == 0 &&
         ELEM(socket.type,
              SOCK_BOOLEAN,
              SOCK_INT,
              SOCK_FLOAT,
              SOCK_VECTOR,
              SOCK_ROTATION,
              SOCK_RGBA,
              SOCK_STRING,
              SOCK_MATERIAL);
}

static bool set_socket_material_parameter_input_supported(const bNodeSocket &socket)
{
  return socket.default_value != nullptr && (socket.flag & SOCK_UNAVAIL) == 0 &&
         ELEM(socket.type,
              SOCK_BOOLEAN,
              SOCK_INT,
              SOCK_FLOAT,
              SOCK_VECTOR,
              SOCK_ROTATION,
              SOCK_RGBA);
}

static bool set_socket_supported_material_input(const bNode &logic_node, const bNodeSocket &socket)
{
  return set_socket_is_material_parameter_node(logic_node) ?
             set_socket_material_parameter_input_supported(socket) :
             set_socket_supported_material_input(socket);
}

static const bNodeSocket *set_socket_find_input(const bNode &node, const char *identifier)
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

static bNodeSocket *set_socket_find_input(bNode &node, const char *identifier)
{
  return const_cast<bNodeSocket *>(
      set_socket_find_input(const_cast<const bNode &>(node), identifier));
}

static Object *set_socket_selected_object(const bNode &node, const bContext *C)
{
  const bNodeSocket *object_socket = set_socket_find_input(node, "Object");
  if (object_socket == nullptr || object_socket->link != nullptr) {
    return nullptr;
  }

  Object *object = nullptr;
  if (object_socket->default_value != nullptr && object_socket->type == SOCK_OBJECT) {
    object = static_cast<bNodeSocketValueObject *>(object_socket->default_value)->value;
  }
  if (object == nullptr && C != nullptr && set_socket_per_object_only(node)) {
    object = CTX_data_active_object(C);
  }
  return object;
}

static int set_socket_unlinked_slot_value(const bNode &node)
{
  const bNodeSocket *slot_socket = set_socket_find_input(node, "Slot");
  if (slot_socket == nullptr || slot_socket->link != nullptr ||
      slot_socket->default_value == nullptr || slot_socket->type != SOCK_INT)
  {
    return -1;
  }
  return static_cast<const bNodeSocketValueInt *>(slot_socket->default_value)->value;
}

static bool set_socket_per_object_target_is_dynamic(const bNode &node)
{
  const bNodeSocket *object_socket = set_socket_find_input(node, "Object");
  const bNodeSocket *slot_socket = set_socket_find_input(node, "Slot");
  return (object_socket != nullptr && object_socket->link != nullptr) ||
         (slot_socket != nullptr && slot_socket->link != nullptr);
}

static void set_socket_update_slot_bounds(bNode &node, const bContext *C = nullptr)
{
  UNUSED_VARS(C);

  bNodeSocket *slot_socket = set_socket_find_input(node, "Slot");
  if (slot_socket == nullptr || slot_socket->default_value == nullptr ||
      slot_socket->type != SOCK_INT)
  {
    return;
  }

  bNodeSocketValueInt *value = static_cast<bNodeSocketValueInt *>(slot_socket->default_value);
  value->min = 0;
  value->max = 127;
}

static Material *set_socket_unlinked_material_input_value(const bNode &node)
{
  const bNodeSocket *material_socket = set_socket_find_input(node, "Material");
  if (material_socket == nullptr || material_socket->link != nullptr ||
      material_socket->default_value == nullptr || material_socket->type != SOCK_MATERIAL)
  {
    return nullptr;
  }
  return static_cast<bNodeSocketValueMaterial *>(material_socket->default_value)->value;
}

static void set_socket_set_unlinked_material_input_value(bNode &node, Material *material)
{
  bNodeSocket *material_socket = set_socket_find_input(node, "Material");
  if (material_socket == nullptr || material_socket->link != nullptr ||
      material_socket->default_value == nullptr || material_socket->type != SOCK_MATERIAL)
  {
    return;
  }
  static_cast<bNodeSocketValueMaterial *>(material_socket->default_value)->value = material;
}

static Material *set_socket_selected_slot_material(const bNode &node, const bContext *C)
{
  if (!set_socket_per_object_only(node) || set_socket_per_object_target_is_dynamic(node)) {
    return nullptr;
  }

  Object *object = set_socket_selected_object(node, C);
  if (object == nullptr) {
    return nullptr;
  }
  const int slot_index = set_socket_unlinked_slot_value(node);
  if (slot_index < 0 || slot_index >= object->totcol) {
    return nullptr;
  }
  return BKE_object_material_get(object, short(slot_index + 1));
}

static Material *set_socket_selected_material(const bNode &node, const bContext *C = nullptr)
{
  if (!set_socket_per_object_only(node)) {
    return set_socket_unlinked_material_input_value(node);
  }

  if (Material *material = set_socket_selected_slot_material(node, C)) {
    return material;
  }

  return (C == nullptr && !set_socket_per_object_target_is_dynamic(node)) ?
             set_socket_unlinked_material_input_value(node) :
             nullptr;
}

static bool set_socket_node_has_supported_inputs(const bNode &logic_node, const bNode &node)
{
  for (const bNodeSocket *socket = static_cast<const bNodeSocket *>(node.inputs.first);
       socket != nullptr;
       socket = socket->next)
  {
    if (set_socket_supported_material_input(logic_node, *socket)) {
      return true;
    }
  }
  return false;
}

static bool set_socket_has_pickable_material_targets(const bNode &node,
                                                     const bContext *C = nullptr)
{
  const bNodeSocket *node_name = set_socket_find_input(node, "Node Name");
  const bNodeSocket *socket_name = set_socket_find_input(node, "Socket");
  if ((node_name && node_name->link) || (socket_name && socket_name->link)) {
    return false;
  }

  const Material *material = set_socket_selected_material(node, C);
  if (material == nullptr || material->nodetree == nullptr) {
    return false;
  }

  for (const bNode *material_node = static_cast<const bNode *>(material->nodetree->nodes.first);
       material_node != nullptr;
       material_node = material_node->next)
  {
    if (set_socket_node_has_supported_inputs(node, *material_node)) {
      return true;
    }
  }
  return false;
}

static bool set_socket_has_dynamic_target(const bNode &node)
{
  const bNodeSocket *material = set_socket_find_input(node, "Material");
  const bNodeSocket *object = set_socket_find_input(node, "Object");
  const bNodeSocket *slot = set_socket_find_input(node, "Slot");
  const bNodeSocket *node_name = set_socket_find_input(node, "Node Name");
  const bNodeSocket *socket_name = set_socket_find_input(node, "Socket");
  const bool dynamic_material = set_socket_per_object_only(node) ?
                                    ((object && object->link) || (slot && slot->link)) :
                                    (material && material->link);
  return dynamic_material || (node_name && node_name->link) || (socket_name && socket_name->link);
}

static bNode *set_socket_find_material_node_by_name(bNodeTree &ntree, const char *name)
{
  if (name == nullptr || name[0] == '\0') {
    return nullptr;
  }
  if (bNode *node = bke::node_find_node_by_name(ntree, name)) {
    return node;
  }
  for (bNode *node = static_cast<bNode *>(ntree.nodes.first); node != nullptr; node = node->next) {
    if (node->label[0] != '\0' && STREQ(node->label, name)) {
      return node;
    }
  }
  return nullptr;
}

static bNode *set_socket_first_material_node_with_supported_inputs(const bNode &logic_node,
                                                                   bNodeTree &ntree)
{
  for (bNode *node = static_cast<bNode *>(ntree.nodes.first); node != nullptr; node = node->next) {
    if (set_socket_node_has_supported_inputs(logic_node, *node)) {
      return node;
    }
  }
  return nullptr;
}

static const bNodeSocket *set_socket_find_supported_material_input(const bNode &node,
                                                                   const bNode &material_node,
                                                                   const char *name)
{
  for (const bNodeSocket *socket = static_cast<const bNodeSocket *>(material_node.inputs.first);
       socket != nullptr;
       socket = socket->next)
  {
    if (set_socket_supported_material_input(node, *socket) &&
        ((name && STREQ(socket->identifier, name)) || (name && STREQ(socket->name, name))))
    {
      return socket;
    }
  }
  return nullptr;
}

static const bNodeSocket *set_socket_first_supported_material_input(const bNode &logic_node,
                                                                    const bNode &node)
{
  for (const bNodeSocket *socket = static_cast<const bNodeSocket *>(node.inputs.first);
       socket != nullptr;
       socket = socket->next)
  {
    if (set_socket_supported_material_input(logic_node, *socket)) {
      return socket;
    }
  }
  return nullptr;
}

static int set_socket_value_type_for_material_input(const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_BOOLEAN:
      return set_socket_value_bool;
    case SOCK_INT:
      return set_socket_value_int;
    case SOCK_FLOAT:
      return set_socket_value_float;
    case SOCK_VECTOR:
    case SOCK_ROTATION:
      return set_socket_value_vector;
    case SOCK_RGBA:
      return set_socket_value_color;
    case SOCK_STRING:
      return set_socket_value_string;
    case SOCK_MATERIAL:
      return set_socket_value_material;
    default:
      return set_socket_value_generic;
  }
}

static const char *set_socket_string_input_value(const bNode &node, const char *identifier)
{
  const bNodeSocket *socket = set_socket_find_input(node, identifier);
  if (socket == nullptr || socket->default_value == nullptr || socket->type != SOCK_STRING) {
    return "";
  }
  return static_cast<const bNodeSocketValueString *>(socket->default_value)->value;
}

static void set_socket_set_string_input_value(bNode &node,
                                              const char *identifier,
                                              const char *value)
{
  bNodeSocket *socket = set_socket_find_input(node, identifier);
  if (socket == nullptr || socket->default_value == nullptr || socket->type != SOCK_STRING) {
    return;
  }
  STRNCPY(static_cast<bNodeSocketValueString *>(socket->default_value)->value, value ? value : "");
}

static void set_socket_sync_material_cache(bNode &logic_node, const bContext *C)
{
  if (!set_socket_per_object_only(logic_node)) {
    return;
  }
  set_socket_set_unlinked_material_input_value(logic_node,
                                               set_socket_selected_slot_material(logic_node, C));
}

static void set_socket_sync_material_selection(bNode &logic_node, const bContext *C = nullptr)
{
  if (!set_socket_has_pickable_material_targets(logic_node, C)) {
    return;
  }
  Material *material = set_socket_selected_material(logic_node, C);
  if (material == nullptr || material->nodetree == nullptr) {
    return;
  }

  bNode *material_node = set_socket_find_material_node_by_name(
      *material->nodetree, set_socket_string_input_value(logic_node, "Node Name"));
  if (material_node == nullptr ||
      !set_socket_node_has_supported_inputs(logic_node, *material_node))
  {
    material_node = set_socket_first_material_node_with_supported_inputs(logic_node,
                                                                         *material->nodetree);
  }
  if (material_node == nullptr) {
    return;
  }

  set_socket_set_string_input_value(logic_node, "Node Name", material_node->name);

  const bNodeSocket *material_socket = set_socket_find_supported_material_input(
      logic_node, *material_node, set_socket_string_input_value(logic_node, "Socket"));
  if (material_socket == nullptr) {
    material_socket = set_socket_first_supported_material_input(logic_node, *material_node);
  }
  if (material_socket == nullptr) {
    return;
  }

  set_socket_set_string_input_value(logic_node, "Socket", material_socket->identifier);
  logic_node.custom1 = set_socket_value_type_for_material_input(*material_socket);
}

static void set_socket_availability(bNodeTree *ntree, bNode *node)
{
  if (ntree == nullptr || node == nullptr) {
    return;
  }

  if (!set_socket_value_type_is_valid(node->custom1)) {
    node->custom1 = set_socket_value_float;
  }
  if (set_socket_is_material_parameter_node(*node) &&
      !set_socket_value_type_is_material_parameter_supported(node->custom1))
  {
    node->custom1 = set_socket_value_color;
  }

  const int value_type = node->custom1;
  const bool per_object_only = set_socket_per_object_only(*node);
  const auto set_available = [&](const UString identifier, const bool available) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
  };

  set_available("Object"_ustr, per_object_only);
  set_available("Slot"_ustr, per_object_only);
  set_available("Material"_ustr, !per_object_only);
  set_available("Node Name"_ustr, true);
  set_available("Socket"_ustr, true);
  set_available("Value"_ustr, value_type == set_socket_value_generic);
  set_available("Float Value"_ustr, value_type == set_socket_value_float);
  set_available("Integer Value"_ustr, value_type == set_socket_value_int);
  set_available("Boolean Value"_ustr, value_type == set_socket_value_bool);
  set_available("Vector Value"_ustr, value_type == set_socket_value_vector);
  set_available("Color Value"_ustr, value_type == set_socket_value_color);
  set_available("String Value"_ustr, value_type == set_socket_value_string);
  set_available("Material Value"_ustr, value_type == set_socket_value_material);
}

static void init_set_material_parameter(bNodeTree *ntree, bNode *node)
{
  node->custom1 = set_socket_value_color;
  node->custom2 = set_socket_target_per_object_only;
  set_socket_availability(ntree, node);
}

static void update_set_node_socket_for_context(bNodeTree *ntree, bNode *node, const bContext *C)
{
  if (node != nullptr) {
    set_socket_update_slot_bounds(*node, C);
    set_socket_sync_material_cache(*node, C);
    set_socket_sync_material_selection(*node, C);
  }
  set_socket_availability(ntree, node);
}

static void update_set_node_socket(bNodeTree *ntree, bNode *node)
{
  update_set_node_socket_for_context(ntree, node, nullptr);
}

static void draw_set_socket_flow(CustomSocketDrawParams &params)
{
  update_set_node_socket_for_context(&params.tree, &params.node, &params.C);
  params.draw_standard(params.layout);
  params.layout.prop(
      &params.node_ptr, "per_object_only", UI_ITEM_NONE, "Per Object Only", ICON_NONE);
}

static void draw_set_socket_slot_material_info(ui::Layout &layout, bNode &node, const bContext &C);

static void draw_set_socket_material_slot(CustomSocketDrawParams &params)
{
  update_set_node_socket_for_context(&params.tree, &params.node, &params.C);
  params.draw_standard(params.layout);
  if (set_socket_per_object_only(params.node)) {
    draw_set_socket_slot_material_info(params.layout, params.node, params.C);
  }
}

static void draw_set_socket_material(CustomSocketDrawParams &params)
{
  update_set_node_socket_for_context(&params.tree, &params.node, &params.C);
  params.draw_standard(params.layout);
}

static void draw_set_socket_slot_material_info(ui::Layout &layout, bNode &node, const bContext &C)
{
  Object *object = set_socket_selected_object(node, &C);
  Material *material = set_socket_selected_slot_material(node, &C);
  const bool dynamic_target = set_socket_per_object_target_is_dynamic(node);
  const char *material_name = material ?
                                  material->id.name + 2 :
                                  ((object != nullptr && !dynamic_target) ? "No material in slot" :
                                                                            "Runtime object slot");
  const int icon = material ? BKE_icon_id_ensure(&material->id) : ICON_MATERIAL_DATA;
  layout.alignment_set(ui::LayoutAlign::Expand);
  ui::Layout &row = layout.row(true);
  row.enabled_set(false);
  row.label("Slot Material:", ICON_NONE);
  row.label(material_name, icon);
}

static void draw_set_socket_material_node(CustomSocketDrawParams &params)
{
  update_set_node_socket_for_context(&params.tree, &params.node, &params.C);
  if (!params.socket.is_directly_linked() &&
      set_socket_has_pickable_material_targets(params.node, &params.C))
  {
    params.layout.alignment_set(ui::LayoutAlign::Expand);
    ui::Layout &row = params.layout.row(true);
    row.label("Node:", ICON_NONE);
    row.prop(&params.node_ptr, "material_node", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    return;
  }
  params.draw_standard(params.layout);
}

static void draw_set_socket_material_socket(CustomSocketDrawParams &params)
{
  update_set_node_socket_for_context(&params.tree, &params.node, &params.C);
  if (!params.socket.is_directly_linked() &&
      set_socket_has_pickable_material_targets(params.node, &params.C))
  {
    params.layout.alignment_set(ui::LayoutAlign::Expand);
    ui::Layout &row = params.layout.row(true);
    row.label("Socket:", ICON_NONE);
    row.prop(&params.node_ptr, "material_socket", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    return;
  }
  params.draw_standard(params.layout);
  if (set_socket_has_dynamic_target(params.node) &&
      !set_socket_has_pickable_material_targets(params.node, &params.C))
  {
    params.layout.prop(&params.node_ptr, "value_type", UI_ITEM_NONE, "", ICON_NONE);
  }
}

static void draw_get_material_object(CustomSocketDrawParams &params)
{
  update_set_node_socket_for_context(&params.tree, &params.node, &params.C);
  params.draw_standard(params.layout);
}

static void draw_get_material_slot(CustomSocketDrawParams &params)
{
  update_set_node_socket_for_context(&params.tree, &params.node, &params.C);
  params.draw_standard(params.layout);
  if (set_socket_per_object_only(params.node)) {
    draw_set_socket_slot_material_info(params.layout, params.node, params.C);
  }
}

static void draw_get_material_node(CustomSocketDrawParams &params)
{
  update_set_node_socket_for_context(&params.tree, &params.node, &params.C);
  if (!params.socket.is_directly_linked() &&
      set_socket_has_pickable_material_targets(params.node, &params.C))
  {
    params.layout.alignment_set(ui::LayoutAlign::Expand);
    ui::Layout &row = params.layout.row(true);
    row.label("Node:", ICON_NONE);
    row.prop(&params.node_ptr, "material_node", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    return;
  }
  params.draw_standard(params.layout);
}

static void draw_get_material_socket(CustomSocketDrawParams &params)
{
  update_set_node_socket_for_context(&params.tree, &params.node, &params.C);
  if (!params.socket.is_directly_linked() &&
      set_socket_has_pickable_material_targets(params.node, &params.C))
  {
    params.layout.alignment_set(ui::LayoutAlign::Expand);
    ui::Layout &row = params.layout.row(true);
    row.label("Socket:", ICON_NONE);
    row.prop(&params.node_ptr, "material_socket", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    return;
  }
  params.draw_standard(params.layout);
}

static void declare_get_bone_attribute(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>(""_ustr, "Object"_ustr);
  b.add_input<decl::String>(""_ustr, "Bone Name"_ustr).default_value("Bone");
  b.add_output<decl::String>(""_ustr, "Value"_ustr);
  b.add_output<decl::Vector>("Vector"_ustr);
  b.add_output<decl::Bool>("Bool"_ustr);
}

static void declare_set_bone_attribute(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>(""_ustr, "Object"_ustr);
  b.add_input<decl::String>(""_ustr, "Bone Name"_ustr).default_value("Bone");
  b.add_input<decl::Bool>("Bool"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void init_set_bone_attribute(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 2; /* Inherit Scale. */
  node->custom2 = 1; /* Full, matching the existing scale-mode enum values. */
}

static void declare_set_constraint_target(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Armature"_ustr);
  b.add_input<decl::String>(""_ustr, "Bone"_ustr).default_value("Bone");
  b.add_input<decl::String>(""_ustr, "Constraint"_ustr).default_value("Constraint");
  b.add_input<decl::Object>("Target"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void declare_set_constraint_attribute(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Armature"_ustr);
  b.add_input<decl::String>(""_ustr, "Bone"_ustr).default_value("Bone");
  b.add_input<decl::String>(""_ustr, "Constraint"_ustr).default_value("Constraint");
  b.add_input<decl::String>("Attribute"_ustr).default_value("influence");
  b.add_input<decl::Generic>(""_ustr, "Value"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void declare_get_material_from_slot(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr).description("Object to read; defaults to the owner");
  b.add_input<decl::Int>("Slot"_ustr).min(0).max(127).default_value(0);
  b.add_output<decl::Material>("Material"_ustr);
  b.add_output<decl::Bool>("Found"_ustr);
}

static void declare_get_material_slot_count(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr).description("Object to read; defaults to the owner");
  b.add_output<decl::Int>("Count"_ustr);
}

static void declare_get_material_name(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Material>("Material"_ustr);
  b.add_output<decl::String>("Name"_ustr);
}

static void declare_get_material_parameter(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr)
      .description("Object to read; defaults to the owner")
      .custom_draw(draw_get_material_object);
  b.add_input<decl::Int>("Slot"_ustr)
      .min(0)
      .max(127)
      .default_value(0)
      .description("Zero-based material slot to read")
      .custom_draw(draw_get_material_slot);
  b.add_input<decl::String>("Node"_ustr, "Node Name"_ustr)
      .default_value("Principled BSDF")
      .description("Material node identifier or visible name")
      .custom_draw(draw_get_material_node);
  b.add_input<decl::String>("Socket"_ustr)
      .default_value("Base Color")
      .description("Input socket identifier or visible name")
      .custom_draw(draw_get_material_socket);
  b.add_output<decl::Generic>("Value"_ustr);
  b.add_output<decl::Bool>("Found"_ustr);
}

static void declare_set_material_parameter(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).custom_draw(draw_set_socket_flow);
  b.add_input<decl::Object>("Object"_ustr)
      .description("Target object; defaults to the owner when unset");
  b.add_input<decl::Int>("Slot"_ustr)
      .min(0)
      .max(127)
      .default_value(0)
      .description("Zero-based material slot to edit")
      .custom_draw(draw_set_socket_material_slot);
  b.add_input<decl::Material>("Material"_ustr).custom_draw(draw_set_socket_material);
  b.add_input<decl::String>("Node"_ustr, "Node Name"_ustr)
      .default_value("Principled BSDF")
      .description("Material node identifier or visible name")
      .custom_draw(draw_set_socket_material_node);
  b.add_input<decl::String>("Socket"_ustr)
      .default_value("Base Color")
      .description("Input socket identifier or visible name")
      .custom_draw(draw_set_socket_material_socket);
  b.add_input<decl::Generic>("Value"_ustr).description("Linked generic value");
  b.add_input<decl::Float>("Value"_ustr, "Float Value"_ustr).default_value(0.0f);
  b.add_input<decl::Int>("Value"_ustr, "Integer Value"_ustr).default_value(0);
  b.add_input<decl::Bool>("Value"_ustr, "Boolean Value"_ustr).default_value(false);
  b.add_input<decl::Vector>("Value"_ustr, "Vector Value"_ustr).default_value(float3(0.0f));
  b.add_input<decl::Color>("Value"_ustr, "Color Value"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_output<decl::Execution>("Done"_ustr);
}

static void declare_get_tree_socket(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Datablock>("Tree"_ustr);
  b.add_input<decl::String>("Node Name"_ustr);
  b.add_input<decl::Int>("Input"_ustr).min(0).default_value(0);
  b.add_output<decl::Generic>("Value"_ustr);
}

static void declare_set_tree_socket(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Datablock>("Tree"_ustr);
  b.add_input<decl::String>("Node Name"_ustr);
  b.add_input<decl::Int>("Input"_ustr).min(0).default_value(0);
  b.add_input<decl::Generic>("Value"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void add_geometry_setter_value_sockets(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Generic>("Value"_ustr).description("Linked generic value");
  b.add_input<decl::Float>("Value"_ustr, "Float Value"_ustr).default_value(0.0f);
  b.add_input<decl::Int>("Value"_ustr, "Integer Value"_ustr).default_value(0);
  b.add_input<decl::Bool>("Value"_ustr, "Boolean Value"_ustr).default_value(false);
  b.add_input<decl::Vector>("Value"_ustr, "Vector Value"_ustr).default_value(float3(0.0f));
  b.add_input<decl::Color>("Value"_ustr, "Color Value"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::String>("Value"_ustr, "String Value"_ustr);
  b.add_input<decl::Material>("Value"_ustr, "Material Value"_ustr);
}

static void draw_geometry_setter_modifier(CustomSocketDrawParams &params);
static void draw_geometry_setter_interface_input(CustomSocketDrawParams &params);
static void draw_node_mute_flow(CustomSocketDrawParams &params);
static void draw_node_mute_node(CustomSocketDrawParams &params);
static void draw_editor_value_flow(CustomSocketDrawParams &params);
static void draw_editor_value_modifier(CustomSocketDrawParams &params);
static void draw_editor_value_node(CustomSocketDrawParams &params);
static void draw_editor_value_socket(CustomSocketDrawParams &params);
static void draw_make_node_tree_unique_flow(CustomSocketDrawParams &params);

static void declare_set_geometry_nodes_input(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("Target object; an empty input uses the Logic Tree owner");
  b.add_input<decl::String>("Modifier"_ustr)
      .description("Geometry Nodes modifier name")
      .custom_draw(draw_geometry_setter_modifier);
  b.add_input<decl::String>("Input"_ustr)
      .description("Stable interface input identifier or visible input name")
      .custom_draw(draw_geometry_setter_interface_input);
  add_geometry_setter_value_sockets(b);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void declare_get_editor_node_value(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr)
      .description("Object containing the target material slot or Geometry Nodes modifier");
  b.add_input<decl::Int>("Slot"_ustr)
      .min(0)
      .max(127)
      .default_value(0)
      .description("Zero-based material slot");
  b.add_input<decl::String>("Modifier"_ustr)
      .description("Geometry Nodes modifier name")
      .custom_draw(draw_editor_value_modifier);
  b.add_input<decl::String>("Node"_ustr, "Node Name"_ustr)
      .description("Stable node name in the selected node tree")
      .custom_draw(draw_editor_value_node);
  b.add_input<decl::String>("Socket"_ustr)
      .description("Stable identifier of an unlinked input socket")
      .custom_draw(draw_editor_value_socket);
  b.add_output<decl::Generic>("Value"_ustr);
  b.add_output<decl::Bool>("Found"_ustr);
}

static void declare_set_editor_node_value(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).custom_draw(draw_editor_value_flow);
  b.add_input<decl::Object>("Object"_ustr)
      .description("Object containing the target material slot or Geometry Nodes modifier");
  b.add_input<decl::Int>("Slot"_ustr)
      .min(0)
      .max(127)
      .default_value(0)
      .description("Zero-based material slot");
  b.add_input<decl::String>("Modifier"_ustr)
      .description("Geometry Nodes modifier name")
      .custom_draw(draw_editor_value_modifier);
  b.add_input<decl::String>("Node"_ustr, "Node Name"_ustr)
      .description("Stable node name in the selected node tree")
      .custom_draw(draw_editor_value_node);
  b.add_input<decl::String>("Socket"_ustr)
      .description("Stable identifier of an unlinked input socket")
      .custom_draw(draw_editor_value_socket);
  add_geometry_setter_value_sockets(b);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void declare_make_node_tree_unique(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).custom_draw(draw_make_node_tree_unique_flow);
  b.add_input<decl::Object>("Object"_ustr)
      .description("Object whose material slot or Geometry Nodes modifier becomes single-user");
  b.add_input<decl::Int>("Slot"_ustr)
      .min(0)
      .max(127)
      .default_value(0)
      .description("Zero-based material slot");
  b.add_input<decl::String>("Modifier"_ustr)
      .description("Geometry Nodes modifier name")
      .custom_draw(draw_editor_value_modifier);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void declare_set_node_mute(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).custom_draw(draw_node_mute_flow);
  b.add_input<decl::String>("Node"_ustr, "Node Name"_ustr)
      .description("Node identifier or visible label in the selected shared node tree")
      .custom_draw(draw_node_mute_node);
  b.add_input<decl::Bool>("Muted"_ustr)
      .default_value(true)
      .description("Use Blender's native node bypass behavior when enabled");
  b.add_output<decl::Execution>("Done"_ustr);
}

enum ModifierTargetMode {
  modifier_target_name = 0,
  modifier_target_stack_position = 1,
  modifier_target_persistent_id = 2,
};

enum ModifierStackPosition {
  modifier_stack_first = 0,
  modifier_stack_last = 1,
  modifier_stack_index = 2,
};

static void declare_enable_disable_modifier(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("Target object; an empty input uses the Logic Tree owner");
  b.add_input<decl::String>("Modifier"_ustr)
      .description("Object-local modifier name")
      .custom_draw(draw_geometry_setter_modifier);
  b.add_input<decl::Int>("Index"_ustr)
      .default_value(0)
      .min(0)
      .description("Zero-based position in the current modifier stack");
  b.add_input<decl::Int>("Modifier ID"_ustr)
      .default_value(0)
      .min(0)
      .description("Stable runtime modifier ID returned by Assign Geometry Nodes Modifier");
  b.add_input<decl::Bool>("Enabled"_ustr)
      .default_value(true)
      .description("Enable the modifier for realtime game evaluation");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void set_enable_disable_modifier_availability(bNodeTree *ntree, bNode *node)
{
  if (ntree == nullptr || node == nullptr) {
    return;
  }
  if (node->custom1 != modifier_target_name &&
      node->custom1 != modifier_target_stack_position &&
      node->custom1 != modifier_target_persistent_id)
  {
    node->custom1 = modifier_target_name;
  }
  if (node->custom2 != modifier_stack_first && node->custom2 != modifier_stack_last &&
      node->custom2 != modifier_stack_index)
  {
    node->custom2 = modifier_stack_last;
  }
  const auto set_available = [&](const UString identifier, const bool available) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
  };
  set_available("Modifier"_ustr, node->custom1 == modifier_target_name);
  set_available("Index"_ustr,
                node->custom1 == modifier_target_stack_position &&
                    node->custom2 == modifier_stack_index);
  set_available("Modifier ID"_ustr, node->custom1 == modifier_target_persistent_id);
}

enum AssignGeometryNodesModifierOperation {
  assign_geometry_modifier_append = 0,
  assign_geometry_modifier_insert = 1,
  assign_geometry_modifier_replace = 2,
};

enum AssignGeometryNodesModifierTarget {
  assign_geometry_modifier_target_name = 0,
  assign_geometry_modifier_target_first = 1,
  assign_geometry_modifier_target_last = 2,
  assign_geometry_modifier_target_index = 3,
  assign_geometry_modifier_target_persistent_id = 4,
};

static void declare_assign_geometry_nodes_modifier(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("Target object; an empty input uses the Logic Tree owner");
  b.add_input<decl::String>("Modifier Name"_ustr)
      .description("Optional name for a newly appended or inserted modifier");
  b.add_input<decl::String>("Modifier"_ustr)
      .description("Existing Geometry Nodes modifier name")
      .custom_draw(draw_geometry_setter_modifier);
  b.add_input<decl::Int>("Index"_ustr)
      .default_value(0)
      .min(0)
      .description("Exact zero-based insertion or replacement position");
  b.add_input<decl::Int>("Modifier ID"_ustr)
      .default_value(0)
      .min(0)
      .description("Stable runtime modifier ID returned by this node");
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Int>("Modifier ID"_ustr)
      .description("Stable ID of the created or replaced modifier; zero when the operation fails");
}

static void set_assign_geometry_nodes_modifier_availability(bNodeTree *ntree, bNode *node)
{
  if (ntree == nullptr || node == nullptr) {
    return;
  }
  if (!ELEM(node->custom1,
            assign_geometry_modifier_append,
            assign_geometry_modifier_insert,
            assign_geometry_modifier_replace))
  {
    node->custom1 = assign_geometry_modifier_append;
  }
  if (!ELEM(node->custom2,
            assign_geometry_modifier_target_name,
            assign_geometry_modifier_target_first,
            assign_geometry_modifier_target_last,
            assign_geometry_modifier_target_index,
            assign_geometry_modifier_target_persistent_id))
  {
    node->custom2 = assign_geometry_modifier_target_name;
  }
  const auto set_available = [&](const UString identifier, const bool available) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
  };
  const bool replace = node->custom1 == assign_geometry_modifier_replace;
  set_available("Modifier Name"_ustr, !replace);
  set_available("Modifier"_ustr,
                replace && node->custom2 == assign_geometry_modifier_target_name);
  set_available("Index"_ustr,
                node->custom1 == assign_geometry_modifier_insert ||
                    (replace && node->custom2 == assign_geometry_modifier_target_index));
  set_available("Modifier ID"_ustr,
                replace && node->custom2 == assign_geometry_modifier_target_persistent_id);
}

static void init_assign_geometry_nodes_modifier(bNodeTree *ntree, bNode *node)
{
  node->custom1 = assign_geometry_modifier_append;
  node->custom2 = assign_geometry_modifier_target_name;
  set_assign_geometry_nodes_modifier_availability(ntree, node);
}

static void layout_assign_geometry_nodes_modifier(ui::Layout &layout,
                                                  bContext *C,
                                                  PointerRNA *ptr)
{
  const bNode *node = ptr->data_as<bNode>();
  layout.prop(ptr, "operation", UI_ITEM_NONE, "Operation", ICON_NONE);
  template_id(&layout, C, ptr, "geometry_node_group", nullptr, nullptr, nullptr);
  if (node != nullptr && node->custom1 == assign_geometry_modifier_replace) {
    layout.prop(ptr, "replace_target", UI_ITEM_NONE, "Target", ICON_NONE);
  }
}

static void init_enable_disable_modifier(bNodeTree *ntree, bNode *node)
{
  node->custom1 = modifier_target_name;
  node->custom2 = modifier_stack_last;
  set_enable_disable_modifier_availability(ntree, node);
}

static void layout_enable_disable_modifier(ui::Layout &layout,
                                           bContext * /*C*/,
                                           PointerRNA *ptr)
{
  const bNode *node = ptr->data_as<bNode>();
  layout.prop(ptr, "target_mode", UI_ITEM_NONE, "Target", ICON_NONE);
  if (node != nullptr && node->custom1 == modifier_target_stack_position) {
    layout.prop(ptr, "stack_position", UI_ITEM_NONE, "Position", ICON_NONE);
  }
}

static void set_geometry_setter_availability(bNodeTree *ntree, bNode *node)
{
  if (ntree == nullptr || node == nullptr) {
    return;
  }
  if (!set_socket_value_type_is_valid(node->custom1)) {
    node->custom1 = set_socket_value_float;
  }
  const int value_type = node->custom1;
  const auto set_available = [&](const UString identifier, const bool available) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
  };
  set_available("Value"_ustr, value_type == set_socket_value_generic);
  set_available("Float Value"_ustr, value_type == set_socket_value_float);
  set_available("Integer Value"_ustr, value_type == set_socket_value_int);
  set_available("Boolean Value"_ustr, value_type == set_socket_value_bool);
  set_available("Vector Value"_ustr, value_type == set_socket_value_vector);
  set_available("Color Value"_ustr, value_type == set_socket_value_color);
  set_available("String Value"_ustr, value_type == set_socket_value_string);
  set_available("Material Value"_ustr, value_type == set_socket_value_material);
}

static void init_geometry_setter(bNodeTree *ntree, bNode *node)
{
  node->custom1 = set_socket_value_float;
  node->custom2 = 0;
  set_geometry_setter_availability(ntree, node);
}

enum class GeometrySetterPicker : uint8_t {
  Modifier,
  InterfaceInput,
};

struct GeometrySetterSearchData {
  int32_t node_id;
  GeometrySetterPicker picker;
  char socket_identifier[MAX_NAME];
  char display[256];
};

BLI_STATIC_ASSERT(std::is_trivially_copyable_v<GeometrySetterSearchData>, "");

struct GeometryInterfaceInputInfo {
  PointerRNA input_ptr = PointerRNA_NULL;
  PropertyRNA *value_prop = nullptr;
  int value_type = -1;
};

static bool geometry_setter_picker_prerequisite_is_dynamic(const bNode &node,
                                                           GeometrySetterPicker picker);

static bool geometry_setter_is_direct_socket_node(const bNode &node)
{
  if (STREQ(node.idname, "LogicNativeGetEditorNodeValue")) {
    return node.custom1 == 2;
  }
  if (STREQ(node.idname, "LogicNativeSetEditorNodeValue")) {
    return ((node.custom2 >> 4) & 0x3) == 2;
  }
  return false;
}

static bool geometry_setter_is_modifier_control(const bNode &node)
{
  return ELEM(StringRef(node.idname),
              StringRef("LogicNativeEnableDisableModifier"),
              StringRef("LogicNativeAssignGeometryNodesModifier"));
}

static bool geometry_setter_tree_is_bound_to_object(const bNodeTree &tree, const Object &object)
{
  for (const LogicNodeBinding *binding =
           static_cast<const LogicNodeBinding *>(object.logic_node_bindings.first);
       binding != nullptr;
       binding = binding->next)
  {
    if (STREQ(binding->tree_name, tree.id.name + 2)) {
      return true;
    }
  }
  return false;
}

static Object *geometry_setter_selected_object(const bNodeTree &tree,
                                               const bNode &node,
                                               const bContext &C)
{
  const bNodeSocket *object_socket = set_socket_find_input(node, "Object");
  if (object_socket == nullptr || object_socket->is_directly_linked()) {
    return nullptr;
  }
  if (object_socket->default_value != nullptr && object_socket->type == SOCK_OBJECT) {
    if (Object *object =
            static_cast<bNodeSocketValueObject *>(object_socket->default_value)->value)
    {
      return object;
    }
  }

  Object *active_object = CTX_data_active_object(&C);
  return active_object != nullptr &&
                 geometry_setter_tree_is_bound_to_object(tree, *active_object) ?
             active_object :
             nullptr;
}

static bool geometry_setter_input_is_linked(const bNode &node, const char *identifier)
{
  const bNodeSocket *socket = set_socket_find_input(node, identifier);
  return socket != nullptr && socket->is_directly_linked();
}

static bool geometry_setter_uses_dynamic_target(const bNode &node)
{
  if (geometry_setter_input_is_linked(node, "Object") ||
      geometry_setter_input_is_linked(node, "Modifier"))
  {
    return true;
  }
  if (geometry_setter_is_direct_socket_node(node)) {
    return geometry_setter_input_is_linked(node, "Node Name") ||
           geometry_setter_input_is_linked(node, "Socket");
  }
  return geometry_setter_input_is_linked(node, "Input");
}

static NodesModifierData *geometry_setter_modifier(Object *object, const char *name)
{
  if (object == nullptr || name == nullptr || name[0] == '\0') {
    return nullptr;
  }
  ModifierData *modifier = BKE_modifiers_findby_name(object, name);
  if (modifier == nullptr || modifier->type != eModifierType_Nodes) {
    return nullptr;
  }
  NodesModifierData *nodes_modifier = reinterpret_cast<NodesModifierData *>(modifier);
  return nodes_modifier->node_group != nullptr &&
                 nodes_modifier->node_group->type == NTREE_GEOMETRY ?
             nodes_modifier :
             nullptr;
}

static NodesModifierData *geometry_setter_selected_modifier(Object *object, const bNode &node)
{
  return geometry_setter_modifier(object, set_socket_string_input_value(node, "Modifier"));
}

static const bNodeTreeInterfaceSocket *geometry_setter_find_interface_input(
    bNodeTree &tree, const char *identifier_or_name)
{
  if (identifier_or_name == nullptr || identifier_or_name[0] == '\0') {
    return nullptr;
  }
  tree.ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    if (STREQ(socket->identifier, identifier_or_name)) {
      return socket;
    }
  }
  const bNodeTreeInterfaceSocket *name_match = nullptr;
  for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    if (STREQ(socket->name, identifier_or_name)) {
      if (name_match != nullptr) {
        return nullptr;
      }
      name_match = socket;
    }
  }
  return name_match;
}

static bNode *geometry_setter_find_node(bNodeTree &tree, const char *identifier_or_label)
{
  if (identifier_or_label == nullptr || identifier_or_label[0] == '\0') {
    return nullptr;
  }
  if (bNode *node = bke::node_find_node_by_name(tree, identifier_or_label)) {
    return node;
  }
  bNode *label_match = nullptr;
  for (bNode &node : tree.nodes) {
    if (node.label[0] != '\0' && STREQ(node.label, identifier_or_label)) {
      if (label_match != nullptr) {
        return nullptr;
      }
      label_match = &node;
    }
  }
  return label_match;
}

static bNodeSocket *geometry_setter_find_socket(bNode &node, const char *identifier_or_name)
{
  if (identifier_or_name == nullptr || identifier_or_name[0] == '\0') {
    return nullptr;
  }
  for (bNodeSocket &socket : node.inputs) {
    if (STREQ(socket.identifier, identifier_or_name)) {
      return &socket;
    }
  }
  bNodeSocket *name_match = nullptr;
  for (bNodeSocket &socket : node.inputs) {
    if (STREQ(socket.name, identifier_or_name)) {
      if (name_match != nullptr) {
        return nullptr;
      }
      name_match = &socket;
    }
  }
  return name_match;
}

static int geometry_setter_value_type_for_socket(const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_BOOLEAN:
      return set_socket_value_bool;
    case SOCK_INT:
    case SOCK_MENU:
      return set_socket_value_int;
    case SOCK_INT_VECTOR:
    case SOCK_VECTOR:
    case SOCK_ROTATION:
      return set_socket_value_vector;
    case SOCK_FLOAT:
      return set_socket_value_float;
    case SOCK_RGBA:
      return set_socket_value_color;
    case SOCK_STRING:
      return set_socket_value_string;
    case SOCK_MATERIAL:
      return set_socket_value_material;
    case SOCK_OBJECT:
    case SOCK_IMAGE:
    case SOCK_COLLECTION:
    case SOCK_TEXTURE:
    case SOCK_FONT:
    case SOCK_SCENE:
    case SOCK_TEXT_ID:
    case SOCK_MASK:
    case SOCK_SOUND:
      return set_socket_value_generic;
    default:
      return -1;
  }
}

static bool geometry_setter_supported_socket(const bNodeSocket &socket)
{
  if (socket.default_value == nullptr || (socket.flag & SOCK_UNAVAIL) != 0 ||
      socket.is_directly_linked() || geometry_setter_value_type_for_socket(socket) < 0)
  {
    return false;
  }
  if (socket.type == SOCK_MENU) {
    const bNodeSocketValueMenu &menu = *static_cast<const bNodeSocketValueMenu *>(
        socket.default_value);
    return menu.enum_items != nullptr && !menu.has_conflict() && !menu.enum_items->items.is_empty();
  }
  return true;
}

static bool geometry_setter_node_has_supported_socket(bNodeTree &tree, const bNode &node)
{
  tree.ensure_topology_cache();
  for (const bNodeSocket &socket : node.inputs) {
    if (geometry_setter_supported_socket(socket)) {
      return true;
    }
  }
  return false;
}

static bool geometry_setter_interface_input_info(bContext &C,
                                                 Object &object,
                                                 NodesModifierData &modifier,
                                                 const bNodeTreeInterfaceSocket &interface_socket,
                                                 GeometryInterfaceInputInfo &r_info)
{
  PointerRNA modifier_ptr = RNA_pointer_create_discrete(&object.id, RNA_NodesModifier, &modifier);
  PointerRNA properties_ptr = RNA_pointer_get(&modifier_ptr, "properties");
  PointerRNA inputs_ptr = RNA_pointer_get(&properties_ptr, "inputs");
  r_info.input_ptr = RNA_pointer_get(&inputs_ptr, interface_socket.identifier);
  if (r_info.input_ptr.data == nullptr) {
    return false;
  }

  PropertyRNA *type_prop = RNA_struct_find_property(&r_info.input_ptr, "type");
  int value_mode = 0;
  if (type_prop == nullptr ||
      !RNA_property_enum_value(&C, &r_info.input_ptr, type_prop, "VALUE", &value_mode) ||
      RNA_property_enum_get(&r_info.input_ptr, type_prop) != value_mode)
  {
    return false;
  }

  r_info.value_prop = RNA_struct_find_property(&r_info.input_ptr, "value");
  if (r_info.value_prop == nullptr || !RNA_property_editable(&r_info.input_ptr, r_info.value_prop))
  {
    return false;
  }
  const int array_length = RNA_property_array_check(r_info.value_prop) ?
                               RNA_property_array_length(&r_info.input_ptr, r_info.value_prop) :
                               0;
  if (array_length > 4) {
    return false;
  }

  switch (RNA_property_type(r_info.value_prop)) {
    case PROP_BOOLEAN:
      r_info.value_type = array_length == 0 ? set_socket_value_bool : -1;
      break;
    case PROP_INT:
      r_info.value_type = array_length == 0 ? set_socket_value_int :
                          array_length <= 3 ? set_socket_value_vector :
                                              set_socket_value_generic;
      break;
    case PROP_FLOAT:
      if (array_length == 0) {
        r_info.value_type = set_socket_value_float;
      }
      else if (interface_socket.socket_typeinfo() != nullptr &&
               interface_socket.socket_typeinfo()->type == SOCK_RGBA)
      {
        r_info.value_type = set_socket_value_color;
      }
      else {
        r_info.value_type = array_length <= 3 ? set_socket_value_vector : set_socket_value_generic;
      }
      break;
    case PROP_STRING:
      r_info.value_type = array_length == 0 ? set_socket_value_string : -1;
      break;
    case PROP_ENUM:
      r_info.value_type = array_length == 0 ? set_socket_value_int : -1;
      break;
    case PROP_POINTER:
      r_info.value_type = interface_socket.socket_typeinfo() != nullptr &&
                                  interface_socket.socket_typeinfo()->type == SOCK_MATERIAL ?
                              set_socket_value_material :
                              set_socket_value_generic;
      break;
    default:
      r_info.value_type = -1;
      break;
  }
  return r_info.value_type >= 0;
}

static const char *geometry_setter_value_socket_identifier(const int value_type)
{
  switch (value_type) {
    case set_socket_value_int:
      return "Integer Value";
    case set_socket_value_bool:
      return "Boolean Value";
    case set_socket_value_vector:
      return "Vector Value";
    case set_socket_value_color:
      return "Color Value";
    case set_socket_value_string:
      return "String Value";
    case set_socket_value_material:
      return "Material Value";
    case set_socket_value_generic:
      return "Value";
    case set_socket_value_float:
    default:
      return "Float Value";
  }
}

static bNodeSocket *geometry_setter_value_socket(bNode &node, const int value_type)
{
  return set_socket_find_input(node, geometry_setter_value_socket_identifier(value_type));
}

static void geometry_setter_set_material_value(bNodeSocket &socket, Material *material)
{
  Material *&current = static_cast<bNodeSocketValueMaterial *>(socket.default_value)->value;
  if (current == material) {
    return;
  }
  if (material != nullptr) {
    id_us_plus(&material->id);
  }
  if (current != nullptr) {
    id_us_min(&current->id);
  }
  current = material;
}

static void geometry_setter_set_value_type(bNodeTree &tree, bNode &node, const int value_type)
{
  if (!set_socket_value_type_is_valid(value_type)) {
    return;
  }
  node.custom1 = value_type;
  set_geometry_setter_availability(&tree, &node);
}

static void geometry_setter_copy_interface_value(bNode &logic_node,
                                                 const GeometryInterfaceInputInfo &info)
{
  bNodeSocket *value_socket = geometry_setter_value_socket(logic_node, info.value_type);
  if (value_socket == nullptr || value_socket->default_value == nullptr ||
      info.value_prop == nullptr)
  {
    return;
  }

  PointerRNA input_ptr = info.input_ptr;
  switch (info.value_type) {
    case set_socket_value_bool:
      static_cast<bNodeSocketValueBoolean *>(value_socket->default_value)->value =
          RNA_property_boolean_get(&input_ptr, info.value_prop);
      break;
    case set_socket_value_int:
      static_cast<bNodeSocketValueInt *>(value_socket->default_value)->value =
          RNA_property_type(info.value_prop) == PROP_ENUM ?
              RNA_property_enum_get(&input_ptr, info.value_prop) :
              RNA_property_int_get(&input_ptr, info.value_prop);
      break;
    case set_socket_value_float:
      static_cast<bNodeSocketValueFloat *>(value_socket->default_value)->value =
          RNA_property_float_get(&input_ptr, info.value_prop);
      break;
    case set_socket_value_vector: {
      float values[3] = {0.0f, 0.0f, 0.0f};
      const int length = std::min(RNA_property_array_length(&input_ptr, info.value_prop), 3);
      if (RNA_property_type(info.value_prop) == PROP_INT) {
        int int_values[3] = {0, 0, 0};
        RNA_property_int_get_array_at_most(&input_ptr, info.value_prop, int_values, length);
        for (int i = 0; i < length; i++) {
          values[i] = float(int_values[i]);
        }
      }
      else {
        RNA_property_float_get_array_at_most(&input_ptr, info.value_prop, values, length);
      }
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(value_socket->default_value)->value,
                 values);
      break;
    }
    case set_socket_value_color: {
      float values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      const int length = std::min(RNA_property_array_length(&input_ptr, info.value_prop), 4);
      RNA_property_float_get_array_at_most(&input_ptr, info.value_prop, values, length);
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(value_socket->default_value)->value, values);
      break;
    }
    case set_socket_value_string:
      STRNCPY(static_cast<bNodeSocketValueString *>(value_socket->default_value)->value,
              RNA_property_string_get(&input_ptr, info.value_prop).c_str());
      break;
    case set_socket_value_material: {
      PointerRNA material_ptr = RNA_property_pointer_get(&input_ptr, info.value_prop);
      geometry_setter_set_material_value(*value_socket,
                                         static_cast<Material *>(material_ptr.data));
      break;
    }
    default:
      break;
  }
}

static void geometry_setter_copy_socket_value(bNode &logic_node, const bNodeSocket &source)
{
  const int value_type = geometry_setter_value_type_for_socket(source);
  bNodeSocket *value_socket = geometry_setter_value_socket(logic_node, value_type);
  if (value_socket == nullptr || value_socket->default_value == nullptr ||
      source.default_value == nullptr)
  {
    return;
  }

  switch (source.type) {
    case SOCK_BOOLEAN:
      static_cast<bNodeSocketValueBoolean *>(value_socket->default_value)->value =
          static_cast<const bNodeSocketValueBoolean *>(source.default_value)->value;
      break;
    case SOCK_INT:
      static_cast<bNodeSocketValueInt *>(value_socket->default_value)->value =
          static_cast<const bNodeSocketValueInt *>(source.default_value)->value;
      break;
    case SOCK_MENU:
      static_cast<bNodeSocketValueInt *>(value_socket->default_value)->value =
          static_cast<const bNodeSocketValueMenu *>(source.default_value)->value;
      break;
    case SOCK_FLOAT:
      static_cast<bNodeSocketValueFloat *>(value_socket->default_value)->value =
          static_cast<const bNodeSocketValueFloat *>(source.default_value)->value;
      break;
    case SOCK_INT_VECTOR: {
      const bNodeSocketValueIntVector &source_value =
          *static_cast<const bNodeSocketValueIntVector *>(source.default_value);
      bNodeSocketValueVector &target_value = *static_cast<bNodeSocketValueVector *>(
          value_socket->default_value);
      for (int i = 0; i < 3; i++) {
        target_value.value[i] = float(source_value.value[i]);
      }
      break;
    }
    case SOCK_VECTOR:
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(value_socket->default_value)->value,
                 static_cast<const bNodeSocketValueVector *>(source.default_value)->value);
      break;
    case SOCK_ROTATION:
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(value_socket->default_value)->value,
                 static_cast<const bNodeSocketValueRotation *>(source.default_value)->value_euler);
      break;
    case SOCK_RGBA:
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(value_socket->default_value)->value,
                 static_cast<const bNodeSocketValueRGBA *>(source.default_value)->value);
      break;
    case SOCK_STRING:
      STRNCPY(static_cast<bNodeSocketValueString *>(value_socket->default_value)->value,
              static_cast<const bNodeSocketValueString *>(source.default_value)->value);
      break;
    case SOCK_MATERIAL:
      geometry_setter_set_material_value(
          *value_socket,
          static_cast<const bNodeSocketValueMaterial *>(source.default_value)->value);
      break;
    default:
      break;
  }
}

static bool geometry_setter_search_context(const bContext &C,
                                           const GeometrySetterSearchData &data,
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
  if (r_node == nullptr) {
    return false;
  }
  if (data.picker == GeometrySetterPicker::InterfaceInput) {
    return STREQ(r_node->idname, "LogicNativeSetGeometryNodesInput");
  }
  return geometry_setter_is_direct_socket_node(*r_node) ||
         STREQ(r_node->idname, "LogicNativeSetGeometryNodesInput") ||
         STREQ(r_node->idname, "LogicNativeMakeNodeTreeUnique") ||
         geometry_setter_is_modifier_control(*r_node);
}

static bool geometry_setter_search_item_add(ui::SearchItems *items,
                                            const StringRef name,
                                            const StringRef identifier,
                                            void *item,
                                            const int icon)
{
  if (identifier.is_empty() || name == identifier) {
    return ui::search_item_add(items, name, item, icon, 0, 0);
  }
  const std::string text = name + UI_SEP_CHAR_S + identifier;
  return ui::search_item_add(items, text, item, icon, ui::BUT_HAS_SEP_CHAR, 0);
}

static void geometry_setter_search_update(const bContext *C,
                                          void *data_v,
                                          const char *search_string,
                                          ui::SearchItems *items,
                                          const bool is_first)
{
  GeometrySetterSearchData &data = *static_cast<GeometrySetterSearchData *>(data_v);
  bNodeTree *logic_tree = nullptr;
  bNode *logic_node = nullptr;
  if (C == nullptr || !geometry_setter_search_context(*C, data, logic_tree, logic_node)) {
    return;
  }

  Object *object = geometry_setter_selected_object(*logic_tree, *logic_node, *C);
  if (object == nullptr) {
    return;
  }
  const StringRef query = is_first ? StringRef() : StringRef(search_string);

  if (data.picker == GeometrySetterPicker::Modifier) {
    const bool any_modifier = geometry_setter_is_modifier_control(*logic_node);
    ui::string_search::StringSearch<ModifierData> search;
    for (ModifierData &modifier : object->modifiers) {
      if (!any_modifier) {
        if (modifier.type != eModifierType_Nodes) {
          continue;
        }
        NodesModifierData &nodes_modifier = reinterpret_cast<NodesModifierData &>(modifier);
        if (nodes_modifier.node_group == nullptr ||
            nodes_modifier.node_group->type != NTREE_GEOMETRY)
        {
          continue;
        }
      }
      search.add(modifier.name, &modifier);
    }
    for (ModifierData *modifier : search.query(query)) {
      if (!geometry_setter_search_item_add(
              items,
              modifier->name,
              {},
              modifier,
              any_modifier ? ICON_MODIFIER : ICON_GEOMETRY_NODES))
      {
        break;
      }
    }
    return;
  }

  NodesModifierData *modifier = geometry_setter_selected_modifier(object, *logic_node);
  if (modifier == nullptr || modifier->node_group == nullptr) {
    return;
  }
  bNodeTree &geometry_tree = *modifier->node_group;

  if (data.picker == GeometrySetterPicker::InterfaceInput) {
    geometry_tree.ensure_interface_cache();
    ui::string_search::StringSearch<const bNodeTreeInterfaceSocket> search;
    for (const bNodeTreeInterfaceSocket *interface_socket : geometry_tree.interface_inputs()) {
      GeometryInterfaceInputInfo info;
      if (!geometry_setter_interface_input_info(
              *const_cast<bContext *>(C), *object, *modifier, *interface_socket, info))
      {
        continue;
      }
      const std::string searchable = std::string(interface_socket->name) + " " +
                                     interface_socket->identifier;
      search.add(searchable, interface_socket);
    }
    for (const bNodeTreeInterfaceSocket *interface_socket : search.query(query)) {
      if (!geometry_setter_search_item_add(
              items,
              interface_socket->name,
              interface_socket->identifier,
              const_cast<bNodeTreeInterfaceSocket *>(interface_socket),
              ICON_NONE))
      {
        break;
      }
    }
    return;
  }

}

static void geometry_setter_search_exec(bContext *C, void *data_v, void *item_v)
{
  if (C == nullptr || item_v == nullptr) {
    return;
  }
  GeometrySetterSearchData &data = *static_cast<GeometrySetterSearchData *>(data_v);
  bNodeTree *logic_tree = nullptr;
  bNode *logic_node = nullptr;
  if (!geometry_setter_search_context(*C, data, logic_tree, logic_node)) {
    return;
  }

  bNodeSocket *picker_socket = set_socket_find_input(*logic_node, data.socket_identifier);
  if (picker_socket == nullptr || picker_socket->is_directly_linked() ||
      geometry_setter_picker_prerequisite_is_dynamic(*logic_node, data.picker))
  {
    return;
  }
  Object *object = geometry_setter_selected_object(*logic_tree, *logic_node, *C);
  if (object == nullptr) {
    return;
  }

  bool changed = false;
  if (data.picker == GeometrySetterPicker::Modifier) {
    const bool any_modifier = geometry_setter_is_modifier_control(*logic_node);
    ModifierData *selected = nullptr;
    for (ModifierData &modifier : object->modifiers) {
      if (&modifier != item_v) {
        continue;
      }
      if (any_modifier) {
        selected = &modifier;
      }
      else if (modifier.type == eModifierType_Nodes) {
        NodesModifierData &nodes_modifier = reinterpret_cast<NodesModifierData &>(modifier);
        if (nodes_modifier.node_group != nullptr &&
            nodes_modifier.node_group->type == NTREE_GEOMETRY)
        {
          selected = &modifier;
        }
      }
      break;
    }
    if (selected == nullptr) {
      return;
    }
    NodesModifierData *previous_nodes = any_modifier ? nullptr :
                                                      geometry_setter_selected_modifier(
                                                          object, *logic_node);
    ModifierData *previous = any_modifier ?
                                 BKE_modifiers_findby_name(
                                     object,
                                     set_socket_string_input_value(*logic_node, "Modifier")) :
                                 previous_nodes ? &previous_nodes->modifier : nullptr;
    if (previous == selected) {
      return;
    }
    set_socket_set_string_input_value(*logic_node, "Modifier", selected->name);
    if (any_modifier) {
      /* The modifier control has no dependent picker fields. */
    }
    else if (geometry_setter_is_direct_socket_node(*logic_node)) {
      set_socket_set_string_input_value(*logic_node, "Node Name", "");
      set_socket_set_string_input_value(*logic_node, "Socket", "");
    }
    else {
      set_socket_set_string_input_value(*logic_node, "Input", "");
    }
    changed = true;
  }
  else {
    NodesModifierData *modifier = geometry_setter_selected_modifier(object, *logic_node);
    if (modifier == nullptr || modifier->node_group == nullptr) {
      return;
    }
    bNodeTree &geometry_tree = *modifier->node_group;

    geometry_tree.ensure_interface_cache();
    const bNodeTreeInterfaceSocket *selected = nullptr;
    for (const bNodeTreeInterfaceSocket *interface_socket : geometry_tree.interface_inputs()) {
      if (interface_socket == item_v) {
        selected = interface_socket;
        break;
      }
    }
    GeometryInterfaceInputInfo info;
    if (selected == nullptr ||
        !geometry_setter_interface_input_info(*C, *object, *modifier, *selected, info))
    {
      return;
    }
    set_socket_set_string_input_value(*logic_node, "Input", selected->identifier);
    geometry_setter_set_value_type(*logic_tree, *logic_node, info.value_type);
    geometry_setter_copy_interface_value(*logic_node, info);
    changed = true;
  }

  if (!changed) {
    return;
  }
  BKE_ntree_update_tag_node_property(logic_tree, logic_node);
  BKE_main_ensure_invariants(*CTX_data_main(C), logic_tree->id);
  ED_undo_push(C,
               geometry_setter_is_modifier_control(*logic_node) ? "Select Modifier" :
                                                                  "Select Geometry Nodes Target");
}

static ui::Block *geometry_setter_search_popup(bContext *C, ARegion *region, void *data_v)
{
  static char search[256] = "";
  search[0] = '\0';

  /* Own a stable copy because the parent button may redraw while this popup is open. */
  GeometrySetterSearchData *search_data = MEM_new<GeometrySetterSearchData>(
      __func__, *static_cast<const GeometrySetterSearchData *>(data_v));
  const int width = ui::searchbox_size_x_guess(C, geometry_setter_search_update, search_data);
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
                                             "Search available modifier or Geometry Nodes targets");
  button_placeholder_set(search_button, "Search...");
  button_func_search_set_results_are_suggestions(search_button, true);
  button_func_search_set_sep_string(search_button, UI_MENU_ARROW_SEP);
  button_func_search_set(search_button,
                         nullptr,
                         geometry_setter_search_update,
                         search_data,
                         true,
                         nullptr,
                         geometry_setter_search_exec,
                         nullptr);

  ui::block_bounds_set_normal(block, UI_SEARCHBOX_BOUNDS);
  ui::block_direction_set(block, ui::UI_DIR_DOWN);
  ui::button_focus_on_enter_event(CTX_wm_window(C), search_button);
  return block;
}

static bool geometry_setter_picker_display(bContext &C,
                                           bNodeTree &logic_tree,
                                           bNode &logic_node,
                                           const GeometrySetterPicker picker,
                                           char *r_display,
                                           bool &r_enabled)
{
  r_display[0] = '\0';
  r_enabled = false;
  Object *object = geometry_setter_selected_object(logic_tree, logic_node, C);
  if (object == nullptr) {
    return false;
  }

  if (picker == GeometrySetterPicker::Modifier) {
    r_enabled = true;
    const char *stored = set_socket_string_input_value(logic_node, "Modifier");
    if (geometry_setter_is_modifier_control(logic_node)) {
      ModifierData *modifier = BKE_modifiers_findby_name(object, stored);
      if (modifier != nullptr) {
        BLI_strncpy(r_display, modifier->name, 256);
        return true;
      }
      BLI_strncpy(r_display, stored, 256);
      return stored[0] == '\0';
    }
    NodesModifierData *modifier = geometry_setter_modifier(object, stored);
    if (modifier != nullptr) {
      BLI_strncpy(r_display, modifier->modifier.name, 256);
      return true;
    }
    BLI_strncpy(r_display, stored, 256);
    return stored[0] == '\0';
  }

  NodesModifierData *modifier = geometry_setter_selected_modifier(object, logic_node);
  if (modifier == nullptr || modifier->node_group == nullptr) {
    return false;
  }
  r_enabled = true;
  bNodeTree &geometry_tree = *modifier->node_group;

  if (picker == GeometrySetterPicker::InterfaceInput) {
    const char *stored = set_socket_string_input_value(logic_node, "Input");
    const bNodeTreeInterfaceSocket *interface_socket = geometry_setter_find_interface_input(
        geometry_tree, stored);
    GeometryInterfaceInputInfo info;
    if (interface_socket != nullptr &&
        geometry_setter_interface_input_info(C, *object, *modifier, *interface_socket, info))
    {
      BLI_strncpy(r_display, interface_socket->name, 256);
      geometry_setter_set_value_type(logic_tree, logic_node, info.value_type);
      return true;
    }
    BLI_strncpy(r_display, stored, 256);
    return stored[0] == '\0';
  }

  return false;
}

static const char *geometry_setter_picker_label(const GeometrySetterPicker picker)
{
  switch (picker) {
    case GeometrySetterPicker::Modifier:
      return "Modifier:";
    case GeometrySetterPicker::InterfaceInput:
      return "Input:";
  }
  return "";
}

static const char *geometry_setter_picker_placeholder(const GeometrySetterPicker picker)
{
  switch (picker) {
    case GeometrySetterPicker::Modifier:
      return "Select Modifier";
    case GeometrySetterPicker::InterfaceInput:
      return "Select Modifier Input";
  }
  return "";
}

static bool geometry_setter_picker_prerequisite_is_dynamic(const bNode &node,
                                                           const GeometrySetterPicker picker)
{
  if (geometry_setter_input_is_linked(node, "Object")) {
    return true;
  }
  if (picker == GeometrySetterPicker::Modifier) {
    return false;
  }
  return geometry_setter_input_is_linked(node, "Modifier");
}

static void draw_geometry_setter_picker(CustomSocketDrawParams &params,
                                        const GeometrySetterPicker picker)
{
  if (params.socket.is_directly_linked() ||
      geometry_setter_picker_prerequisite_is_dynamic(params.node, picker) ||
      geometry_setter_selected_object(params.tree, params.node, params.C) == nullptr)
  {
    params.draw_standard(params.layout);
    return;
  }

  GeometrySetterSearchData *data = MEM_new_zeroed<GeometrySetterSearchData>(__func__);
  data->node_id = params.node.identifier;
  data->picker = picker;
  STRNCPY(data->socket_identifier, params.socket.identifier);
  bool enabled = true;
  const bool valid = geometry_setter_picker_display(*const_cast<bContext *>(&params.C),
                                                    params.tree,
                                                    params.node,
                                                    picker,
                                                    data->display,
                                                    enabled);

  params.layout.alignment_set(ui::LayoutAlign::Expand);
  ui::Layout &row = params.layout.row(true);
  row.label(geometry_setter_picker_label(picker), ICON_NONE);
  ui::Block *block = row.block();
  ui::Button *button = uiDefBlockButN(block,
                                      geometry_setter_search_popup,
                                      data,
                                      data->display,
                                      0,
                                      0,
                                      10 * UI_UNIT_X,
                                      UI_UNIT_Y,
                                      geometry_setter_picker_placeholder(picker));
  button_drawflag_enable(button, ui::BUT_TEXT_LEFT);
  if (!valid && data->display[0] != '\0') {
    button_flag_enable(button, ui::BUT_REDALERT);
  }
  row.enabled_set(enabled);
}

static void draw_geometry_setter_modifier(CustomSocketDrawParams &params)
{
  draw_geometry_setter_picker(params, GeometrySetterPicker::Modifier);
}

static void draw_geometry_setter_interface_input(CustomSocketDrawParams &params)
{
  draw_geometry_setter_picker(params, GeometrySetterPicker::InterfaceInput);
}

static void layout_geometry_setter(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  const bNode *node = ptr->data_as<bNode>();
  const bNodeTree *tree = reinterpret_cast<const bNodeTree *>(ptr->owner_id);
  if (node == nullptr || tree == nullptr || C == nullptr ||
      geometry_setter_uses_dynamic_target(*node) ||
      geometry_setter_selected_object(*tree, *node, *C) == nullptr)
  {
    layout.prop(ptr, "value_type", UI_ITEM_NONE, "Value Type", ICON_NONE);
  }
}

enum class NodeMutePicker : uint8_t {
  Tree,
  Node,
  Socket,
};

enum NodeMuteEditorType {
  node_mute_editor_unset = 0,
  node_mute_editor_shader = 1,
  node_mute_editor_geometry = 2,
  node_mute_editor_compositor = 3,
};

static constexpr int editor_value_owner_target = 1 << 0;
static constexpr int editor_value_set_editor_shift = 4;
static constexpr int editor_value_set_editor_mask = 0x3 << editor_value_set_editor_shift;

static bool editor_value_is_getter(const bNode &node)
{
  return STREQ(node.idname, "LogicNativeGetEditorNodeValue");
}

static bool editor_value_is_setter(const bNode &node)
{
  return STREQ(node.idname, "LogicNativeSetEditorNodeValue");
}

static bool editor_value_is_unique(const bNode &node)
{
  return STREQ(node.idname, "LogicNativeMakeNodeTreeUnique");
}

static bool editor_value_is_node(const bNode &node)
{
  return editor_value_is_getter(node) || editor_value_is_setter(node);
}

static int editor_value_editor_type(const bNode &node)
{
  if (editor_value_is_setter(node)) {
    const int editor = (node.custom2 & editor_value_set_editor_mask) >>
                       editor_value_set_editor_shift;
    return ELEM(editor,
                node_mute_editor_shader,
                node_mute_editor_geometry,
                node_mute_editor_compositor) ?
               editor :
               node_mute_editor_shader;
  }
  return ELEM(node.custom1,
              node_mute_editor_shader,
              node_mute_editor_geometry,
              node_mute_editor_compositor) ?
             node.custom1 :
             node_mute_editor_shader;
}

static bool editor_value_uses_owner(const bNode &node)
{
  const int editor = editor_value_editor_type(node);
  if (editor == node_mute_editor_geometry) {
    return true;
  }
  if (editor == node_mute_editor_compositor) {
    return false;
  }
  return (node.custom2 & editor_value_owner_target) != 0;
}

static int editor_value_slot(const bNode &node)
{
  const bNodeSocket *socket = set_socket_find_input(node, "Slot");
  return socket != nullptr && socket->type == SOCK_INT && socket->default_value != nullptr ?
             static_cast<const bNodeSocketValueInt *>(socket->default_value)->value :
             -1;
}

struct NodeMuteSearchData {
  int32_t node_id;
  NodeMutePicker picker;
  char display[256];
};

BLI_STATIC_ASSERT(std::is_trivially_copyable_v<NodeMuteSearchData>, "");

static char node_mute_clear_tree_item;
static char node_mute_clear_node_item;

static const bNodeTree *node_mute_target_tree(const ID *id)
{
  if (id == nullptr) {
    return nullptr;
  }
  switch (GS(id->name)) {
    case ID_MA:
      return reinterpret_cast<const Material *>(id)->nodetree;
    case ID_SCE:
      return reinterpret_cast<const Scene *>(id)->compositing_node_group;
    case ID_NT:
      return reinterpret_cast<const bNodeTree *>(id);
    default:
      return nullptr;
  }
}

static bNodeTree *node_mute_target_tree(ID *id)
{
  return const_cast<bNodeTree *>(node_mute_target_tree(static_cast<const ID *>(id)));
}

static bool node_mute_tree_supported(const bNodeTree *tree)
{
  return tree != nullptr && ELEM(tree->type, NTREE_SHADER, NTREE_GEOMETRY, NTREE_COMPOSIT);
}

static int node_mute_editor_from_target(const ID *id)
{
  const bNodeTree *tree = node_mute_target_tree(id);
  if (tree == nullptr) {
    return node_mute_editor_shader;
  }
  switch (tree->type) {
    case NTREE_GEOMETRY:
      return node_mute_editor_geometry;
    case NTREE_COMPOSIT:
      return node_mute_editor_compositor;
    case NTREE_SHADER:
    default:
      return node_mute_editor_shader;
  }
}

static bool node_mute_target_supported(const ID *id, int editor_type);

static int node_mute_editor_type(bNode &node)
{
  if (editor_value_is_node(node) || editor_value_is_unique(node)) {
    return editor_value_editor_type(node);
  }
  if (!ELEM(node.custom1,
            node_mute_editor_shader,
            node_mute_editor_geometry,
            node_mute_editor_compositor))
  {
    node.custom1 = node_mute_editor_from_target(node.id);
  }
  return node.custom1;
}

static bool editor_value_target_supported(const bNode &logic_node, const ID *id)
{
  if (id == nullptr) {
    return false;
  }
  if (editor_value_is_unique(logic_node)) {
    return editor_value_editor_type(logic_node) == node_mute_editor_compositor &&
           GS(id->name) == ID_SCE &&
           node_mute_target_supported(id, node_mute_editor_compositor);
  }
  if (editor_value_is_node(logic_node)) {
    switch (editor_value_editor_type(logic_node)) {
      case node_mute_editor_shader:
        return !editor_value_uses_owner(logic_node) && GS(id->name) == ID_MA &&
               node_mute_target_supported(id, node_mute_editor_shader);
      case node_mute_editor_compositor:
        return node_mute_target_supported(id, node_mute_editor_compositor);
      default:
        return false;
    }
  }
  return node_mute_target_supported(id, node_mute_editor_type(const_cast<bNode &>(logic_node)));
}

static bNodeTree *editor_value_target_tree(const bNodeTree &logic_tree,
                                           bNode &logic_node,
                                           const bContext *C)
{
  if (!editor_value_is_node(logic_node)) {
    return node_mute_target_tree(logic_node.id);
  }

  const int editor = editor_value_editor_type(logic_node);
  if (!editor_value_uses_owner(logic_node)) {
    return editor_value_target_supported(logic_node, logic_node.id) ?
               node_mute_target_tree(logic_node.id) :
               nullptr;
  }
  if (C == nullptr) {
    return nullptr;
  }
  Object *object = geometry_setter_selected_object(logic_tree, logic_node, *C);
  if (object == nullptr) {
    return nullptr;
  }
  if (editor == node_mute_editor_shader) {
    const int slot = editor_value_slot(logic_node);
    Material *material = slot >= 0 && slot < object->totcol ?
                             BKE_object_material_get(object, short(slot + 1)) :
                             nullptr;
    return material != nullptr ? material->nodetree : nullptr;
  }
  if (editor == node_mute_editor_geometry) {
    NodesModifierData *modifier = geometry_setter_selected_modifier(object, logic_node);
    return modifier != nullptr && modifier->node_group != nullptr &&
                   modifier->node_group->type == NTREE_GEOMETRY ?
               modifier->node_group :
               nullptr;
  }
  return nullptr;
}

static bool node_mute_target_supported(const ID *id, const int editor_type)
{
  const bNodeTree *tree = node_mute_target_tree(id);
  if (!node_mute_tree_supported(tree)) {
    return false;
  }
  switch (editor_type) {
    case node_mute_editor_shader:
      return GS(id->name) != ID_SCE && tree->type == NTREE_SHADER;
    case node_mute_editor_geometry:
      return GS(id->name) == ID_NT && tree->type == NTREE_GEOMETRY;
    case node_mute_editor_compositor:
      return ELEM(GS(id->name), ID_SCE, ID_NT) && tree->type == NTREE_COMPOSIT;
    default:
      return false;
  }
}

static bool node_mute_is_scene_compositor(const Main &bmain, const bNodeTree &tree)
{
  for (const Scene &scene : bmain.scenes) {
    if (scene.compositing_node_group == &tree) {
      return true;
    }
  }
  return false;
}

static std::string node_mute_target_display(const ID &id)
{
  const char *name = id.name + 2;
  switch (GS(id.name)) {
    case ID_MA:
      return std::string("Material: ") + name;
    case ID_SCE:
      return std::string("Scene: ") + name;
    case ID_NT:
      return std::string("Group: ") + name;
    default:
      return name;
  }
}

static int node_mute_target_icon(const ID &id)
{
  switch (GS(id.name)) {
    case ID_MA:
      return ICON_MATERIAL;
    case ID_SCE:
      return ICON_SCENE_DATA;
    case ID_NT: {
      const bNodeTree &tree = reinterpret_cast<const bNodeTree &>(id);
      return tree.type == NTREE_GEOMETRY ? ICON_GEOMETRY_NODES : ICON_NODETREE;
    }
    default:
      return ICON_NODETREE;
  }
}

static bool node_mute_search_context(const bContext &C,
                                     const NodeMuteSearchData &data,
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
  return r_node != nullptr &&
         ELEM(StringRef(r_node->idname),
              StringRef("LogicNativeSetNodeMute"),
              StringRef("LogicNativeGetEditorNodeValue"),
              StringRef("LogicNativeSetEditorNodeValue"),
              StringRef("LogicNativeMakeNodeTreeUnique"));
}

static void node_mute_search_update(const bContext *C,
                                    void *data_v,
                                    const char *search_string,
                                    ui::SearchItems *items,
                                    const bool is_first)
{
  NodeMuteSearchData &data = *static_cast<NodeMuteSearchData *>(data_v);
  bNodeTree *logic_tree = nullptr;
  bNode *logic_node = nullptr;
  if (C == nullptr || !node_mute_search_context(*C, data, logic_tree, logic_node)) {
    return;
  }
  const StringRef query = is_first ? StringRef() : StringRef(search_string);

  if (data.picker == NodeMutePicker::Tree) {
    if (query.is_empty() &&
        !ui::search_item_add(items, "None", &node_mute_clear_tree_item, ICON_X, 0, 0))
    {
      return;
    }
    Main *bmain = CTX_data_main(C);
    if (bmain == nullptr) {
      return;
    }
    ui::string_search::StringSearch<ID> search;
    for (Material &material : bmain->materials) {
      if (editor_value_target_supported(*logic_node, &material.id)) {
        search.add(node_mute_target_display(material.id), &material.id);
      }
    }
    for (Scene &scene : bmain->scenes) {
      if (editor_value_target_supported(*logic_node, &scene.id)) {
        search.add(node_mute_target_display(scene.id), &scene.id);
      }
    }
    for (bNodeTree &tree : bmain->nodetrees) {
      if (editor_value_target_supported(*logic_node, &tree.id) &&
          !node_mute_is_scene_compositor(*bmain, tree))
      {
        search.add(node_mute_target_display(tree.id), &tree.id);
      }
    }
    for (ID *id : search.query(query)) {
      const std::string display = node_mute_target_display(*id);
      if (!geometry_setter_search_item_add(
              items, display, id->name + 2, id, node_mute_target_icon(*id)))
      {
        break;
      }
    }
    return;
  }

  bNodeTree *target_tree = editor_value_target_tree(*logic_tree, *logic_node, C);
  if (target_tree == nullptr) {
    return;
  }
  if (data.picker == NodeMutePicker::Node && query.is_empty() &&
      !ui::search_item_add(items, "None", &node_mute_clear_node_item, ICON_X, 0, 0))
  {
    return;
  }
  if (data.picker == NodeMutePicker::Node) {
    ui::string_search::StringSearch<bNode> search;
    for (bNode &node : target_tree->nodes) {
      const bool supported = editor_value_is_node(*logic_node) ?
                                 geometry_setter_node_has_supported_socket(*target_tree, node) :
                                 node.typeinfo != nullptr && !node.typeinfo->no_muting;
      if (!supported) {
        continue;
      }
      const StringRef display = node.label[0] != '\0' ? StringRef(node.label) :
                                                         StringRef(node.name);
      search.add(display + " " + node.name, &node);
    }
    for (bNode *node : search.query(query)) {
      const StringRef display = node->label[0] != '\0' ? StringRef(node->label) :
                                                          StringRef(node->name);
      if (!geometry_setter_search_item_add(items, display, node->name, node, ICON_NODETREE)) {
        break;
      }
    }
    return;
  }

  bNode *target_node = geometry_setter_find_node(
      *target_tree, set_socket_string_input_value(*logic_node, "Node Name"));
  if (target_node == nullptr) {
    return;
  }
  ui::string_search::StringSearch<bNodeSocket> search;
  for (bNodeSocket &socket : target_node->inputs) {
    if (geometry_setter_supported_socket(socket)) {
      search.add(std::string(socket.name) + " " + socket.identifier, &socket);
    }
  }
  for (bNodeSocket *socket : search.query(query)) {
    if (!geometry_setter_search_item_add(
            items, socket->name, socket->identifier, socket, ICON_NONE))
    {
      break;
    }
  }
}

static void node_mute_search_exec(bContext *C, void *data_v, void *item_v)
{
  if (C == nullptr || item_v == nullptr) {
    return;
  }
  NodeMuteSearchData &data = *static_cast<NodeMuteSearchData *>(data_v);
  bNodeTree *logic_tree = nullptr;
  bNode *logic_node = nullptr;
  if (!node_mute_search_context(*C, data, logic_tree, logic_node)) {
    return;
  }

  bool changed = false;
  if (data.picker == NodeMutePicker::Tree) {
    ID *selected = item_v == &node_mute_clear_tree_item ? nullptr : static_cast<ID *>(item_v);
    if (selected != nullptr && !editor_value_target_supported(*logic_node, selected))
    {
      return;
    }
    if (selected != logic_node->id) {
      if (selected != nullptr) {
        id_us_plus(selected);
      }
      if (logic_node->id != nullptr) {
        id_us_min(logic_node->id);
      }
      logic_node->id = selected;
      changed = true;
    }
    if (set_socket_string_input_value(*logic_node, "Node Name")[0] != '\0') {
      set_socket_set_string_input_value(*logic_node, "Node Name", "");
      changed = true;
    }
    if (editor_value_is_node(*logic_node) &&
        set_socket_string_input_value(*logic_node, "Socket")[0] != '\0')
    {
      set_socket_set_string_input_value(*logic_node, "Socket", "");
      changed = true;
    }
  }
  else if (data.picker == NodeMutePicker::Node) {
    bNodeTree *target_tree = editor_value_target_tree(*logic_tree, *logic_node, C);
    if (target_tree == nullptr) {
      return;
    }
    bNode *selected = nullptr;
    if (item_v != &node_mute_clear_node_item) {
      for (bNode &node : target_tree->nodes) {
        const bool supported = editor_value_is_node(*logic_node) ?
                                   geometry_setter_node_has_supported_socket(*target_tree, node) :
                                   node.typeinfo != nullptr && !node.typeinfo->no_muting;
        if (&node == item_v && supported) {
          selected = &node;
          break;
        }
      }
      if (selected == nullptr) {
        return;
      }
    }
    const char *name = selected != nullptr ? selected->name : "";
    if (!STREQ(set_socket_string_input_value(*logic_node, "Node Name"), name)) {
      set_socket_set_string_input_value(*logic_node, "Node Name", name);
      changed = true;
    }
    if (editor_value_is_node(*logic_node) &&
        set_socket_string_input_value(*logic_node, "Socket")[0] != '\0')
    {
      set_socket_set_string_input_value(*logic_node, "Socket", "");
      changed = true;
    }
  }
  else {
    bNodeTree *target_tree = editor_value_target_tree(*logic_tree, *logic_node, C);
    bNode *target_node = target_tree != nullptr ?
                             geometry_setter_find_node(
                                 *target_tree,
                                 set_socket_string_input_value(*logic_node, "Node Name")) :
                             nullptr;
    if (target_node == nullptr) {
      return;
    }
    bNodeSocket *selected = nullptr;
    for (bNodeSocket &socket : target_node->inputs) {
      if (&socket == item_v && geometry_setter_supported_socket(socket)) {
        selected = &socket;
        break;
      }
    }
    if (selected == nullptr) {
      return;
    }
    if (!STREQ(set_socket_string_input_value(*logic_node, "Socket"), selected->identifier)) {
      set_socket_set_string_input_value(*logic_node, "Socket", selected->identifier);
      changed = true;
    }
    if (editor_value_is_setter(*logic_node)) {
      geometry_setter_set_value_type(
          *logic_tree, *logic_node, geometry_setter_value_type_for_socket(*selected));
      geometry_setter_copy_socket_value(*logic_node, *selected);
      changed = true;
    }
  }

  if (!changed) {
    return;
  }
  BKE_ntree_update_tag_node_property(logic_tree, logic_node);
  BKE_main_ensure_invariants(*CTX_data_main(C), logic_tree->id);
  ED_undo_push(C,
               data.picker == NodeMutePicker::Tree ? "Select Node Tree" :
               data.picker == NodeMutePicker::Node ? "Select Node" :
                                                     "Select Input Socket");
}

static ui::Block *node_mute_search_popup(bContext *C, ARegion *region, void *data_v)
{
  static char search[256] = "";
  search[0] = '\0';

  NodeMuteSearchData *search_data = MEM_new<NodeMuteSearchData>(
      __func__, *static_cast<const NodeMuteSearchData *>(data_v));
  const int width = ui::searchbox_size_x_guess(C, node_mute_search_update, search_data);
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
                                             "Search available node trees or nodes");
  button_placeholder_set(search_button, "Search...");
  button_func_search_set_results_are_suggestions(search_button, true);
  button_func_search_set_sep_string(search_button, UI_MENU_ARROW_SEP);
  button_func_search_set(search_button,
                         nullptr,
                         node_mute_search_update,
                         search_data,
                         true,
                         nullptr,
                         node_mute_search_exec,
                         nullptr);

  ui::block_bounds_set_normal(block, UI_SEARCHBOX_BOUNDS);
  ui::block_direction_set(block, ui::UI_DIR_DOWN);
  ui::button_focus_on_enter_event(CTX_wm_window(C), search_button);
  return block;
}

static bNode *node_mute_selected_node(const bNode &logic_node)
{
  bNodeTree *tree = node_mute_target_tree(logic_node.id);
  const char *name = set_socket_string_input_value(logic_node, "Node Name");
  if (!node_mute_target_supported(logic_node.id, logic_node.custom1) || name[0] == '\0') {
    return nullptr;
  }
  if (bNode *node = bke::node_find_node_by_name(*tree, name)) {
    return node->typeinfo != nullptr && !node->typeinfo->no_muting ? node : nullptr;
  }
  bNode *label_match = nullptr;
  for (bNode &node : tree->nodes) {
    if (node.typeinfo == nullptr || node.typeinfo->no_muting || node.label[0] == '\0' ||
        !STREQ(node.label, name))
    {
      continue;
    }
    if (label_match != nullptr) {
      return nullptr;
    }
    label_match = &node;
  }
  return label_match;
}

static void set_node_mute_state(bNodeTree * /*tree*/, bNode *node)
{
  if (node == nullptr) {
    return;
  }
  const int editor_type = node_mute_editor_type(*node);
  if (node->id == nullptr) {
    if (set_socket_string_input_value(*node, "Node Name")[0] != '\0') {
      set_socket_set_string_input_value(*node, "Node Name", "");
    }
    return;
  }
  if (node_mute_target_supported(node->id, editor_type)) {
    return;
  }
  id_us_min(node->id);
  node->id = nullptr;
  set_socket_set_string_input_value(*node, "Node Name", "");
}

static void init_set_node_mute(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = node_mute_editor_shader;
}

static void set_editor_value_state(bNodeTree *tree, bNode *node)
{
  if (tree == nullptr || node == nullptr) {
    return;
  }
  const int editor = editor_value_editor_type(*node);
  const bool is_value_node = editor_value_is_node(*node);
  const bool owner = is_value_node && editor_value_uses_owner(*node);
  const bool needs_static_target = editor == node_mute_editor_compositor ||
                                   (is_value_node && editor == node_mute_editor_shader && !owner);

  if (!needs_static_target ||
      (node->id != nullptr && !editor_value_target_supported(*node, node->id)))
  {
    if (node->id != nullptr) {
      id_us_min(node->id);
      node->id = nullptr;
    }
  }

  const auto set_available = [&](const UString identifier, const bool available) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, available);
    }
  };
  const bool material_owner = editor == node_mute_editor_shader &&
                              (owner || editor_value_is_unique(*node));
  const bool geometry_owner = editor == node_mute_editor_geometry;
  set_available("Object"_ustr, material_owner || geometry_owner);
  set_available("Slot"_ustr, material_owner);
  set_available("Modifier"_ustr, geometry_owner);
  set_available("Node Name"_ustr, is_value_node);
  set_available("Socket"_ustr, is_value_node);

  if (editor_value_is_setter(*node)) {
    if (!set_socket_value_type_is_valid(node->custom1)) {
      node->custom1 = set_socket_value_float;
    }
    const int value_type = node->custom1;
    set_available("Value"_ustr, value_type == set_socket_value_generic);
    set_available("Float Value"_ustr, value_type == set_socket_value_float);
    set_available("Integer Value"_ustr, value_type == set_socket_value_int);
    set_available("Boolean Value"_ustr, value_type == set_socket_value_bool);
    set_available("Vector Value"_ustr, value_type == set_socket_value_vector);
    set_available("Color Value"_ustr, value_type == set_socket_value_color);
    set_available("String Value"_ustr, value_type == set_socket_value_string);
    set_available("Material Value"_ustr, value_type == set_socket_value_material);
  }
}

static void init_set_editor_node_value(bNodeTree *tree, bNode *node)
{
  node->custom1 = set_socket_value_float;
  node->custom2 = (node_mute_editor_shader << editor_value_set_editor_shift) |
                  editor_value_owner_target;
  set_editor_value_state(tree, node);
}

static void init_get_editor_node_value(bNodeTree *tree, bNode *node)
{
  node->custom1 = node_mute_editor_shader;
  node->custom2 = editor_value_owner_target;
  set_editor_value_state(tree, node);
}

static void init_make_node_tree_unique(bNodeTree *tree, bNode *node)
{
  node->custom1 = node_mute_editor_shader;
  node->custom2 = 0;
  set_editor_value_state(tree, node);
}

static void draw_node_mute_tree(ui::Layout &layout, bNode &node)
{
  NodeMuteSearchData *data = MEM_new_zeroed<NodeMuteSearchData>(__func__);
  data->node_id = node.identifier;
  data->picker = NodeMutePicker::Tree;
  if (node.id != nullptr) {
    const std::string display = node_mute_target_display(*node.id);
    BLI_strncpy(data->display, display.c_str(), sizeof(data->display));
  }

  layout.alignment_set(ui::LayoutAlign::Expand);
  ui::Layout &row = layout.row(true);
  row.label("Node Tree:", ICON_NONE);
  ui::Button *button = uiDefBlockButN(
      row.block(),
      node_mute_search_popup,
      data,
      data->display,
      0,
      0,
      10 * UI_UNIT_X,
      UI_UNIT_Y,
      "Select a shared node tree from the chosen editor; changes affect all users");
  button_drawflag_enable(button, ui::BUT_TEXT_LEFT);
  if (node.id != nullptr && !editor_value_target_supported(node, node.id)) {
    button_flag_enable(button, ui::BUT_REDALERT);
  }
}

static bool editor_value_needs_tree_picker(const bNode &node)
{
  const int editor = editor_value_editor_type(node);
  if (editor_value_is_unique(node)) {
    return editor == node_mute_editor_compositor;
  }
  return editor == node_mute_editor_compositor ||
         (editor == node_mute_editor_shader && !editor_value_uses_owner(node));
}

static void draw_editor_value_header(ui::Layout &layout, bNode &node, PointerRNA &node_ptr)
{
  layout.prop(&node_ptr, "node_editor_type", UI_ITEM_NONE, "Node Editor", ICON_NONE);
  if (editor_value_is_node(node) &&
      editor_value_editor_type(node) == node_mute_editor_shader)
  {
    layout.prop(&node_ptr, "target_scope", UI_ITEM_NONE, "Target", ICON_NONE);
  }
  if (editor_value_needs_tree_picker(node)) {
    draw_node_mute_tree(layout, node);
  }
}

static void layout_get_editor_node_value(ui::Layout &layout,
                                         bContext * /*C*/,
                                         PointerRNA *ptr)
{
  bNode *node = ptr != nullptr ? ptr->data_as<bNode>() : nullptr;
  if (node != nullptr) {
    draw_editor_value_header(layout, *node, *ptr);
  }
}

static void draw_editor_value_flow(CustomSocketDrawParams &params)
{
  set_editor_value_state(&params.tree, &params.node);
  ui::Layout &column = params.layout.column(false);
  params.draw_standard(column);
  draw_editor_value_header(column, params.node, params.node_ptr);
}

static void draw_make_node_tree_unique_flow(CustomSocketDrawParams &params)
{
  set_editor_value_state(&params.tree, &params.node);
  ui::Layout &column = params.layout.column(false);
  params.draw_standard(column);
  draw_editor_value_header(column, params.node, params.node_ptr);
}

static void draw_editor_value_modifier(CustomSocketDrawParams &params)
{
  if (editor_value_editor_type(params.node) == node_mute_editor_geometry) {
    draw_geometry_setter_picker(params, GeometrySetterPicker::Modifier);
  }
  else {
    params.draw_standard(params.layout);
  }
}

static void draw_node_mute_flow(CustomSocketDrawParams &params)
{
  set_node_mute_state(&params.tree, &params.node);
  ui::Layout &column = params.layout.column(false);
  params.draw_standard(column);
  column.prop(
      &params.node_ptr, "node_editor_type", UI_ITEM_NONE, "Node Editor", ICON_NONE);
  draw_node_mute_tree(column, params.node);
}

static void draw_node_mute_node(CustomSocketDrawParams &params)
{
  if (params.socket.is_directly_linked()) {
    params.draw_standard(params.layout);
    return;
  }

  NodeMuteSearchData *data = MEM_new_zeroed<NodeMuteSearchData>(__func__);
  data->node_id = params.node.identifier;
  data->picker = NodeMutePicker::Node;
  const char *stored = set_socket_string_input_value(params.node, "Node Name");
  bNode *selected = node_mute_selected_node(params.node);
  BLI_strncpy(data->display,
              selected != nullptr ?
                  (selected->label[0] != '\0' ? selected->label : selected->name) :
                  stored,
              sizeof(data->display));

  params.layout.alignment_set(ui::LayoutAlign::Expand);
  ui::Layout &row = params.layout.row(true);
  row.label("Node:", ICON_NONE);
  ui::Button *button = uiDefBlockButN(row.block(),
                                      node_mute_search_popup,
                                      data,
                                      data->display,
                                      0,
                                      0,
                                      10 * UI_UNIT_X,
                                      UI_UNIT_Y,
                                      "Select a muteable node in the chosen shared node tree");
  button_drawflag_enable(button, ui::BUT_TEXT_LEFT);
  if (stored[0] != '\0' && selected == nullptr) {
    button_flag_enable(button, ui::BUT_REDALERT);
  }
  row.enabled_set(node_mute_target_supported(params.node.id, params.node.custom1));
}

static void draw_editor_value_picker(CustomSocketDrawParams &params,
                                     const NodeMutePicker picker)
{
  if (params.socket.is_directly_linked()) {
    params.draw_standard(params.layout);
    return;
  }

  bNodeTree *target_tree = editor_value_target_tree(params.tree, params.node, &params.C);
  if (target_tree == nullptr) {
    params.draw_standard(params.layout);
    return;
  }
  const char *stored = set_socket_string_input_value(
      params.node, picker == NodeMutePicker::Node ? "Node Name" : "Socket");
  bNode *target_node = target_tree != nullptr ?
                           geometry_setter_find_node(
                               *target_tree,
                               set_socket_string_input_value(params.node, "Node Name")) :
                           nullptr;
  bNodeSocket *target_socket = picker == NodeMutePicker::Socket && target_node != nullptr ?
                                   geometry_setter_find_socket(*target_node, stored) :
                                   nullptr;
  const bool valid = picker == NodeMutePicker::Node ?
                         target_node != nullptr && target_tree != nullptr &&
                             geometry_setter_node_has_supported_socket(*target_tree, *target_node) :
                         target_socket != nullptr && geometry_setter_supported_socket(*target_socket);

  NodeMuteSearchData *data = MEM_new_zeroed<NodeMuteSearchData>(__func__);
  data->node_id = params.node.identifier;
  data->picker = picker;
  if (valid) {
    const char *display = picker == NodeMutePicker::Node ?
                              (target_node->label[0] != '\0' ? target_node->label :
                                                                target_node->name) :
                              target_socket->name;
    BLI_strncpy(data->display, display, sizeof(data->display));
  }
  else {
    BLI_strncpy(data->display, stored, sizeof(data->display));
  }

  params.layout.alignment_set(ui::LayoutAlign::Expand);
  ui::Layout &row = params.layout.row(true);
  row.label(picker == NodeMutePicker::Node ? "Node:" : "Socket:", ICON_NONE);
  ui::Button *button = uiDefBlockButN(
      row.block(),
      node_mute_search_popup,
      data,
      data->display,
      0,
      0,
      10 * UI_UNIT_X,
      UI_UNIT_Y,
      picker == NodeMutePicker::Node ? "Select a node with a supported input socket" :
                                       "Select an unlinked input socket with a supported value");
  button_drawflag_enable(button, ui::BUT_TEXT_LEFT);
  if (stored[0] != '\0' && !valid) {
    button_flag_enable(button, ui::BUT_REDALERT);
  }
  row.enabled_set(picker == NodeMutePicker::Node || target_node != nullptr);
}

static void draw_editor_value_node(CustomSocketDrawParams &params)
{
  draw_editor_value_picker(params, NodeMutePicker::Node);
}

static void draw_editor_value_socket(CustomSocketDrawParams &params)
{
  draw_editor_value_picker(params, NodeMutePicker::Socket);
}

static void declare_play_material_sequence(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Material>("Material"_ustr);
  b.add_input<decl::String>("Node Name"_ustr);
  b.add_input<decl::Int>("Mode"_ustr).default_value(0);
  b.add_input<decl::Bool>("Continue"_ustr);
  b.add_input<decl::Vector>("Frames"_ustr);
  b.add_input<decl::Float>("FPS"_ustr).default_value(60.0f).min(0.0f);
  b.add_output<decl::Execution>("On Start"_ustr);
  b.add_output<decl::Execution>("Running"_ustr);
  b.add_output<decl::Execution>("On Finish"_ustr);
  b.add_output<decl::Float>("Current Frame"_ustr);
}

static void declare_combine_xyzw(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("X"_ustr);
  b.add_input<decl::Float>("Y"_ustr);
  b.add_input<decl::Float>("Z"_ustr);
  b.add_input<decl::Float>("W"_ustr);
  b.add_output<decl::Vector>("Vector"_ustr);
}

static void declare_vector_in_out(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Vector"_ustr);
  b.add_output<decl::Vector>("Vector"_ustr);
}

static void declare_xyz_to_matrix(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("XYZ"_ustr);
  b.add_output<decl::Generic>("Matrix"_ustr);
}

static void declare_matrix_to_xyz(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Generic>("Matrix"_ustr);
  b.add_output<decl::Vector>("Vector"_ustr);
}

static void declare_vector_rotate(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Origin"_ustr);
  b.add_input<decl::Vector>("Pivot"_ustr);
  b.add_input<decl::Vector>("Axis"_ustr).default_value({0.0f, 0.0f, 1.0f});
  b.add_input<decl::Float>("Angle"_ustr);
  b.add_input<decl::Rotation>("Euler"_ustr);
  b.add_output<decl::Vector>("Point"_ustr);
}

static void declare_vector_to_rotation(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Direction"_ustr).default_value({0.0f, 0.0f, 1.0f});
  b.add_input<decl::Vector>("Up Reference"_ustr, "Up"_ustr).default_value({0.0f, 1.0f, 0.0f});
  b.add_output<decl::Rotation>("Rotation"_ustr);
}

static void declare_file_path(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>(""_ustr, "Value"_ustr);
  b.add_output<decl::String>("Path"_ustr);
}

static void declare_start_speaker(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Speaker"_ustr);
  b.add_input<decl::Bool>("Use Occlusion"_ustr);
  b.add_input<decl::Float>("Transition"_ustr).default_value(0.1f).min(0.0f).max(1.0f);
  b.add_input<decl::Float>("Lowpass"_ustr).default_value(0.1f).min(0.0f).max(1.0f);
  b.add_input<decl::Int>("Mode"_ustr).default_value(0);
  b.add_input<decl::Bool>("Ignore Timescale"_ustr);
  b.add_output<decl::Execution>("On Start"_ustr);
  b.add_output<decl::Execution>("On Finish"_ustr);
  b.add_output<decl::Generic>("Sound"_ustr);
}

static void declare_get_sound(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Sound>("Sound File"_ustr);
  b.add_output<decl::Sound>("Sound File"_ustr);
}

static void declare_get_image(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Image>("Image"_ustr);
  b.add_output<decl::Image>("Image"_ustr);
}

static void declare_get_font(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Font>("Font"_ustr);
  b.add_output<decl::Font>("Font"_ustr);
}

static void declare_get_object_id(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_output<decl::String>("ID"_ustr);
}

static void declare_get_axis_vector(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_output<decl::Vector>("Vector"_ustr);
}

static void declare_draw_line(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Color>("Color"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Vector>("From"_ustr);
  b.add_input<decl::Vector>("To"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void declare_draw_cube(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Color>("Color"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Vector>("Origin"_ustr);
  b.add_input<decl::Float>("Width"_ustr).default_value(1.0f).min(0.0f);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void declare_draw_box(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Color>("Color"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Vector>("Origin"_ustr);
  b.add_input<decl::Float>("Width"_ustr, "Width (X)"_ustr).default_value(1.0f).min(0.0f);
  b.add_input<decl::Float>("Length"_ustr, "Length (Y)"_ustr).default_value(1.0f).min(0.0f);
  b.add_input<decl::Float>("Height"_ustr, "Height (Z)"_ustr).default_value(1.0f).min(0.0f);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void declare_draw(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Color>("Color"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Vector>("Origin"_ustr);
  b.add_input<decl::Vector>("Target"_ustr);
  b.add_input<decl::List>("Points"_ustr);
  b.add_input<decl::Float>("Width"_ustr).default_value(1.0f).min(0.0f);
  b.add_input<decl::Float>("Length"_ustr).default_value(1.0f).min(0.0f);
  b.add_input<decl::Float>("Height"_ustr).default_value(1.0f).min(0.0f);
  b.add_input<decl::Object>("Object"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void declare_join_path(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Path"_ustr);
  b.add_input<decl::String>("Path_001"_ustr, "Path"_ustr);
  b.add_output<decl::String>("Path"_ustr);
}

static void declare_get_master_folder(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Name"_ustr).default_value("Data");
  b.add_output<decl::String>("Path"_ustr);
}

static void declare_load_file_content(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_output<decl::Execution>("Loaded"_ustr);
  b.add_output<decl::Execution>("Updated"_ustr);
  b.add_output<decl::Float>("Status"_ustr).min(0.0f).max(1.0f);
  b.add_output<decl::String>("Datatype"_ustr);
  b.add_output<decl::String>("Item"_ustr);
}

static void declare_set_custom_cursor(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Image>("Texture"_ustr);
  b.add_input<decl::Vector>("Size"_ustr).default_value({30.0f, 30.0f, 0.0f});
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::UI>("Cursor"_ustr);
}

static void setup_node_type(bke::bNodeType &ntype,
                            const char *idname,
                            const int nclass,
                            void (*declare_fn)(NodeDeclarationBuilder &),
                            void (*layout_fn)(ui::Layout &, bContext *, PointerRNA *) = nullptr)
{
  logic_node_type_base(&ntype, UString(idname));
  ntype.nclass = nclass;
  ntype.declare = declare_fn;
  ntype.draw_buttons = layout_fn;
}

#define REGISTER_C_TIER_NODE(func, idname, nclass, declare_fn, layout_fn) \
  static void func() \
  { \
    static bke::bNodeType ntype; \
    setup_node_type(ntype, idname, nclass, declare_fn, layout_fn); \
    bke::node_register_type(ntype); \
  }

REGISTER_C_TIER_NODE(register_get_bone_attribute,
                     "LogicNativeGetBoneAttribute",
                     NODE_CLASS_OP_VECTOR,
                     declare_get_bone_attribute,
                     layout_bone_attribute)

static void register_set_bone_attribute()
{
  static bke::bNodeType ntype;
  setup_node_type(ntype,
                  "LogicNativeSetBoneAttribute",
                  NODE_CLASS_OP_VECTOR,
                  declare_set_bone_attribute,
                  layout_bone_set_attribute);
  ntype.initfunc = init_set_bone_attribute;
  bke::node_register_type(ntype);
}

REGISTER_C_TIER_NODE(register_set_constraint_target,
                     "LogicNativeSetBoneConstraintTarget",
                     NODE_CLASS_OP_VECTOR,
                     declare_set_constraint_target,
                     nullptr)
REGISTER_C_TIER_NODE(register_set_constraint_attribute,
                     "LogicNativeSetBoneConstraintAttribute",
                     NODE_CLASS_OP_VECTOR,
                     declare_set_constraint_attribute,
                     nullptr)
REGISTER_C_TIER_NODE(register_get_material_from_slot,
                     "LogicNativeGetMaterialFromSlot",
                     NODE_CLASS_INPUT,
                     declare_get_material_from_slot,
                     nullptr)
REGISTER_C_TIER_NODE(register_get_material_slot_count,
                     "LogicNativeGetMaterialSlotCount",
                     NODE_CLASS_INPUT,
                     declare_get_material_slot_count,
                     nullptr)
REGISTER_C_TIER_NODE(register_get_material_name,
                     "LogicNativeGetMaterialName",
                     NODE_CLASS_INPUT,
                     declare_get_material_name,
                     nullptr)
static void register_get_material_parameter()
{
  static bke::bNodeType ntype;
  setup_node_type(ntype,
                  "LogicNativeGetMaterialParameter",
                  NODE_CLASS_INPUT,
                  declare_get_material_parameter,
                  nullptr);
  ntype.initfunc = init_set_material_parameter;
  ntype.updatefunc = update_set_node_socket;
  bke::node_register_type(ntype);
}
static void register_set_material_parameter()
{
  static bke::bNodeType ntype;
  setup_node_type(ntype,
                  "LogicNativeSetMaterialParameter",
                  NODE_CLASS_OP_COLOR,
                  declare_set_material_parameter,
                  nullptr);
  ntype.initfunc = init_set_material_parameter;
  ntype.updatefunc = update_set_node_socket;
  bke::node_register_type(ntype);
}
static void register_set_geometry_nodes_input()
{
  static bke::bNodeType ntype;
  setup_node_type(ntype,
                  "LogicNativeSetGeometryNodesInput",
                  NODE_CLASS_OP_VECTOR,
                  declare_set_geometry_nodes_input,
                  layout_geometry_setter);
  ntype.initfunc = init_geometry_setter;
  ntype.updatefunc = set_geometry_setter_availability;
  bke::node_register_type(ntype);
}
static void register_get_editor_node_value()
{
  static bke::bNodeType ntype;
  setup_node_type(ntype,
                  "LogicNativeGetEditorNodeValue",
                  NODE_CLASS_INPUT,
                  declare_get_editor_node_value);
  ntype.initfunc = init_get_editor_node_value;
  ntype.updatefunc = set_editor_value_state;
  ntype.draw_buttons = layout_get_editor_node_value;
  bke::node_register_type(ntype);
}
static void register_set_editor_node_value()
{
  static bke::bNodeType ntype;
  setup_node_type(ntype,
                  "LogicNativeSetEditorNodeValue",
                  NODE_CLASS_OP_VECTOR,
                  declare_set_editor_node_value);
  ntype.initfunc = init_set_editor_node_value;
  ntype.updatefunc = set_editor_value_state;
  bke::node_register_type(ntype);
}
static void register_make_node_tree_unique()
{
  static bke::bNodeType ntype;
  setup_node_type(ntype,
                  "LogicNativeMakeNodeTreeUnique",
                  NODE_CLASS_OP_VECTOR,
                  declare_make_node_tree_unique);
  ntype.initfunc = init_make_node_tree_unique;
  ntype.updatefunc = set_editor_value_state;
  bke::node_register_type(ntype);
}
static void register_set_node_mute()
{
  static bke::bNodeType ntype;
  setup_node_type(ntype,
                  "LogicNativeSetNodeMute",
                  NODE_CLASS_OP_VECTOR,
                  declare_set_node_mute);
  ntype.initfunc = init_set_node_mute;
  ntype.updatefunc = set_node_mute_state;
  bke::node_register_type(ntype);
}
static void register_enable_disable_modifier()
{
  static bke::bNodeType ntype;
  setup_node_type(ntype,
                  "LogicNativeEnableDisableModifier",
                  NODE_CLASS_OP_VECTOR,
                  declare_enable_disable_modifier,
                  layout_enable_disable_modifier);
  ntype.initfunc = init_enable_disable_modifier;
  ntype.updatefunc = set_enable_disable_modifier_availability;
  bke::node_register_type(ntype);
}
static void register_assign_geometry_nodes_modifier()
{
  static bke::bNodeType ntype;
  setup_node_type(ntype,
                  "LogicNativeAssignGeometryNodesModifier",
                  NODE_CLASS_OP_VECTOR,
                  declare_assign_geometry_nodes_modifier,
                  layout_assign_geometry_nodes_modifier);
  ntype.initfunc = init_assign_geometry_nodes_modifier;
  ntype.updatefunc = set_assign_geometry_nodes_modifier_availability;
  bke::node_register_type(ntype);
}
REGISTER_C_TIER_NODE(register_get_node_group_socket_value,
                     "LogicNativeGetNodeGroupSocketValue",
                     NODE_CLASS_INPUT,
                     declare_get_tree_socket,
                     nullptr)
REGISTER_C_TIER_NODE(register_set_node_group_socket_value,
                     "LogicNativeSetNodeGroupSocketValue",
                     NODE_CLASS_OP_VECTOR,
                     declare_set_tree_socket,
                     nullptr)
REGISTER_C_TIER_NODE(register_play_material_sequence,
                     "LogicNativePlayMaterialSequence",
                     NODE_CLASS_OP_VECTOR,
                     declare_play_material_sequence,
                     nullptr)
REGISTER_C_TIER_NODE(register_combine_xyzw,
                     "LogicNativeCombineXYZW",
                     NODE_CLASS_CONVERTER,
                     declare_combine_xyzw,
                     nullptr)
REGISTER_C_TIER_NODE(register_resize_vector,
                     "LogicNativeResizeVector",
                     NODE_CLASS_CONVERTER,
                     declare_vector_in_out,
                     layout_resize_vector)
REGISTER_C_TIER_NODE(register_xyz_to_matrix,
                     "LogicNativeXYZToMatrix",
                     NODE_CLASS_CONVERTER,
                     declare_xyz_to_matrix,
                     nullptr)
REGISTER_C_TIER_NODE(register_matrix_to_xyz,
                     "LogicNativeMatrixToXYZ",
                     NODE_CLASS_CONVERTER,
                     declare_matrix_to_xyz,
                     layout_matrix_to_xyz)
REGISTER_C_TIER_NODE(register_vector_rotate,
                     "LogicNativeVectorRotate",
                     NODE_CLASS_CONVERTER,
                     declare_vector_rotate,
                     layout_vector_rotate)

static void update_vector_to_rotation(bNodeTree *ntree, bNode *node)
{
  if (ntree == nullptr || node == nullptr) {
    return;
  }
  if (bNodeSocket *socket = node->input_by_identifier("Up"_ustr)) {
    bke::node_set_socket_availability(*ntree, *socket, (node->custom2 & 1) != 0);
  }
}

static void init_vector_to_rotation(bNodeTree *tree, bNode *node)
{
  node->custom1 = 2;
  node->custom2 = 0;
  update_vector_to_rotation(tree, node);
}

static void register_vector_to_rotation()
{
  static bke::bNodeType ntype;
  setup_node_type(ntype,
                  "LogicNativeVectorToRotation",
                  NODE_CLASS_CONVERTER,
                  declare_vector_to_rotation,
                  layout_vector_to_rotation);
  ntype.initfunc = init_vector_to_rotation;
  ntype.updatefunc = update_vector_to_rotation;
  bke::node_register_type(ntype);
}

REGISTER_C_TIER_NODE(
    register_file_path, "LogicNativeFilePath", NODE_CLASS_INPUT, declare_file_path, nullptr)
REGISTER_C_TIER_NODE(register_start_speaker,
                     "LogicNativeStartSpeaker",
                     NODE_CLASS_OP_VECTOR,
                     declare_start_speaker,
                     nullptr)
REGISTER_C_TIER_NODE(
    register_get_sound, "LogicNativeGetSound", NODE_CLASS_INPUT, declare_get_sound, nullptr)
REGISTER_C_TIER_NODE(
    register_get_image, "LogicNativeGetImage", NODE_CLASS_INPUT, declare_get_image, nullptr)
REGISTER_C_TIER_NODE(
    register_get_font, "LogicNativeGetFont", NODE_CLASS_INPUT, declare_get_font, nullptr)
REGISTER_C_TIER_NODE(register_get_object_id,
                     "LogicNativeGetObjectID",
                     NODE_CLASS_INPUT,
                     declare_get_object_id,
                     nullptr)
REGISTER_C_TIER_NODE(register_get_axis_vector,
                     "LogicNativeGetAxisVector",
                     NODE_CLASS_OP_VECTOR,
                     declare_get_axis_vector,
                     layout_axis)
REGISTER_C_TIER_NODE(
    register_draw_line, "LogicNativeDrawLine", NODE_CLASS_OP_VECTOR, declare_draw_line, nullptr)
REGISTER_C_TIER_NODE(register_draw_cube,
                     "LogicNativeDrawCube",
                     NODE_CLASS_OP_VECTOR,
                     declare_draw_cube,
                     layout_volume_origin)
REGISTER_C_TIER_NODE(register_draw_box,
                     "LogicNativeDrawBox",
                     NODE_CLASS_OP_VECTOR,
                     declare_draw_box,
                     layout_volume_origin)
REGISTER_C_TIER_NODE(
    register_draw, "LogicNativeDraw", NODE_CLASS_OP_VECTOR, declare_draw, layout_draw)
REGISTER_C_TIER_NODE(
    register_join_path, "LogicNativeJoinPath", NODE_CLASS_CONVERTER, declare_join_path, nullptr)
REGISTER_C_TIER_NODE(register_get_master_folder,
                     "LogicNativeGetMasterFolder",
                     NODE_CLASS_INPUT,
                     declare_get_master_folder,
                     nullptr)
REGISTER_C_TIER_NODE(register_load_file_content,
                     "LogicNativeLoadFileContent",
                     NODE_CLASS_OP_VECTOR,
                     declare_load_file_content,
                     nullptr)
REGISTER_C_TIER_NODE(register_set_custom_cursor,
                     "LogicNativeSetCustomCursor",
                     NODE_CLASS_OP_VECTOR,
                     declare_set_custom_cursor,
                     nullptr)

#undef REGISTER_C_TIER_NODE

static void node_register()
{
  register_get_bone_attribute();
  register_set_bone_attribute();
  register_set_constraint_target();
  register_set_constraint_attribute();
  register_get_material_from_slot();
  register_get_material_slot_count();
  register_get_material_name();
  register_get_material_parameter();
  register_set_material_parameter();
  register_set_geometry_nodes_input();
  register_get_editor_node_value();
  register_set_editor_node_value();
  register_make_node_tree_unique();
  register_set_node_mute();
  register_enable_disable_modifier();
  register_assign_geometry_nodes_modifier();
  register_get_node_group_socket_value();
  register_set_node_group_socket_value();
  register_play_material_sequence();
  register_combine_xyzw();
  register_resize_vector();
  register_xyz_to_matrix();
  register_matrix_to_xyz();
  register_vector_rotate();
  register_vector_to_rotation();
  register_file_path();
  register_start_speaker();
  register_get_sound();
  register_get_image();
  register_get_font();
  register_get_object_id();
  register_get_axis_vector();
  register_draw_line();
  register_draw_cube();
  register_draw_box();
  register_draw();
  register_join_path();
  register_get_master_folder();
  register_load_file_content();
  register_set_custom_cursor();
}

NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_c_tier_cc
