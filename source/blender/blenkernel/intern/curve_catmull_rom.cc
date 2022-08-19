/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"

namespace blender::bke::curves::catmull_rom {

int calculate_evaluated_num(const int points_num, const bool cyclic, const int resolution)
{
  const int eval_num = resolution * segments_num(points_num, cyclic);
  /* If the curve isn't cyclic, one last point is added to the final point. */
  return cyclic ? eval_num : eval_num + 1;
}

/* Adapted from Cycles #catmull_rom_basis_eval function. */
template<typename T>
static T calculate_basis(const T &a, const T &b, const T &c, const T &d, const float parameter)
{
  const float t = parameter;
  const float s = 1.0f - parameter;
  const float n0 = -t * s * s;
  const float n1 = 2.0f + t * t * (3.0f * t - 5.0f);
  const float n2 = 2.0f + s * s * (3.0f * s - 5.0f);
  const float n3 = -s * t * t;
  return 0.5f * (a * n0 + b * n1 + c * n2 + d * n3);
}

template<typename T>
static void evaluate_segment(const T &a, const T &b, const T &c, const T &d, MutableSpan<T> dst)
{
  const float step = 1.0f / dst.size();
  dst.first() = b;
  for (const int i : dst.index_range().drop_front(1)) {
    dst[i] = calculate_basis<T>(a, b, c, d, i * step);
  }
}

/**
 * \param range_fn: Returns an index range describing where in the #dst span each segment should be
 * evaluated to, and how many points to add to it. This is used to avoid the need to allocate an
 * actual offsets array in typical evaluation use cases where the resolution is per-curve.
 */
template<typename T, typename RangeForSegmentFn>
static void interpolate_to_evaluated(const Span<T> src,
                                     const bool cyclic,
                                     const RangeForSegmentFn &range_fn,
                                     MutableSpan<T> dst)

{
  /* - First deal with one and two point curves need special attention.
   * - Then evaluate the first and last segment(s) whose control points need to wrap around
   *   to the other side of the source array.
   * - Finally evaluate all of the segments in the middle in parallel. */

  if (src.size() == 1) {
    dst.first() = src.first();
    return;
  }

  const IndexRange first = range_fn(0);

  if (src.size() == 2) {
    evaluate_segment(src.first(), src.first(), src.last(), src.last(), dst.slice(first));
    if (cyclic) {
      const IndexRange last = range_fn(1);
      evaluate_segment(src.last(), src.last(), src.first(), src.first(), dst.slice(last));
    }
    else {
      dst.last() = src.last();
    }
    return;
  }

  const IndexRange second_to_last = range_fn(src.index_range().last(1));
  const IndexRange last = range_fn(src.index_range().last());
  if (cyclic) {
    evaluate_segment(src.last(), src[0], src[1], src[2], dst.slice(first));
    evaluate_segment(src.last(2), src.last(1), src.last(), src.first(), dst.slice(second_to_last));
    evaluate_segment(src.last(1), src.last(), src[0], src[1], dst.slice(last));
  }
  else {
    evaluate_segment(src[0], src[0], src[1], src[2], dst.slice(first));
    evaluate_segment(src.last(2), src.last(1), src.last(), src.last(), dst.slice(second_to_last));
    /* For non-cyclic curves, the last segment should always just have a single point. We could
     * assert that the size of the provided range is 1 here, but that would require specializing
     * the #range_fn implementation for the last point, which may have a performance cost. */
    dst.last() = src.last();
  }

  /* Evaluate every segment that isn't the first or last. */
  const IndexRange inner_range = src.index_range().drop_back(2).drop_front(1);
  threading::parallel_for(inner_range, 512, [&](IndexRange range) {
    for (const int i : range) {
      const IndexRange segment = range_fn(i);
      evaluate_segment(src[i - 1], src[i], src[i + 1], src[i + 2], dst.slice(segment));
    }
  });
}

template<typename T>
static void interpolate_to_evaluated(const Span<T> src,
                                     const bool cyclic,
                                     const int resolution,
                                     MutableSpan<T> dst)

{
  BLI_assert(dst.size() == calculate_evaluated_num(src.size(), cyclic, resolution));
  interpolate_to_evaluated(
      src,
      cyclic,
      [resolution](const int segment_i) -> IndexRange {
        return {segment_i * resolution, resolution};
      },
      dst);
}

template<typename T>
static void interpolate_to_evaluated(const Span<T> src,
                                     const bool cyclic,
                                     const Span<int> evaluated_offsets,
                                     MutableSpan<T> dst)

{
  interpolate_to_evaluated(
      src,
      cyclic,
      [evaluated_offsets](const int segment_i) -> IndexRange {
        return bke::offsets_to_range(evaluated_offsets, segment_i);
      },
      dst);
}

void interpolate_to_evaluated(const GSpan src,
                              const bool cyclic,
                              const int resolution,
                              GMutableSpan dst)
{
  attribute_math::convert_to_static_type(src.type(), [&](auto dummy) {
    using T = decltype(dummy);
    /* TODO: Use DefaultMixer or other generic mixing in the basis evaluation function to simplify
     * supporting more types. */
    if constexpr (is_same_any_v<T, float, float2, float3, float4, int8_t, int, int64_t>) {
      interpolate_to_evaluated(src.typed<T>(), cyclic, resolution, dst.typed<T>());
    }
  });
}

void interpolate_to_evaluated(const GSpan src,
                              const bool cyclic,
                              const Span<int> evaluated_offsets,
                              GMutableSpan dst)
{
  attribute_math::convert_to_static_type(src.type(), [&](auto dummy) {
    using T = decltype(dummy);
    /* TODO: Use DefaultMixer or other generic mixing in the basis evaluation function to simplify
     * supporting more types. */
    if constexpr (is_same_any_v<T, float, float2, float3, float4, int8_t, int, int64_t>) {
      interpolate_to_evaluated(src.typed<T>(), cyclic, evaluated_offsets, dst.typed<T>());
    }
  });
}

}  // namespace blender::bke::curves::catmull_rom
