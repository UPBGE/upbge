/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_devirtualize_parameters.hh"
#include "BLI_generic_array.hh"
#include "BLI_length_parameterize.hh"

#include "BKE_curves.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "NOD_socket_search_link.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_sample_cc {

NODE_STORAGE_FUNCS(NodeGeometryCurveSample)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Curves"))
      .only_realized_data()
      .supported_type(GEO_COMPONENT_TYPE_CURVE);

  b.add_input<decl::Float>(N_("Value"), "Value_Float").hide_value().field_on_all();
  b.add_input<decl::Int>(N_("Value"), "Value_Int").hide_value().field_on_all();
  b.add_input<decl::Vector>(N_("Value"), "Value_Vector").hide_value().field_on_all();
  b.add_input<decl::Color>(N_("Value"), "Value_Color").hide_value().field_on_all();
  b.add_input<decl::Bool>(N_("Value"), "Value_Bool").hide_value().field_on_all();

  b.add_input<decl::Float>(N_("Factor"))
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .field_on_all()
      .make_available([](bNode &node) { node_storage(node).mode = GEO_NODE_CURVE_SAMPLE_FACTOR; });
  b.add_input<decl::Float>(N_("Length"))
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .field_on_all()
      .make_available([](bNode &node) { node_storage(node).mode = GEO_NODE_CURVE_SAMPLE_LENGTH; });
  b.add_input<decl::Int>(N_("Curve Index")).field_on_all().make_available([](bNode &node) {
    node_storage(node).use_all_curves = false;
  });

  b.add_output<decl::Float>(N_("Value"), "Value_Float").dependent_field();
  b.add_output<decl::Int>(N_("Value"), "Value_Int").dependent_field();
  b.add_output<decl::Vector>(N_("Value"), "Value_Vector").dependent_field();
  b.add_output<decl::Color>(N_("Value"), "Value_Color").dependent_field();
  b.add_output<decl::Bool>(N_("Value"), "Value_Bool").dependent_field();

  b.add_output<decl::Vector>(N_("Position")).dependent_field();
  b.add_output<decl::Vector>(N_("Tangent")).dependent_field();
  b.add_output<decl::Vector>(N_("Normal")).dependent_field();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "data_type", 0, "", ICON_NONE);
  uiItemR(layout, ptr, "mode", UI_ITEM_R_EXPAND, nullptr, ICON_NONE);
  uiItemR(layout, ptr, "use_all_curves", 0, nullptr, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryCurveSample *data = MEM_cnew<NodeGeometryCurveSample>(__func__);
  data->mode = GEO_NODE_CURVE_SAMPLE_FACTOR;
  data->use_all_curves = false;
  data->data_type = CD_PROP_FLOAT;
  node->storage = data;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const NodeGeometryCurveSample &storage = node_storage(*node);
  const GeometryNodeCurveSampleMode mode = (GeometryNodeCurveSampleMode)storage.mode;
  const eCustomDataType data_type = eCustomDataType(storage.data_type);

  bNodeSocket *in_socket_float = static_cast<bNodeSocket *>(node->inputs.first)->next;
  bNodeSocket *in_socket_int32 = in_socket_float->next;
  bNodeSocket *in_socket_vector = in_socket_int32->next;
  bNodeSocket *in_socket_color4f = in_socket_vector->next;
  bNodeSocket *in_socket_bool = in_socket_color4f->next;

  bNodeSocket *factor = in_socket_bool->next;
  bNodeSocket *length = factor->next;
  bNodeSocket *curve_index = length->next;

  nodeSetSocketAvailability(ntree, factor, mode == GEO_NODE_CURVE_SAMPLE_FACTOR);
  nodeSetSocketAvailability(ntree, length, mode == GEO_NODE_CURVE_SAMPLE_LENGTH);
  nodeSetSocketAvailability(ntree, curve_index, !storage.use_all_curves);

  nodeSetSocketAvailability(ntree, in_socket_vector, data_type == CD_PROP_FLOAT3);
  nodeSetSocketAvailability(ntree, in_socket_float, data_type == CD_PROP_FLOAT);
  nodeSetSocketAvailability(ntree, in_socket_color4f, data_type == CD_PROP_COLOR);
  nodeSetSocketAvailability(ntree, in_socket_bool, data_type == CD_PROP_BOOL);
  nodeSetSocketAvailability(ntree, in_socket_int32, data_type == CD_PROP_INT32);

  bNodeSocket *out_socket_float = static_cast<bNodeSocket *>(node->outputs.first);
  bNodeSocket *out_socket_int32 = out_socket_float->next;
  bNodeSocket *out_socket_vector = out_socket_int32->next;
  bNodeSocket *out_socket_color4f = out_socket_vector->next;
  bNodeSocket *out_socket_bool = out_socket_color4f->next;

  nodeSetSocketAvailability(ntree, out_socket_vector, data_type == CD_PROP_FLOAT3);
  nodeSetSocketAvailability(ntree, out_socket_float, data_type == CD_PROP_FLOAT);
  nodeSetSocketAvailability(ntree, out_socket_color4f, data_type == CD_PROP_COLOR);
  nodeSetSocketAvailability(ntree, out_socket_bool, data_type == CD_PROP_BOOL);
  nodeSetSocketAvailability(ntree, out_socket_int32, data_type == CD_PROP_INT32);
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const NodeDeclaration &declaration = *params.node_type().fixed_declaration;
  search_link_ops_for_declarations(params, declaration.inputs.as_span().take_front(4));
  search_link_ops_for_declarations(params, declaration.outputs.as_span().take_front(3));

  const std::optional<eCustomDataType> type = node_data_type_to_custom_data_type(
      eNodeSocketDatatype(params.other_socket().type));
  if (type && *type != CD_PROP_STRING) {
    /* The input and output sockets have the same name. */
    params.add_item(IFACE_("Value"), [type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeSampleCurve");
      node_storage(node).data_type = *type;
      params.update_and_connect_available_socket(node, "Value");
    });
  }
}

