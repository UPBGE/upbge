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

namespace blender::nodes::node_logic_raycast_from_mesh_cc {
namespace decl = blender::nodes::logic::decl;

enum Domain { Vertices = 0, Faces };
enum Direction { Normals = 0, InvertedNormals };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Optional trigger; an unconnected query evaluates when an output is read");
  b.add_input<decl::Object>("Source Object"_ustr)
      .description("Object whose current game-engine mesh supplies ray origins and normals; uses the logic-tree owner when empty");
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
      .description("Hard per-evaluation ray limit; larger meshes are sampled evenly and deterministically");
  b.add_input<decl::String>("Property"_ustr)
      .description("Only report hits on objects with this game property; empty accepts any object");
  b.add_input<decl::Bool>("X-Ray"_ustr)
      .description("Skip objects that do not match the property filter instead of letting them block a ray");
  b.add_input<decl::CollisionLayers>("Mask"_ustr)
      .default_value(1023)
      .min(0)
      .max(1023)
      .description("Collision layers filter shared by all generated rays; zero accepts all layers");
  b.add_input<decl::Bool>("Include Sensors"_ustr)
      .description("Allow Jolt sensor bodies to be returned");
  b.add_input<decl::Bool>("Hit Backfaces"_ustr)
      .description("Report back sides of one-sided triangle surfaces");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .description("Draw every generated ray as a transient game-engine debug line");

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
  b.add_output<decl::List>("Points"_ustr)
      .description("World positions from matching hits only, in generated-ray order; length equals Hit Count");
  b.add_output<decl::List>("Normals"_ustr)
      .description("World normals from matching hits only, in generated-ray order; length equals Hit Count");
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
