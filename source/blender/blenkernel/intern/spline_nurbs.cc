/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_span.hh"
#include "BLI_virtual_array.hh"

#include "BKE_attribute_math.hh"
#include "BKE_spline.hh"

using blender::Array;
using blender::float3;
using blender::GVArray;
using blender::IndexRange;
using blender::MutableSpan;
using blender::Span;
using blender::VArray;

void NURBSpline::copy_settings(Spline &dst) const
{
  NURBSpline &nurbs = static_cast<NURBSpline &>(dst);
  nurbs.knots_mode = knots_mode;
  nurbs.resolution_ = resolution_;
  nurbs.order_ = order_;
}

void NURBSpline::copy_data(Spline &dst) const
{
  NURBSpline &nurbs = static_cast<NURBSpline &>(dst);
  nurbs.positions_ = positions_;
  nurbs.weights_ = weights_;
  nurbs.knots_ = knots_;
  nurbs.knots_dirty_ = knots_dirty_;
  nurbs.radii_ = radii_;
  nurbs.tilts_ = tilts_;
}

int NURBSpline::size() const
{
  const int size = positions_.size();
  BLI_assert(size == radii_.size());
  BLI_assert(size == tilts_.size());
  BLI_assert(size == weights_.size());
  return size;
}

int NURBSpline::resolution() const
{
  return resolution_;
}

void NURBSpline::set_resolution(const int value)
{
  BLI_assert(value > 0);
  resolution_ = value;
  this->mark_cache_invalid();
}

uint8_t NURBSpline::order() const
{
  return order_;
}

void NURBSpline::set_order(const uint8_t value)
{
  BLI_assert(value >= 2 && value <= 6);
  order_ = value;
  this->mark_cache_invalid();
}

void NURBSpline::resize(const int size)
{
  positions_.resize(size);
  radii_.resize(size);
  tilts_.resize(size);
  weights_.resize(size);
  this->mark_cache_invalid();
  attributes.reallocate(size);
}

MutableSpan<float3> NURBSpline::positions()
{
  return positions_;
}
Span<float3> NURBSpline::positions() const
{
  return positions_;
}
MutableSpan<float> NURBSpline::radii()
{
  return radii_;
}
Span<float> NURBSpline::radii() const
{
  return radii_;
}
MutableSpan<float> NURBSpline::tilts()
{
  return tilts_;
}
Span<float> NURBSpline::tilts() const
{
  return tilts_;
}
MutableSpan<float> NURBSpline::weights()
{
  return weights_;
}
Span<float> NURBSpline::weights() const
{
  return weights_;
}

void NURBSpline::reverse_impl()
{
  this->weights().reverse();
}

void NURBSpline::mark_cache_invalid()
{
  basis_cache_dirty_ = true;
  position_cache_dirty_ = true;
  tangent_cache_dirty_ = true;
  normal_cache_dirty_ = true;
  length_cache_dirty_ = true;
}

int NURBSpline::evaluated_points_num() const
{
  if (!this->check_valid_num_and_order()) {
    return 0;
  }
  return resolution_ * this->segments_num();
}

void NURBSpline::correct_end_tangents() const
{
}

bool NURBSpline::check_valid_num_and_order() const
{
  if (this->size() < order_) {
    return false;
  }

  if (ELEM(this->knots_mode, NURBS_KNOT_MODE_BEZIER, NURBS_KNOT_MODE_ENDPOINT_BEZIER)) {
    if (this->knots_mode == NURBS_KNOT_MODE_BEZIER && this->size() <= order_) {
      return false;
    }
    return (!is_cyclic_ || this->size() % (order_ - 1) == 0);
  }

  return true;
}

int NURBSpline::knots_num() const
{
  const int num = this->size() + order_;
  return is_cyclic_ ? num + order_ - 1 : num;
}

void NURBSpline::calculate_knots() const
{
  const KnotsMode mode = this->knots_mode;
  const int order = order_;
  const bool is_bezier = ELEM(mode, NURBS_KNOT_MODE_BEZIER, NURBS_KNOT_MODE_ENDPOINT_BEZIER);
  const bool is_end_point = ELEM(mode, NURBS_KNOT_MODE_ENDPOINT, NURBS_KNOT_MODE_ENDPOINT_BEZIER);
  /* Inner knots are always repeated once except on Bezier case. */
  const int repeat_inner = is_bezier ? order - 1 : 1;
  /* How many times to repeat 0.0 at the beginning of knot. */
  const int head = is_end_point ? (order - (is_cyclic_ ? 1 : 0)) :
                                  (is_bezier ? min_ii(2, repeat_inner) : 1);
  /* Number of knots replicating widths of the starting knots.
   * Covers both Cyclic and EndPoint cases. */
  const int tail = is_cyclic_ ? 2 * order - 1 : (is_end_point ? order : 0);

  knots_.resize(this->knots_num());
  MutableSpan<float> knots = knots_;

  int r = head;
  float current = 0.0f;

  const int offset = is_end_point && is_cyclic_ ? 1 : 0;
  if (offset) {
    knots[0] = current;
    current += 1.0f;
  }

  for (const int i : IndexRange(offset, knots.size() - offset - tail)) {
    knots[i] = current;
    r--;
    if (r == 0) {
      current += 1.0;
      r = repeat_inner;
    }
  }

  const int tail_index = knots.size() - tail;
  for (const int i : IndexRange(tail)) {
    knots[tail_index + i] = current + (knots[i] - knots[0]);
  }
}

