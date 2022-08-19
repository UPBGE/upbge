/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_spline.hh"
#include "BLI_task.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "NOD_socket_search_link.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_trim_cc {

using blender::attribute_math::mix2;

NODE_STORAGE_FUNCS(NodeGeometryCurveTrim)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Curve")).supported_type(GEO_COMPONENT_TYPE_CURVE);
  b.add_input<decl::Float>(N_("Start"))
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .make_available([](bNode &node) { node_storage(node).mode = GEO_NODE_CURVE_SAMPLE_FACTOR; })
      .supports_field();
  b.add_input<decl::Float>(N_("End"))
      .min(0.0f)
      .max(1.0f)
      .default_value(1.0f)
      .subtype(PROP_FACTOR)
      .make_available([](bNode &node) { node_storage(node).mode = GEO_NODE_CURVE_SAMPLE_FACTOR; })
      .supports_field();
  b.add_input<decl::Float>(N_("Start"), "Start_001")
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .make_available([](bNode &node) { node_storage(node).mode = GEO_NODE_CURVE_SAMPLE_LENGTH; })
      .supports_field();
  b.add_input<decl::Float>(N_("End"), "End_001")
      .min(0.0f)
      .default_value(1.0f)
      .subtype(PROP_DISTANCE)
      .make_available([](bNode &node) { node_storage(node).mode = GEO_NODE_CURVE_SAMPLE_LENGTH; })
      .supports_field();
  b.add_output<decl::Geometry>(N_("Curve"));
}

static void node_layout(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "mode", UI_ITEM_R_EXPAND, nullptr, ICON_NONE);
}

static void node_init(bNodeTree *UNUSED(tree), bNode *node)
{
  NodeGeometryCurveTrim *data = MEM_cnew<NodeGeometryCurveTrim>(__func__);

  data->mode = GEO_NODE_CURVE_SAMPLE_FACTOR;
  node->storage = data;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const NodeGeometryCurveTrim &storage = node_storage(*node);
  const GeometryNodeCurveSampleMode mode = (GeometryNodeCurveSampleMode)storage.mode;

  bNodeSocket *start_fac = ((bNodeSocket *)node->inputs.first)->next;
  bNodeSocket *end_fac = start_fac->next;
  bNodeSocket *start_len = end_fac->next;
  bNodeSocket *end_len = start_len->next;

  nodeSetSocketAvailability(ntree, start_fac, mode == GEO_NODE_CURVE_SAMPLE_FACTOR);
  nodeSetSocketAvailability(ntree, end_fac, mode == GEO_NODE_CURVE_SAMPLE_FACTOR);
  nodeSetSocketAvailability(ntree, start_len, mode == GEO_NODE_CURVE_SAMPLE_LENGTH);
  nodeSetSocketAvailability(ntree, end_len, mode == GEO_NODE_CURVE_SAMPLE_LENGTH);
}

class SocketSearchOp {
 public:
  StringRef socket_name;
  GeometryNodeCurveSampleMode mode;
  void operator()(LinkSearchOpParams &params)
  {
    bNode &node = params.add_node("GeometryNodeTrimCurve");
    node_storage(node).mode = mode;
    params.update_and_connect_available_socket(node, socket_name);
  }
};

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const NodeDeclaration &declaration = *params.node_type().fixed_declaration;

  search_link_ops_for_declarations(params, declaration.outputs());
  search_link_ops_for_declarations(params, declaration.inputs().take_front(1));

  if (params.in_out() == SOCK_IN) {
    if (params.node_tree().typeinfo->validate_link(
            static_cast<eNodeSocketDatatype>(params.other_socket().type), SOCK_FLOAT)) {
      params.add_item(IFACE_("Start (Factor)"),
                      SocketSearchOp{"Start", GEO_NODE_CURVE_SAMPLE_FACTOR});
      params.add_item(IFACE_("End (Factor)"), SocketSearchOp{"End", GEO_NODE_CURVE_SAMPLE_FACTOR});
      params.add_item(IFACE_("Start (Length)"),
                      SocketSearchOp{"Start", GEO_NODE_CURVE_SAMPLE_LENGTH});
      params.add_item(IFACE_("End (Length)"), SocketSearchOp{"End", GEO_NODE_CURVE_SAMPLE_LENGTH});
    }
  }
}

