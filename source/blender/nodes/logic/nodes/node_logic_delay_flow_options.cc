/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Delay (LogicNativeDelayFlow, NOD_logic_merged.hh), each the node
 * it was before 2026-11-08: Delay, Timer. They are not node types of their own:
 * logic_option_register keeps them for the merged node, which forwards its declaration, buttons,
 * init and update to the option it holds (node_logic_merged.cc).
 *
 * Both have Remaining and Waiting and a folded "Stop and Pause" panel (2026-11-17): Stop, Pause
 * and Resume flows acting on the node's countdowns (the node's op with an action,
 * LN_TIME_FLOW_*).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_delay_flow_options_cc {

/** The countdown sockets' descriptions, `done` the node's done output. */
static std::string remaining_description(const char *done)
{
  return std::string("Seconds until ") + done +
         " comes; 0 while nothing waits. It holds while paused";
}
static std::string waiting_description(const char *done)
{
  return std::string(done) + " is still to come (also while paused)";
}
static std::string stop_description(const char *done)
{
  return std::string("Forget the countdown: no ") + done +
         " comes from it (one already due this step still runs). Also ends a pause";
}
static const char *pause_description =
    "Hold the countdown where it is until Resume (a Flow arriving meanwhile waits paused too): "
    "a bomb that stops ticking while a menu is open";
static const char *resume_description = "Let a paused countdown run on from where it stopped";

}  // namespace blender::nodes::node_logic_delay_flow_options_cc

namespace blender::nodes::node_logic_delay_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  namespace countdown = node_logic_delay_flow_options_cc;
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr).is_default_link_socket();
  b.add_output<decl::Float>("Remaining"_ustr)
      .description(countdown::remaining_description("the next Done"));
  b.add_output<decl::Bool>("Waiting"_ustr)
      .description(countdown::waiting_description("the next Done"));
  b.add_default_layout();
  b.add_input<decl::Execution>("Flow"_ustr).is_default_link_socket();
  b.add_input<decl::Float>("Delay"_ustr).default_value(0.0f).min(0.0f);
  b.add_input<decl::Bool>("Ignore Timescale"_ustr).default_value(false);
  PanelDeclarationBuilder &panel = b.add_panel("Stop and Pause"_ustr).default_closed(true);
  panel.add_input<decl::Execution>("Stop"_ustr).description(countdown::stop_description("Done"));
  panel.add_input<decl::Execution>("Pause"_ustr).description(countdown::pause_description);
  panel.add_input<decl::Execution>("Resume"_ustr).description(countdown::resume_description);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeDelay"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_delay_cc

namespace blender::nodes::node_logic_timer_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  namespace countdown = node_logic_delay_flow_options_cc;
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("When Elapsed"_ustr, "Out"_ustr);
  b.add_output<decl::Float>("Remaining"_ustr)
      .description(countdown::remaining_description("When Elapsed"));
  b.add_output<decl::Bool>("Waiting"_ustr)
      .description(countdown::waiting_description("When Elapsed"));
  b.add_default_layout();
  b.add_input<decl::Execution>("Set Timer"_ustr);
  b.add_input<decl::Float>("Seconds"_ustr).default_value(0.0f).min(0.0f);
  b.add_input<decl::Bool>("Ignore Timescale"_ustr).default_value(false);
  PanelDeclarationBuilder &panel = b.add_panel("Stop and Pause"_ustr).default_closed(true);
  panel.add_input<decl::Execution>("Stop"_ustr)
      .description(countdown::stop_description("When Elapsed"));
  panel.add_input<decl::Execution>("Pause"_ustr).description(countdown::pause_description);
  panel.add_input<decl::Execution>("Resume"_ustr).description(countdown::resume_description);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeTimer"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_timer_cc
