/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_curve_types.h"

#include "BKE_anonymous_attribute.hh"
#include "BKE_curve.h"
#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_spline.hh"

using blender::Array;
using blender::float3;
using blender::float4x4;
using blender::GVArray;
using blender::GVArraySpan;
using blender::IndexRange;
using blender::Map;
using blender::MutableSpan;
using blender::Span;
using blender::StringRefNull;
using blender::VArray;
using blender::VArraySpan;
using blender::Vector;
using blender::bke::AttributeIDRef;
using blender::bke::AttributeMetaData;

blender::Span<SplinePtr> CurveEval::splines() const
{
  return splines_;
}

blender::MutableSpan<SplinePtr> CurveEval::splines()
{
  return splines_;
}

bool CurveEval::has_spline_with_type(const CurveType type) const
{
  for (const SplinePtr &spline : this->splines()) {
    if (spline->type() == type) {
      return true;
    }
  }
  return false;
}

void CurveEval::resize(const int size)
{
  splines_.resize(size);
  attributes.reallocate(size);
}

void CurveEval::add_spline(SplinePtr spline)
{
  splines_.append(std::move(spline));
}

void CurveEval::add_splines(MutableSpan<SplinePtr> splines)
{
  for (SplinePtr &spline : splines) {
    this->add_spline(std::move(spline));
  }
}

void CurveEval::remove_splines(blender::IndexMask mask)
{
  for (int i = mask.size() - 1; i >= 0; i--) {
    splines_.remove_and_reorder(mask.indices()[i]);
  }
}

void CurveEval::translate(const float3 &translation)
{
  for (SplinePtr &spline : this->splines()) {
    spline->translate(translation);
    spline->mark_cache_invalid();
  }
}

void CurveEval::transform(const float4x4 &matrix)
{
  for (SplinePtr &spline : this->splines()) {
    spline->transform(matrix);
  }
}

bool CurveEval::bounds_min_max(float3 &min, float3 &max, const bool use_evaluated) const
{
  bool have_minmax = false;
  for (const SplinePtr &spline : this->splines()) {
    if (spline->size()) {
      spline->bounds_min_max(min, max, use_evaluated);
      have_minmax = true;
    }
  }

  return have_minmax;
}

float CurveEval::total_length() const
{
  float length = 0.0f;
  for (const SplinePtr &spline : this->splines()) {
    length += spline->length();
  }
  return length;
}

int CurveEval::total_control_point_num() const
{
  int count = 0;
  for (const SplinePtr &spline : this->splines()) {
    count += spline->size();
  }
  return count;
}

blender::Array<int> CurveEval::control_point_offsets() const
{
  Array<int> offsets(splines_.size() + 1);
  int offset = 0;
  for (const int i : splines_.index_range()) {
    offsets[i] = offset;
    offset += splines_[i]->size();
  }
  offsets.last() = offset;
  return offsets;
}

blender::Array<int> CurveEval::evaluated_point_offsets() const
{
  Array<int> offsets(splines_.size() + 1);
  int offset = 0;
  for (const int i : splines_.index_range()) {
    offsets[i] = offset;
    offset += splines_[i]->evaluated_points_num();
  }
  offsets.last() = offset;
  return offsets;
}

blender::Array<float> CurveEval::accumulated_spline_lengths() const
{
  Array<float> spline_lengths(splines_.size() + 1);
  float spline_length = 0.0f;
  for (const int i : splines_.index_range()) {
    spline_lengths[i] = spline_length;
    spline_length += splines_[i]->length();
  }
  spline_lengths.last() = spline_length;
  return spline_lengths;
}

void CurveEval::mark_cache_invalid()
{
  for (SplinePtr &spline : splines_) {
    spline->mark_cache_invalid();
  }
}

static HandleType handle_type_from_dna_bezt(const eBezTriple_Handle dna_handle_type)
{
  switch (dna_handle_type) {
    case HD_FREE:
      return BEZIER_HANDLE_FREE;
    case HD_AUTO:
      return BEZIER_HANDLE_AUTO;
    case HD_VECT:
      return BEZIER_HANDLE_VECTOR;
    case HD_ALIGN:
      return BEZIER_HANDLE_ALIGN;
    case HD_AUTO_ANIM:
      return BEZIER_HANDLE_AUTO;
    case HD_ALIGN_DOUBLESIDE:
      return BEZIER_HANDLE_ALIGN;
  }
  BLI_assert_unreachable();
  return BEZIER_HANDLE_AUTO;
}

