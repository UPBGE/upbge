/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "GPU_material.h"

#include "COM_shader_node.hh"

#include "node_composite_util.hh"

/* **************** SEPARATE RGBA ******************** */

namespace blender::nodes::node_composite_separate_rgba_cc {

static void cmp_node_seprgba_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image"))
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_domain_priority(0);
  b.add_output<decl::Float>(N_("R"));
  b.add_output<decl::Float>(N_("G"));
  b.add_output<decl::Float>(N_("B"));
  b.add_output<decl::Float>(N_("A"));
}

using namespace blender::realtime_compositor;

class SeparateRGBAShaderNode : public ShaderNode {
 public:
  using ShaderNode::ShaderNode;

  void compile(GPUMaterial *material) override
  {
    GPUNodeStack *inputs = get_inputs_array();
    GPUNodeStack *outputs = get_outputs_array();

    GPU_stack_link(material, &bnode(), "node_composite_separate_rgba", inputs, outputs);
  }
};

static ShaderNode *get_compositor_shader_node(DNode node)
{
  return new SeparateRGBAShaderNode(node);
}

}  // namespace blender::nodes::node_composite_separate_rgba_cc

void register_node_type_cmp_seprgba()
{
  namespace file_ns = blender::nodes::node_composite_separate_rgba_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_SEPRGBA_LEGACY, "Separate RGBA", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::cmp_node_seprgba_declare;
  ntype.gather_link_search_ops = nullptr;
  ntype.get_compositor_shader_node = file_ns::get_compositor_shader_node;

  nodeRegisterType(&ntype);
}

/* **************** COMBINE RGBA ******************** */

namespace blender::nodes::node_composite_combine_rgba_cc {

static void cmp_node_combrgba_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("R")).min(0.0f).max(1.0f).compositor_domain_priority(0);
  b.add_input<decl::Float>(N_("G")).min(0.0f).max(1.0f).compositor_domain_priority(1);
  b.add_input<decl::Float>(N_("B")).min(0.0f).max(1.0f).compositor_domain_priority(2);
  b.add_input<decl::Float>(N_("A"))
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .compositor_domain_priority(3);
  b.add_output<decl::Color>(N_("Image"));
}

using namespace blender::realtime_compositor;

class CombineRGBAShaderNode : public ShaderNode {
 public:
  using ShaderNode::ShaderNode;

  void compile(GPUMaterial *material) override
  {
    GPUNodeStack *inputs = get_inputs_array();
    GPUNodeStack *outputs = get_outputs_array();

    GPU_stack_link(material, &bnode(), "node_composite_combine_rgba", inputs, outputs);
  }
};

static ShaderNode *get_compositor_shader_node(DNode node)
{
  return new CombineRGBAShaderNode(node);
}

}  // namespace blender::nodes::node_composite_combine_rgba_cc

void register_node_type_cmp_combrgba()
{
  namespace file_ns = blender::nodes::node_composite_combine_rgba_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_COMBRGBA_LEGACY, "Combine RGBA", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::cmp_node_combrgba_declare;
  ntype.gather_link_search_ops = nullptr;
  ntype.get_compositor_shader_node = file_ns::get_compositor_shader_node;

  nodeRegisterType(&ntype);
}
