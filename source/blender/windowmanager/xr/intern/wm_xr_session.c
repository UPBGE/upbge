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

#include "BKE_callbacks.h"
#include "BKE_context.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_scene.h"
#include "BKE_screen.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math.h"

#include "DEG_depsgraph.h"

#include "DNA_camera_types.h"
#include "DNA_object_types.h"

#include "DRW_engine.h"

#include "ED_object.h"
#include "ED_space_api.h"

#include "GHOST_C-api.h"

#include "GPU_viewport.h"

#include "MEM_guardedalloc.h"

#include "WM_api.h"
#include "WM_types.h"

#include "wm_event_system.h"
#include "wm_surface.h"
#include "wm_window.h"
#include "wm_xr_intern.h"

static wmSurface *g_xr_surface = NULL;
static CLG_LogRef LOG = {"wm.xr"};

/* -------------------------------------------------------------------- */

static void wm_xr_session_create_cb(void *customdata)
{
  wmXrData *xr_data = customdata;
  Main *bmain = CTX_data_main(xr_data->runtime->bcontext);

  /* Get action set data from Python. */
  BKE_callback_exec_null(bmain, BKE_CB_EVT_XR_SESSION_START_PRE);

  wm_xr_session_actions_init(xr_data);
}

static void wm_xr_session_exit_cb(void *customdata)
{
  wmXrData *xr_data = customdata;

  xr_data->runtime->session_state.is_started = false;

  wm_xr_session_actions_uninit(xr_data);

  if (xr_data->runtime->exit_fn) {
    xr_data->runtime->exit_fn(xr_data);
  }

  /* Free the entire runtime data (including session state and context), to play safe. */
  wm_xr_runtime_data_free(&xr_data->runtime);
}

static void wm_xr_session_begin_info_create(wmXrData *xr_data,
                                            GHOST_XrSessionBeginInfo *r_begin_info)
{
  /* Callback for when the session is created. This is needed to create and bind OpenXR actions
   * after the session is created but before it is started. */
  r_begin_info->create_fn = wm_xr_session_create_cb;
  r_begin_info->create_customdata = xr_data;

  /* WM-XR exit function, does some own stuff and calls callback passed to wm_xr_session_toggle(),
   * to allow external code to execute its own session-exit logic. */
  r_begin_info->exit_fn = wm_xr_session_exit_cb;
  r_begin_info->exit_customdata = xr_data;
}

void wm_xr_session_toggle(bContext *C,
                          wmWindowManager *wm,
                          wmWindow *session_root_win,
                          wmXrSessionExitFn session_exit_fn)
{
  wmXrData *xr_data = &wm->xr;

  if (WM_xr_session_exists(xr_data)) {
    GHOST_XrSessionEnd(xr_data->runtime->context);
  }
  else {
    GHOST_XrSessionBeginInfo begin_info;

    xr_data->runtime->bcontext = C;
    xr_data->runtime->session_root_win = session_root_win;
    xr_data->runtime->session_state.is_started = true;
    xr_data->runtime->exit_fn = session_exit_fn;

    wm_xr_session_begin_info_create(xr_data, &begin_info);
    GHOST_XrSessionStart(xr_data->runtime->context, &begin_info);
  }
}

/**
 * Check if the XR-Session was triggered.
 * If an error happened while trying to start a session, this returns false too.
 */
bool WM_xr_session_exists(const wmXrData *xr)
{
  return xr->runtime && xr->runtime->context && xr->runtime->session_state.is_started;
}

void WM_xr_session_base_pose_reset(wmXrData *xr)
{
  xr->runtime->session_state.force_reset_to_base_pose = true;
}

/**
 * Check if the session is running, according to the OpenXR definition.
 */
bool WM_xr_session_is_ready(const wmXrData *xr)
{
  return WM_xr_session_exists(xr) && GHOST_XrSessionIsRunning(xr->runtime->context);
}

