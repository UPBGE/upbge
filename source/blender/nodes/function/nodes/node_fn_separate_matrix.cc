/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_inverse_eval_params.hh"
#include "NOD_value_elem_eval.hh"

#include "node_function_util.hh"

namespace blender::nodes::node_fn_separate_matrix_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  PanelDeclarationBuilder &column_a = b.add_panel("Column 1").default_closed(true);
  column_a.add_output<decl::Float>("Column 1 Row 1");
  column_a.add_output<decl::Float>("Column 1 Row 2");
  column_a.add_output<decl::Float>("Column 1 Row 3");
  column_a.add_output<decl::Float>("Column 1 Row 4");

  PanelDeclarationBuilder &column_b = b.add_panel("Column 2").default_closed(true);
  column_b.add_output<decl::Float>("Column 2 Row 1");
  column_b.add_output<decl::Float>("Column 2 Row 2");
  column_b.add_output<decl::Float>("Column 2 Row 3");
  column_b.add_output<decl::Float>("Column 2 Row 4");

  PanelDeclarationBuilder &column_c = b.add_panel("Column 3").default_closed(true);
  column_c.add_output<decl::Float>("Column 3 Row 1");
  column_c.add_output<decl::Float>("Column 3 Row 2");
  column_c.add_output<decl::Float>("Column 3 Row 3");
  column_c.add_output<decl::Float>("Column 3 Row 4");

  PanelDeclarationBuilder &column_d = b.add_panel("Column 4").default_closed(true);
  column_d.add_output<decl::Float>("Column 4 Row 1");
  column_d.add_output<decl::Float>("Column 4 Row 2");
  column_d.add_output<decl::Float>("Column 4 Row 3");
  column_d.add_output<decl::Float>("Column 4 Row 4");

  b.add_input<decl::Matrix>("Matrix");
}

static void copy_with_stride(const IndexMask &mask,
                             const Span<float> src,
                             const int64_t src_step,
                             const int64_t src_begin,
                             const int64_t dst_step,
                             const int64_t dst_begin,
                             MutableSpan<float> dst)
{
  if (dst.is_empty()) {
    return;
  }
  BLI_assert(src_begin < src_step);
  BLI_assert(dst_begin < dst_step);
  mask.foreach_index_optimized<int>([&](const int64_t index) {
    dst[dst_begin + dst_step * index] = src[src_begin + src_step * index];
  });
}

class SeparateMatrixFunction : public mf::MultiFunction {
 public:
  SeparateMatrixFunction()
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Separate Matrix", signature};
      builder.single_input<float4x4>("Matrix");

