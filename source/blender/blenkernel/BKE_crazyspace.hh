/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "BLI_float3x3.hh"
#include "BLI_math_vec_types.hh"
#include "BLI_span.hh"

struct Depsgraph;
struct Object;

namespace blender::bke::crazyspace {

/**
 * Contains information about how points have been deformed during evaluation.
 * This allows mapping edits on evaluated data back to original data in some cases.
 */
struct GeometryDeformation {
  /**
   * Positions of the deformed points. This may also point to the original position if no
   * deformation data is available.
   */
  Span<float3> positions;
  /**
   * Matrices that transform point translations on original data into corresponding translations in
   * evaluated data. This may be empty if not available.
   */
  Span<float3x3> deform_mats;

  float3 translation_from_deformed_to_original(const int position_i,
                                               const float3 &translation) const
  {
    if (this->deform_mats.is_empty()) {
      return translation;
    }
    const float3x3 &deform_mat = this->deform_mats[position_i];
    return deform_mat.inverted() * translation;
  }
};

/**
 * During evaluation of the object, deformation data may have been generated for this object. This
 * function either retrieves the deformation data from the evaluated object, or falls back to
 * returning the original data.
 */
GeometryDeformation get_evaluated_curves_deformation(const Depsgraph &depsgraph,
                                                     const Object &ob_orig);

}  // namespace blender::bke::crazyspace
