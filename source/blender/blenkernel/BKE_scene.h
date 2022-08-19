/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */
#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_sys_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct AviCodecData;
struct Collection;
struct Depsgraph;
struct GHash;
struct Main;
struct Object;
struct RenderData;
struct Scene;
struct TransformOrientation;
struct UnitSettings;
struct View3DCursor;
struct ViewLayer;

typedef enum eSceneCopyMethod {
  SCE_COPY_NEW = 0,
  SCE_COPY_EMPTY = 1,
  SCE_COPY_LINK_COLLECTION = 2,
  SCE_COPY_FULL = 3,
} eSceneCopyMethod;

/** Use as the contents of a 'for' loop: `for (SETLOOPER(...)) { ... }`. */
#define SETLOOPER(_sce_basis, _sce_iter, _base) \
  _sce_iter = _sce_basis, \
  _base = _setlooper_base_step( \
      &_sce_iter, BKE_view_layer_context_active_PLACEHOLDER(_sce_basis), NULL); \
  _base; \
  _base = _setlooper_base_step(&_sce_iter, NULL, _base)

#define SETLOOPER_VIEW_LAYER(_sce_basis, _view_layer, _sce_iter, _base) \
  _sce_iter = _sce_basis, _base = _setlooper_base_step(&_sce_iter, _view_layer, NULL); \
  _base; \
  _base = _setlooper_base_step(&_sce_iter, NULL, _base)

#define SETLOOPER_SET_ONLY(_sce_basis, _sce_iter, _base) \
  _sce_iter = _sce_basis, _base = _setlooper_base_step(&_sce_iter, NULL, NULL); \
  _base; \
  _base = _setlooper_base_step(&_sce_iter, NULL, _base)

/**
 * Helper function for the #SETLOOPER and #SETLOOPER_VIEW_LAYER macros
 *
 * It iterates over the bases of the active layer and then the bases
 * of the active layer of the background (set) scenes recursively.
 */
struct Base *_setlooper_base_step(struct Scene **sce_iter,
                                  struct ViewLayer *view_layer,
                                  struct Base *base);

void free_avicodecdata(struct AviCodecData *acd);

struct Scene *BKE_scene_add(struct Main *bmain, const char *name);

void BKE_scene_remove_rigidbody_object(struct Main *bmain,
                                       struct Scene *scene,
                                       struct Object *ob,
                                       bool free_us);

/**
 * Check if there is any instance of the object in the scene.
 */
bool BKE_scene_object_find(struct Scene *scene, struct Object *ob);
struct Object *BKE_scene_object_find_by_name(const struct Scene *scene, const char *name);

/* Scene base iteration function.
 * Define struct here, so no need to bother with alloc/free it.
 */
typedef struct SceneBaseIter {
  struct ListBase *duplilist;
  struct DupliObject *dupob;
  float omat[4][4];
  struct Object *dupli_refob;
  int phase;
} SceneBaseIter;

/**
 * Used by meta-balls, return *all* objects (including duplis)
 * existing in the scene (including scene's sets).
 */
int BKE_scene_base_iter_next(struct Depsgraph *depsgraph,
                             struct SceneBaseIter *iter,
                             struct Scene **scene,
                             int val,
                             struct Base **base,
                             struct Object **ob);

void BKE_scene_base_flag_to_objects(struct ViewLayer *view_layer);
/**
 * Synchronize object base flags
 *
 * This is usually handled by the depsgraph.
 * However, in rare occasions we need to use the latest object flags
 * before depsgraph is fully updated.
 *
 * It should (ideally) only run for copy-on-written objects since this is
 * runtime data generated per-view-layer.
 */
void BKE_scene_object_base_flag_sync_from_base(struct Base *base);

/**
 * Sets the active scene, mainly used when running in background mode
 * (`--scene` command line argument).
 * This is also called to set the scene directly, bypassing windowing code.
 * Otherwise #WM_window_set_active_scene is used when changing scenes by the user.
 */
void BKE_scene_set_background(struct Main *bmain, struct Scene *sce);
/**
 * Called from `creator_args.c`.
 */
struct Scene *BKE_scene_set_name(struct Main *bmain, const char *name);

/**
 * \param flag: copying options (see BKE_lib_id.h's `LIB_ID_COPY_...` flags for more).
 */
struct ToolSettings *BKE_toolsettings_copy(struct ToolSettings *toolsettings, int flag);
void BKE_toolsettings_free(struct ToolSettings *toolsettings);

struct Scene *BKE_scene_duplicate(struct Main *bmain, struct Scene *sce, eSceneCopyMethod type);
void BKE_scene_groups_relink(struct Scene *sce);

bool BKE_scene_can_be_removed(const struct Main *bmain, const struct Scene *scene);

bool BKE_scene_has_view_layer(const struct Scene *scene, const struct ViewLayer *layer);
struct Scene *BKE_scene_find_from_collection(const struct Main *bmain,
                                             const struct Collection *collection);

