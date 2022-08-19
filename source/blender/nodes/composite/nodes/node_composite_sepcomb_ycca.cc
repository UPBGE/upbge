/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_assert.h"

#include "GPU_material.h"

#include "COM_shader_node.hh"

#include "node_composite_util.hh"

/* **************** SEPARATE YCCA ******************** */

namespace blender::nodes::node_composite_separate_ycca_cc {

static void cmp_node_sepycca_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image"))
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_domain_priority(0);
  b.add_output<decl::Float>(N_("Y"));
  b.add_output<decl::Float>(N_("Cb"));
  b.add_output<decl::Float>(N_("Cr"));
  b.add_output<decl::Float>(N_("A"));
}

static void node_composit_init_mode_sepycca(bNodeTree *UNUSED(ntree), bNode *node)
{
  node->custom1 = 1; /* BLI_YCC_ITU_BT709 */
}

using namespace blender::realtime_compositor;

class SeparateYCCAShaderNode : public ShaderNode {
 public:
  using ShaderNode::ShaderNode;

  void compile(GPUMaterial *material) override
  {
    GPUNodeStack *inputs = get_inputs_array();
    GPUNodeStack *outputs = get_outputs_array();

    GPU_stack_link(material, &bnode(), get_shader_function_name(), inputs, outputs);
  }

  int get_mode()
  {
    return bnode().custom1;
  }

  const char *get_shader_function_name()
  {
    switch (get_mode()) {
      case BLI_YCC_ITU_BT601:
        return "node_composite_separate_ycca_itu_601";
      case BLI_YCC_ITU_BT709:
        return "node_composite_separate_ycca_itu_709";
      case BLI_YCC_JFIF_0_255:
        return "node_composite_separate_ycca_jpeg";
    }

    BLI_assert_unreachable();
    return nullptr;
  }
};

static ShaderNode *get_compositor_shader_node(DNode node)
{
  return new SeparateYCCAShaderNode(node);
}

}  // namespace blender::nodes::node_composite_separate_ycca_cc

void register_node_type_cmp_sepycca()
{
  namespace file_ns = blender::nodes::node_composite_separate_ycca_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_SEPYCCA_LEGACY, "Separate YCbCrA", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::cmp_node_sepycca_declare;
  node_type_init(&ntype, file_ns::node_composit_init_mode_sepycca);
  ntype.gather_link_search_ops = nullptr;
  ntype.get_compositor_shader_node = file_ns::get_compositor_shader_node;

  nodeRegisterType(&ntype);
}

/* **************** COMBINE YCCA ******************** */

namespace blender::nodes::node_composite_combine_ycca_cc {

static void cmp_node_combycca_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("Y")).min(0.0f).max(1.0f).compositor_domain_priority(0);
  b.add_input<decl::Float>(N_("Cb"))
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .compositor_domain_priority(1);
  b.add_input<decl::Float>(N_("Cr"))
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .compositor_domain_priority(2);
  b.add_input<decl::Float>(N_("A"))
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .compositor_domain_priority(3);
  b.add_output<decl::Color>(N_("Image"));
}

static void node_composit_init_mode_combycca(bNodeTree *UNUSED(ntree), bNode *node)
{
  node->custom1 = 1; /* BLI_YCC_ITU_BT709 */
}

using namespace blender::realtime_compositor;

class CombineYCCAShaderNode : public ShaderNode {
 public:
  using ShaderNode::ShaderNode;

  void compile(GPUMaterial *material) override
  {
    GPUNodeStack *inputs = get_inputs_array();
    GPUNodeStack *outputs = get_outputs_array();

    GPU_stack_link(material, &bnode(), get_shader_function_name(), inputs, outputs);
  }

  int get_mode()
  {
    return bnode().custom1;
  }

  const char *get_shader_function_name()
  {
    switch (get_mode()) {
      case BLI_YCC_ITU_BT601:
        return "node_composite_combine_ycca_itu_601";
      case BLI_YCC_ITU_BT709:
        return "node_composite_combine_ycca_itu_709";
      case BLI_YCC_JFIF_0_255:
        return "node_composite_combine_ycca_jpeg";
    }

    BLI_assert_unreachable();
    return nullptr;
  }
};

static ShaderNode *get_compositor_shader_node(DNode node)
{
  return new CombineYCCAShaderNode(node);
}

}  // namespace blender::nodes::node_composite_combine_ycca_cc

void register_node_type_cmp_combycca()
{
  namespace file_ns = blender::nodes::node_composite_combine_ycca_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_COMBYCCA_LEGACY, "Combine YCbCrA", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::cmp_node_combycca_declare;
  node_type_init(&ntype, file_ns::node_composit_init_mode_combycca);
  ntype.gather_link_search_ops = nullptr;
  ntype.get_compositor_shader_node = file_ns::get_compositor_shader_node;

  nodeRegisterType(&ntype);
}
