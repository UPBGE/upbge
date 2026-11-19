/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Find Objects (2026-11-16): the nearest, farthest, a random one or all of a set of objects
 * around a center (Query::FindObjects, LN_FindObjects.h). custom1 the source (0 All Objects,
 * 1 Collection, 2 With Property, 3 List), custom2 the pick (0 Nearest, 1 Farthest, 2 Random,
 * 3 All). The objects are where they were when the step started, for every reader.
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_find_objects_cc {
namespace decl = blender::nodes::decl;
namespace logic_decl = blender::nodes::logic::decl;

enum class Source : int16_t { All = 0, Collection = 1, Property = 2, List = 3 };
enum class Pick : int16_t { Nearest = 0, Farthest = 1, Random = 2, All = 3 };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<logic_decl::Collection>("Collection"_ustr)
      .description("The objects of this collection (its child collections too)");
  b.add_input<logic_decl::String>("Property"_ustr)
      .description(std::string("The objects with this game property (\"enemy\", \"pickup\"). ") +
                   logic::game_property_filter_note)
      .custom_draw(logic::game_property_filter_field());
  b.add_input<logic_decl::List>("List"_ustr).description("The objects of this list");
  b.add_input<logic_decl::Object>("Center"_ustr)
      .description("Where to search from; empty: the tree's own object");
  b.add_input<logic_decl::Float>("Radius"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .description("How far to search; 0: any distance");
  b.add_input<decl::Float>("View Angle"_ustr)
      .default_value(float(2.0 * M_PI))
      .min(0.0f)
      .max(float(2.0 * M_PI))
      .subtype(PROP_ANGLE)
      .description("The whole width of the cone around the Center's forward (+Y) axis (90: 45 "
                   "to each side); 360: all around");
  b.add_input<logic_decl::Bool>("Exclude Self"_ustr)
      .default_value(true)
      .description("Never find the Center or the tree's own object");
  b.add_input<logic_decl::Bool>("Only Visible"_ustr)
      .default_value(false)
      .description("Only objects the Center sees: a ray from it to each one, blocked by any "
                   "other object's body (not the Center's or the tree's own object's): cover "
                   "from a blast, a guard's sight. Costs a ray for each object in range");
  b.add_output<logic_decl::Bool>("Found"_ustr).description("An object was found");
  b.add_output<logic_decl::Object>("Object"_ustr).description("The object picked");
  b.add_output<logic_decl::Vector>("Position"_ustr)
      .description("Where the object was when the step started");
  b.add_output<logic_decl::Float>("Distance"_ustr)
      .description("How far it is from the Center");
  b.add_output<logic_decl::Int>("Count"_ustr)
      .description("How many objects are within the Radius and the View Angle (with Only "
                   "Visible: how many of them are seen)");
  b.add_output<logic_decl::List>("Objects"_ustr)
      .description("All of them, nearest first");
  b.add_output<logic_decl::List>("Distances"_ustr)
      .description("How far each of Objects is from the Center, in the same order (damage that "
                   "falls off with distance)");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "source", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "pick", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const Source source = Source(node->custom1);
  const auto show = [&](bNodeSocket *socket, const bool available) {
    if (socket != nullptr) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
  };
  show(node->input_by_identifier("Collection"_ustr), source == Source::Collection);
  show(node->input_by_identifier("Property"_ustr), source == Source::Property);
  show(node->input_by_identifier("List"_ustr), source == Source::List);
  show(node->output_by_identifier("Objects"_ustr), Pick(node->custom2) == Pick::All);
  show(node->output_by_identifier("Distances"_ustr), Pick(node->custom2) == Pick::All);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = int16_t(Source::Property);
  node->custom2 = int16_t(Pick::Nearest);
  node_update(ntree, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeFindObjects"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_type_size(ntype, 180, 140, NODE_DEFAULT_MAX_WIDTH);

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_find_objects_cc
