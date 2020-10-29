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
 * \ingroup GHOST
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <list>
#include <set>
#include <sstream>

#include "GHOST_C-api.h"

#include "GHOST_IXrGraphicsBinding.h"
#include "GHOST_XrContext.h"
#include "GHOST_XrException.h"
#include "GHOST_XrSwapchain.h"
#include "GHOST_Xr_intern.h"

#include "GHOST_XrSession.h"

struct OpenXRActionSet;

struct OpenXRSessionData {
  XrSystemId system_id = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;

  /* Only stereo rendering supported now. */
  const XrViewConfigurationType view_type = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  XrSpace reference_space;
  XrSpace view_space;
  std::vector<XrView> views;
  std::vector<GHOST_XrSwapchain> swapchains;

  std::map<std::string, OpenXRActionSet> action_sets;
};

struct GHOST_XrDrawInfo {
  XrFrameState frame_state;

  /** Time at frame start to benchmark frame render durations. */
  std::chrono::high_resolution_clock::time_point frame_begin_time;
  /* Time previous frames took for rendering (in ms). */
  std::list<double> last_frame_times;
};

struct OpenXRActionProfile {
  XrPath profile;
  /* Bindings identified by interaction (user (subaction) + component) path. */
  std::map<std::string, XrPath> bindings;
};

struct OpenXRAction {
  XrAction action;
  /* Spaces identified by user (subaction) path. */
  std::map<std::string, XrSpace> spaces;
  /* Profiles identified by interaction profile path. */
  std::map<std::string, OpenXRActionProfile> profiles;
};

struct OpenXRActionSet {
  XrActionSet set;
  std::map<std::string, OpenXRAction> actions;
};

/* -------------------------------------------------------------------- */
/** \name Create, Initialize and Destruct
 *
 * \{ */

GHOST_XrSession::GHOST_XrSession(GHOST_XrContext &xr_context)
    : m_context(&xr_context), m_oxr(std::make_unique<OpenXRSessionData>())
{
}

GHOST_XrSession::~GHOST_XrSession()
{
  for (auto &action_set : m_oxr->action_sets) {
    destroyActionSet(action_set.first.c_str(), false);
  }
  m_oxr->action_sets.clear();

  unbindGraphicsContext();

  m_oxr->swapchains.clear();

  if (m_oxr->reference_space != XR_NULL_HANDLE) {
    CHECK_XR_ASSERT(xrDestroySpace(m_oxr->reference_space));
  }
  if (m_oxr->view_space != XR_NULL_HANDLE) {
    CHECK_XR_ASSERT(xrDestroySpace(m_oxr->view_space));
  }
  if (m_oxr->session != XR_NULL_HANDLE) {
    CHECK_XR_ASSERT(xrDestroySession(m_oxr->session));
  }

  m_oxr->session = XR_NULL_HANDLE;
  m_oxr->session_state = XR_SESSION_STATE_UNKNOWN;

  m_context->getCustomFuncs().session_exit_fn(m_context->getCustomFuncs().session_exit_customdata);
}

/**
 * A system in OpenXR the combination of some sort of HMD plus controllers and whatever other
 * devices are managed through OpenXR. So this attempts to init the HMD and the other devices.
 */
void GHOST_XrSession::initSystem()
{
  assert(m_context->getInstance() != XR_NULL_HANDLE);
  assert(m_oxr->system_id == XR_NULL_SYSTEM_ID);

  XrSystemGetInfo system_info = {};
  system_info.type = XR_TYPE_SYSTEM_GET_INFO;
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

  CHECK_XR(xrGetSystem(m_context->getInstance(), &system_info, &m_oxr->system_id),
           "Failed to get device information. Is a device plugged in?");
}

/** \} */ /* Create, Initialize and Destruct */

/* -------------------------------------------------------------------- */
/** \name State Management
 *
 * \{ */

static void create_reference_spaces(OpenXRSessionData &oxr, const GHOST_XrPose &base_pose)
{
  XrReferenceSpaceCreateInfo create_info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  create_info.poseInReferenceSpace.orientation.w = 1.0f;

  create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
#if 0
/* TODO
 *
 * Proper reference space set up is not supported yet. We simply hand OpenXR
 * the global space as reference space and apply its pose onto the active
 * camera matrix to get a basic viewing experience going. If there's no active
 * camera with stick to the world origin.
 *
 * Once we have proper reference space set up (i.e. a way to define origin, up-
 * direction and an initial view rotation perpendicular to the up-direction),
 * we can hand OpenXR a proper reference pose/space.
 */
  create_info.poseInReferenceSpace.position.x = base_pose->position[0];
  create_info.poseInReferenceSpace.position.y = base_pose->position[1];
  create_info.poseInReferenceSpace.position.z = base_pose->position[2];
  create_info.poseInReferenceSpace.orientation.x = base_pose->orientation_quat[1];
  create_info.poseInReferenceSpace.orientation.y = base_pose->orientation_quat[2];
  create_info.poseInReferenceSpace.orientation.z = base_pose->orientation_quat[3];
  create_info.poseInReferenceSpace.orientation.w = base_pose->orientation_quat[0];
#else
  (void)base_pose;
#endif

  CHECK_XR(xrCreateReferenceSpace(oxr.session, &create_info, &oxr.reference_space),
           "Failed to create reference space.");

  create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  CHECK_XR(xrCreateReferenceSpace(oxr.session, &create_info, &oxr.view_space),
           "Failed to create view reference space.");
}