static void wm_xr_session_base_pose_calc(const Scene *scene,
                                         const XrSessionSettings *settings,
                                         GHOST_XrPose *r_base_pose)
{
  const Object *base_pose_object = ((settings->base_pose_type == XR_BASE_POSE_OBJECT) &&
                                    settings->base_pose_object) ?
                                       settings->base_pose_object :
                                       scene->camera;

  if (settings->base_pose_type == XR_BASE_POSE_CUSTOM) {
    float tmp_quatx[4], tmp_quatz[4];

    copy_v3_v3(r_base_pose->position, settings->base_pose_location);
    axis_angle_to_quat_single(tmp_quatx, 'X', M_PI_2);
    axis_angle_to_quat_single(tmp_quatz, 'Z', settings->base_pose_angle);
    mul_qt_qtqt(r_base_pose->orientation_quat, tmp_quatz, tmp_quatx);
  }
  else if (base_pose_object) {
    float tmp_quat[4];
    float tmp_eul[3];

    mat4_to_loc_quat(r_base_pose->position, tmp_quat, base_pose_object->obmat);

    /* Only use rotation around Z-axis to align view with floor. */
    quat_to_eul(tmp_eul, tmp_quat);
    tmp_eul[0] = M_PI_2;
    tmp_eul[1] = 0;
    eul_to_quat(r_base_pose->orientation_quat, tmp_eul);
  }
  else {
    copy_v3_fl(r_base_pose->position, 0.0f);
    axis_angle_to_quat_single(r_base_pose->orientation_quat, 'X', M_PI_2);
  }
}

static void wm_xr_session_draw_data_populate(wmXrData *xr_data,
                                             Scene *scene,
                                             Depsgraph *depsgraph,
                                             wmXrDrawData *r_draw_data)
{
  const XrSessionSettings *settings = &xr_data->session_settings;

  memset(r_draw_data, 0, sizeof(*r_draw_data));
  r_draw_data->scene = scene;
  r_draw_data->depsgraph = depsgraph;
  r_draw_data->xr_data = xr_data;
  r_draw_data->surface_data = g_xr_surface->customdata;

  wm_xr_session_base_pose_calc(r_draw_data->scene, settings, &r_draw_data->base_pose);
}

wmWindow *wm_xr_session_root_window_or_fallback_get(const wmWindowManager *wm,
                                                    const wmXrRuntimeData *runtime_data)
{
  if (runtime_data->session_root_win &&
      BLI_findindex(&wm->windows, runtime_data->session_root_win) != -1) {
    /* Root window is still valid, use it. */
    return runtime_data->session_root_win;
  }
  /* Otherwise, fallback. */
  return wm->windows.first;
}

/**
 * Get the scene and depsgraph shown in the VR session's root window (the window the session was
 * started from) if still available. If it's not available, use some fallback window.
 *
 * It's important that the VR session follows some existing window, otherwise it would need to have
 * an own depsgraph, which is an expense we should avoid.
 */
static void wm_xr_session_scene_and_evaluated_depsgraph_get(Main *bmain,
                                                            const wmWindowManager *wm,
                                                            Scene **r_scene,
                                                            Depsgraph **r_depsgraph)
{
  const wmWindow *root_win = wm_xr_session_root_window_or_fallback_get(wm, wm->xr.runtime);

  /* Follow the scene & view layer shown in the root 3D View. */
  Scene *scene = WM_window_get_active_scene(root_win);
  ViewLayer *view_layer = WM_window_get_active_view_layer(root_win);

  Depsgraph *depsgraph = BKE_scene_get_depsgraph(scene, view_layer);
  BLI_assert(scene && view_layer && depsgraph);
  BKE_scene_graph_evaluated_ensure(depsgraph, bmain);
  *r_scene = scene;
  *r_depsgraph = depsgraph;
}

typedef enum wmXrSessionStateEvent {
  SESSION_STATE_EVENT_NONE = 0,
  SESSION_STATE_EVENT_START,
  SESSION_STATE_EVENT_RESET_TO_BASE_POSE,
  SESSION_STATE_EVENT_POSITON_TRACKING_TOGGLE,
} wmXrSessionStateEvent;

static bool wm_xr_session_draw_data_needs_reset_to_base_pose(const wmXrSessionState *state,
                                                             const XrSessionSettings *settings)
{
  if (state->force_reset_to_base_pose) {
    return true;
  }
  return ((settings->flag & XR_SESSION_USE_POSITION_TRACKING) == 0) &&
         ((state->prev_base_pose_type != settings->base_pose_type) ||
          (state->prev_base_pose_object != settings->base_pose_object));
}

