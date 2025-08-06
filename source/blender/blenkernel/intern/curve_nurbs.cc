/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BLI_task.hh"

#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"

namespace blender::bke::curves::nurbs {

bool check_valid_num_and_order(const int points_num,
                               const int8_t order,
                               const bool cyclic,
                               const KnotsMode knots_mode)
{
  if (points_num < order) {
    return false;
  }

  if (ELEM(knots_mode, NURBS_KNOT_MODE_BEZIER, NURBS_KNOT_MODE_ENDPOINT_BEZIER)) {
    if (knots_mode == NURBS_KNOT_MODE_BEZIER && points_num <= order) {
      return false;
    }
    return (!cyclic || points_num % (order - 1) == 0);
  }

  return true;
}

static int calc_nonzero_knot_spans(const int points_num,
                                   const KnotsMode mode,
                                   const int8_t order,
                                   const bool cyclic)
{
  const bool is_bezier = ELEM(mode, NURBS_KNOT_MODE_BEZIER, NURBS_KNOT_MODE_ENDPOINT_BEZIER);
  const bool is_end_point = ELEM(mode, NURBS_KNOT_MODE_ENDPOINT, NURBS_KNOT_MODE_ENDPOINT_BEZIER);
  /* Inner knots are always repeated once except on Bezier case. */
  const int repeat_inner = is_bezier ? order - 1 : 1;
  /* For non endpoint Bezier repeated knots are shifted by one. */
  const int knots_before_geometry = order + int(is_bezier && !is_end_point && order > 2);
  const int knots_after_geometry = order - 1 +
                                   (cyclic && mode == NURBS_KNOT_MODE_ENDPOINT ? order - 2 : 0);

  const int knots_total = knots_num(points_num, order, cyclic);
  /* On these knots as parameters actual geometry is generated. */
  const int geometry_knots = knots_total - knots_before_geometry - knots_after_geometry;
  /* `repeat_inner - 1` is added to `ceil`. */
  const int non_zero_knots = (geometry_knots + repeat_inner - 1) / repeat_inner;
  return non_zero_knots;
}

static int count_nonzero_knot_spans(const int points_num,
                                    const int order,
                                    const bool cyclic,
                                    const Span<float> knots)
{
  BLI_assert(points_num > 0);
  const int degree = order - 1;
  int span_num = 0;
  for (const int knot_span : IndexRange::from_begin_end(cyclic ? 0 : degree, points_num)) {
    span_num += (knots[knot_span + 1] - knots[knot_span]) > 0.0f;
  }
  return span_num;
}

int calculate_evaluated_num(const int points_num,
                            const int8_t order,
                            const bool cyclic,
                            const int resolution,
                            const KnotsMode knots_mode,
                            const Span<float> knots)
{
  if (!check_valid_num_and_order(points_num, order, cyclic, knots_mode)) {
    return points_num;
  }
  const int nonzero_span_num = knots_mode == KnotsMode::NURBS_KNOT_MODE_CUSTOM &&
                                       !knots.is_empty() ?
                                   count_nonzero_knot_spans(points_num, order, cyclic, knots) :
                                   calc_nonzero_knot_spans(points_num, knots_mode, order, cyclic);
  return resolution * nonzero_span_num + int(!cyclic);
}

static void copy_custom_knots(const int8_t order,
                              const bool cyclic,
                              const Span<float> custom_knots,
                              MutableSpan<float> knots)
{
  knots.slice(0, custom_knots.size()).copy_from(custom_knots);
  if (cyclic) {
    const float last_knot = custom_knots.last();
    const float shift = last_knot - knots[order - 1];
    const MutableSpan<float> tail = knots.take_back(order - 1);
    for (const int knot : tail.index_range()) {
      tail[knot] = knots[order + knot] + shift;
    }
  }
}

void calculate_knots(const int points_num,
                     const KnotsMode mode,
                     const int8_t order,
                     const bool cyclic,
                     MutableSpan<float> knots)
{
  BLI_assert(knots.size() == knots_num(points_num, order, cyclic));
  UNUSED_VARS_NDEBUG(points_num);

  const bool is_bezier = ELEM(mode, NURBS_KNOT_MODE_BEZIER, NURBS_KNOT_MODE_ENDPOINT_BEZIER);
  const bool is_end_point = ELEM(mode, NURBS_KNOT_MODE_ENDPOINT, NURBS_KNOT_MODE_ENDPOINT_BEZIER);
  /* Inner knots are always repeated once except on Bezier case. */
  const int repeat_inner = is_bezier ? order - 1 : 1;
  /* How many times to repeat 0.0 at the beginning of knot. */
  const int head = is_end_point ? (order - (cyclic ? 1 : 0)) :
                                  (is_bezier ? min_ii(2, repeat_inner) : 1);
  /* Number of knots replicating widths of the starting knots.
   * Covers both Cyclic and EndPoint cases. */
  const int tail = cyclic ? 2 * order - 1 : (is_end_point ? order : 0);

  int r = head;
  float current = 0.0f;

  const int offset = is_end_point && cyclic ? 1 : 0;
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

void load_curve_knots(const KnotsMode mode,
                      const int points_num,
                      const int8_t order,
                      const bool cyclic,
                      const IndexRange curve_knots,
                      const Span<float> custom_knots,
                      MutableSpan<float> knots)
{
  if (mode == NURBS_KNOT_MODE_CUSTOM) {
    BLI_assert(!custom_knots.is_empty());
    BLI_assert(!curve_knots.is_empty());
    copy_custom_knots(order, cyclic, custom_knots.slice(curve_knots), knots);
  }
  else {
    calculate_knots(points_num, mode, order, cyclic, knots);
  }
}

Vector<int> calculate_multiplicity_sequence(const Span<float> knots)
{
  Vector<int> multiplicity;
  multiplicity.reserve(knots.size());

  int m = 1;
  for (const int64_t i : knots.index_range().drop_front(1)) {
    /* Only consider multiplicity for exact matching values. */
    if (knots[i - 1] == knots[i]) {
      m++;
    }
    else {
      multiplicity.append(m);
      m = 1;
    }
  }
  multiplicity.append(m);
  return multiplicity;
}

static void calculate_basis_for_point(const float parameter,
                                      const int wrapped_points_num,
                                      const int degree,
                                      const Span<float> knots,
                                      MutableSpan<float> r_weights,
                                      int &r_start_index)
{
  const int order = degree + 1;

  int start = 0;
  int end = 0;
  for (const int i : IndexRange(wrapped_points_num + degree)) {
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
      end = wrapped_points_num + degree - i_order;
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

void calculate_basis_cache(const int points_num,
                           const int evaluated_num,
                           const int8_t order,
                           const int resolution,
                           const bool cyclic,
                           const Span<float> knots,
                           BasisCache &basis_cache)
{
  BLI_assert(points_num > 0);

  const int8_t degree = order - 1;

  basis_cache.weights.resize(evaluated_num * order);
  basis_cache.start_indices.resize(evaluated_num);

  if (evaluated_num == 0) {
    return;
  }

  MutableSpan<float> basis_weights(basis_cache.weights);
  MutableSpan<int> basis_start_indices(basis_cache.start_indices);

  const int wrapped_points_num = control_points_num(points_num, order, cyclic);

  int eval_point = 0;

  for (const int knot_span : IndexRange::from_begin_end(degree, wrapped_points_num)) {
    const float start = knots[knot_span];
    const float end = knots[knot_span + 1];
    if (start == end) {
      continue;
    }
    const float step_width = (end - start) / resolution;
    for (const int step : IndexRange::from_begin_size(0, resolution)) {
      const float parameter = start + step * step_width;
      calculate_basis_for_point(parameter,
                                wrapped_points_num,
                                degree,
                                knots,
                                basis_weights.slice(eval_point * order, order),
                                basis_start_indices[eval_point]);
      eval_point++;
    }
  }
  if (!cyclic) {
    calculate_basis_for_point(knots[wrapped_points_num],
                              wrapped_points_num,
                              degree,
                              knots,
                              basis_weights.slice(eval_point * order, order),
                              basis_start_indices[eval_point]);
  }
}

template<typename T>
static void interpolate_to_evaluated(const BasisCache &basis_cache,
                                     const int8_t order,
                                     const Span<T> src,
                                     MutableSpan<T> dst)
{
  attribute_math::DefaultMixer<T> mixer{dst};

  threading::parallel_for(dst.index_range(), 128, [&](const IndexRange range) {
    for (const int i : range) {
      Span<float> point_weights = basis_cache.weights.as_span().slice(i * order, order);
      for (const int j : point_weights.index_range()) {
        const int point_index = (basis_cache.start_indices[i] + j) % src.size();
        mixer.mix_in(i, src[point_index], point_weights[j]);
      }
    }
    mixer.finalize(range);
  });
}

template<typename T>
static void interpolate_to_evaluated_rational(const BasisCache &basis_cache,
                                              const int8_t order,
                                              const Span<float> control_weights,
                                              const Span<T> src,
                                              MutableSpan<T> dst)
{
  attribute_math::DefaultMixer<T> mixer{dst};

  threading::parallel_for(dst.index_range(), 128, [&](const IndexRange range) {
    for (const int i : range) {
      Span<float> point_weights = basis_cache.weights.as_span().slice(i * order, order);

      for (const int j : point_weights.index_range()) {
        const int point_index = (basis_cache.start_indices[i] + j) % src.size();
        const float weight = point_weights[j] * control_weights[point_index];
        mixer.mix_in(i, src[point_index], weight);
      }
    }
    mixer.finalize(range);
  });
}

void interpolate_to_evaluated(const BasisCache &basis_cache,
                              const int8_t order,
                              const Span<float> control_weights,
                              const GSpan src,
                              GMutableSpan dst)
{
  if (basis_cache.invalid) {
    dst.copy_from(src);
    return;
  }

  BLI_assert(dst.size() == basis_cache.start_indices.size());
  attribute_math::convert_to_static_type(src.type(), [&](auto dummy) {
    using T = decltype(dummy);
    if constexpr (!std::is_void_v<attribute_math::DefaultMixer<T>>) {
      if (control_weights.is_empty()) {
        interpolate_to_evaluated(basis_cache, order, src.typed<T>(), dst.typed<T>());
      }
      else {
        interpolate_to_evaluated_rational(
            basis_cache, order, control_weights, src.typed<T>(), dst.typed<T>());
      }
    }
  });
}

}  // namespace blender::bke::curves::nurbs
