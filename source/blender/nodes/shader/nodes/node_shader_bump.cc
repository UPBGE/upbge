/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "UI_interface.h"
#include "UI_resources.h"

/* **************** BUMP ******************** */

namespace blender::nodes::node_shader_bump_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("Strength"))
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Float>(N_("Distance")).default_value(1.0f).min(0.0f).max(1000.0f);
  b.add_input<decl::Float>(N_("Height"))
      .default_value(1.0f)
      .min(-1000.0f)
      .max(1000.0f)
      .hide_value();
  b.add_input<decl::Vector>(N_("Normal")).min(-1.0f).max(1.0f).hide_value();
  b.add_output<decl::Vector>(N_("Normal"));
}

static void node_shader_buts_bump(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "invert", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, 0);
}

static int gpu_shader_bump(GPUMaterial *mat,
                           bNode *node,
                           bNodeExecData *UNUSED(execdata),
                           GPUNodeStack *in,
                           GPUNodeStack *out)
{
  /* If there is no Height input, the node becomes a no-op. */
  if (!in[2].link) {
    if (!in[3].link) {
      return GPU_link(mat, "world_normals_get", &out[0].link);
    }
    else {
      /* Actually running the bump code would normalize, but Cycles handles it as total no-op. */
      return GPU_link(mat, "vector_copy", in[3].link, &out[0].link);
    }
  }

  if (!in[3].link) {
    GPU_link(mat, "world_normals_get", &in[3].link);
  }

  const char *height_function = GPU_material_split_sub_function(mat, GPU_FLOAT, &in[2].link);

  GPUNodeLink *dheight = GPU_differentiate_float_function(height_function);

  float invert = (node->custom1) ? -1.0 : 1.0;

  return GPU_stack_link(mat, node, "node_bump", in, out, dheight, GPU_constant(&invert));
}

}  // namespace blender::nodes::node_shader_bump_cc

/* node type definition */
void register_node_type_sh_bump()
{
  namespace file_ns = blender::nodes::node_shader_bump_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_BUMP, "Bump", NODE_CLASS_OP_VECTOR);
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_bump;
  node_type_gpu(&ntype, file_ns::gpu_shader_bump);

  nodeRegisterType(&ntype);
}
