/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup editors
 */

#pragma once

#include "BLI_utildefines.h"
#include "DNA_scene_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ********* exports for space_view3d/ module ********** */
struct ARegion;
struct BMEdge;
struct BMElem;
struct BMFace;
struct BMVert;
struct BPoint;
struct Base;
struct BezTriple;
struct BoundBox;
struct Camera;
struct CustomData_MeshMasks;
struct Depsgraph;
struct EditBone;
struct GPUSelectResult;
struct ID;
struct MVert;
struct Main;
struct MetaElem;
struct Nurb;
struct Object;
struct RV3DMatrixStore;
struct RegionView3D;
struct RenderEngineType;
struct Scene;
struct ScrArea;
struct SnapObjectContext;
struct View3D;
struct ViewContext;
struct ViewLayer;
struct bContext;
struct bPoseChannel;
struct bScreen;
struct rctf;
struct rcti;
struct wmGizmo;
struct wmWindow;
struct wmWindowManager;

/* for derivedmesh drawing callbacks, for view3d_select, .... */
typedef struct ViewContext {
  struct bContext *C;
  struct Main *bmain;
  /* Dependency graph is uses for depth drawing, viewport camera matrix access, and also some areas
   * are re-using this to access evaluated entities.
   *
   * Moral of the story: assign to a fully evaluated state. */
  struct Depsgraph *depsgraph;
  struct Scene *scene;
  struct ViewLayer *view_layer;
  struct Object *obact;
  struct Object *obedit;
  struct ARegion *region;
  struct View3D *v3d;
  struct wmWindow *win;
  struct RegionView3D *rv3d;
  struct BMEditMesh *em;
  int mval[2];
} ViewContext;

typedef struct ViewDepths {
  unsigned short w, h;
  short x, y; /* only for temp use for sub-rects, added to region->winx/y */
  float *depths;
  double depth_range[2];
} ViewDepths;

/* Rotate 3D cursor on placement. */
enum eV3DCursorOrient {
  V3D_CURSOR_ORIENT_NONE = 0,
  V3D_CURSOR_ORIENT_VIEW,
  V3D_CURSOR_ORIENT_XFORM,
  V3D_CURSOR_ORIENT_GEOM,
};

void ED_view3d_background_color_get(const struct Scene *scene,
                                    const struct View3D *v3d,
                                    float r_color[3]);
bool ED_view3d_has_workbench_in_texture_color(const struct Scene *scene,
                                              const struct Object *ob,
                                              const struct View3D *v3d);
/**
 * Cursor position in `r_cursor_co`, result in `r_cursor_co`, `mval` in region coords.
 *
 * \note cannot use `event->mval` here, called by #object_add().
 */
void ED_view3d_cursor3d_position(struct bContext *C,
                                 const int mval[2],
                                 bool use_depth,
                                 float r_cursor_co[3]);
void ED_view3d_cursor3d_position_rotation(struct bContext *C,
                                          const int mval[2],
                                          bool use_depth,
                                          enum eV3DCursorOrient orientation,
                                          float r_cursor_co[3],
                                          float r_cursor_quat[4]);
void ED_view3d_cursor3d_update(struct bContext *C,
                               const int mval[2],
                               bool use_depth,
                               enum eV3DCursorOrient orientation);

struct Camera *ED_view3d_camera_data_get(struct View3D *v3d, struct RegionView3D *rv3d);

/**
 * Calculate the view transformation matrix from RegionView3D input.
 * The resulting matrix is equivalent to #RegionView3D.viewinv
 * \param mat: The view 4x4 transformation matrix to calculate.
 * \param ofs: The view offset, normally from #RegionView3D.ofs.
 * \param quat: The view rotation, quaternion normally from #RegionView3D.viewquat.
 * \param dist: The view distance from ofs, normally from #RegionView3D.dist.
 */
void ED_view3d_to_m4(float mat[4][4], const float ofs[3], const float quat[4], float dist);
/**
 * Set the view transformation from a 4x4 matrix.
 *
 * \param mat: The view 4x4 transformation matrix to assign.
 * \param ofs: The view offset, normally from #RegionView3D.ofs.
 * \param quat: The view rotation, quaternion normally from #RegionView3D.viewquat.
 * \param dist: The view distance from `ofs`, normally from #RegionView3D.dist.
 */
void ED_view3d_from_m4(const float mat[4][4], float ofs[3], float quat[4], const float *dist);

/**
 * Set the #RegionView3D members from an objects transformation and optionally lens.
 * \param ob: The object to set the view to.
 * \param ofs: The view offset to be set, normally from #RegionView3D.ofs.
 * \param quat: The view rotation to be set, quaternion normally from #RegionView3D.viewquat.
 * \param dist: The view distance from `ofs `to be set, normally from #RegionView3D.dist.
 * \param lens: The view lens angle set for cameras and lights, normally from View3D.lens.
 */
void ED_view3d_from_object(
    const struct Object *ob, float ofs[3], float quat[4], float *dist, float *lens);
/**
 * Set the object transformation from #RegionView3D members.
 * \param depsgraph: The depsgraph to get the evaluated object parent
 * for the transformation calculation.
 * \param ob: The object which has the transformation assigned.
 * \param ofs: The view offset, normally from #RegionView3D.ofs.
 * \param quat: The view rotation, quaternion normally from #RegionView3D.viewquat.
 * \param dist: The view distance from `ofs`, normally from #RegionView3D.dist.
 */
void ED_view3d_to_object(const struct Depsgraph *depsgraph,
                         struct Object *ob,
                         const float ofs[3],
                         const float quat[4],
                         float dist);

bool ED_view3d_camera_to_view_selected(struct Main *bmain,
                                       struct Depsgraph *depsgraph,
                                       const struct Scene *scene,
                                       struct Object *camera_ob);

bool ED_view3d_camera_to_view_selected_with_set_clipping(struct Main *bmain,
                                                         struct Depsgraph *depsgraph,
                                                         const struct Scene *scene,
                                                         struct Object *camera_ob);

/**
 * Use to store the last view, before entering camera view.
 */
void ED_view3d_lastview_store(struct RegionView3D *rv3d);

/* Depth buffer */
typedef enum {
  /** Redraw viewport without Grease Pencil and Annotations. */
  V3D_DEPTH_NO_GPENCIL = 0,
  /** Redraw viewport with Grease Pencil and Annotations only. */
  V3D_DEPTH_GPENCIL_ONLY,
  /** Redraw viewport with active object only. */
  V3D_DEPTH_OBJECT_ONLY,

} eV3DDepthOverrideMode;
/**
 * Redraw the viewport depth buffer.
 */
void ED_view3d_depth_override(struct Depsgraph *depsgraph,
                              struct ARegion *region,
                              struct View3D *v3d,
                              struct Object *obact,
                              eV3DDepthOverrideMode mode,
                              struct ViewDepths **r_depths);
