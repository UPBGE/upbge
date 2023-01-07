/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_math_vector_types.hh"

#include "BKE_global.h"
#include "BKE_image.h"

#include "RNA_access.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "GPU_shader.h"
#include "GPU_state.h"
#include "GPU_texture.h"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

/* **************** VIEWER ******************** */

namespace blender::nodes::node_composite_viewer_cc {

static void cmp_node_viewer_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image")).default_value({0.0f, 0.0f, 0.0f, 1.0f});
  b.add_input<decl::Float>(N_("Alpha")).default_value(1.0f).min(0.0f).max(1.0f);
  b.add_input<decl::Float>(N_("Z")).default_value(1.0f).min(0.0f).max(1.0f);
}

static void node_composit_init_viewer(bNodeTree * /*ntree*/, bNode *node)
{
  ImageUser *iuser = MEM_cnew<ImageUser>(__func__);
  node->storage = iuser;
  iuser->sfra = 1;
  node->custom3 = 0.5f;
  node->custom4 = 0.5f;

  node->id = (ID *)BKE_image_ensure_viewer(G.main, IMA_TYPE_COMPOSITE, "Viewer Node");
}

static void node_composit_buts_viewer(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "use_alpha", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
}

static void node_composit_buts_viewer_ex(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiLayout *col;

  uiItemR(layout, ptr, "use_alpha", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "tile_order", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  if (RNA_enum_get(ptr, "tile_order") == 0) {
    col = uiLayoutColumn(layout, true);
    uiItemR(col, ptr, "center_x", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
    uiItemR(col, ptr, "center_y", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
  }
}

using namespace blender::realtime_compositor;

class ViewerOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &image = get_input("Image");
    const Result &alpha = get_input("Alpha");

    if (image.is_single_value() && alpha.is_single_value()) {
      execute_clear();
    }
    else if (ignore_alpha()) {
      execute_ignore_alpha();
    }
    else if (!node().input_by_identifier("Alpha")->is_logically_linked()) {
      execute_copy();
    }
    else {
      execute_set_alpha();
    }
  }

  /* Executes when all inputs are single values, in which case, the output texture can just be
   * cleared to the appropriate color. */
  void execute_clear()
  {
    const Result &image = get_input("Image");
    const Result &alpha = get_input("Alpha");

    float4 color = image.get_color_value();
    if (ignore_alpha()) {
      color.w = 1.0f;
    }
    else if (node().input_by_identifier("Alpha")->is_logically_linked()) {
      color.w = alpha.get_float_value();
    }

    GPU_texture_clear(context().get_output_texture(), GPU_DATA_FLOAT, color);
  }

  /* Executes when the alpha channel of the image is ignored. */
  void execute_ignore_alpha()
  {
    GPUShader *shader = shader_manager().get("compositor_write_output_opaque");
    GPU_shader_bind(shader);

    /* The compositing space might be limited to a smaller region of the output texture, so only
     * write into that compositing region. */
    const rcti compositing_region = context().get_compositing_region();
    const int2 lower_bound = int2(compositing_region.xmin, compositing_region.ymin);
    GPU_shader_uniform_2iv(shader, "compositing_region_lower_bound", lower_bound);

    const Result &image = get_input("Image");
    image.bind_as_texture(shader, "input_tx");

    GPUTexture *output_texture = context().get_output_texture();
    const int image_unit = GPU_shader_get_texture_binding(shader, "output_img");
    GPU_texture_image_bind(output_texture, image_unit);

    const int2 compositing_region_size = context().get_compositing_region_size();
    compute_dispatch_threads_at_least(shader, compositing_region_size);

    image.unbind_as_texture();
    GPU_texture_image_unbind(output_texture);
    GPU_shader_unbind();
  }

  /* Executes when the image texture is written with no adjustments and can thus be copied directly
   * to the output texture. */
  void execute_copy()
  {
    GPUShader *shader = shader_manager().get("compositor_write_output");
    GPU_shader_bind(shader);

    /* The compositing space might be limited to a smaller region of the output texture, so only
     * write into that compositing region. */
    const rcti compositing_region = context().get_compositing_region();
    const int2 lower_bound = int2(compositing_region.xmin, compositing_region.ymin);
    GPU_shader_uniform_2iv(shader, "compositing_region_lower_bound", lower_bound);

    const Result &image = get_input("Image");
    image.bind_as_texture(shader, "input_tx");

    GPUTexture *output_texture = context().get_output_texture();
    const int image_unit = GPU_shader_get_texture_binding(shader, "output_img");
    GPU_texture_image_bind(output_texture, image_unit);

    const int2 compositing_region_size = context().get_compositing_region_size();
    compute_dispatch_threads_at_least(shader, compositing_region_size);

    image.unbind_as_texture();
    GPU_texture_image_unbind(output_texture);
    GPU_shader_unbind();
  }

  /* Executes when the alpha channel of the image is set as the value of the input alpha. */
  void execute_set_alpha()
  {
    GPUShader *shader = shader_manager().get("compositor_write_output_alpha");
    GPU_shader_bind(shader);

    /* The compositing space might be limited to a smaller region of the output texture, so only
     * write into that compositing region. */
    const rcti compositing_region = context().get_compositing_region();
    const int2 lower_bound = int2(compositing_region.xmin, compositing_region.ymin);
    GPU_shader_uniform_2iv(shader, "compositing_region_lower_bound", lower_bound);

    const Result &image = get_input("Image");
    image.bind_as_texture(shader, "input_tx");

    const Result &alpha = get_input("Alpha");
    alpha.bind_as_texture(shader, "alpha_tx");

    GPUTexture *output_texture = context().get_output_texture();
    const int image_unit = GPU_shader_get_texture_binding(shader, "output_img");
    GPU_texture_image_bind(output_texture, image_unit);

    const int2 compositing_region_size = context().get_compositing_region_size();
    compute_dispatch_threads_at_least(shader, compositing_region_size);

    image.unbind_as_texture();
    alpha.unbind_as_texture();
    GPU_texture_image_unbind(output_texture);
    GPU_shader_unbind();
  }

  /* If true, the alpha channel of the image is set to 1, that is, it becomes opaque. If false, the
   * alpha channel of the image is retained, but only if the alpha input is not linked. If the
   * alpha input is linked, it the value of that input will be used as the alpha of the image. */
  bool ignore_alpha()
  {
    return bnode().custom2 & CMP_NODE_OUTPUT_IGNORE_ALPHA;
  }

  /* The operation domain has the same size as the compositing region without any transformations
   * applied. */
  Domain compute_domain() override
  {
    return Domain(context().get_compositing_region_size());
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new ViewerOperation(context, node);
}

}  // namespace blender::nodes::node_composite_viewer_cc

void register_node_type_cmp_viewer()
{
  namespace file_ns = blender::nodes::node_composite_viewer_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_VIEWER, "Viewer", NODE_CLASS_OUTPUT);
  ntype.declare = file_ns::cmp_node_viewer_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_viewer;
  ntype.draw_buttons_ex = file_ns::node_composit_buts_viewer_ex;
  ntype.flag |= NODE_PREVIEW;
  ntype.initfunc = file_ns::node_composit_init_viewer;
  node_type_storage(&ntype, "ImageUser", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  ntype.no_muting = true;

  nodeRegisterType(&ntype);
}