static wmXrSessionStateEvent wm_xr_session_state_to_event(const wmXrSessionState *state,
                                                          const XrSessionSettings *settings)
{
  if (!state->is_view_data_set) {
    return SESSION_STATE_EVENT_START;
  }
  if (wm_xr_session_draw_data_needs_reset_to_base_pose(state, settings)) {
    return SESSION_STATE_EVENT_RESET_TO_BASE_POSE;
  }

  const bool position_tracking_toggled = ((state->prev_settings_flag &
                                           XR_SESSION_USE_POSITION_TRACKING) !=
                                          (settings->flag & XR_SESSION_USE_POSITION_TRACKING));
  if (position_tracking_toggled) {
    return SESSION_STATE_EVENT_POSITON_TRACKING_TOGGLE;
  }

  return SESSION_STATE_EVENT_NONE;
}

void wm_xr_session_draw_data_update(const wmXrSessionState *state,
                                    const XrSessionSettings *settings,
                                    const GHOST_XrDrawViewInfo *draw_view,
                                    wmXrDrawData *draw_data)
{
  const wmXrSessionStateEvent event = wm_xr_session_state_to_event(state, settings);
  const bool use_position_tracking = (settings->flag & XR_SESSION_USE_POSITION_TRACKING);

  switch (event) {
    case SESSION_STATE_EVENT_START:
      if (use_position_tracking) {
        /* We want to start the session exactly at landmark position.
         * Run-times may have a non-[0,0,0] starting position that we have to subtract for that. */
        copy_v3_v3(draw_data->eye_position_ofs, draw_view->local_pose.position);
      }
      else {
        copy_v3_fl(draw_data->eye_position_ofs, 0.0f);
      }
      break;
      /* This should be triggered by the VR add-on if a landmark changes. */
    case SESSION_STATE_EVENT_RESET_TO_BASE_POSE:
      if (use_position_tracking) {
        /* Switch exactly to base pose, so use eye offset to cancel out current position delta. */
        copy_v3_v3(draw_data->eye_position_ofs, draw_view->local_pose.position);
      }
      else {
        copy_v3_fl(draw_data->eye_position_ofs, 0.0f);
      }
      break;
    case SESSION_STATE_EVENT_POSITON_TRACKING_TOGGLE:
      if (use_position_tracking) {
        /* Keep the current position, and let the user move from there. */
        copy_v3_v3(draw_data->eye_position_ofs, state->prev_eye_position_ofs);
      }
      else {
        /* Back to the exact base-pose position. */
        copy_v3_fl(draw_data->eye_position_ofs, 0.0f);
      }
      break;
    case SESSION_STATE_EVENT_NONE:
      /* Keep previous offset when positional tracking is disabled. */
      copy_v3_v3(draw_data->eye_position_ofs, state->prev_eye_position_ofs);
      break;
  }
}

/**
 * Update information that is only stored for external state queries. E.g. for Python API to
 * request the current (as in, last known) viewer pose.
 * Controller data and action sets will be updated separately via wm_xr_session_actions_update().
 */
void wm_xr_session_state_update(const XrSessionSettings *settings,
                                const wmXrDrawData *draw_data,
                                const GHOST_XrDrawViewInfo *draw_view,
                                const float viewmat[4][4],
                                const float winmat[4][4],
                                wmXrSessionState *state)
{
  GHOST_XrPose viewer_pose;
  const bool use_position_tracking = settings->flag & XR_SESSION_USE_POSITION_TRACKING;
  wmXrEyeData *eye = &state->eyes[draw_view->view];

  mul_qt_qtqt(viewer_pose.orientation_quat,
              draw_data->base_pose.orientation_quat,
              draw_view->local_pose.orientation_quat);
  copy_v3_v3(viewer_pose.position, draw_data->base_pose.position);
  /* The local pose and the eye pose (which is copied from an earlier local pose) both are view
   * space, so Y-up. In this case we need them in regular Z-up. */
  viewer_pose.position[0] -= draw_data->eye_position_ofs[0];
  viewer_pose.position[1] += draw_data->eye_position_ofs[2];
  viewer_pose.position[2] -= draw_data->eye_position_ofs[1];
  if (use_position_tracking) {
    viewer_pose.position[0] += draw_view->local_pose.position[0];
    viewer_pose.position[1] -= draw_view->local_pose.position[2];
    viewer_pose.position[2] += draw_view->local_pose.position[1];
  }

  copy_v3_v3(state->viewer_pose.position, viewer_pose.position);
  copy_qt_qt(state->viewer_pose.orientation_quat, viewer_pose.orientation_quat);
  wm_xr_pose_to_viewmat(&viewer_pose, state->viewer_viewmat);

  eye->width = draw_view->width;
  eye->height = draw_view->height;
  /* No idea why, but multiplying by two seems to make it match the VR view more. */
  eye->focal_len = 2.0f *
                   fov_to_focallength(draw_view->fov.angle_right - draw_view->fov.angle_left,
                                      DEFAULT_SENSOR_WIDTH);
  copy_m4_m4(eye->viewmat, viewmat);
  copy_m4_m4(eye->winmat, winmat);

  memcpy(&state->prev_base_pose, &draw_data->base_pose, sizeof(GHOST_XrPose));
  memcpy(&state->prev_local_pose, &draw_view->local_pose, sizeof(GHOST_XrPose));
  copy_v3_v3(state->prev_eye_position_ofs, draw_data->eye_position_ofs);

  state->prev_settings_flag = settings->flag;
  state->prev_base_pose_type = settings->base_pose_type;
  state->prev_base_pose_object = settings->base_pose_object;
  state->is_view_data_set = true;
  /* Assume this was already done through wm_xr_session_draw_data_update(). */
  state->force_reset_to_base_pose = false;
}