static void sample_indices_and_lengths(const Span<float> accumulated_lengths,
                                       const Span<float> sample_lengths,
                                       const GeometryNodeCurveSampleMode length_mode,
                                       const IndexMask mask,
                                       MutableSpan<int> r_segment_indices,
                                       MutableSpan<float> r_length_in_segment)
{
  const float total_length = accumulated_lengths.last();
  length_parameterize::SampleSegmentHint hint;

  mask.to_best_mask_type([&](const auto mask) {
    for (const int64_t i : mask) {
      const float sample_length = length_mode == GEO_NODE_CURVE_SAMPLE_FACTOR ?
                                      sample_lengths[i] * total_length :
                                      sample_lengths[i];
      int segment_i;
      float factor_in_segment;
      length_parameterize::sample_at_length(accumulated_lengths,
                                            std::clamp(sample_length, 0.0f, total_length),
                                            segment_i,
                                            factor_in_segment,
                                            &hint);
      const float segment_start = segment_i == 0 ? 0.0f : accumulated_lengths[segment_i - 1];
      const float segment_end = accumulated_lengths[segment_i];
      const float segment_length = segment_end - segment_start;

      r_segment_indices[i] = segment_i;
      r_length_in_segment[i] = factor_in_segment * segment_length;
    }
  });
}