void GHOST_XrSession::start(const GHOST_XrSessionBeginInfo *begin_info)
{
  assert(m_context->getInstance() != XR_NULL_HANDLE);
  assert(m_oxr->session == XR_NULL_HANDLE);
  if (m_context->getCustomFuncs().gpu_ctx_bind_fn == nullptr) {
    throw GHOST_XrException(
        "Invalid API usage: No way to bind graphics context to the XR session. Call "
        "GHOST_XrGraphicsContextBindFuncs() with valid parameters before starting the "
        "session (through GHOST_XrSessionStart()).");
  }

  initSystem();

  bindGraphicsContext();
  if (m_gpu_ctx == nullptr) {
    throw GHOST_XrException(
        "Invalid API usage: No graphics context returned through the callback set with "
        "GHOST_XrGraphicsContextBindFuncs(). This is required for session starting (through "
        "GHOST_XrSessionStart()).");
  }

  std::string requirement_str;
  m_gpu_binding = GHOST_XrGraphicsBindingCreateFromType(m_context->getGraphicsBindingType(),
                                                        *m_gpu_ctx);
  if (!m_gpu_binding->checkVersionRequirements(
          *m_gpu_ctx, m_context->getInstance(), m_oxr->system_id, &requirement_str)) {
    std::ostringstream strstream;
    strstream << "Available graphics context version does not meet the following requirements: "
              << requirement_str;
    throw GHOST_XrException(strstream.str().c_str());
  }
  m_gpu_binding->initFromGhostContext(*m_gpu_ctx);

  XrSessionCreateInfo create_info = {};
  create_info.type = XR_TYPE_SESSION_CREATE_INFO;
  create_info.systemId = m_oxr->system_id;
  create_info.next = &m_gpu_binding->oxr_binding;

  CHECK_XR(xrCreateSession(m_context->getInstance(), &create_info, &m_oxr->session),
           "Failed to create VR session. The OpenXR runtime may have additional requirements for "
           "the graphics driver that are not met. Other causes are possible too however.\nTip: "
           "The --debug-xr command line option for Blender might allow the runtime to output "
           "detailed error information to the command line.");

  prepareDrawing();
  create_reference_spaces(*m_oxr, begin_info->base_pose);

  /* Create and bind actions here. */
  m_context->getCustomFuncs().session_create_fn(
      m_context->getCustomFuncs().session_create_customdata);
}

void GHOST_XrSession::requestEnd()
{
  xrRequestExitSession(m_oxr->session);
}

void GHOST_XrSession::beginSession()
{
  XrSessionBeginInfo begin_info = {XR_TYPE_SESSION_BEGIN_INFO};
  begin_info.primaryViewConfigurationType = m_oxr->view_type;
  CHECK_XR(xrBeginSession(m_oxr->session, &begin_info), "Failed to cleanly begin the VR session.");
}

void GHOST_XrSession::endSession()
{
  assert(m_oxr->session != XR_NULL_HANDLE);
  CHECK_XR(xrEndSession(m_oxr->session), "Failed to cleanly end the VR session.");
}

GHOST_XrSession::LifeExpectancy GHOST_XrSession::handleStateChangeEvent(
    const XrEventDataSessionStateChanged &lifecycle)
{
  m_oxr->session_state = lifecycle.state;

  /* Runtime may send events for apparently destroyed session. Our handle should be NULL then. */
  assert((m_oxr->session == XR_NULL_HANDLE) || (m_oxr->session == lifecycle.session));

  switch (lifecycle.state) {
    case XR_SESSION_STATE_READY: {
      beginSession();
      break;
    }
    case XR_SESSION_STATE_STOPPING:
      endSession();
      break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
      return SESSION_DESTROY;
    default:
      break;
  }

  return SESSION_KEEP_ALIVE;
}
/** \} */ /* State Management */

/* -------------------------------------------------------------------- */
/** \name Drawing
 *
 * \{ */

void GHOST_XrSession::prepareDrawing()
{
  std::vector<XrViewConfigurationView> view_configs;
  uint32_t view_count;

  CHECK_XR(
      xrEnumerateViewConfigurationViews(
          m_context->getInstance(), m_oxr->system_id, m_oxr->view_type, 0, &view_count, nullptr),
      "Failed to get count of view configurations.");
  view_configs.resize(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  CHECK_XR(xrEnumerateViewConfigurationViews(m_context->getInstance(),
                                             m_oxr->system_id,
                                             m_oxr->view_type,
                                             view_configs.size(),
                                             &view_count,
                                             view_configs.data()),
           "Failed to get count of view configurations.");

  for (const XrViewConfigurationView &view_config : view_configs) {
    m_oxr->swapchains.emplace_back(*m_gpu_binding, m_oxr->session, view_config);
  }

  m_oxr->views.resize(view_count, {XR_TYPE_VIEW});

  m_draw_info = std::make_unique<GHOST_XrDrawInfo>();
}

void GHOST_XrSession::beginFrameDrawing()
{
  XrFrameWaitInfo wait_info = {XR_TYPE_FRAME_WAIT_INFO};
  XrFrameBeginInfo begin_info = {XR_TYPE_FRAME_BEGIN_INFO};
  XrFrameState frame_state = {XR_TYPE_FRAME_STATE};

  /* TODO Blocking call. Drawing should run on a separate thread to avoid interferences. */
  CHECK_XR(xrWaitFrame(m_oxr->session, &wait_info, &frame_state),
           "Failed to synchronize frame rates between Blender and the device.");

  CHECK_XR(xrBeginFrame(m_oxr->session, &begin_info),
           "Failed to submit frame rendering start state.");

  m_draw_info->frame_state = frame_state;

  if (m_context->isDebugTimeMode()) {
    m_draw_info->frame_begin_time = std::chrono::high_resolution_clock::now();
  }
}

static void print_debug_timings(GHOST_XrDrawInfo &draw_info)
{
  /** Render time of last 8 frames (in ms) to calculate an average. */
  std::chrono::duration<double, std::milli> duration = std::chrono::high_resolution_clock::now() -
                                                       draw_info.frame_begin_time;
  const double duration_ms = duration.count();
  const int avg_frame_count = 8;
  double avg_ms_tot = 0.0;

  if (draw_info.last_frame_times.size() >= avg_frame_count) {
    draw_info.last_frame_times.pop_front();
    assert(draw_info.last_frame_times.size() == avg_frame_count - 1);
  }
  draw_info.last_frame_times.push_back(duration_ms);
  for (double ms_iter : draw_info.last_frame_times) {
    avg_ms_tot += ms_iter;
  }

  printf("VR frame render time: %.0fms - %.2f FPS (%.2f FPS 8 frames average)\n",
         duration_ms,
         1000.0 / duration_ms,
         1000.0 / (avg_ms_tot / draw_info.last_frame_times.size()));
}

void GHOST_XrSession::endFrameDrawing(std::vector<XrCompositionLayerBaseHeader *> &layers)
{
  XrFrameEndInfo end_info = {XR_TYPE_FRAME_END_INFO};

  end_info.displayTime = m_draw_info->frame_state.predictedDisplayTime;
  end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end_info.layerCount = layers.size();
  end_info.layers = layers.data();

  CHECK_XR(xrEndFrame(m_oxr->session, &end_info), "Failed to submit rendered frame.");

  if (m_context->isDebugTimeMode()) {
    print_debug_timings(*m_draw_info);
  }
}

void GHOST_XrSession::draw(void *draw_customdata)
{
  std::vector<XrCompositionLayerProjectionView>
      projection_layer_views; /* Keep alive until xrEndFrame() call! */
  XrCompositionLayerProjection proj_layer;
  std::vector<XrCompositionLayerBaseHeader *> layers;

  beginFrameDrawing();

  if (m_draw_info->frame_state.shouldRender) {
    proj_layer = drawLayer(projection_layer_views, draw_customdata);
    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&proj_layer));
  }

  endFrameDrawing(layers);
}

