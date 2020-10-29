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
 *
 * \name Window-Manager XR Actions
 *
 * Uses the Ghost-XR API to manage OpenXR actions.
 * All functions are designed to be usable by RNA / the Python API.
 */

#include "BLI_ghash.h"
#include "BLI_math.h"

#include "GHOST_C-api.h"

#include "MEM_guardedalloc.h"

#include "WM_api.h"
#include "WM_types.h"

#include "wm_xr_intern.h"

/* -------------------------------------------------------------------- */
/** \name XR-Action API
 *
 * API functions for managing OpenXR actions.
 *
 * \{ */

static wmXrActionSet *action_set_find(wmXrData *xr, const char *action_set_name)
{
  GHash *action_sets = xr->runtime->session_state.action_sets;
  return action_sets ? BLI_ghash_lookup(action_sets, action_set_name) : NULL;
}

static wmXrActionSet *action_set_create(const GHOST_XrActionSetInfo *info)
{
  wmXrActionSet *action_set = MEM_callocN(sizeof(wmXrActionSet), __func__);
  action_set->name = MEM_mallocN(strlen(info->name) + 1, __func__);
  strcpy(action_set->name, info->name);

  return action_set;
}

static void action_set_destroy(void *val)
{
  wmXrActionSet *action_set = val;

  if (action_set->name) {
    MEM_freeN(action_set->name);
  }

  MEM_freeN(action_set);
}

static wmXrAction *action_find(wmXrActionSet *action_set, const char *action_name)
{
  GHash *actions = action_set->actions;
  return actions ? BLI_ghash_lookup(actions, action_name) : NULL;
}

static wmXrAction *action_create(const GHOST_XrActionInfo *info)
{
  wmXrAction *action = MEM_callocN(sizeof(wmXrAction), __func__);
  action->name = MEM_mallocN(strlen(info->name) + 1, __func__);
  strcpy(action->name, info->name);
  action->type = info->type;

  const unsigned int count = info->count_subaction_paths;
  action->count_subaction_paths = count;

  action->subaction_paths = MEM_mallocN(sizeof(char *) * count, __func__);
  for (unsigned int i = 0; i < count; ++i) {
    action->subaction_paths[i] = MEM_mallocN(strlen(info->subaction_paths[i]) + 1, __func__);
    strcpy(action->subaction_paths[i], info->subaction_paths[i]);
  }

  size_t size;
  switch (info->type) {
    case GHOST_kXrActionTypeBooleanInput: {
      size = sizeof(bool);
      break;
    }
    case GHOST_kXrActionTypeFloatInput: {
      size = sizeof(float);
      break;
    }
    case GHOST_kXrActionTypeVector2fInput: {
      size = sizeof(float[2]);
      break;
    }
    case GHOST_kXrActionTypePoseInput: {
      size = sizeof(GHOST_XrPose);
      break;
    }
    default: {
      return action;
    }
  }
  action->states = MEM_calloc_arrayN(count, size, __func__);
  action->states_prev = MEM_calloc_arrayN(count, size, __func__);

  action->threshold = info->threshold;
  CLAMP(action->threshold, 0.0f, 1.0f);

  action->ot = info->ot;
  action->op_properties = info->op_properties;
  action->op_flag = info->op_flag;

  return action;
}

static void action_destroy(void *val)
{
  wmXrAction *action = val;

  if (action->name) {
    MEM_freeN(action->name);
  }

  const unsigned int count = action->count_subaction_paths;
  char **subaction_paths = action->subaction_paths;
  if (subaction_paths) {
    for (unsigned int i = 0; i < count; ++i) {
      if (subaction_paths[i]) {
        MEM_freeN(subaction_paths[i]);
      }
    }
    MEM_freeN(subaction_paths);
  }

  if (action->states) {
    MEM_freeN(action->states);
  }
  if (action->states_prev) {
    MEM_freeN(action->states_prev);
  }

  MEM_freeN(action);
}

bool WM_xr_action_set_create(wmXrData *xr, const GHOST_XrActionSetInfo *info)
{
  if (action_set_find(xr, info->name)) {
    return false;
  }

  if (!GHOST_XrCreateActionSet(xr->runtime->context, info)) {
    return false;
  }

  GHash *action_sets = xr->runtime->session_state.action_sets;
  if (!action_sets) {
    action_sets = xr->runtime->session_state.action_sets = BLI_ghash_str_new(__func__);
  }

  wmXrActionSet *action_set = action_set_create(info);
  BLI_ghash_insert(
      action_sets,
      action_set->name,
      action_set); /* Important to use action_set->name, since only a pointer is stored. */

  return true;
}

