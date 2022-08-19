/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "GPU_material.h"

#include "COM_shader_node.hh"

#include "node_composite_util.hh"

/* **************** SEPARATE YUVA ******************** */

namespace blender::nodes::node_composite_separate_yuva_cc {

static void cmp_node_sepyuva_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image"))
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_domain_priority(0);
  b.add_output<decl::Float>(N_("Y"));
  b.add_output<decl::Float>(N_("U"));
  b.add_output<decl::Float>(N_("V"));
  b.add_output<decl::Float>(N_("A"));
}

using namespace blender::realtime_compositor;

class SeparateYUVAShaderNode : public ShaderNode {
 public:
  using ShaderNode::ShaderNode;

  void compile(GPUMaterial *material) override
  {
    GPUNodeStack *inputs = get_inputs_array();
    GPUNodeStack *outputs = get_outputs_array();

    GPU_stack_link(material, &bnode(), "node_composite_separate_yuva_itu_709", inputs, outputs);
  }
};

static ShaderNode *get_compositor_shader_node(DNode node)
{
  return new SeparateYUVAShaderNode(node);
}

}  // namespace blender::nodes::node_composite_separate_yuva_cc

void register_node_type_cmp_sepyuva()
{
  namespace file_ns = blender::nodes::node_composite_separate_yuva_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_SEPYUVA_LEGACY, "Separate YUVA", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::cmp_node_sepyuva_declare;
  ntype.gather_link_search_ops = nullptr;
  ntype.get_compositor_shader_node = file_ns::get_compositor_shader_node;

  nodeRegisterType(&ntype);
}

/* **************** COMBINE YUVA ******************** */

namespace blender::nodes::node_composite_combine_yuva_cc {

static void cmp_node_combyuva_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("Y")).min(0.0f).max(1.0f).compositor_domain_priority(0);
  b.add_input<decl::Float>(N_("U")).min(0.0f).max(1.0f).compositor_domain_priority(1);
  b.add_input<decl::Float>(N_("V")).min(0.0f).max(1.0f).compositor_domain_priority(2);
  b.add_input<decl::Float>(N_("A"))
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .compositor_domain_priority(3);
  b.add_output<decl::Color>(N_("Image"));
}

using namespace blender::realtime_compositor;

class CombineYUVAShaderNode : public ShaderNode {
 public:
  using ShaderNode::ShaderNode;

  void compile(GPUMaterial *material) override
  {
    GPUNodeStack *inputs = get_inputs_array();
    GPUNodeStack *outputs = get_outputs_array();

    GPU_stack_link(material, &bnode(), "node_composite_combine_yuva_itu_709", inputs, outputs);
  }
};

static ShaderNode *get_compositor_shader_node(DNode node)
{
  return new CombineYUVAShaderNode(node);
}

}  // namespace blender::nodes::node_composite_combine_yuva_cc

void register_node_type_cmp_combyuva()
{
  namespace file_ns = blender::nodes::node_composite_combine_yuva_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_COMBYUVA_LEGACY, "Combine YUVA", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::cmp_node_combyuva_declare;
  ntype.gather_link_search_ops = nullptr;
  ntype.get_compositor_shader_node = file_ns::get_compositor_shader_node;

  nodeRegisterType(&ntype);
}