void ED_view3d_depths_free(ViewDepths *depths);
bool ED_view3d_depth_read_cached(const ViewDepths *vd,
                                 const int mval[2],
                                 int margin,
                                 float *r_depth);
bool ED_view3d_depth_read_cached_normal(const struct ARegion *region,
                                        const ViewDepths *depths,
                                        const int mval[2],
                                        float r_normal[3]);
bool ED_view3d_depth_unproject_v3(const struct ARegion *region,
                                  const int mval[2],
                                  double depth,
                                  float r_location_world[3]);

/* Projection */
#define IS_CLIPPED 12000

/* return values for ED_view3d_project_...() */
typedef enum {
  V3D_PROJ_RET_OK = 0,
  /** can't avoid this when in perspective mode, (can't avoid) */
  V3D_PROJ_RET_CLIP_NEAR = 1,
  /** After clip_end. */
  V3D_PROJ_RET_CLIP_FAR = 2,
  /** so close to zero we can't apply a perspective matrix usefully */
  V3D_PROJ_RET_CLIP_ZERO = 3,
  /** bounding box clip - RV3D_CLIPPING */
  V3D_PROJ_RET_CLIP_BB = 4,
  /** outside window bounds */
  V3D_PROJ_RET_CLIP_WIN = 5,
  /** outside range (mainly for short), (can't avoid) */
  V3D_PROJ_RET_OVERFLOW = 6,
} eV3DProjStatus;

/* some clipping tests are optional */
typedef enum {
  V3D_PROJ_TEST_NOP = 0,
  V3D_PROJ_TEST_CLIP_BB = (1 << 0),
  V3D_PROJ_TEST_CLIP_WIN = (1 << 1),
  V3D_PROJ_TEST_CLIP_NEAR = (1 << 2),
  V3D_PROJ_TEST_CLIP_FAR = (1 << 3),
  V3D_PROJ_TEST_CLIP_ZERO = (1 << 4),
  /**
   * Clip the contents of the data being iterated over.
   * Currently this is only used to edges when projecting into screen space.
   *
   * Clamp the edge within the viewport limits defined by
   * #V3D_PROJ_TEST_CLIP_WIN, #V3D_PROJ_TEST_CLIP_NEAR & #V3D_PROJ_TEST_CLIP_FAR.
   * This resolves the problem of a visible edge having one of it's vertices
   * behind the viewport. See: T32214.
   *
   * This is not default behavior as it may be important for the screen-space location
   * of an edges vertex to represent that vertices location (instead of a location along the edge).
   *
   * \note Perspective views should enable #V3D_PROJ_TEST_CLIP_WIN along with
   * #V3D_PROJ_TEST_CLIP_NEAR as the near-plane-clipped location of a point
   * may become very large (even infinite) when projected into screen-space.
   * Unless that point happens to coincide with the camera's point of view.
   *
   * Use #V3D_PROJ_TEST_CLIP_CONTENT_DEFAULT instead of #V3D_PROJ_TEST_CLIP_CONTENT,
   * to avoid accidentally enabling near clipping without clipping by window bounds.
   */
  V3D_PROJ_TEST_CLIP_CONTENT = (1 << 5),
} eV3DProjTest;
ENUM_OPERATORS(eV3DProjTest, V3D_PROJ_TEST_CLIP_CONTENT);

#define V3D_PROJ_TEST_CLIP_DEFAULT \
  (V3D_PROJ_TEST_CLIP_BB | V3D_PROJ_TEST_CLIP_WIN | V3D_PROJ_TEST_CLIP_NEAR)
#define V3D_PROJ_TEST_ALL \
  (V3D_PROJ_TEST_CLIP_DEFAULT | V3D_PROJ_TEST_CLIP_FAR | V3D_PROJ_TEST_CLIP_ZERO | \
   V3D_PROJ_TEST_CLIP_CONTENT)

#define V3D_PROJ_TEST_CLIP_CONTENT_DEFAULT \
  (V3D_PROJ_TEST_CLIP_CONTENT | V3D_PROJ_TEST_CLIP_NEAR | V3D_PROJ_TEST_CLIP_FAR | \
   V3D_PROJ_TEST_CLIP_WIN)

/* view3d_snap.c */

bool ED_view3d_snap_selected_to_location(struct bContext *C,
                                         const float snap_target_global[3],
                                         int pivot_point);

/* view3d_cursor_snap.c */

#define USE_SNAP_DETECT_FROM_KEYMAP_HACK
typedef enum {
  V3D_SNAPCURSOR_TOGGLE_ALWAYS_TRUE = 1 << 0,
  V3D_SNAPCURSOR_OCCLUSION_ALWAYS_TRUE = 1 << 1,
  V3D_SNAPCURSOR_OCCLUSION_ALWAYS_FALSE = 1 << 2, /* TODO. */
  V3D_SNAPCURSOR_SNAP_EDIT_GEOM_FINAL = 1 << 3,
  V3D_SNAPCURSOR_SNAP_EDIT_GEOM_CAGE = 1 << 4,
} eV3DSnapCursor;

typedef enum {
  V3D_PLACE_DEPTH_SURFACE = 0,
  V3D_PLACE_DEPTH_CURSOR_PLANE = 1,
  V3D_PLACE_DEPTH_CURSOR_VIEW = 2,
} eV3DPlaceDepth;

typedef enum {
  V3D_PLACE_ORIENT_SURFACE = 0,
  V3D_PLACE_ORIENT_DEFAULT = 1,
} eV3DPlaceOrient;

typedef struct V3DSnapCursorData {
  eSnapMode snap_elem;
  float loc[3];
  float nor[3];
  float obmat[4][4];
  int elem_index[3];
  float plane_omat[3][3];
  bool is_snap_invert;

  /** Enabled when snap is activated, even if it didn't find anything. */
  bool is_enabled;
} V3DSnapCursorData;

typedef struct V3DSnapCursorState {
  /* Setup. */
  eV3DSnapCursor flag;
  eV3DPlaceDepth plane_depth;
  eV3DPlaceOrient plane_orient;
  uchar color_line[4];
  uchar color_point[4];
  uchar color_box[4];
  struct wmGizmoGroupType *gzgrp_type; /* Force cursor to be drawn only when gizmo is available. */
  float *prevpoint;
  float box_dimensions[3];
  eSnapMode snap_elem_force; /* If SCE_SNAP_MODE_NONE, use scene settings. */
  short plane_axis;
  bool use_plane_axis_auto;
  bool draw_point;
  bool draw_plane;
  bool draw_box;
} V3DSnapCursorState;

void ED_view3d_cursor_snap_state_default_set(V3DSnapCursorState *state);
V3DSnapCursorState *ED_view3d_cursor_snap_state_get(void);
V3DSnapCursorState *ED_view3d_cursor_snap_active(void);
void ED_view3d_cursor_snap_deactive(V3DSnapCursorState *state);
void ED_view3d_cursor_snap_prevpoint_set(V3DSnapCursorState *state, const float prev_point[3]);
void ED_view3d_cursor_snap_data_update(V3DSnapCursorState *state,
                                       const struct bContext *C,
                                       int x,
                                       int y);