static void copy_openxr_pose_to_ghost_pose(const XrPosef &oxr_pose, GHOST_XrPose &r_ghost_pose)
{
  /* Set and convert to Blender coodinate space. */
  r_ghost_pose.position[0] = oxr_pose.position.x;
  r_ghost_pose.position[1] = oxr_pose.position.y;
  r_ghost_pose.position[2] = oxr_pose.position.z;
  r_ghost_pose.orientation_quat[0] = oxr_pose.orientation.w;
  r_ghost_pose.orientation_quat[1] = oxr_pose.orientation.x;
  r_ghost_pose.orientation_quat[2] = oxr_pose.orientation.y;
  r_ghost_pose.orientation_quat[3] = oxr_pose.orientation.z;
}

static void copy_ghost_pose_to_openxr_pose(const GHOST_XrPose &ghost_pose, XrPosef &r_oxr_pose)
{
  /* Set and convert to OpenXR coodinate space. */
  r_oxr_pose.position.x = ghost_pose.position[0];
  r_oxr_pose.position.y = ghost_pose.position[1];
  r_oxr_pose.position.z = ghost_pose.position[2];
  r_oxr_pose.orientation.w = ghost_pose.orientation_quat[0];
  r_oxr_pose.orientation.x = ghost_pose.orientation_quat[1];
  r_oxr_pose.orientation.y = ghost_pose.orientation_quat[2];
  r_oxr_pose.orientation.z = ghost_pose.orientation_quat[3];
}

static void ghost_xr_draw_view_info_from_view(const XrView &view, GHOST_XrDrawViewInfo &r_info)
{
  /* Set and convert to Blender coodinate space. */
  copy_openxr_pose_to_ghost_pose(view.pose, r_info.eye_pose);

  r_info.fov.angle_left = view.fov.angleLeft;
  r_info.fov.angle_right = view.fov.angleRight;
  r_info.fov.angle_up = view.fov.angleUp;
  r_info.fov.angle_down = view.fov.angleDown;
}

void GHOST_XrSession::drawView(GHOST_XrSwapchain &swapchain,
                               XrCompositionLayerProjectionView &r_proj_layer_view,
                               XrSpaceLocation &view_location,
                               XrView &view,
                               uint32_t view_idx,
                               void *draw_customdata)
{
  XrSwapchainImageBaseHeader *swapchain_image = swapchain.acquireDrawableSwapchainImage();
  GHOST_XrDrawViewInfo draw_view_info = {};

  r_proj_layer_view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
  r_proj_layer_view.pose = view.pose;
  r_proj_layer_view.fov = view.fov;
  swapchain.updateCompositionLayerProjectViewSubImage(r_proj_layer_view.subImage);

  draw_view_info.view = (char)view_idx;
  draw_view_info.expects_srgb_buffer = swapchain.isBufferSRGB();
  draw_view_info.ofsx = r_proj_layer_view.subImage.imageRect.offset.x;
  draw_view_info.ofsy = r_proj_layer_view.subImage.imageRect.offset.y;
  draw_view_info.width = r_proj_layer_view.subImage.imageRect.extent.width;
  draw_view_info.height = r_proj_layer_view.subImage.imageRect.extent.height;
  copy_openxr_pose_to_ghost_pose(view_location.pose, draw_view_info.local_pose);
  ghost_xr_draw_view_info_from_view(view, draw_view_info);

  /* Draw! */
  m_context->getCustomFuncs().draw_view_fn(&draw_view_info, draw_customdata);
  m_gpu_binding->submitToSwapchainImage(*swapchain_image, draw_view_info);

  swapchain.releaseImage();
}

