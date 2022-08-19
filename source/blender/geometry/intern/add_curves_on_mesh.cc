/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_length_parameterize.hh"
#include "BLI_task.hh"

#include "BKE_attribute_math.hh"
#include "BKE_mesh_sample.hh"

#include "GEO_add_curves_on_mesh.hh"
#include "GEO_reverse_uv_sampler.hh"

/**
 * The code below uses a suffix naming convention to indicate the coordinate space:
 * cu: Local space of the curves object that is being edited.
 * su: Local space of the surface object.
 */

namespace blender::geometry {

using bke::CurvesGeometry;

struct NeighborCurve {
  /* Curve index of the neighbor. */
  int index;
  /* The weights of all neighbors of a new curve add up to 1. */
  float weight;
};

static constexpr int max_neighbors = 5;
using NeighborCurves = Vector<NeighborCurve, max_neighbors>;

float3 compute_surface_point_normal(const MLoopTri &looptri,
                                    const float3 &bary_coord,
                                    const Span<float3> corner_normals)
{
  const int l0 = looptri.tri[0];
  const int l1 = looptri.tri[1];
  const int l2 = looptri.tri[2];

  const float3 &l0_normal = corner_normals[l0];
  const float3 &l1_normal = corner_normals[l1];
  const float3 &l2_normal = corner_normals[l2];

  const float3 normal = math::normalize(
      attribute_math::mix3(bary_coord, l0_normal, l1_normal, l2_normal));
  return normal;
}

static void initialize_straight_curve_positions(const float3 &p1,
                                                const float3 &p2,
                                                MutableSpan<float3> r_positions)
{
  const float step = 1.0f / (float)(r_positions.size() - 1);
  for (const int i : r_positions.index_range()) {
    r_positions[i] = math::interpolate(p1, p2, i * step);
  }
}

static Array<NeighborCurves> find_curve_neighbors(const Span<float3> root_positions,
                                                  const KDTree_3d &old_roots_kdtree)
{
  const int tot_added_curves = root_positions.size();
  Array<NeighborCurves> neighbors_per_curve(tot_added_curves);
  threading::parallel_for(IndexRange(tot_added_curves), 128, [&](const IndexRange range) {
    for (const int i : range) {
      const float3 root = root_positions[i];
      std::array<KDTreeNearest_3d, max_neighbors> nearest_n;
      const int found_neighbors = BLI_kdtree_3d_find_nearest_n(
          &old_roots_kdtree, root, nearest_n.data(), max_neighbors);
      float tot_weight = 0.0f;
      for (const int neighbor_i : IndexRange(found_neighbors)) {
        KDTreeNearest_3d &nearest = nearest_n[neighbor_i];
        const float weight = 1.0f / std::max(nearest.dist, 0.00001f);
        tot_weight += weight;
        neighbors_per_curve[i].append({nearest.index, weight});
      }
      /* Normalize weights. */
      for (NeighborCurve &neighbor : neighbors_per_curve[i]) {
        neighbor.weight /= tot_weight;
      }
    }
  });
  return neighbors_per_curve;
}

template<typename T, typename GetValueF>
void interpolate_from_neighbors(const Span<NeighborCurves> neighbors_per_curve,
                                const T &fallback,
                                const GetValueF &get_value_from_neighbor,
                                MutableSpan<T> r_interpolated_values)
{
  attribute_math::DefaultMixer<T> mixer{r_interpolated_values};
  threading::parallel_for(r_interpolated_values.index_range(), 512, [&](const IndexRange range) {
    for (const int i : range) {
      const NeighborCurves &neighbors = neighbors_per_curve[i];
      if (neighbors.is_empty()) {
        mixer.mix_in(i, fallback, 1.0f);
      }
      else {
        for (const NeighborCurve &neighbor : neighbors) {
          const T neighbor_value = get_value_from_neighbor(neighbor.index);
          mixer.mix_in(i, neighbor_value, neighbor.weight);
        }
      }
    }
    mixer.finalize(range);
  });
}

static void interpolate_position_without_interpolation(
    CurvesGeometry &curves,
    const int old_curves_num,
    const Span<float3> root_positions_cu,
    const Span<float> new_lengths_cu,
    const Span<float3> new_normals_su,
    const float4x4 &surface_to_curves_normal_mat)
{
  const int added_curves_num = root_positions_cu.size();
  MutableSpan<float3> positions_cu = curves.positions_for_write();
  threading::parallel_for(IndexRange(added_curves_num), 256, [&](const IndexRange range) {
    for (const int i : range) {
      const int curve_i = old_curves_num + i;
      const IndexRange points = curves.points_for_curve(curve_i);
      const float3 &root_cu = root_positions_cu[i];
      const float length = new_lengths_cu[i];
      const float3 &normal_su = new_normals_su[i];
      const float3 normal_cu = math::normalize(surface_to_curves_normal_mat * normal_su);
      const float3 tip_cu = root_cu + length * normal_cu;

      initialize_straight_curve_positions(root_cu, tip_cu, positions_cu.slice(points));
    }
  });
}

static void interpolate_position_with_interpolation(CurvesGeometry &curves,
                                                    const Span<float3> root_positions_cu,
                                                    const Span<NeighborCurves> neighbors_per_curve,
                                                    const int old_curves_num,
                                                    const Span<float> new_lengths_cu,
                                                    const Span<float3> new_normals_su,
                                                    const bke::CurvesSurfaceTransforms &transforms,
                                                    const ReverseUVSampler &reverse_uv_sampler,
                                                    const Span<float3> corner_normals_su)
{
  MutableSpan<float3> positions_cu = curves.positions_for_write();
  const int added_curves_num = root_positions_cu.size();

  const Span<float2> uv_coords = curves.surface_uv_coords();

  threading::parallel_for(IndexRange(added_curves_num), 256, [&](const IndexRange range) {
    for (const int added_curve_i : range) {
      const NeighborCurves &neighbors = neighbors_per_curve[added_curve_i];
      const int curve_i = old_curves_num + added_curve_i;
      const IndexRange points = curves.points_for_curve(curve_i);

      const float length_cu = new_lengths_cu[added_curve_i];
      const float3 &normal_su = new_normals_su[added_curve_i];
      const float3 normal_cu = math::normalize(transforms.surface_to_curves_normal * normal_su);

      const float3 &root_cu = root_positions_cu[added_curve_i];

      if (neighbors.is_empty()) {
        /* If there are no neighbors, just make a straight line. */
        const float3 tip_cu = root_cu + length_cu * normal_cu;
        initialize_straight_curve_positions(root_cu, tip_cu, positions_cu.slice(points));
        continue;
      }

      positions_cu.slice(points).fill(root_cu);

      for (const NeighborCurve &neighbor : neighbors) {
        const int neighbor_curve_i = neighbor.index;
        const float2 neighbor_uv = uv_coords[neighbor_curve_i];
        const ReverseUVSampler::Result result = reverse_uv_sampler.sample(neighbor_uv);
        if (result.type != ReverseUVSampler::ResultType::Ok) {
          continue;
        }

        const float3 neighbor_normal_su = compute_surface_point_normal(
            *result.looptri, result.bary_weights, corner_normals_su);
        const float3 neighbor_normal_cu = math::normalize(transforms.surface_to_curves_normal *
                                                          neighbor_normal_su);

        /* The rotation matrix used to transform relative coordinates of the neighbor curve
         * to the new curve. */
        float normal_rotation_cu[3][3];
        rotation_between_vecs_to_mat3(normal_rotation_cu, neighbor_normal_cu, normal_cu);

        const IndexRange neighbor_points = curves.points_for_curve(neighbor_curve_i);
        const float3 &neighbor_root_cu = positions_cu[neighbor_points[0]];

        /* Sample the positions on neighbors and mix them into the final positions of the curve.
         * Resampling is necessary if the length of the new curve does not match the length of the
         * neighbors or the number of handle points is different.
         *
         * TODO: The lengths can be cached so they aren't recomputed if a curve is a neighbor for
         * multiple new curves. Also, allocations could be avoided by reusing some arrays. */

        const Span<float3> neighbor_positions_cu = positions_cu.slice(neighbor_points);
        if (neighbor_positions_cu.size() == 1) {
          /* Skip interpolating positions from neighbors with only one point. */
          continue;
        }
        Array<float, 32> lengths(length_parameterize::segments_num(neighbor_points.size(), false));
        length_parameterize::accumulate_lengths<float3>(neighbor_positions_cu, false, lengths);
        const float neighbor_length_cu = lengths.last();

        Array<float, 32> sample_lengths(points.size());
        const float length_factor = std::min(1.0f, length_cu / neighbor_length_cu);
        const float resample_factor = (1.0f / (points.size() - 1.0f)) * length_factor;
        for (const int i : sample_lengths.index_range()) {
          sample_lengths[i] = i * resample_factor * neighbor_length_cu;
        }

        Array<int, 32> indices(points.size());
        Array<float, 32> factors(points.size());
        length_parameterize::sample_at_lengths(lengths, sample_lengths, indices, factors);

        for (const int i : IndexRange(points.size())) {
          const float3 sample_cu = math::interpolate(neighbor_positions_cu[indices[i]],
                                                     neighbor_positions_cu[indices[i] + 1],
                                                     factors[i]);
          const float3 relative_to_root_cu = sample_cu - neighbor_root_cu;
          float3 rotated_relative_coord = relative_to_root_cu;
          mul_m3_v3(normal_rotation_cu, rotated_relative_coord);
          positions_cu[points[i]] += neighbor.weight * rotated_relative_coord;
        }
      }
    }
  });
}

AddCurvesOnMeshOutputs add_curves_on_mesh(CurvesGeometry &curves,
                                          const AddCurvesOnMeshInputs &inputs)
{
  AddCurvesOnMeshOutputs outputs;

  const bool use_interpolation = inputs.interpolate_length || inputs.interpolate_point_count ||
                                 inputs.interpolate_shape;

  Vector<float3> root_positions_cu;
  Vector<float3> bary_coords;
  Vector<const MLoopTri *> looptris;
  Vector<float2> used_uvs;

  /* Find faces that the passed in uvs belong to. */
  for (const int i : inputs.uvs.index_range()) {
    const float2 &uv = inputs.uvs[i];
    const ReverseUVSampler::Result result = inputs.reverse_uv_sampler->sample(uv);
    if (result.type != ReverseUVSampler::ResultType::Ok) {
      outputs.uv_error = true;
      continue;
    }
    const MLoopTri &looptri = *result.looptri;
    bary_coords.append(result.bary_weights);
    looptris.append(&looptri);
    const float3 root_position_su = attribute_math::mix3<float3>(
        result.bary_weights,
        inputs.surface->mvert[inputs.surface->mloop[looptri.tri[0]].v].co,
        inputs.surface->mvert[inputs.surface->mloop[looptri.tri[1]].v].co,
        inputs.surface->mvert[inputs.surface->mloop[looptri.tri[2]].v].co);
    root_positions_cu.append(inputs.transforms->surface_to_curves * root_position_su);
    used_uvs.append(uv);
  }

  Array<NeighborCurves> neighbors_per_curve;
  if (use_interpolation) {
    BLI_assert(inputs.old_roots_kdtree != nullptr);
    neighbors_per_curve = find_curve_neighbors(root_positions_cu, *inputs.old_roots_kdtree);
  }

  const int added_curves_num = root_positions_cu.size();
  const int old_points_num = curves.points_num();
  const int old_curves_num = curves.curves_num();
  const int new_curves_num = old_curves_num + added_curves_num;

  /* Grow number of curves first, so that the offsets array can be filled. */
  curves.resize(old_points_num, new_curves_num);

  /* Compute new curve offsets. */
  MutableSpan<int> curve_offsets = curves.offsets_for_write();
  MutableSpan<int> new_point_counts_per_curve = curve_offsets.take_back(added_curves_num);
  if (inputs.interpolate_point_count) {
    interpolate_from_neighbors<int>(
        neighbors_per_curve,
        inputs.fallback_point_count,
        [&](const int curve_i) { return curves.points_for_curve(curve_i).size(); },
        new_point_counts_per_curve);
  }
  else {
    new_point_counts_per_curve.fill(inputs.fallback_point_count);
  }
  for (const int i : IndexRange(added_curves_num)) {
    curve_offsets[old_curves_num + i + 1] += curve_offsets[old_curves_num + i];
  }

  const int new_points_num = curves.offsets().last();
  curves.resize(new_points_num, new_curves_num);
  MutableSpan<float3> positions_cu = curves.positions_for_write();

  /* Initialize attachment information. */
  MutableSpan<float2> surface_uv_coords = curves.surface_uv_coords_for_write();
  surface_uv_coords.take_back(added_curves_num).copy_from(used_uvs);

  /* Determine length of new curves. */
  Array<float> new_lengths_cu(added_curves_num);
  if (inputs.interpolate_length) {
    interpolate_from_neighbors<float>(
        neighbors_per_curve,
        inputs.fallback_curve_length,
        [&](const int curve_i) {
          const IndexRange points = curves.points_for_curve(curve_i);
          float length = 0.0f;
          for (const int segment_i : points.drop_back(1)) {
            const float3 &p1 = positions_cu[segment_i];
            const float3 &p2 = positions_cu[segment_i + 1];
            length += math::distance(p1, p2);
          }
          return length;
        },
        new_lengths_cu);
  }
  else {
    new_lengths_cu.fill(inputs.fallback_curve_length);
  }

  /* Find surface normal at root points. */
  Array<float3> new_normals_su(added_curves_num);
  threading::parallel_for(IndexRange(added_curves_num), 256, [&](const IndexRange range) {
    for (const int i : range) {
      new_normals_su[i] = compute_surface_point_normal(
          *looptris[i], bary_coords[i], inputs.corner_normals_su);
    }
  });

  /* Update selection arrays when available. */
  const VArray<float> points_selection = curves.selection_point_float();
  if (points_selection.is_span()) {
    MutableSpan<float> points_selection_span = curves.selection_point_float_for_write();
    points_selection_span.drop_front(old_points_num).fill(1.0f);
  }
  const VArray<float> curves_selection = curves.selection_curve_float();
  if (curves_selection.is_span()) {
    MutableSpan<float> curves_selection_span = curves.selection_curve_float_for_write();
    curves_selection_span.drop_front(old_curves_num).fill(1.0f);
  }

  /* Initialize position attribute. */
  if (inputs.interpolate_shape) {
    interpolate_position_with_interpolation(curves,
                                            root_positions_cu,
                                            neighbors_per_curve,
                                            old_curves_num,
                                            new_lengths_cu,
                                            new_normals_su,
                                            *inputs.transforms,
                                            *inputs.reverse_uv_sampler,
                                            inputs.corner_normals_su);
  }
  else {
    interpolate_position_without_interpolation(curves,
                                               old_curves_num,
                                               root_positions_cu,
                                               new_lengths_cu,
                                               new_normals_su,
                                               inputs.transforms->surface_to_curves_normal);
  }

  /* Set curve types. */
  MutableSpan<int8_t> types_span = curves.curve_types_for_write();
  types_span.drop_front(old_curves_num).fill(CURVE_TYPE_CATMULL_ROM);
  curves.update_curve_types();

  return outputs;
}

}  // namespace blender::geometry
