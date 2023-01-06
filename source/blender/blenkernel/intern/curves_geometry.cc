/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <mutex>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_array_utils.hh"
#include "BLI_bounds.hh"
#include "BLI_index_mask_ops.hh"
#include "BLI_length_parameterize.hh"
#include "BLI_math_rotation_legacy.hh"
#include "BLI_task.hh"

#include "DNA_curves_types.h"

#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"

namespace blender::bke {

static const std::string ATTR_POSITION = "position";
static const std::string ATTR_RADIUS = "radius";
static const std::string ATTR_TILT = "tilt";
static const std::string ATTR_CURVE_TYPE = "curve_type";
static const std::string ATTR_CYCLIC = "cyclic";
static const std::string ATTR_RESOLUTION = "resolution";
static const std::string ATTR_NORMAL_MODE = "normal_mode";
static const std::string ATTR_HANDLE_TYPE_LEFT = "handle_type_left";
static const std::string ATTR_HANDLE_TYPE_RIGHT = "handle_type_right";
static const std::string ATTR_HANDLE_POSITION_LEFT = "handle_left";
static const std::string ATTR_HANDLE_POSITION_RIGHT = "handle_right";
static const std::string ATTR_NURBS_ORDER = "nurbs_order";
static const std::string ATTR_NURBS_WEIGHT = "nurbs_weight";
static const std::string ATTR_NURBS_KNOTS_MODE = "knots_mode";
static const std::string ATTR_SURFACE_UV_COORDINATE = "surface_uv_coordinate";

/* -------------------------------------------------------------------- */
/** \name Constructors/Destructor
 * \{ */

CurvesGeometry::CurvesGeometry() : CurvesGeometry(0, 0)
{
}

CurvesGeometry::CurvesGeometry(const int point_num, const int curve_num)
{
  this->point_num = point_num;
  this->curve_num = curve_num;
  CustomData_reset(&this->point_data);
  CustomData_reset(&this->curve_data);

  CustomData_add_layer_named(&this->point_data,
                             CD_PROP_FLOAT3,
                             CD_CONSTRUCT,
                             nullptr,
                             this->point_num,
                             ATTR_POSITION.c_str());

  this->curve_offsets = (int *)MEM_malloc_arrayN(this->curve_num + 1, sizeof(int), __func__);
#ifdef DEBUG
  this->offsets_for_write().fill(-1);
#endif
  this->offsets_for_write().first() = 0;

  this->runtime = MEM_new<CurvesGeometryRuntime>(__func__);
  /* Fill the type counts with the default so they're in a valid state. */
  this->runtime->type_counts[CURVE_TYPE_CATMULL_ROM] = curve_num;
}

/**
 * \note Expects `dst` to be initialized, since the original attributes must be freed.
 */
static void copy_curves_geometry(CurvesGeometry &dst, const CurvesGeometry &src)
{
  CustomData_free(&dst.point_data, dst.point_num);
  CustomData_free(&dst.curve_data, dst.curve_num);
  dst.point_num = src.point_num;
  dst.curve_num = src.curve_num;
  CustomData_copy(&src.point_data, &dst.point_data, CD_MASK_ALL, CD_DUPLICATE, dst.point_num);
  CustomData_copy(&src.curve_data, &dst.curve_data, CD_MASK_ALL, CD_DUPLICATE, dst.curve_num);

  MEM_SAFE_FREE(dst.curve_offsets);
  dst.curve_offsets = (int *)MEM_malloc_arrayN(dst.point_num + 1, sizeof(int), __func__);
  dst.offsets_for_write().copy_from(src.offsets());

  dst.tag_topology_changed();

  /* Though type counts are a cache, they must be copied because they are calculated eagerly. */
  dst.runtime->type_counts = src.runtime->type_counts;
  dst.runtime->bounds_cache = src.runtime->bounds_cache;
}

CurvesGeometry::CurvesGeometry(const CurvesGeometry &other)
    : CurvesGeometry(other.point_num, other.curve_num)
{
  copy_curves_geometry(*this, other);
}

CurvesGeometry &CurvesGeometry::operator=(const CurvesGeometry &other)
{
  if (this != &other) {
    copy_curves_geometry(*this, other);
  }
  return *this;
}

/* The source should be empty, but in a valid state so that using it further will work. */
static void move_curves_geometry(CurvesGeometry &dst, CurvesGeometry &src)
{
  dst.point_num = src.point_num;
  std::swap(dst.point_data, src.point_data);
  CustomData_free(&src.point_data, src.point_num);
  src.point_num = 0;

  dst.curve_num = src.curve_num;
  std::swap(dst.curve_data, src.curve_data);
  CustomData_free(&src.curve_data, src.curve_num);
  src.curve_num = 0;

  std::swap(dst.curve_offsets, src.curve_offsets);
  MEM_SAFE_FREE(src.curve_offsets);

  std::swap(dst.runtime, src.runtime);
}

CurvesGeometry::CurvesGeometry(CurvesGeometry &&other)
    : CurvesGeometry(other.point_num, other.curve_num)
{
  move_curves_geometry(*this, other);
}

CurvesGeometry &CurvesGeometry::operator=(CurvesGeometry &&other)
{
  if (this != &other) {
    move_curves_geometry(*this, other);
  }
  return *this;
}

CurvesGeometry::~CurvesGeometry()
{
  CustomData_free(&this->point_data, this->point_num);
  CustomData_free(&this->curve_data, this->curve_num);
  MEM_SAFE_FREE(this->curve_offsets);
  MEM_delete(this->runtime);
  this->runtime = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Accessors
 * \{ */

static int domain_num(const CurvesGeometry &curves, const eAttrDomain domain)
{
  return domain == ATTR_DOMAIN_POINT ? curves.points_num() : curves.curves_num();
}

static CustomData &domain_custom_data(CurvesGeometry &curves, const eAttrDomain domain)
{
  return domain == ATTR_DOMAIN_POINT ? curves.point_data : curves.curve_data;
}

static const CustomData &domain_custom_data(const CurvesGeometry &curves, const eAttrDomain domain)
{
  return domain == ATTR_DOMAIN_POINT ? curves.point_data : curves.curve_data;
}

template<typename T>
static VArray<T> get_varray_attribute(const CurvesGeometry &curves,
                                      const eAttrDomain domain,
                                      const StringRefNull name,
                                      const T default_value)
{
  const int num = domain_num(curves, domain);
  const eCustomDataType type = cpp_type_to_custom_data_type(CPPType::get<T>());
  const CustomData &custom_data = domain_custom_data(curves, domain);

  const T *data = (const T *)CustomData_get_layer_named(&custom_data, type, name.c_str());
  if (data != nullptr) {
    return VArray<T>::ForSpan(Span<T>(data, num));
  }
  return VArray<T>::ForSingle(default_value, num);
}

template<typename T>
static Span<T> get_span_attribute(const CurvesGeometry &curves,
                                  const eAttrDomain domain,
                                  const StringRefNull name)
{
  const int num = domain_num(curves, domain);
  const CustomData &custom_data = domain_custom_data(curves, domain);
  const eCustomDataType type = cpp_type_to_custom_data_type(CPPType::get<T>());

  T *data = (T *)CustomData_get_layer_named(&custom_data, type, name.c_str());
  if (data == nullptr) {
    return {};
  }
  return {data, num};
}

template<typename T>
static MutableSpan<T> get_mutable_attribute(CurvesGeometry &curves,
                                            const eAttrDomain domain,
                                            const StringRefNull name,
                                            const T default_value = T())
{
  const int num = domain_num(curves, domain);
  const eCustomDataType type = cpp_type_to_custom_data_type(CPPType::get<T>());
  CustomData &custom_data = domain_custom_data(curves, domain);

  T *data = (T *)CustomData_duplicate_referenced_layer_named(
      &custom_data, type, name.c_str(), num);
  if (data != nullptr) {
    return {data, num};
  }
  data = (T *)CustomData_add_layer_named(
      &custom_data, type, CD_SET_DEFAULT, nullptr, num, name.c_str());
  MutableSpan<T> span = {data, num};
  if (num > 0 && span.first() != default_value) {
    span.fill(default_value);
  }
  return span;
}

VArray<int8_t> CurvesGeometry::curve_types() const
{
  return get_varray_attribute<int8_t>(
      *this, ATTR_DOMAIN_CURVE, ATTR_CURVE_TYPE, CURVE_TYPE_CATMULL_ROM);
}

MutableSpan<int8_t> CurvesGeometry::curve_types_for_write()
{
  return get_mutable_attribute<int8_t>(*this, ATTR_DOMAIN_CURVE, ATTR_CURVE_TYPE);
}

void CurvesGeometry::fill_curve_types(const CurveType type)
{
  this->curve_types_for_write().fill(type);
  this->runtime->type_counts.fill(0);
  this->runtime->type_counts[type] = this->curves_num();
  this->tag_topology_changed();
}

void CurvesGeometry::fill_curve_types(const IndexMask selection, const CurveType type)
{
  if (selection.size() == this->curves_num()) {
    this->fill_curve_types(type);
    return;
  }
  if (std::optional<int8_t> single_type = this->curve_types().get_if_single()) {
    if (single_type == type) {
      /* No need for an array if the types are already a single with the correct type. */
      return;
    }
  }
  /* A potential performance optimization is only counting the changed indices. */
  this->curve_types_for_write().fill_indices(selection, type);
  this->update_curve_types();
  this->tag_topology_changed();
}

std::array<int, CURVE_TYPES_NUM> calculate_type_counts(const VArray<int8_t> &types)
{
  using CountsType = std::array<int, CURVE_TYPES_NUM>;
  CountsType counts;
  counts.fill(0);

  if (types.is_single()) {
    counts[types.get_internal_single()] = types.size();
    return counts;
  }

  Span<int8_t> types_span = types.get_internal_span();
  return threading::parallel_reduce(
      types.index_range(),
      2048,
      counts,
      [&](const IndexRange curves_range, const CountsType &init) {
        CountsType result = init;
        for (const int curve_index : curves_range) {
          result[types_span[curve_index]]++;
        }
        return result;
      },
      [](const CountsType &a, const CountsType &b) {
        CountsType result = a;
        for (const int i : IndexRange(CURVE_TYPES_NUM)) {
          result[i] += b[i];
        }
        return result;
      });
}

void CurvesGeometry::update_curve_types()
{
  this->runtime->type_counts = calculate_type_counts(this->curve_types());
}

Span<float3> CurvesGeometry::positions() const
{
  return get_span_attribute<float3>(*this, ATTR_DOMAIN_POINT, ATTR_POSITION);
}
MutableSpan<float3> CurvesGeometry::positions_for_write()
{
  return get_mutable_attribute<float3>(*this, ATTR_DOMAIN_POINT, ATTR_POSITION);
}

Span<int> CurvesGeometry::offsets() const
{
  return {this->curve_offsets, this->curve_num + 1};
}
MutableSpan<int> CurvesGeometry::offsets_for_write()
{
  return {this->curve_offsets, this->curve_num + 1};
}

VArray<bool> CurvesGeometry::cyclic() const
{
  return get_varray_attribute<bool>(*this, ATTR_DOMAIN_CURVE, ATTR_CYCLIC, false);
}
MutableSpan<bool> CurvesGeometry::cyclic_for_write()
{
  return get_mutable_attribute<bool>(*this, ATTR_DOMAIN_CURVE, ATTR_CYCLIC, false);
}

VArray<int> CurvesGeometry::resolution() const
{
  return get_varray_attribute<int>(*this, ATTR_DOMAIN_CURVE, ATTR_RESOLUTION, 12);
}
MutableSpan<int> CurvesGeometry::resolution_for_write()
{
  return get_mutable_attribute<int>(*this, ATTR_DOMAIN_CURVE, ATTR_RESOLUTION, 12);
}

VArray<int8_t> CurvesGeometry::normal_mode() const
{
  return get_varray_attribute<int8_t>(*this, ATTR_DOMAIN_CURVE, ATTR_NORMAL_MODE, 0);
}
MutableSpan<int8_t> CurvesGeometry::normal_mode_for_write()
{
  return get_mutable_attribute<int8_t>(*this, ATTR_DOMAIN_CURVE, ATTR_NORMAL_MODE);
}

VArray<float> CurvesGeometry::tilt() const
{
  return get_varray_attribute<float>(*this, ATTR_DOMAIN_POINT, ATTR_TILT, 0.0f);
}
MutableSpan<float> CurvesGeometry::tilt_for_write()
{
  return get_mutable_attribute<float>(*this, ATTR_DOMAIN_POINT, ATTR_TILT);
}

VArray<int8_t> CurvesGeometry::handle_types_left() const
{
  return get_varray_attribute<int8_t>(*this, ATTR_DOMAIN_POINT, ATTR_HANDLE_TYPE_LEFT, 0);
}
MutableSpan<int8_t> CurvesGeometry::handle_types_left_for_write()
{
  return get_mutable_attribute<int8_t>(*this, ATTR_DOMAIN_POINT, ATTR_HANDLE_TYPE_LEFT, 0);
}

VArray<int8_t> CurvesGeometry::handle_types_right() const
{
  return get_varray_attribute<int8_t>(*this, ATTR_DOMAIN_POINT, ATTR_HANDLE_TYPE_RIGHT, 0);
}
MutableSpan<int8_t> CurvesGeometry::handle_types_right_for_write()
{
  return get_mutable_attribute<int8_t>(*this, ATTR_DOMAIN_POINT, ATTR_HANDLE_TYPE_RIGHT, 0);
}

Span<float3> CurvesGeometry::handle_positions_left() const
{
  return get_span_attribute<float3>(*this, ATTR_DOMAIN_POINT, ATTR_HANDLE_POSITION_LEFT);
}
MutableSpan<float3> CurvesGeometry::handle_positions_left_for_write()
{
  return get_mutable_attribute<float3>(*this, ATTR_DOMAIN_POINT, ATTR_HANDLE_POSITION_LEFT);
}

Span<float3> CurvesGeometry::handle_positions_right() const
{
  return get_span_attribute<float3>(*this, ATTR_DOMAIN_POINT, ATTR_HANDLE_POSITION_RIGHT);
}
MutableSpan<float3> CurvesGeometry::handle_positions_right_for_write()
{
  return get_mutable_attribute<float3>(*this, ATTR_DOMAIN_POINT, ATTR_HANDLE_POSITION_RIGHT);
}

VArray<int8_t> CurvesGeometry::nurbs_orders() const
{
  return get_varray_attribute<int8_t>(*this, ATTR_DOMAIN_CURVE, ATTR_NURBS_ORDER, 4);
}
MutableSpan<int8_t> CurvesGeometry::nurbs_orders_for_write()
{
  return get_mutable_attribute<int8_t>(*this, ATTR_DOMAIN_CURVE, ATTR_NURBS_ORDER, 4);
}

Span<float> CurvesGeometry::nurbs_weights() const
{
  return get_span_attribute<float>(*this, ATTR_DOMAIN_POINT, ATTR_NURBS_WEIGHT);
}
MutableSpan<float> CurvesGeometry::nurbs_weights_for_write()
{
  return get_mutable_attribute<float>(*this, ATTR_DOMAIN_POINT, ATTR_NURBS_WEIGHT);
}

VArray<int8_t> CurvesGeometry::nurbs_knots_modes() const
{
  return get_varray_attribute<int8_t>(*this, ATTR_DOMAIN_CURVE, ATTR_NURBS_KNOTS_MODE, 0);
}
MutableSpan<int8_t> CurvesGeometry::nurbs_knots_modes_for_write()
{
  return get_mutable_attribute<int8_t>(*this, ATTR_DOMAIN_CURVE, ATTR_NURBS_KNOTS_MODE, 0);
}

Span<float2> CurvesGeometry::surface_uv_coords() const
{
  return get_span_attribute<float2>(*this, ATTR_DOMAIN_CURVE, ATTR_SURFACE_UV_COORDINATE);
}

MutableSpan<float2> CurvesGeometry::surface_uv_coords_for_write()
{
  return get_mutable_attribute<float2>(*this, ATTR_DOMAIN_CURVE, ATTR_SURFACE_UV_COORDINATE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Evaluation
 * \{ */

template<typename CountFn> void build_offsets(MutableSpan<int> offsets, const CountFn &count_fn)
{
  int offset = 0;
  for (const int i : offsets.drop_back(1).index_range()) {
    offsets[i] = offset;
    offset += count_fn(i);
  }
  offsets.last() = offset;
}

static void calculate_evaluated_offsets(const CurvesGeometry &curves,
                                        MutableSpan<int> offsets,
                                        MutableSpan<int> bezier_evaluated_offsets)
{
  VArray<int8_t> types = curves.curve_types();
  VArray<int> resolution = curves.resolution();
  VArray<bool> cyclic = curves.cyclic();

  VArraySpan<int8_t> handle_types_left{curves.handle_types_left()};
  VArraySpan<int8_t> handle_types_right{curves.handle_types_right()};

  VArray<int8_t> nurbs_orders = curves.nurbs_orders();
  VArray<int8_t> nurbs_knots_modes = curves.nurbs_knots_modes();

  build_offsets(offsets, [&](const int curve_index) -> int {
    const IndexRange points = curves.points_for_curve(curve_index);
    switch (types[curve_index]) {
      case CURVE_TYPE_CATMULL_ROM:
        return curves::catmull_rom::calculate_evaluated_num(
            points.size(), cyclic[curve_index], resolution[curve_index]);
      case CURVE_TYPE_POLY:
        return points.size();
      case CURVE_TYPE_BEZIER:
        curves::bezier::calculate_evaluated_offsets(handle_types_left.slice(points),
                                                    handle_types_right.slice(points),
                                                    cyclic[curve_index],
                                                    resolution[curve_index],
                                                    bezier_evaluated_offsets.slice(points));
        return bezier_evaluated_offsets[points.last()];
      case CURVE_TYPE_NURBS:
        return curves::nurbs::calculate_evaluated_num(points.size(),
                                                      nurbs_orders[curve_index],
                                                      cyclic[curve_index],
                                                      resolution[curve_index],
                                                      KnotsMode(nurbs_knots_modes[curve_index]));
    }
    BLI_assert_unreachable();
    return 0;
  });
}

void CurvesGeometry::ensure_evaluated_offsets() const
{
  this->runtime->offsets_cache_mutex.ensure([&]() {
    this->runtime->evaluated_offsets_cache.resize(this->curves_num() + 1);

    if (this->has_curve_with_type(CURVE_TYPE_BEZIER)) {
      this->runtime->bezier_evaluated_offsets.resize(this->points_num());
    }
    else {
      this->runtime->bezier_evaluated_offsets.clear_and_shrink();
    }

    calculate_evaluated_offsets(
        *this, this->runtime->evaluated_offsets_cache, this->runtime->bezier_evaluated_offsets);
  });
}

Span<int> CurvesGeometry::evaluated_offsets() const
{
  this->ensure_evaluated_offsets();
  return this->runtime->evaluated_offsets_cache;
}

IndexMask CurvesGeometry::indices_for_curve_type(const CurveType type,
                                                 Vector<int64_t> &r_indices) const
{
  return this->indices_for_curve_type(type, this->curves_range(), r_indices);
}

IndexMask CurvesGeometry::indices_for_curve_type(const CurveType type,
                                                 const IndexMask selection,
                                                 Vector<int64_t> &r_indices) const
{
  return curves::indices_for_type(
      this->curve_types(), this->curve_type_counts(), type, selection, r_indices);
}

Array<int> CurvesGeometry::point_to_curve_map() const
{
  Array<int> map(this->points_num());
  for (const int i : this->curves_range()) {
    map.as_mutable_span().slice(this->points_for_curve(i)).fill(i);
  }
  return map;
}

void CurvesGeometry::ensure_nurbs_basis_cache() const
{
  this->runtime->nurbs_basis_cache_mutex.ensure([&]() {
    Vector<int64_t> nurbs_indices;
    const IndexMask nurbs_mask = this->indices_for_curve_type(CURVE_TYPE_NURBS, nurbs_indices);
    if (nurbs_mask.is_empty()) {
      return;
    }

    this->runtime->nurbs_basis_cache.resize(this->curves_num());
    MutableSpan<curves::nurbs::BasisCache> basis_caches(this->runtime->nurbs_basis_cache);

    VArray<bool> cyclic = this->cyclic();
    VArray<int8_t> orders = this->nurbs_orders();
    VArray<int8_t> knots_modes = this->nurbs_knots_modes();

    threading::parallel_for(nurbs_mask.index_range(), 64, [&](const IndexRange range) {
      for (const int curve_index : nurbs_mask.slice(range)) {
        const IndexRange points = this->points_for_curve(curve_index);
        const IndexRange evaluated_points = this->evaluated_points_for_curve(curve_index);

        const int8_t order = orders[curve_index];
        const bool is_cyclic = cyclic[curve_index];
        const KnotsMode mode = KnotsMode(knots_modes[curve_index]);

        if (!curves::nurbs::check_valid_num_and_order(points.size(), order, is_cyclic, mode)) {
          basis_caches[curve_index].invalid = true;
          continue;
        }

        const int knots_num = curves::nurbs::knots_num(points.size(), order, is_cyclic);
        Array<float> knots(knots_num);
        curves::nurbs::calculate_knots(points.size(), mode, order, is_cyclic, knots);
        curves::nurbs::calculate_basis_cache(points.size(),
                                             evaluated_points.size(),
                                             order,
                                             is_cyclic,
                                             knots,
                                             basis_caches[curve_index]);
      }
    });
  });
}

Span<float3> CurvesGeometry::evaluated_positions() const
{
  this->runtime->position_cache_mutex.ensure([&]() {
    if (this->is_single_type(CURVE_TYPE_POLY)) {
      this->runtime->evaluated_positions_span = this->positions();
      this->runtime->evaluated_position_cache.clear_and_shrink();
      return;
    }

    this->runtime->evaluated_position_cache.resize(this->evaluated_points_num());
    MutableSpan<float3> evaluated_positions = this->runtime->evaluated_position_cache;
    this->runtime->evaluated_positions_span = evaluated_positions;

    VArray<int8_t> types = this->curve_types();
    VArray<bool> cyclic = this->cyclic();
    VArray<int> resolution = this->resolution();
    Span<float3> positions = this->positions();

    Span<float3> handle_positions_left = this->handle_positions_left();
    Span<float3> handle_positions_right = this->handle_positions_right();
    Span<int> bezier_evaluated_offsets = this->runtime->bezier_evaluated_offsets;

    VArray<int8_t> nurbs_orders = this->nurbs_orders();
    Span<float> nurbs_weights = this->nurbs_weights();

    this->ensure_nurbs_basis_cache();

    threading::parallel_for(this->curves_range(), 128, [&](IndexRange curves_range) {
      for (const int curve_index : curves_range) {
        const IndexRange points = this->points_for_curve(curve_index);
        const IndexRange evaluated_points = this->evaluated_points_for_curve(curve_index);

        switch (types[curve_index]) {
          case CURVE_TYPE_CATMULL_ROM:
            curves::catmull_rom::interpolate_to_evaluated(
                positions.slice(points),
                cyclic[curve_index],
                resolution[curve_index],
                evaluated_positions.slice(evaluated_points));
            break;
          case CURVE_TYPE_POLY:
            evaluated_positions.slice(evaluated_points).copy_from(positions.slice(points));
            break;
          case CURVE_TYPE_BEZIER:
            curves::bezier::calculate_evaluated_positions(
                positions.slice(points),
                handle_positions_left.slice(points),
                handle_positions_right.slice(points),
                bezier_evaluated_offsets.slice(points),
                evaluated_positions.slice(evaluated_points));
            break;
          case CURVE_TYPE_NURBS: {
            curves::nurbs::interpolate_to_evaluated(this->runtime->nurbs_basis_cache[curve_index],
                                                    nurbs_orders[curve_index],
                                                    nurbs_weights.slice_safe(points),
                                                    positions.slice(points),
                                                    evaluated_positions.slice(evaluated_points));
            break;
          }
          default:
            BLI_assert_unreachable();
            break;
        }
      }
    });
  });
  return this->runtime->evaluated_positions_span;
}

Span<float3> CurvesGeometry::evaluated_tangents() const
{
  this->runtime->tangent_cache_mutex.ensure([&]() {
    const Span<float3> evaluated_positions = this->evaluated_positions();
    const VArray<bool> cyclic = this->cyclic();

    this->runtime->evaluated_tangent_cache.resize(this->evaluated_points_num());
    MutableSpan<float3> tangents = this->runtime->evaluated_tangent_cache;

    threading::parallel_for(this->curves_range(), 128, [&](IndexRange curves_range) {
      for (const int curve_index : curves_range) {
        const IndexRange evaluated_points = this->evaluated_points_for_curve(curve_index);
        curves::poly::calculate_tangents(evaluated_positions.slice(evaluated_points),
                                         cyclic[curve_index],
                                         tangents.slice(evaluated_points));
      }
    });

    /* Correct the first and last tangents of non-cyclic Bezier curves so that they align with
     * the inner handles. This is a separate loop to avoid the cost when Bezier type curves are
     * not used. */
    Vector<int64_t> bezier_indices;
    const IndexMask bezier_mask = this->indices_for_curve_type(CURVE_TYPE_BEZIER, bezier_indices);
    if (!bezier_mask.is_empty()) {
      const Span<float3> positions = this->positions();
      const Span<float3> handles_left = this->handle_positions_left();
      const Span<float3> handles_right = this->handle_positions_right();

      threading::parallel_for(bezier_mask.index_range(), 1024, [&](IndexRange range) {
        for (const int curve_index : bezier_mask.slice(range)) {
          if (cyclic[curve_index]) {
            continue;
          }
          const IndexRange points = this->points_for_curve(curve_index);
          const IndexRange evaluated_points = this->evaluated_points_for_curve(curve_index);

          const float epsilon = 1e-6f;
          if (!math::almost_equal_relative(
                  handles_right[points.first()], positions[points.first()], epsilon)) {
            tangents[evaluated_points.first()] = math::normalize(handles_right[points.first()] -
                                                                 positions[points.first()]);
          }
          if (!math::almost_equal_relative(
                  handles_left[points.last()], positions[points.last()], epsilon)) {
            tangents[evaluated_points.last()] = math::normalize(positions[points.last()] -
                                                                handles_left[points.last()]);
          }
        }
      });
    }
  });
  return this->runtime->evaluated_tangent_cache;
}

static void rotate_directions_around_axes(MutableSpan<float3> directions,
                                          const Span<float3> axes,
                                          const Span<float> angles)
{
  for (const int i : directions.index_range()) {
    directions[i] = math::rotate_direction_around_axis(directions[i], axes[i], angles[i]);
  }
}

Span<float3> CurvesGeometry::evaluated_normals() const
{
  this->runtime->normal_cache_mutex.ensure([&]() {
    const Span<float3> evaluated_tangents = this->evaluated_tangents();
    const VArray<bool> cyclic = this->cyclic();
    const VArray<int8_t> normal_mode = this->normal_mode();
    const VArray<int8_t> types = this->curve_types();
    const VArray<float> tilt = this->tilt();

    this->runtime->evaluated_normal_cache.resize(this->evaluated_points_num());
    MutableSpan<float3> evaluated_normals = this->runtime->evaluated_normal_cache;

    threading::parallel_for(this->curves_range(), 128, [&](IndexRange curves_range) {
      /* Reuse a buffer for the evaluated tilts. */
      Vector<float> evaluated_tilts;

      for (const int curve_index : curves_range) {
        const IndexRange evaluated_points = this->evaluated_points_for_curve(curve_index);
        switch (normal_mode[curve_index]) {
          case NORMAL_MODE_Z_UP:
            curves::poly::calculate_normals_z_up(evaluated_tangents.slice(evaluated_points),
                                                 evaluated_normals.slice(evaluated_points));
            break;
          case NORMAL_MODE_MINIMUM_TWIST:
            curves::poly::calculate_normals_minimum(evaluated_tangents.slice(evaluated_points),
                                                    cyclic[curve_index],
                                                    evaluated_normals.slice(evaluated_points));
            break;
        }

        /* If the "tilt" attribute exists, rotate the normals around the tangents by the
         * evaluated angles. We can avoid copying the tilts to evaluate them for poly curves. */
        if (!(tilt.is_single() && tilt.get_internal_single() == 0.0f)) {
          const IndexRange points = this->points_for_curve(curve_index);
          Span<float> curve_tilt = tilt.get_internal_span().slice(points);
          if (types[curve_index] == CURVE_TYPE_POLY) {
            rotate_directions_around_axes(evaluated_normals.slice(evaluated_points),
                                          evaluated_tangents.slice(evaluated_points),
                                          curve_tilt);
          }
          else {
            evaluated_tilts.clear();
            evaluated_tilts.resize(evaluated_points.size());
            this->interpolate_to_evaluated(
                curve_index, curve_tilt, evaluated_tilts.as_mutable_span());
            rotate_directions_around_axes(evaluated_normals.slice(evaluated_points),
                                          evaluated_tangents.slice(evaluated_points),
                                          evaluated_tilts.as_span());
          }
        }
      }
    });
  });
  return this->runtime->evaluated_normal_cache;
}

void CurvesGeometry::interpolate_to_evaluated(const int curve_index,
                                              const GSpan src,
                                              GMutableSpan dst) const
{
  BLI_assert(this->runtime->offsets_cache_mutex.is_cached());
  BLI_assert(this->runtime->nurbs_basis_cache_mutex.is_cached());
  const IndexRange points = this->points_for_curve(curve_index);
  BLI_assert(src.size() == points.size());
  BLI_assert(dst.size() == this->evaluated_points_for_curve(curve_index).size());
  switch (this->curve_types()[curve_index]) {
    case CURVE_TYPE_CATMULL_ROM:
      curves::catmull_rom::interpolate_to_evaluated(
          src, this->cyclic()[curve_index], this->resolution()[curve_index], dst);
      return;
    case CURVE_TYPE_POLY:
      dst.type().copy_assign_n(src.data(), dst.data(), src.size());
      return;
    case CURVE_TYPE_BEZIER:
      curves::bezier::interpolate_to_evaluated(
          src, this->runtime->bezier_evaluated_offsets.as_span().slice(points), dst);
      return;
    case CURVE_TYPE_NURBS:
      curves::nurbs::interpolate_to_evaluated(this->runtime->nurbs_basis_cache[curve_index],
                                              this->nurbs_orders()[curve_index],
                                              this->nurbs_weights().slice_safe(points),
                                              src,
                                              dst);
      return;
  }
  BLI_assert_unreachable();
}

void CurvesGeometry::interpolate_to_evaluated(const GSpan src, GMutableSpan dst) const
{
  BLI_assert(this->runtime->offsets_cache_mutex.is_cached());
  BLI_assert(this->runtime->nurbs_basis_cache_mutex.is_cached());
  const VArray<int8_t> types = this->curve_types();
  const VArray<int> resolution = this->resolution();
  const VArray<bool> cyclic = this->cyclic();
  const VArray<int8_t> nurbs_orders = this->nurbs_orders();
  const Span<float> nurbs_weights = this->nurbs_weights();

  threading::parallel_for(this->curves_range(), 512, [&](IndexRange curves_range) {
    for (const int curve_index : curves_range) {
      const IndexRange points = this->points_for_curve(curve_index);
      const IndexRange evaluated_points = this->evaluated_points_for_curve(curve_index);
      switch (types[curve_index]) {
        case CURVE_TYPE_CATMULL_ROM:
          curves::catmull_rom::interpolate_to_evaluated(src.slice(points),
                                                        cyclic[curve_index],
                                                        resolution[curve_index],
                                                        dst.slice(evaluated_points));
          continue;
        case CURVE_TYPE_POLY:
          dst.slice(evaluated_points).copy_from(src.slice(points));
          continue;
        case CURVE_TYPE_BEZIER:
          curves::bezier::interpolate_to_evaluated(
              src.slice(points),
              this->runtime->bezier_evaluated_offsets.as_span().slice(points),
              dst.slice(evaluated_points));
          continue;
        case CURVE_TYPE_NURBS:
          curves::nurbs::interpolate_to_evaluated(this->runtime->nurbs_basis_cache[curve_index],
                                                  nurbs_orders[curve_index],
                                                  nurbs_weights.slice_safe(points),
                                                  src.slice(points),
                                                  dst.slice(evaluated_points));
          continue;
      }
    }
  });
}

void CurvesGeometry::ensure_evaluated_lengths() const
{
  this->runtime->length_cache_mutex.ensure([&]() {
    /* Use an extra length value for the final cyclic segment for a consistent size
     * (see comment on #evaluated_length_cache). */
    const int total_num = this->evaluated_points_num() + this->curves_num();
    this->runtime->evaluated_length_cache.resize(total_num);
    MutableSpan<float> evaluated_lengths = this->runtime->evaluated_length_cache;

    Span<float3> evaluated_positions = this->evaluated_positions();
    VArray<bool> curves_cyclic = this->cyclic();

    threading::parallel_for(this->curves_range(), 128, [&](IndexRange curves_range) {
      for (const int curve_index : curves_range) {
        const bool cyclic = curves_cyclic[curve_index];
        const IndexRange evaluated_points = this->evaluated_points_for_curve(curve_index);
        const IndexRange lengths_range = this->lengths_range_for_curve(curve_index, cyclic);
        length_parameterize::accumulate_lengths(evaluated_positions.slice(evaluated_points),
                                                cyclic,
                                                evaluated_lengths.slice(lengths_range));
      }
    });
  });
}

void CurvesGeometry::ensure_can_interpolate_to_evaluated() const
{
  this->ensure_evaluated_offsets();
  this->ensure_nurbs_basis_cache();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operations
 * \{ */

void CurvesGeometry::resize(const int points_num, const int curves_num)
{
  if (points_num != this->point_num) {
    CustomData_realloc(&this->point_data, this->points_num(), points_num);
    this->point_num = points_num;
  }
  if (curves_num != this->curve_num) {
    CustomData_realloc(&this->curve_data, this->curves_num(), curves_num);
    this->curve_num = curves_num;
    this->curve_offsets = (int *)MEM_reallocN(this->curve_offsets, sizeof(int) * (curves_num + 1));
  }
  this->tag_topology_changed();
}

void CurvesGeometry::tag_positions_changed()
{
  this->runtime->position_cache_mutex.tag_dirty();
  this->runtime->tangent_cache_mutex.tag_dirty();
  this->runtime->normal_cache_mutex.tag_dirty();
  this->runtime->length_cache_mutex.tag_dirty();
  this->runtime->bounds_cache.tag_dirty();
}
void CurvesGeometry::tag_topology_changed()
{
  this->tag_positions_changed();
  this->runtime->offsets_cache_mutex.tag_dirty();
  this->runtime->nurbs_basis_cache_mutex.tag_dirty();
}
void CurvesGeometry::tag_normals_changed()
{
  this->runtime->normal_cache_mutex.tag_dirty();
}
void CurvesGeometry::tag_radii_changed()
{
  this->runtime->bounds_cache.tag_dirty();
}

static void translate_positions(MutableSpan<float3> positions, const float3 &translation)
{
  threading::parallel_for(positions.index_range(), 2048, [&](const IndexRange range) {
    for (float3 &position : positions.slice(range)) {
      position += translation;
    }
  });
}

static void transform_positions(MutableSpan<float3> positions, const float4x4 &matrix)
{
  threading::parallel_for(positions.index_range(), 1024, [&](const IndexRange range) {
    for (float3 &position : positions.slice(range)) {
      position = matrix * position;
    }
  });
}

void CurvesGeometry::calculate_bezier_auto_handles()
{
  if (!this->has_curve_with_type(CURVE_TYPE_BEZIER)) {
    return;
  }
  if (this->handle_positions_left().is_empty() || this->handle_positions_right().is_empty()) {
    return;
  }
  const VArray<int8_t> types = this->curve_types();
  const VArray<bool> cyclic = this->cyclic();
  const VArraySpan<int8_t> types_left{this->handle_types_left()};
  const VArraySpan<int8_t> types_right{this->handle_types_right()};
  const Span<float3> positions = this->positions();
  MutableSpan<float3> positions_left = this->handle_positions_left_for_write();
  MutableSpan<float3> positions_right = this->handle_positions_right_for_write();

  threading::parallel_for(this->curves_range(), 128, [&](IndexRange range) {
    for (const int i_curve : range) {
      if (types[i_curve] == CURVE_TYPE_BEZIER) {
        const IndexRange points = this->points_for_curve(i_curve);
        curves::bezier::calculate_auto_handles(cyclic[i_curve],
                                               types_left.slice(points),
                                               types_right.slice(points),
                                               positions.slice(points),
                                               positions_left.slice(points),
                                               positions_right.slice(points));
      }
    }
  });
}

void CurvesGeometry::translate(const float3 &translation)
{
  translate_positions(this->positions_for_write(), translation);
  if (!this->handle_positions_left().is_empty()) {
    translate_positions(this->handle_positions_left_for_write(), translation);
  }
  if (!this->handle_positions_right().is_empty()) {
    translate_positions(this->handle_positions_right_for_write(), translation);
  }
  this->tag_positions_changed();
}

void CurvesGeometry::transform(const float4x4 &matrix)
{
  transform_positions(this->positions_for_write(), matrix);
  if (!this->handle_positions_left().is_empty()) {
    transform_positions(this->handle_positions_left_for_write(), matrix);
  }
  if (!this->handle_positions_right().is_empty()) {
    transform_positions(this->handle_positions_right_for_write(), matrix);
  }
  this->tag_positions_changed();
}

bool CurvesGeometry::bounds_min_max(float3 &min, float3 &max) const
{
  if (this->points_num() == 0) {
    return false;
  }

  this->runtime->bounds_cache.ensure([&](Bounds<float3> &r_bounds) {
    const Span<float3> positions = this->evaluated_positions();
    if (this->attributes().contains("radius")) {
      const VArraySpan<float> radii = this->attributes().lookup<float>("radius");
      Array<float> evaluated_radii(this->evaluated_points_num());
      this->ensure_can_interpolate_to_evaluated();
      this->interpolate_to_evaluated(radii, evaluated_radii.as_mutable_span());
      r_bounds = *bounds::min_max_with_radii(positions, evaluated_radii.as_span());
    }
    else {
      r_bounds = *bounds::min_max(positions);
    }
  });

  const Bounds<float3> &bounds = this->runtime->bounds_cache.data();
  min = math::min(bounds.min, min);
  max = math::max(bounds.max, max);
  return true;
}

static void copy_between_buffers(const CPPType &type,
                                 const void *src_buffer,
                                 void *dst_buffer,
                                 const IndexRange src_range,
                                 const IndexRange dst_range)
{
  BLI_assert(src_range.size() == dst_range.size());
  type.copy_construct_n(POINTER_OFFSET(src_buffer, type.size() * src_range.start()),
                        POINTER_OFFSET(dst_buffer, type.size() * dst_range.start()),
                        src_range.size());
}

static void copy_with_map(const GSpan src, const Span<int> map, GMutableSpan dst)
{
  attribute_math::convert_to_static_type(src.type(), [&](auto dummy) {
    using T = decltype(dummy);
    array_utils::gather(src.typed<T>(), map, dst.typed<T>());
  });
}

/**
 * Builds an array that for every point, contains the corresponding curve index.
 */
static Array<int> build_point_to_curve_map(const CurvesGeometry &curves)
{
  Array<int> point_to_curve_map(curves.points_num());
  threading::parallel_for(curves.curves_range(), 1024, [&](const IndexRange curves_range) {
    for (const int i_curve : curves_range) {
      point_to_curve_map.as_mutable_span().slice(curves.points_for_curve(i_curve)).fill(i_curve);
    }
  });
  return point_to_curve_map;
}

static CurvesGeometry copy_with_removed_points(
    const CurvesGeometry &curves,
    const IndexMask points_to_delete,
    const AnonymousAttributePropagationInfo &propagation_info)
{
  /* Use a map from points to curves to facilitate using an #IndexMask input. */
  const Array<int> point_to_curve_map = build_point_to_curve_map(curves);

  const Vector<IndexRange> copy_point_ranges = points_to_delete.extract_ranges_invert(
      curves.points_range());

  /* For every range of points to copy, find the offset in the result curves point layers. */
  int new_point_count = 0;
  Array<int> copy_point_range_dst_offsets(copy_point_ranges.size());
  for (const int i : copy_point_ranges.index_range()) {
    copy_point_range_dst_offsets[i] = new_point_count;
    new_point_count += copy_point_ranges[i].size();
  }
  BLI_assert(new_point_count == (curves.points_num() - points_to_delete.size()));

  /* Find out how many non-deleted points there are in every curve. */
  Array<int> curve_point_counts(curves.curves_num(), 0);
  for (const IndexRange range : copy_point_ranges) {
    for (const int point_i : range) {
      curve_point_counts[point_to_curve_map[point_i]]++;
    }
  }

  /* Build the offsets for the new curve points, skipping curves that had all points deleted.
   * Also store the original indices of the corresponding input curves, to facilitate parallel
   * copying of curve domain data. */
  int new_curve_count = 0;
  int curve_point_offset = 0;
  Vector<int> new_curve_offsets;
  Vector<int> new_curve_orig_indices;
  new_curve_offsets.append(0);
  for (const int i : curve_point_counts.index_range()) {
    if (curve_point_counts[i] > 0) {
      curve_point_offset += curve_point_counts[i];
      new_curve_offsets.append(curve_point_offset);

      new_curve_count++;
      new_curve_orig_indices.append(i);
    }
  }

  CurvesGeometry new_curves{new_point_count, new_curve_count};
  Vector<bke::AttributeTransferData> point_attributes = bke::retrieve_attributes_for_transfer(
      curves.attributes(),
      new_curves.attributes_for_write(),
      ATTR_DOMAIN_MASK_POINT,
      propagation_info);
  Vector<bke::AttributeTransferData> curve_attributes = bke::retrieve_attributes_for_transfer(
      curves.attributes(),
      new_curves.attributes_for_write(),
      ATTR_DOMAIN_MASK_CURVE,
      propagation_info);

  threading::parallel_invoke(
      256 < new_point_count * new_curve_count,
      /* Initialize curve offsets. */
      [&]() { new_curves.offsets_for_write().copy_from(new_curve_offsets); },
      [&]() {
        /* Copy over point attributes. */
        for (bke::AttributeTransferData &attribute : point_attributes) {
          threading::parallel_for(copy_point_ranges.index_range(), 128, [&](IndexRange range) {
            for (const int range_i : range) {
              const IndexRange src_range = copy_point_ranges[range_i];
              copy_between_buffers(attribute.src.type(),
                                   attribute.src.data(),
                                   attribute.dst.span.data(),
                                   src_range,
                                   {copy_point_range_dst_offsets[range_i], src_range.size()});
            }
          });
        }
      },
      [&]() {
        /* Copy over curve attributes.
         * In some cases points are just dissolved, so the number of
         * curves will be the same. That could be optimized in the future. */
        for (bke::AttributeTransferData &attribute : curve_attributes) {
          if (new_curves.curves_num() == curves.curves_num()) {
            attribute.dst.span.copy_from(attribute.src);
          }
          else {
            copy_with_map(attribute.src, new_curve_orig_indices, attribute.dst.span);
          }
        }
      });

  for (bke::AttributeTransferData &attribute : point_attributes) {
    attribute.dst.finish();
  }
  for (bke::AttributeTransferData &attribute : curve_attributes) {
    attribute.dst.finish();
  }

  if (new_curves.curves_num() != curves.curves_num()) {
    new_curves.remove_attributes_based_on_types();
  }

  return new_curves;
}

void CurvesGeometry::remove_points(const IndexMask points_to_delete,
                                   const AnonymousAttributePropagationInfo &propagation_info)
{
  if (points_to_delete.is_empty()) {
    return;
  }
  if (points_to_delete.size() == this->points_num()) {
    *this = {};
  }
  *this = copy_with_removed_points(*this, points_to_delete, propagation_info);
}

static CurvesGeometry copy_with_removed_curves(
    const CurvesGeometry &curves,
    const IndexMask curves_to_delete,
    const AnonymousAttributePropagationInfo &propagation_info)
{
  const Span<int> old_offsets = curves.offsets();
  const Vector<IndexRange> old_curve_ranges = curves_to_delete.extract_ranges_invert(
      curves.curves_range(), nullptr);
  Vector<IndexRange> new_curve_ranges;
  Vector<IndexRange> old_point_ranges;
  Vector<IndexRange> new_point_ranges;
  int new_tot_points = 0;
  int new_tot_curves = 0;
  for (const IndexRange &curve_range : old_curve_ranges) {
    new_curve_ranges.append(IndexRange(new_tot_curves, curve_range.size()));
    new_tot_curves += curve_range.size();

    const IndexRange old_point_range = curves.points_for_curves(curve_range);
    old_point_ranges.append(old_point_range);
    new_point_ranges.append(IndexRange(new_tot_points, old_point_range.size()));
    new_tot_points += old_point_range.size();
  }

  CurvesGeometry new_curves{new_tot_points, new_tot_curves};
  Vector<bke::AttributeTransferData> point_attributes = bke::retrieve_attributes_for_transfer(
      curves.attributes(),
      new_curves.attributes_for_write(),
      ATTR_DOMAIN_MASK_POINT,
      propagation_info);
  Vector<bke::AttributeTransferData> curve_attributes = bke::retrieve_attributes_for_transfer(
      curves.attributes(),
      new_curves.attributes_for_write(),
      ATTR_DOMAIN_MASK_CURVE,
      propagation_info);

  threading::parallel_invoke(
      256 < new_tot_points * new_tot_curves,
      /* Initialize curve offsets. */
      [&]() {
        MutableSpan<int> new_offsets = new_curves.offsets_for_write();
        new_offsets.last() = new_tot_points;
        threading::parallel_for(
            old_curve_ranges.index_range(), 128, [&](const IndexRange ranges_range) {
              for (const int range_i : ranges_range) {
                const IndexRange old_curve_range = old_curve_ranges[range_i];
                const IndexRange new_curve_range = new_curve_ranges[range_i];
                const IndexRange old_point_range = old_point_ranges[range_i];
                const IndexRange new_point_range = new_point_ranges[range_i];
                const int offset_shift = new_point_range.start() - old_point_range.start();
                const int curves_in_range = old_curve_range.size();
                threading::parallel_for(
                    IndexRange(curves_in_range), 512, [&](const IndexRange range) {
                      for (const int i : range) {
                        const int old_curve_i = old_curve_range[i];
                        const int new_curve_i = new_curve_range[i];
                        const int old_offset = old_offsets[old_curve_i];
                        const int new_offset = old_offset + offset_shift;
                        new_offsets[new_curve_i] = new_offset;
                      }
                    });
              }
            });
      },
      [&]() {
        /* Copy over point attributes. */
        for (bke::AttributeTransferData &attribute : point_attributes) {
          threading::parallel_for(old_curve_ranges.index_range(), 128, [&](IndexRange range) {
            for (const int range_i : range) {
              copy_between_buffers(attribute.src.type(),
                                   attribute.src.data(),
                                   attribute.dst.span.data(),
                                   old_point_ranges[range_i],
                                   new_point_ranges[range_i]);
            }
          });
        }
      },
      [&]() {
        /* Copy over curve attributes. */
        for (bke::AttributeTransferData &attribute : curve_attributes) {
          threading::parallel_for(old_curve_ranges.index_range(), 128, [&](IndexRange range) {
            for (const int range_i : range) {
              copy_between_buffers(attribute.src.type(),
                                   attribute.src.data(),
                                   attribute.dst.span.data(),
                                   old_curve_ranges[range_i],
                                   new_curve_ranges[range_i]);
            }
          });
        }
      });

  for (bke::AttributeTransferData &attribute : point_attributes) {
    attribute.dst.finish();
  }
  for (bke::AttributeTransferData &attribute : curve_attributes) {
    attribute.dst.finish();
  }

  new_curves.remove_attributes_based_on_types();

  return new_curves;
}

void CurvesGeometry::remove_curves(const IndexMask curves_to_delete,
                                   const AnonymousAttributePropagationInfo &propagation_info)
{
  if (curves_to_delete.is_empty()) {
    return;
  }
  if (curves_to_delete.size() == this->curves_num()) {
    *this = {};
    return;
  }
  *this = copy_with_removed_curves(*this, curves_to_delete, propagation_info);
}

template<typename T>
static void reverse_curve_point_data(const CurvesGeometry &curves,
                                     const IndexMask curve_selection,
                                     MutableSpan<T> data)
{
  threading::parallel_for(curve_selection.index_range(), 256, [&](IndexRange range) {
    for (const int curve_i : curve_selection.slice(range)) {
      data.slice(curves.points_for_curve(curve_i)).reverse();
    }
  });
}

template<typename T>
static void reverse_swap_curve_point_data(const CurvesGeometry &curves,
                                          const IndexMask curve_selection,
                                          MutableSpan<T> data_a,
                                          MutableSpan<T> data_b)
{
  threading::parallel_for(curve_selection.index_range(), 256, [&](IndexRange range) {
    for (const int curve_i : curve_selection.slice(range)) {
      const IndexRange points = curves.points_for_curve(curve_i);
      MutableSpan<T> a = data_a.slice(points);
      MutableSpan<T> b = data_b.slice(points);
      for (const int i : IndexRange(points.size() / 2)) {
        const int end_index = points.size() - 1 - i;
        std::swap(a[end_index], b[i]);
        std::swap(b[end_index], a[i]);
      }
      if (points.size() % 2) {
        const int64_t middle_index = points.size() / 2;
        std::swap(a[middle_index], b[middle_index]);
      }
    }
  });
}

void CurvesGeometry::reverse_curves(const IndexMask curves_to_reverse)
{
  Set<StringRef> bezier_handle_names{{ATTR_HANDLE_POSITION_LEFT,
                                      ATTR_HANDLE_POSITION_RIGHT,
                                      ATTR_HANDLE_TYPE_LEFT,
                                      ATTR_HANDLE_TYPE_RIGHT}};

  MutableAttributeAccessor attributes = this->attributes_for_write();

  attributes.for_all([&](const AttributeIDRef &id, AttributeMetaData meta_data) {
    if (meta_data.domain != ATTR_DOMAIN_POINT) {
      return true;
    }
    if (meta_data.data_type == CD_PROP_STRING) {
      return true;
    }
    if (bezier_handle_names.contains(id.name())) {
      return true;
    }

    GSpanAttributeWriter attribute = attributes.lookup_for_write_span(id);
    attribute_math::convert_to_static_type(attribute.span.type(), [&](auto dummy) {
      using T = decltype(dummy);
      reverse_curve_point_data<T>(*this, curves_to_reverse, attribute.span.typed<T>());
    });
    attribute.finish();
    return true;
  });

  /* In order to maintain the shape of Bezier curves, handle attributes must reverse, but also the
   * values for the left and right must swap. Use a utility to swap and reverse at the same time,
   * to avoid loading the attribute twice. Generally we can expect the right layer to exist when
   * the left does, but there's no need to count on it, so check for both attributes. */

  if (attributes.contains(ATTR_HANDLE_POSITION_LEFT) &&
      attributes.contains(ATTR_HANDLE_POSITION_RIGHT)) {
    reverse_swap_curve_point_data(*this,
                                  curves_to_reverse,
                                  this->handle_positions_left_for_write(),
                                  this->handle_positions_right_for_write());
  }
  if (attributes.contains(ATTR_HANDLE_TYPE_LEFT) && attributes.contains(ATTR_HANDLE_TYPE_RIGHT)) {
    reverse_swap_curve_point_data(*this,
                                  curves_to_reverse,
                                  this->handle_types_left_for_write(),
                                  this->handle_types_right_for_write());
  }

  this->tag_topology_changed();
}

void CurvesGeometry::remove_attributes_based_on_types()
{
  MutableAttributeAccessor attributes = this->attributes_for_write();
  if (!this->has_curve_with_type(CURVE_TYPE_BEZIER)) {
    attributes.remove(ATTR_HANDLE_TYPE_LEFT);
    attributes.remove(ATTR_HANDLE_TYPE_RIGHT);
    attributes.remove(ATTR_HANDLE_POSITION_LEFT);
    attributes.remove(ATTR_HANDLE_POSITION_RIGHT);
  }
  if (!this->has_curve_with_type(CURVE_TYPE_NURBS)) {
    attributes.remove(ATTR_NURBS_WEIGHT);
    attributes.remove(ATTR_NURBS_ORDER);
    attributes.remove(ATTR_NURBS_KNOTS_MODE);
  }
  if (!this->has_curve_with_type({CURVE_TYPE_BEZIER, CURVE_TYPE_CATMULL_ROM, CURVE_TYPE_NURBS})) {
    attributes.remove(ATTR_RESOLUTION);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Domain Interpolation
 * \{ */

/**
 * Mix together all of a curve's control point values.
 *
 * \note Theoretically this interpolation does not need to compute all values at once.
 * However, doing that makes the implementation simpler, and this can be optimized in the future if
 * only some values are required.
 */
template<typename T>
static void adapt_curve_domain_point_to_curve_impl(const CurvesGeometry &curves,
                                                   const VArray<T> &old_values,
                                                   MutableSpan<T> r_values)
{
  attribute_math::DefaultMixer<T> mixer(r_values);

  threading::parallel_for(curves.curves_range(), 128, [&](const IndexRange range) {
    for (const int i_curve : range) {
      for (const int i_point : curves.points_for_curve(i_curve)) {
        mixer.mix_in(i_curve, old_values[i_point]);
      }
    }
    mixer.finalize(range);
  });
}

/**
 * A curve is selected if all of its control points were selected.
 *
 * \note Theoretically this interpolation does not need to compute all values at once.
 * However, doing that makes the implementation simpler, and this can be optimized in the future if
 * only some values are required.
 */
template<>
void adapt_curve_domain_point_to_curve_impl(const CurvesGeometry &curves,
                                            const VArray<bool> &old_values,
                                            MutableSpan<bool> r_values)
{
  r_values.fill(true);
  for (const int i_curve : IndexRange(curves.curves_num())) {
    for (const int i_point : curves.points_for_curve(i_curve)) {
      if (!old_values[i_point]) {
        r_values[i_curve] = false;
        break;
      }
    }
  }
}

static GVArray adapt_curve_domain_point_to_curve(const CurvesGeometry &curves,
                                                 const GVArray &varray)
{
  GVArray new_varray;
  attribute_math::convert_to_static_type(varray.type(), [&](auto dummy) {
    using T = decltype(dummy);
    if constexpr (!std::is_void_v<attribute_math::DefaultMixer<T>>) {
      Array<T> values(curves.curves_num());
      adapt_curve_domain_point_to_curve_impl<T>(curves, varray.typed<T>(), values);
      new_varray = VArray<T>::ForContainer(std::move(values));
    }
  });
  return new_varray;
}

/**
 * Copy the value from a curve to all of its points.
 *
 * \note Theoretically this interpolation does not need to compute all values at once.
 * However, doing that makes the implementation simpler, and this can be optimized in the future if
 * only some values are required.
 */
template<typename T>
static void adapt_curve_domain_curve_to_point_impl(const CurvesGeometry &curves,
                                                   const VArray<T> &old_values,
                                                   MutableSpan<T> r_values)
{
  for (const int i_curve : IndexRange(curves.curves_num())) {
    r_values.slice(curves.points_for_curve(i_curve)).fill(old_values[i_curve]);
  }
}

static GVArray adapt_curve_domain_curve_to_point(const CurvesGeometry &curves,
                                                 const GVArray &varray)
{
  GVArray new_varray;
  attribute_math::convert_to_static_type(varray.type(), [&](auto dummy) {
    using T = decltype(dummy);
    Array<T> values(curves.points_num());
    adapt_curve_domain_curve_to_point_impl<T>(curves, varray.typed<T>(), values);
    new_varray = VArray<T>::ForContainer(std::move(values));
  });
  return new_varray;
}

GVArray CurvesGeometry::adapt_domain(const GVArray &varray,
                                     const eAttrDomain from,
                                     const eAttrDomain to) const
{
  if (!varray) {
    return {};
  }
  if (varray.is_empty()) {
    return {};
  }
  if (from == to) {
    return varray;
  }
  if (varray.is_single()) {
    BUFFER_FOR_CPP_TYPE_VALUE(varray.type(), value);
    varray.get_internal_single(value);
    return GVArray::ForSingle(varray.type(), this->attributes().domain_size(to), value);
  }

  if (from == ATTR_DOMAIN_POINT && to == ATTR_DOMAIN_CURVE) {
    return adapt_curve_domain_point_to_curve(*this, varray);
  }
  if (from == ATTR_DOMAIN_CURVE && to == ATTR_DOMAIN_POINT) {
    return adapt_curve_domain_curve_to_point(*this, varray);
  }

  BLI_assert_unreachable();
  return {};
}

/** \} */

}  // namespace blender::bke