V3DSnapCursorData *ED_view3d_cursor_snap_data_get(void);
struct SnapObjectContext *ED_view3d_cursor_snap_context_ensure(struct Scene *scene);
void ED_view3d_cursor_snap_draw_util(struct RegionView3D *rv3d,
                                     const float loc_prev[3],
                                     const float loc_curr[3],
                                     const float normal[3],
                                     const uchar color_line[4],
                                     const uchar color_point[4],
                                     eSnapMode snap_elem_type);

/* view3d_iterators.c */

/* foreach iterators */

void meshobject_foreachScreenVert(
    struct ViewContext *vc,
    void (*func)(void *userData, struct MVert *eve, const float screen_co[2], int index),
    void *userData,
    eV3DProjTest clip_flag);
void mesh_foreachScreenVert(
    struct ViewContext *vc,
    void (*func)(void *userData, struct BMVert *eve, const float screen_co[2], int index),
    void *userData,
    eV3DProjTest clip_flag);
void mesh_foreachScreenEdge(struct ViewContext *vc,
                            void (*func)(void *userData,
                                         struct BMEdge *eed,
                                         const float screen_co_a[2],
                                         const float screen_co_b[2],
                                         int index),
                            void *userData,
                            eV3DProjTest clip_flag);

/**
 * A version of #mesh_foreachScreenEdge that clips the segment when
 * there is a clipping bounding box.
 */
void mesh_foreachScreenEdge_clip_bb_segment(struct ViewContext *vc,
                                            void (*func)(void *userData,
                                                         struct BMEdge *eed,
                                                         const float screen_co_a[2],
                                                         const float screen_co_b[2],
                                                         int index),
                                            void *userData,
                                            eV3DProjTest clip_flag);

void mesh_foreachScreenFace(
    struct ViewContext *vc,
    void (*func)(void *userData, struct BMFace *efa, const float screen_co[2], int index),
    void *userData,
    eV3DProjTest clip_flag);
void nurbs_foreachScreenVert(struct ViewContext *vc,
                             void (*func)(void *userData,
                                          struct Nurb *nu,
                                          struct BPoint *bp,
                                          struct BezTriple *bezt,
                                          int beztindex,
                                          bool handle_visible,
                                          const float screen_co[2]),
                             void *userData,
                             eV3DProjTest clip_flag);
/**
 * #ED_view3d_init_mats_rv3d must be called first.
 */
void mball_foreachScreenElem(struct ViewContext *vc,
                             void (*func)(void *userData,
                                          struct MetaElem *ml,
                                          const float screen_co[2]),
                             void *userData,
                             eV3DProjTest clip_flag);
void lattice_foreachScreenVert(struct ViewContext *vc,
                               void (*func)(void *userData,
                                            struct BPoint *bp,
                                            const float screen_co[2]),
                               void *userData,
                               eV3DProjTest clip_flag);
/**
 * #ED_view3d_init_mats_rv3d must be called first.
 */
void armature_foreachScreenBone(struct ViewContext *vc,
                                void (*func)(void *userData,
                                             struct EditBone *ebone,
                                             const float screen_co_a[2],
                                             const float screen_co_b[2]),
                                void *userData,
                                eV3DProjTest clip_flag);

/**
 * ED_view3d_init_mats_rv3d must be called first.
 */
void pose_foreachScreenBone(struct ViewContext *vc,
                            void (*func)(void *userData,
                                         struct bPoseChannel *pchan,
                                         const float screen_co_a[2],
                                         const float screen_co_b[2]),
                            void *userData,
                            eV3DProjTest clip_flag);
/* *** end iterators *** */

/* view3d_project.c */

/**
 * \note use #ED_view3d_ob_project_mat_get to get the projection matrix
 */
void ED_view3d_project_float_v2_m4(const struct ARegion *region,
                                   const float co[3],
                                   float r_co[2],
                                   const float mat[4][4]);
/**
 * \note use #ED_view3d_ob_project_mat_get to get projecting mat
 */
void ED_view3d_project_float_v3_m4(const struct ARegion *region,
                                   const float co[3],
                                   float r_co[3],
                                   const float mat[4][4]);

eV3DProjStatus ED_view3d_project_base(const struct ARegion *region, struct Base *base);

/* *** short *** */
eV3DProjStatus ED_view3d_project_short_ex(const struct ARegion *region,
                                          float perspmat[4][4],
                                          bool is_local,
                                          const float co[3],
                                          short r_co[2],
                                          eV3DProjTest flag);
/* --- short --- */
eV3DProjStatus ED_view3d_project_short_global(const struct ARegion *region,
                                              const float co[3],
                                              short r_co[2],
                                              eV3DProjTest flag);
/* object space, use ED_view3d_init_mats_rv3d before calling */
eV3DProjStatus ED_view3d_project_short_object(const struct ARegion *region,
                                              const float co[3],
                                              short r_co[2],
                                              eV3DProjTest flag);

/* *** int *** */
eV3DProjStatus ED_view3d_project_int_ex(const struct ARegion *region,
                                        float perspmat[4][4],
                                        bool is_local,
                                        const float co[3],
                                        int r_co[2],
                                        eV3DProjTest flag);
/* --- int --- */
eV3DProjStatus ED_view3d_project_int_global(const struct ARegion *region,
                                            const float co[3],
                                            int r_co[2],
                                            eV3DProjTest flag);
/* object space, use ED_view3d_init_mats_rv3d before calling */
eV3DProjStatus ED_view3d_project_int_object(const struct ARegion *region,
                                            const float co[3],
                                            int r_co[2],
                                            eV3DProjTest flag);

/* *** float *** */
eV3DProjStatus ED_view3d_project_float_ex(const struct ARegion *region,
                                          float perspmat[4][4],
                                          bool is_local,
                                          const float co[3],
                                          float r_co[2],
                                          eV3DProjTest flag);
/* --- float --- */
eV3DProjStatus ED_view3d_project_float_global(const struct ARegion *region,
                                              const float co[3],
                                              float r_co[2],
                                              eV3DProjTest flag);
/**
 * Object space, use #ED_view3d_init_mats_rv3d before calling.
 */
eV3DProjStatus ED_view3d_project_float_object(const struct ARegion *region,
                                              const float co[3],
                                              float r_co[2],
                                              eV3DProjTest flag);

float ED_view3d_pixel_size(const struct RegionView3D *rv3d, const float co[3]);
float ED_view3d_pixel_size_no_ui_scale(const struct RegionView3D *rv3d, const float co[3]);

/**
 * Calculate a depth value from \a co, use with #ED_view3d_win_to_delta.
 *
 * \param r_flip: Set to `zfac < 0.0` before the value is made signed.
 * Since it's important in some cases to know if the value was flipped.
 *
 * \return The unsigned depth component of `co` multiplied by `rv3d->persmat` matrix,
 * with additional sanitation to ensure the result is never negative
 * as this isn't useful for tool-code.
 */
