/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup wm
 */

#pragma once

#include "CLG_log.h"

#include "wm_xr.h"

struct wmXrActionSet;
struct GHash;

typedef struct wmXrEyeData {
  int width, height;
  float focal_len;
  float viewmat[4][4];
  float winmat[4][4];
} wmXrEyeData;

typedef struct wmXrControllerData {
  /** OpenXR path identifier. */
  char subaction_path[64];
  /** Last known controller pose (in world space) stored for queries. */
  GHOST_XrPose pose;
  /** The last known controller matrix, calculated from above's controller pose. */
  float mat[4][4];
  /** Mesh object, used to draw the controller. */
  Object *ob;
} wmXrControllerData;

typedef struct wmXrSessionState {
  bool is_started;

  /** Last known viewer pose (centroid of eyes, in world space) stored for queries. */
  GHOST_XrPose viewer_pose;
  /** The last known view matrix, calculated from above's viewer pose. */
  float viewer_viewmat[4][4];
  /** Last known eye data. */
  wmXrEyeData eyes[2];

  /** Copy of XrSessionSettings.base_pose_ data to detect changes that need
   * resetting to base pose. */
  char prev_base_pose_type; /* eXRSessionBasePoseType */
  Object *prev_base_pose_object;
  /** Copy of XrSessionSettings.flag created on the last draw call, stored to detect changes. */
  int prev_settings_flag;
  /** Copy of wmXrDrawData.base_pose. */
  GHOST_XrPose prev_base_pose;
  /** Copy of GHOST_XrDrawViewInfo.local_pose. */
  GHOST_XrPose prev_local_pose;
  /** Copy of wmXrDrawData.eye_position_ofs. */
  float prev_eye_position_ofs[3];

  bool force_reset_to_base_pose;
  bool is_view_data_set;

  /** Last known controller data. */
  wmXrControllerData controllers[2];

  struct GHash *action_sets; /* wmXrActionSet */
  /** Shared pointer with the GHash. The currently active action set that will be updated
   * on calls to wm_xr_session_actions_update().
   * If NULL, all action sets will be treated as active and updated. */
  struct wmXrActionSet *active_action_set;
} wmXrSessionState;

typedef struct wmXrRuntimeData {
  GHOST_XrContextHandle context;

  /** The context the session was started in. Stored to execute Python handlers
   * for "xr_session_start_pre". Afterwards, this may be an invalid reference. */
  struct bContext *bcontext;

  /** The window the session was started in. Stored to be able to follow its view-layer. This may
   * be an invalid reference, i.e. the window may have been closed. */
  wmWindow *session_root_win;

  /** Although this struct is internal, RNA gets a handle to this for state information queries. */
  wmXrSessionState session_state;
  wmXrSessionExitFn exit_fn;
} wmXrRuntimeData;

typedef struct {
  struct GPUOffScreen *offscreen;
  struct GPUViewport *viewport;

  /** XR events. */
  ListBase events;

  /** Dummy region type. Used to add draw callbacks. */
  struct ARegionType *art;
  /** Controller draw callback handle. */
  void *controller_draw_handle;
} wmXrSurfaceData;

typedef struct wmXrDrawData {
  struct Scene *scene;
  struct Depsgraph *depsgraph;

  wmXrData *xr_data;
  wmXrSurfaceData *surface_data;

  /** The pose (location + rotation) to which eye deltas will be applied to when drawing (world
   * space). With positional tracking enabled, it should be the same as the base pose, when
   * disabled it also contains a location delta from the moment the option was toggled. */
  GHOST_XrPose base_pose;
  /** Offset to _substract_ from the OpenXR eye and viewer pose to get the wanted effective pose
   * (e.g. a pose exactly at the landmark position). */
  float eye_position_ofs[3]; /* Local/view space. */
} wmXrDrawData;

/** Same as GHOST_XrActionInfo but with non-const strings. */
typedef struct wmXrAction {
  char *name;
  GHOST_XrActionType type;
  unsigned int count_subaction_paths;
  char **subaction_paths;
  /** States for each subaction path. */
  void *states;
  /** Previous states, stored to determine XR events. */
  void *states_prev;

  /** Input threshold for float actions. */
  float threshold;

  /** Operator to be called on XR events. */
  struct wmOperatorType *ot;
  IDProperty *op_properties;
  char op_flag; /* wmXrOpFlag */
} wmXrAction;

typedef struct wmXrActionSet {
  char *name;
  struct GHash *actions; /* wmXrAction */
  /** Shared pointer with the GHash. The XR pose action that determines the controller
   * transforms. This is usually identified by the OpenXR path "/grip/pose" or "/aim/pose",
   * although it could differ depending on the specification and hardware. */
  wmXrAction *controller_pose_action;
} wmXrActionSet;

wmXrRuntimeData *wm_xr_runtime_data_create(void);
void wm_xr_runtime_data_free(wmXrRuntimeData **runtime);

wmWindow *wm_xr_session_root_window_or_fallback_get(const wmWindowManager *wm,
                                                    const wmXrRuntimeData *runtime_data);
void wm_xr_session_draw_data_update(const wmXrSessionState *state,
                                    const XrSessionSettings *settings,
                                    const GHOST_XrDrawViewInfo *draw_view,
                                    wmXrDrawData *draw_data);
void wm_xr_session_state_update(const XrSessionSettings *settings,
                                const wmXrDrawData *draw_data,
                                const GHOST_XrDrawViewInfo *draw_view,
                                const float viewmat[4][4],
                                const float winmat[4][4],
                                wmXrSessionState *state);
bool wm_xr_session_surface_offscreen_ensure(wmXrSurfaceData *surface_data,
                                            const GHOST_XrDrawViewInfo *draw_view);
void *wm_xr_session_gpu_binding_context_create(void);
void wm_xr_session_gpu_binding_context_destroy(GHOST_ContextHandle context);

void wm_xr_session_actions_init(wmXrData *xr);
void wm_xr_session_actions_update(wmXrData *xr);
void wm_xr_session_actions_uninit(wmXrData *xr);
void wm_xr_session_controller_data_populate(const wmXrAction *controller_pose_action,
                                            struct bContext *C,
                                            wmXrSessionState *state);
void wm_xr_session_controller_data_clear(unsigned int count_subaction_paths,
                                         struct bContext *C,
                                         wmXrSessionState *state);

void wm_xr_pose_to_viewmat(const GHOST_XrPose *pose, float r_viewmat[4][4]);
void wm_xr_controller_pose_to_mat(const GHOST_XrPose *pose, float r_mat[4][4]);
void wm_xr_draw_view(const GHOST_XrDrawViewInfo *draw_view, void *customdata);
void wm_xr_draw_controllers(const struct bContext *C, struct ARegion *region, void *customdata);