wmXrSessionState *WM_xr_session_state_handle_get(const wmXrData *xr)
{
  return xr->runtime ? &xr->runtime->session_state : NULL;
}

bool WM_xr_session_state_viewer_pose_location_get(const wmXrData *xr, float r_location[3])
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set) {
    zero_v3(r_location);
    return false;
  }

  copy_v3_v3(r_location, xr->runtime->session_state.viewer_pose.position);
  return true;
}

bool WM_xr_session_state_viewer_pose_rotation_get(const wmXrData *xr, float r_rotation[4])
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set) {
    unit_qt(r_rotation);
    return false;
  }

  copy_v4_v4(r_rotation, xr->runtime->session_state.viewer_pose.orientation_quat);
  return true;
}

bool WM_xr_session_state_viewer_pose_matrix_info_get(const wmXrData *xr,
                                                     bool from_selection_eye,
                                                     float r_viewmat[4][4],
                                                     float *r_focal_len,
                                                     float *r_clip_start,
                                                     float *r_clip_end)
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set) {
    unit_m4(r_viewmat);
    *r_focal_len = *r_clip_start = *r_clip_end = 0.0f;
    return false;
  }

  const wmXrEyeData *eye = &xr->runtime->session_state.eyes[xr->session_settings.selection_eye];
  if (from_selection_eye) {
    copy_m4_m4(r_viewmat, eye->viewmat);
  }
  else {
    copy_m4_m4(r_viewmat, xr->runtime->session_state.viewer_viewmat);
  }
  /* Since eye centroid does not have a focal length, just take it from selection eye. */
  *r_focal_len = eye->focal_len;
  *r_clip_start = xr->session_settings.clip_start;
  *r_clip_end = xr->session_settings.clip_end;

  return true;
}

bool WM_xr_session_state_controller_pose_location_get(const wmXrData *xr,
                                                      unsigned int subaction_idx,
                                                      float r_location[3])
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set ||
      subaction_idx > 1) {
    zero_v3(r_location);
    return false;
  }

  copy_v3_v3(r_location, xr->runtime->session_state.controllers[subaction_idx].pose.position);
  return true;
}

bool WM_xr_session_state_controller_pose_rotation_get(const wmXrData *xr,
                                                      unsigned int subaction_idx,
                                                      float r_rotation[4])
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set ||
      subaction_idx > 1) {
    unit_qt(r_rotation);
    return false;
  }

  copy_v4_v4(r_rotation,
             xr->runtime->session_state.controllers[subaction_idx].pose.orientation_quat);
  return true;
}

/* -------------------------------------------------------------------- */
/** \name XR-Session Actions
 *
 * XR action processing and event dispatching.
 *
 * \{ */

void wm_xr_session_actions_init(wmXrData *xr)
{
  if (!xr->runtime) {
    return;
  }

  GHash *action_sets = xr->runtime->session_state.action_sets;
  bool attach_actions = false;

  /* Check if there are any created actions. */
  if (action_sets && BLI_ghash_len(action_sets) > 0) {
    GHashIterator *ghi = BLI_ghashIterator_new(action_sets);
    GHASH_ITER (*ghi, action_sets) {
      wmXrActionSet *action_set = BLI_ghashIterator_getValue(ghi);
      if (action_set && action_set->actions && BLI_ghash_len(action_set->actions) > 0) {
        attach_actions = true;
        break;
      }
    }
    BLI_ghashIterator_free(ghi);
  }

  if (attach_actions) {
    GHOST_XrAttachActionSets(xr->runtime->context);
  }
}