struct TrimLocation {
  /* Control point index at the start side of the trim location. */
  int left_index;
  /* Control point index at the end of the trim location's segment. */
  int right_index;
  /* The factor between the left and right indices. */
  float factor;
};

template<typename T>
static void shift_slice_to_start(MutableSpan<T> data, const int start_index, const int num)
{
  BLI_assert(start_index + num - 1 <= data.size());
  memmove(data.data(), &data[start_index], sizeof(T) * num);
}

/* Shift slice to start of span and modifies start and end data. */
template<typename T>
static void linear_trim_data(const TrimLocation &start,
                             const TrimLocation &end,
                             MutableSpan<T> data)
{
  const int num = end.right_index - start.left_index + 1;

  if (start.left_index > 0) {
    shift_slice_to_start<T>(data, start.left_index, num);
  }

  const T start_data = mix2<T>(start.factor, data.first(), data[1]);
  const T end_data = mix2<T>(end.factor, data[num - 2], data[num - 1]);

  data.first() = start_data;
  data[num - 1] = end_data;
}

/**
 * Identical operation as #linear_trim_data, but copy data to a new #MutableSpan rather than
 * modifying the original data.
 */
template<typename T>
static void linear_trim_to_output_data(const TrimLocation &start,
                                       const TrimLocation &end,
                                       Span<T> src,
                                       MutableSpan<T> dst)
{
  const int num = end.right_index - start.left_index + 1;

  const T start_data = mix2<T>(start.factor, src[start.left_index], src[start.right_index]);
  const T end_data = mix2<T>(end.factor, src[end.left_index], src[end.right_index]);

  dst.copy_from(src.slice(start.left_index, num));
  dst.first() = start_data;
  dst.last() = end_data;
}

/* Look up the control points to the left and right of factor, and get the factor between them. */
static TrimLocation lookup_control_point_position(const Spline::LookupResult &lookup,
                                                  const BezierSpline &spline)
{
  Span<int> offsets = spline.control_point_offsets();

  const int *offset = std::lower_bound(offsets.begin(), offsets.end(), lookup.evaluated_index);
  const int index = offset - offsets.begin();

  const int left = offsets[index] > lookup.evaluated_index ? index - 1 : index;
  const int right = left == (spline.size() - 1) ? 0 : left + 1;

  const float offset_in_segment = lookup.evaluated_index + lookup.factor - offsets[left];
  const int segment_eval_num = offsets[left + 1] - offsets[left];
  const float factor = std::clamp(offset_in_segment / segment_eval_num, 0.0f, 1.0f);

  return {left, right, factor};
}

static void trim_poly_spline(Spline &spline,
                             const Spline::LookupResult &start_lookup,
                             const Spline::LookupResult &end_lookup)
{
  /* Poly splines have a 1 to 1 mapping between control points and evaluated points. */
  const TrimLocation start = {
      start_lookup.evaluated_index, start_lookup.next_evaluated_index, start_lookup.factor};
  const TrimLocation end = {
      end_lookup.evaluated_index, end_lookup.next_evaluated_index, end_lookup.factor};

  const int num = end.right_index - start.left_index + 1;

  linear_trim_data<float3>(start, end, spline.positions());
  linear_trim_data<float>(start, end, spline.radii());
  linear_trim_data<float>(start, end, spline.tilts());

  spline.attributes.foreach_attribute(
      [&](const AttributeIDRef &attribute_id, const AttributeMetaData &UNUSED(meta_data)) {
        std::optional<GMutableSpan> src = spline.attributes.get_for_write(attribute_id);
        BLI_assert(src);
        attribute_math::convert_to_static_type(src->type(), [&](auto dummy) {
          using T = decltype(dummy);
          linear_trim_data<T>(start, end, src->typed<T>());
        });
        return true;
      },
      ATTR_DOMAIN_POINT);

  spline.resize(num);
}

/**
 * Trim NURB splines by converting to a poly spline.
 */