void WM_xr_action_set_destroy(wmXrData *xr, const char *action_set_name, bool remove_reference)
{
  GHOST_XrContextHandle context = xr->runtime->context;
  if (context && GHOST_XrSessionIsRunning(context)) {
    GHOST_XrDestroyActionSet(context, action_set_name);
  }

  wmXrSessionState *session_state = &xr->runtime->session_state;
  GHash *action_sets = session_state->action_sets;
  wmXrActionSet *action_set = BLI_ghash_lookup(action_sets, action_set_name);
  if (!action_set) {
    return;
  }

  if (action_set == session_state->active_action_set) {
    if (action_set->controller_pose_action) {
      wm_xr_session_controller_data_clear(
          action_set->controller_pose_action->count_subaction_paths,
          xr->runtime->bcontext,
          &xr->runtime->session_state);
      action_set->controller_pose_action = NULL;
    }
    session_state->active_action_set = NULL;
  }

  if (action_set->actions) {
    BLI_ghash_free(action_set->actions, NULL, action_destroy);
  }

  if (remove_reference) {
    BLI_ghash_remove(action_sets, action_set_name, NULL, action_set_destroy);
  }
  else {
    action_set_destroy(action_set);
  }
}

bool WM_xr_actions_create(wmXrData *xr,
                          const char *action_set_name,
                          unsigned int count,
                          const GHOST_XrActionInfo *infos)
{
  wmXrActionSet *action_set = action_set_find(xr, action_set_name);
  if (!action_set) {
    return false;
  }

  if (!GHOST_XrCreateActions(xr->runtime->context, action_set_name, count, infos)) {
    return false;
  }

  GHash *actions = action_set->actions;
  if (!actions) {
    actions = action_set->actions = BLI_ghash_str_new(__func__);
  }

  for (unsigned int i = 0; i < count; ++i) {
    const GHOST_XrActionInfo *info = &infos[i];
    if (action_find(action_set, info->name)) {
      continue;
    }

    wmXrAction *action = action_create(info);
    if (action) {
      BLI_ghash_insert(
          actions,
          action->name,
          action); /* Important to use action->name, since only a pointer is stored. */
    }
  }

  return true;
}

void WM_xr_actions_destroy(wmXrData *xr,
                           const char *action_set_name,
                           unsigned int count,
                           const char *const *action_names)
{
  wmXrActionSet *action_set = action_set_find(xr, action_set_name);
  if (!action_set) {
    return;
  }

  GHOST_XrDestroyActions(xr->runtime->context, action_set_name, count, action_names);

  /* Save name of controller pose action in case the action is removed from the GHash. */
  char controller_pose_name[64];
  strcpy(controller_pose_name, action_set->controller_pose_action->name);
  const unsigned int controller_pose_count =
      action_set->controller_pose_action->count_subaction_paths;

  GHash *actions = action_set->actions;
  for (unsigned int i = 0; i < count; ++i) {
    BLI_ghash_remove(actions, action_names[i], NULL, action_destroy);
  }

  if (!action_find(action_set, controller_pose_name)) {
    if (action_set == xr->runtime->session_state.active_action_set) {
      wm_xr_session_controller_data_clear(
          controller_pose_count, xr->runtime->bcontext, &xr->runtime->session_state);
    }
    action_set->controller_pose_action = NULL;
  }
}

bool WM_xr_action_spaces_create(wmXrData *xr,
                                const char *action_set_name,
                                unsigned int count,
                                const GHOST_XrActionSpaceInfo *infos)
{
  return GHOST_XrCreateActionSpaces(xr->runtime->context, action_set_name, count, infos) ? true :
                                                                                           false;
}

void WM_xr_action_spaces_destroy(wmXrData *xr,
                                 const char *action_set_name,
                                 unsigned int count,
                                 const GHOST_XrActionSpaceInfo *infos)
{
  GHOST_XrDestroyActionSpaces(xr->runtime->context, action_set_name, count, infos);
}

bool WM_xr_action_bindings_create(wmXrData *xr,
                                  const char *action_set_name,
                                  unsigned int count,
                                  const GHOST_XrActionBindingsInfo *infos)
{
  return GHOST_XrCreateActionBindings(xr->runtime->context, action_set_name, count, infos);
}