static NormalMode normal_mode_from_dna_curve(const int twist_mode)
{
  switch (twist_mode) {
    case CU_TWIST_Z_UP:
    case CU_TWIST_TANGENT:
      return NORMAL_MODE_Z_UP;
    case CU_TWIST_MINIMUM:
      return NORMAL_MODE_MINIMUM_TWIST;
  }
  BLI_assert_unreachable();
  return NORMAL_MODE_MINIMUM_TWIST;
}

static KnotsMode knots_mode_from_dna_nurb(const short flag)
{
  switch (flag & (CU_NURB_ENDPOINT | CU_NURB_BEZIER)) {
    case CU_NURB_ENDPOINT:
      return NURBS_KNOT_MODE_ENDPOINT;
    case CU_NURB_BEZIER:
      return NURBS_KNOT_MODE_BEZIER;
    case CU_NURB_ENDPOINT | CU_NURB_BEZIER:
      return NURBS_KNOT_MODE_ENDPOINT_BEZIER;
    default:
      return NURBS_KNOT_MODE_NORMAL;
  }

  BLI_assert_unreachable();
  return NURBS_KNOT_MODE_NORMAL;
}

static SplinePtr spline_from_dna_bezier(const Nurb &nurb)
{
  std::unique_ptr<BezierSpline> spline = std::make_unique<BezierSpline>();
  spline->set_resolution(nurb.resolu);
  spline->set_cyclic(nurb.flagu & CU_NURB_CYCLIC);

  Span<const BezTriple> src_points{nurb.bezt, nurb.pntsu};
  spline->resize(src_points.size());
  MutableSpan<float3> positions = spline->positions();
  MutableSpan<float3> handle_positions_left = spline->handle_positions_left(true);
  MutableSpan<float3> handle_positions_right = spline->handle_positions_right(true);
  MutableSpan<int8_t> handle_types_left = spline->handle_types_left();
  MutableSpan<int8_t> handle_types_right = spline->handle_types_right();
  MutableSpan<float> radii = spline->radii();
  MutableSpan<float> tilts = spline->tilts();

  blender::threading::parallel_for(src_points.index_range(), 2048, [&](IndexRange range) {
    for (const int i : range) {
      const BezTriple &bezt = src_points[i];
      positions[i] = bezt.vec[1];
      handle_positions_left[i] = bezt.vec[0];
      handle_types_left[i] = handle_type_from_dna_bezt((eBezTriple_Handle)bezt.h1);
      handle_positions_right[i] = bezt.vec[2];
      handle_types_right[i] = handle_type_from_dna_bezt((eBezTriple_Handle)bezt.h2);
      radii[i] = bezt.radius;
      tilts[i] = bezt.tilt;
    }
  });

  return spline;
}

static SplinePtr spline_from_dna_nurbs(const Nurb &nurb)
{
  std::unique_ptr<NURBSpline> spline = std::make_unique<NURBSpline>();
  spline->set_resolution(nurb.resolu);
  spline->set_cyclic(nurb.flagu & CU_NURB_CYCLIC);
  spline->set_order(nurb.orderu);
  spline->knots_mode = knots_mode_from_dna_nurb(nurb.flagu);

  Span<const BPoint> src_points{nurb.bp, nurb.pntsu};
  spline->resize(src_points.size());
  MutableSpan<float3> positions = spline->positions();
  MutableSpan<float> weights = spline->weights();
  MutableSpan<float> radii = spline->radii();
  MutableSpan<float> tilts = spline->tilts();

  blender::threading::parallel_for(src_points.index_range(), 2048, [&](IndexRange range) {
    for (const int i : range) {
      const BPoint &bp = src_points[i];
      positions[i] = bp.vec;
      weights[i] = bp.vec[3];
      radii[i] = bp.radius;
      tilts[i] = bp.tilt;
    }
  });

  return spline;
}

