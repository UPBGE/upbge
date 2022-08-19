/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_task.hh"

#include "DNA_ID_enums.h"
#include "DNA_curve_types.h"

#include "BKE_attribute_math.hh"
#include "BKE_curve.h"
#include "BKE_geometry_set.hh"
#include "BKE_lib_id.h"
#include "BKE_spline.hh"

#include "attribute_access_intern.hh"

using blender::GMutableSpan;
using blender::GSpan;
using blender::GVArray;
using blender::GVArraySpan;

/* -------------------------------------------------------------------- */
/** \name Geometry Component Implementation
 * \{ */

CurveComponentLegacy::CurveComponentLegacy() : GeometryComponent(GEO_COMPONENT_TYPE_CURVE)
{
}

CurveComponentLegacy::~CurveComponentLegacy()
{
  this->clear();
}

GeometryComponent *CurveComponentLegacy::copy() const
{
  CurveComponentLegacy *new_component = new CurveComponentLegacy();
  if (curve_ != nullptr) {
    new_component->curve_ = new CurveEval(*curve_);
    new_component->ownership_ = GeometryOwnershipType::Owned;
  }
  return new_component;
}

void CurveComponentLegacy::clear()
{
  BLI_assert(this->is_mutable());
  if (curve_ != nullptr) {
    if (ownership_ == GeometryOwnershipType::Owned) {
      delete curve_;
    }
    curve_ = nullptr;
  }
}

bool CurveComponentLegacy::has_curve() const
{
  return curve_ != nullptr;
}

void CurveComponentLegacy::replace(CurveEval *curve, GeometryOwnershipType ownership)
{
  BLI_assert(this->is_mutable());
  this->clear();
  curve_ = curve;
  ownership_ = ownership;
}

CurveEval *CurveComponentLegacy::release()
{
  BLI_assert(this->is_mutable());
  CurveEval *curve = curve_;
  curve_ = nullptr;
  return curve;
}

const CurveEval *CurveComponentLegacy::get_for_read() const
{
  return curve_;
}

CurveEval *CurveComponentLegacy::get_for_write()
{
  BLI_assert(this->is_mutable());
  if (ownership_ == GeometryOwnershipType::ReadOnly) {
    curve_ = new CurveEval(*curve_);
    ownership_ = GeometryOwnershipType::Owned;
  }
  return curve_;
}

bool CurveComponentLegacy::is_empty() const
{
  return curve_ == nullptr;
}

bool CurveComponentLegacy::owns_direct_data() const
{
  return ownership_ == GeometryOwnershipType::Owned;
}