static void sample_indices_and_factors_to_compressed(const Span<float> accumulated_lengths,
                                                     const Span<float> sample_lengths,
                                                     const GeometryNodeCurveSampleMode length_mode,
                                                     const IndexMask mask,
                                                     MutableSpan<int> r_segment_indices,
                                                     MutableSpan<float> r_factor_in_segment)
{
  const float total_length = accumulated_lengths.last();
  length_parameterize::SampleSegmentHint hint;

  switch (length_mode) {
    case GEO_NODE_CURVE_SAMPLE_FACTOR:
      mask.to_best_mask_type([&](const auto mask) {
        for (const int64_t i : IndexRange(mask.size())) {
          const float length = sample_lengths[mask[i]] * total_length;
          length_parameterize::sample_at_length(accumulated_lengths,
                                                std::clamp(length, 0.0f, total_length),
                                                r_segment_indices[i],
                                                r_factor_in_segment[i],
                                                &hint);
        }
      });
      break;
    case GEO_NODE_CURVE_SAMPLE_LENGTH:
      mask.to_best_mask_type([&](const auto mask) {
        for (const int64_t i : IndexRange(mask.size())) {
          const float length = sample_lengths[mask[i]];
          length_parameterize::sample_at_length(accumulated_lengths,
                                                std::clamp(length, 0.0f, total_length),
                                                r_segment_indices[i],
                                                r_factor_in_segment[i],
                                                &hint);
        }
      });
      break;
  }
}

/**
 * Given an array of accumulated lengths, find the segment indices that
 * sample lengths lie on, and how far along the segment they are.
 */
class SampleFloatSegmentsFunction : public fn::MultiFunction {
 private:
  Array<float> accumulated_lengths_;
  GeometryNodeCurveSampleMode length_mode_;

 public:
  SampleFloatSegmentsFunction(Array<float> accumulated_lengths,
                              const GeometryNodeCurveSampleMode length_mode)
      : accumulated_lengths_(std::move(accumulated_lengths)), length_mode_(length_mode)
  {
    static fn::MFSignature signature = create_signature();
    this->set_signature(&signature);
  }

  static fn::MFSignature create_signature()
  {
    fn::MFSignatureBuilder signature{"Sample Curve Index"};
    signature.single_input<float>("Length");

    signature.single_output<int>("Curve Index");
    signature.single_output<float>("Length in Curve");
    return signature.build();
  }

  void call(IndexMask mask, fn::MFParams params, fn::MFContext /*context*/) const override
  {
    const VArraySpan<float> lengths = params.readonly_single_input<float>(0, "Length");
    MutableSpan<int> indices = params.uninitialized_single_output<int>(1, "Curve Index");
    MutableSpan<float> lengths_in_segments = params.uninitialized_single_output<float>(
        2, "Length in Curve");

    sample_indices_and_lengths(
        accumulated_lengths_, lengths, length_mode_, mask, indices, lengths_in_segments);
  }
};

class SampleCurveFunction : public fn::MultiFunction {
 private:
  /**
   * The function holds a geometry set instead of curves or a curve component reference in order
   * to maintain a ownership of the geometry while the field tree is being built and used, so
   * that the curve is not freed before the function can execute.
   */
  GeometrySet geometry_set_;
  GField src_field_;
  GeometryNodeCurveSampleMode length_mode_;

  fn::MFSignature signature_;

  std::optional<bke::CurvesFieldContext> source_context_;
  std::unique_ptr<FieldEvaluator> source_evaluator_;
  const GVArray *source_data_;

 public:
  SampleCurveFunction(GeometrySet geometry_set,
                      const GeometryNodeCurveSampleMode length_mode,
                      const GField &src_field)
      : geometry_set_(std::move(geometry_set)), src_field_(src_field), length_mode_(length_mode)
  {
    signature_ = create_signature();
    this->set_signature(&signature_);
    this->evaluate_source();
  }

  fn::MFSignature create_signature()
  {
    fn::MFSignatureBuilder signature{"Sample Curve"};
    signature.single_input<int>("Curve Index");
    signature.single_input<float>("Length");
    signature.single_output<float3>("Position");
    signature.single_output<float3>("Tangent");
    signature.single_output<float3>("Normal");
    signature.single_output("Value", src_field_.cpp_type());
    return signature.build();
  }