static SplinePtr spline_from_dna_poly(const Nurb &nurb)
{
  std::unique_ptr<PolySpline> spline = std::make_unique<PolySpline>();
  spline->set_cyclic(nurb.flagu & CU_NURB_CYCLIC);

  Span<const BPoint> src_points{nurb.bp, nurb.pntsu};
  spline->resize(src_points.size());
  MutableSpan<float3> positions = spline->positions();
  MutableSpan<float> radii = spline->radii();
  MutableSpan<float> tilts = spline->tilts();

  blender::threading::parallel_for(src_points.index_range(), 2048, [&](IndexRange range) {
    for (const int i : range) {
      const BPoint &bp = src_points[i];
      positions[i] = bp.vec;
      radii[i] = bp.radius;
      tilts[i] = bp.tilt;
    }
  });

  return spline;
}

std::unique_ptr<CurveEval> curve_eval_from_dna_curve(const Curve &dna_curve,
                                                     const ListBase &nurbs_list)
{
  Vector<const Nurb *> nurbs(nurbs_list);

  std::unique_ptr<CurveEval> curve = std::make_unique<CurveEval>();
  curve->resize(nurbs.size());
  MutableSpan<SplinePtr> splines = curve->splines();

  blender::threading::parallel_for(nurbs.index_range(), 256, [&](IndexRange range) {
    for (const int i : range) {
      switch (nurbs[i]->type) {
        case CU_BEZIER:
          splines[i] = spline_from_dna_bezier(*nurbs[i]);
          break;
        case CU_NURBS:
          splines[i] = spline_from_dna_nurbs(*nurbs[i]);
          break;
        case CU_POLY:
          splines[i] = spline_from_dna_poly(*nurbs[i]);
          break;
        default:
          BLI_assert_unreachable();
          break;
      }
    }
  });

  /* Normal mode is stored separately in each spline to facilitate combining
   * splines from multiple curve objects, where the value may be different. */
  const NormalMode normal_mode = normal_mode_from_dna_curve(dna_curve.twist_mode);
  for (SplinePtr &spline : curve->splines()) {
    spline->normal_mode = normal_mode;
  }

  return curve;
}

std::unique_ptr<CurveEval> curve_eval_from_dna_curve(const Curve &dna_curve)
{
  return curve_eval_from_dna_curve(dna_curve, *BKE_curve_nurbs_get_for_read(&dna_curve));
}

static void copy_attributes_between_components(
    const blender::bke::AttributeAccessor &src_attributes,
    blender::bke::MutableAttributeAccessor &dst_attributes,
    Span<std::string> skip)
{
  src_attributes.for_all([&](const AttributeIDRef &id, const AttributeMetaData meta_data) {
    if (id.is_named() && skip.contains(id.name())) {
      return true;
    }

    GVArray src_attribute = src_attributes.lookup(id, meta_data.domain, meta_data.data_type);
    if (!src_attribute) {
      return true;
    }
    GVArraySpan src_attribute_data{src_attribute};

    blender::bke::GAttributeWriter dst_attribute = dst_attributes.lookup_or_add_for_write(
        id, meta_data.domain, meta_data.data_type);
    if (!dst_attribute) {
      return true;
    }
    dst_attribute.varray.set_all(src_attribute_data.data());
    dst_attribute.finish();
    return true;
  });
}