void CurveComponentLegacy::ensure_owns_direct_data()
{
  BLI_assert(this->is_mutable());
  if (ownership_ != GeometryOwnershipType::Owned) {
    curve_ = new CurveEval(*curve_);
    ownership_ = GeometryOwnershipType::Owned;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Attribute Access Helper Functions
 * \{ */

namespace blender::bke {

namespace {
struct PointIndices {
  int spline_index;
  int point_index;
};
}  // namespace
static PointIndices lookup_point_indices(Span<int> offsets, const int index)
{
  const int spline_index = std::upper_bound(offsets.begin(), offsets.end(), index) -
                           offsets.begin() - 1;
  const int index_in_spline = index - offsets[spline_index];
  return {spline_index, index_in_spline};
}

/**
 * Mix together all of a spline's control point values.
 *
 * \note Theoretically this interpolation does not need to compute all values at once.
 * However, doing that makes the implementation simpler, and this can be optimized in the future if
 * only some values are required.
 */
template<typename T>
static void adapt_curve_domain_point_to_spline_impl(const CurveEval &curve,
                                                    const VArray<T> &old_values,
                                                    MutableSpan<T> r_values)
{
  const int splines_len = curve.splines().size();
  Array<int> offsets = curve.control_point_offsets();
  BLI_assert(r_values.size() == splines_len);
  attribute_math::DefaultMixer<T> mixer(r_values);

  for (const int i_spline : IndexRange(splines_len)) {
    const int spline_offset = offsets[i_spline];
    const int spline_point_len = offsets[i_spline + 1] - spline_offset;
    for (const int i_point : IndexRange(spline_point_len)) {
      const T value = old_values[spline_offset + i_point];
      mixer.mix_in(i_spline, value);
    }
  }

  mixer.finalize();
}

/**
 * A spline is selected if all of its control points were selected.
 *
 * \note Theoretically this interpolation does not need to compute all values at once.
 * However, doing that makes the implementation simpler, and this can be optimized in the future if
 * only some values are required.
 */
template<>
void adapt_curve_domain_point_to_spline_impl(const CurveEval &curve,
                                             const VArray<bool> &old_values,
                                             MutableSpan<bool> r_values)
{
  const int splines_len = curve.splines().size();
  Array<int> offsets = curve.control_point_offsets();
  BLI_assert(r_values.size() == splines_len);

  r_values.fill(true);

  for (const int i_spline : IndexRange(splines_len)) {
    const int spline_offset = offsets[i_spline];
    const int spline_point_len = offsets[i_spline + 1] - spline_offset;

    for (const int i_point : IndexRange(spline_point_len)) {
      if (!old_values[spline_offset + i_point]) {
        r_values[i_spline] = false;
        break;
      }
    }
  }
}

static GVArray adapt_curve_domain_point_to_spline(const CurveEval &curve, GVArray varray)
{
  GVArray new_varray;
  attribute_math::convert_to_static_type(varray.type(), [&](auto dummy) {
    using T = decltype(dummy);
    if constexpr (!std::is_void_v<attribute_math::DefaultMixer<T>>) {
      Array<T> values(curve.splines().size());
      adapt_curve_domain_point_to_spline_impl<T>(curve, varray.typed<T>(), values);
      new_varray = VArray<T>::ForContainer(std::move(values));
    }
  });
  return new_varray;
}

/**
 * A virtual array implementation for the conversion of spline attributes to control point
 * attributes. The goal is to avoid copying the spline value for every one of its control points
 * unless it is necessary (in that case the materialize functions will be called).
 */
template<typename T> class VArray_For_SplineToPoint final : public VArrayImpl<T> {
  GVArray original_varray_;
  /* Store existing data materialized if it was not already a span. This is expected
   * to be worth it because a single spline's value will likely be accessed many times. */
  VArraySpan<T> original_data_;
  Array<int> offsets_;

 public:
  VArray_For_SplineToPoint(GVArray original_varray, Array<int> offsets)
      : VArrayImpl<T>(offsets.last()),
        original_varray_(std::move(original_varray)),
        original_data_(original_varray_.typed<T>()),
        offsets_(std::move(offsets))
  {
  }

  T get(const int64_t index) const final
  {
    const PointIndices indices = lookup_point_indices(offsets_, index);
    return original_data_[indices.spline_index];
  }

  void materialize(const IndexMask mask, MutableSpan<T> r_span) const final
  {
    const int total_num = offsets_.last();
    if (mask.is_range() && mask.as_range() == IndexRange(total_num)) {
      for (const int spline_index : original_data_.index_range()) {
        const int offset = offsets_[spline_index];
        const int next_offset = offsets_[spline_index + 1];
        r_span.slice(offset, next_offset - offset).fill(original_data_[spline_index]);
      }
    }
    else {
      int spline_index = 0;
      for (const int dst_index : mask) {
        while (offsets_[spline_index] < dst_index) {
          spline_index++;
        }
        r_span[dst_index] = original_data_[spline_index];
      }
    }
  }

  void materialize_to_uninitialized(const IndexMask mask, MutableSpan<T> r_span) const final
  {
    T *dst = r_span.data();
    const int total_num = offsets_.last();
    if (mask.is_range() && mask.as_range() == IndexRange(total_num)) {
      for (const int spline_index : original_data_.index_range()) {
        const int offset = offsets_[spline_index];
        const int next_offset = offsets_[spline_index + 1];
        uninitialized_fill_n(dst + offset, next_offset - offset, original_data_[spline_index]);
      }
    }
    else {
      int spline_index = 0;
      for (const int dst_index : mask) {
        while (offsets_[spline_index] < dst_index) {
          spline_index++;
        }
        new (dst + dst_index) T(original_data_[spline_index]);
      }
    }
  }
};

static GVArray adapt_curve_domain_spline_to_point(const CurveEval &curve, GVArray varray)
{
  GVArray new_varray;
  attribute_math::convert_to_static_type(varray.type(), [&](auto dummy) {
    using T = decltype(dummy);

    Array<int> offsets = curve.control_point_offsets();
    new_varray = VArray<T>::template For<VArray_For_SplineToPoint<T>>(std::move(varray),
                                                                      std::move(offsets));
  });
  return new_varray;
}

}  // namespace blender::bke

static GVArray adapt_curve_attribute_domain(const CurveEval &curve,
                                            const GVArray &varray,
                                            const eAttrDomain from_domain,
                                            const eAttrDomain to_domain)
{
  if (!varray) {
    return {};
  }
  if (varray.is_empty()) {
    return {};
  }
  if (from_domain == to_domain) {
    return varray;
  }

  if (from_domain == ATTR_DOMAIN_POINT && to_domain == ATTR_DOMAIN_CURVE) {
    return blender::bke::adapt_curve_domain_point_to_spline(curve, std::move(varray));
  }
  if (from_domain == ATTR_DOMAIN_CURVE && to_domain == ATTR_DOMAIN_POINT) {
    return blender::bke::adapt_curve_domain_spline_to_point(curve, std::move(varray));
  }

  return {};
}

/** \} */

namespace blender::bke {

/* -------------------------------------------------------------------- */
/** \name Builtin Spline Attributes
 *
 * Attributes with a value for every spline, stored contiguously or in every spline separately.
 * \{ */

class BuiltinSplineAttributeProvider final : public BuiltinAttributeProvider {
  using AsReadAttribute = GVArray (*)(const CurveEval &data);
  using AsWriteAttribute = GVMutableArray (*)(CurveEval &data);
  const AsReadAttribute as_read_attribute_;
  const AsWriteAttribute as_write_attribute_;

 public:
  BuiltinSplineAttributeProvider(std::string attribute_name,
                                 const eCustomDataType attribute_type,
                                 const WritableEnum writable,
                                 const AsReadAttribute as_read_attribute,
                                 const AsWriteAttribute as_write_attribute)
      : BuiltinAttributeProvider(std::move(attribute_name),
                                 ATTR_DOMAIN_CURVE,
                                 attribute_type,
                                 BuiltinAttributeProvider::NonCreatable,
                                 writable,
                                 BuiltinAttributeProvider::NonDeletable),
        as_read_attribute_(as_read_attribute),
        as_write_attribute_(as_write_attribute)
  {
  }

  GVArray try_get_for_read(const void *owner) const final
  {
    const CurveEval *curve = static_cast<const CurveEval *>(owner);
    if (curve == nullptr) {
      return {};
    }
    return as_read_attribute_(*curve);
  }

  GAttributeWriter try_get_for_write(void *owner) const final
  {
    if (writable_ != Writable) {
      return {};
    }
    CurveEval *curve = static_cast<CurveEval *>(owner);
    if (curve == nullptr) {
      return {};
    }
    return {as_write_attribute_(*curve), domain_};
  }

  bool try_delete(void *UNUSED(owner)) const final
  {
    return false;
  }

  bool try_create(void *UNUSED(owner), const AttributeInit &UNUSED(initializer)) const final
  {
    return false;
  }

  bool exists(const void *owner) const final
  {
    const CurveEval *curve = static_cast<const CurveEval *>(owner);
    return !curve->splines().is_empty();
  }
};

static int get_spline_resolution(const SplinePtr &spline)
{
  if (const BezierSpline *bezier_spline = dynamic_cast<const BezierSpline *>(spline.get())) {
    return bezier_spline->resolution();
  }
  if (const NURBSpline *nurb_spline = dynamic_cast<const NURBSpline *>(spline.get())) {
    return nurb_spline->resolution();
  }
  return 1;
}

static void set_spline_resolution(SplinePtr &spline, const int resolution)
{
  if (BezierSpline *bezier_spline = dynamic_cast<BezierSpline *>(spline.get())) {
    bezier_spline->set_resolution(std::max(resolution, 1));
  }
  if (NURBSpline *nurb_spline = dynamic_cast<NURBSpline *>(spline.get())) {
    nurb_spline->set_resolution(std::max(resolution, 1));
  }
}

static GVArray make_resolution_read_attribute(const CurveEval &curve)
{
  return VArray<int>::ForDerivedSpan<SplinePtr, get_spline_resolution>(curve.splines());
}

static GVMutableArray make_resolution_write_attribute(CurveEval &curve)
{
  return VMutableArray<int>::
      ForDerivedSpan<SplinePtr, get_spline_resolution, set_spline_resolution>(curve.splines());
}

static bool get_cyclic_value(const SplinePtr &spline)
{
  return spline->is_cyclic();
}

static void set_cyclic_value(SplinePtr &spline, const bool value)
{
  if (spline->is_cyclic() != value) {
    spline->set_cyclic(value);
    spline->mark_cache_invalid();
  }
}

static GVArray make_cyclic_read_attribute(const CurveEval &curve)
{
  return VArray<bool>::ForDerivedSpan<SplinePtr, get_cyclic_value>(curve.splines());
}

static GVMutableArray make_cyclic_write_attribute(CurveEval &curve)
{
  return VMutableArray<bool>::ForDerivedSpan<SplinePtr, get_cyclic_value, set_cyclic_value>(
      curve.splines());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Builtin Control Point Attributes
 *
 * Attributes with a value for every control point. Most of the complexity here is due to the fact
 * that we must provide access to the attribute data as if it was a contiguous array when it is
 * really stored separately on each spline. That will be inherently rather slow, but these virtual
 * array implementations try to make it workable in common situations.
 * \{ */

/**
 * Individual spans in \a data may be empty if that spline contains no data for the attribute.
 */
template<typename T>
static void point_attribute_materialize(Span<Span<T>> data,
                                        Span<int> offsets,
                                        const IndexMask mask,
                                        MutableSpan<T> r_span)
{
  const int total_num = offsets.last();
  if (mask.is_range() && mask.as_range() == IndexRange(total_num)) {
    for (const int spline_index : data.index_range()) {
      const int offset = offsets[spline_index];
      const int next_offset = offsets[spline_index + 1];

      Span<T> src = data[spline_index];
      MutableSpan<T> dst = r_span.slice(offset, next_offset - offset);
      if (src.is_empty()) {
        dst.fill(T());
      }
      else {
        dst.copy_from(src);
      }
    }
  }
  else {
    int spline_index = 0;
    for (const int dst_index : mask) {
      /* Skip splines that don't have any control points in the mask. */
      while (dst_index >= offsets[spline_index + 1]) {
        spline_index++;
      }

      const int index_in_spline = dst_index - offsets[spline_index];
      Span<T> src = data[spline_index];
      if (src.is_empty()) {
        r_span[dst_index] = T();
      }
      else {
        r_span[dst_index] = src[index_in_spline];
      }
    }
  }
}

/**
 * Individual spans in \a data may be empty if that spline contains no data for the attribute.
 */
template<typename T>
static void point_attribute_materialize_to_uninitialized(Span<Span<T>> data,
                                                         Span<int> offsets,
                                                         const IndexMask mask,
                                                         MutableSpan<T> r_span)
{
  T *dst = r_span.data();
  const int total_num = offsets.last();
  if (mask.is_range() && mask.as_range() == IndexRange(total_num)) {
    for (const int spline_index : data.index_range()) {
      const int offset = offsets[spline_index];
      const int next_offset = offsets[spline_index + 1];

      Span<T> src = data[spline_index];
      if (src.is_empty()) {
        uninitialized_fill_n(dst + offset, next_offset - offset, T());
      }
      else {
        uninitialized_copy_n(src.data(), next_offset - offset, dst + offset);
      }
    }
  }
  else {
    int spline_index = 0;
    for (const int dst_index : mask) {
      /* Skip splines that don't have any control points in the mask. */
      while (dst_index >= offsets[spline_index + 1]) {
        spline_index++;
      }

      const int index_in_spline = dst_index - offsets[spline_index];
      Span<T> src = data[spline_index];
      if (src.is_empty()) {
        new (dst + dst_index) T();
      }
      else {
        new (dst + dst_index) T(src[index_in_spline]);
      }
    }
  }
}

static GVArray varray_from_initializer(const AttributeInit &initializer,
                                       const eCustomDataType data_type,
                                       const Span<SplinePtr> splines)
{
  switch (initializer.type) {
    case AttributeInit::Type::Default:
      /* This function shouldn't be called in this case, since there
       * is no need to copy anything to the new custom data array. */
      BLI_assert_unreachable();
      return {};
    case AttributeInit::Type::VArray:
      return static_cast<const AttributeInitVArray &>(initializer).varray;
    case AttributeInit::Type::MoveArray:
      int total_num = 0;
      for (const SplinePtr &spline : splines) {
        total_num += spline->size();
      }
      return GVArray::ForSpan(GSpan(*bke::custom_data_type_to_cpp_type(data_type),
                                    static_cast<const AttributeInitMove &>(initializer).data,
                                    total_num));
  }
  BLI_assert_unreachable();
  return {};
}

static bool create_point_attribute(CurveEval *curve,
                                   const AttributeIDRef &attribute_id,
                                   const AttributeInit &initializer,
                                   const eCustomDataType data_type)
{
  if (curve == nullptr || curve->splines().size() == 0) {
    return false;
  }

  MutableSpan<SplinePtr> splines = curve->splines();

  /* First check the one case that allows us to avoid copying the input data. */
  if (splines.size() == 1 && initializer.type == AttributeInit::Type::MoveArray) {
    void *source_data = static_cast<const AttributeInitMove &>(initializer).data;
    if (!splines.first()->attributes.create_by_move(attribute_id, data_type, source_data)) {
      MEM_freeN(source_data);
      return false;
    }
    return true;
  }

  /* Otherwise just create a custom data layer on each of the splines. */
  for (const int i : splines.index_range()) {
    if (!splines[i]->attributes.create(attribute_id, data_type)) {
      /* If attribute creation fails on one of the splines, we cannot leave the custom data
       * layers in the previous splines around, so delete them before returning. However,
       * this is not an expected case. */
      BLI_assert_unreachable();
      return false;
    }
  }

  /* With a default initializer type, we can keep the values at their initial values. */
  if (initializer.type == AttributeInit::Type::Default) {
    return true;
  }

  GAttributeWriter write_attribute = curve->attributes_for_write().lookup_for_write(attribute_id);
  /* We just created the attribute, it should exist. */
  BLI_assert(write_attribute);

  GVArray source_varray = varray_from_initializer(initializer, data_type, splines);
  /* TODO: When we can call a variant of #set_all with a virtual array argument,
   * this theoretically unnecessary materialize step could be removed. */
  GVArraySpan source_VArraySpan{source_varray};
  write_attribute.varray.set_all(source_VArraySpan.data());
  write_attribute.finish();

  if (initializer.type == AttributeInit::Type::MoveArray) {
    MEM_freeN(static_cast<const AttributeInitMove &>(initializer).data);
  }

  return true;
}

static bool remove_point_attribute(CurveEval *curve, const AttributeIDRef &attribute_id)
{
  if (curve == nullptr) {
    return false;
  }

  /* Reuse the boolean for all splines; we expect all splines to have the same attributes. */
  bool layer_freed = false;
  for (SplinePtr &spline : curve->splines()) {
    layer_freed = spline->attributes.remove(attribute_id);
  }
  return layer_freed;
}

/**
 * Mutable virtual array for any control point data accessed with spans and an offset array.
 */
template<typename T> class VArrayImpl_For_SplinePoints final : public VMutableArrayImpl<T> {
 private:
  Array<MutableSpan<T>> data_;
  Array<int> offsets_;

 public:
  VArrayImpl_For_SplinePoints(Array<MutableSpan<T>> data, Array<int> offsets)
      : VMutableArrayImpl<T>(offsets.last()), data_(std::move(data)), offsets_(std::move(offsets))
  {
  }

  T get(const int64_t index) const final
  {
    const PointIndices indices = lookup_point_indices(offsets_, index);
    return data_[indices.spline_index][indices.point_index];
  }

  void set(const int64_t index, T value) final
  {
    const PointIndices indices = lookup_point_indices(offsets_, index);
    data_[indices.spline_index][indices.point_index] = value;
  }

  void set_all(Span<T> src) final
  {
    for (const int spline_index : data_.index_range()) {
      const int offset = offsets_[spline_index];
      const int next_offsets = offsets_[spline_index + 1];
      data_[spline_index].copy_from(src.slice(offset, next_offsets - offset));
    }
  }

  void materialize(const IndexMask mask, MutableSpan<T> r_span) const final
  {
    point_attribute_materialize({(Span<T> *)data_.data(), data_.size()}, offsets_, mask, r_span);
  }

  void materialize_to_uninitialized(const IndexMask mask, MutableSpan<T> r_span) const final
  {
    point_attribute_materialize_to_uninitialized(
        {(Span<T> *)data_.data(), data_.size()}, offsets_, mask, r_span);
  }
};

template<typename T> VArray<T> point_data_varray(Array<MutableSpan<T>> spans, Array<int> offsets)
{
  return VArray<T>::template For<VArrayImpl_For_SplinePoints<T>>(std::move(spans),
                                                                 std::move(offsets));
}

template<typename T>
VMutableArray<T> point_data_varray_mutable(Array<MutableSpan<T>> spans, Array<int> offsets)
{
  return VMutableArray<T>::template For<VArrayImpl_For_SplinePoints<T>>(std::move(spans),
                                                                        std::move(offsets));
}

/**
 * Virtual array implementation specifically for control point positions. This is only needed for
 * Bezier splines, where adjusting the position also requires adjusting handle positions depending
 * on handle types. We pay a small price for this when other spline types are mixed with Bezier.
 *
 * \note There is no need to check the handle type to avoid changing auto handles, since
 * retrieving write access to the position data will mark them for recomputation anyway.
 */
class VArrayImpl_For_SplinePosition final : public VMutableArrayImpl<float3> {
 private:
  MutableSpan<SplinePtr> splines_;
  Array<int> offsets_;

 public:
  VArrayImpl_For_SplinePosition(MutableSpan<SplinePtr> splines, Array<int> offsets)
      : VMutableArrayImpl<float3>(offsets.last()), splines_(splines), offsets_(std::move(offsets))
  {
  }

  float3 get(const int64_t index) const final
  {
    const PointIndices indices = lookup_point_indices(offsets_, index);
    return splines_[indices.spline_index]->positions()[indices.point_index];
  }

  void set(const int64_t index, float3 value) final
  {
    const PointIndices indices = lookup_point_indices(offsets_, index);
    Spline &spline = *splines_[indices.spline_index];
    spline.positions()[indices.point_index] = value;
  }

  void set_all(Span<float3> src) final
  {
    for (const int spline_index : splines_.index_range()) {
      Spline &spline = *splines_[spline_index];
      const int offset = offsets_[spline_index];
      const int next_offset = offsets_[spline_index + 1];
      spline.positions().copy_from(src.slice(offset, next_offset - offset));
    }
  }

  /** Utility so we can pass positions to the materialize functions above. */
  Array<Span<float3>> get_position_spans() const
  {
    Array<Span<float3>> spans(splines_.size());
    for (const int i : spans.index_range()) {
      spans[i] = splines_[i]->positions();
    }
    return spans;
  }

  void materialize(const IndexMask mask, MutableSpan<float3> r_span) const final
  {
    Array<Span<float3>> spans = this->get_position_spans();
    point_attribute_materialize(spans.as_span(), offsets_, mask, r_span);
  }

  void materialize_to_uninitialized(const IndexMask mask, MutableSpan<float3> r_span) const final
  {
    Array<Span<float3>> spans = this->get_position_spans();
    point_attribute_materialize_to_uninitialized(spans.as_span(), offsets_, mask, r_span);
  }
};

class VArrayImpl_For_BezierHandles final : public VMutableArrayImpl<float3> {
 private:
  MutableSpan<SplinePtr> splines_;
  Array<int> offsets_;
  bool is_right_;

 public:
  VArrayImpl_For_BezierHandles(MutableSpan<SplinePtr> splines,
                               Array<int> offsets,
                               const bool is_right)
      : VMutableArrayImpl<float3>(offsets.last()),
        splines_(splines),
        offsets_(std::move(offsets)),
        is_right_(is_right)
  {
  }

  float3 get(const int64_t index) const final
  {
    const PointIndices indices = lookup_point_indices(offsets_, index);
    const Spline &spline = *splines_[indices.spline_index];
    if (spline.type() == CURVE_TYPE_BEZIER) {
      const BezierSpline &bezier_spline = static_cast<const BezierSpline &>(spline);
      return is_right_ ? bezier_spline.handle_positions_right()[indices.point_index] :
                         bezier_spline.handle_positions_left()[indices.point_index];
    }
    return float3(0);
  }

  void set(const int64_t index, float3 value) final
  {
    const PointIndices indices = lookup_point_indices(offsets_, index);
    Spline &spline = *splines_[indices.spline_index];
    if (spline.type() == CURVE_TYPE_BEZIER) {
      BezierSpline &bezier_spline = static_cast<BezierSpline &>(spline);
      if (is_right_) {
        bezier_spline.handle_positions_right()[indices.point_index] = value;
      }
      else {
        bezier_spline.handle_positions_left()[indices.point_index] = value;
      }
      bezier_spline.mark_cache_invalid();
    }
  }

  void set_all(Span<float3> src) final
  {
    for (const int spline_index : splines_.index_range()) {
      Spline &spline = *splines_[spline_index];
      if (spline.type() == CURVE_TYPE_BEZIER) {
        const int offset = offsets_[spline_index];

        BezierSpline &bezier_spline = static_cast<BezierSpline &>(spline);
        if (is_right_) {
          for (const int i : IndexRange(bezier_spline.size())) {
            bezier_spline.handle_positions_right()[i] = src[offset + i];
          }
        }
        else {
          for (const int i : IndexRange(bezier_spline.size())) {
            bezier_spline.handle_positions_left()[i] = src[offset + i];
          }
        }
        bezier_spline.mark_cache_invalid();
      }
    }
  }

  void materialize(const IndexMask mask, MutableSpan<float3> r_span) const final
  {
    Array<Span<float3>> spans = get_handle_spans(splines_, is_right_);
    point_attribute_materialize(spans.as_span(), offsets_, mask, r_span);
  }

  void materialize_to_uninitialized(const IndexMask mask, MutableSpan<float3> r_span) const final
  {
    Array<Span<float3>> spans = get_handle_spans(splines_, is_right_);
    point_attribute_materialize_to_uninitialized(spans.as_span(), offsets_, mask, r_span);
  }

  /**
   * Utility so we can pass handle positions to the materialize functions above.
   *
   * \note This relies on the ability of the materialize implementations to
   * handle empty spans, since only Bezier splines have handles.
   */
  static Array<Span<float3>> get_handle_spans(Span<SplinePtr> splines, const bool is_right)
  {
    Array<Span<float3>> spans(splines.size());
    for (const int i : spans.index_range()) {
      if (splines[i]->type() == CURVE_TYPE_BEZIER) {
        BezierSpline &bezier_spline = static_cast<BezierSpline &>(*splines[i]);
        spans[i] = is_right ? bezier_spline.handle_positions_right() :
                              bezier_spline.handle_positions_left();
      }
      else {
        spans[i] = {};
      }
    }
    return spans;
  }
};

/**
 * Provider for any builtin control point attribute that doesn't need
 * special handling like access to other arrays in the spline.
 */
template<typename T> class BuiltinPointAttributeProvider : public BuiltinAttributeProvider {
 protected:
  using GetSpan = Span<T> (*)(const Spline &spline);
  using GetMutableSpan = MutableSpan<T> (*)(Spline &spline);
  using UpdateOnWrite = void (*)(Spline &spline);
  const GetSpan get_span_;
  const GetMutableSpan get_mutable_span_;
  const UpdateOnWrite update_on_write_;
  bool stored_in_custom_data_;

 public:
  BuiltinPointAttributeProvider(std::string attribute_name,
                                const CreatableEnum creatable,
                                const DeletableEnum deletable,
                                const GetSpan get_span,
                                const GetMutableSpan get_mutable_span,
                                const UpdateOnWrite update_on_write,
                                const bool stored_in_custom_data)
      : BuiltinAttributeProvider(std::move(attribute_name),
                                 ATTR_DOMAIN_POINT,
                                 bke::cpp_type_to_custom_data_type(CPPType::get<T>()),
                                 creatable,
                                 WritableEnum::Writable,
                                 deletable),
        get_span_(get_span),
        get_mutable_span_(get_mutable_span),
        update_on_write_(update_on_write),
        stored_in_custom_data_(stored_in_custom_data)
  {
  }

  GVArray try_get_for_read(const void *owner) const override
  {
    const CurveEval *curve = static_cast<const CurveEval *>(owner);
    if (curve == nullptr) {
      return {};
    }

    if (!this->exists(owner)) {
      return {};
    }

    Span<SplinePtr> splines = curve->splines();
    if (splines.size() == 1) {
      return GVArray::ForSpan(get_span_(*splines.first()));
    }

    Array<int> offsets = curve->control_point_offsets();
    Array<MutableSpan<T>> spans(splines.size());
    for (const int i : splines.index_range()) {
      Span<T> span = get_span_(*splines[i]);
      /* Use const-cast because the underlying virtual array implementation is shared between const
       * and non const data. */
      spans[i] = MutableSpan<T>(const_cast<T *>(span.data()), span.size());
    }

    return point_data_varray(spans, offsets);
  }

  GAttributeWriter try_get_for_write(void *owner) const override
  {
    CurveEval *curve = static_cast<CurveEval *>(owner);
    if (curve == nullptr) {
      return {};
    }

    if (!this->exists(owner)) {
      return {};
    }

    std::function<void()> tag_modified_fn;
    if (update_on_write_ != nullptr) {
      tag_modified_fn = [curve, update = update_on_write_]() {
        for (SplinePtr &spline : curve->splines()) {
          update(*spline);
        }
      };
    }

    MutableSpan<SplinePtr> splines = curve->splines();
    if (splines.size() == 1) {
      return {GVMutableArray::ForSpan(get_mutable_span_(*splines.first())),
              domain_,
              std::move(tag_modified_fn)};
    }

    Array<int> offsets = curve->control_point_offsets();
    Array<MutableSpan<T>> spans(splines.size());
    for (const int i : splines.index_range()) {
      spans[i] = get_mutable_span_(*splines[i]);
    }

    return {point_data_varray_mutable(spans, offsets), domain_, tag_modified_fn};
  }

  bool try_delete(void *owner) const final
  {
    if (deletable_ == DeletableEnum::NonDeletable) {
      return false;
    }
    CurveEval *curve = static_cast<CurveEval *>(owner);
    return remove_point_attribute(curve, name_);
  }

  bool try_create(void *owner, const AttributeInit &initializer) const final
  {
    if (createable_ == CreatableEnum::NonCreatable) {
      return false;
    }
    CurveEval *curve = static_cast<CurveEval *>(owner);
    return create_point_attribute(curve, name_, initializer, CD_PROP_INT32);
  }

  bool exists(const void *owner) const final
  {
    const CurveEval *curve = static_cast<const CurveEval *>(owner);
    if (curve == nullptr) {
      return false;
    }

    Span<SplinePtr> splines = curve->splines();
    if (splines.size() == 0) {
      return false;
    }

    if (stored_in_custom_data_) {
      if (!curve->splines().first()->attributes.get_for_read(name_)) {
        return false;
      }
    }

    bool has_point = false;
    for (const SplinePtr &spline : curve->splines()) {
      if (spline->size() != 0) {
        has_point = true;
        break;
      }
    }

    if (!has_point) {
      return false;
    }

    return true;
  }
};

/**
 * Special attribute provider for the position attribute. Keeping this separate means we don't
 * need to make #BuiltinPointAttributeProvider overly generic, and the special handling for the
 * positions is more clear.
 */
class PositionAttributeProvider final : public BuiltinPointAttributeProvider<float3> {
 public:
  PositionAttributeProvider()
      : BuiltinPointAttributeProvider(
            "position",
            BuiltinAttributeProvider::NonCreatable,
            BuiltinAttributeProvider::NonDeletable,
            [](const Spline &spline) { return spline.positions(); },
            [](Spline &spline) { return spline.positions(); },
            [](Spline &spline) { spline.mark_cache_invalid(); },
            false)
  {
  }

  GAttributeWriter try_get_for_write(void *owner) const final
  {
    CurveEval *curve = static_cast<CurveEval *>(owner);
    if (curve == nullptr) {
      return {};
    }

    /* Use the regular position virtual array when there aren't any Bezier splines
     * to avoid the overhead of checking the spline type for every point. */
    if (!curve->has_spline_with_type(CURVE_TYPE_BEZIER)) {
      return BuiltinPointAttributeProvider<float3>::try_get_for_write(owner);
    }

    auto tag_modified_fn = [curve]() {
      /* Changing the positions requires recalculation of cached evaluated data in many cases.
       * This could set more specific flags in the future to avoid unnecessary recomputation. */
      curve->mark_cache_invalid();
    };

    Array<int> offsets = curve->control_point_offsets();
    return {VMutableArray<float3>::For<VArrayImpl_For_SplinePosition>(curve->splines(),
                                                                      std::move(offsets)),
            domain_,
            tag_modified_fn};
  }
};

class BezierHandleAttributeProvider : public BuiltinAttributeProvider {
 private:
  bool is_right_;

 public:
  BezierHandleAttributeProvider(const bool is_right)
      : BuiltinAttributeProvider(is_right ? "handle_right" : "handle_left",
                                 ATTR_DOMAIN_POINT,
                                 CD_PROP_FLOAT3,
                                 BuiltinAttributeProvider::NonCreatable,
                                 BuiltinAttributeProvider::Writable,
                                 BuiltinAttributeProvider::NonDeletable),
        is_right_(is_right)
  {
  }

  GVArray try_get_for_read(const void *owner) const override
  {
    const CurveEval *curve = static_cast<const CurveEval *>(owner);
    if (curve == nullptr) {
      return {};
    }

    if (!curve->has_spline_with_type(CURVE_TYPE_BEZIER)) {
      return {};
    }

    Array<int> offsets = curve->control_point_offsets();
    /* Use const-cast because the underlying virtual array implementation is shared between const
     * and non const data. */
    return VArray<float3>::For<VArrayImpl_For_BezierHandles>(
        const_cast<CurveEval *>(curve)->splines(), std::move(offsets), is_right_);
  }

  GAttributeWriter try_get_for_write(void *owner) const override
  {
    CurveEval *curve = static_cast<CurveEval *>(owner);
    if (curve == nullptr) {
      return {};
    }

    if (!curve->has_spline_with_type(CURVE_TYPE_BEZIER)) {
      return {};
    }

    auto tag_modified_fn = [curve]() { curve->mark_cache_invalid(); };

    Array<int> offsets = curve->control_point_offsets();
    return {VMutableArray<float3>::For<VArrayImpl_For_BezierHandles>(
                curve->splines(), std::move(offsets), is_right_),
            domain_,
            tag_modified_fn};
  }

  bool try_delete(void *UNUSED(owner)) const final
  {
    return false;
  }

  bool try_create(void *UNUSED(owner), const AttributeInit &UNUSED(initializer)) const final
  {
    return false;
  }

  bool exists(const void *owner) const final
  {
    const CurveEval *curve = static_cast<const CurveEval *>(owner);
    if (curve == nullptr) {
      return false;
    }

    CurveComponentLegacy component;
    component.replace(const_cast<CurveEval *>(curve), GeometryOwnershipType::ReadOnly);

    return curve->has_spline_with_type(CURVE_TYPE_BEZIER) && !curve->splines().is_empty();
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dynamic Control Point Attributes
 *
 * The dynamic control point attribute implementation is very similar to the builtin attribute
 * implementation-- it uses the same virtual array types. In order to work, this code depends on
 * the fact that all a curve's splines will have the same attributes and they all have the same
 * type.
 * \{ */

class DynamicPointAttributeProvider final : public DynamicAttributesProvider {
 private:
  static constexpr uint64_t supported_types_mask = CD_MASK_PROP_FLOAT | CD_MASK_PROP_FLOAT2 |
                                                   CD_MASK_PROP_FLOAT3 | CD_MASK_PROP_INT32 |
                                                   CD_MASK_PROP_COLOR | CD_MASK_PROP_BOOL |
                                                   CD_MASK_PROP_INT8;

 public:
  GAttributeReader try_get_for_read(const void *owner,
                                    const AttributeIDRef &attribute_id) const final
  {
    const CurveEval *curve = static_cast<const CurveEval *>(owner);
    if (curve == nullptr || curve->splines().size() == 0) {
      return {};
    }

    Span<SplinePtr> splines = curve->splines();
    Vector<GSpan> spans; /* GSpan has no default constructor. */
    spans.reserve(splines.size());
    std::optional<GSpan> first_span = splines[0]->attributes.get_for_read(attribute_id);
    if (!first_span) {
      return {};
    }
    spans.append(*first_span);
    for (const int i : IndexRange(1, splines.size() - 1)) {
      std::optional<GSpan> span = splines[i]->attributes.get_for_read(attribute_id);
      if (!span) {
        /* All splines should have the same set of data layers. It would be possible to recover
         * here and return partial data instead, but that would add a lot of complexity for a
         * situation we don't even expect to encounter. */
        BLI_assert_unreachable();
        return {};
      }
      if (span->type() != spans.last().type()) {
        /* Data layer types on separate splines do not match. */
        BLI_assert_unreachable();
        return {};
      }
      spans.append(*span);
    }

    /* First check for the simpler situation when we can return a simpler span virtual array. */
    if (spans.size() == 1) {
      return {GVArray::ForSpan(spans.first()), ATTR_DOMAIN_POINT};
    }

    GAttributeReader attribute = {};
    Array<int> offsets = curve->control_point_offsets();
    attribute_math::convert_to_static_type(spans[0].type(), [&](auto dummy) {
      using T = decltype(dummy);
      Array<MutableSpan<T>> data(splines.size());
      for (const int i : splines.index_range()) {
        Span<T> span = spans[i].typed<T>();
        /* Use const-cast because the underlying virtual array implementation is shared between
         * const and non const data. */
        data[i] = MutableSpan<T>(const_cast<T *>(span.data()), span.size());
        BLI_assert(data[i].data() != nullptr);
      }
      attribute = {point_data_varray(data, offsets), ATTR_DOMAIN_POINT};
    });
    return attribute;
  }

  /* This function is almost the same as #try_get_for_read, but without const. */
  GAttributeWriter try_get_for_write(void *owner, const AttributeIDRef &attribute_id) const final
  {
    CurveEval *curve = static_cast<CurveEval *>(owner);
    if (curve == nullptr || curve->splines().size() == 0) {
      return {};
    }

    MutableSpan<SplinePtr> splines = curve->splines();
    Vector<GMutableSpan> spans; /* GMutableSpan has no default constructor. */
    spans.reserve(splines.size());
    std::optional<GMutableSpan> first_span = splines[0]->attributes.get_for_write(attribute_id);
    if (!first_span) {
      return {};
    }
    spans.append(*first_span);
    for (const int i : IndexRange(1, splines.size() - 1)) {
      std::optional<GMutableSpan> span = splines[i]->attributes.get_for_write(attribute_id);
      if (!span) {
        /* All splines should have the same set of data layers. It would be possible to recover
         * here and return partial data instead, but that would add a lot of complexity for a
         * situation we don't even expect to encounter. */
        BLI_assert_unreachable();
        return {};
      }
      if (span->type() != spans.last().type()) {
        /* Data layer types on separate splines do not match. */
        BLI_assert_unreachable();
        return {};
      }
      spans.append(*span);
    }

    /* First check for the simpler situation when we can return a simpler span virtual array. */
    if (spans.size() == 1) {
      return {GVMutableArray::ForSpan(spans.first()), ATTR_DOMAIN_POINT};
    }

    GAttributeWriter attribute = {};
    Array<int> offsets = curve->control_point_offsets();
    attribute_math::convert_to_static_type(spans[0].type(), [&](auto dummy) {
      using T = decltype(dummy);
      Array<MutableSpan<T>> data(splines.size());
      for (const int i : splines.index_range()) {
        data[i] = spans[i].typed<T>();
        BLI_assert(data[i].data() != nullptr);
      }
      attribute = {point_data_varray_mutable(data, offsets), ATTR_DOMAIN_POINT};
    });
    return attribute;
  }

  bool try_delete(void *owner, const AttributeIDRef &attribute_id) const final
  {
    CurveEval *curve = static_cast<CurveEval *>(owner);
    return remove_point_attribute(curve, attribute_id);
  }

  bool try_create(void *owner,
                  const AttributeIDRef &attribute_id,
                  const eAttrDomain domain,
                  const eCustomDataType data_type,
                  const AttributeInit &initializer) const final
  {
    BLI_assert(this->type_is_supported(data_type));
    if (domain != ATTR_DOMAIN_POINT) {
      return false;
    }
    CurveEval *curve = static_cast<CurveEval *>(owner);
    return create_point_attribute(curve, attribute_id, initializer, data_type);
  }

  bool foreach_attribute(const void *owner, const AttributeForeachCallback callback) const final
  {
    const CurveEval *curve = static_cast<const CurveEval *>(owner);
    if (curve == nullptr || curve->splines().size() == 0) {
      return false;
    }

    Span<SplinePtr> splines = curve->splines();

    /* In a debug build, check that all corresponding custom data layers have the same type. */
    curve->assert_valid_point_attributes();

    /* Use the first spline as a representative for all the others. */
    splines.first()->attributes.foreach_attribute(callback, ATTR_DOMAIN_POINT);

    return true;
  }

  void foreach_domain(const FunctionRef<void(eAttrDomain)> callback) const final
  {
    callback(ATTR_DOMAIN_POINT);
  }

  bool type_is_supported(eCustomDataType data_type) const
  {
    return ((1ULL << data_type) & supported_types_mask) != 0;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Attribute Provider Declaration
 * \{ */

/**
 * In this function all the attribute providers for a curve component are created.
 * Most data in this function is statically allocated, because it does not change over time.
 */
static ComponentAttributeProviders create_attribute_providers_for_curve()
{
  static BuiltinSplineAttributeProvider resolution("resolution",
                                                   CD_PROP_INT32,
                                                   BuiltinAttributeProvider::Writable,
                                                   make_resolution_read_attribute,
                                                   make_resolution_write_attribute);

  static BuiltinSplineAttributeProvider cyclic("cyclic",
                                               CD_PROP_BOOL,
                                               BuiltinAttributeProvider::Writable,
                                               make_cyclic_read_attribute,
                                               make_cyclic_write_attribute);

  static CustomDataAccessInfo spline_custom_data_access = {
      [](void *owner) -> CustomData * {
        CurveEval *curve = static_cast<CurveEval *>(owner);
        return curve ? &curve->attributes.data : nullptr;
      },
      [](const void *owner) -> const CustomData * {
        const CurveEval *curve = static_cast<const CurveEval *>(owner);
        return curve ? &curve->attributes.data : nullptr;
      },
      [](const void *owner) -> int {
        const CurveEval *curve = static_cast<const CurveEval *>(owner);
        return curve->splines().size();
      },
      nullptr};

  static CustomDataAttributeProvider spline_custom_data(ATTR_DOMAIN_CURVE,
                                                        spline_custom_data_access);

  static PositionAttributeProvider position;
  static BezierHandleAttributeProvider handles_start(false);
  static BezierHandleAttributeProvider handles_end(true);

  static BuiltinPointAttributeProvider<int> id(
      "id",
      BuiltinAttributeProvider::Creatable,
      BuiltinAttributeProvider::Deletable,
      [](const Spline &spline) {
        std::optional<GSpan> span = spline.attributes.get_for_read("id");
        return span ? span->typed<int>() : Span<int>();
      },
      [](Spline &spline) {
        std::optional<GMutableSpan> span = spline.attributes.get_for_write("id");
        return span ? span->typed<int>() : MutableSpan<int>();
      },
      {},
      true);

  static BuiltinPointAttributeProvider<float> radius(
      "radius",
      BuiltinAttributeProvider::NonCreatable,
      BuiltinAttributeProvider::NonDeletable,
      [](const Spline &spline) { return spline.radii(); },
      [](Spline &spline) { return spline.radii(); },
      nullptr,
      false);

  static BuiltinPointAttributeProvider<float> tilt(
      "tilt",
      BuiltinAttributeProvider::NonCreatable,
      BuiltinAttributeProvider::NonDeletable,
      [](const Spline &spline) { return spline.tilts(); },
      [](Spline &spline) { return spline.tilts(); },
      [](Spline &spline) { spline.mark_cache_invalid(); },
      false);

  static DynamicPointAttributeProvider point_custom_data;

  return ComponentAttributeProviders(
      {&position, &id, &radius, &tilt, &handles_start, &handles_end, &resolution, &cyclic},
      {&spline_custom_data, &point_custom_data});
}

/** \} */

static AttributeAccessorFunctions get_curve_accessor_functions()
{
  static const ComponentAttributeProviders providers = create_attribute_providers_for_curve();
  AttributeAccessorFunctions fn =
      attribute_accessor_functions::accessor_functions_for_providers<providers>();
  fn.domain_size = [](const void *owner, const eAttrDomain domain) -> int {
    if (owner == nullptr) {
      return 0;
    }
    const CurveEval &curve_eval = *static_cast<const CurveEval *>(owner);
    switch (domain) {
      case ATTR_DOMAIN_POINT:
        return curve_eval.total_control_point_num();
      case ATTR_DOMAIN_CURVE:
        return curve_eval.splines().size();
      default:
        return 0;
    }
  };
  fn.domain_supported = [](const void *UNUSED(owner), const eAttrDomain domain) {
    return ELEM(domain, ATTR_DOMAIN_POINT, ATTR_DOMAIN_CURVE);
  };
  fn.adapt_domain = [](const void *owner,
                       const blender::GVArray &varray,
                       const eAttrDomain from_domain,
                       const eAttrDomain to_domain) -> GVArray {
    if (owner == nullptr) {
      return {};
    }
    const CurveEval &curve_eval = *static_cast<const CurveEval *>(owner);
    return adapt_curve_attribute_domain(curve_eval, varray, from_domain, to_domain);
  };
  return fn;
}

static const AttributeAccessorFunctions &get_curve_accessor_functions_ref()
{
  static const AttributeAccessorFunctions fn = get_curve_accessor_functions();
  return fn;
}

}  // namespace blender::bke

std::optional<blender::bke::AttributeAccessor> CurveComponentLegacy::attributes() const
{
  return blender::bke::AttributeAccessor(curve_, blender::bke::get_curve_accessor_functions_ref());
}

std::optional<blender::bke::MutableAttributeAccessor> CurveComponentLegacy::attributes_for_write()
{
  CurveEval *curve = this->get_for_write();
  return blender::bke::MutableAttributeAccessor(curve,
                                                blender::bke::get_curve_accessor_functions_ref());
}

blender::bke::MutableAttributeAccessor CurveEval::attributes_for_write()
{
  return blender::bke::MutableAttributeAccessor(this,
                                                blender::bke::get_curve_accessor_functions_ref());
}