      builder.single_output<float>("Column 1 Row 1", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 1 Row 2", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 1 Row 3", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 1 Row 4", mf::ParamFlag::SupportsUnusedOutput);

      builder.single_output<float>("Column 2 Row 1", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 2 Row 2", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 2 Row 3", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 2 Row 4", mf::ParamFlag::SupportsUnusedOutput);

      builder.single_output<float>("Column 3 Row 1", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 3 Row 2", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 3 Row 3", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 3 Row 4", mf::ParamFlag::SupportsUnusedOutput);

      builder.single_output<float>("Column 4 Row 1", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 4 Row 2", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 4 Row 3", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float>("Column 4 Row 4", mf::ParamFlag::SupportsUnusedOutput);
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float4x4> matrices = params.readonly_single_input<float4x4>(0, "Matrix");

    MutableSpan<float> column_1_row_1 = params.uninitialized_single_output_if_required<float>(
        1, "Column 1 Row 1");
    MutableSpan<float> column_1_row_2 = params.uninitialized_single_output_if_required<float>(
        2, "Column 1 Row 2");
    MutableSpan<float> column_1_row_3 = params.uninitialized_single_output_if_required<float>(
        3, "Column 1 Row 3");
    MutableSpan<float> column_1_row_4 = params.uninitialized_single_output_if_required<float>(
        4, "Column 1 Row 4");

    MutableSpan<float> column_2_row_1 = params.uninitialized_single_output_if_required<float>(
        5, "Column 2 Row 1");
    MutableSpan<float> column_2_row_2 = params.uninitialized_single_output_if_required<float>(
        6, "Column 2 Row 2");
    MutableSpan<float> column_2_row_3 = params.uninitialized_single_output_if_required<float>(
        7, "Column 2 Row 3");
    MutableSpan<float> column_2_row_4 = params.uninitialized_single_output_if_required<float>(
        8, "Column 2 Row 4");

    MutableSpan<float> column_3_row_1 = params.uninitialized_single_output_if_required<float>(
        9, "Column 3 Row 1");
    MutableSpan<float> column_3_row_2 = params.uninitialized_single_output_if_required<float>(
        10, "Column 3 Row 2");
    MutableSpan<float> column_3_row_3 = params.uninitialized_single_output_if_required<float>(
        11, "Column 3 Row 3");
    MutableSpan<float> column_3_row_4 = params.uninitialized_single_output_if_required<float>(
        12, "Column 3 Row 4");

    MutableSpan<float> column_4_row_1 = params.uninitialized_single_output_if_required<float>(
        13, "Column 4 Row 1");
    MutableSpan<float> column_4_row_2 = params.uninitialized_single_output_if_required<float>(
        14, "Column 4 Row 2");
    MutableSpan<float> column_4_row_3 = params.uninitialized_single_output_if_required<float>(
        15, "Column 4 Row 3");
    MutableSpan<float> column_4_row_4 = params.uninitialized_single_output_if_required<float>(
        16, "Column 4 Row 4");

    if (const std::optional<float4x4> single = matrices.get_if_single()) {
      const float4x4 matrix = *single;
      column_1_row_1.fill(matrix[0][0]);
      column_1_row_2.fill(matrix[0][1]);
      column_1_row_3.fill(matrix[0][2]);
      column_1_row_4.fill(matrix[0][3]);

      column_2_row_1.fill(matrix[1][0]);
      column_2_row_2.fill(matrix[1][1]);
      column_2_row_3.fill(matrix[1][2]);
      column_2_row_4.fill(matrix[1][3]);

      column_3_row_1.fill(matrix[2][0]);
      column_3_row_2.fill(matrix[2][1]);
      column_3_row_3.fill(matrix[2][2]);
      column_3_row_4.fill(matrix[2][3]);

      column_4_row_1.fill(matrix[3][0]);
      column_4_row_2.fill(matrix[3][1]);
      column_4_row_3.fill(matrix[3][2]);
      column_4_row_4.fill(matrix[3][3]);
      return;
    }

    const VArraySpan<float4x4> span_matrices(matrices);
    const Span<float> components = span_matrices.cast<float>();

    copy_with_stride(mask, components, 16, 0, 1, 0, column_1_row_1);
    copy_with_stride(mask, components, 16, 1, 1, 0, column_1_row_2);
    copy_with_stride(mask, components, 16, 2, 1, 0, column_1_row_3);
    copy_with_stride(mask, components, 16, 3, 1, 0, column_1_row_4);

    copy_with_stride(mask, components, 16, 4, 1, 0, column_2_row_1);
    copy_with_stride(mask, components, 16, 5, 1, 0, column_2_row_2);
    copy_with_stride(mask, components, 16, 6, 1, 0, column_2_row_3);
    copy_with_stride(mask, components, 16, 7, 1, 0, column_2_row_4);

    copy_with_stride(mask, components, 16, 8, 1, 0, column_3_row_1);
    copy_with_stride(mask, components, 16, 9, 1, 0, column_3_row_2);
    copy_with_stride(mask, components, 16, 10, 1, 0, column_3_row_3);
    copy_with_stride(mask, components, 16, 11, 1, 0, column_3_row_4);

    copy_with_stride(mask, components, 16, 12, 1, 0, column_4_row_1);
    copy_with_stride(mask, components, 16, 13, 1, 0, column_4_row_2);
    copy_with_stride(mask, components, 16, 14, 1, 0, column_4_row_3);
    copy_with_stride(mask, components, 16, 15, 1, 0, column_4_row_4);
  }
};

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const static SeparateMatrixFunction fn;
  builder.set_matching_fn(fn);
}

static void node_eval_elem(value_elem::ElemEvalParams &params)
{
  using namespace value_elem;
  const MatrixElem matrix_elem = params.get_input_elem<MatrixElem>("Matrix");
  std::array<std::array<FloatElem, 4>, 4> output_elems;

  output_elems[3][0] = matrix_elem.translation.x;
  output_elems[3][1] = matrix_elem.translation.y;
  output_elems[3][2] = matrix_elem.translation.z;

  if (matrix_elem.rotation || matrix_elem.scale) {
    for (const int col : IndexRange(3)) {
      for (const int row : IndexRange(3)) {
        output_elems[col][row] = FloatElem::all();
      }
    }
  }

  if (matrix_elem.any_non_transform) {
    for (const int col : IndexRange(4)) {
      output_elems[col][3] = FloatElem::all();
    }
  }

  for (const int col : IndexRange(4)) {
    for (const int row : IndexRange(4)) {
      const bNodeSocket &socket = params.node.output_socket(col * 4 + row);
      params.set_output_elem(socket.identifier, output_elems[col][row]);
    }
  }
}

static void node_eval_inverse_elem(value_elem::InverseElemEvalParams &params)
{
  using namespace value_elem;

  std::array<std::array<FloatElem, 4>, 4> output_elems;
  for (const int col : IndexRange(4)) {
    for (const int row : IndexRange(4)) {
      const bNodeSocket &socket = params.node.output_socket(col * 4 + row);
      output_elems[col][row] = params.get_output_elem<FloatElem>(socket.identifier);
    }
  }

  MatrixElem matrix_elem;
  matrix_elem.translation.x = output_elems[3][0];
  matrix_elem.translation.y = output_elems[3][1];
  matrix_elem.translation.z = output_elems[3][2];

  bool any_inner_3x3 = false;
  for (const int col : IndexRange(3)) {
    for (const int row : IndexRange(3)) {
      any_inner_3x3 |= output_elems[col][row];
    }
  }

  if (any_inner_3x3) {
    matrix_elem.rotation = RotationElem::all();
    matrix_elem.scale = VectorElem::all();
  }

  const bool any_non_transform = output_elems[0][3] || output_elems[1][3] || output_elems[2][3] ||
                                 output_elems[3][3];
  if (any_non_transform) {
    matrix_elem.any_non_transform = FloatElem::all();
  }

  params.set_input_elem("Matrix", matrix_elem);
}

static void node_eval_inverse(inverse_eval::InverseEvalParams &params)
{
  float4x4 matrix;
  for (const int col : IndexRange(4)) {
    for (const int row : IndexRange(4)) {
      const bNodeSocket &socket = params.node.output_socket(col * 4 + row);
      matrix[col][row] = params.get_output<float>(socket.identifier);
    }
  }
  params.set_input("Matrix", matrix);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  fn_node_type_base(&ntype, "FunctionNodeSeparateMatrix", FN_NODE_SEPARATE_MATRIX);
  ntype.ui_name = "Separate Matrix";
  ntype.ui_description = "Split a 4x4 matrix into its individual values";
  ntype.enum_name_legacy = "SEPARATE_MATRIX";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  ntype.eval_elem = node_eval_elem;
  ntype.eval_inverse_elem = node_eval_inverse_elem;
  ntype.eval_inverse = node_eval_inverse;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_separate_matrix_cc