static PolySpline trim_nurbs_spline(const Spline &spline,
                                    const Spline::LookupResult &start_lookup,
                                    const Spline::LookupResult &end_lookup)
{
  /* Since this outputs a poly spline, the evaluated indices are the control point indices. */
  const TrimLocation start = {
      start_lookup.evaluated_index, start_lookup.next_evaluated_index, start_lookup.factor};
  const TrimLocation end = {
      end_lookup.evaluated_index, end_lookup.next_evaluated_index, end_lookup.factor};

  const int num = end.right_index - start.left_index + 1;

  /* Create poly spline and copy trimmed data to it. */
  PolySpline new_spline;
  new_spline.resize(num);

  /* Copy generic attribute data. */
  spline.attributes.foreach_attribute(
      [&](const AttributeIDRef &attribute_id, const AttributeMetaData &meta_data) {
        std::optional<GSpan> src = spline.attributes.get_for_read(attribute_id);
        BLI_assert(src);
        if (!new_spline.attributes.create(attribute_id, meta_data.data_type)) {
          BLI_assert_unreachable();
          return false;
        }
        std::optional<GMutableSpan> dst = new_spline.attributes.get_for_write(attribute_id);
        BLI_assert(dst);

        attribute_math::convert_to_static_type(src->type(), [&](auto dummy) {
          using T = decltype(dummy);
          VArray<T> eval_data = spline.interpolate_to_evaluated<T>(src->typed<T>());
          linear_trim_to_output_data<T>(
              start, end, eval_data.get_internal_span(), dst->typed<T>());
        });
        return true;
      },
      ATTR_DOMAIN_POINT);

  linear_trim_to_output_data<float3>(
      start, end, spline.evaluated_positions(), new_spline.positions());

  VArray<float> evaluated_radii = spline.interpolate_to_evaluated(spline.radii());
  linear_trim_to_output_data<float>(
      start, end, evaluated_radii.get_internal_span(), new_spline.radii());

  VArray<float> evaluated_tilts = spline.interpolate_to_evaluated(spline.tilts());
  linear_trim_to_output_data<float>(
      start, end, evaluated_tilts.get_internal_span(), new_spline.tilts());

  return new_spline;
}

/**
 * Trim Bezier splines by adjusting the first and last handles
 * and control points to maintain the original shape.
 */
