/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_generic_span.hh"
#include "BLI_index_mask.hh"
#include "BLI_offset_indices.hh"

#include "BKE_curves.hh"

namespace blender::geometry {

/**
 * 1D Gaussian-like smoothing function.
 *
 * \param iterations: Number of times to repeat the smoothing.
 * \param influence: Influence factor for each point.
 * \param smooth_ends: Smooth the first and last value.
 * \param keep_shape: Changes the gaussian kernel to avoid severe deformations.
 * \param is_cyclic: Propagate smoothing across the ends of the input as if they were connected.
 */
void gaussian_blur_1D(const GSpan src,
                      int iterations,
                      const VArray<float> &influence_by_point,
                      const bool smooth_ends,
                      const bool keep_shape,
                      const bool is_cyclic,
                      GMutableSpan dst);

/**
 * Smooths the \a attribute_data using a 1D gaussian blur.
 */
void smooth_curve_attribute(const IndexMask &curves_to_smooth,
                            const OffsetIndices<int> points_by_curve,
                            const VArray<bool> &point_selection,
                            const VArray<bool> &cyclic,
                            int iterations,
                            float influence,
                            bool smooth_ends,
                            bool keep_shape,
                            GMutableSpan attribute_data);

/**
 * Smooths the \a attribute_data using a 1D gaussian blur.
 */
void smooth_curve_attribute(const IndexMask &curves_to_smooth,
                            const OffsetIndices<int> points_by_curve,
                            const VArray<bool> &point_selection,
                            const VArray<bool> &cyclic,
                            int iterations,
                            const VArray<float> &influence_by_point,
                            bool smooth_ends,
                            bool keep_shape,
                            GMutableSpan attribute_data);

/**
 * Smooths the positions of \a curves using a 1D gaussian blur.
 */
void smooth_curve_positions(bke::CurvesGeometry &curves,
                            const IndexMask &curves_to_smooth,
                            const VArray<bool> &point_selection,
                            int iterations,
                            const VArray<float> &influence_by_point,
                            bool smooth_ends,
                            bool keep_shape);
void smooth_curve_positions(bke::CurvesGeometry &curves,
                            const IndexMask &curves_to_smooth,
                            const VArray<bool> &point_selection,
                            int iterations,
                            float influence,
                            bool smooth_ends,
                            bool keep_shape);

}  // namespace blender::geometry
