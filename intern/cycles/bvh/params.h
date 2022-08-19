/* SPDX-License-Identifier: Apache-2.0
 * Adapted from code copyright 2009-2010 NVIDIA Corporation
 * Modifications Copyright 2011-2022 Blender Foundation. */

#ifndef __BVH_PARAMS_H__
#define __BVH_PARAMS_H__

#include "util/boundbox.h"

#include "kernel/types.h"

CCL_NAMESPACE_BEGIN

/* Layout of BVH tree.
 *
 * For example, how wide BVH tree is, in terms of number of children
 * per node.
 */
typedef KernelBVHLayout BVHLayout;

/* Type of BVH, in terms whether it is supported dynamic updates of meshes
 * or whether modifying geometry requires full BVH rebuild.
 */
enum BVHType {
  /* BVH supports dynamic updates of geometry.
   *
   * Faster for updating BVH tree when doing modifications in viewport,
   * but slower for rendering.
   */
  BVH_TYPE_DYNAMIC = 0,
  /* BVH tree is calculated for specific scene, updates in geometry
   * requires full tree rebuild.
   *
   * Slower to update BVH tree when modifying objects in viewport, also
   * slower to build final BVH tree but gives best possible render speed.
   */
  BVH_TYPE_STATIC = 1,

  BVH_NUM_TYPES,
};

/* Names bit-flag type to denote which BVH layouts are supported by
 * particular area.
 *
 * Bit-flags are the BVH_LAYOUT_* values.
 */
typedef int BVHLayoutMask;

/* Get human readable name of BVH layout. */
const char *bvh_layout_name(BVHLayout layout);

/* BVH Parameters */

class BVHParams {
 public:
  /* spatial split area threshold */
  bool use_spatial_split;
  float spatial_split_alpha;

  /* Unaligned nodes creation threshold */
  float unaligned_split_threshold;

  /* SAH costs */
  float sah_node_cost;
  float sah_primitive_cost;

  /* number of primitives in leaf */
  int min_leaf_size;
  int max_triangle_leaf_size;
  int max_motion_triangle_leaf_size;
  int max_curve_leaf_size;
  int max_motion_curve_leaf_size;
  int max_point_leaf_size;
  int max_motion_point_leaf_size;

  /* object or mesh level bvh */
  bool top_level;

  /* BVH layout to be built. */
  BVHLayout bvh_layout;

  /* Use unaligned bounding boxes.
   * Only used for curves BVH.
   */
  bool use_unaligned_nodes;

  /* Use compact acceleration structure (Embree)*/
  bool use_compact_structure;

  /* Split time range to this number of steps and create leaf node for each
   * of this time steps.
   *
   * Speeds up rendering of motion primitives in the cost of higher memory usage.
   */

  /* Same as above, but for triangle primitives. */
  int num_motion_triangle_steps;
  int num_motion_curve_steps;
  int num_motion_point_steps;

  /* Same as in SceneParams. */
  int bvh_type;

  /* These are needed for Embree. */
  int curve_subdivisions;

  /* fixed parameters */
  enum { MAX_DEPTH = 64, MAX_SPATIAL_DEPTH = 48, NUM_SPATIAL_BINS = 32 };

  BVHParams()
  {
    use_spatial_split = true;
    spatial_split_alpha = 1e-5f;

    unaligned_split_threshold = 0.7f;

    /* todo: see if splitting up primitive cost to be separate for triangles
     * and curves can help. so far in tests it doesn't help, but why? */
    sah_node_cost = 1.0f;
    sah_primitive_cost = 1.0f;

    min_leaf_size = 1;
    max_triangle_leaf_size = 8;
    max_motion_triangle_leaf_size = 8;
    max_curve_leaf_size = 1;
    max_motion_curve_leaf_size = 4;
    max_point_leaf_size = 8;
    max_motion_point_leaf_size = 8;

    top_level = false;
    bvh_layout = BVH_LAYOUT_BVH2;
    use_compact_structure = false;
    use_unaligned_nodes = false;

    num_motion_curve_steps = 0;
    num_motion_triangle_steps = 0;
    num_motion_point_steps = 0;

    bvh_type = 0;

    curve_subdivisions = 4;
  }

  /* SAH costs */
  __forceinline float cost(int num_nodes, int num_primitives) const
  {
    return node_cost(num_nodes) + primitive_cost(num_primitives);
  }

  __forceinline float primitive_cost(int n) const
  {
    return n * sah_primitive_cost;
  }

  __forceinline float node_cost(int n) const
  {
    return n * sah_node_cost;
  }

