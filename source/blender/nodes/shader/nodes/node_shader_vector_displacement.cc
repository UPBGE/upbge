/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_vector_displacement_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Vector")).hide_value();
  b.add_input<decl::Float>(N_("Midlevel")).default_value(0.0f).min(0.0f).max(1000.0f);
  b.add_input<decl::Float>(N_("Scale")).default_value(1.0f).min(0.0f).max(1000.0f);
  b.add_output<decl::Vector>(N_("Displacement"));
}

static void node_shader_init_vector_displacement(bNodeTree *UNUSED(ntree), bNode *node)
{
  node->custom1 = SHD_SPACE_TANGENT; /* space */
}

static int gpu_shader_vector_displacement(GPUMaterial *mat,
                                          bNode *node,
                                          bNodeExecData *UNUSED(execdata),
                                          GPUNodeStack *in,
                                          GPUNodeStack *out)
{
  switch (node->custom1) {
    case SHD_SPACE_TANGENT:
      return GPU_stack_link(mat,
                            node,
                            "node_vector_displacement_tangent",
                            in,
                            out,
                            GPU_attribute(mat, CD_TANGENT, ""));
    case SHD_SPACE_OBJECT:
      return GPU_stack_link(mat, node, "node_vector_displacement_object", in, out);
    case SHD_SPACE_WORLD:
    default:
      return GPU_stack_link(mat, node, "node_vector_displacement_world", in, out);
  }
}

}  // namespace blender::nodes::node_shader_vector_displacement_cc

/* node type definition */
void register_node_type_sh_vector_displacement()
{
  namespace file_ns = blender::nodes::node_shader_vector_displacement_cc;

  static bNodeType ntype;

  sh_node_type_base(
      &ntype, SH_NODE_VECTOR_DISPLACEMENT, "Vector Displacement", NODE_CLASS_OP_VECTOR);
  ntype.declare = file_ns::node_declare;
  node_type_init(&ntype, file_ns::node_shader_init_vector_displacement);
  node_type_gpu(&ntype, file_ns::gpu_shader_vector_displacement);

  nodeRegisterType(&ntype);
}
