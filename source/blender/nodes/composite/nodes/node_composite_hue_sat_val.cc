/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "GPU_material.h"

#include "COM_shader_node.hh"

#include "node_composite_util.hh"

/* **************** Hue Saturation ******************** */

namespace blender::nodes::node_composite_hue_sat_val_cc {

static void cmp_node_huesatval_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image"))
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_domain_priority(0);
  b.add_input<decl::Float>(N_("Hue"))
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(1);
  b.add_input<decl::Float>(N_("Saturation"))
      .default_value(1.0f)
      .min(0.0f)
      .max(2.0f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(2);
  b.add_input<decl::Float>(N_("Value"))
      .default_value(1.0f)
      .min(0.0f)
      .max(2.0f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(3);
  b.add_input<decl::Float>(N_("Fac"))
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .compositor_domain_priority(4);
  b.add_output<decl::Color>(N_("Image"));
}

using namespace blender::realtime_compositor;

class HueSaturationValueShaderNode : public ShaderNode {
 public:
  using ShaderNode::ShaderNode;

  void compile(GPUMaterial *material) override
  {
    GPUNodeStack *inputs = get_inputs_array();
    GPUNodeStack *outputs = get_outputs_array();

    GPU_stack_link(material, &bnode(), "node_composite_hue_saturation_value", inputs, outputs);
  }
};

static ShaderNode *get_compositor_shader_node(DNode node)
{
  return new HueSaturationValueShaderNode(node);
}

}  // namespace blender::nodes::node_composite_hue_sat_val_cc

void register_node_type_cmp_hue_sat()
{
  namespace file_ns = blender::nodes::node_composite_hue_sat_val_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_HUE_SAT, "Hue Saturation Value", NODE_CLASS_OP_COLOR);
  ntype.declare = file_ns::cmp_node_huesatval_declare;
  ntype.get_compositor_shader_node = file_ns::get_compositor_shader_node;

  nodeRegisterType(&ntype);
}
