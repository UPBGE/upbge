/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_evaluate_property_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr)
      .default_value("prop")
      .description(std::string("Game Property mode: the game property compared. ") +
                   logic::game_property_pick_note +
                   ". Attribute mode: the object's attribute, such as \"worldPosition\"")
      .custom_draw([field = logic::game_property_field("Object")](CustomSocketDrawParams &params) {
        /* Attribute mode (custom2 1) names an attribute: a plain text field. */
        if (params.node.custom2 != 0) {
          params.draw_standard(params.layout);
          return;
        }
        field(params);
      });
  b.add_input<decl::Generic>("Value"_ustr);
  b.add_output<decl::Condition>("If True"_ustr);
  b.add_output<decl::Generic>("Value"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
  node->custom2 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeEvaluateProperty"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_evaluate_property_cc