float ED_view3d_calc_zfac_ex(const struct RegionView3D *rv3d, const float co[3], bool *r_flip);
/** See #ED_view3d_calc_zfac_ex doc-string. */
float ED_view3d_calc_zfac(const struct RegionView3D *rv3d, const float co[3]);
/**
 * Calculate a depth value from `co` (result should only be used for comparison).
 */
float ED_view3d_calc_depth_for_comparison(const struct RegionView3D *rv3d, const float co[3]);

bool ED_view3d_clip_segment(const struct RegionView3D *rv3d, float ray_start[3], float ray_end[3]);
/**
 * Calculate a 3d viewpoint and direction vector from 2d window coordinates.
 * This ray_start is located at the viewpoint, ray_normal is the direction towards mval.
 * ray_start is clipped by the view near limit so points in front of it are always in view.
 * In orthographic view the resulting ray_normal will match the view vector.
 * \param region: The region (used for the window width and height).
 * \param v3d: The 3d viewport (used for near clipping value).
 * \param mval: The area relative 2d location (such as event->mval, converted into float[2]).
 * \param r_ray_start: The world-space point where the ray intersects the window plane.
 * \param r_ray_normal: The normalized world-space direction of towards mval.
 * \param do_clip_planes: Optionally clip the start of the ray by the view clipping planes.
 * \return success, false if the ray is totally clipped.
 */
bool ED_view3d_win_to_ray_clipped(struct Depsgraph *depsgraph,
                                  const struct ARegion *region,
                                  const struct View3D *v3d,
                                  const float mval[2],
                                  float r_ray_start[3],
                                  float r_ray_normal[3],
                                  bool do_clip_planes);
/**
 * Calculate a 3d viewpoint and direction vector from 2d window coordinates.
 * This ray_start is located at the viewpoint, ray_normal is the direction towards `mval`.
 * ray_start is clipped by the view near limit so points in front of it are always in view.
 * In orthographic view the resulting ray_normal will match the view vector.
 * This version also returns the ray_co point of the ray on window plane, useful to fix precision
 * issues especially with orthographic view, where default ray_start is set rather far away.
 * \param region: The region (used for the window width and height).
 * \param v3d: The 3d viewport (used for near clipping value).
 * \param mval: The area relative 2d location (such as `event->mval`, converted into float[2]).
 * \param r_ray_co: The world-space point where the ray intersects the window plane.
 * \param r_ray_normal: The normalized world-space direction of towards mval.
 * \param r_ray_start: The world-space starting point of the ray.
 * \param do_clip_planes: Optionally clip the start of the ray by the view clipping planes.
 * \return success, false if the ray is totally clipped.
 */
bool ED_view3d_win_to_ray_clipped_ex(struct Depsgraph *depsgraph,
                                     const struct ARegion *region,
                                     const struct View3D *v3d,
                                     const float mval[2],
                                     float r_ray_co[3],
                                     float r_ray_normal[3],
                                     float r_ray_start[3],
                                     bool do_clip_planes);
/**
 * Calculate a 3d viewpoint and direction vector from 2d window coordinates.
 * This ray_start is located at the viewpoint, ray_normal is the direction towards `mval`.
 * \param region: The region (used for the window width and height).
 * \param mval: The area relative 2d location (such as `event->mval`, converted into float[2]).
 * \param r_ray_start: The world-space point where the ray intersects the window plane.
 * \param r_ray_normal: The normalized world-space direction of towards mval.
 *
 * \note Ignores view near/far clipping,
 * to take this into account use #ED_view3d_win_to_ray_clipped.
 */
void ED_view3d_win_to_ray(const struct ARegion *region,
                          const float mval[2],
                          float r_ray_start[3],
                          float r_ray_normal[3]);
/**
 * Calculate a normalized 3d direction vector from the viewpoint towards a global location.
 * In orthographic view the resulting vector will match the view vector.
 * \param rv3d: The region (used for the window width and height).
 * \param coord: The world-space location.
 * \param vec: The resulting normalized vector.
 */
void ED_view3d_global_to_vector(const struct RegionView3D *rv3d,
                                const float coord[3],
                                float vec[3]);
/**
 * Calculate a 3d location from 2d window coordinates.
 * \param region: The region (used for the window width and height).
 * \param depth_pt: The reference location used to calculate the Z depth.
 * \param mval: The area relative location (such as `event->mval` converted to floats).
 * \param r_out: The resulting world-space location.
 */
void ED_view3d_win_to_3d(const struct View3D *v3d,
                         const struct ARegion *region,
                         const float depth_pt[3],
                         const float mval[2],
                         float r_out[3]);
void ED_view3d_win_to_3d_int(const struct View3D *v3d,
                             const struct ARegion *region,
                             const float depth_pt[3],
                             const int mval[2],
                             float r_out[3]);
bool ED_view3d_win_to_3d_on_plane(const struct ARegion *region,
                                  const float plane[4],
                                  const float mval[2],
                                  bool do_clip,
                                  float r_out[3]);
/**
 * A wrapper for #ED_view3d_win_to_3d_on_plane that projects onto \a plane_fallback
 * then maps this back to \a plane.
 *
 * This is intended to be used when \a plane is orthogonal to the views Z axis where
 * projecting the \a mval doesn't work well (or fail completely when exactly aligned).
 */
bool ED_view3d_win_to_3d_on_plane_with_fallback(const struct ARegion *region,
                                                const float plane[4],
                                                const float mval[2],
                                                bool do_clip,
                                                const float plane_fallback[4],
                                                float r_out[3]);
bool ED_view3d_win_to_3d_on_plane_int(const struct ARegion *region,
                                      const float plane[4],
                                      const int mval[2],
                                      bool do_clip,
                                      float r_out[3]);
/**
 * Calculate a 3d difference vector from 2d window offset.
 *
 * \note that #ED_view3d_calc_zfac() must be called first to determine
 * the depth used to calculate the delta.
 *
 * When the `zfac` is calculated based on a world-space location directly under the cursor,
 * the value of `r_out` can be subtracted from #RegionView3D.ofs to pan the view
 * with the contents following the cursor perfectly (without sliding).
 *
 * \param region: The region (used for the window width and height).
 * \param xy_delta: 2D difference (in pixels) such as `event->mval[0] - other_x`.
 * \param zfac: The depth result typically calculated by #ED_view3d_calc_zfac
 * (see it's doc-string for details).
 * \param r_out: The resulting world-space delta.
 */
void ED_view3d_win_to_delta(const struct ARegion *region,
                            const float xy_delta[2],
                            float zfac,
                            float r_out[3]);