static void wm_xr_session_controller_mats_update(const XrSessionSettings *settings,
                                                 const wmXrAction *controller_pose_action,
                                                 wmXrSessionState *state,
                                                 ViewLayer *view_layer)
{
  const unsigned int count = (unsigned int)min_ii(
      (int)controller_pose_action->count_subaction_paths, 2);

  float view_ofs[3];
  float base_inv[4][4];
  float tmp[4][4];

  copy_v3_v3(view_ofs, state->prev_eye_position_ofs);
  if ((settings->flag & XR_SESSION_USE_POSITION_TRACKING) == 0) {
    add_v3_v3(view_ofs, state->prev_local_pose.position);
  }

  wm_xr_pose_to_viewmat(&state->prev_base_pose, base_inv);
  invert_m4(base_inv);

  for (unsigned int i = 0; i < count; ++i) {
    wmXrControllerData *controller = &state->controllers[i];

    /* Calculate controller matrix in world space. */
    wm_xr_controller_pose_to_mat(&((GHOST_XrPose *)controller_pose_action->states)[i], tmp);

    /* Apply eye position and base pose offsets. */
    sub_v3_v3(tmp[3], view_ofs);
    mul_m4_m4m4(controller->mat, base_inv, tmp);

    /* Save final pose. */
    mat4_to_loc_quat(
        controller->pose.position, controller->pose.orientation_quat, controller->mat);

    if (controller->ob) {
      /* TODO_XR: Handle case where object was deleted but then undone. */
      Base *base = BKE_view_layer_base_find(view_layer, controller->ob);
      if (base) {
        copy_v3_v3(controller->ob->loc, controller->pose.position);
        quat_to_eul(controller->ob->rot, controller->pose.orientation_quat);
        DEG_id_tag_update(&controller->ob->id, ID_RECALC_TRANSFORM);
      }
    }
  }
}

static const GHOST_XrPose *wm_xr_session_controller_pose_find(const wmXrSessionState *state,
                                                              const char *subaction_path)
{
  for (unsigned int i = 0; i < (unsigned int)ARRAY_SIZE(state->controllers); ++i) {
    if (STREQ(state->controllers[i].subaction_path, subaction_path)) {
      return &state->controllers[i].pose;
    }
  }

  return NULL;
}

/* Dispatch events to XR surface / window queues. */
static void wm_xr_session_events_dispatch(const XrSessionSettings *settings,
                                          const char *action_set_name,
                                          GHash *actions,
                                          wmXrSessionState *session_state,
                                          wmSurface *surface,
                                          wmWindow *win)
{
  GHashIterator *ghi = BLI_ghashIterator_new(actions);
  GHASH_ITER (*ghi, actions) {
    wmXrAction *action = BLI_ghashIterator_getValue(ghi);
    if (action && action->ot) {
      bool modal = action->ot->modal ? true : false;

      for (unsigned int i = 0; i < action->count_subaction_paths; ++i) {
        short val = KM_ANY;
        bool press_start;

        switch (action->type) {
          case GHOST_kXrActionTypeBooleanInput: {
            const bool *state = &((bool *)action->states)[i];
            bool *state_prev = &((bool *)action->states_prev)[i];
            if (*state) {
              if (!*state_prev) {
                if (modal || action->op_flag == XR_OP_PRESS) {
                  val = KM_PRESS;
                  press_start = true;
                }
              }
              else if (modal) {
                val = KM_PRESS;
                press_start = false;
              }
            }
            else if (*state_prev) {
              if (modal || action->op_flag == XR_OP_RELEASE) {
                val = KM_RELEASE;
                press_start = false;
              }
            }
            *state_prev = *state;
            break;
          }
          case GHOST_kXrActionTypeFloatInput: {
            const float *state = &((float *)action->states)[i];
            float *state_prev = &((float *)action->states_prev)[i];
            if (*state > action->threshold) {
              if (*state_prev <= action->threshold) {
                if (modal || action->op_flag == XR_OP_PRESS) {
                  val = KM_PRESS;
                  press_start = true;
                }
              }
              else if (modal) {
                val = KM_PRESS;
                press_start = false;
              }
            }
            else if (*state_prev > action->threshold) {
              if (modal || action->op_flag == XR_OP_RELEASE) {
                val = KM_RELEASE;
                press_start = false;
              }
            }
            *state_prev = *state;
            break;
          }
          case GHOST_kXrActionTypeVector2fInput: {
            const float(*state)[2] = &((float(*)[2])action->states)[i];
            float(*state_prev)[2] = &((float(*)[2])action->states_prev)[i];
            if ((*state)[0] > action->threshold || (*state)[1] > action->threshold) {
              if ((*state_prev)[0] <= action->threshold && (*state_prev)[1] <= action->threshold) {
                if (modal || action->op_flag == XR_OP_PRESS) {
                  val = KM_PRESS;
                  press_start = true;
                }
              }
              else if (modal) {
                val = KM_PRESS;
                press_start = false;
              }
            }
            else if ((*state_prev)[0] > action->threshold ||
                     (*state_prev)[1] > action->threshold) {
              if (modal || action->op_flag == XR_OP_RELEASE) {
                val = KM_RELEASE;
                press_start = false;
              }
            }
            memcpy(state_prev, state, sizeof(float[2]));
            break;
          }
          default: {
            break;
          }
        }

        if (val != KM_ANY) {
          const GHOST_XrPose *pose = wm_xr_session_controller_pose_find(
              session_state, action->subaction_paths[i]);
          wm_event_add_xrevent(action_set_name,
                               action,
                               pose,
                               &session_state->eyes[settings->selection_eye],
                               surface,
                               win,
                               i,
                               val,
                               press_start);
        }
      }
    }
  }

  BLI_ghashIterator_free(ghi);
}

