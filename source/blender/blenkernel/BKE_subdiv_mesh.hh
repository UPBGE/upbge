/* SPDX-FileCopyrightText: 2018 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"

struct Mesh;

namespace blender::bke::subdiv {

struct Subdiv;

struct ToMeshSettings {
  /**
   * Resolution at which regular PTEX (created for quad face) are being
   * evaluated. This defines how many vertices final mesh will have: every
   * regular PTEX has resolution^2 vertices. Special (irregular, or PTEX
   * created for a corner of non-quad face) will have resolution of
   * `resolution - 1`.
   */
  int resolution;
  /** When true, only edges emitted from coarse ones will be displayed. */
  bool use_optimal_display;
};

/** Create real hi-res mesh from subdivision, all geometry is "real". */
Mesh *subdiv_to_mesh(Subdiv *subdiv, const ToMeshSettings *settings, const Mesh *coarse_mesh);

/**
 * Interpolate a position along the `coarse_edge` at the relative `u` coordinate.
 * If `is_simple` is false, this will perform a B-Spline interpolation using the edge neighbors,
 * otherwise a linear interpolation will be done base on the edge vertices.
 */
float3 mesh_interpolate_position_on_edge(Span<float3> coarse_positions,
                                         Span<int2> coarse_edges,
                                         GroupedSpan<int> vert_to_edge_map,
                                         int coarse_edge_index,
                                         bool is_simple,
                                         float u);

/**
 * Calculate positions position of the given mesh vertices at the limit surface of the mesh.
 *
 * The limit_positions is to be sized at exactly the number of the base mesh vertices.
 */
void calculate_limit_positions(Mesh *mesh, MutableSpan<float3> limit_positions);

}  // namespace blender::bke::subdiv
