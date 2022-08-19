/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "UI_interface.h"
#include "UI_resources.h"

#include "GPU_material.h"

#include "COM_shader_node.hh"

#include "node_composite_util.hh"

/* **************** Map Range ******************** */

namespace blender::nodes::node_composite_map_range_cc {

static void cmp_node_map_range_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("Value"))
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .compositor_domain_priority(0);
  b.add_input<decl::Float>(N_("From Min"))
      .default_value(0.0f)
      .min(-10000.0f)
      .max(10000.0f)
      .compositor_domain_priority(1);
  b.add_input<decl::Float>(N_("From Max"))
      .default_value(1.0f)
      .min(-10000.0f)
      .max(10000.0f)
      .compositor_domain_priority(2);
  b.add_input<decl::Float>(N_("To Min"))
      .default_value(0.0f)
      .min(-10000.0f)
      .max(10000.0f)
      .compositor_domain_priority(3);
  b.add_input<decl::Float>(N_("To Max"))
      .default_value(1.0f)
      .min(-10000.0f)
      .max(10000.0f)
      .compositor_domain_priority(4);
  b.add_output<decl::Float>(N_("Value"));
}

static void node_composit_buts_map_range(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiLayout *col;

  col = uiLayoutColumn(layout, true);
  uiItemR(col, ptr, "use_clamp", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
}

using namespace blender::realtime_compositor;

class MapRangeShaderNode : public ShaderNode {
 public:
  using ShaderNode::ShaderNode;

  void compile(GPUMaterial *material) override
  {
    GPUNodeStack *inputs = get_inputs_array();
    GPUNodeStack *outputs = get_outputs_array();

    const float should_clamp = get_should_clamp();

    GPU_stack_link(material,
                   &bnode(),
                   "node_composite_map_range",
                   inputs,
                   outputs,
                   GPU_constant(&should_clamp));
  }

  bool get_should_clamp()
  {
    return bnode().custom1;
  }
};

static ShaderNode *get_compositor_shader_node(DNode node)
{
  return new MapRangeShaderNode(node);
}

}  // namespace blender::nodes::node_composite_map_range_cc

void register_node_type_cmp_map_range()
{
  namespace file_ns = blender::nodes::node_composite_map_range_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_MAP_RANGE, "Map Range", NODE_CLASS_OP_VECTOR);
  ntype.declare = file_ns::cmp_node_map_range_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_map_range;
  ntype.get_compositor_shader_node = file_ns::get_compositor_shader_node;

  nodeRegisterType(&ntype);
}
