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
 * \name Window-Manager XR Drawing
 *
 * Implements Blender specific drawing functionality for use with the Ghost-XR API.
 */

#include <string.h>

#include "BKE_context.h"

#include "BLI_math.h"

#include "ED_view3d_offscreen.h"

#include "GHOST_C-api.h"

#include "GPU_immediate.h"
#include "GPU_viewport.h"

#include "WM_api.h"

#include "wm_surface.h"
#include "wm_xr_intern.h"

void wm_xr_pose_to_viewmat(const GHOST_XrPose *pose, float r_viewmat[4][4])
{
  float iquat[4];
  invert_qt_qt_normalized(iquat, pose->orientation_quat);
  quat_to_mat4(r_viewmat, iquat);
  translate_m4(r_viewmat, -pose->position[0], -pose->position[1], -pose->position[2]);
}

void wm_xr_controller_pose_to_mat(const GHOST_XrPose *pose, float r_mat[4][4])
{
  quat_to_mat4(r_mat, pose->orientation_quat);
  copy_v3_v3(r_mat[3], pose->position);
}

static void wm_xr_draw_matrices_create(const wmXrDrawData *draw_data,
                                       const GHOST_XrDrawViewInfo *draw_view,
                                       const XrSessionSettings *session_settings,
                                       float r_view_mat[4][4],
                                       float r_proj_mat[4][4])
{
  GHOST_XrPose eye_pose;

  copy_qt_qt(eye_pose.orientation_quat, draw_view->eye_pose.orientation_quat);
  copy_v3_v3(eye_pose.position, draw_view->eye_pose.position);
  sub_v3_v3(eye_pose.position, draw_data->eye_position_ofs);
  if ((session_settings->flag & XR_SESSION_USE_POSITION_TRACKING) == 0) {
    sub_v3_v3(eye_pose.position, draw_view->local_pose.position);
  }

  perspective_m4_fov(r_proj_mat,
                     draw_view->fov.angle_left,
                     draw_view->fov.angle_right,
                     draw_view->fov.angle_up,
                     draw_view->fov.angle_down,
                     session_settings->clip_start,
                     session_settings->clip_end);

  float eye_mat[4][4];
  float base_mat[4][4];

  wm_xr_pose_to_viewmat(&eye_pose, eye_mat);
  /* Calculate the base pose matrix (in world space!). */
  wm_xr_pose_to_viewmat(&draw_data->base_pose, base_mat);

  mul_m4_m4m4(r_view_mat, eye_mat, base_mat);
}

static void wm_xr_draw_viewport_buffers_to_active_framebuffer(
    const wmXrRuntimeData *runtime_data,
    const wmXrSurfaceData *surface_data,
    const GHOST_XrDrawViewInfo *draw_view)
{
  const bool is_upside_down = GHOST_XrSessionNeedsUpsideDownDrawing(runtime_data->context);
  rcti rect = {.xmin = 0, .ymin = 0, .xmax = draw_view->width - 1, .ymax = draw_view->height - 1};

  wmViewport(&rect);

  /* For upside down contexts, draw with inverted y-values. */
  if (is_upside_down) {
    SWAP(int, rect.ymin, rect.ymax);
  }
  GPU_viewport_draw_to_screen_ex(surface_data->viewport, 0, &rect, draw_view->expects_srgb_buffer);
}

/**
 * \brief Draw a viewport for a single eye.
 *
 * This is the main viewport drawing function for VR sessions. It's assigned to Ghost-XR as a
 * callback (see GHOST_XrDrawViewFunc()) and executed for each view (read: eye).
 */