XrCompositionLayerProjection GHOST_XrSession::drawLayer(
    std::vector<XrCompositionLayerProjectionView> &r_proj_layer_views, void *draw_customdata)
{
  XrViewLocateInfo viewloc_info = {XR_TYPE_VIEW_LOCATE_INFO};
  XrViewState view_state = {XR_TYPE_VIEW_STATE};
  XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrSpaceLocation view_location{XR_TYPE_SPACE_LOCATION};
  uint32_t view_count;

  viewloc_info.viewConfigurationType = m_oxr->view_type;
  viewloc_info.displayTime = m_draw_info->frame_state.predictedDisplayTime;
  viewloc_info.space = m_oxr->reference_space;

  CHECK_XR(xrLocateViews(m_oxr->session,
                         &viewloc_info,
                         &view_state,
                         m_oxr->views.size(),
                         &view_count,
                         m_oxr->views.data()),
           "Failed to query frame view and projection state.");
  assert(m_oxr->swapchains.size() == view_count);

  CHECK_XR(
      xrLocateSpace(
          m_oxr->view_space, m_oxr->reference_space, viewloc_info.displayTime, &view_location),
      "Failed to query frame view space");

  r_proj_layer_views.resize(view_count);

  for (uint32_t view_idx = 0; view_idx < view_count; view_idx++) {
    drawView(m_oxr->swapchains[view_idx],
             r_proj_layer_views[view_idx],
             view_location,
             m_oxr->views[view_idx],
             view_idx,
             draw_customdata);
  }

  layer.space = m_oxr->reference_space;
  layer.viewCount = r_proj_layer_views.size();
  layer.views = r_proj_layer_views.data();

  return layer;
}

bool GHOST_XrSession::needsUpsideDownDrawing() const
{
  return m_gpu_binding && m_gpu_binding->needsUpsideDownDrawing(*m_gpu_ctx);
}

/** \} */ /* Drawing */

/* -------------------------------------------------------------------- */
/** \name State Queries
 *
 * \{ */

bool GHOST_XrSession::isRunning() const
{
  if (m_oxr->session == XR_NULL_HANDLE) {
    return false;
  }
  switch (m_oxr->session_state) {
    case XR_SESSION_STATE_READY:
    case XR_SESSION_STATE_SYNCHRONIZED:
    case XR_SESSION_STATE_VISIBLE:
    case XR_SESSION_STATE_FOCUSED:
      return true;
    default:
      return false;
  }
}

/** \} */ /* State Queries */

/* -------------------------------------------------------------------- */
/** \name Graphics Context Injection
 *
 * Sessions need access to Ghost graphics context information. Additionally, this API allows
 * creating contexts on the fly (created on start, destructed on end). For this, callbacks to bind
 * (potentially create) and unbind (potentially destruct) a Ghost graphics context have to be set,
 * which will be called on session start and end respectively.
 *
 * \{ */

void GHOST_XrSession::bindGraphicsContext()
{
  const GHOST_XrCustomFuncs &custom_funcs = m_context->getCustomFuncs();
  assert(custom_funcs.gpu_ctx_bind_fn);
  m_gpu_ctx = static_cast<GHOST_Context *>(custom_funcs.gpu_ctx_bind_fn());
}

void GHOST_XrSession::unbindGraphicsContext()
{
  const GHOST_XrCustomFuncs &custom_funcs = m_context->getCustomFuncs();
  if (custom_funcs.gpu_ctx_unbind_fn) {
    custom_funcs.gpu_ctx_unbind_fn((GHOST_ContextHandle)m_gpu_ctx);
  }
  m_gpu_ctx = nullptr;
}

/** \} */ /* Graphics Context Injection */

/* -------------------------------------------------------------------- */
/** \name Actions
 *
 * \{ */

static OpenXRActionSet *find_action_set(OpenXRSessionData *oxr, const char *action_set_name)
{
  auto action_set = oxr->action_sets.find(action_set_name);
  if (action_set == oxr->action_sets.end()) {
    return nullptr;
  }
  return &action_set->second;
}

static OpenXRAction *find_action(OpenXRActionSet *action_set, const char *action_name)
{
  auto action = action_set->actions.find(action_name);
  if (action == action_set->actions.end()) {
    return nullptr;
  }
  return &action->second;
}

static XrSpace *find_action_space(OpenXRAction *action, const char *subaction_path)
{
  auto space = action->spaces.find(subaction_path);
  if (space == action->spaces.end()) {
    return nullptr;
  }
  return &space->second;
}

static OpenXRActionProfile *find_action_profile(OpenXRAction *action,
                                                const char *interaction_profile_path)
{
  auto profile = action->profiles.find(interaction_profile_path);
  if (profile == action->profiles.end()) {
    return nullptr;
  }
  return &profile->second;
}

bool GHOST_XrSession::createActionSet(const GHOST_XrActionSetInfo *info)
{
  XrActionSetCreateInfo action_set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
  strcpy(action_set_info.actionSetName, info->name);
  strcpy(action_set_info.localizedActionSetName,
         info->name); /* Just use same name for localized. This can be changed in the future if
                         necessary. */
  action_set_info.priority = info->priority;

  OpenXRActionSet action_set;
  CHECK_XR(xrCreateActionSet(m_context->getInstance(), &action_set_info, &action_set.set),
           (m_error_msg = std::string("Failed to create action set \"") + info->name +
                          "\".\nName must not contain upper case letters or special characters "
                          "other than '-', '_', or '.'.")
               .c_str());

  std::map<std::string, OpenXRActionSet> &action_sets = m_oxr->action_sets;
  if (action_sets.find(info->name) == action_sets.end()) {
    action_sets.insert({info->name, std::move(action_set)});
  }
  else {
    action_sets[info->name] = std::move(action_set);
  }

  return true;
}

