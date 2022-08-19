/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

#include "UI_interface.h"
#include "UI_resources.h"

namespace blender::nodes::node_shader_attribute_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>(N_("Color"));
  b.add_output<decl::Vector>(N_("Vector"));
  b.add_output<decl::Float>(N_("Fac"));
  b.add_output<decl::Float>(N_("Alpha"));
}

static void node_shader_buts_attribute(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "attribute_type", UI_ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Type"), ICON_NONE);
  uiItemR(layout, ptr, "attribute_name", UI_ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Name"), ICON_NONE);
}

static void node_shader_init_attribute(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeShaderAttribute *attr = MEM_cnew<NodeShaderAttribute>("NodeShaderAttribute");
  node->storage = attr;
}

static int node_shader_gpu_attribute(GPUMaterial *mat,
                                     bNode *node,
                                     bNodeExecData *UNUSED(execdata),
                                     GPUNodeStack *in,
                                     GPUNodeStack *out)
{
  NodeShaderAttribute *attr = static_cast<NodeShaderAttribute *>(node->storage);
  bool is_varying = attr->type == SHD_ATTRIBUTE_GEOMETRY;

  GPUNodeLink *cd_attr;

  if (is_varying) {
    cd_attr = GPU_attribute(mat, CD_AUTO_FROM_NAME, attr->name);
  }
  else {
    cd_attr = GPU_uniform_attribute(mat, attr->name, attr->type == SHD_ATTRIBUTE_INSTANCER);
  }

  if (STREQ(attr->name, "color")) {
    GPU_link(mat, "node_attribute_color", cd_attr, &cd_attr);
  }
  else if (STREQ(attr->name, "temperature")) {
    GPU_link(mat, "node_attribute_temperature", cd_attr, &cd_attr);
  }

  GPU_stack_link(mat, node, "node_attribute", in, out, cd_attr);

  int i;
  LISTBASE_FOREACH_INDEX (bNodeSocket *, sock, &node->outputs, i) {
    node_shader_gpu_bump_tex_coord(mat, node, &out[i].link);
  }

  return 1;
}

}  // namespace blender::nodes::node_shader_attribute_cc

/* node type definition */
void register_node_type_sh_attribute()
{
  namespace file_ns = blender::nodes::node_shader_attribute_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_ATTRIBUTE, "Attribute", NODE_CLASS_INPUT);
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_attribute;
  node_type_init(&ntype, file_ns::node_shader_init_attribute);
  node_type_storage(
      &ntype, "NodeShaderAttribute", node_free_standard_storage, node_copy_standard_storage);
  node_type_gpu(&ntype, file_ns::node_shader_gpu_attribute);

  nodeRegisterType(&ntype);
}
