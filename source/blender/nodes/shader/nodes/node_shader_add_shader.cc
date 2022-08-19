/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_add_shader_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Shader>(N_("Shader"));
  b.add_input<decl::Shader>(N_("Shader"), "Shader_001");
  b.add_output<decl::Shader>(N_("Shader"));
}

static int node_shader_gpu_add_shader(GPUMaterial *mat,
                                      bNode *node,
                                      bNodeExecData *UNUSED(execdata),
                                      GPUNodeStack *in,
                                      GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "node_add_shader", in, out);
}

}  // namespace blender::nodes::node_shader_add_shader_cc

/* node type definition */
void register_node_type_sh_add_shader()
{
  namespace file_ns = blender::nodes::node_shader_add_shader_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_ADD_SHADER, "Add Shader", NODE_CLASS_SHADER);
  ntype.declare = file_ns::node_declare;
  node_type_gpu(&ntype, file_ns::node_shader_gpu_add_shader);

  nodeRegisterType(&ntype);
}