  void call(IndexMask mask, fn::MFParams params, fn::MFContext /*context*/) const override
  {
    MutableSpan<float3> sampled_positions = params.uninitialized_single_output_if_required<float3>(
        2, "Position");
    MutableSpan<float3> sampled_tangents = params.uninitialized_single_output_if_required<float3>(
        3, "Tangent");
    MutableSpan<float3> sampled_normals = params.uninitialized_single_output_if_required<float3>(
        4, "Normal");
    GMutableSpan sampled_values = params.uninitialized_single_output_if_required(5, "Value");

    auto return_default = [&]() {
      if (!sampled_positions.is_empty()) {
        sampled_positions.fill_indices(mask, {0, 0, 0});
      }
      if (!sampled_tangents.is_empty()) {
        sampled_tangents.fill_indices(mask, {0, 0, 0});
      }
      if (!sampled_normals.is_empty()) {
        sampled_normals.fill_indices(mask, {0, 0, 0});
      }
    };

    if (!geometry_set_.has_curves()) {
      return return_default();
    }

    const Curves &curves_id = *geometry_set_.get_curves_for_read();
    const bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id.geometry);
    if (curves.points_num() == 0) {
      return return_default();
    }
    curves.ensure_can_interpolate_to_evaluated();
    Span<float3> evaluated_positions = curves.evaluated_positions();
    Span<float3> evaluated_tangents;
    Span<float3> evaluated_normals;
    if (!sampled_tangents.is_empty()) {
      evaluated_tangents = curves.evaluated_tangents();
    }
    if (!sampled_normals.is_empty()) {
      evaluated_normals = curves.evaluated_normals();
    }

    const VArray<int> curve_indices = params.readonly_single_input<int>(0, "Curve Index");
    const VArraySpan<float> lengths = params.readonly_single_input<float>(1, "Length");
    const VArray<bool> cyclic = curves.cyclic();

    Array<int> indices;
    Array<float> factors;
    GArray<> src_original_values(source_data_->type());
    GArray<> src_evaluated_values(source_data_->type());

    auto fill_invalid = [&](const IndexMask mask) {
      if (!sampled_positions.is_empty()) {
        sampled_positions.fill_indices(mask, float3(0));
      }
      if (!sampled_tangents.is_empty()) {
        sampled_tangents.fill_indices(mask, float3(0));
      }
      if (!sampled_normals.is_empty()) {
        sampled_normals.fill_indices(mask, float3(0));
      }
      if (!sampled_values.is_empty()) {
        attribute_math::convert_to_static_type(source_data_->type(), [&](auto dummy) {
          using T = decltype(dummy);
          sampled_values.typed<T>().fill_indices(mask, {});
        });
      }
    };