#ifdef DURIAN_CAMERA_SWITCH
struct Object *BKE_scene_camera_switch_find(struct Scene *scene); /* DURIAN_CAMERA_SWITCH */
#endif
bool BKE_scene_camera_switch_update(struct Scene *scene);

const char *BKE_scene_find_marker_name(const struct Scene *scene, int frame);
/**
 * Return the current marker for this frame,
 * we can have more than 1 marker per frame, this just returns the first (unfortunately).
 */
const char *BKE_scene_find_last_marker_name(const struct Scene *scene, int frame);

int BKE_scene_frame_snap_by_seconds(struct Scene *scene, double interval_in_seconds, int frame);

/**
 * Checks for cycle, returns true if it's all OK.
 */
bool BKE_scene_validate_setscene(struct Main *bmain, struct Scene *sce);

/**
 * Return fractional frame number taking into account sub-frames and time
 * remapping. This the time value used by animation, modifiers and physics
 * evaluation. */
float BKE_scene_ctime_get(const struct Scene *scene);
/**
 * Convert integer frame number to fractional frame number taking into account
 * sub-frames and time remapping.
 */
float BKE_scene_frame_to_ctime(const struct Scene *scene, int frame);

/**
 * Get current fractional frame based on frame and sub-frame.
 */
float BKE_scene_frame_get(const struct Scene *scene);
/**
 * Set current frame and sub-frame based on a fractional frame.
 */
void BKE_scene_frame_set(struct Scene *scene, float frame);

struct TransformOrientationSlot *BKE_scene_orientation_slot_get_from_flag(struct Scene *scene,
                                                                          int flag);
struct TransformOrientationSlot *BKE_scene_orientation_slot_get(struct Scene *scene,
                                                                int slot_index);
/**
 * Activate a transform orientation in a 3D view based on an enum value.
 *
 * \param orientation: If this is #V3D_ORIENT_CUSTOM or greater, the custom transform orientation
 * with index \a orientation - #V3D_ORIENT_CUSTOM gets activated.
 */
void BKE_scene_orientation_slot_set_index(struct TransformOrientationSlot *orient_slot,
                                          int orientation);
int BKE_scene_orientation_slot_get_index(const struct TransformOrientationSlot *orient_slot);
int BKE_scene_orientation_get_index(struct Scene *scene, int slot_index);
int BKE_scene_orientation_get_index_from_flag(struct Scene *scene, int flag);

/* **  Scene evaluation ** */

void BKE_scene_update_sound(struct Depsgraph *depsgraph, struct Main *bmain);
void BKE_scene_update_tag_audio_volume(struct Depsgraph *, struct Scene *scene);

void BKE_scene_graph_update_tagged(struct Depsgraph *depsgraph, struct Main *bmain);
void BKE_scene_graph_evaluated_ensure(struct Depsgraph *depsgraph, struct Main *bmain);

void BKE_scene_graph_update_for_newframe(struct Depsgraph *depsgraph);
/**
 * Applies changes right away, does all sets too.
 */
void BKE_scene_graph_update_for_newframe_ex(struct Depsgraph *depsgraph, bool clear_recalc);

/**
 * Ensures given scene/view_layer pair has a valid, up-to-date depsgraph.
 *
 * \warning Sets matching depsgraph as active,
 * so should only be called from the active editing context (usually, from operators).
 */
void BKE_scene_view_layer_graph_evaluated_ensure(struct Main *bmain,
                                                 struct Scene *scene,
                                                 struct ViewLayer *view_layer);

/**
 * Return default view.
 */
struct SceneRenderView *BKE_scene_add_render_view(struct Scene *sce, const char *name);
bool BKE_scene_remove_render_view(struct Scene *scene, struct SceneRenderView *srv);

/* Render profile. */

int get_render_subsurf_level(const struct RenderData *r, int lvl, bool for_render);
int get_render_child_particle_number(const struct RenderData *r, int child_num, bool for_render);

bool BKE_scene_use_shading_nodes_custom(struct Scene *scene);
bool BKE_scene_use_spherical_stereo(struct Scene *scene);

bool BKE_scene_uses_blender_eevee(const struct Scene *scene);
bool BKE_scene_uses_blender_workbench(const struct Scene *scene);
bool BKE_scene_uses_cycles(const struct Scene *scene);

/**
 * Return whether the Cycles experimental feature is enabled. It is invalid to call without first
 * ensuring that Cycles is the active render engine (e.g. with #BKE_scene_uses_cycles).
 *
 * \note We cannot use `const` as RNA_id_pointer_create is not using a const ID.
 */
bool BKE_scene_uses_cycles_experimental_features(struct Scene *scene);

void BKE_scene_copy_data_eevee(struct Scene *sce_dst, const struct Scene *sce_src);

void BKE_scene_disable_color_management(struct Scene *scene);
bool BKE_scene_check_color_management_enabled(const struct Scene *scene);
bool BKE_scene_check_rigidbody_active(const struct Scene *scene);

int BKE_scene_num_threads(const struct Scene *scene);
int BKE_render_num_threads(const struct RenderData *r);

