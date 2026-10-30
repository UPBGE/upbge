/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_parent_cc {
namespace decl = blender::nodes::logic::decl;

enum ParentType : int16_t { Object = 0, Bone = 1 };

constexpr int16_t settings_initialized = 1 << 14;
constexpr int16_t compound_initialized = 1 << 13;

static bool uses_modern_settings(const bNode &node)
{
  return (node.custom2 & settings_initialized) != 0;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Child Object"_ustr)
      .description("Child object; defaults to the owner when unset");
  b.add_input<decl::Object>("Parent Object"_ustr)
      .description("Parent object, or armature object when Parent Type is Bone");
  b.add_input<decl::String>("Bone Name"_ustr)
      .description("Pose bone name on the parent armature; evaluated at execution time");
  b.add_input<decl::Bool>("Compound"_ustr)
      .default_value(false)
      .description("Merge the child collision shape into the top compound parent's Jolt body");
  b.add_input<decl::Bool>("Ghost"_ustr)
      .default_value(true)
      .description("Legacy input retained only for nodes authored before modern parenting");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  const bNode *node = ptr->data_as<bNode>();
  if (!uses_modern_settings(*node)) {
    layout.label("Legacy parenting behavior", ICON_INFO);
    return;
  }
  layout.prop(ptr, "parent_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = Object;
  node->custom2 = settings_initialized | compound_initialized;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  const bool modern = uses_modern_settings(*node);
  if (modern) {
    node->custom1 = ELEM(node->custom1, Object, Bone) ? node->custom1 : Object;
    /* Compound used to be an ignored, hidden legacy socket with a true default. Initialize
     * pre-toggle modern nodes once so revealing it cannot silently change their behavior. */
    if ((node->custom2 & compound_initialized) == 0) {
      if (bNodeSocket *socket = node->input_by_identifier("Compound"_ustr)) {
        if (socket->default_value != nullptr) {
          static_cast<bNodeSocketValueBoolean *>(socket->default_value)->value = false;
        }
      }
      node->custom2 |= compound_initialized;
    }
  }

  auto available = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  available("Bone Name"_ustr, modern && node->custom1 == Bone);
  available("Compound"_ustr, !modern || node->custom1 == Object);
  available("Ghost"_ustr, !modern);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetParent"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_parent_cc
