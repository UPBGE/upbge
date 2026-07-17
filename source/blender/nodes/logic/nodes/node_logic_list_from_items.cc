/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include <algorithm>
#include <iterator>

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_list_from_items_cc {
namespace decl = blender::nodes::logic::decl;

static void node_update(bNodeTree *ntree, bNode *node);

static constexpr const char *socket_names[] = {"Values",
                                               "Floats",
                                               "Integers",
                                               "Strings",
                                               "Booleans",
                                               "Vectors",
                                               "Colors",
                                               "Lists",
                                               "Dictionaries",
                                               "Datablocks",
                                               "Objects",
                                               "Collections",
                                               "Conditions",
                                               "Instances",
                                               "Widgets"};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Generic>("Values"_ustr).multi_input(true);
  b.add_input<decl::Float>("Floats"_ustr).multi_input(true);
  b.add_input<decl::Int>("Integers"_ustr).multi_input(true);
  b.add_input<decl::String>("Strings"_ustr).multi_input(true);
  b.add_input<decl::Bool>("Booleans"_ustr).default_value(false).multi_input(true);
  b.add_input<decl::Vector>("Vectors"_ustr).multi_input(true);
  b.add_input<decl::Color>("Colors"_ustr).multi_input(true);
  b.add_input<decl::List>("Lists"_ustr).multi_input(true);
  b.add_input<decl::Dictionary>("Dictionaries"_ustr).multi_input(true);
  b.add_input<decl::Datablock>("Datablocks"_ustr).multi_input(true);
  b.add_input<decl::Object>("Objects"_ustr).multi_input(true);
  b.add_input<decl::Collection>("Collections"_ustr).multi_input(true);
  b.add_input<decl::Condition>("Conditions"_ustr).default_value(false).multi_input(true);
  b.add_input<decl::Generic>("Instances"_ustr).multi_input(true);
  b.add_input<decl::UI>("Widgets"_ustr).multi_input(true);

  b.add_output<decl::List>("Values"_ustr);
  b.add_output<decl::List>("Floats"_ustr);
  b.add_output<decl::List>("Integers"_ustr);
  b.add_output<decl::List>("Strings"_ustr);
  b.add_output<decl::List>("Booleans"_ustr);
  b.add_output<decl::List>("Vectors"_ustr);
  b.add_output<decl::List>("Colors"_ustr);
  b.add_output<decl::List>("Lists"_ustr);
  b.add_output<decl::List>("Dictionaries"_ustr);
  b.add_output<decl::List>("Datablocks"_ustr);
  b.add_output<decl::List>("Objects"_ustr);
  b.add_output<decl::List>("Collections"_ustr);
  b.add_output<decl::List>("Conditions"_ustr);
  b.add_output<decl::List>("Instances"_ustr);
  b.add_output<decl::List>("Widgets"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = 0;
  node_update(ntree, node);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const int mode = std::clamp<int>(node->custom1, 0, int(std::size(socket_names)) - 1);
  for (int index = 0; index < int(std::size(socket_names)); index++) {
    const bool available = index == mode;
    const UString identifier(StringRef(socket_names[index]));
    if (bNodeSocket *input_socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*ntree, *input_socket, available);
    }
    if (bNodeSocket *output_socket = node->output_by_identifier(identifier)) {
      bke::node_set_socket_availability(*ntree, *output_socket, available);
    }
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeListFromItems"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_list_from_items_cc
