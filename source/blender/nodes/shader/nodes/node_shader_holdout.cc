/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_holdout_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("Weight")).unavailable();
  b.add_output<decl::Shader>(N_("Holdout"));
}

static int gpu_shader_rgb(GPUMaterial *mat,
                          bNode *node,
                          bNodeExecData *UNUSED(execdata),
                          GPUNodeStack *in,
                          GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "node_holdout", in, out);
}

}  // namespace blender::nodes::node_shader_holdout_cc

/* node type definition */
void register_node_type_sh_holdout()
{
  namespace file_ns = blender::nodes::node_shader_holdout_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_HOLDOUT, "Holdout", NODE_CLASS_SHADER);
  ntype.declare = file_ns::node_declare;
  node_type_gpu(&ntype, file_ns::gpu_shader_rgb);

  nodeRegisterType(&ntype);
}