void wm_xr_draw_view(const GHOST_XrDrawViewInfo *draw_view, void *customdata)
{
  wmXrDrawData *draw_data = customdata;
  wmXrData *xr_data = draw_data->xr_data;
  wmXrSurfaceData *surface_data = draw_data->surface_data;
  wmXrSessionState *session_state = &xr_data->runtime->session_state;
  XrSessionSettings *settings = &xr_data->session_settings;

  const int display_flags = V3D_OFSDRAW_OVERRIDE_SCENE_SETTINGS | settings->draw_flags;

  float viewmat[4][4], winmat[4][4];

  BLI_assert(WM_xr_session_is_ready(xr_data));

  wm_xr_session_draw_data_update(session_state, settings, draw_view, draw_data);
  wm_xr_draw_matrices_create(draw_data, draw_view, settings, viewmat, winmat);
  wm_xr_session_state_update(settings, draw_data, draw_view, viewmat, winmat, session_state);

  if (!wm_xr_session_surface_offscreen_ensure(surface_data, draw_view)) {
    return;
  }

  /* In case a framebuffer is still bound from drawing the last eye. */
  GPU_framebuffer_restore();
  /* Some systems have drawing glitches without this. */
  GPU_clear_depth(1.0f);

  /* Draws the view into the surface_data->viewport's framebuffers */
  ED_view3d_draw_offscreen_simple(draw_data->depsgraph,
                                  draw_data->scene,
                                  &settings->shading,
                                  settings->shading.type,
                                  draw_view->width,
                                  draw_view->height,
                                  display_flags,
                                  viewmat,
                                  winmat,
                                  settings->clip_start,
                                  settings->clip_end,
                                  true,
                                  false,
                                  true,
                                  true,
                                  NULL,
                                  false,
                                  surface_data->offscreen,
                                  surface_data->viewport);

  /* The draw-manager uses both GPUOffscreen and GPUViewport to manage frame and texture buffers. A
   * call to GPU_viewport_draw_to_screen() is still needed to get the final result from the
   * viewport buffers composited together and potentially color managed for display on screen.
   * It needs a bound frame-buffer to draw into, for which we simply reuse the GPUOffscreen one.
   *
   * In a next step, Ghost-XR will use the currently bound frame-buffer to retrieve the image
   * to be submitted to the OpenXR swap-chain. So do not un-bind the off-screen yet! */

  GPU_offscreen_bind(surface_data->offscreen, false);

  wm_xr_draw_viewport_buffers_to_active_framebuffer(xr_data->runtime, surface_data, draw_view);
}

void wm_xr_draw_controllers(const bContext *UNUSED(C), ARegion *UNUSED(region), void *customdata)
{
  const wmXrSessionState *state = customdata;

  const eGPUDepthTest depth_test_prev = GPU_depth_test_get();
  GPU_depth_test(GPU_DEPTH_ALWAYS);

  /* For now, just draw controller axes. In the future this can be replaced
   * with actual controller geometry. */
  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  GPU_line_width(2.0f);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  const float r[4] = {1.0f, 0.2f, 0.322f, 1.0f};
  const float g[4] = {0.545f, 0.863f, 0.0f, 1.0f};
  const float b[4] = {0.157f, 0.565f, 1.0f, 1.0f};

  const float scale = 0.1f;
  float x_axis[3];
  float y_axis[3];
  float z_axis[3];

  for (int i = 0; i < 2; ++i) {
    const float(*mat)[4] = state->controllers[i].mat;

    normalize_v3_v3(x_axis, mat[0]);
    mul_v3_fl(x_axis, scale);
    add_v3_v3(x_axis, mat[3]);

    normalize_v3_v3(y_axis, mat[1]);
    mul_v3_fl(y_axis, scale);
    add_v3_v3(y_axis, mat[3]);

    normalize_v3_v3(z_axis, mat[2]);
    mul_v3_fl(z_axis, scale);
    add_v3_v3(z_axis, mat[3]);

    immBegin(GPU_PRIM_LINES, 2);
    immUniformColor4fv(r);
    immVertex3fv(pos, mat[3]);
    immVertex3fv(pos, x_axis);
    immEnd();

    immBegin(GPU_PRIM_LINES, 2);
    immUniformColor4fv(g);
    immVertex3fv(pos, mat[3]);
    immVertex3fv(pos, y_axis);
    immEnd();

    immBegin(GPU_PRIM_LINES, 2);
    immUniformColor4fv(b);
    immVertex3fv(pos, mat[3]);
    immVertex3fv(pos, z_axis);
    immEnd();
  }

  immUnbindProgram();

  GPU_depth_test(depth_test_prev);
}
