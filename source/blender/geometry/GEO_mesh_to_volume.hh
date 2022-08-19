/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_float4x4.hh"
#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"

#pragma once

struct Volume;
struct VolumeGrid;
struct Depsgraph;

/** \file
 * \ingroup geo
 */

namespace blender::geometry {

struct MeshToVolumeResolution {
  MeshToVolumeModifierResolutionMode mode;
  union {
    float voxel_size;
    float voxel_amount;
  } settings;
};

#ifdef WITH_OPENVDB

/**
 * \param bounds_fn: Return the bounds of the mesh positions,
 * used for deciding the voxel size in "Amount" mode.
 */
float volume_compute_voxel_size(const Depsgraph *depsgraph,
                                FunctionRef<void(float3 &r_min, float3 &r_max)> bounds_fn,
                                const MeshToVolumeResolution resolution,
                                float exterior_band_width,
                                const float4x4 &transform);
/**
 * Add a new VolumeGrid to the Volume by converting the supplied mesh
 */
VolumeGrid *volume_grid_add_from_mesh(Volume *volume,
                                      const StringRefNull name,
                                      const Mesh *mesh,
                                      const float4x4 &mesh_to_volume_space_transform,
                                      float voxel_size,
                                      bool fill_volume,
                                      float exterior_band_width,
                                      float interior_band_width,
                                      float density);
#endif
}  // namespace blender::geometry