void BKE_render_resolution(const struct RenderData *r,
                           const bool use_crop,
                           int *r_width,
                           int *r_height);
int BKE_render_preview_pixel_size(const struct RenderData *r);

/**********************************/

/**
 * Apply the needed correction factor to value, based on unit_type
 * (only length-related are affected currently) and `unit->scale_length`.
 */
double BKE_scene_unit_scale(const struct UnitSettings *unit, int unit_type, double value);

/* Multi-view. */

bool BKE_scene_multiview_is_stereo3d(const struct RenderData *rd);
/**
 * Return whether to render this #SceneRenderView.
 */
bool BKE_scene_multiview_is_render_view_active(const struct RenderData *rd,
                                               const struct SceneRenderView *srv);
/**
 * \return true if `viewname` is the first or if the name is NULL or not found.
 */
bool BKE_scene_multiview_is_render_view_first(const struct RenderData *rd, const char *viewname);
/**
 * \return true if `viewname` is the last or if the name is NULL or not found.
 */
bool BKE_scene_multiview_is_render_view_last(const struct RenderData *rd, const char *viewname);
int BKE_scene_multiview_num_views_get(const struct RenderData *rd);
struct SceneRenderView *BKE_scene_multiview_render_view_findindex(const struct RenderData *rd,
                                                                  int view_id);
const char *BKE_scene_multiview_render_view_name_get(const struct RenderData *rd, int view_id);
int BKE_scene_multiview_view_id_get(const struct RenderData *rd, const char *viewname);
void BKE_scene_multiview_filepath_get(const struct SceneRenderView *srv,
                                      const char *filepath,
                                      char *r_filepath);
/**
 * When multi-view is not used the `filepath` is as usual (e.g., `Image.jpg`).
 * When multi-view is on, even if only one view is enabled the view is incorporated
 * into the file name (e.g., `Image_L.jpg`). That allows for the user to re-render
 * individual views.
 */
void BKE_scene_multiview_view_filepath_get(const struct RenderData *rd,
                                           const char *filepath,
                                           const char *view,
                                           char *r_filepath);
const char *BKE_scene_multiview_view_suffix_get(const struct RenderData *rd, const char *viewname);
const char *BKE_scene_multiview_view_id_suffix_get(const struct RenderData *rd, int view_id);
void BKE_scene_multiview_view_prefix_get(struct Scene *scene,
                                         const char *name,
                                         char *r_prefix,
                                         const char **r_ext);
void BKE_scene_multiview_videos_dimensions_get(
    const struct RenderData *rd, size_t width, size_t height, size_t *r_width, size_t *r_height);
int BKE_scene_multiview_num_videos_get(const struct RenderData *rd);

/* depsgraph */
void BKE_scene_allocate_depsgraph_hash(struct Scene *scene);
void BKE_scene_ensure_depsgraph_hash(struct Scene *scene);
void BKE_scene_free_depsgraph_hash(struct Scene *scene);
void BKE_scene_free_view_layer_depsgraph(struct Scene *scene, struct ViewLayer *view_layer);

/**
 * \note Do not allocate new depsgraph.
 */
struct Depsgraph *BKE_scene_get_depsgraph(const struct Scene *scene,
                                          const struct ViewLayer *view_layer);
/**
 * \note Allocate new depsgraph if necessary.
 */
struct Depsgraph *BKE_scene_ensure_depsgraph(struct Main *bmain,
                                             struct Scene *scene,
                                             struct ViewLayer *view_layer);

struct GHash *BKE_scene_undo_depsgraphs_extract(struct Main *bmain);
void BKE_scene_undo_depsgraphs_restore(struct Main *bmain, struct GHash *depsgraph_extract);

void BKE_scene_transform_orientation_remove(struct Scene *scene,
                                            struct TransformOrientation *orientation);
struct TransformOrientation *BKE_scene_transform_orientation_find(const struct Scene *scene,
                                                                  int index);
/**
 * \return the index that \a orientation has within \a scene's transform-orientation list
 * or -1 if not found.
 */
int BKE_scene_transform_orientation_get_index(const struct Scene *scene,
                                              const struct TransformOrientation *orientation);

void BKE_scene_cursor_rot_to_mat3(const struct View3DCursor *cursor, float mat[3][3]);
void BKE_scene_cursor_mat3_to_rot(struct View3DCursor *cursor,
                                  const float mat[3][3],
                                  bool use_compat);

void BKE_scene_cursor_rot_to_quat(const struct View3DCursor *cursor, float quat[4]);
void BKE_scene_cursor_quat_to_rot(struct View3DCursor *cursor,
                                  const float quat[4],
                                  bool use_compat);

void BKE_scene_cursor_to_mat4(const struct View3DCursor *cursor, float mat[4][4]);
void BKE_scene_cursor_from_mat4(struct View3DCursor *cursor,
                                const float mat[4][4],
                                bool use_compat);

#ifdef __cplusplus
}
#endif
