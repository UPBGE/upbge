/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. All rights reserved. */

#pragma once

/** \file
 * \ingroup bke
 */

#ifdef __cplusplus

#  include <mutex>

#  include "MEM_guardedalloc.h"

#  include "BLI_array.hh"
#  include "BLI_bit_vector.hh"
#  include "BLI_bounds_types.hh"
#  include "BLI_math_vector_types.hh"
#  include "BLI_shared_cache.hh"
#  include "BLI_span.hh"

#  include "DNA_customdata_types.h"
#  include "DNA_meshdata_types.h"

struct BVHCache;
struct EditMeshData;
struct MLoopTri;
struct ShrinkwrapBoundaryData;
struct SubdivCCG;
struct SubsurfRuntimeData;

#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum eMeshBatchDirtyMode {
  BKE_MESH_BATCH_DIRTY_ALL = 0,
  BKE_MESH_BATCH_DIRTY_SELECT,
  BKE_MESH_BATCH_DIRTY_SELECT_PAINT,
  BKE_MESH_BATCH_DIRTY_SHADING,
  BKE_MESH_BATCH_DIRTY_UVEDIT_ALL,
  BKE_MESH_BATCH_DIRTY_UVEDIT_SELECT,
} eMeshBatchDirtyMode;

/** #MeshRuntime.wrapper_type */
typedef enum eMeshWrapperType {
  /** Use mesh data (#Mesh.mvert, #Mesh.medge, #Mesh.mloop, #Mesh.mpoly). */
  ME_WRAPPER_TYPE_MDATA = 0,
  /** Use edit-mesh data (#Mesh.edit_mesh, #MeshRuntime.edit_data). */
  ME_WRAPPER_TYPE_BMESH = 1,
  /** Use subdivision mesh data (#MeshRuntime.mesh_eval). */
  ME_WRAPPER_TYPE_SUBD = 2,
} eMeshWrapperType;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

namespace blender::bke {

/**
 * Cache of a mesh's loose edges, accessed with #Mesh::loose_edges(). *
 */
struct LooseEdgeCache {
  /**
   * A bitmap set to true for each loose edge, false if the edge is used by any face.
   * Allocated only if there is at least one loose edge.
   */
  blender::BitVector<> is_loose_bits;
  /**
   * The number of loose edges. If zero, the #is_loose_bits shouldn't be accessed.
   * If less than zero, the cache has been accessed in an invalid way
   * (i.e.directly instead of through #Mesh::loose_edges()).
   */
  int count = -1;
};

struct MeshRuntime {
  /* Evaluated mesh for objects which do not have effective modifiers.
   * This mesh is used as a result of modifier stack evaluation.
   * Since modifier stack evaluation is threaded on object level we need some synchronization. */
  Mesh *mesh_eval = nullptr;
  std::mutex eval_mutex;

  /* A separate mutex is needed for normal calculation, because sometimes
   * the normals are needed while #eval_mutex is already locked. */
  std::mutex normals_mutex;

  /** Needed to ensure some thread-safety during render data pre-processing. */
  std::mutex render_mutex;

  /**
   * A cache of bounds shared between data-blocks with unchanged positions. When changing positions
   * affect the bounds, the cache is "un-shared" with other geometries. See #SharedCache comments.
   */
  SharedCache<Bounds<float3>> bounds_cache;

  /** Lazily initialized SoA data from the #edit_mesh field in #Mesh. */
  EditMeshData *edit_data = nullptr;

  /**
   * Data used to efficiently draw the mesh in the viewport, especially useful when
   * the same mesh is used in many objects or instances. See `draw_cache_impl_mesh.cc`.
   */
  void *batch_cache = nullptr;

  /** Cache for derived triangulation of the mesh, accessed with #Mesh::looptris(). */
  SharedCache<Array<MLoopTri>> looptris_cache;

  /** Cache for BVH trees generated for the mesh. Defined in 'BKE_bvhutil.c' */
  BVHCache *bvh_cache = nullptr;

  /** Cache of non-manifold boundary data for Shrink-wrap Target Project. */
  ShrinkwrapBoundaryData *shrinkwrap_data = nullptr;

  /** Needed in case we need to lazily initialize the mesh. */
  CustomData_MeshMasks cd_mask_extra = {};

  SubdivCCG *subdiv_ccg = nullptr;
  int subdiv_ccg_tot_level = 0;

  /** Set by modifier stack if only deformed from original. */
  bool deformed_only = false;
  /**
   * Copied from edit-mesh (hint, draw with edit-mesh data when true).
   *
   * Modifiers that edit the mesh data in-place must set this to false
   * (most #eModifierTypeType_NonGeometrical modifiers). Otherwise the edit-mesh
   * data will be used for drawing, missing changes from modifiers. See T79517.
   */
  bool is_original_bmesh = false;

  /** #eMeshWrapperType and others. */
  eMeshWrapperType wrapper_type = ME_WRAPPER_TYPE_MDATA;
  /**
   * A type mask from wrapper_type,
   * in case there are differences in finalizing logic between types.
   */
  eMeshWrapperType wrapper_type_finalize = ME_WRAPPER_TYPE_MDATA;

  /**
   * Settings for lazily evaluating the subdivision on the CPU if needed. These are
   * set in the modifier when GPU subdivision can be performed, and owned by the by
   * the modifier in the object.
   */
  SubsurfRuntimeData *subsurf_runtime_data = nullptr;

  /**
   * Caches for lazily computed vertex and polygon normals. These are stored here rather than in
   * #CustomData because they can be calculated on a `const` mesh, and adding custom data layers on
   * a `const` mesh is not thread-safe.
   */
  bool vert_normals_dirty = true;
  bool poly_normals_dirty = true;
  float (*vert_normals)[3] = nullptr;
  float (*poly_normals)[3] = nullptr;

  /**
   * A cache of data about the loose edges. Can be shared with other data-blocks with unchanged
   * topology. Accessed with #Mesh::loose_edges().
   */
  SharedCache<LooseEdgeCache> loose_edges_cache;

  /**
   * A #BLI_bitmap containing tags for the center vertices of subdivided polygons, set by the
   * subdivision surface modifier and used by drawing code instead of polygon center face dots.
   */
  uint32_t *subsurf_face_dot_tags = nullptr;

  MeshRuntime() = default;
  ~MeshRuntime();

  MEM_CXX_CLASS_ALLOC_FUNCS("MeshRuntime")
};

}  // namespace blender::bke

#endif