static void wm_xr_session_action_set_update(const XrSessionSettings *settings,
                                            GHOST_XrContextHandle xr_context,
                                            wmXrSessionState *state,
                                            wmXrActionSet *action_set,
                                            ViewLayer *view_layer,
                                            wmSurface *surface,
                                            wmWindow *win)
{
  GHash *actions = action_set->actions;
  if (!actions) {
    return;
  }

  const unsigned int count = BLI_ghash_len(actions);
  if (count < 1) {
    return;
  }

  /* Create an aligned list of action infos. */
  /* TODO_XR: Store array and number of actions to avoid allocation on each call. */
  GHOST_XrActionInfo **infos = MEM_callocN(sizeof(GHOST_XrActionInfo *) * count, __func__);

  GHashIterator *ghi = BLI_ghashIterator_new(actions);
  unsigned int i = 0;
  GHASH_ITER (*ghi, actions) {
    BLI_STATIC_ASSERT(sizeof(wmXrAction) == sizeof(GHOST_XrActionInfo),
                      "wmXrAction and GHOST_XrActionInfo sizes do not match.");
    infos[i] = BLI_ghashIterator_getValue(
        ghi); /* TODO_XR: Avoid casting wmXrAction to GHOST_XrActionInfo. */
    ++i;
  }
  BLI_ghashIterator_free(ghi);

  int ret = GHOST_XrGetActionStates(xr_context, action_set->name, count, infos);
  MEM_freeN(infos);

  if (ret &&
      (action_set == state->active_action_set)) { /* Only dispatch events for active action set. */
    if (action_set->controller_pose_action) {
      wm_xr_session_controller_mats_update(
          settings, action_set->controller_pose_action, state, view_layer);
    }

    if (surface && win) {
      wm_xr_session_events_dispatch(settings, action_set->name, actions, state, surface, win);
    }
  }
}

