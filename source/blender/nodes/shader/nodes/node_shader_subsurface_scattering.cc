/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

#include "UI_interface.h"
#include "UI_resources.h"

namespace blender::nodes::node_shader_subsurface_scattering_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Color")).default_value({0.8f, 0.8f, 0.8f, 1.0f});
  b.add_input<decl::Float>(N_("Scale")).default_value(1.0f).min(0.0f).max(1000.0f);
  b.add_input<decl::Vector>(N_("Radius"))
      .default_value({1.0f, 0.2f, 0.1f})
      .min(0.0f)
      .max(100.0f)
      .compact();
  b.add_input<decl::Float>(N_("IOR")).default_value(1.4f).min(1.01f).max(3.8f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Float>(N_("Anisotropy"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Vector>(N_("Normal")).hide_value();
  b.add_input<decl::Float>(N_("Weight")).unavailable();
  b.add_output<decl::Shader>(N_("BSSRDF"));
}

static void node_shader_buts_subsurface(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "falloff", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

static void node_shader_init_subsurface_scattering(bNodeTree *UNUSED(ntree), bNode *node)
{
  node->custom1 = SHD_SUBSURFACE_RANDOM_WALK;
  node->custom2 = true;
}

static int node_shader_gpu_subsurface_scattering(GPUMaterial *mat,
                                                 bNode *node,
                                                 bNodeExecData *UNUSED(execdata),
                                                 GPUNodeStack *in,
                                                 GPUNodeStack *out)
{
  if (!in[5].link) {
    GPU_link(mat, "world_normals_get", &in[5].link);
  }

  bNodeSocket *socket = (bNodeSocket *)BLI_findlink(&node->original->inputs, 2);
  bNodeSocketValueRGBA *socket_data = (bNodeSocketValueRGBA *)socket->default_value;
  /* For some reason it seems that the socket value is in ARGB format. */
  bool use_subsurf = GPU_material_sss_profile_create(mat, &socket_data->value[1]);

  float use_sss = (use_subsurf) ? 1.0f : 0.0f;

  GPU_material_flag_set(mat, GPU_MATFLAG_DIFFUSE | GPU_MATFLAG_SUBSURFACE);

  return GPU_stack_link(mat, node, "node_subsurface_scattering", in, out, GPU_uniform(&use_sss));
}

static void node_shader_update_subsurface_scattering(bNodeTree *ntree, bNode *node)
{
  const int sss_method = node->custom1;

  LISTBASE_FOREACH (bNodeSocket *, sock, &node->inputs) {
    if (STR_ELEM(sock->name, "IOR", "Anisotropy")) {
      nodeSetSocketAvailability(ntree, sock, sss_method != SHD_SUBSURFACE_BURLEY);
    }
  }
}

}  // namespace blender::nodes::node_shader_subsurface_scattering_cc

/* node type definition */
void register_node_type_sh_subsurface_scattering()
{
  namespace file_ns = blender::nodes::node_shader_subsurface_scattering_cc;

  static bNodeType ntype;

  sh_node_type_base(
      &ntype, SH_NODE_SUBSURFACE_SCATTERING, "Subsurface Scattering", NODE_CLASS_SHADER);
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_subsurface;
  node_type_size_preset(&ntype, NODE_SIZE_MIDDLE);
  node_type_init(&ntype, file_ns::node_shader_init_subsurface_scattering);
  node_type_gpu(&ntype, file_ns::node_shader_gpu_subsurface_scattering);
  node_type_update(&ntype, file_ns::node_shader_update_subsurface_scattering);

  nodeRegisterType(&ntype);
}
