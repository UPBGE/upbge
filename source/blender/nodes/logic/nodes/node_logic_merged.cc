/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Merged Logic Nodes (2026-11-07, NOD_logic_merged.hh): one node type per table entry. Each option
 * is an older node: the node's declaration, buttons, init and update are the older node's own,
 * called for the option the node holds in custom4. Changing the option (RNA "variant",
 * rna_nodetree.cc) runs the new option's init, so custom1 / custom2 start as that older node's.
 * The older nodes are no node types of their own since 2026-11-08: they live in
 * node_logic_<merged>_options.cc and register only for this file (logic_option_register), which
 * also registers their operators (Switch Flow's case count buttons).
 */

#include <utility>

#include "node_logic_util.hh"

#include "NOD_logic_merged.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_merged_cc {

namespace decl = blender::nodes::logic::decl;
using logic::MergedNode;
using logic::PinMetadata;
using logic::PinType;

static const bke::bNodeType *option_type(const MergedNode &merged, const bNode *node)
{
  const char *idname = node != nullptr ? logic::merged_option_idname(merged, *node) :
                                         merged.options[0].idname;
  return logic::find_option_node_type(idname);
}

static void declare_extra_output(NodeDeclarationBuilder &b, const PinMetadata &pin)
{
  const UString name{StringRef(pin.ui_name)};
  const UString identifier{StringRef(pin.identifier)};
  switch (pin.type) {
    case PinType::Condition:
      b.add_output<decl::Condition>(name, identifier)
          .description(StringRef(pin.identifier) == "Exists" ?
                           "True when the object has a game property of that name, of any type" :
                           "");
      break;
    default:
      BLI_assert_unreachable();
      break;
  }
}

template<int I> static const MergedNode &merged()
{
  return logic::all_merged_nodes[I];
}

template<int I> static void node_declare(NodeDeclarationBuilder &b)
{
  /* node_or_null() makes the declaration depend on the node, so changing the option rebuilds
   * the sockets (as Switch Flow's case count does). */
  const bNode *node = b.node_or_null();
  if (const bke::bNodeType *type = option_type(merged<I>(), node); type && type->declare) {
    type->declare(b);
  }
  for (const PinMetadata &pin : merged<I>().extra_outputs) {
    declare_extra_output(b, pin);
  }
}

template<int I> static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  const MergedNode &info = merged<I>();
  const bNode &node = *ptr->data_as<bNode>();
  const bke::bNodeType *type = option_type(info, &node);
  const StringRef idname = info.idname;
  if (idname == "LogicNativeThresholdValue") {
    /* One dropdown for both older nodes and their own operation (rna "pass_when"). */
    layout.prop(ptr, "pass_when", UI_ITEM_NONE, "", ICON_NONE);
    return;
  }
  if (idname == "LogicNativeModifyGameProperty") {
    /* Modify Property's own buttons draw Operation (here with Toggle) and Clamp. */
    if (logic::merged_option_index(info, node) == 0 && type && type->draw_buttons) {
      type->draw_buttons(layout, C, ptr);
    }
    else {
      layout.prop(ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
    }
    return;
  }
  layout.prop(ptr, "variant", UI_ITEM_NONE, "", ICON_NONE);
  if (type && type->draw_buttons) {
    type->draw_buttons(layout, C, ptr);
  }
}

template<int I> static void node_init(bNodeTree *tree, bNode *node)
{
  node->custom4 = 0.0f;
  if (const bke::bNodeType *type = option_type(merged<I>(), node); type && type->initfunc) {
    type->initfunc(tree, node);
  }
}

template<int I> static void node_update(bNodeTree *tree, bNode *node)
{
  /* An older node's update finds its sockets by identifier through the tree's topology cache,
   * and a new option's sockets are made in this same update: without the cache made again first,
   * a node switched to Gamepad kept its Strength output shown (found by the socket audit). */
  tree->ensure_topology_cache();
  if (const bke::bNodeType *type = option_type(merged<I>(), node); type && type->updatefunc) {
    type->updatefunc(tree, node);
  }
}

/** The header color of the merged node's first option. */
static int merged_node_class(const StringRef idname)
{
  if (ELEM(idname,
           "LogicNativeGetGamePropertyValue",
           "LogicNativeGetLightAttribute",
           "LogicNativeCursor",
           "LogicNativeGetDisplaySetting"))
  {
    return NODE_CLASS_INPUT;
  }
  if (ELEM(idname,
           "LogicNativeModifyGameProperty",
           "LogicNativeSwitchFlow",
           "LogicNativeOnValueChange",
           "LogicNativeOverlayCollection",
           "LogicNativeIsNone",
           "LogicNativeVariableLoad",
           "LogicNativeVariableSave",
           "LogicNativeVariableRemove",
           "LogicNativeThresholdValue",
           "LogicNativeDelayFlow"))
  {
    return NODE_CLASS_CONVERTER;
  }
  if (idname == "LogicNativeSetLightAttribute") {
    return NODE_CLASS_OP_COLOR;
  }
  return NODE_CLASS_OP_VECTOR;
}

template<int I> static void register_option_operators()
{
  for (const logic::MergedNodeOption &option : merged<I>().options) {
    if (const bke::bNodeType *type = logic::find_option_node_type(option.idname);
        type && type->register_operators)
    {
      type->register_operators();
    }
  }
}

template<int I> static void register_merged_node()
{
  static bke::bNodeType ntype;
  const MergedNode &info = merged<I>();
  logic_node_type_base(&ntype, UString(StringRef(info.idname)));
  ntype.ui_name = info.ui_name;
  ntype.ui_description = info.ui_description;
  ntype.nclass = merged_node_class(info.idname);
  ntype.declare = node_declare<I>;
  ntype.draw_buttons = node_layout<I>;
  ntype.initfunc = node_init<I>;
  ntype.updatefunc = node_update<I>;
  ntype.register_operators = register_option_operators<I>;
  bke::node_type_size(ntype, 170, 140, NODE_DEFAULT_MAX_WIDTH);
  bke::node_register_type(ntype);
}

template<int... I> static void register_merged_nodes(std::integer_sequence<int, I...> /*indices*/)
{
  (register_merged_node<I>(), ...);
}

static void node_register()
{
  register_merged_nodes(std::make_integer_sequence<int, logic::merged_node_count>());
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_merged_cc
