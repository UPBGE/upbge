/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_gamma_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Color")).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Float>(N_("Gamma"))
      .default_value(1.0f)
      .min(0.001f)
      .max(10.0f)
      .subtype(PROP_UNSIGNED);
  b.add_output<decl::Color>(N_("Color"));
}

static int node_shader_gpu_gamma(GPUMaterial *mat,
                                 bNode *node,
                                 bNodeExecData *UNUSED(execdata),
                                 GPUNodeStack *in,
                                 GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "node_gamma", in, out);
}

}  // namespace blender::nodes::node_shader_gamma_cc

void register_node_type_sh_gamma()
{
  namespace file_ns = blender::nodes::node_shader_gamma_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_GAMMA, "Gamma", NODE_CLASS_OP_COLOR);
  ntype.declare = file_ns::node_declare;
  node_type_gpu(&ntype, file_ns::node_shader_gpu_gamma);

  nodeRegisterType(&ntype);
}
