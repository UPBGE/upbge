/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Set Game Property (LogicNativeSetGamePropertyValue,
 * NOD_logic_merged.hh), each the node it was before 2026-11-08: SetGamePropertyFloat,
 * SetGamePropertyInt, SetGamePropertyBool, SetGamePropertyString, SetGameProperty. They are not
 * node types of their own: logic_option_register keeps them for the merged node, which forwards
 * its declaration, buttons, init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_game_property_float_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr)
      .default_value("property")
      .description(std::string("The game property to write. ") + logic::game_property_pick_note)
      .custom_draw(logic::game_property_field("Object"));
  b.add_input<decl::Float>("Value"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeSetGamePropertyFloat"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_game_property_float_cc

namespace blender::nodes::node_logic_set_game_property_int_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr)
      .default_value("property")
      .description(std::string("The game property to write. ") + logic::game_property_pick_note)
      .custom_draw(logic::game_property_field("Object"));
  b.add_input<decl::Int>("Value"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeSetGamePropertyInt"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_game_property_int_cc

namespace blender::nodes::node_logic_set_game_property_bool_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr)
      .default_value("property")
      .description(std::string("The game property to write. ") + logic::game_property_pick_note)
      .custom_draw(logic::game_property_field("Object"));
  b.add_input<decl::Bool>("Value"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeSetGamePropertyBool"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_game_property_bool_cc

namespace blender::nodes::node_logic_set_game_property_string_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr)
      .default_value("property")
      .description(std::string("The game property to write. ") + logic::game_property_pick_note)
      .custom_draw(logic::game_property_field("Object"));
  b.add_input<decl::String>("Value"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeSetGamePropertyString"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_game_property_string_cc

namespace blender::nodes::node_logic_set_game_property_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description("Empty uses the owner");
  b.add_input<decl::String>("Property"_ustr)
      .default_value("property")
      .description(std::string("The game property to write. ") + logic::game_property_pick_note)
      .custom_draw(logic::game_property_field("Object"));
  b.add_input<decl::Generic>("Value"_ustr)
      .description("Any value, such as a List, Dictionary or Object");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetGameProperty"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_game_property_cc

/* The typed options of 2026-11-15 (the user's call): Vector, Color, Object and List properties
 * written from their own sockets (before, only Any wrote them, from a value of any type). A
 * property keeps its type: a Vector property takes a Vector, a Color property a Color. */

namespace blender::nodes::node_logic_set_game_property_vector_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description("Empty uses the owner");
  b.add_input<decl::String>("Property"_ustr)
      .default_value("property")
      .description(std::string("The Vector game property to write (a missing one is made). ") +
                   logic::game_property_pick_note)
      .custom_draw(logic::game_property_field("Object"));
  b.add_input<decl::Vector>("Value"_ustr)
      .default_value(float3(0.0f, 0.0f, 0.0f))
      .description("The three numbers, written at the end of the step");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetGamePropertyVector"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_game_property_vector_cc

namespace blender::nodes::node_logic_set_game_property_color_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description("Empty uses the owner");
  b.add_input<decl::String>("Property"_ustr)
      .default_value("property")
      .description(std::string("The Color game property to write (a missing one is made). ") +
                   logic::game_property_pick_note)
      .custom_draw(logic::game_property_field("Object"));
  b.add_input<decl::Color>("Value"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .description("The color, written at the end of the step (a Vector linked in: alpha 1)");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetGamePropertyColor"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_game_property_color_cc

namespace blender::nodes::node_logic_set_game_property_object_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description("Empty uses the owner");
  b.add_input<decl::String>("Property"_ustr)
      .default_value("property")
      .description(std::string("The Object game property to write (a missing one is made). ") +
                   logic::game_property_pick_note)
      .custom_draw(logic::game_property_field("Object"));
  b.add_input<decl::Object>("Value"_ustr)
      .description("The object, written at the end of the step; empty: no object (clears it)");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetGamePropertyObject"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_game_property_object_cc

namespace blender::nodes::node_logic_set_game_property_list_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description("Empty uses the owner");
  b.add_input<decl::String>("Property"_ustr)
      .default_value("property")
      .description(std::string("The List or Table game property to write (a missing one is "
                               "made). ") +
                   logic::game_property_pick_note)
      .custom_draw(logic::game_property_field("Object"));
  b.add_input<decl::List>("Value"_ustr)
      .description("The list, written whole at the end of the step (a Table: one Dictionary per "
                   "row); empty: an empty list. To change one item, the list nodes' Game "
                   "Property mode (Append, Set List Index...) does it in place");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetGamePropertyList"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_game_property_list_cc
