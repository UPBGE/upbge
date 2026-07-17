/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BLI_math_base.hh"

#include "BKE_node.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_gamepad_look_cc {
namespace decl = blender::nodes::logic::decl;

static bool socket_bool_default(const bNodeSocket *socket)
{
  if (socket == nullptr || socket->type != SOCK_BOOLEAN || socket->default_value == nullptr) {
    return false;
  }
  return static_cast<const bNodeSocketValueBoolean *>(socket->default_value)->value;
}

static void draw_cap_range_socket(CustomSocketDrawParams &params)
{
  if (!params.socket.is_available()) {
    return;
  }
  if (params.socket.is_directly_linked()) {
    if (!params.label.is_empty()) {
      params.layout.label(params.label, ICON_NONE);
    }
    return;
  }
  ui::Layout &row = params.layout.row(true);
  row.prop(&params.socket_ptr, "default_value", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Body"_ustr, "Body Object"_ustr);
  b.add_input<decl::Object>("Head"_ustr, "Head Object"_ustr);
  b.add_input<decl::Vector>("Inverted"_ustr)
      .default_value(float3(0.0f, 0.0f, 0.0f))
      .description("Non-zero X or Y values invert the corresponding stick axis");
  b.add_input<decl::Int>("Index"_ustr).default_value(0).min(0);
  b.add_input<decl::Float>("Sensitivity"_ustr).default_value(0.25f).min(0.0f);
  b.add_input<decl::Float>("Exponent"_ustr).default_value(2.3f).min(0.0f);
  b.add_input<decl::Bool>("Cap Left / Right"_ustr).default_value(false);
  b.add_input<decl::Vector>(""_ustr, "Left / Right Range"_ustr)
      .dimensions(2)
      .subtype(PROP_EULER)
      .default_value(float2(0.0f, 0.0f))
      .custom_draw(draw_cap_range_socket);
  b.add_input<decl::Bool>("Cap Up / Down"_ustr).default_value(false);
  b.add_input<decl::Vector>(""_ustr, "Up / Down Range"_ustr)
      .dimensions(2)
      .subtype(PROP_EULER)
      .default_value(float2(DEG2RADF(-89.0f), DEG2RADF(89.0f)))
      .custom_draw(draw_cap_range_socket);
  b.add_input<decl::Float>("Threshold"_ustr).default_value(0.1f).min(0.0f).max(1.0f);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "axis", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 1;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *cap_x = node->input_by_identifier("Cap Left / Right"_ustr);
  bNodeSocket *cap_x_range = node->input_by_identifier("Left / Right Range"_ustr);
  bNodeSocket *cap_y = node->input_by_identifier("Cap Up / Down"_ustr);
  bNodeSocket *cap_y_range = node->input_by_identifier("Up / Down Range"_ustr);
  const bool use_cap_x = cap_x && (cap_x->flag & SOCK_IS_LINKED) ?
                             true :
                             socket_bool_default(cap_x);
  const bool use_cap_y = cap_y && (cap_y->flag & SOCK_IS_LINKED) ?
                             true :
                             socket_bool_default(cap_y);
  if (cap_x_range) {
    bke::node_set_socket_availability(*ntree, *cap_x_range, use_cap_x);
  }
  if (cap_y_range) {
    bke::node_set_socket_availability(*ntree, *cap_y_range, use_cap_y);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGamepadLook"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_gamepad_look_cc