static void trim_bezier_spline(Spline &spline,
                               const Spline::LookupResult &start_lookup,
                               const Spline::LookupResult &end_lookup)
{
  BezierSpline &bezier_spline = static_cast<BezierSpline &>(spline);

  const TrimLocation start = lookup_control_point_position(start_lookup, bezier_spline);
  TrimLocation end = lookup_control_point_position(end_lookup, bezier_spline);

  const Span<int> control_offsets = bezier_spline.control_point_offsets();

  /* The number of control points in the resulting spline. */
  const int num = end.right_index - start.left_index + 1;

  /* Trim the spline attributes. Done before end.factor recalculation as it needs
   * the original end.factor value. */
  linear_trim_data<float>(start, end, bezier_spline.radii());
  linear_trim_data<float>(start, end, bezier_spline.tilts());
  spline.attributes.foreach_attribute(
      [&](const AttributeIDRef &attribute_id, const AttributeMetaData &UNUSED(meta_data)) {
        std::optional<GMutableSpan> src = spline.attributes.get_for_write(attribute_id);
        BLI_assert(src);
        attribute_math::convert_to_static_type(src->type(), [&](auto dummy) {
          using T = decltype(dummy);
          linear_trim_data<T>(start, end, src->typed<T>());
        });
        return true;
      },
      ATTR_DOMAIN_POINT);

  /* Recalculate end.factor if the `num` is two, because the adjustment in the
   * position of the control point of the spline to the left of the new end point will change the
   * factor between them. */
  if (num == 2) {
    if (start_lookup.factor == 1.0f) {
      end.factor = 0.0f;
    }
    else {
      end.factor = (end_lookup.evaluated_index + end_lookup.factor -
                    (start_lookup.evaluated_index + start_lookup.factor)) /
                   (control_offsets[end.right_index] -
                    (start_lookup.evaluated_index + start_lookup.factor));
      end.factor = std::clamp(end.factor, 0.0f, 1.0f);
    }
  }

  BezierSpline::InsertResult start_point = bezier_spline.calculate_segment_insertion(
      start.left_index, start.right_index, start.factor);

  /* Update the start control point parameters so they are used calculating the new end point. */
  bezier_spline.positions()[start.left_index] = start_point.position;
  bezier_spline.handle_positions_right()[start.left_index] = start_point.right_handle;
  bezier_spline.handle_positions_left()[start.right_index] = start_point.handle_next;

  const BezierSpline::InsertResult end_point = bezier_spline.calculate_segment_insertion(
      end.left_index, end.right_index, end.factor);

  /* If `num` is two, then the start point right handle needs to change to reflect the end point
   * previous handle update. */
  if (num == 2) {
    start_point.right_handle = end_point.handle_prev;
  }

  /* Shift control point position data to start at beginning of array. */
  if (start.left_index > 0) {
    shift_slice_to_start(bezier_spline.positions(), start.left_index, num);
    shift_slice_to_start(bezier_spline.handle_positions_left(), start.left_index, num);
    shift_slice_to_start(bezier_spline.handle_positions_right(), start.left_index, num);
  }

  bezier_spline.positions().first() = start_point.position;
  bezier_spline.positions()[num - 1] = end_point.position;

  bezier_spline.handle_positions_left().first() = start_point.left_handle;
  bezier_spline.handle_positions_left()[num - 1] = end_point.left_handle;

  bezier_spline.handle_positions_right().first() = start_point.right_handle;
  bezier_spline.handle_positions_right()[num - 1] = end_point.right_handle;

  /* If there is at least one control point between the endpoints, update the control
   * point handle to the right of the start point and to the left of the end point. */
  if (num > 2) {
    bezier_spline.handle_positions_left()[start.right_index - start.left_index] =
        start_point.handle_next;
    bezier_spline.handle_positions_right()[end.left_index - start.left_index] =
        end_point.handle_prev;
  }

  bezier_spline.resize(num);
}

static void trim_spline(SplinePtr &spline,
                        const Spline::LookupResult start,
                        const Spline::LookupResult end)
{
  switch (spline->type()) {
    case CURVE_TYPE_BEZIER:
      trim_bezier_spline(*spline, start, end);
      break;
    case CURVE_TYPE_POLY:
      trim_poly_spline(*spline, start, end);
      break;
    case CURVE_TYPE_NURBS:
      spline = std::make_unique<PolySpline>(trim_nurbs_spline(*spline, start, end));
      break;
    case CURVE_TYPE_CATMULL_ROM:
      BLI_assert_unreachable();
      spline = {};
  }
  spline->mark_cache_invalid();
}

template<typename T>
static void to_single_point_data(const TrimLocation &trim, MutableSpan<T> data)
{
  data.first() = mix2<T>(trim.factor, data[trim.left_index], data[trim.right_index]);
}
template<typename T>
static void to_single_point_data(const TrimLocation &trim, Span<T> src, MutableSpan<T> dst)
{
  dst.first() = mix2<T>(trim.factor, src[trim.left_index], src[trim.right_index]);
}

static void to_single_point_bezier(Spline &spline, const Spline::LookupResult &lookup)
{
  BezierSpline &bezier = static_cast<BezierSpline &>(spline);

  const TrimLocation trim = lookup_control_point_position(lookup, bezier);

  const BezierSpline::InsertResult new_point = bezier.calculate_segment_insertion(
      trim.left_index, trim.right_index, trim.factor);
  bezier.positions().first() = new_point.position;
  bezier.handle_types_left().first() = BEZIER_HANDLE_FREE;
  bezier.handle_types_right().first() = BEZIER_HANDLE_FREE;
  bezier.handle_positions_left().first() = new_point.left_handle;
  bezier.handle_positions_right().first() = new_point.right_handle;

  to_single_point_data<float>(trim, bezier.radii());
  to_single_point_data<float>(trim, bezier.tilts());
  spline.attributes.foreach_attribute(
      [&](const AttributeIDRef &attribute_id, const AttributeMetaData &UNUSED(meta_data)) {
        std::optional<GMutableSpan> data = spline.attributes.get_for_write(attribute_id);
        attribute_math::convert_to_static_type(data->type(), [&](auto dummy) {
          using T = decltype(dummy);
          to_single_point_data<T>(trim, data->typed<T>());
        });
        return true;
      },
      ATTR_DOMAIN_POINT);
  spline.resize(1);
}

