/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "UI_interface.h"
#include "UI_resources.h"

#include "COM_node_operation.hh"

#include "node_composite_util.hh"

/* **************** Switch ******************** */

namespace blender::nodes::node_composite_switch_cc {

static void cmp_node_switch_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Off")).default_value({0.8f, 0.8f, 0.8f, 1.0f});
  b.add_input<decl::Color>(N_("On")).default_value({0.8f, 0.8f, 0.8f, 1.0f});
  b.add_output<decl::Color>(N_("Image"));
}

static void node_composit_buts_switch(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "check", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
}

using namespace blender::realtime_compositor;

class SwitchOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    Result &input = get_input(get_condition() ? "On" : "Off");
    Result &result = get_result("Image");
    input.pass_through(result);
  }

  bool get_condition()
  {
    return bnode().custom1;
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new SwitchOperation(context, node);
}

}  // namespace blender::nodes::node_composite_switch_cc

void register_node_type_cmp_switch()
{
  namespace file_ns = blender::nodes::node_composite_switch_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_SWITCH, "Switch", NODE_CLASS_LAYOUT);
  ntype.declare = file_ns::cmp_node_switch_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_switch;
  node_type_size_preset(&ntype, NODE_SIZE_SMALL);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  nodeRegisterType(&ntype);
}
