/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

#include "UI_interface.h"
#include "UI_resources.h"

namespace blender::nodes::node_shader_uv_along_stroke_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>(N_("UV"));
}

static void node_shader_buts_uvalongstroke(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "use_tips", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, 0);
}

}  // namespace blender::nodes::node_shader_uv_along_stroke_cc

/* node type definition */
void register_node_type_sh_uvalongstroke()
{
  namespace file_ns = blender::nodes::node_shader_uv_along_stroke_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_UVALONGSTROKE, "UV Along Stroke", NODE_CLASS_INPUT);
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_uvalongstroke;

  nodeRegisterType(&ntype);
}
