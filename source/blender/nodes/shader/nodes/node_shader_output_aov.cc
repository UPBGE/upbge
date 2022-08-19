/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

#include "BLI_hash.h"

#include "UI_interface.h"
#include "UI_resources.h"

namespace blender::nodes::node_shader_output_aov_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Color")).default_value({0.0f, 0.0f, 0.0f, 1.0f});
  b.add_input<decl::Float>(N_("Value")).default_value(0.0f).min(0.0f).max(1.0f);
}

static void node_shader_buts_output_aov(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "name", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
}

static void node_shader_init_output_aov(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeShaderOutputAOV *aov = MEM_cnew<NodeShaderOutputAOV>("NodeShaderOutputAOV");
  node->storage = aov;
}

static int node_shader_gpu_output_aov(GPUMaterial *mat,
                                      bNode *node,
                                      bNodeExecData *UNUSED(execdata),
                                      GPUNodeStack *in,
                                      GPUNodeStack *out)
{
  GPUNodeLink *outlink;
  NodeShaderOutputAOV *aov = (NodeShaderOutputAOV *)node->storage;
  uint hash = BLI_hash_string(aov->name);
  /* WORKAROUND: We don't support int/uint constants for now. So make sure the aliasing works.
   * We cast back to uint in GLSL. */
  BLI_STATIC_ASSERT(sizeof(float) == sizeof(uint),
                    "GPUCodegen: AOV hash needs float and uint to be the same size.");
  GPUNodeLink *hash_link = GPU_constant((float *)&hash);

  GPU_material_flag_set(mat, GPU_MATFLAG_AOV);
  GPU_stack_link(mat, node, "node_output_aov", in, out, hash_link, &outlink);
  GPU_material_add_output_link_aov(mat, outlink, hash);
  return true;
}

}  // namespace blender::nodes::node_shader_output_aov_cc

/* node type definition */
void register_node_type_sh_output_aov()
{
  namespace file_ns = blender::nodes::node_shader_output_aov_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_OUTPUT_AOV, "AOV Output", NODE_CLASS_OUTPUT);
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_output_aov;
  node_type_init(&ntype, file_ns::node_shader_init_output_aov);
  node_type_storage(
      &ntype, "NodeShaderOutputAOV", node_free_standard_storage, node_copy_standard_storage);
  node_type_gpu(&ntype, file_ns::node_shader_gpu_output_aov);

  ntype.no_muting = true;

  nodeRegisterType(&ntype);
}
