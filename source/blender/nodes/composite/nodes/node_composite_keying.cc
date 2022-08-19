/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_math_base.h"

#include "BLT_translation.h"

#include "DNA_movieclip_types.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "COM_node_operation.hh"

#include "node_composite_util.hh"

/* **************** Keying  ******************** */

namespace blender::nodes::node_composite_keying_cc {

static void cmp_node_keying_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image")).default_value({0.8f, 0.8f, 0.8f, 1.0f});
  b.add_input<decl::Color>(N_("Key Color")).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Float>(N_("Garbage Matte")).hide_value();
  b.add_input<decl::Float>(N_("Core Matte")).hide_value();
  b.add_output<decl::Color>(N_("Image"));
  b.add_output<decl::Float>(N_("Matte"));
  b.add_output<decl::Float>(N_("Edges"));
}

static void node_composit_init_keying(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeKeyingData *data = MEM_cnew<NodeKeyingData>(__func__);

  data->screen_balance = 0.5f;
  data->despill_balance = 0.5f;
  data->despill_factor = 1.0f;
  data->edge_kernel_radius = 3;
  data->edge_kernel_tolerance = 0.1f;
  data->clip_black = 0.0f;
  data->clip_white = 1.0f;
  node->storage = data;
}

static void node_composit_buts_keying(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  /* bNode *node = (bNode*)ptr->data; */ /* UNUSED */

  uiItemR(layout, ptr, "blur_pre", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "screen_balance", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "despill_factor", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "despill_balance", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "edge_kernel_radius", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "edge_kernel_tolerance", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "clip_black", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "clip_white", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "dilate_distance", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "feather_falloff", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "feather_distance", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "blur_post", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
}

using namespace blender::realtime_compositor;

class KeyingOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    get_input("Image").pass_through(get_result("Image"));
    get_result("Matte").allocate_invalid();
    get_result("Edges").allocate_invalid();
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new KeyingOperation(context, node);
}

}  // namespace blender::nodes::node_composite_keying_cc

void register_node_type_cmp_keying()
{
  namespace file_ns = blender::nodes::node_composite_keying_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_KEYING, "Keying", NODE_CLASS_MATTE);
  ntype.declare = file_ns::cmp_node_keying_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_keying;
  node_type_init(&ntype, file_ns::node_composit_init_keying);
  node_type_storage(
      &ntype, "NodeKeyingData", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  nodeRegisterType(&ntype);
}