    auto sample_curve = [&](const int curve_i, const IndexMask mask) {
      const Span<float> accumulated_lengths = curves.evaluated_lengths_for_curve(curve_i,
                                                                                 cyclic[curve_i]);
      if (accumulated_lengths.is_empty()) {
        fill_invalid(mask);
        return;
      }
      /* Store the sampled indices and factors in arrays the size of the mask.
       * Then, during interpolation, move the results back to the masked indices. */
      indices.reinitialize(mask.size());
      factors.reinitialize(mask.size());
      sample_indices_and_factors_to_compressed(
          accumulated_lengths, lengths, length_mode_, mask, indices, factors);

      const IndexRange evaluated_points = curves.evaluated_points_for_curve(curve_i);
      if (!sampled_positions.is_empty()) {
        length_parameterize::interpolate_to_masked<float3>(
            evaluated_positions.slice(evaluated_points),
            indices,
            factors,
            mask,
            sampled_positions);
      }
      if (!sampled_tangents.is_empty()) {
        length_parameterize::interpolate_to_masked<float3>(
            evaluated_tangents.slice(evaluated_points), indices, factors, mask, sampled_tangents);
        for (const int64_t i : mask) {
          sampled_tangents[i] = math::normalize(sampled_tangents[i]);
        }
      }
      if (!sampled_normals.is_empty()) {
        length_parameterize::interpolate_to_masked<float3>(
            evaluated_normals.slice(evaluated_points), indices, factors, mask, sampled_normals);
        for (const int64_t i : mask) {
          sampled_normals[i] = math::normalize(sampled_normals[i]);
        }
      }
      if (!sampled_values.is_empty()) {
        const IndexRange points = curves.points_for_curve(curve_i);
        src_original_values.reinitialize(points.size());
        source_data_->materialize_compressed_to_uninitialized(points, src_original_values.data());
        src_evaluated_values.reinitialize(curves.evaluated_points_for_curve(curve_i).size());
        curves.interpolate_to_evaluated(curve_i, src_original_values, src_evaluated_values);
        attribute_math::convert_to_static_type(source_data_->type(), [&](auto dummy) {
          using T = decltype(dummy);
          const Span<T> src_evaluated_values_typed = src_evaluated_values.as_span().typed<T>();
          MutableSpan<T> sampled_values_typed = sampled_values.typed<T>();
          length_parameterize::interpolate_to_masked<T>(
              src_evaluated_values_typed, indices, factors, mask, sampled_values_typed);
        });
      }
    };

    if (const std::optional<int> curve_i = curve_indices.get_if_single()) {
      if (curves.curves_range().contains(*curve_i)) {
        sample_curve(*curve_i, mask);
      }
      else {
        fill_invalid(mask);
      }
    }
    else {
      Vector<int64_t> invalid_indices;
      MultiValueMap<int, int64_t> indices_per_curve;
      devirtualize_varray(curve_indices, [&](const auto curve_indices) {
        for (const int64_t i : mask) {
          const int curve_i = curve_indices[i];
          if (curves.curves_range().contains(curve_i)) {
            indices_per_curve.add(curve_i, i);
          }
          else {
            invalid_indices.append(i);
          }
        }
      });

      for (const int curve_i : indices_per_curve.keys()) {
        sample_curve(curve_i, IndexMask(indices_per_curve.lookup(curve_i)));
      }
      fill_invalid(IndexMask(invalid_indices));
    }
  }

 private:
  void evaluate_source()
  {
    const Curves &curves_id = *geometry_set_.get_curves_for_read();
    const bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id.geometry);
    source_context_.emplace(bke::CurvesFieldContext{curves, ATTR_DOMAIN_POINT});
    source_evaluator_ = std::make_unique<FieldEvaluator>(*source_context_, curves.points_num());
    source_evaluator_->add(src_field_);
    source_evaluator_->evaluate();
    source_data_ = &source_evaluator_->get_evaluated(0);
  }
};

static Array<float> curve_accumulated_lengths(const bke::CurvesGeometry &curves)
{

  Array<float> curve_lengths(curves.curves_num());
  const VArray<bool> cyclic = curves.cyclic();
  float length = 0.0f;
  for (const int i : curves.curves_range()) {
    length += curves.evaluated_length_total_for_curve(i, cyclic[i]);
    curve_lengths[i] = length;
  }
  return curve_lengths;
}

static GField get_input_attribute_field(GeoNodeExecParams &params, const eCustomDataType data_type)
{
  switch (data_type) {
    case CD_PROP_FLOAT:
      return params.extract_input<Field<float>>("Value_Float");
    case CD_PROP_FLOAT3:
      return params.extract_input<Field<float3>>("Value_Vector");
    case CD_PROP_COLOR:
      return params.extract_input<Field<ColorGeometry4f>>("Value_Color");
    case CD_PROP_BOOL:
      return params.extract_input<Field<bool>>("Value_Bool");
    case CD_PROP_INT32:
      return params.extract_input<Field<int>>("Value_Int");
    default:
      BLI_assert_unreachable();
  }
  return {};
}