void WM_xr_action_bindings_destroy(wmXrData *xr,
                                   const char *action_set_name,
                                   unsigned int count,
                                   const GHOST_XrActionBindingsInfo *infos)
{
  GHOST_XrDestroyActionBindings(xr->runtime->context, action_set_name, count, infos);
}

bool WM_xr_active_action_set_set(wmXrData *xr, const char *action_set_name)
{
  wmXrActionSet *action_set = action_set_find(xr, action_set_name);
  if (!action_set) {
    return false;
  }

  xr->runtime->session_state.active_action_set = action_set;

  if (action_set->controller_pose_action) {
    wm_xr_session_controller_data_populate(
        action_set->controller_pose_action, xr->runtime->bcontext, &xr->runtime->session_state);
  }

  return true;
}

bool WM_xr_controller_pose_action_set(wmXrData *xr,
                                      const char *action_set_name,
                                      const char *action_name)
{
  wmXrActionSet *action_set = action_set_find(xr, action_set_name);
  if (!action_set) {
    return false;
  }

  wmXrAction *action = action_find(action_set, action_name);
  if (!action) {
    return false;
  }

  action_set->controller_pose_action = action;

  if (action_set == xr->runtime->session_state.active_action_set) {
    wm_xr_session_controller_data_populate(
        action, xr->runtime->bcontext, &xr->runtime->session_state);
  }

  return true;
}

bool WM_xr_action_states_get(const wmXrData *xr,
                             const char *action_set_name,
                             unsigned int count,
                             GHOST_XrActionInfo *r_infos)
{
  const wmXrActionSet *action_set = action_set_find((wmXrData *)xr, action_set_name);
  if (!action_set) {
    return false;
  }

  bool ret = true;

  for (unsigned int info_idx = 0; info_idx < count; ++info_idx) {
    GHOST_XrActionInfo *info = &r_infos[info_idx];
    const wmXrAction *action = action_find((wmXrActionSet *)action_set, info->name);
    if (action) {
      BLI_assert(action->type == info->type);

      for (unsigned int ist_idx = 0; ist_idx < info->count_subaction_paths;
           ++ist_idx) { /* info state index */
        const char *subaction_path = info->subaction_paths[ist_idx];

        /* Find the matching action state. */
        unsigned int ast_idx = 0;
        for (; ast_idx < action->count_subaction_paths; ++ast_idx) { /* action state index */
          if (STREQ(subaction_path, action->subaction_paths[ast_idx])) {
            switch (info->type) {
              case GHOST_kXrActionTypeBooleanInput: {
                ((bool *)info->states)[ist_idx] = ((bool *)action->states)[ast_idx];
                break;
              }
              case GHOST_kXrActionTypeFloatInput: {
                ((float *)info->states)[ist_idx] = ((float *)action->states)[ast_idx];
                break;
              }
              case GHOST_kXrActionTypeVector2fInput: {
                memcpy(((float(*)[2])info->states)[ist_idx],
                       ((float(*)[2])action->states)[ast_idx],
                       sizeof(float[2]));
                break;
              }
              case GHOST_kXrActionTypePoseInput: {
                const GHOST_XrPose *action_state = &((GHOST_XrPose *)action->states)[ast_idx];
                GHOST_XrPose *info_state = &((GHOST_XrPose *)info->states)[ist_idx];
                memcpy(info_state, action_state, sizeof(GHOST_XrPose));
                break;
              }
              default: {
                break;
              }
            }
            break;
          }
        }

        if (ret && (ast_idx == action->count_subaction_paths)) {
          ret = false;
        }
      }
    }
  }

  return ret;
}

bool WM_xr_haptic_action_apply(wmXrData *xr,
                               const char *action_set_name,
                               const char *action_name,
                               unsigned int count,
                               const char *const *subaction_paths,
                               const long long *duration,
                               const float *frequency,
                               const float *amplitude)
{
  return GHOST_XrApplyHapticAction(xr->runtime->context,
                                   action_set_name,
                                   action_name,
                                   count,
                                   subaction_paths,
                                   duration,
                                   frequency,
                                   amplitude) ?
             true :
             false;
}

void WM_xr_haptic_action_stop(wmXrData *xr,
                              const char *action_set_name,
                              const char *action_name,
                              unsigned int count,
                              const char *const *subaction_paths)
{
  GHOST_XrStopHapticAction(
      xr->runtime->context, action_set_name, action_name, count, subaction_paths);
}

/** \} */ /* XR-Action API */