static void to_single_point_poly(Spline &spline, const Spline::LookupResult &lookup)
{
  const TrimLocation trim{lookup.evaluated_index, lookup.next_evaluated_index, lookup.factor};

  to_single_point_data<float3>(trim, spline.positions());
  to_single_point_data<float>(trim, spline.radii());
  to_single_point_data<float>(trim, spline.tilts());
  spline.attributes.foreach_attribute(
      [&](const AttributeIDRef &attribute_id, const AttributeMetaData &UNUSED(meta_data)) {
        std::optional<GMutableSpan> data = spline.attributes.get_for_write(attribute_id);
        attribute_math::convert_to_static_type(data->type(), [&](auto dummy) {
          using T = decltype(dummy);
          to_single_point_data<T>(trim, data->typed<T>());
        });
        return true;
      },
      ATTR_DOMAIN_POINT);
  spline.resize(1);
}

static PolySpline to_single_point_nurbs(const Spline &spline, const Spline::LookupResult &lookup)
{
  /* Since this outputs a poly spline, the evaluated indices are the control point indices. */
  const TrimLocation trim{lookup.evaluated_index, lookup.next_evaluated_index, lookup.factor};

  /* Create poly spline and copy trimmed data to it. */
  PolySpline new_spline;
  new_spline.resize(1);

  spline.attributes.foreach_attribute(
      [&](const AttributeIDRef &attribute_id, const AttributeMetaData &meta_data) {
        new_spline.attributes.create(attribute_id, meta_data.data_type);
        std::optional<GSpan> src = spline.attributes.get_for_read(attribute_id);
        std::optional<GMutableSpan> dst = new_spline.attributes.get_for_write(attribute_id);
        attribute_math::convert_to_static_type(src->type(), [&](auto dummy) {
          using T = decltype(dummy);
          VArray<T> eval_data = spline.interpolate_to_evaluated<T>(src->typed<T>());
          to_single_point_data<T>(trim, eval_data.get_internal_span(), dst->typed<T>());
        });
        return true;
      },
      ATTR_DOMAIN_POINT);

  to_single_point_data<float3>(trim, spline.evaluated_positions(), new_spline.positions());

  VArray<float> evaluated_radii = spline.interpolate_to_evaluated(spline.radii());
  to_single_point_data<float>(trim, evaluated_radii.get_internal_span(), new_spline.radii());

  VArray<float> evaluated_tilts = spline.interpolate_to_evaluated(spline.tilts());
  to_single_point_data<float>(trim, evaluated_tilts.get_internal_span(), new_spline.tilts());

  return new_spline;
}

static void to_single_point_spline(SplinePtr &spline, const Spline::LookupResult &lookup)
{
  switch (spline->type()) {
    case CURVE_TYPE_BEZIER:
      to_single_point_bezier(*spline, lookup);
      break;
    case CURVE_TYPE_POLY:
      to_single_point_poly(*spline, lookup);
      break;
    case CURVE_TYPE_NURBS:
      spline = std::make_unique<PolySpline>(to_single_point_nurbs(*spline, lookup));
      break;
    case CURVE_TYPE_CATMULL_ROM:
      BLI_assert_unreachable();
      spline = {};
  }
}