static void output_attribute_field(GeoNodeExecParams &params, GField field)
{
  switch (bke::cpp_type_to_custom_data_type(field.cpp_type())) {
    case CD_PROP_FLOAT: {
      params.set_output("Value_Float", Field<float>(field));
      break;
    }
    case CD_PROP_FLOAT3: {
      params.set_output("Value_Vector", Field<float3>(field));
      break;
    }
    case CD_PROP_COLOR: {
      params.set_output("Value_Color", Field<ColorGeometry4f>(field));
      break;
    }
    case CD_PROP_BOOL: {
      params.set_output("Value_Bool", Field<bool>(field));
      break;
    }
    case CD_PROP_INT32: {
      params.set_output("Value_Int", Field<int>(field));
      break;
    }
    default:
      break;
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curves");
  if (!geometry_set.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }

  const Curves &curves_id = *geometry_set.get_curves_for_read();
  const bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id.geometry);
  if (curves.points_num() == 0) {
    params.set_default_remaining_outputs();
    return;
  }

  curves.ensure_evaluated_lengths();

  const NodeGeometryCurveSample &storage = node_storage(params.node());
  const GeometryNodeCurveSampleMode mode = (GeometryNodeCurveSampleMode)storage.mode;
  const eCustomDataType data_type = eCustomDataType(storage.data_type);

  Field<float> length_field = params.extract_input<Field<float>>(
      mode == GEO_NODE_CURVE_SAMPLE_FACTOR ? "Factor" : "Length");
  GField src_values_field = get_input_attribute_field(params, data_type);

  std::shared_ptr<FieldOperation> sample_op;
  if (curves.curves_num() == 1) {
    sample_op = FieldOperation::Create(
        std::make_unique<SampleCurveFunction>(
            std::move(geometry_set), mode, std::move(src_values_field)),
        {fn::make_constant_field<int>(0), std::move(length_field)});
  }
  else {
    if (storage.use_all_curves) {
      auto index_fn = std::make_unique<SampleFloatSegmentsFunction>(
          curve_accumulated_lengths(curves), mode);
      auto index_op = FieldOperation::Create(std::move(index_fn), {std::move(length_field)});
      Field<int> curve_index = Field<int>(index_op, 0);
      Field<float> length_in_curve = Field<float>(index_op, 1);
      sample_op = FieldOperation::Create(
          std::make_unique<SampleCurveFunction>(
              std::move(geometry_set), GEO_NODE_CURVE_SAMPLE_LENGTH, std::move(src_values_field)),
          {std::move(curve_index), std::move(length_in_curve)});
    }
    else {
      Field<int> curve_index = params.extract_input<Field<int>>("Curve Index");
      Field<float> length_in_curve = std::move(length_field);
      sample_op = FieldOperation::Create(
          std::make_unique<SampleCurveFunction>(
              std::move(geometry_set), mode, std::move(src_values_field)),
          {std::move(curve_index), std::move(length_in_curve)});
    }
  }

  params.set_output("Position", Field<float3>(sample_op, 0));
  params.set_output("Tangent", Field<float3>(sample_op, 1));
  params.set_output("Normal", Field<float3>(sample_op, 2));
  output_attribute_field(params, GField(sample_op, 3));
}

}  // namespace blender::nodes::node_geo_curve_sample_cc

void register_node_type_geo_curve_sample()
{
  namespace file_ns = blender::nodes::node_geo_curve_sample_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_SAMPLE_CURVE, "Sample Curve", NODE_CLASS_GEOMETRY);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  node_type_storage(
      &ntype, "NodeGeometryCurveSample", node_free_standard_storage, node_copy_standard_storage);
  ntype.draw_buttons = file_ns::node_layout;
  ntype.gather_link_search_ops = file_ns::node_gather_link_searches;
  nodeRegisterType(&ntype);
}
