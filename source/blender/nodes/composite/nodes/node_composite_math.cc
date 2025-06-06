/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include "NOD_math_functions.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "GPU_material.hh"

#include "COM_utilities_gpu_material.hh"

#include "node_composite_util.hh"

/* **************** SCALAR MATH ******************** */

namespace blender::nodes::node_composite_math_cc {

static void cmp_node_math_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Value")
      .default_value(0.5f)
      .min(-10000.0f)
      .max(10000.0f)
      .compositor_domain_priority(0);
  b.add_input<decl::Float>("Value", "Value_001")
      .default_value(0.5f)
      .min(-10000.0f)
      .max(10000.0f)
      .compositor_domain_priority(1);
  b.add_input<decl::Float>("Value", "Value_002")
      .default_value(0.5f)
      .min(-10000.0f)
      .max(10000.0f)
      .compositor_domain_priority(2);
  b.add_output<decl::Float>("Value");
}

using namespace blender::compositor;

static NodeMathOperation get_operation(const bNode &node)
{
  return static_cast<NodeMathOperation>(node.custom1);
}

static const char *get_shader_function_name(const bNode &node)
{
  return get_float_math_operation_info(get_operation(node))->shader_name.c_str();
}

static bool get_should_clamp(const bNode &node)
{
  return node.custom2 & SHD_MATH_CLAMP;
}

static int node_gpu_material(GPUMaterial *material,
                             bNode *node,
                             bNodeExecData * /*execdata*/,
                             GPUNodeStack *inputs,
                             GPUNodeStack *outputs)
{
  const bool is_valid = GPU_stack_link(
      material, node, get_shader_function_name(*node), inputs, outputs);

  if (!is_valid || !get_should_clamp(*node)) {
    return is_valid;
  }

  const float min = 0.0f;
  const float max = 1.0f;
  return GPU_link(material,
                  "clamp_value",
                  get_shader_node_output(*node, outputs, "Value").link,
                  GPU_constant(&min),
                  GPU_constant(&max),
                  &get_shader_node_output(*node, outputs, "Value").link);
}

}  // namespace blender::nodes::node_composite_math_cc

static void register_node_type_cmp_math()
{
  namespace file_ns = blender::nodes::node_composite_math_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeMath", CMP_NODE_MATH);
  ntype.ui_name = "Math";
  ntype.ui_description = "Perform math operations";
  ntype.enum_name_legacy = "MATH";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::cmp_node_math_declare;
  ntype.labelfunc = node_math_label;
  ntype.updatefunc = node_math_update;
  ntype.gpu_fn = file_ns::node_gpu_material;
  ntype.build_multi_function = blender::nodes::node_math_build_multi_function;
  ntype.gather_link_search_ops = nullptr;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_math)