std::unique_ptr<CurveEval> curves_to_curve_eval(const Curves &curves_id)
{
  CurveComponent src_component;
  src_component.replace(&const_cast<Curves &>(curves_id), GeometryOwnershipType::ReadOnly);
  const blender::bke::CurvesGeometry &curves = blender::bke::CurvesGeometry::wrap(
      curves_id.geometry);
  const blender::bke::AttributeAccessor src_attributes = curves.attributes();

  VArray<int> resolution = curves.resolution();
  VArray<int8_t> normal_mode = curves.normal_mode();

  VArraySpan<float> nurbs_weights{
      src_attributes.lookup_or_default<float>("nurbs_weight", ATTR_DOMAIN_POINT, 1.0f)};
  VArraySpan<int8_t> nurbs_orders{
      src_attributes.lookup_or_default<int8_t>("nurbs_order", ATTR_DOMAIN_CURVE, 4)};
  VArraySpan<int8_t> nurbs_knots_modes{
      src_attributes.lookup_or_default<int8_t>("knots_mode", ATTR_DOMAIN_CURVE, 0)};

  VArraySpan<int8_t> handle_types_right{
      src_attributes.lookup_or_default<int8_t>("handle_type_right", ATTR_DOMAIN_POINT, 0)};
  VArraySpan<int8_t> handle_types_left{
      src_attributes.lookup_or_default<int8_t>("handle_type_left", ATTR_DOMAIN_POINT, 0)};

  /* Create splines with the correct size and type. */
  VArray<int8_t> curve_types = curves.curve_types();
  std::unique_ptr<CurveEval> curve_eval = std::make_unique<CurveEval>();
  for (const int curve_index : curve_types.index_range()) {
    const IndexRange points = curves.points_for_curve(curve_index);

    std::unique_ptr<Spline> spline;
    /* #CurveEval does not support catmull rom curves, so convert those to poly splines. */
    switch (std::max<int8_t>(1, curve_types[curve_index])) {
      case CURVE_TYPE_POLY: {
        spline = std::make_unique<PolySpline>();
        spline->resize(points.size());
        break;
      }
      case CURVE_TYPE_BEZIER: {
        std::unique_ptr<BezierSpline> bezier_spline = std::make_unique<BezierSpline>();
        bezier_spline->resize(points.size());
        bezier_spline->set_resolution(resolution[curve_index]);
        bezier_spline->handle_types_left().copy_from(handle_types_left.slice(points));
        bezier_spline->handle_types_right().copy_from(handle_types_right.slice(points));

        spline = std::move(bezier_spline);
        break;
      }
      case CURVE_TYPE_NURBS: {
        std::unique_ptr<NURBSpline> nurb_spline = std::make_unique<NURBSpline>();
        nurb_spline->resize(points.size());
        nurb_spline->set_resolution(resolution[curve_index]);
        nurb_spline->weights().copy_from(nurbs_weights.slice(points));
        nurb_spline->set_order(nurbs_orders[curve_index]);
        nurb_spline->knots_mode = static_cast<KnotsMode>(nurbs_knots_modes[curve_index]);

        spline = std::move(nurb_spline);
        break;
      }
      case CURVE_TYPE_CATMULL_ROM:
        /* Not supported yet. */
        BLI_assert_unreachable();
        continue;
    }
    spline->positions().fill(float3(0));
    spline->tilts().fill(0.0f);
    spline->radii().fill(1.0f);
    spline->normal_mode = static_cast<NormalMode>(normal_mode[curve_index]);
    curve_eval->add_spline(std::move(spline));
  }

  curve_eval->attributes.reallocate(curve_eval->splines().size());

  CurveComponentLegacy dst_component;
  dst_component.replace(curve_eval.get(), GeometryOwnershipType::Editable);
  blender::bke::MutableAttributeAccessor dst_attributes = *dst_component.attributes_for_write();

  copy_attributes_between_components(src_attributes,
                                     dst_attributes,
                                     {"curve_type",
                                      "resolution",
                                      "normal_mode",
                                      "nurbs_weight",
                                      "nurbs_order",
                                      "knots_mode",
                                      "handle_type_right",
                                      "handle_type_left"});

  return curve_eval;
}

