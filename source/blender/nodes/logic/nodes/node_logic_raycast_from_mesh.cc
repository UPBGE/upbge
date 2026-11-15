/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "MEM_guardedalloc.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BLI_vector.hh"
#include "DNA_logic_node_binding_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"
#include "ED_undo.hh"
#include "RNA_access.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_string_search.hh"

namespace blender::nodes::node_logic_raycast_from_mesh_cc {
namespace decl = blender::nodes::logic::decl;

enum Domain { Vertices = 0, Faces };
enum Direction { Normals = 0, InvertedNormals };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues after the complete synchronous batch has finished");
  b.add_output<decl::Condition>("Any Hit"_ustr)
      .description("True when at least one generated ray has a matching hit");
  b.add_output<decl::Condition>("All Hit"_ustr)
      .description("True when at least one ray was generated and every ray has a matching hit");
  b.add_output<decl::Int>("Hit Count"_ustr)
      .description("Number of generated rays that have a matching hit");
  b.add_output<decl::Int>("Ray Count"_ustr)
      .description("Number of valid rays generated after filtering and Max Rays sampling");
  b.add_output<decl::List>("Hits"_ustr)
      .description("Per-ray matching-hit flags, one Boolean for every generated ray");
  b.add_output<decl::List>("Blocked"_ustr)
      .description(
          "Per-ray flags for nonmatching blockers when X-Ray is disabled, one Boolean for every generated ray");
  b.add_output<decl::List>("Hit Objects"_ustr)
      .description("Objects from matching hits only, in generated-ray order; length equals Hit Count");
  b.add_output<decl::List>("Hit Parts"_ustr)
      .description("For matching hits only: the part of a compound object that was hit (its "
                   "child with Compound), or the hit object itself; length equals Hit Count");
  b.add_output<decl::List>("Points"_ustr)
      .description("World positions from matching hits only, in generated-ray order; length equals Hit Count");
  b.add_output<decl::List>("Normals"_ustr)
      .description("World normals from matching hits only, in generated-ray order; length equals Hit Count");
  b.add_output<decl::List>("Hit Materials"_ustr)
      .description("For matching hits only: the name of the material on the hit face (empty "
                   "without one); length equals Hit Count");
  b.add_output<decl::List>("Distances"_ustr)
      .description("Travel distances from matching hits only, in generated-ray order; length equals Hit Count");
  b.add_output<decl::List>("Fractions"_ustr)
      .description(
          "Normalized distances from matching hits only, in generated-ray order; length equals Hit Count");
  b.add_output<decl::List>("Face Indices"_ustr)
      .description(
          "Polygon indices from matching hits only, in generated-ray order; unavailable values are -1");
  b.add_output<decl::List>("Has UVs"_ustr)
      .description("UV-validity flags for matching hits only, in generated-ray order");
  b.add_output<decl::List>("UVs"_ustr)
      .description(
          "UV values for matching hits only as X/Y vectors with Z set to 0; invalid UVs are zero vectors");
  b.add_output<decl::List>("Ray Origins"_ustr)
      .description("Per-ray world origins after Start Offset is applied; length equals Ray Count");
  b.add_output<decl::List>("Ray Directions"_ustr)
      .description("Per-ray normalized world directions; length equals Ray Count");
  b.add_output<decl::List>("End Points"_ustr)
      .description("Per-ray furthest world points tested; length equals Ray Count");
  b.add_default_layout();
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Optional trigger; an unconnected query evaluates when an output is read");
  b.add_input<decl::Object>("Source Object"_ustr)
      .description(
          "Object whose mesh gives the rays, in the shape it had when the game started (an "
          "armature or shape keys do not move the rays); the logic tree's owner when empty");
  b.add_input<decl::String>("Vertex Group"_ustr)
      .description(
          "Cast only from this vertex group (or a Boolean or Float attribute of that name): "
          "its vertices with a weight above 0, or the faces whose corners are all in it. Max "
          "Rays spreads over the group only. Empty: the whole mesh. The list shows the groups of "
          "the Source Object (or of the selected object using this tree); any name can be typed. "
          "Red: no object using this tree has that name. A name the mesh does not have casts "
          "nothing (warned once)")
      .custom_draw(logic::vertex_group_field("Source Object"));
  b.add_input<decl::Object>("Ignore Object"_ustr)
      .description("Additional object to exclude; the source object is always excluded");
  b.add_input<decl::List>("Ignore Objects"_ustr)
      .description("Additional objects to exclude; invalid entries and duplicates are ignored");
  b.add_input<decl::Float>("Start Offset"_ustr)
      .default_value(0.01f)
      .min(0.0f)
      .description("Move each origin along its ray direction before casting to avoid self-intersection");
  b.add_input<decl::Float>("Max Distance"_ustr)
      .default_value(100.0f)
      .min(0.001f)
      .description("Maximum distance travelled by every generated ray in world units");
  b.add_input<decl::Int>("Max Rays"_ustr)
      .default_value(256)
      .min(1)
      .max(256)
      .description(
          "The most rays cast. A mesh with more faces (or vertices) than this casts from an even "
          "spread of them in Blender's order, the same ones every time: 3 rays on 4 faces use "
          "faces 0, 1 and 2");
  b.add_input<decl::String>("Property"_ustr)
      .description(std::string("Only report hits on objects with this game property; empty "
                               "accepts any object. ") +
                   logic::game_property_filter_note)
      .custom_draw(logic::game_property_filter_field());
  b.add_input<decl::CollisionLayers>("Mask"_ustr)
      .default_value(1023)
      .min(0)
      .max(1023)
      .description("Collision layers filter shared by all generated rays; zero accepts all layers");
  logic::add_ray_filters_panel(b);
  logic::add_visualize_panel(b);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mesh_domain", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "ray_direction", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = Vertices;
  node->custom2 = Normals;
}

static void node_update(bNodeTree * /*tree*/, bNode *node)
{
  if (node->custom1 < Vertices || node->custom1 > Faces) {
    node->custom1 = Vertices;
  }
  if (node->custom2 < Normals || node->custom2 > InvertedNormals) {
    node->custom2 = Normals;
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeRaycastFromMesh"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_raycast_from_mesh_cc