Span<float> NURBSpline::knots() const
{
  if (!knots_dirty_) {
    BLI_assert(knots_.size() == this->knots_num());
    return knots_;
  }

  std::lock_guard lock{knots_mutex_};
  if (!knots_dirty_) {
    BLI_assert(knots_.size() == this->knots_num());
    return knots_;
  }

  this->calculate_knots();

  knots_dirty_ = false;

  return knots_;
}

static void calculate_basis_for_point(const float parameter,
                                      const int num,
                                      const int degree,
                                      const Span<float> knots,
                                      MutableSpan<float> r_weights,
                                      int &r_start_index)
{
  const int order = degree + 1;

  int start = 0;
  int end = 0;
  for (const int i : IndexRange(num + degree)) {
    const bool knots_equal = knots[i] == knots[i + 1];
    if (knots_equal || parameter < knots[i] || parameter > knots[i + 1]) {
      continue;
    }

    start = std::max(i - degree, 0);
    end = i;
    break;
  }

  Array<float, 12> buffer(order * 2, 0.0f);

  buffer[end - start] = 1.0f;

  for (const int i_order : IndexRange(2, degree)) {
    if (end + i_order >= knots.size()) {
      end = num + degree - i_order;
    }
    for (const int i : IndexRange(end - start + 1)) {
      const int knot_index = start + i;

      float new_basis = 0.0f;
      if (buffer[i] != 0.0f) {
        new_basis += ((parameter - knots[knot_index]) * buffer[i]) /
                     (knots[knot_index + i_order - 1] - knots[knot_index]);
      }

      if (buffer[i + 1] != 0.0f) {
        new_basis += ((knots[knot_index + i_order] - parameter) * buffer[i + 1]) /
                     (knots[knot_index + i_order] - knots[knot_index + 1]);
      }

      buffer[i] = new_basis;
    }
  }

  buffer.as_mutable_span().drop_front(end - start + 1).fill(0.0f);
  r_weights.copy_from(buffer.as_span().take_front(order));
  r_start_index = start;
}

const NURBSpline::BasisCache &NURBSpline::calculate_basis_cache() const
{
  if (!basis_cache_dirty_) {
    return basis_cache_;
  }

  std::lock_guard lock{basis_cache_mutex_};
  if (!basis_cache_dirty_) {
    return basis_cache_;
  }

  const int num = this->size();
  const int eval_num = this->evaluated_points_num();

  const int order = this->order();
  const int degree = order - 1;

  basis_cache_.weights.resize(eval_num * order);
  basis_cache_.start_indices.resize(eval_num);

  if (eval_num == 0) {
    return basis_cache_;
  }

  MutableSpan<float> basis_weights(basis_cache_.weights);
  MutableSpan<int> basis_start_indices(basis_cache_.start_indices);

  const Span<float> control_weights = this->weights();
  const Span<float> knots = this->knots();

  const int last_control_point_index = is_cyclic_ ? num + degree : num;

  const float start = knots[degree];
  const float end = knots[last_control_point_index];
  const float step = (end - start) / this->evaluated_edges_num();
  for (const int i : IndexRange(eval_num)) {
    /* Clamp parameter due to floating point inaccuracy. */
    const float parameter = std::clamp(start + step * i, knots[0], knots[num + degree]);

    MutableSpan<float> point_weights = basis_weights.slice(i * order, order);

    calculate_basis_for_point(
        parameter, last_control_point_index, degree, knots, point_weights, basis_start_indices[i]);

    for (const int j : point_weights.index_range()) {
      const int point_index = (basis_start_indices[i] + j) % num;
      point_weights[j] *= control_weights[point_index];
    }
  }

  basis_cache_dirty_ = false;
  return basis_cache_;
}

template<typename T>
void interpolate_to_evaluated_impl(const NURBSpline::BasisCache &basis_cache,
                                   const int order,
                                   const blender::VArray<T> &src,
                                   MutableSpan<T> dst)
{
  const int num = src.size();
  blender::attribute_math::DefaultMixer<T> mixer(dst);

  for (const int i : dst.index_range()) {
    Span<float> point_weights = basis_cache.weights.as_span().slice(i * order, order);
    const int start_index = basis_cache.start_indices[i];

    for (const int j : point_weights.index_range()) {
      const int point_index = (start_index + j) % num;
      mixer.mix_in(i, src[point_index], point_weights[j]);
    }
  }

  mixer.finalize();
}

GVArray NURBSpline::interpolate_to_evaluated(const GVArray &src) const
{
  BLI_assert(src.size() == this->size());

  if (src.is_single()) {
    return src;
  }

  const BasisCache &basis_cache = this->calculate_basis_cache();

  GVArray new_varray;
  blender::attribute_math::convert_to_static_type(src.type(), [&](auto dummy) {
    using T = decltype(dummy);
    if constexpr (!std::is_void_v<blender::attribute_math::DefaultMixer<T>>) {
      Array<T> values(this->evaluated_points_num());
      interpolate_to_evaluated_impl<T>(basis_cache, this->order(), src.typed<T>(), values);
      new_varray = VArray<T>::ForContainer(std::move(values));
    }
  });

  return new_varray;
}

Span<float3> NURBSpline::evaluated_positions() const
{
  if (!position_cache_dirty_) {
    return evaluated_position_cache_;
  }

  std::lock_guard lock{position_cache_mutex_};
  if (!position_cache_dirty_) {
    return evaluated_position_cache_;
  }

  const int eval_num = this->evaluated_points_num();
  evaluated_position_cache_.resize(eval_num);

  /* TODO: Avoid copying the evaluated data from the temporary array. */
  VArray<float3> evaluated = Spline::interpolate_to_evaluated(positions_.as_span());
  evaluated.materialize(evaluated_position_cache_);

  position_cache_dirty_ = false;
  return evaluated_position_cache_;
}
