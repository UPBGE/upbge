/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_math_base.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "GPU_shader.h"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

/* **************** BILATERALBLUR ******************** */

namespace blender::nodes::node_composite_bilateralblur_cc {

static void cmp_node_bilateralblur_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image")).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Color>(N_("Determinator")).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_output<decl::Color>(N_("Image"));
}

static void node_composit_init_bilateralblur(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeBilateralBlurData *nbbd = MEM_cnew<NodeBilateralBlurData>(__func__);
  node->storage = nbbd;
  nbbd->iter = 1;
  nbbd->sigma_color = 0.3;
  nbbd->sigma_space = 5.0;
}

static void node_composit_buts_bilateralblur(uiLayout *layout,
                                             bContext *UNUSED(C),
                                             PointerRNA *ptr)
{
  uiLayout *col;

  col = uiLayoutColumn(layout, true);
  uiItemR(col, ptr, "iterations", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(col, ptr, "sigma_color", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(col, ptr, "sigma_space", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
}

using namespace blender::realtime_compositor;

class BilateralBlurOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input_image = get_input("Image");
    /* Single value inputs can't be blurred and are returned as is. */
    if (input_image.is_single_value()) {
      get_input("Image").pass_through(get_result("Image"));
      return;
    }

    GPUShader *shader = shader_manager().get("compositor_bilateral_blur");
    GPU_shader_bind(shader);

    GPU_shader_uniform_1i(shader, "radius", get_blur_radius());
    GPU_shader_uniform_1f(shader, "threshold", get_threshold());

    input_image.bind_as_texture(shader, "input_tx");

    const Result &determinator_image = get_input("Determinator");
    determinator_image.bind_as_texture(shader, "determinator_tx");

    const Domain domain = compute_domain();
    Result &output_image = get_result("Image");
    output_image.allocate_texture(domain);
    output_image.bind_as_image(shader, "output_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    GPU_shader_unbind();
    output_image.unbind_as_image();
    input_image.unbind_as_texture();
    determinator_image.unbind_as_texture();
  }

  int get_blur_radius()
  {
    return math::ceil(get_node_bilateral_blur_data().iter +
                      get_node_bilateral_blur_data().sigma_space);
  }

  float get_threshold()
  {
    return get_node_bilateral_blur_data().sigma_color;
  }

  NodeBilateralBlurData &get_node_bilateral_blur_data()
  {
    return *static_cast<NodeBilateralBlurData *>(bnode().storage);
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new BilateralBlurOperation(context, node);
}

}  // namespace blender::nodes::node_composite_bilateralblur_cc

void register_node_type_cmp_bilateralblur()
{
  namespace file_ns = blender::nodes::node_composite_bilateralblur_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_BILATERALBLUR, "Bilateral Blur", NODE_CLASS_OP_FILTER);
  ntype.declare = file_ns::cmp_node_bilateralblur_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_bilateralblur;
  node_type_init(&ntype, file_ns::node_composit_init_bilateralblur);
  node_type_storage(
      &ntype, "NodeBilateralBlurData", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  nodeRegisterType(&ntype);
}
