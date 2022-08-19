/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2013 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_sepcomb_hsv_cc {

/* **************** SEPARATE HSV ******************** */

static void node_declare_sephsv(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Color")).default_value({0.8f, 0.8f, 0.8f, 1.0});
  b.add_output<decl::Float>(N_("H"));
  b.add_output<decl::Float>(N_("S"));
  b.add_output<decl::Float>(N_("V"));
}

static int gpu_shader_sephsv(GPUMaterial *mat,
                             bNode *node,
                             bNodeExecData *UNUSED(execdata),
                             GPUNodeStack *in,
                             GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "separate_hsv", in, out);
}

}  // namespace blender::nodes::node_shader_sepcomb_hsv_cc

void register_node_type_sh_sephsv()
{
  namespace file_ns = blender::nodes::node_shader_sepcomb_hsv_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_SEPHSV_LEGACY, "Separate HSV", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::node_declare_sephsv;
  node_type_gpu(&ntype, file_ns::gpu_shader_sephsv);
  ntype.gather_link_search_ops = nullptr;

  nodeRegisterType(&ntype);
}

namespace blender::nodes::node_shader_sepcomb_hsv_cc {

/* **************** COMBINE HSV ******************** */

static void node_declare_combhsv(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("H")).default_value(0.0f).min(0.0f).max(1.0f).subtype(PROP_UNSIGNED);
  b.add_input<decl::Float>(N_("S")).default_value(0.0f).min(0.0f).max(1.0f).subtype(PROP_UNSIGNED);
  b.add_input<decl::Float>(N_("V")).default_value(0.0f).min(0.0f).max(1.0f).subtype(PROP_UNSIGNED);
  b.add_output<decl::Color>(N_("Color"));
}

static int gpu_shader_combhsv(GPUMaterial *mat,
                              bNode *node,
                              bNodeExecData *UNUSED(execdata),
                              GPUNodeStack *in,
                              GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "combine_hsv", in, out);
}

}  // namespace blender::nodes::node_shader_sepcomb_hsv_cc

void register_node_type_sh_combhsv()
{
  namespace file_ns = blender::nodes::node_shader_sepcomb_hsv_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_COMBHSV_LEGACY, "Combine HSV", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::node_declare_combhsv;
  node_type_gpu(&ntype, file_ns::gpu_shader_combhsv);
  ntype.gather_link_search_ops = nullptr;

  nodeRegisterType(&ntype);
}
