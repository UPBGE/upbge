/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "DNA_scene_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_vsync_cc {
namespace decl = blender::nodes::logic::decl;

/* The dropdown's From Input item (VSYNC_* are 0-2; rna_nodetree.cc, LN_TreeCompiler.cpp). */
constexpr int kVSyncFromInput = 3;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Int>("Mode"_ustr)
      .default_value(VSYNC_OFF)
      .description("0 On, 1 Off, 2 Adaptive (the numbers Get Display Setting's VSync gives); any "
                   "other number turns VSync off");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "vsync_mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = VSYNC_OFF;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  if (bNodeSocket *socket = node->input_by_identifier("Mode"_ustr)) {
    bke::node_set_socket_availability(*ntree, *socket, node->custom1 == kVSyncFromInput);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetVSync"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_vsync_cc