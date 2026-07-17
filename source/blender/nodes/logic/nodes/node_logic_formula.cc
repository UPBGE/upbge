/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "BLI_string.h"

#include "MEM_guardedalloc.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_formula_cc {
namespace decl = blender::nodes::logic::decl;

static const char *formula_text_for_node(const bNode &node)
{
  static const char *predefined[] = {
      "a + b",
      "abs(a)",
      "acos(a)",
      "acosh(a)",
      "asin(a)",
      "asinh(a)",
      "atan(a)",
      "atan2(a,b)",
      "atanh(a)",
      "ceil(a)",
      "cos(a)",
      "cosh(a)",
      "curt(a)",
      "degrees(a)",
      "e",
      "exp(a)",
      "floor(a)",
      "hypot(a,b)",
      "log(a)",
      "log10(a)",
      "mod(a,b)",
      "pi",
      "pow(a,b)",
      "radians(a)",
      "sign(a)",
      "sin(a)",
      "sinh(a)",
      "sqrt(a)",
      "tan(a)",
      "tanh(a)",
  };
  if (node.custom1 == 0) {
    const char *formula = static_cast<const char *>(node.storage);
    return formula != nullptr ? formula : "a + b";
  }
  const int index = node.custom1 - 1;
  return (index >= 0 && index < int(std::size(predefined))) ? predefined[index] : "a + b";
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("a"_ustr);
  b.add_input<decl::Float>("b"_ustr);
  b.add_output<decl::Float>("Result"_ustr, "Out"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "predefined_formulas", UI_ITEM_NONE, "Predef.", ICON_NONE);
  if (RNA_enum_get(ptr, "predefined_formulas") == 0) {
    layout.prop(ptr, "formula", UI_ITEM_NONE, "Formula", ICON_NONE);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
  node->storage = BLI_strdup("a + b");
}

static void node_free(bNode *node)
{
  if (node->storage != nullptr) {
    MEM_delete_void(node->storage);
    node->storage = nullptr;
  }
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const StringRef formula = formula_text_for_node(*node);
  if (bNodeSocket *a = node->input_by_identifier("a"_ustr)) {
    bke::node_set_socket_availability(*ntree, *a, formula.find("a") != StringRef::not_found);
  }
  if (bNodeSocket *b = node->input_by_identifier("b"_ustr)) {
    bke::node_set_socket_availability(*ntree, *b, formula.find("b") != StringRef::not_found);
  }
}

static void node_copy(bNodeTree * /*tree*/, bNode *dest_node, const bNode *src_node)
{
  const char *src_formula = static_cast<const char *>(src_node->storage);
  dest_node->storage = src_formula != nullptr ? BLI_strdup(src_formula) : BLI_strdup("a + b");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeFormula"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.freefunc = node_free;
  ntype.copyfunc = node_copy;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_formula_cc