/**
 * Calculate a 3d origin from 2d window coordinates.
 * \note Orthographic views have a less obvious origin,
 * Since far clip can be a very large value resulting in numeric precision issues,
 * the origin in this case is close to zero coordinate.
 *
 * \param region: The region (used for the window width and height).
 * \param mval: The area relative 2d location (such as `event->mval` converted to float).
 * \param r_out: The resulting normalized world-space direction vector.
 */
void ED_view3d_win_to_origin(const struct ARegion *region, const float mval[2], float r_out[3]);
/**
 * Calculate a 3d direction vector from 2d window coordinates.
 * This direction vector starts and the view in the direction of the 2d window coordinates.
 * In orthographic view all window coordinates yield the same vector.
 *
 * \note doesn't rely on #ED_view3d_calc_zfac
 * for perspective view, get the vector direction to
 * the mouse cursor as a normalized vector.
 *
 * \param region: The region (used for the window width and height).
 * \param mval: The area relative 2d location (such as `event->mval` converted to float).
 * \param r_out: The resulting normalized world-space direction vector.
 */
void ED_view3d_win_to_vector(const struct ARegion *region, const float mval[2], float r_out[3]);
/**
 * Calculate a 3d segment from 2d window coordinates.
 * This ray_start is located at the viewpoint, ray_end is a far point.
 * ray_start and ray_end are clipped by the view near and far limits
 * so points along this line are always in view.
 * In orthographic view all resulting segments will be parallel.
 * \param region: The region (used for the window width and height).
 * \param v3d: The 3d viewport (used for near and far clipping range).
 * \param mval: The area relative 2d location (such as event->mval, converted into float[2]).
 * \param r_ray_start: The world-space starting point of the segment.
 * \param r_ray_end: The world-space end point of the segment.
 * \param do_clip_planes: Optionally clip the ray by the view clipping planes.
 * \return success, false if the segment is totally clipped.
 */
bool ED_view3d_win_to_segment_clipped(const struct Depsgraph *depsgraph,
                                      const struct ARegion *region,
                                      const struct View3D *v3d,
                                      const float mval[2],
                                      float r_ray_start[3],
                                      float r_ray_end[3],
                                      bool do_clip_planes);
void ED_view3d_ob_project_mat_get(const struct RegionView3D *v3d,
                                  const struct Object *ob,
                                  float r_pmat[4][4]);
void ED_view3d_ob_project_mat_get_from_obmat(const struct RegionView3D *rv3d,
                                             const float obmat[4][4],
                                             float r_pmat[4][4]);

/**
 * Convert between region relative coordinates (x,y) and depth component z and
 * a point in world space.
 */
void ED_view3d_project_v3(const struct ARegion *region,
                          const float world[3],
                          float r_region_co[3]);
void ED_view3d_project_v2(const struct ARegion *region,
                          const float world[3],
                          float r_region_co[2]);
bool ED_view3d_unproject_v3(
    const struct ARegion *region, float regionx, float regiony, float regionz, float world[3]);

/* end */

void ED_view3d_dist_range_get(const struct View3D *v3d, float r_dist_range[2]);
/**
 * \note copies logic of #ED_view3d_viewplane_get(), keep in sync.
 */
bool ED_view3d_clip_range_get(const struct Depsgraph *depsgraph,
                              const struct View3D *v3d,
                              const struct RegionView3D *rv3d,
                              float *r_clipsta,
                              float *r_clipend,
                              bool use_ortho_factor);
bool ED_view3d_viewplane_get(struct Depsgraph *depsgraph,
                             const struct View3D *v3d,
                             const struct RegionView3D *rv3d,
                             int winxi,
                             int winyi,
                             struct rctf *r_viewplane,
                             float *r_clipsta,
                             float *r_clipend,
                             float *r_pixsize);

/**
 * Use instead of: `GPU_polygon_offset(rv3d->dist, ...)` see bug T37727.
 */
void ED_view3d_polygon_offset(const struct RegionView3D *rv3d, float dist);

void ED_view3d_calc_camera_border(const struct Scene *scene,
                                  struct Depsgraph *depsgraph,
                                  const struct ARegion *region,
                                  const struct View3D *v3d,
                                  const struct RegionView3D *rv3d,
                                  struct rctf *r_viewborder,
                                  bool no_shift);
void ED_view3d_calc_camera_border_size(const struct Scene *scene,
                                       struct Depsgraph *depsgraph,
                                       const struct ARegion *region,
                                       const struct View3D *v3d,
                                       const struct RegionView3D *rv3d,
                                       float r_size[2]);
bool ED_view3d_calc_render_border(const struct Scene *scene,
                                  struct Depsgraph *depsgraph,
                                  struct View3D *v3d,
                                  struct ARegion *region,
                                  struct rcti *rect);

void ED_view3d_clipping_calc_from_boundbox(float clip[4][4],
                                           const struct BoundBox *clipbb,
                                           bool is_flip);
void ED_view3d_clipping_calc(struct BoundBox *bb,
                             float planes[4][4],
                             const struct ARegion *region,
                             const struct Object *ob,
                             const struct rcti *rect);
/**
 * Clamp min/max by the viewport clipping.
 *
 * \note This is an approximation, with the limitation that the bounding box from the (mix, max)
 * calculation might not have any geometry inside the clipped region.
 * Performing a clipping test on each vertex would work well enough for most cases,
 * although it's not perfect either as edges/faces may intersect the clipping without having any
 * of their vertices inside it.
 * A more accurate result would be quite involved.
 *
 * \return True when the arguments were clamped.
 */
bool ED_view3d_clipping_clamp_minmax(const struct RegionView3D *rv3d, float min[3], float max[3]);

void ED_view3d_clipping_local(struct RegionView3D *rv3d, const float mat[4][4]);
/**
 * Return true when `co` is hidden by the 3D views clipping planes.
 *
 * \param is_local: When true use local (object-space) #ED_view3d_clipping_local must run first,
 * then all comparisons can be done in local-space.
 * \return True when `co` is outside all clipping planes.
 *
 * \note Callers should check #RV3D_CLIPPING_ENABLED first.
 */
bool ED_view3d_clipping_test(const struct RegionView3D *rv3d, const float co[3], bool is_local);

float ED_view3d_radius_to_dist_persp(float angle, float radius);
float ED_view3d_radius_to_dist_ortho(float lens, float radius);
/**
 * Return a new #RegionView3D.dist value to fit the \a radius.
 *
 * \note Depth isn't taken into account, this will fit a flat plane exactly,
 * but points towards the view (with a perspective projection),
 * may be within the radius but outside the view. eg:
 *
 * <pre>
 *           +
 * pt --> + /^ radius
 *         / |
 *        /  |
 * view  +   +
 *        \  |
 *         \ |
 *          \|
 *           +
 * </pre>
 *
 * \param region: Can be NULL if \a use_aspect is false.
 * \param persp: Allow the caller to tell what kind of perspective to use (ortho/view/camera)
 * \param use_aspect: Increase the distance to account for non 1:1 view aspect.
 * \param radius: The radius will be fitted exactly,
 * typically pre-scaled by a margin (#VIEW3D_MARGIN).
 */