Curves *curve_eval_to_curves(const CurveEval &curve_eval)
{
  Curves *curves_id = blender::bke::curves_new_nomain(curve_eval.total_control_point_num(),
                                                      curve_eval.splines().size());
  CurveComponent dst_component;
  dst_component.replace(curves_id, GeometryOwnershipType::Editable);
  blender::bke::MutableAttributeAccessor dst_attributes = *dst_component.attributes_for_write();

  blender::bke::CurvesGeometry &curves = blender::bke::CurvesGeometry::wrap(curves_id->geometry);
  curves.offsets_for_write().copy_from(curve_eval.control_point_offsets());
  MutableSpan<int8_t> curve_types = curves.curve_types_for_write();

  blender::bke::SpanAttributeWriter<int8_t> normal_mode =
      dst_attributes.lookup_or_add_for_write_only_span<int8_t>("normal_mode", ATTR_DOMAIN_CURVE);
  blender::bke::SpanAttributeWriter<float> nurbs_weight;
  blender::bke::SpanAttributeWriter<int8_t> nurbs_order;
  blender::bke::SpanAttributeWriter<int8_t> nurbs_knots_mode;
  if (curve_eval.has_spline_with_type(CURVE_TYPE_NURBS)) {
    nurbs_weight = dst_attributes.lookup_or_add_for_write_only_span<float>("nurbs_weight",
                                                                           ATTR_DOMAIN_POINT);
    nurbs_order = dst_attributes.lookup_or_add_for_write_only_span<int8_t>("nurbs_order",
                                                                           ATTR_DOMAIN_CURVE);
    nurbs_knots_mode = dst_attributes.lookup_or_add_for_write_only_span<int8_t>("knots_mode",
                                                                                ATTR_DOMAIN_CURVE);
  }
  blender::bke::SpanAttributeWriter<int8_t> handle_type_right;
  blender::bke::SpanAttributeWriter<int8_t> handle_type_left;
  if (curve_eval.has_spline_with_type(CURVE_TYPE_BEZIER)) {
    handle_type_right = dst_attributes.lookup_or_add_for_write_only_span<int8_t>(
        "handle_type_right", ATTR_DOMAIN_POINT);
    handle_type_left = dst_attributes.lookup_or_add_for_write_only_span<int8_t>("handle_type_left",
                                                                                ATTR_DOMAIN_POINT);
  }

  for (const int curve_index : curve_eval.splines().index_range()) {
    const Spline &spline = *curve_eval.splines()[curve_index];
    curve_types[curve_index] = curve_eval.splines()[curve_index]->type();
    normal_mode.span[curve_index] = curve_eval.splines()[curve_index]->normal_mode;
    const IndexRange points = curves.points_for_curve(curve_index);

    switch (spline.type()) {
      case CURVE_TYPE_POLY:
        break;
      case CURVE_TYPE_BEZIER: {
        const BezierSpline &src = static_cast<const BezierSpline &>(spline);
        handle_type_right.span.slice(points).copy_from(src.handle_types_right());
        handle_type_left.span.slice(points).copy_from(src.handle_types_left());
        break;
      }
      case CURVE_TYPE_NURBS: {
        const NURBSpline &src = static_cast<const NURBSpline &>(spline);
        nurbs_knots_mode.span[curve_index] = static_cast<int8_t>(src.knots_mode);
        nurbs_order.span[curve_index] = src.order();
        nurbs_weight.span.slice(points).copy_from(src.weights());
        break;
      }
      case CURVE_TYPE_CATMULL_ROM: {
        BLI_assert_unreachable();
        break;
      }
    }
  }

  curves.update_curve_types();

  normal_mode.finish();
  nurbs_weight.finish();
  nurbs_order.finish();
  nurbs_knots_mode.finish();
  handle_type_right.finish();
  handle_type_left.finish();

  CurveComponentLegacy src_component;
  src_component.replace(&const_cast<CurveEval &>(curve_eval), GeometryOwnershipType::ReadOnly);
  const blender::bke::AttributeAccessor src_attributes = *src_component.attributes();

  copy_attributes_between_components(src_attributes, dst_attributes, {});

  return curves_id;
}

void CurveEval::assert_valid_point_attributes() const
{
#ifdef DEBUG
  if (splines_.size() == 0) {
    return;
  }
  const int layer_len = splines_.first()->attributes.data.totlayer;

  Array<AttributeIDRef> ids_in_order(layer_len);
  Array<AttributeMetaData> meta_data_in_order(layer_len);

  {
    int i = 0;
    splines_.first()->attributes.foreach_attribute(
        [&](const AttributeIDRef &attribute_id, const AttributeMetaData &meta_data) {
          ids_in_order[i] = attribute_id;
          meta_data_in_order[i] = meta_data;
          i++;
          return true;
        },
        ATTR_DOMAIN_POINT);
  }

  for (const SplinePtr &spline : splines_) {
    /* All splines should have the same number of attributes. */
    BLI_assert(spline->attributes.data.totlayer == layer_len);

    int i = 0;
    spline->attributes.foreach_attribute(
        [&](const AttributeIDRef &attribute_id, const AttributeMetaData &meta_data) {
          /* Attribute names and IDs should have the same order and exist on all splines. */
          BLI_assert(attribute_id == ids_in_order[i]);

          /* Attributes with the same ID different splines should all have the same type. */
          BLI_assert(meta_data == meta_data_in_order[i]);

          i++;
          return true;
        },
        ATTR_DOMAIN_POINT);
  }

#endif
}