void GHOST_XrSession::destroyActionSet(const char *action_set_name, bool remove_reference)
{
  OpenXRActionSet *action_set = find_action_set(m_oxr.get(), action_set_name);
  if (action_set == nullptr) {
    return;
  }

  for (auto &action : action_set->actions) {
    for (auto &space : action.second.spaces) {
      CHECK_XR(xrDestroySpace(space.second),
               (m_error_msg = std::string("Failed to destroy space \"") + space.first +
                              "\" for action \"" + action.first + "\".")
                   .c_str());
    }
  }
  /* According to the spec, this will also destroy all actions in the set. */
  CHECK_XR(xrDestroyActionSet(action_set->set),
           (m_error_msg = std::string("Failed to destroy action set \"") + action_set_name + "\".")
               .c_str());

  if (remove_reference) {
    m_oxr->action_sets.erase(action_set_name);
  }
}

bool GHOST_XrSession::createActions(const char *action_set_name,
                                    uint32_t count,
                                    const GHOST_XrActionInfo *infos)
{
  OpenXRActionSet *action_set = find_action_set(m_oxr.get(), action_set_name);
  if (action_set == nullptr) {
    return false;
  }

  std::map<std::string, OpenXRAction> &actions = action_set->actions;
  XrInstance instance = m_context->getInstance();

  for (uint32_t i = 0; i < count; ++i) {
    const GHOST_XrActionInfo &info = infos[i];

    std::vector<XrPath> subaction_paths(info.count_subaction_paths);
    for (uint32_t i = 0; i < info.count_subaction_paths; ++i) {
      CHECK_XR(xrStringToPath(instance, info.subaction_paths[i], &subaction_paths[i]),
               (m_error_msg = std::string("Failed to get user path \"") + info.subaction_paths[i] +
                              "\".")
                   .c_str());
    }

    XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
    strcpy(action_info.actionName, info.name);
    strcpy(action_info.localizedActionName,
           info.name); /* Just use same name for localized. This can be changed in the future if
                          necessary. */

    switch (info.type) {
      case GHOST_kXrActionTypeBooleanInput: {
        action_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        break;
      }
      case GHOST_kXrActionTypeFloatInput: {
        action_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        break;
      }
      case GHOST_kXrActionTypeVector2fInput: {
        action_info.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
        break;
      }
      case GHOST_kXrActionTypePoseInput: {
        action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
        break;
      }
      case GHOST_kXrActionTypeVibrationOutput: {
        action_info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
        break;
      }
      default: {
        continue;
      }
    }
    action_info.countSubactionPaths = info.count_subaction_paths;
    action_info.subactionPaths = subaction_paths.data();

    OpenXRAction action;
    CHECK_XR(
        xrCreateAction(action_set->set, &action_info, &action.action),
        (m_error_msg = std::string("Failed to create action \"") + info.name +
                       "\".\nAction name and/or paths are invalid.\nName must not contain upper "
                       "case letters or special characters other than '-', '_', or '.'.")
            .c_str());

    if (actions.find(info.name) == actions.end()) {
      actions.insert({info.name, std::move(action)});
    }
    else {
      actions[info.name] = std::move(action);
    }
  }

  return true;
}

void GHOST_XrSession::destroyActions(const char *action_set_name,
                                     uint32_t count,
                                     const char *const *action_names)
{
  OpenXRActionSet *action_set = find_action_set(m_oxr.get(), action_set_name);
  if (action_set == nullptr) {
    return;
  }

  std::map<std::string, OpenXRAction> &actions = action_set->actions;

  for (uint32_t i = 0; i < count; ++i) {
    const char *action_name = action_names[i];

    OpenXRAction *action = find_action(action_set, action_name);
    if (action == nullptr) {
      continue;
    }

    for (auto &space : action->spaces) {
      CHECK_XR(xrDestroySpace(space.second),
               (m_error_msg = std::string("Failed to destroy space \"") + space.first +
                              "\" for action \"" + action_name + "\".")
                   .c_str());
    }
    CHECK_XR(
        xrDestroyAction(action->action),
        (m_error_msg = std::string("Failed to destroy action \"") + action_name + "\".").c_str());

    actions.erase(action_name);
  }
}

bool GHOST_XrSession::createActionSpaces(const char *action_set_name,
                                         uint32_t count,
                                         const GHOST_XrActionSpaceInfo *infos)
{
  OpenXRActionSet *action_set = find_action_set(m_oxr.get(), action_set_name);
  if (action_set == nullptr) {
    return false;
  }

  XrInstance instance = m_context->getInstance();
  XrSession &session = m_oxr->session;

  for (uint32_t action_idx = 0; action_idx < count; ++action_idx) {
    const GHOST_XrActionSpaceInfo &info = infos[action_idx];

    OpenXRAction *action = find_action(action_set, info.action_name);
    if (action == nullptr) {
      continue;
    }

    XrActionSpaceCreateInfo action_space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    action_space_info.action = action->action;

    for (uint32_t subaction_idx = 0; subaction_idx < info.count_subaction_paths; ++subaction_idx) {
      const char *subaction_path = info.subaction_paths[subaction_idx];

      CHECK_XR(xrStringToPath(instance, subaction_path, &action_space_info.subactionPath),
               (m_error_msg = std::string("Failed to get user path \"") + subaction_path + "\".")
                   .c_str());
      copy_ghost_pose_to_openxr_pose(info.poses[subaction_idx],
                                     action_space_info.poseInActionSpace);

      XrSpace space;
      CHECK_XR(xrCreateActionSpace(session, &action_space_info, &space),
               (m_error_msg = std::string("Failed to create space \"") + subaction_path +
                              "\" for action \"" + info.action_name + "\".")
                   .c_str());

      std::map<std::string, XrSpace> &spaces = action->spaces;
      if (spaces.find(info.action_name) == spaces.end()) {
        spaces.insert({subaction_path, std::move(space)});
      }
      else {
        spaces[subaction_path] = std::move(space);
      }
    }
  }

  return true;
}

