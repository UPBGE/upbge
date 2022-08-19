/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "UI_interface.h"
#include "UI_resources.h"

static void node_combsep_color_init(bNodeTree *UNUSED(tree), bNode *node)
{
  NodeCombSepColor *data = MEM_cnew<NodeCombSepColor>(__func__);
  data->mode = NODE_COMBSEP_COLOR_RGB;
  node->storage = data;
}

/* **************** SEPARATE COLOR ******************** */

namespace blender::nodes::node_shader_separate_color_cc {

NODE_STORAGE_FUNCS(NodeCombSepColor)

static void sh_node_sepcolor_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Color")).default_value({0.8f, 0.8f, 0.8f, 1.0f});
  b.add_output<decl::Float>(N_("Red"));
  b.add_output<decl::Float>(N_("Green"));
  b.add_output<decl::Float>(N_("Blue"));
}

static void node_sepcolor_update(bNodeTree *UNUSED(ntree), bNode *node)
{
  const NodeCombSepColor &storage = node_storage(*node);
  node_combsep_color_label(&node->outputs, (NodeCombSepColorMode)storage.mode);
}

static const char *gpu_shader_get_name(int mode)
{
  switch (mode) {
    case NODE_COMBSEP_COLOR_RGB:
      return "separate_color_rgb";
    case NODE_COMBSEP_COLOR_HSV:
      return "separate_color_hsv";
    case NODE_COMBSEP_COLOR_HSL:
      return "separate_color_hsl";
  }

  return nullptr;
}

static int gpu_shader_sepcolor(GPUMaterial *mat,
                               bNode *node,
                               bNodeExecData *UNUSED(execdata),
                               GPUNodeStack *in,
                               GPUNodeStack *out)
{
  const NodeCombSepColor &storage = node_storage(*node);
  const char *name = gpu_shader_get_name(storage.mode);
  if (name != nullptr) {
    return GPU_stack_link(mat, node, name, in, out);
  }

  return 0;
}

}  // namespace blender::nodes::node_shader_separate_color_cc

void register_node_type_sh_sepcolor()
{
  namespace file_ns = blender::nodes::node_shader_separate_color_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_SEPARATE_COLOR, "Separate Color", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::sh_node_sepcolor_declare;
  node_type_update(&ntype, file_ns::node_sepcolor_update);
  node_type_init(&ntype, node_combsep_color_init);
  node_type_storage(
      &ntype, "NodeCombSepColor", node_free_standard_storage, node_copy_standard_storage);
  node_type_gpu(&ntype, file_ns::gpu_shader_sepcolor);

  nodeRegisterType(&ntype);
}

/* **************** COMBINE COLOR ******************** */

namespace blender::nodes::node_shader_combine_color_cc {

NODE_STORAGE_FUNCS(NodeCombSepColor)

static void sh_node_combcolor_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("Red")).default_value(0.0f).min(0.0f).max(1.0f).subtype(PROP_FACTOR);
  b.add_input<decl::Float>(N_("Green"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Float>(N_("Blue"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_output<decl::Color>(N_("Color"));
}

static void node_combcolor_update(bNodeTree *UNUSED(ntree), bNode *node)
{
  const NodeCombSepColor &storage = node_storage(*node);
  node_combsep_color_label(&node->inputs, (NodeCombSepColorMode)storage.mode);
}

static const char *gpu_shader_get_name(int mode)
{
  switch (mode) {
    case NODE_COMBSEP_COLOR_RGB:
      return "combine_color_rgb";
    case NODE_COMBSEP_COLOR_HSV:
      return "combine_color_hsv";
    case NODE_COMBSEP_COLOR_HSL:
      return "combine_color_hsl";
  }

  return nullptr;
}

static int gpu_shader_combcolor(GPUMaterial *mat,
                                bNode *node,
                                bNodeExecData *UNUSED(execdata),
                                GPUNodeStack *in,
                                GPUNodeStack *out)
{
  const NodeCombSepColor &storage = node_storage(*node);
  const char *name = gpu_shader_get_name(storage.mode);
  if (name != nullptr) {
    return GPU_stack_link(mat, node, name, in, out);
  }

  return 0;
}

}  // namespace blender::nodes::node_shader_combine_color_cc

void register_node_type_sh_combcolor()
{
  namespace file_ns = blender::nodes::node_shader_combine_color_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_COMBINE_COLOR, "Combine Color", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::sh_node_combcolor_declare;
  node_type_update(&ntype, file_ns::node_combcolor_update);
  node_type_init(&ntype, node_combsep_color_init);
  node_type_storage(
      &ntype, "NodeCombSepColor", node_free_standard_storage, node_copy_standard_storage);
  node_type_gpu(&ntype, file_ns::gpu_shader_combcolor);

  nodeRegisterType(&ntype);
}