float ED_view3d_radius_to_dist(const struct View3D *v3d,
                               const struct ARegion *region,
                               const struct Depsgraph *depsgraph,
                               char persp,
                               bool use_aspect,
                               float radius);

/**
 * Back-buffer select and draw support.
 */
void ED_view3d_backbuf_depth_validate(struct ViewContext *vc);
/**
 * allow for small values [0.5 - 2.5],
 * and large values, FLT_MAX by clamping by the area size
 */
int ED_view3d_backbuf_sample_size_clamp(struct ARegion *region, float dist);

void ED_view3d_select_id_validate(struct ViewContext *vc);

/**
 * Get the world-space 3d location from a screen-space 2d point.
 * TODO: Implement #alphaoverride. We don't want to zoom into billboards.
 *
 * \param mval: Input screen-space pixel location.
 * \param mouse_worldloc: Output world-space location.
 * \param fallback_depth_pt: Use this points depth when no depth can be found.
 */
bool ED_view3d_autodist(struct Depsgraph *depsgraph,
                        struct ARegion *region,
                        struct View3D *v3d,
                        const int mval[2],
                        float mouse_worldloc[3],
                        bool alphaoverride,
                        const float fallback_depth_pt[3]);

/**
 * No 4x4 sampling, run #ED_view3d_depth_override first.
 */
bool ED_view3d_autodist_simple(struct ARegion *region,
                               const int mval[2],
                               float mouse_worldloc[3],
                               int margin,
                               const float *force_depth);
bool ED_view3d_depth_read_cached_seg(
    const ViewDepths *vd, const int mval_sta[2], const int mval_end[2], int margin, float *depth);

/**
 * The default value for the maximum number of elements that can be selected at once
 * using view-port selection.
 *
 * \note in many cases this defines the size of fixed-size stack buffers,
 * so take care increasing this value.
 */
#define MAXPICKELEMS 2500

typedef enum {
  /* all elements in the region, ignore depth */
  VIEW3D_SELECT_ALL = 0,
  /* pick also depth sorts (only for small regions!) */
  VIEW3D_SELECT_PICK_ALL = 1,
  /* sorts and only returns visible objects (only for small regions!) */
  VIEW3D_SELECT_PICK_NEAREST = 2,
} eV3DSelectMode;

typedef enum {
  /** Don't exclude anything. */
  VIEW3D_SELECT_FILTER_NOP = 0,
  /** Don't select objects outside the current mode. */
  VIEW3D_SELECT_FILTER_OBJECT_MODE_LOCK = 1,
  /** A version of #VIEW3D_SELECT_FILTER_OBJECT_MODE_LOCK that allows pose-bone selection. */
  VIEW3D_SELECT_FILTER_WPAINT_POSE_MODE_LOCK = 2,
} eV3DSelectObjectFilter;

eV3DSelectObjectFilter ED_view3d_select_filter_from_mode(const struct Scene *scene,
                                                         const struct Object *obact);

/**
 * Optionally cache data for multiple calls to #view3d_opengl_select
 *
 * just avoid GPU_select headers outside this file
 */
void view3d_opengl_select_cache_begin(void);
void view3d_opengl_select_cache_end(void);

/**
 * \warning be sure to account for a negative return value
 * This is an error, "Too many objects in select buffer"
 * and no action should be taken (can crash blender) if this happens
 *
 * \note (vc->obedit == NULL) can be set to explicitly skip edit-object selection.
 */
int view3d_opengl_select_ex(struct ViewContext *vc,
                            struct GPUSelectResult *buffer,
                            unsigned int buffer_len,
                            const struct rcti *input,
                            eV3DSelectMode select_mode,
                            eV3DSelectObjectFilter select_filter,
                            bool do_material_slot_selection);
int view3d_opengl_select(struct ViewContext *vc,
                         struct GPUSelectResult *buffer,
                         unsigned int buffer_len,
                         const struct rcti *input,
                         eV3DSelectMode select_mode,
                         eV3DSelectObjectFilter select_filter);
int view3d_opengl_select_with_id_filter(struct ViewContext *vc,
                                        struct GPUSelectResult *buffer,
                                        unsigned int buffer_len,
                                        const struct rcti *input,
                                        eV3DSelectMode select_mode,
                                        eV3DSelectObjectFilter select_filter,
                                        uint select_id);

/* view3d_select.cc */

float ED_view3d_select_dist_px(void);
void ED_view3d_viewcontext_init(struct bContext *C,
                                struct ViewContext *vc,
                                struct Depsgraph *depsgraph);

/**
 * Re-initialize `vc` with `obact` as if it's active object (with some differences).
 *
 * This is often used when operating on multiple objects in modes (edit, pose mode etc)
 * where the `vc` is passed in as an argument which then references it's object data.
 *
 * \note members #ViewContext.obedit & #ViewContext.em are only initialized if they're already set,
 * by #ED_view3d_viewcontext_init in most cases.
 * This is necessary because the active object defines the current object-mode.
 * When iterating over objects in object-mode it doesn't make sense to perform
 * an edit-mode action on an object that happens to contain edit-mode data.
 * In some cases these values are cleared allowing the owner of `vc` to explicitly
 * disable edit-mode operation (to force object selection in edit-mode for e.g.).
 * So object-mode specific values should remain cleared when initialized with another object.
 */
void ED_view3d_viewcontext_init_object(struct ViewContext *vc, struct Object *obact);
/**
 * Use this call when executing an operator,
 * event system doesn't set for each event the OpenGL drawing context.
 */
void view3d_operator_needs_opengl(const struct bContext *C);
void view3d_region_operator_needs_opengl(struct wmWindow *win, struct ARegion *region);

/** XXX: should move to BLI_math */
bool edge_inside_circle(const float cent[2],
                        float radius,
                        const float screen_co_a[2],
                        const float screen_co_b[2]);

/**
 * Get 3D region from context, also if mouse is in header or toolbar.
 */
struct RegionView3D *ED_view3d_context_rv3d(struct bContext *C);
/**
 * Ideally would return an rv3d but in some cases the region is needed too
 * so return that, the caller can then access the `region->regiondata`.
 */
bool ED_view3d_context_user_region(struct bContext *C,
                                   struct View3D **r_v3d,
                                   struct ARegion **r_region);
/**
 * Similar to #ED_view3d_context_user_region() but does not use context. Always performs a lookup.
 * Also works if \a v3d is not the active space.
 */
bool ED_view3d_area_user_region(const struct ScrArea *area,
                                const struct View3D *v3d,
                                struct ARegion **r_region);
bool ED_operator_rv3d_user_region_poll(struct bContext *C);