void wm_xr_session_actions_update(wmXrData *xr)
{
  if (!xr->runtime) {
    return;
  }

  wmXrSessionState *state = &xr->runtime->session_state;
  GHash *action_sets = state->action_sets;
  if (!action_sets) {
    return;
  }

  GHOST_XrContextHandle xr_context = xr->runtime->context;
  wmXrActionSet *active_action_set = state->active_action_set;

  int ret = GHOST_XrSyncActions(xr_context, active_action_set ? active_action_set->name : NULL);
  if (!ret) {
    return;
  }

  const XrSessionSettings *settings = &xr->session_settings;
  bContext *C = xr->runtime->bcontext;
  wmWindowManager *wm = CTX_wm_manager(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  wmSurface *surface = (g_xr_surface && g_xr_surface->customdata) ? g_xr_surface : NULL;
  wmWindow *win = wm_xr_session_root_window_or_fallback_get(wm, xr->runtime);

  if (active_action_set) {
    wm_xr_session_action_set_update(
        settings, xr_context, state, active_action_set, view_layer, surface, win);
  }
  else {
    GHashIterator *ghi_set = BLI_ghashIterator_new(action_sets);
    GHASH_ITER (*ghi_set, action_sets) {
      wmXrActionSet *action_set = BLI_ghashIterator_getValue(ghi_set);
      if (action_set) {
        wm_xr_session_action_set_update(
            settings, xr_context, state, action_set, view_layer, surface, win);
      }
    }

    BLI_ghashIterator_free(ghi_set);
  }
}

void wm_xr_session_actions_uninit(wmXrData *xr)
{
  GHash *action_sets = xr->runtime->session_state.action_sets;

  if (action_sets) {
    GHashIterator *ghi = BLI_ghashIterator_new(action_sets);
    GHASH_ITER (*ghi, action_sets) {
      wmXrActionSet *action_set = BLI_ghashIterator_getValue(ghi);
      WM_xr_action_set_destroy(xr, action_set->name, false);
    }
    BLI_ghashIterator_free(ghi);

    BLI_ghash_free(action_sets, NULL, NULL);
  }
}

void wm_xr_session_controller_data_populate(const wmXrAction *controller_pose_action,
                                            bContext *C,
                                            wmXrSessionState *state)
{
  const unsigned int count = (unsigned int)min_ii(
      (int)ARRAY_SIZE(state->controllers), (int)controller_pose_action->count_subaction_paths);

  for (unsigned int i = 0; i < count; ++i) {
    wmXrControllerData *c = &state->controllers[i];
    strcpy(c->subaction_path, controller_pose_action->subaction_paths[i]);
    memset(&c->pose, 0, sizeof(c->pose));
    memset(c->mat, 0, sizeof(c->mat));
#if 0
    if (!c->ob) {
      /* Just use zeroed-out pose.position for loc and rot. */
      c->ob = ED_object_add_type(
          C, OB_MESH, "xr_controller", c->pose.position, c->pose.position, false, 0);
      if (c->ob) {
        c->ob->runtime.is_xr = true;
      }
    }
#else
    UNUSED_VARS(C);
#endif
  }

  /* Activate draw callback. */
  if (g_xr_surface) {
    wmXrSurfaceData *surface_data = g_xr_surface->customdata;
    if (surface_data && !surface_data->controller_draw_handle) {
      if (surface_data->art) {
        surface_data->controller_draw_handle = ED_region_draw_cb_activate(
            surface_data->art, wm_xr_draw_controllers, state, REGION_DRAW_POST_VIEW);
      }
    }
  }
}

void wm_xr_session_controller_data_clear(unsigned int count_subaction_paths,
                                         bContext *C,
                                         wmXrSessionState *state)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  bool notify = false;

  const unsigned int count = (unsigned int)min_ii((int)ARRAY_SIZE(state->controllers),
                                                  (int)count_subaction_paths);

  for (unsigned int i = 0; i < count; ++i) {
    Object *ob = state->controllers[i].ob;
    if (ob) {
      /* TODO_XR: Handle case where object was deleted but then undone. */
      Base *base = BKE_view_layer_base_find(view_layer, ob);
      if (base) {
        ED_object_base_free_and_unlink(bmain, scene, ob);
        DEG_graph_id_tag_update(bmain, depsgraph, &ob->id, 0);
        notify = true;
      }
    }
    memset(&state->controllers[i], 0, sizeof(state->controllers[i]));
  }

  if (notify) {
    DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
    WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);
  }

  /* Deactivate draw callback. */
  if (g_xr_surface) {
    wmXrSurfaceData *surface_data = g_xr_surface->customdata;
    if (surface_data && surface_data->controller_draw_handle) {
      if (surface_data->art) {
        ED_region_draw_cb_exit(surface_data->art, surface_data->controller_draw_handle);
      }
      surface_data->controller_draw_handle = NULL;
    }
  }
}

/** \} */ /* XR-Session Actions */

/* -------------------------------------------------------------------- */
/** \name XR-Session Surface
 *
 * A wmSurface is used to manage drawing of the VR viewport. It's created and destroyed with the
 * session.
 *
 * \{ */

/**
 * \brief Call Ghost-XR to draw a frame
 *
 * Draw callback for the XR-session surface. It's expected to be called on each main loop
 * iteration and tells Ghost-XR to submit a new frame by drawing its views. Note that for drawing
 * each view, #wm_xr_draw_view() will be called through Ghost-XR (see GHOST_XrDrawViewFunc()).
 */
