/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include <cmath>

#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"

#include "UI_resources.hh"

#include "GPU_shader.hh"

#include "COM_algorithm_smaa.hh"
#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

/* **************** ID Mask  ******************** */

namespace blender::nodes::node_composite_id_mask_cc {

static void cmp_node_idmask_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("ID value")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Int>("Index").default_value(0).min(0);
  b.add_input<decl::Bool>("Anti-Alias").default_value(false);

  b.add_output<decl::Float>("Alpha").structure_type(StructureType::Dynamic);
}

using namespace blender::compositor;

class IDMaskOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input_mask = get_input("ID value");
    if (input_mask.is_single_value()) {
      execute_single_value();
      return;
    }

    /* If anti-aliasing is disabled, write to the output directly, otherwise, write to a temporary
     * result to later perform anti-aliasing. */
    Result non_anti_aliased_mask = context().create_result(ResultType::Float);
    Result &output_mask = use_anti_aliasing() ? non_anti_aliased_mask : get_result("Alpha");

    if (this->context().use_gpu()) {
      this->execute_gpu(output_mask);
    }
    else {
      this->execute_cpu(output_mask);
    }

    if (this->use_anti_aliasing()) {
      smaa(context(), non_anti_aliased_mask, get_result("Alpha"));
      non_anti_aliased_mask.release();
    }
  }

  void execute_gpu(Result &output_mask)
  {
    gpu::Shader *shader = context().get_shader("compositor_id_mask");
    GPU_shader_bind(shader);

    GPU_shader_uniform_1i(shader, "index", get_index());

    const Result &input_mask = get_input("ID value");
    input_mask.bind_as_texture(shader, "input_mask_tx");

    const Domain domain = compute_domain();
    output_mask.allocate_texture(domain);
    output_mask.bind_as_image(shader, "output_mask_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    input_mask.unbind_as_texture();
    output_mask.unbind_as_image();
    GPU_shader_unbind();
  }

  void execute_cpu(Result &output_mask)
  {
    const int index = this->get_index();

    const Result &input_mask = get_input("ID value");

    const Domain domain = compute_domain();
    output_mask.allocate_texture(domain);

    parallel_for(domain.size, [&](const int2 texel) {
      float input_mask_value = input_mask.load_pixel<float>(texel);
      float mask = int(math::round(input_mask_value)) == index ? 1.0f : 0.0f;
      output_mask.store_pixel(texel, mask);
    });
  }

  void execute_single_value()
  {
    const float input_mask_value = get_input("ID value").get_single_value<float>();
    const float mask = int(round(input_mask_value)) == get_index() ? 1.0f : 0.0f;
    get_result("Alpha").allocate_single_value();
    get_result("Alpha").set_single_value(mask);
  }

  int get_index()
  {
    return math::max(0, this->get_input("Index").get_single_value_default(0));
  }

  bool use_anti_aliasing()
  {
    return this->get_input("Anti-Alias").get_single_value_default(false);
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new IDMaskOperation(context, node);
}

}  // namespace blender::nodes::node_composite_id_mask_cc

static void register_node_type_cmp_idmask()
{
  namespace file_ns = blender::nodes::node_composite_id_mask_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeIDMask", CMP_NODE_ID_MASK);
  ntype.ui_name = "ID Mask";
  ntype.ui_description = "Create a matte from an object or material index pass";
  ntype.enum_name_legacy = "ID_MASK";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::cmp_node_idmask_declare;
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_idmask)