/**
 * Most of the time this isn't needed since you could assume the view matrix was
 * set while drawing, however when functions like mesh_foreachScreenVert are
 * called by selection tools, we can't be sure this object was the last.
 *
 * for example, transparent objects are drawn after edit-mode and will cause
 * the rv3d mat's to change and break selection.
 *
 * 'ED_view3d_init_mats_rv3d' should be called before
 * view3d_project_short_clip and view3d_project_short_noclip in cases where
 * these functions are not used during draw_object
 */
void ED_view3d_init_mats_rv3d(const struct Object *ob, struct RegionView3D *rv3d);
void ED_view3d_init_mats_rv3d_gl(const struct Object *ob, struct RegionView3D *rv3d);
#ifdef DEBUG
/**
 * Ensure we correctly initialize.
 */
void ED_view3d_clear_mats_rv3d(struct RegionView3D *rv3d);
void ED_view3d_check_mats_rv3d(struct RegionView3D *rv3d);
#else
#  define ED_view3d_clear_mats_rv3d(rv3d) (void)(rv3d)
#  define ED_view3d_check_mats_rv3d(rv3d) (void)(rv3d)
#endif

struct RV3DMatrixStore *ED_view3d_mats_rv3d_backup(struct RegionView3D *rv3d);
void ED_view3d_mats_rv3d_restore(struct RegionView3D *rv3d, struct RV3DMatrixStore *rv3dmat);

void ED_draw_object_facemap(struct Depsgraph *depsgraph,
                            struct Object *ob,
                            const float col[4],
                            int facemap);

struct RenderEngineType *ED_view3d_engine_type(const struct Scene *scene, int drawtype);

bool ED_view3d_context_activate(struct bContext *C);
/**
 * Set the correct matrices
 */
void ED_view3d_draw_setup_view(const struct wmWindowManager *wm,
                               struct wmWindow *win,
                               struct Depsgraph *depsgraph,
                               struct Scene *scene,
                               struct ARegion *region,
                               struct View3D *v3d,
                               const float viewmat[4][4],
                               const float winmat[4][4],
                               const struct rcti *rect);

/**
 * `mval` comes from event->mval, only use within region handlers.
 */
struct Base *ED_view3d_give_base_under_cursor(struct bContext *C, const int mval[2]);
struct Object *ED_view3d_give_object_under_cursor(struct bContext *C, const int mval[2]);
struct Object *ED_view3d_give_material_slot_under_cursor(struct bContext *C,
                                                         const int mval[2],
                                                         int *r_material_slot);
bool ED_view3d_is_object_under_cursor(struct bContext *C, const int mval[2]);
/**
 * 'clip' is used to know if our clip setting has changed.
 */
void ED_view3d_quadview_update(struct ScrArea *area, struct ARegion *region, bool do_clip);
/**
 * \note keep this synced with #ED_view3d_mats_rv3d_backup/#ED_view3d_mats_rv3d_restore
 */
void ED_view3d_update_viewmat(struct Depsgraph *depsgraph,
                              const struct Scene *scene,
                              struct View3D *v3d,
                              struct ARegion *region,
                              const float viewmat[4][4],
                              const float winmat[4][4],
                              const struct rcti *rect,
                              bool offscreen);
bool ED_view3d_quat_from_axis_view(char view, char view_axis_roll, float r_quat[4]);
bool ED_view3d_quat_to_axis_view(const float viewquat[4],
                                 float epsilon,
                                 char *r_view,
                                 char *r_view_axis_rotation);
/**
 * A version of #ED_view3d_quat_to_axis_view that updates `viewquat`
 * if it's within `epsilon` to an axis-view.
 *
 * \note Include the special case function since most callers need to perform these operations.
 */
bool ED_view3d_quat_to_axis_view_and_reset_quat(float viewquat[4],
                                                float epsilon,
                                                char *r_view,
                                                char *r_view_axis_rotation);

char ED_view3d_lock_view_from_index(int index);
char ED_view3d_axis_view_opposite(char view);
bool ED_view3d_lock(struct RegionView3D *rv3d);

void ED_view3d_datamask(const struct bContext *C,
                        const struct Scene *scene,
                        const struct View3D *v3d,
                        struct CustomData_MeshMasks *r_cddata_masks);
/**
 * Goes over all modes and view3d settings.
 */
void ED_view3d_screen_datamask(const struct bContext *C,
                               const struct Scene *scene,
                               const struct bScreen *screen,
                               struct CustomData_MeshMasks *r_cddata_masks);

bool ED_view3d_offset_lock_check(const struct View3D *v3d, const struct RegionView3D *rv3d);
/**
 * For viewport operators that exit camera perspective.
 *
 * \note This differs from simply setting `rv3d->persp = persp` because it
 * sets the `ofs` and `dist` values of the viewport so it matches the camera,
 * otherwise switching out of camera view may jump to a different part of the scene.
 */
void ED_view3d_persp_switch_from_camera(const struct Depsgraph *depsgraph,
                                        struct View3D *v3d,
                                        struct RegionView3D *rv3d,
                                        char persp);
/**
 * Action to take when rotating the view,
 * handle auto-perspective and logic for switching out of views.
 *
 * shared with NDOF.
 */
bool ED_view3d_persp_ensure(const struct Depsgraph *depsgraph,
                            struct View3D *v3d,
                            struct ARegion *region);

/* Camera view functions. */

/**
 * Utility to scale zoom level when in camera-view #RegionView3D.camzoom and apply limits.
 * \return true a change was made.
 */
bool ED_view3d_camera_view_zoom_scale(struct RegionView3D *rv3d, const float scale);
/**
 * Utility to pan when in camera view.
 * \param event_ofs: The offset the pan in screen (pixel) coordinates.
 * \return true when a change was made.
 */
bool ED_view3d_camera_view_pan(struct ARegion *region, const float event_ofs[2]);

/* Camera lock functions */

/**
 * \return true when the 3D Viewport is locked to its camera.
 */
bool ED_view3d_camera_lock_check(const struct View3D *v3d, const struct RegionView3D *rv3d);
/**
 * Copy the camera to the view before starting a view transformation.
 *
 * Apply the camera object transformation to the 3D Viewport.
 * (needed so we can use regular 3D Viewport manipulation operators, that sync back to the camera).
 */
void ED_view3d_camera_lock_init_ex(const struct Depsgraph *depsgraph,
                                   struct View3D *v3d,
                                   struct RegionView3D *rv3d,
                                   bool calc_dist);
void ED_view3d_camera_lock_init(const struct Depsgraph *depsgraph,
                                struct View3D *v3d,
                                struct RegionView3D *rv3d);
/**
 * Copy the view to the camera, return true if.
 *
 * Apply the 3D Viewport transformation back to the camera object.
 *
 * \return true if the camera (or one of it's parents) was moved.
 */
bool ED_view3d_camera_lock_sync(const struct Depsgraph *depsgraph,
                                struct View3D *v3d,
                                struct RegionView3D *rv3d);

bool ED_view3d_camera_autokey(const struct Scene *scene,
                              struct ID *id_key,
                              struct bContext *C,
                              bool do_rotate,
                              bool do_translate);