static void wm_xr_session_surface_draw(bContext *C)
{
  wmXrSurfaceData *surface_data = g_xr_surface->customdata;
  wmWindowManager *wm = CTX_wm_manager(C);
  Main *bmain = CTX_data_main(C);
  wmXrDrawData draw_data;

  if (!GHOST_XrSessionIsRunning(wm->xr.runtime->context)) {
    return;
  }

  Scene *scene;
  Depsgraph *depsgraph;
  wm_xr_session_scene_and_evaluated_depsgraph_get(bmain, wm, &scene, &depsgraph);
  wm_xr_session_draw_data_populate(&wm->xr, scene, depsgraph, &draw_data);

  DRW_xr_drawing_begin();

  GHOST_XrSessionDrawViews(wm->xr.runtime->context, &draw_data);

  GPU_offscreen_unbind(surface_data->offscreen, false);
  DRW_xr_drawing_end();
}

bool wm_xr_session_surface_offscreen_ensure(wmXrSurfaceData *surface_data,
                                            const GHOST_XrDrawViewInfo *draw_view)
{
  const bool size_changed = surface_data->offscreen &&
                            (GPU_offscreen_width(surface_data->offscreen) != draw_view->width) &&
                            (GPU_offscreen_height(surface_data->offscreen) != draw_view->height);
  char err_out[256] = "unknown";
  bool failure = false;

  if (surface_data->offscreen) {
    BLI_assert(surface_data->viewport);

    if (!size_changed) {
      return true;
    }
    GPU_viewport_free(surface_data->viewport);
    GPU_offscreen_free(surface_data->offscreen);
  }

  if (!(surface_data->offscreen = GPU_offscreen_create(
            draw_view->width, draw_view->height, true, false, err_out))) {
    failure = true;
  }

  if (failure) {
    /* Pass. */
  }
  else if (!(surface_data->viewport = GPU_viewport_create())) {
    GPU_offscreen_free(surface_data->offscreen);
    failure = true;
  }

  if (failure) {
    CLOG_ERROR(&LOG, "Failed to get buffer, %s\n", err_out);
    return false;
  }

  return true;
}

static void wm_xr_session_surface_free_data(wmSurface *surface)
{
  wmXrSurfaceData *data = surface->customdata;

  if (data->viewport) {
    GPU_viewport_free(data->viewport);
  }
  if (data->offscreen) {
    GPU_offscreen_free(data->offscreen);
  }
  if (data->art) {
    BLI_freelistN(&data->art->drawcalls);
    MEM_freeN(data->art);
  }

  MEM_freeN(surface->customdata);

  g_xr_surface = NULL;
}

static wmSurface *wm_xr_session_surface_create(void)
{
  if (g_xr_surface) {
    BLI_assert(false);
    return g_xr_surface;
  }

  wmSurface *surface = MEM_callocN(sizeof(*surface), __func__);
  wmXrSurfaceData *data = MEM_callocN(sizeof(*data), "XrSurfaceData");
  data->art = MEM_callocN(sizeof(*(data->art)), "XrRegionType");

  surface->draw = wm_xr_session_surface_draw;
  surface->free_data = wm_xr_session_surface_free_data;
  surface->ghost_ctx = DRW_xr_opengl_context_get();
  surface->gpu_ctx = DRW_xr_gpu_context_get();
  surface->is_xr = true;

  data->art->regionid = RGN_TYPE_XR;
  surface->customdata = data;

  g_xr_surface = surface;

  return surface;
}

void *wm_xr_session_gpu_binding_context_create(void)
{
  wmSurface *surface = wm_xr_session_surface_create();

  wm_surface_add(surface);

  /* Some regions may need to redraw with updated session state after the session is entirely up
   * and running. */
  WM_main_add_notifier(NC_WM | ND_XR_DATA_CHANGED, NULL);

  return surface->ghost_ctx;
}

void wm_xr_session_gpu_binding_context_destroy(GHOST_ContextHandle UNUSED(context))
{
  if (g_xr_surface) { /* Might have been freed already */
    wm_surface_remove(g_xr_surface);
  }

  wm_window_reset_drawable();

  /* Some regions may need to redraw with updated session state after the session is entirely
   * stopped. */
  WM_main_add_notifier(NC_WM | ND_XR_DATA_CHANGED, NULL);
}

ARegionType *WM_xr_surface_region_type_get(void)
{
  if (g_xr_surface) {
    wmXrSurfaceData *data = g_xr_surface->customdata;
    return data->art;
  }

  return NULL;
}

/** \} */ /* XR-Session Surface */
