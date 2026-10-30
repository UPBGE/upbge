/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_on_constraint_break_cc {
namespace decl = blender::nodes::logic::decl;

enum MatchMode {
  Exact = 0,
  Contains,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr)
      .description("Owner of the named rigid body constraint; empty uses this tree's object");
  b.add_input<decl::String>("Constraint Name"_ustr)
      .default_value("constraint")
      .description(
          "Case-sensitive full name or substring of a Blender Rigid Body Constraint or Add Physics Constraint node; ignored when Match Any is enabled");
  b.add_input<decl::Bool>("Match Any"_ustr)
      .default_value(false)
      .description("Listen for every automatically broken constraint on the selected owner");

  b.add_output<decl::Execution>("Broken"_ustr)
      .description("Once on the fixed logic step after one or more matching constraints automatically break");
  b.add_output<decl::String>("Constraint Name"_ustr)
      .description("Actual name of the first matching broken constraint");
  b.add_output<decl::Object>("Target Object"_ustr)
      .description("The constraint's second rigid body, if it still exists");
  b.add_output<decl::Vector>("Anchor Position"_ustr)
      .description("World-space anchor on the owner body; this is not a collision contact point");
  b.add_output<decl::Float>("Linear Impulse"_ustr)
      .description("Linear solver impulse that contributed to the break");
  b.add_output<decl::Float>("Angular Impulse"_ustr)
      .description("Angular solver impulse that contributed to the break");
  b.add_output<decl::Float>("Applied Impulse"_ustr)
      .description("Threshold aggregate reported by Jolt; use this to compare with Breaking Threshold");
  b.add_output<decl::Int>("Break Count"_ustr)
      .description("Number of matching constraints that broke during this physics step");
  b.add_output<decl::List>("Break Events"_ustr)
      .description("All matching break records in deterministic constraint-ID order; each item is a dictionary with constraint_name, target_object, anchor_position, linear_impulse, angular_impulse, and applied_impulse");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "match_mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = MatchMode::Exact;
}

static void node_update(bNodeTree * /*ntree*/, bNode *node)
{
  if (!ELEM(node->custom1, MatchMode::Exact, MatchMode::Contains)) {
    node->custom1 = MatchMode::Exact;
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeOnConstraintBreak"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_on_constraint_break_cc
