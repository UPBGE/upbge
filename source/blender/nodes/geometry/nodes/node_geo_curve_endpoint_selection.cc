/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_endpoint_selection_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>(N_("Start Size"))
      .min(0)
      .default_value(1)
      .supports_field()
      .description(N_("The amount of points to select from the start of each spline"));
  b.add_input<decl::Int>(N_("End Size"))
      .min(0)
      .default_value(1)
      .supports_field()
      .description(N_("The amount of points to select from the end of each spline"));
  b.add_output<decl::Bool>(N_("Selection"))
      .field_source()
      .description(
          N_("The selection from the start and end of the splines based on the input sizes"));
}

class EndpointFieldInput final : public GeometryFieldInput {
  Field<int> start_size_;
  Field<int> end_size_;

 public:
  EndpointFieldInput(Field<int> start_size, Field<int> end_size)
      : GeometryFieldInput(CPPType::get<bool>(), "Endpoint Selection node"),
        start_size_(start_size),
        end_size_(end_size)
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const GeometryComponent &component,
                                 const eAttrDomain domain,
                                 IndexMask UNUSED(mask)) const final
  {
    if (component.type() != GEO_COMPONENT_TYPE_CURVE || domain != ATTR_DOMAIN_POINT) {
      return nullptr;
    }

    const CurveComponent &curve_component = static_cast<const CurveComponent &>(component);
    if (!curve_component.has_curves()) {
      return nullptr;
    }

    const Curves &curves_id = *curve_component.get_for_read();
    const bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id.geometry);
    if (curves.points_num() == 0) {
      return nullptr;
    }

    GeometryComponentFieldContext size_context{curve_component, ATTR_DOMAIN_CURVE};
    fn::FieldEvaluator evaluator{size_context, curves.curves_num()};
    evaluator.add(start_size_);
    evaluator.add(end_size_);
    evaluator.evaluate();
    const VArray<int> start_size = evaluator.get_evaluated<int>(0);
    const VArray<int> end_size = evaluator.get_evaluated<int>(1);

    Array<bool> selection(curves.points_num(), false);
    MutableSpan<bool> selection_span = selection.as_mutable_span();
    devirtualize_varray2(start_size, end_size, [&](const auto &start_size, const auto &end_size) {
      threading::parallel_for(curves.curves_range(), 1024, [&](IndexRange curves_range) {
        for (const int i : curves_range) {
          const IndexRange range = curves.points_for_curve(i);
          const int start = std::max(start_size[i], 0);
          const int end = std::max(end_size[i], 0);

          selection_span.slice(range.take_front(start)).fill(true);
          selection_span.slice(range.take_back(end)).fill(true);
        }
      });
    });

    return VArray<bool>::ForContainer(std::move(selection));
  };

  uint64_t hash() const override
  {
    return get_default_hash_2(start_size_, end_size_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const EndpointFieldInput *other_endpoint = dynamic_cast<const EndpointFieldInput *>(
            &other)) {
      return start_size_ == other_endpoint->start_size_ && end_size_ == other_endpoint->end_size_;
    }
    return false;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<int> start_size = params.extract_input<Field<int>>("Start Size");
  Field<int> end_size = params.extract_input<Field<int>>("End Size");
  Field<bool> selection_field{std::make_shared<EndpointFieldInput>(start_size, end_size)};
  params.set_output("Selection", std::move(selection_field));
}
}  // namespace blender::nodes::node_geo_curve_endpoint_selection_cc

void register_node_type_geo_curve_endpoint_selection()
{
  namespace file_ns = blender::nodes::node_geo_curve_endpoint_selection_cc;

  static bNodeType ntype;

  geo_node_type_base(
      &ntype, GEO_NODE_CURVE_ENDPOINT_SELECTION, "Endpoint Selection", NODE_CLASS_INPUT);
  ntype.declare = file_ns::node_declare;
  ntype.geometry_node_execute = file_ns::node_geo_exec;

  nodeRegisterType(&ntype);
}