  __forceinline bool small_enough_for_leaf(int size, int level)
  {
    return (size <= min_leaf_size || level >= MAX_DEPTH);
  }

  bool use_motion_steps()
  {
    return num_motion_curve_steps > 0 || num_motion_triangle_steps > 0 ||
           num_motion_point_steps > 0;
  }

  /* Gets best matching BVH.
   *
   * If the requested layout is supported by the device, it will be used.
   * Otherwise, widest supported layout below that will be used.
   */
  static BVHLayout best_bvh_layout(BVHLayout requested_layout, BVHLayoutMask supported_layouts);
};

/* BVH Reference
 *
 * Reference to a primitive. Primitive index and object are sneakily packed
 * into BoundBox to reduce memory usage and align nicely */

class BVHReference {
 public:
  __forceinline BVHReference()
  {
  }

  __forceinline BVHReference(const BoundBox &bounds_,
                             int prim_index_,
                             int prim_object_,
                             int prim_type,
                             float time_from = 0.0f,
                             float time_to = 1.0f)
      : rbounds(bounds_), time_from_(time_from), time_to_(time_to)
  {
    rbounds.min.w = __int_as_float(prim_index_);
    rbounds.max.w = __int_as_float(prim_object_);
    type = prim_type;
  }

  __forceinline const BoundBox &bounds() const
  {
    return rbounds;
  }
  __forceinline int prim_index() const
  {
    return __float_as_int(rbounds.min.w);
  }
  __forceinline int prim_object() const
  {
    return __float_as_int(rbounds.max.w);
  }
  __forceinline int prim_type() const
  {
    return type;
  }
  __forceinline float time_from() const
  {
    return time_from_;
  }
  __forceinline float time_to() const
  {
    return time_to_;
  }

  BVHReference &operator=(const BVHReference &arg)
  {
    if (&arg != this) {
      /* TODO(sergey): Check if it is still faster to memcpy() with
       * modern compilers.
       */
      memcpy((void *)this, &arg, sizeof(BVHReference));
    }
    return *this;
  }

 protected:
  BoundBox rbounds;
  uint type;
  float time_from_, time_to_;
};

/* BVH Range
 *
 * Build range used during construction, to indicate the bounds and place in
 * the reference array of a subset of primitives Again uses trickery to pack
 * integers into BoundBox for alignment purposes. */

class BVHRange {
 public:
  __forceinline BVHRange()
  {
    rbounds.min.w = __int_as_float(0);
    rbounds.max.w = __int_as_float(0);
  }

  __forceinline BVHRange(const BoundBox &bounds_, int start_, int size_) : rbounds(bounds_)
  {
    rbounds.min.w = __int_as_float(start_);
    rbounds.max.w = __int_as_float(size_);
  }

  __forceinline BVHRange(const BoundBox &bounds_, const BoundBox &cbounds_, int start_, int size_)
      : rbounds(bounds_), cbounds(cbounds_)
  {
    rbounds.min.w = __int_as_float(start_);
    rbounds.max.w = __int_as_float(size_);
  }

  __forceinline void set_start(int start_)
  {
    rbounds.min.w = __int_as_float(start_);
  }

  __forceinline const BoundBox &bounds() const
  {
    return rbounds;
  }
  __forceinline const BoundBox &cent_bounds() const
  {
    return cbounds;
  }
  __forceinline int start() const
  {
    return __float_as_int(rbounds.min.w);
  }
  __forceinline int size() const
  {
    return __float_as_int(rbounds.max.w);
  }
  __forceinline int end() const
  {
    return start() + size();
  }

 protected:
  BoundBox rbounds;
  BoundBox cbounds;
};

/* BVH Spatial Bin */

struct BVHSpatialBin {
  BoundBox bounds;
  int enter;
  int exit;

  __forceinline BVHSpatialBin()
  {
  }
};

/* BVH Spatial Storage
 *
 * The idea of this storage is have thread-specific storage for the spatial
 * splitters. We can pre-allocate this storage in advance and avoid heavy memory
 * operations during split process.
 */

struct BVHSpatialStorage {
  /* Accumulated bounds when sweeping from right to left. */
  vector<BoundBox> right_bounds;

  /* Bins used for histogram when selecting best split plane. */
  BVHSpatialBin bins[3][BVHParams::NUM_SPATIAL_BINS];

  /* Temporary storage for the new references. Used by spatial split to store
   * new references in before they're getting inserted into actual array,
   */
  vector<BVHReference> new_references;
};

CCL_NAMESPACE_END

#endif /* __BVH_PARAMS_H__ */
