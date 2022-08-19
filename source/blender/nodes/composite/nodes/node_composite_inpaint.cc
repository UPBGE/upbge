/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "UI_interface.h"
#include "UI_resources.h"

#include "COM_node_operation.hh"

#include "node_composite_util.hh"

/* **************** Inpaint/ ******************** */

namespace blender::nodes::node_composite_inpaint_cc {

static void cmp_node_inpaint_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image")).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_output<decl::Color>(N_("Image"));
}

static void node_composit_buts_inpaint(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "distance", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
}

using namespace blender::realtime_compositor;

class InpaintOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    get_input("Image").pass_through(get_result("Image"));
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new InpaintOperation(context, node);
}

}  // namespace blender::nodes::node_composite_inpaint_cc

void register_node_type_cmp_inpaint()
{
  namespace file_ns = blender::nodes::node_composite_inpaint_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_INPAINT, "Inpaint", NODE_CLASS_OP_FILTER);
  ntype.declare = file_ns::cmp_node_inpaint_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_inpaint;
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  nodeRegisterType(&ntype);
}
