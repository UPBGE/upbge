/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_float3x3.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vec_types.hh"
#include "BLI_math_vector.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "GPU_shader.h"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_directionalblur_cc {

static void cmp_node_directional_blur_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image")).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_output<decl::Color>(N_("Image"));
}

static void node_composit_init_dblur(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeDBlurData *ndbd = MEM_cnew<NodeDBlurData>(__func__);
  node->storage = ndbd;
  ndbd->iter = 1;
  ndbd->center_x = 0.5;
  ndbd->center_y = 0.5;
}

static void node_composit_buts_dblur(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiLayout *col;

  uiItemR(layout, ptr, "iterations", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "use_wrap", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);

  col = uiLayoutColumn(layout, true);
  uiItemL(col, IFACE_("Center:"), ICON_NONE);
  uiItemR(col, ptr, "center_x", UI_ITEM_R_SPLIT_EMPTY_NAME, IFACE_("X"), ICON_NONE);
  uiItemR(col, ptr, "center_y", UI_ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Y"), ICON_NONE);

  uiItemS(layout);

  col = uiLayoutColumn(layout, true);
  uiItemR(col, ptr, "distance", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(col, ptr, "angle", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);

  uiItemS(layout);

  uiItemR(layout, ptr, "spin", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "zoom", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
}

using namespace blender::realtime_compositor;

class DirectionalBlurOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    if (is_identity()) {
      get_input("Image").pass_through(get_result("Image"));
      return;
    }

    GPUShader *shader = shader_manager().get("compositor_directional_blur");
    GPU_shader_bind(shader);

    /* The number of iterations does not cover the original image, that is, the image with no
     * transformation. So add an extra iteration for the original image and put that into
     * consideration in the shader. */
    GPU_shader_uniform_1i(shader, "iterations", get_iterations() + 1);
    GPU_shader_uniform_mat3_as_mat4(shader, "inverse_transformation", get_transformation().ptr());

    const Result &input_image = get_input("Image");
    input_image.bind_as_texture(shader, "input_tx");

    GPU_texture_filter_mode(input_image.texture(), true);
    GPU_texture_wrap_mode(input_image.texture(), false, false);

    const Domain domain = compute_domain();
    Result &output_image = get_result("Image");
    output_image.allocate_texture(domain);
    output_image.bind_as_image(shader, "output_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    GPU_shader_unbind();
    output_image.unbind_as_image();
    input_image.unbind_as_texture();
  }

  /* Get the amount of translation that will be applied on each iteration. The translation is in
   * the negative x direction rotated in the clock-wise direction, hence the negative sign for the
   * rotation and translation vector. */
  float2 get_translation()
  {
    const float diagonal_length = math::length(float2(get_input("Image").domain().size));
    const float translation_amount = diagonal_length * get_node_directional_blur_data().distance;
    const float3x3 rotation = float3x3::from_rotation(-get_node_directional_blur_data().angle);
    return rotation * float2(-translation_amount / get_iterations(), 0.0f);
  }

  /* Get the amount of rotation that will be applied on each iteration. */
  float get_rotation()
  {
    return get_node_directional_blur_data().spin / get_iterations();
  }

  /* Get the amount of scale that will be applied on each iteration. The scale is identity when the
   * user supplies 0, so we add 1. */
  float2 get_scale()
  {
    return float2(1.0f + get_node_directional_blur_data().zoom / get_iterations());
  }

  float2 get_origin()
  {
    const float2 center = float2(get_node_directional_blur_data().center_x,
                                 get_node_directional_blur_data().center_y);
    return float2(get_input("Image").domain().size) * center;
  }

  float3x3 get_transformation()
  {
    /* Construct the transformation that will be applied on each iteration. */
    const float3x3 transformation = float3x3::from_translation_rotation_scale(
        get_translation(), get_rotation(), get_scale());
    /* Change the origin of the transformation to the user-specified origin. */
    const float3x3 origin_transformation = float3x3::from_origin_transformation(transformation,
                                                                                get_origin());
    /* The shader will transform the coordinates, not the image itself, so take the inverse. */
    return origin_transformation.inverted();
  }

  /* The actual number of iterations is 2 to the power of the user supplied iterations. The power
   * is implemented using a bit shift. But also make sure it doesn't exceed the upper limit which
   * is the number of diagonal pixels. */
  int get_iterations()
  {
    const int iterations = 2 << (get_node_directional_blur_data().iter - 1);
    const int upper_limit = math::ceil(math::length(float2(get_input("Image").domain().size)));
    return math::min(iterations, upper_limit);
  }

  /* Returns true if the operation does nothing and the input can be passed through. */
  bool is_identity()
  {
    const Result &input = get_input("Image");
    /* Single value inputs can't be blurred and are returned as is. */
    if (input.is_single_value()) {
      return true;
    }

    /* If any of the following options are non-zero, then the operation is not an identity. */
    if (get_node_directional_blur_data().distance != 0.0f) {
      return false;
    }

    if (get_node_directional_blur_data().spin != 0.0f) {
      return false;
    }

    if (get_node_directional_blur_data().zoom != 0.0f) {
      return false;
    }

    return true;
  }

  NodeDBlurData &get_node_directional_blur_data()
  {
    return *static_cast<NodeDBlurData *>(bnode().storage);
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new DirectionalBlurOperation(context, node);
}

}  // namespace blender::nodes::node_composite_directionalblur_cc

void register_node_type_cmp_dblur()
{
  namespace file_ns = blender::nodes::node_composite_directionalblur_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_DBLUR, "Directional Blur", NODE_CLASS_OP_FILTER);
  ntype.declare = file_ns::cmp_node_directional_blur_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_dblur;
  node_type_init(&ntype, file_ns::node_composit_init_dblur);
  node_type_storage(
      &ntype, "NodeDBlurData", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  nodeRegisterType(&ntype);
}
