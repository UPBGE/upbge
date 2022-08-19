/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

struct PBVHGPUFormat;

/** \file
 * \ingroup bke
 */

#ifdef __cplusplus
extern "C" {
#endif

struct MLoop;
struct MLoopTri;
struct MPoly;
struct MVert;

/* Axis-aligned bounding box */
typedef struct {
  float bmin[3], bmax[3];
} BB;

/* Axis-aligned bounding box with centroid */
typedef struct {
  float bmin[3], bmax[3], bcentroid[3];
} BBC;

struct MeshElemMap;

/* NOTE: this structure is getting large, might want to split it into
 * union'd structs */
struct PBVHNode {
  /* Opaque handle for drawing code */
  struct GPU_PBVH_Buffers *draw_buffers;

  /* Voxel bounds */
  BB vb;
  BB orig_vb;

  /* For internal nodes, the offset of the children in the PBVH
   * 'nodes' array. */
  int children_offset;

  /* List of primitives for this node. Semantics depends on
   * PBVH type:
   *
   * - PBVH_FACES: Indices into the PBVH.looptri array.
   * - PBVH_GRIDS: Multires grid indices.
   * - PBVH_BMESH: Unused.  See PBVHNode.bm_faces.
   *
   * NOTE: This is a pointer inside of PBVH.prim_indices; it
   * is not allocated separately per node.
   */
  int *prim_indices;
  unsigned int totprim; /* Number of primitives inside prim_indices. */

  /* Array of indices into the mesh's MVert array. Contains the
   * indices of all vertices used by faces that are within this
   * node's bounding box.
   *
   * Note that a vertex might be used by a multiple faces, and
   * these faces might be in different leaf nodes. Such a vertex
   * will appear in the vert_indices array of each of those leaf
   * nodes.
   *
   * In order to support cases where you want access to multiple
   * nodes' vertices without duplication, the vert_indices array
   * is ordered such that the first part of the array, up to
   * index 'uniq_verts', contains "unique" vertex indices. These
   * vertices might not be truly unique to this node, but if
   * they appear in another node's vert_indices array, they will
   * be above that node's 'uniq_verts' value.
   *
   * Used for leaf nodes in a mesh-based PBVH (not multires.)
   */
  const int *vert_indices;
  unsigned int uniq_verts, face_verts;

  /* Array of indices into the Mesh's MLoop array.
   * PBVH_FACES only.
   */
  int *loop_indices;
  unsigned int loop_indices_num;

  /* An array mapping face corners into the vert_indices
   * array. The array is sized to match 'totprim', and each of
   * the face's corners gets an index into the vert_indices
   * array, in the same order as the corners in the original
   * MLoopTri.
   *
   * Used for leaf nodes in a mesh-based PBVH (not multires.)
   */
  const int (*face_vert_indices)[3];

  /* Indicates whether this node is a leaf or not; also used for
   * marking various updates that need to be applied. */
  PBVHNodeFlags flag : 16;

  /* Used for raycasting: how close bb is to the ray point. */
  float tmin;

  /* Scalar displacements for sculpt mode's layer brush. */
  float *layer_disp;

  int proxy_count;
  PBVHProxyNode *proxies;

  /* Dyntopo */

  /* GSet of pointers to the BMFaces used by this node.
   * NOTE: PBVH_BMESH only. Faces are always triangles
   * (dynamic topology forcibly triangulates the mesh).
   */
  GSet *bm_faces;
  GSet *bm_unique_verts;
  GSet *bm_other_verts;
  float (*bm_orco)[3];
  int (*bm_ortri)[3];
  int bm_tot_ortri;

  /* Used to store the brush color during a stroke and composite it over the original color */
  PBVHColorBufferNode color_buffer;
  PBVHPixelsNode pixels;

  /* Used to flash colors of updated node bounding boxes in
   * debug draw mode (when G.debug_value / bpy.app.debug_value is 889).
   */
  int debug_draw_gen;
};

typedef enum { PBVH_DYNTOPO_SMOOTH_SHADING = 1 } PBVHFlags;

typedef struct PBVHBMeshLog PBVHBMeshLog;

struct PBVH {
  struct PBVHPublic header;
  PBVHFlags flags;

  PBVHNode *nodes;
  int node_mem_count, totnode;

  /* Memory backing for PBVHNode.prim_indices. */
  int *prim_indices;
  int totprim;
  int totvert;

  int leaf_limit;

  /* Mesh data */
  struct Mesh *mesh;

  /* NOTE: Normals are not `const` because they can be updated for drawing by sculpt code. */
  float (*vert_normals)[3];
  bool *hide_vert;
  struct MVert *verts;
  const struct MPoly *mpoly;
  const struct MLoop *mloop;
  const struct MLoopTri *looptri;
  CustomData *vdata;
  CustomData *ldata;
  CustomData *pdata;

  int face_sets_color_seed;
  int face_sets_color_default;
  int *face_sets;

  /* Grid Data */
  CCGKey gridkey;
  CCGElem **grids;
  void **gridfaces;
  const DMFlagMat *grid_flag_mats;
  int totgrid;
  BLI_bitmap **grid_hidden;

  /* Used during BVH build and later to mark that a vertex needs to update
   * (its normal must be recalculated). */
  bool *vert_bitmap;

#ifdef PERFCNTRS
  int perf_modified;
#endif

  /* flag are verts/faces deformed */
  bool deformed;
  bool show_mask;
  bool show_face_sets;
  bool respect_hide;

  /* Dynamic topology */
  float bm_max_edge_len;
  float bm_min_edge_len;
  int cd_vert_node_offset;
  int cd_face_node_offset;

  float planes[6][4];
  int num_planes;

  struct BMLog *bm_log;
  struct SubdivCCG *subdiv_ccg;

  const struct MeshElemMap *pmap;

  CustomDataLayer *color_layer;
  eAttrDomain color_domain;

  bool is_drawing;

  /* Used by DynTopo to invalidate the draw cache. */
  bool draw_cache_invalid;

  struct PBVHGPUFormat *vbo_id;
};

/* pbvh.c */

void BB_reset(BB *bb);
/**
 * Expand the bounding box to include a new coordinate.
 */
void BB_expand(BB *bb, const float co[3]);
/**
 * Expand the bounding box to include another bounding box.
 */
void BB_expand_with_bb(BB *bb, BB *bb2);
void BBC_update_centroid(BBC *bbc);
/**
 * Return 0, 1, or 2 to indicate the widest axis of the bounding box.
 */
int BB_widest_axis(const BB *bb);
void pbvh_grow_nodes(PBVH *bvh, int totnode);
bool ray_face_intersection_quad(const float ray_start[3],
                                struct IsectRayPrecalc *isect_precalc,
                                const float t0[3],
                                const float t1[3],
                                const float t2[3],
                                const float t3[3],
                                float *depth);
bool ray_face_intersection_tri(const float ray_start[3],
                               struct IsectRayPrecalc *isect_precalc,
                               const float t0[3],
                               const float t1[3],
                               const float t2[3],
                               float *depth);

bool ray_face_nearest_quad(const float ray_start[3],
                           const float ray_normal[3],
                           const float t0[3],
                           const float t1[3],
                           const float t2[3],
                           const float t3[3],
                           float *r_depth,
                           float *r_dist_sq);
bool ray_face_nearest_tri(const float ray_start[3],
                          const float ray_normal[3],
                          const float t0[3],
                          const float t1[3],
                          const float t2[3],
                          float *r_depth,
                          float *r_dist_sq);

void pbvh_update_BB_redraw(PBVH *bvh, PBVHNode **nodes, int totnode, int flag);

/* pbvh_bmesh.c */

bool pbvh_bmesh_node_raycast(PBVHNode *node,
                             const float ray_start[3],
                             const float ray_normal[3],
                             struct IsectRayPrecalc *isect_precalc,
                             float *dist,
                             bool use_original,
                             PBVHVertRef *r_active_vertex,
                             float *r_face_normal);
bool pbvh_bmesh_node_nearest_to_ray(PBVHNode *node,
                                    const float ray_start[3],
                                    const float ray_normal[3],
                                    float *depth,
                                    float *dist_sq,
                                    bool use_original);

void pbvh_bmesh_normals_update(PBVHNode **nodes, int totnode);

/* pbvh_pixels.hh */

void pbvh_pixels_free(PBVHNode *node);
void pbvh_pixels_free_brush_test(PBVHNode *node);
void pbvh_free_draw_buffers(PBVH *pbvh, PBVHNode *node);

#ifdef __cplusplus
}
#endif