void GHOST_XrSession::destroyActionSpaces(const char *action_set_name,
                                          uint32_t count,
                                          const GHOST_XrActionSpaceInfo *infos)
{
  OpenXRActionSet *action_set = find_action_set(m_oxr.get(), action_set_name);
  if (action_set == nullptr) {
    return;
  }

  for (uint32_t action_idx = 0; action_idx < count; ++action_idx) {
    const GHOST_XrActionSpaceInfo &info = infos[action_idx];

    OpenXRAction *action = find_action(action_set, info.action_name);
    if (action == nullptr) {
      continue;
    }

    std::map<std::string, XrSpace> &spaces = action->spaces;

    for (uint32_t subaction_idx = 0; subaction_idx < info.count_subaction_paths; ++subaction_idx) {
      const char *subaction_path = info.subaction_paths[subaction_idx];

      XrSpace *space = find_action_space(action, subaction_path);
      if (space == nullptr) {
        continue;
      }

      CHECK_XR(xrDestroySpace(*space),
               (m_error_msg = std::string("Failed to destroy space \"") + subaction_path +
                              "\" for action \"" + info.action_name + "\".")
                   .c_str());

      spaces.erase(subaction_path);
    }
  }
}

bool GHOST_XrSession::createActionBindings(const char *action_set_name,
                                           uint32_t count,
                                           const GHOST_XrActionBindingsInfo *infos)
{
  OpenXRActionSet *action_set = find_action_set(m_oxr.get(), action_set_name);
  if (action_set == nullptr) {
    return false;
  }

  XrInstance instance = m_context->getInstance();

  for (uint32_t profile_idx = 0; profile_idx < count; ++profile_idx) {
    const GHOST_XrActionBindingsInfo &info = infos[profile_idx];
    const char *interaction_profile_path = info.interaction_profile_path;

    XrInteractionProfileSuggestedBinding bindings_info{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    CHECK_XR(xrStringToPath(instance, interaction_profile_path, &bindings_info.interactionProfile),
             (m_error_msg = std::string("Failed to get interaction profile path \"") +
                            interaction_profile_path + "\".")
                 .c_str());

    std::vector<XrActionSuggestedBinding> sbindings(info.count_bindings); /* suggested bindings */
    std::map<std::string, XrPath> nbindings;                              /* new bindings */

    for (uint32_t binding_idx = 0; binding_idx < info.count_bindings; ++binding_idx) {
      const GHOST_XrActionBinding &binding = info.bindings[binding_idx];

      OpenXRAction *action = find_action(action_set, binding.action_name);
      if (action == nullptr) {
        sbindings.pop_back();
        continue;
      }

      XrActionSuggestedBinding &sbinding = sbindings[binding_idx];
      sbinding.action = action->action;
      CHECK_XR(xrStringToPath(instance, binding.interaction_path, &sbinding.binding),
               (m_error_msg = std::string("Failed to get interaction path \"") +
                              binding.interaction_path + "\".")
                   .c_str());

      nbindings.insert({binding.interaction_path, sbinding.binding});
    }

    /* Since xrSuggestInteractionProfileBindings() overwrites all bindings, we
     * need to re-add any existing bindings for the interaction profile. */
    for (auto &action : action_set->actions) {
      OpenXRActionProfile *profile = find_action_profile(&action.second, interaction_profile_path);
      if (profile == nullptr) {
        continue;
      }
      for (auto &binding : profile->bindings) {
        if (nbindings.find(binding.first) != nbindings.end()) {
          continue;
        }
        XrActionSuggestedBinding sbinding;
        sbinding.action = action.second.action;
        sbinding.binding = binding.second;

        sbindings.push_back(std::move(sbinding));
      }
    }

    bindings_info.countSuggestedBindings = (uint32_t)sbindings.size();
    bindings_info.suggestedBindings = sbindings.data();

    CHECK_XR(xrSuggestInteractionProfileBindings(instance, &bindings_info),
             (m_error_msg = std::string("Failed to create bindings for profile \"") +
                            interaction_profile_path + "\".\n" +
                            "Are the profile and action paths correct?")
                 .c_str());

    for (uint32_t binding_idx = 0; binding_idx < info.count_bindings; ++binding_idx) {
      const GHOST_XrActionBinding &binding = info.bindings[binding_idx];

      auto nbinding = nbindings.find(binding.interaction_path);
      if (nbinding == nbindings.end()) {
        continue;
      }

      OpenXRAction *action = find_action(action_set, binding.action_name);
      if (action == nullptr) {
        continue;
      }

      OpenXRActionProfile *profile = find_action_profile(action, interaction_profile_path);
      if (profile == nullptr) {
        OpenXRActionProfile p;
        p.profile = bindings_info.interactionProfile;
        p.bindings.insert({nbinding->first, nbinding->second});

        action->profiles.insert({interaction_profile_path, std::move(p)});
      }
      else {
        std::map<std::string, XrPath> &bindings = profile->bindings;
        if (bindings.find(nbinding->first) == bindings.end()) {
          bindings.insert({nbinding->first, nbinding->second});
        }
        else {
          bindings[interaction_profile_path] = nbinding->second;
        }
      }
    }
  }

  return true;
}

void GHOST_XrSession::destroyActionBindings(const char *action_set_name,
                                            uint32_t count,
                                            const GHOST_XrActionBindingsInfo *infos)
{
  OpenXRActionSet *action_set = find_action_set(m_oxr.get(), action_set_name);
  if (action_set == nullptr) {
    return;
  }

  XrInstance instance = m_context->getInstance();

  for (uint32_t profile_idx = 0; profile_idx < count; ++profile_idx) {
    const GHOST_XrActionBindingsInfo &info = infos[profile_idx];
    const char *interaction_profile_path = info.interaction_profile_path;

    XrInteractionProfileSuggestedBinding bindings_info{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    CHECK_XR(xrStringToPath(instance, interaction_profile_path, &bindings_info.interactionProfile),
             (m_error_msg = std::string("Failed to get interaction profile path \"") +
                            interaction_profile_path + "\".")
                 .c_str());

    std::vector<XrActionSuggestedBinding> sbindings; /* suggested bindings */
    std::set<std::string> dbindings;                 /* deleted bindings */

    for (uint32_t binding_idx = 0; binding_idx < info.count_bindings; ++binding_idx) {
      const GHOST_XrActionBinding &binding = info.bindings[binding_idx];

      OpenXRAction *action = find_action(action_set, binding.action_name);
      if (action == nullptr) {
        continue;
      }

      OpenXRActionProfile *profile = find_action_profile(action, interaction_profile_path);
      if (profile == nullptr) {
        continue;
      }

      if (profile->bindings.find(binding.interaction_path) == profile->bindings.end()) {
        continue;
      }

      dbindings.insert({binding.interaction_path});
    }

    /* Create list of suggested bindings that excludes deleted bindings. */
    for (auto &action : action_set->actions) {
      OpenXRActionProfile *profile = find_action_profile(&action.second, interaction_profile_path);
      if (profile == nullptr) {
        continue;
      }

      for (auto &binding : profile->bindings) {
        if (dbindings.find(binding.first) != dbindings.end()) {
          continue;
        }
        XrActionSuggestedBinding sbinding;
        sbinding.action = action.second.action;
        sbinding.binding = binding.second;

        sbindings.push_back(std::move(sbinding));
      }
    }

    bindings_info.countSuggestedBindings = (uint32_t)sbindings.size();
    bindings_info.suggestedBindings = sbindings.data();

    CHECK_XR(xrSuggestInteractionProfileBindings(instance, &bindings_info),
             (m_error_msg = std::string("Failed to destroy bindings for profile \"") +
                            interaction_profile_path + "\".\n" +
                            "Are the profile and action paths correct?")
                 .c_str());

    for (uint32_t binding_idx = 0; binding_idx < info.count_bindings; ++binding_idx) {
      const GHOST_XrActionBinding &binding = info.bindings[binding_idx];

      auto dbinding = dbindings.find(binding.interaction_path);
      if (dbinding == dbindings.end()) {
        continue;
      }

      OpenXRAction *action = find_action(action_set, binding.action_name);
      if (action == nullptr) {
        continue;
      }

      OpenXRActionProfile *profile = find_action_profile(action, interaction_profile_path);
      if (profile == nullptr) {
        continue;
      }

      std::map<std::string, XrPath> &bindings = profile->bindings;
      if (bindings.find(*dbinding) == bindings.end()) {
        continue;
      }
      bindings.erase(*dbinding);

      if (bindings.size() < 1) {
        action->profiles.erase(interaction_profile_path);
      }
    }
  }
}

bool GHOST_XrSession::attachActionSets()
{
  XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach_info.countActionSets = (uint32_t)m_oxr->action_sets.size();

  /* Create an aligned copy of the action sets to pass to xrAttachSessionActionSets().
   * Not that much of a performance concern since attachActionSets() should only be called once
   * per session. */
  std::vector<XrActionSet> action_sets(attach_info.countActionSets);
  uint32_t i = 0;
  for (auto &action_set : m_oxr->action_sets) {
    action_sets[i] = action_set.second.set;
    ++i;
  }
  attach_info.actionSets = action_sets.data();

  CHECK_XR(xrAttachSessionActionSets(m_oxr->session, &attach_info),
           "Failed to attach XR action sets.");

  return true;
}

bool GHOST_XrSession::syncActions(const char *action_set_name)
{
  std::map<std::string, OpenXRActionSet> &action_sets = m_oxr->action_sets;

  XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
  sync_info.countActiveActionSets = (action_set_name != nullptr) ? 1 :
                                                                   (uint32_t)action_sets.size();
  if (sync_info.countActiveActionSets < 1) {
    return false;
  }

  std::vector<XrActiveActionSet> active_action_sets(sync_info.countActiveActionSets);
  if (action_set_name != nullptr) {
    OpenXRActionSet *action_set = find_action_set(m_oxr.get(), action_set_name);
    if (action_set == nullptr || action_set->actions.size() < 1) {
      return false;
    }

    XrActiveActionSet &active_action_set = active_action_sets[0];
    active_action_set.actionSet = action_set->set;
    active_action_set.subactionPath = XR_NULL_PATH;
  }
  else {
    uint32_t i = 0;
    for (auto &action_set : action_sets) {
      if (action_set.second.actions.size() < 1) {
        active_action_sets.pop_back();
        --sync_info.countActiveActionSets;
        continue;
      }

      XrActiveActionSet &active_action_set = active_action_sets[i];
      active_action_set.actionSet = action_set.second.set;
      active_action_set.subactionPath = XR_NULL_PATH;
      ++i;
    }

    if (sync_info.countActiveActionSets < 1) {
      return false;
    }
  }
  sync_info.activeActionSets = active_action_sets.data();

  CHECK_XR(xrSyncActions(m_oxr->session, &sync_info), "Failed to synchronize XR actions.");

  return true;
}

bool GHOST_XrSession::getActionStates(const char *action_set_name,
                                      uint32_t count,
                                      GHOST_XrActionInfo *const *infos)
{
  OpenXRActionSet *action_set = find_action_set(m_oxr.get(), action_set_name);
  if (action_set == nullptr) {
    return false;
  }

  XrInstance instance = m_context->getInstance();
  XrSession &session = m_oxr->session;

  for (uint32_t action_idx = 0; action_idx < count; ++action_idx) {
    GHOST_XrActionInfo *info = infos[action_idx];

    OpenXRAction *action = find_action(action_set, info->name);
    if (action == nullptr) {
      continue;
    }

    XrActionStateGetInfo state_info{XR_TYPE_ACTION_STATE_GET_INFO};
    state_info.action = action->action;

    for (uint32_t subaction_idx = 0; subaction_idx < info->count_subaction_paths;
         ++subaction_idx) {
      const char *subaction_path = info->subaction_paths[subaction_idx];
      CHECK_XR(xrStringToPath(instance, subaction_path, &state_info.subactionPath),
               (m_error_msg = std::string("Failed to get user path \"") + subaction_path + "\".")
                   .c_str());

      switch (info->type) {
        case GHOST_kXrActionTypeBooleanInput: {
          XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
          CHECK_XR(xrGetActionStateBoolean(session, &state_info, &state),
                   (m_error_msg = std::string("Failed to get state for boolean action \"") +
                                  info->name + "\".")
                       .c_str());
          if (state.isActive) {
            ((bool *)info->states)[subaction_idx] = state.currentState;
          }
          break;
        }
        case GHOST_kXrActionTypeFloatInput: {
          XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
          CHECK_XR(xrGetActionStateFloat(session, &state_info, &state),
                   (m_error_msg = std::string("Failed to get state for float action \"") +
                                  info->name + "\".")
                       .c_str());
          if (state.isActive) {
            ((float *)info->states)[subaction_idx] = state.currentState;
          }
          break;
        }
        case GHOST_kXrActionTypeVector2fInput: {
          XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
          CHECK_XR(xrGetActionStateVector2f(session, &state_info, &state),
                   (m_error_msg = std::string("Failed to get state for vector2f action \"") +
                                  info->name + "\".")
                       .c_str());
          if (state.isActive) {
            memcpy(
                ((float(*)[2])info->states)[subaction_idx], &state.currentState, sizeof(float[2]));
          }
          break;
        }
        case GHOST_kXrActionTypePoseInput: {
          XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
          CHECK_XR(
              xrGetActionStatePose(session, &state_info, &state),
              (m_error_msg = std::string("Failed to get state for action \"") + info->name + "\".")
                  .c_str());
          if (state.isActive) {
            XrSpace *space = find_action_space(action, subaction_path);
            if (space) {
              XrSpaceLocation space_location{XR_TYPE_SPACE_LOCATION};
              CHECK_XR(xrLocateSpace(*space,
                                     m_oxr->reference_space,
                                     m_draw_info->frame_state.predictedDisplayTime,
                                     &space_location),
                       (m_error_msg = std::string("Failed to query pose space \"") +
                                      subaction_path + "\" for action \"" + info->name + "\".")
                           .c_str());
              copy_openxr_pose_to_ghost_pose(space_location.pose,
                                             ((GHOST_XrPose *)info->states)[subaction_idx]);
            }
          }
          break;
        }
        default: {
          break;
        }
      }
    }
  }

  return true;
}

bool GHOST_XrSession::applyHapticAction(const char *action_set_name,
                                        const char *action_name,
                                        uint32_t count,
                                        const char *const *subaction_paths,
                                        const GHOST_TInt64 *duration,
                                        const float *frequency,
                                        const float *amplitude)
{
  OpenXRActionSet *action_set = find_action_set(m_oxr.get(), action_set_name);
  if (action_set == nullptr) {
    return false;
  }

  OpenXRAction *action = find_action(action_set, action_name);
  if (action == nullptr) {
    return false;
  }

  XrInstance instance = m_context->getInstance();
  XrSession &session = m_oxr->session;

  XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
  vibration.duration = (*duration == 0) ? XR_MIN_HAPTIC_DURATION :
                                          static_cast<XrDuration>(*duration);
  vibration.frequency = *frequency;
  vibration.amplitude = *amplitude;

  XrHapticActionInfo haptic_info{XR_TYPE_HAPTIC_ACTION_INFO};
  haptic_info.action = action->action;

  for (uint32_t i = 0; i < count; ++i) {
    CHECK_XR(xrStringToPath(instance, subaction_paths[i], &haptic_info.subactionPath),
             (m_error_msg = std::string("Failed to get user path \"") + subaction_paths[i] + "\".")
                 .c_str());

    CHECK_XR(xrApplyHapticFeedback(session, &haptic_info, (const XrHapticBaseHeader *)&vibration),
             (m_error_msg = std::string("Failed to apply haptic action \"") + action_name + "\".")
                 .c_str());
  }

  return true;
}

void GHOST_XrSession::stopHapticAction(const char *action_set_name,
                                       const char *action_name,
                                       uint32_t count,
                                       const char *const *subaction_paths)
{
  OpenXRActionSet *action_set = find_action_set(m_oxr.get(), action_set_name);
  if (action_set == nullptr) {
    return;
  }

  OpenXRAction *action = find_action(action_set, action_name);
  if (action == nullptr) {
    return;
  }

  XrInstance instance = m_context->getInstance();
  XrSession &session = m_oxr->session;

  XrHapticActionInfo haptic_info{XR_TYPE_HAPTIC_ACTION_INFO};
  haptic_info.action = action->action;

  for (uint32_t i = 0; i < count; ++i) {
    CHECK_XR(xrStringToPath(instance, subaction_paths[i], &haptic_info.subactionPath),
             (m_error_msg = std::string("Failed to get user path \"") + subaction_paths[i] + "\".")
                 .c_str());

    CHECK_XR(xrStopHapticFeedback(session, &haptic_info),
             (m_error_msg = std::string("Failed to stop haptic action \"") + action_name + "\".")
                 .c_str());
  }
}

/** \} */ /* Actions */