/**
 * Call after modifying a locked view.
 *
 * \note Not every view edit currently auto-keys (numeric-pad for eg),
 * this is complicated because of smooth-view.
 */
bool ED_view3d_camera_lock_autokey(struct View3D *v3d,
                                   struct RegionView3D *rv3d,
                                   struct bContext *C,
                                   bool do_rotate,
                                   bool do_translate);

void ED_view3d_lock_clear(struct View3D *v3d);

/**
 * Check if creating an undo step should be performed if the viewport moves.
 * \return true if #ED_view3d_camera_lock_undo_push would do an undo push.
 */
bool ED_view3d_camera_lock_undo_test(const View3D *v3d,
                                     const RegionView3D *rv3d,
                                     struct bContext *C);

/**
 * Create an undo step when the camera is locked to the view.
 * \param str: The name of the undo step (typically #wmOperatorType.name should be used).
 *
 * \return true when the call to push an undo step was made.
 */
bool ED_view3d_camera_lock_undo_push(const char *str,
                                     const View3D *v3d,
                                     const struct RegionView3D *rv3d,
                                     struct bContext *C);

/**
 * A version of #ED_view3d_camera_lock_undo_push that performs a grouped undo push.
 *
 * \note use for actions that are likely to be repeated such as mouse wheel to zoom,
 * where adding a separate undo step each time isn't desirable.
 */
bool ED_view3d_camera_lock_undo_grouped_push(const char *str,
                                             const View3D *v3d,
                                             const struct RegionView3D *rv3d,
                                             struct bContext *C);

#define VIEW3D_MARGIN 1.4f
#define VIEW3D_DIST_FALLBACK 1.0f

/**
 * This function solves the problem of having to switch between camera and non-camera views.
 *
 * When viewing from the perspective of \a mat, and having the view center \a ofs,
 * this calculates a distance from \a ofs to the matrix \a mat.
 * Using \a fallback_dist when the distance would be too small.
 *
 * \param mat: A matrix use for the view-point (typically the camera objects matrix).
 * \param ofs: Orbit center (negated), matching #RegionView3D.ofs, which is typically passed in.
 * \param fallback_dist: The distance to use if the object is too near or in front of \a ofs.
 * \returns A newly calculated distance or the fallback.
 */
float ED_view3d_offset_distance(const float mat[4][4], const float ofs[3], float fallback_dist);
/**
 * Set the dist without moving the view (compensate with #RegionView3D.ofs)
 *
 * \note take care that #RegionView3d.viewinv is up to date, #ED_view3d_update_viewmat first.
 */
void ED_view3d_distance_set(struct RegionView3D *rv3d, float dist);
/**
 * Change the distance & offset to match the depth of \a dist_co along the view axis.
 *
 * \param dist_co: A world-space location to use for the new depth.
 * \param dist_min: Resulting distances below this will be ignored.
 * \return Success if the distance was set.
 */
bool ED_view3d_distance_set_from_location(struct RegionView3D *rv3d,
                                          const float dist_co[3],
                                          float dist_min);

/**
 * Could move this elsewhere, but tied into #ED_view3d_grid_scale
 */
float ED_scene_grid_scale(const struct Scene *scene, const char **r_grid_unit);
float ED_view3d_grid_scale(const struct Scene *scene,
                           struct View3D *v3d,
                           const char **r_grid_unit);
void ED_view3d_grid_steps(const struct Scene *scene,
                          struct View3D *v3d,
                          struct RegionView3D *rv3d,
                          float r_grid_steps[8]);
/**
 * Simulates the grid scale that is actually viewed.
 * The actual code is seen in `object_grid_frag.glsl` (see `grid_res`).
 * Currently the simulation is only done when RV3D_VIEW_IS_AXIS.
 */
float ED_view3d_grid_view_scale(struct Scene *scene,
                                struct View3D *v3d,
                                struct ARegion *region,
                                const char **r_grid_unit);

/**
 * \note The info that this uses is updated in #ED_refresh_viewport_fps,
 * which currently gets called during #SCREEN_OT_animation_step.
 */
void ED_scene_draw_fps(const struct Scene *scene, int xoffset, int *yoffset);

/* Render */

void ED_view3d_stop_render_preview(struct wmWindowManager *wm, struct ARegion *region);
void ED_view3d_shade_update(struct Main *bmain, struct View3D *v3d, struct ScrArea *area);

#define XRAY_ALPHA(v3d) \
  (((v3d)->shading.type == OB_WIRE) ? (v3d)->shading.xray_alpha_wire : (v3d)->shading.xray_alpha)
#define XRAY_FLAG(v3d) \
  (((v3d)->shading.type == OB_WIRE) ? V3D_SHADING_XRAY_WIREFRAME : V3D_SHADING_XRAY)
#define XRAY_FLAG_ENABLED(v3d) (((v3d)->shading.flag & XRAY_FLAG(v3d)) != 0)
#define XRAY_ENABLED(v3d) (XRAY_FLAG_ENABLED(v3d) && (XRAY_ALPHA(v3d) < 1.0f))
#define XRAY_ACTIVE(v3d) (XRAY_ENABLED(v3d) && ((v3d)->shading.type < OB_MATERIAL))

/* view3d_draw_legacy.c */

/**
 * Try avoid using these more move out of legacy.
 */
void ED_view3d_draw_bgpic_test(const struct Scene *scene,
                               struct Depsgraph *depsgraph,
                               struct ARegion *region,
                               struct View3D *v3d,
                               bool do_foreground,
                               bool do_camera_frame);

/* view3d_gizmo_preselect_type.c */

void ED_view3d_gizmo_mesh_preselect_get_active(struct bContext *C,
                                               struct wmGizmo *gz,
                                               struct Base **r_base,
                                               struct BMElem **r_ele);
void ED_view3d_gizmo_mesh_preselect_clear(struct wmGizmo *gz);

/* space_view3d.c */

void ED_view3d_buttons_region_layout_ex(const struct bContext *C,
                                        struct ARegion *region,
                                        const char *category_override);

/* view3d_view.c */

/**
 * See if current UUID is valid, otherwise set a valid UUID to v3d,
 * Try to keep the same UUID previously used to allow users to quickly toggle back and forth.
 */
bool ED_view3d_local_collections_set(struct Main *bmain, struct View3D *v3d);
void ED_view3d_local_collections_reset(struct bContext *C, bool reset_all);

#ifdef WITH_XR_OPENXR
void ED_view3d_xr_mirror_update(const struct ScrArea *area, const struct View3D *v3d, bool enable);
void ED_view3d_xr_shading_update(struct wmWindowManager *wm,
                                 const View3D *v3d,
                                 const struct Scene *scene);
bool ED_view3d_is_region_xr_mirror_active(const struct wmWindowManager *wm,
                                          const struct View3D *v3d,
                                          const struct ARegion *region);
#endif

#ifdef __cplusplus
}
#endif
