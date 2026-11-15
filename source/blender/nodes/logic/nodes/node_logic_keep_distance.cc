/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Keep Distance (2026-11-12; idname LogicNativeKeepDistance): pushes a physics object away from
 * every surface within Distance of it, harder the closer, as a cushion would (Mode 0 Push); pulls
 * it toward the nearest surface within Distance, as a magnet (1 Pull); or keeps it at Distance
 * from surfaces, pulling it back toward the nearest when it drifts farther, out to Reach (2 Hold).
 * The Mode is an input (2026-11-15, the user's call: a number a game property can change while
 * the game runs; it was a dropdown). One Jolt proximity
 * query a check (a volume, not rays: thin objects and both sides of a wall are found), the force
 * worked out and applied at the end of the step, just before the physics, for all such nodes at
 * once on the physics threads (the engine's LN_OpCode::KeepDistance).
 */

#include "BLT_translation.hh"
#include "node_logic_util.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_keep_distance_cc {

namespace decl = blender::nodes::logic::decl;

/* Falloff (custom1 bit 0, RNA `falloff`; the engine's LN_KEEP_DISTANCE_FALLOFF_*). */
enum Falloff { Linear = 0, Smooth = 1 };
/* The Mode dropdown of nodes saved before 2026-11-15 (custom1 bits 1-2: 2 Pull, 4 Hold): node_update
 * moves it into the Mode input once. */
enum OldMode { OldPull = 2, OldHold = 4, OldModeMask = 6 };
/* The Mode input's values (the engine's LN_KEEP_DISTANCE_MODE_*). */
enum Mode { Push = 0, Pull = 1, Hold = 2 };
/* Measure From (custom2, RNA `measure_from`; the engine's LN_KEEP_DISTANCE_MEASURE_*). */
enum Measure { ObjectShape = 0, Sphere = 1 };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues at once; the push or pull itself is worked out and applied at the "
                   "end of the step, just before the physics");
  b.add_output<decl::Condition>("Near"_ustr)
      .description("True while a surface is within Distance (Hold: within Reach), as the last "
                   "check found it: the step before");
  b.add_output<decl::Float>("Nearest Distance"_ustr)
      .description("The gap to the nearest surface in meters (0: touching), as of the step "
                   "before; -1 when nothing is within Distance (Hold: Reach)");
  b.add_output<decl::Vector>("Nearest Point"_ustr)
      .description("The nearest point on that surface, in world space (zero when not Near)");
  b.add_output<decl::Vector>("Nearest Normal"_ustr)
      .description("The direction pointing away from that surface (zero when not Near)");
  b.add_output<decl::Object>("Nearest Object"_ustr)
      .description("The object the nearest surface belongs to, as of the last search for "
                   "surfaces (none when not Near)");
  b.add_output<decl::Vector>("Force"_ustr)
      .description("The push or pull it applied in the step before, in newtons (Apply off: the "
                   "force it would apply). Use it to steer a character or an object without "
                   "physics");
  b.add_default_layout();
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Pushes or pulls in this step; link it to On Logic + Physics Step to keep it "
                   "up all the time");
  b.add_input<decl::Object>("Object"_ustr)
      .description("The object pushed or pulled; empty: the object running this tree");
  b.add_input<decl::Int>("Mode"_ustr)
      .default_value(Push)
      .min(Push)
      .max(Hold)
      .description("What it does, a number so that a game property can change it while the game "
                   "runs. 0 Push: pushed away from every surface within Distance, harder the "
                   "closer (a cushion). 1 Pull: pulled toward the nearest surface within "
                   "Distance, harder the closer (a magnet: it ends touching it). 2 Hold: kept at "
                   "Distance, pushed away from anything closer and pulled back toward the nearest "
                   "surface when it drifts farther, out to Reach (hovering, wall running). Below 0 "
                   "counts as 0, above 2 as 2");
  b.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA * /*ptr*/) {
    /* (Short enough for the node's default width: the field above says Mode.) */
    layout.label(IFACE_("0 Push · 1 Pull · 2 Hold"), ICON_NONE);
  });
  b.add_input<decl::Float>("Distance"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Measured from the object's surface. Push (Mode 0): how far from a surface "
                   "the push starts (the cushion). Pull (1): how far from a surface the pull "
                   "starts. Hold (2): the distance it keeps");
  b.add_input<decl::Float>("Reach"_ustr)
      .default_value(2.0f)
      .min(0.0f)
      .description("Hold (Mode 2) only: how far from the nearest surface it is still pulled "
                   "back toward it, from nothing at Distance to all of Strength at Reach; farther "
                   "away it lets go (a jump, a cliff edge)");
  b.add_input<decl::Float>("Strength"_ustr)
      .default_value(20.0f)
      .min(0.0f)
      .description("The push at a touching surface, fading to nothing at Distance (Pull: the "
                   "pull at a touching surface; Hold: also the pull at Reach): an acceleration in "
                   "m/s² with Ignore Mass (9.81 holds an object up against gravity), else a force "
                   "in newtons. Under gravity a pushed object rests lower than Distance (Linear: "
                   "by Distance x 9.81 / Strength)");
  b.add_input<decl::Float>("Damping"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Brakes the movement toward and away from the surfaces it acts on: 0 "
                   "bounces, 1 settles without bouncing, more settles slower (Pull: arrives "
                   "slower). Sideways movement is not braked");
  b.add_input<decl::Float>("Radius"_ustr)
      .default_value(0.5f)
      .min(0.001f)
      .description("The sphere around the object's origin the distance is measured from");
  b.add_input<decl::Int>("Check Every"_ustr)
      .default_value(1)
      .min(1)
      .max(60)
      .description("Search for surfaces every this many steps (1: every step). In between, the "
                   "surfaces found are remembered and measured again each step, so the push stays "
                   "smooth; something that moves in is noticed up to that many steps late. Many "
                   "objects keep their distance faster with 2-4");
  b.add_input<decl::Bool>("Ignore Mass"_ustr)
      .default_value(true)
      .description("Strength is an acceleration: light and heavy objects are pushed and pulled "
                   "alike");
  b.add_input<decl::Bool>("Apply"_ustr)
      .default_value(true)
      .description("Push or pull the object; off: only work out the outputs (Force)");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .default_value(false)
      .description("Draw what it found while the game runs: a line to each surface found (red: "
                   "pushing, magenta: pulling, yellow: found but not acting) and the push or pull "
                   "as a green arrow (1 m for 10 m/s²); Distance and Reach show in the 3D "
                   "viewport before the game (Logic Node Editor > Dashboard > Ray Previews)");
  PanelDeclarationBuilder &filters = b.add_panel("Filters"_ustr).default_closed(true);
  filters.add_input<decl::String>("Property"_ustr)
      .description(std::string("Only objects with this game property push or pull; empty: "
                               "every object. ") +
                   logic::game_property_filter_note)
      .custom_draw(logic::game_property_filter_field());
  filters.add_input<decl::CollisionLayers>("Mask"_ustr)
      .default_value(1023)
      .min(0)
      .max(1023)
      .description("Collision layers of the objects that push or pull");
  filters.add_input<decl::Object>("Ignore Object"_ustr)
      .description("An object that neither pushes nor pulls (the object's own children never "
                   "do)");
  filters.add_input<decl::List>("Ignore Objects"_ustr)
      .description("Objects that neither push nor pull; anything in the list that is not an "
                   "object is skipped");
  filters.add_input<decl::Bool>("Include Sensors"_ustr)
      .default_value(false)
      .description("Sensor objects (Jolt sensor bodies) push and pull too; passed by default");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "measure_from", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "falloff", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  /* A node saved with the Mode dropdown (custom1 bits 1-2): its choice into the Mode input (the
   * sockets are made before this runs when a file is read), once; then custom1 holds only the
   * Falloff. A user count is never touched here (this also runs while a file is read). */
  const int old_mode = node->custom1 & OldModeMask;
  bNodeSocket *mode = node->input_by_identifier("Mode"_ustr);
  if (old_mode != 0 && mode != nullptr && mode->default_value != nullptr) {
    static_cast<bNodeSocketValueInt *>(mode->default_value)->value = old_mode == OldHold ? Hold :
                                                                     old_mode == OldPull ? Pull :
                                                                                           Push;
  }
  if (old_mode == 0 || mode != nullptr) {
    node->custom1 &= Smooth;
  }
  if (node->custom2 != ObjectShape && node->custom2 != Sphere) {
    node->custom2 = ObjectShape;
  }
  if (bNodeSocket *radius = node->input_by_identifier("Radius"_ustr)) {
    bke::node_set_socket_availability(*ntree, *radius, node->custom2 == Sphere);
  }
  /* Reach always shows: the Mode may change while the game runs (a node saved with the dropdown
   * hid it outside Hold). */
  if (bNodeSocket *reach = node->input_by_identifier("Reach"_ustr)) {
    bke::node_set_socket_availability(*ntree, *reach, true);
  }
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = int16_t(Linear);
  node->custom2 = ObjectShape;
  node_update(ntree, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeKeepDistance"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_type_size(ntype, 170, 140, NODE_DEFAULT_MAX_WIDTH);

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_keep_distance_cc
