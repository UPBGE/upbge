/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_eevee_specular_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Base Color")).default_value({0.8f, 0.8f, 0.8f, 1.0f});
  b.add_input<decl::Color>(N_("Specular")).default_value({0.03f, 0.03f, 0.03f, 1.0f});
  b.add_input<decl::Float>(N_("Roughness"))
      .default_value(0.2f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Color>(N_("Emissive Color")).default_value({0.0f, 0.0f, 0.0f, 1.0f});
  b.add_input<decl::Float>(N_("Transparency"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Vector>(N_("Normal")).hide_value();
  b.add_input<decl::Float>(N_("Clear Coat"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Float>(N_("Clear Coat Roughness"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Vector>(N_("Clear Coat Normal")).hide_value();
  b.add_input<decl::Float>(N_("Ambient Occlusion")).hide_value();
  b.add_input<decl::Float>(N_("Weight")).unavailable();
  b.add_output<decl::Shader>(N_("BSDF"));
}

#define socket_not_zero(sock) (in[sock].link || (clamp_f(in[sock].vec[0], 0.0f, 1.0f) > 1e-5f))

static int node_shader_gpu_eevee_specular(GPUMaterial *mat,
                                          bNode *node,
                                          bNodeExecData *UNUSED(execdata),
                                          GPUNodeStack *in,
                                          GPUNodeStack *out)
{
  static float one = 1.0f;

  /* Normals */
  if (!in[5].link) {
    GPU_link(mat, "world_normals_get", &in[5].link);
  }

  /* Clearcoat Normals */
  if (!in[8].link) {
    GPU_link(mat, "world_normals_get", &in[8].link);
  }

  /* Occlusion */
  if (!in[9].link) {
    GPU_link(mat, "set_value", GPU_constant(&one), &in[9].link);
  }

  GPU_material_flag_set(mat, GPU_MATFLAG_DIFFUSE | GPU_MATFLAG_GLOSSY);

  float use_clear = (socket_not_zero(6)) ? 1.0f : 0.0f;

  return GPU_stack_link(mat, node, "node_eevee_specular", in, out, GPU_constant(&use_clear));
}

}  // namespace blender::nodes::node_shader_eevee_specular_cc

/* node type definition */
void register_node_type_sh_eevee_specular()
{
  namespace file_ns = blender::nodes::node_shader_eevee_specular_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_EEVEE_SPECULAR, "Specular BSDF", NODE_CLASS_SHADER);
  ntype.declare = file_ns::node_declare;
  node_type_gpu(&ntype, file_ns::node_shader_gpu_eevee_specular);

  nodeRegisterType(&ntype);
}
