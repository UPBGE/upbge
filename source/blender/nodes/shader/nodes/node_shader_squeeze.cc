/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_squeeze_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("Value")).default_value(0.0f).min(-100.0f).max(100.0f);
  b.add_input<decl::Float>(N_("Width")).default_value(1.0f).min(-100.0f).max(100.0f);
  b.add_input<decl::Float>(N_("Center")).default_value(0.0f).min(-100.0f).max(100.0f);
  b.add_output<decl::Float>(N_("Value"));
}

static int gpu_shader_squeeze(GPUMaterial *mat,
                              bNode *node,
                              bNodeExecData *UNUSED(execdata),
                              GPUNodeStack *in,
                              GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "squeeze", in, out);
}

}  // namespace blender::nodes::node_shader_squeeze_cc

void register_node_type_sh_squeeze()
{
  namespace file_ns = blender::nodes::node_shader_squeeze_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_SQUEEZE, "Squeeze Value", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::node_declare;
  node_type_gpu(&ntype, file_ns::gpu_shader_squeeze);

  nodeRegisterType(&ntype);
}