static void geometry_set_curve_trim(GeometrySet &geometry_set,
                                    const GeometryNodeCurveSampleMode mode,
                                    Field<float> &start_field,
                                    Field<float> &end_field)
{
  if (!geometry_set.has_curves()) {
    return;
  }

  CurveComponent &component = geometry_set.get_component_for_write<CurveComponent>();
  GeometryComponentFieldContext field_context{component, ATTR_DOMAIN_CURVE};
  const int domain_size = component.attribute_domain_size(ATTR_DOMAIN_CURVE);

  fn::FieldEvaluator evaluator{field_context, domain_size};
  evaluator.add(start_field);
  evaluator.add(end_field);
  evaluator.evaluate();
  const VArray<float> starts = evaluator.get_evaluated<float>(0);
  const VArray<float> ends = evaluator.get_evaluated<float>(1);

  const Curves &src_curves_id = *geometry_set.get_curves_for_read();
  std::unique_ptr<CurveEval> curve = curves_to_curve_eval(src_curves_id);
  MutableSpan<SplinePtr> splines = curve->splines();

  threading::parallel_for(splines.index_range(), 128, [&](IndexRange range) {
    for (const int i : range) {
      SplinePtr &spline = splines[i];

      /* Currently trimming cyclic splines is not supported. It could be in the future though. */
      if (spline->is_cyclic()) {
        continue;
      }

      if (spline->evaluated_edges_num() == 0) {
        continue;
      }

      const float length = spline->length();
      if (length == 0.0f) {
        continue;
      }

      const float start = starts[i];
      const float end = ends[i];

      /* When the start and end samples are reversed, instead of implicitly reversing the spline
       * or switching the parameters, create a single point spline with the end sample point. */
      if (end <= start) {
        if (mode == GEO_NODE_CURVE_SAMPLE_LENGTH) {
          to_single_point_spline(spline,
                                 spline->lookup_evaluated_length(std::clamp(start, 0.0f, length)));
        }
        else {
          to_single_point_spline(spline,
                                 spline->lookup_evaluated_factor(std::clamp(start, 0.0f, 1.0f)));
        }
        continue;
      }

      if (mode == GEO_NODE_CURVE_SAMPLE_LENGTH) {
        trim_spline(spline,
                    spline->lookup_evaluated_length(std::clamp(start, 0.0f, length)),
                    spline->lookup_evaluated_length(std::clamp(end, 0.0f, length)));
      }
      else {
        trim_spline(spline,
                    spline->lookup_evaluated_factor(std::clamp(start, 0.0f, 1.0f)),
                    spline->lookup_evaluated_factor(std::clamp(end, 0.0f, 1.0f)));
      }
    }
  });

  Curves *dst_curves_id = curve_eval_to_curves(*curve);
  bke::curves_copy_parameters(src_curves_id, *dst_curves_id);
  geometry_set.replace_curves(dst_curves_id);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeGeometryCurveTrim &storage = node_storage(params.node());
  const GeometryNodeCurveSampleMode mode = (GeometryNodeCurveSampleMode)storage.mode;

  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curve");
  GeometryComponentEditData::remember_deformed_curve_positions_if_necessary(geometry_set);

  if (mode == GEO_NODE_CURVE_SAMPLE_FACTOR) {
    Field<float> start_field = params.extract_input<Field<float>>("Start");
    Field<float> end_field = params.extract_input<Field<float>>("End");
    geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
      geometry_set_curve_trim(geometry_set, mode, start_field, end_field);
    });
  }
  else if (mode == GEO_NODE_CURVE_SAMPLE_LENGTH) {
    Field<float> start_field = params.extract_input<Field<float>>("Start_001");
    Field<float> end_field = params.extract_input<Field<float>>("End_001");
    geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
      geometry_set_curve_trim(geometry_set, mode, start_field, end_field);
    });
  }

  params.set_output("Curve", std::move(geometry_set));
}

}  // namespace blender::nodes::node_geo_curve_trim_cc

void register_node_type_geo_curve_trim()
{
  namespace file_ns = blender::nodes::node_geo_curve_trim_cc;

  static bNodeType ntype;
  geo_node_type_base(&ntype, GEO_NODE_TRIM_CURVE, "Trim Curve", NODE_CLASS_GEOMETRY);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.declare = file_ns::node_declare;
  node_type_storage(
      &ntype, "NodeGeometryCurveTrim", node_free_standard_storage, node_copy_standard_storage);
  node_type_init(&ntype, file_ns::node_init);
  node_type_update(&ntype, file_ns::node_update);
  ntype.gather_link_search_ops = file_ns::node_gather_link_searches;
  nodeRegisterType(&ntype);
}
