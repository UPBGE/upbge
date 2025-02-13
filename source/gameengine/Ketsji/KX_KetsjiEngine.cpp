/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
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
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 * The engine ties all game modules together.
 */

/** \file gameengine/Ketsji/KX_KetsjiEngine.cpp
 *  \ingroup ketsji
 */

#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#endif

#include "KX_KetsjiEngine.h"

#include <fmt/format.h>

#include "BKE_context.hh"
#include "BLI_rect.h"
#include "../draw/intern/draw_command.hh"
#include "DRW_render.hh"
#include "GPU_context.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "WM_api.hh"

#include "BL_Converter.h"
#include "BL_SceneConverter.h"
#include "DEV_Joystick.h"  // for DEV_Joystick::HandleEvents
#include "KX_Camera.h"
#include "KX_Globals.h"
#include "KX_NetworkMessageScene.h"
#include "KX_PyConstraintBinding.h"
#include "KX_PythonInit.h"  // for updatePythonJoysticks
#include "PHY_IPhysicsEnvironment.h"
#include "RAS_FrameBuffer.h"
#include "RAS_ICanvas.h"
#include "SCA_IInputDevice.h"

#define DEFAULT_LOGIC_TIC_RATE 60.0

#ifdef FREE_WINDOWS /* XXX mingw64 (gcc 4.7.0) defines a macro for DrawText that translates to \
                       DrawTextA. Not good */
#  ifdef DrawText
#    undef DrawText
#  endif
#endif

KX_KetsjiEngine::CameraRenderData::CameraRenderData(KX_Camera *rendercam,
                                                    KX_Camera *cullingcam,
                                                    const RAS_Rect &area,
                                                    const RAS_Rect &viewport,
                                                    RAS_Rasterizer::StereoEye eye)
    : m_renderCamera(rendercam),
      m_cullingCamera(cullingcam),
      m_area(area),
      m_viewport(viewport),
      m_eye(eye)
{
  m_renderCamera->AddRef();
}

KX_KetsjiEngine::CameraRenderData::CameraRenderData(const CameraRenderData &other)
{
  m_renderCamera = CM_AddRef(other.m_renderCamera);
  m_cullingCamera = other.m_cullingCamera;
  m_area = other.m_area;
  m_viewport = other.m_viewport;
}

KX_KetsjiEngine::CameraRenderData::~CameraRenderData()
{
  m_renderCamera->Release();
}

KX_KetsjiEngine::SceneRenderData::SceneRenderData(KX_Scene *scene) : m_scene(scene)
{
}

KX_KetsjiEngine::FrameRenderData::FrameRenderData(RAS_Rasterizer::FrameBufferType fbType)
    : m_fbType(fbType)
{
}

const std::string KX_KetsjiEngine::m_profileLabels[tc_numCategories] = {
    "Physics:",     // tc_physics
    "Logic:",       // tc_logic
    "Animations:",  // tc_animations
    "Depsgraph:",   // tc_depsgraph
    "Network:",     // tc_network
    "Scenegraph:",  // tc_scenegraph
    "Rasterizer:",  // tc_rasterizer
    "Services:",    // tc_services
    "Overhead:",    // tc_overhead
    "Outside:",     // tc_outside
    "GPU Latency:"  // tc_latency
};

/**
 * Constructor of the Ketsji Engine
 */
KX_KetsjiEngine::KX_KetsjiEngine(KX_ISystem *system,
                                 bContext *C,
                                 bool useViewportRender,
                                 int shadingTypeRuntime)
    : m_context(C),                              // eevee
      m_useViewportRender(useViewportRender),    // eevee
      m_shadingTypeRuntime(shadingTypeRuntime),  // eevee
      m_canvas(nullptr),
      m_rasterizer(nullptr),
      m_kxsystem(system),
      m_converter(nullptr),
      m_inputDevice(nullptr),
      m_bInitialized(false),
      m_flags(AUTO_ADD_DEBUG_PROPERTIES),
      m_frameTime(0.0f),
      m_clockTime(0.0f),
      m_previousAnimTime(0.0f),
      m_timescale(1.0f),
      m_previousRealTime(0.0f),
      m_previous_deltaTime(0.0f),
      m_firstEngineFrame(true),
      m_maxLogicFrame(5),
      m_maxPhysicsFrame(5),
      m_ticrate(DEFAULT_LOGIC_TIC_RATE),
      m_anim_framerate(25.0),
      m_doRender(true),
      m_exitkey(130),
      m_exitcode(KX_ExitRequest::NO_REQUEST),
      m_exitstring(""),
      m_cameraZoom(1.0f),
      m_overrideCamZoom(1.0f),
      m_logger(KX_TimeCategoryLogger(m_clock, 25)),
      m_average_framerate(0.0),
      m_showBoundingBox(KX_DebugOption::DISABLE),
      m_showArmature(KX_DebugOption::DISABLE),
      m_showCameraFrustum(KX_DebugOption::DISABLE),
      m_showShadowFrustum(KX_DebugOption::DISABLE)
{
  for (int i = tc_first; i < tc_numCategories; i++) {
    m_logger.AddCategory((KX_TimeCategory)i);
  }

#ifdef WITH_PYTHON
  m_pyprofiledict = PyDict_New();
#endif

  m_scenes = new EXP_ListValue<KX_Scene>();
  m_renderingCameras = {};
}

/**
 *	Destructor of the Ketsji Engine, release all memory
 */
KX_KetsjiEngine::~KX_KetsjiEngine()
{
#ifdef WITH_PYTHON
  Py_CLEAR(m_pyprofiledict);
#endif

  m_scenes->Release();
}

/* EEVEE integration */
bContext *KX_KetsjiEngine::GetContext()
{
  return m_context;
}

bool KX_KetsjiEngine::UseViewportRender()
{
  return m_useViewportRender;
}

int KX_KetsjiEngine::ShadingTypeRuntime()
{
  return m_shadingTypeRuntime;
}

/* include Depsgraph update time in tc_depsgraph category
 * (it can include part of animations time too I guess).
 */
void KX_KetsjiEngine::CountDepsgraphTime()
{
  m_logger.StartLog(tc_depsgraph);
}

void KX_KetsjiEngine::EndCountDepsgraphTime()
{
  m_logger.StartLog(tc_rasterizer);
}

std::vector<KX_Camera *> KX_KetsjiEngine::GetRenderingCameras()
{
  return m_renderingCameras;
}

/* End of EEVEE integration */

void KX_KetsjiEngine::SetInputDevice(SCA_IInputDevice *inputDevice)
{
  BLI_assert(inputDevice);
  m_inputDevice = inputDevice;
}

void KX_KetsjiEngine::SetCanvas(RAS_ICanvas *canvas)
{
  BLI_assert(canvas);
  m_canvas = canvas;
}

void KX_KetsjiEngine::SetRasterizer(RAS_Rasterizer *rasterizer)
{
  BLI_assert(rasterizer);
  m_rasterizer = rasterizer;
}

void KX_KetsjiEngine::SetNetworkMessageManager(KX_NetworkMessageManager *manager)
{
  m_networkMessageManager = manager;
}

#ifdef WITH_PYTHON
PyObject *KX_KetsjiEngine::GetPyProfileDict()
{
  Py_INCREF(m_pyprofiledict);
  return m_pyprofiledict;
}
#endif

void KX_KetsjiEngine::SetConverter(BL_Converter *converter)
{
  BLI_assert(converter);
  m_converter = converter;
}

void KX_KetsjiEngine::StartEngine()
{
  // Reset the clock to start at 0.0.
  m_clock.Reset();

  m_bInitialized = true;
}

void KX_KetsjiEngine::BeginFrame()
{
  m_rasterizer->BeginFrame(m_frameTime);

  m_canvas->BeginDraw();
}

void KX_KetsjiEngine::EndFrame()
{
  // Show profiling info
  m_logger.StartLog(tc_overhead);
  if (m_flags & (SHOW_PROFILE | SHOW_FRAMERATE | SHOW_DEBUG_PROPERTIES)) {
    RenderDebugProperties();
  }

  m_rasterizer->FlushDebugDraw(m_canvas);

  double tottime = m_logger.GetAverage();
  if (tottime < 1e-6)
    tottime = 1e-6;

#ifdef WITH_PYTHON
  for (int i = tc_first; i < tc_numCategories; ++i) {
    double time = m_logger.GetAverage((KX_TimeCategory)i);
    PyObject *val = PyTuple_New(2);
    PyTuple_SetItem(val, 0, PyFloat_FromDouble(time * 1000.0));
    PyTuple_SetItem(val, 1, PyFloat_FromDouble(time / tottime * 100.0));

    PyDict_SetItemString(m_pyprofiledict, m_profileLabels[i].c_str(), val);
    Py_DECREF(val);
  }
#endif

  m_average_framerate = 1.0 / tottime;

  // Go to next profiling measurement, time spent after this call is shown in the next frame.
  m_logger.NextMeasurement();

  m_logger.StartLog(tc_rasterizer);
  m_rasterizer->EndFrame();

  m_logger.StartLog(tc_logic);
  m_canvas->FlushScreenshots();

  // swap backbuffer (drawing into this buffer) <-> front/visible buffer
  m_logger.StartLog(tc_latency);
  m_canvas->SwapBuffers();
  m_logger.StartLog(tc_rasterizer);

  m_canvas->EndDraw();
}

void KX_KetsjiEngine::EndFrameViewportRender()
{
  // Show profiling info
  m_logger.StartLog(tc_overhead);
  if (m_flags & (SHOW_PROFILE | SHOW_FRAMERATE | SHOW_DEBUG_PROPERTIES)) {
    RenderDebugProperties();
  }

  m_rasterizer->FlushDebugDraw(m_canvas);

  double tottime = m_logger.GetAverage();
  if (tottime < 1e-6)
    tottime = 1e-6;

#ifdef WITH_PYTHON
  for (int i = tc_first; i < tc_numCategories; ++i) {
    double time = m_logger.GetAverage((KX_TimeCategory)i);
    PyObject *val = PyTuple_New(2);
    PyTuple_SetItem(val, 0, PyFloat_FromDouble(time * 1000.0));
    PyTuple_SetItem(val, 1, PyFloat_FromDouble(time / tottime * 100.0));

    PyDict_SetItemString(m_pyprofiledict, m_profileLabels[i].c_str(), val);
    Py_DECREF(val);
  }
#endif

  m_average_framerate = 1.0 / tottime;

  // Go to next profiling measurement, time spent after this call is shown in the next frame.
  m_logger.NextMeasurement();

  m_logger.StartLog(tc_rasterizer);
  // m_rasterizer->EndFrame();

  m_logger.StartLog(tc_logic);
  m_canvas->FlushScreenshots();

  // swap backbuffer (drawing into this buffer) <-> front/visible buffer
  m_logger.StartLog(tc_latency);
  // m_canvas->SwapBuffers();
  m_logger.StartLog(tc_rasterizer);

  m_canvas->EndDraw();
}

KX_KetsjiEngine::FrameTimes KX_KetsjiEngine::GetFrameTimes()
{
  /*
   * Clock advancement. There is basically two case:
   *   - USE_EXTERNAL_CLOCK is true, the user is responsible to advance the time
   *   manually using setClockTime, so here, we do not do anything.
   *   - USE_EXTERNAL_CLOCK is false, we consider how much
   *   time has elapsed since last call and we scale this time by the time
   *   scaling parameter. If m_timescale is 1.0 (default value), the clock
   *   corresponds to the computer clock.
   *
   * Once clockTime has been computed, we will compute how many logic frames
   * will be executed before the next rendering phase (which will occur at "clockTime").
   * The game time elapsing between two logic frames (called framestep)
   * depends on several variables:
   *   - ticrate
   *   - max_physic_frame
   *   - max_logic_frame
   *   - fixed_framerate
   */

  // Update time if the user is not controlling it.
  if (!(m_flags & USE_EXTERNAL_CLOCK)) {
    m_clockTime = m_clock.GetTimeSecond();
  }

  // if it's the first frame of the game, put m_previousRealTime = m_clockTime to avoid problems.
  if (m_firstEngineFrame) {
    m_previousRealTime = m_clockTime;
    m_firstEngineFrame = false;
  }

  // Get elapsed time.
  double dt = m_clockTime - m_previousRealTime;

  // Fix strange behavior of deltaTime and physics.
  const double averageFrameRate = GetAverageFrameRate();
  double maxDeltaTime = 1.5f;

  // Below 1fps, deltaTime tends to be close to 1, there is no need to adjust.
  if (averageFrameRate >= 1.5f) {
    maxDeltaTime = (averageFrameRate < 15.0f) ? m_previous_deltaTime + 0.5f :
                                                m_previous_deltaTime + 0.05f;  // Max dt
  }
  m_previous_deltaTime = dt;

  // If it exceeds the maximum value, adjust it to the maximum value, this prevents objects from
  // having sudden movements.
  if (dt > maxDeltaTime) {
    dt = maxDeltaTime;  // set deltaTime to max value.
  }

  // Time of a frame (without scale).
  double timestep;
  if (m_flags & FIXED_FRAMERATE) {
    // Normal time step for fixed frame.
    timestep = 1.0 / m_ticrate;
  }
  else {
    // The frame is the smallest as possible.
    timestep = dt;
  }

  // Number of frames to proceed.
  int frames;
  if (m_flags & FIXED_FRAMERATE) {
    // As many as possible for the elapsed time.
    frames = int(dt * m_ticrate);
  }
  else {
    // Proceed always one frame in non-fixed framerate.
    frames = 1;
  }

  // Fix timestep to not exceed max physics and logic frames.
  int maxFrames = max_ii(m_maxLogicFrame, m_maxPhysicsFrame);
  if (frames > maxFrames) {
    timestep = dt / maxFrames;
    frames = maxFrames;
  }

  // If the number of frame is non-zero, update previous time.
  if (frames > 0) {
    m_previousRealTime = m_clockTime;
  }
  //// Else in case of fixed framerate, try to sleep until the next frame.
  // else if (m_flags & FIXED_FRAMERATE) {
  //  const double sleeptime = timestep - dt - 1.0e-3;
  //  /* If the remaining time is greather than 1ms (sleep resolution) sleep this thread.
  //   * The other 1ms will be busy wait.
  //   */
  //  if (sleeptime > 0.0) {
  //    std::this_thread::sleep_for(std::chrono::nanoseconds((long)(sleeptime * 1.0e9)));
  //  }
  //}

  // Frame time with time scale.
  const double framestep = timestep * m_timescale;

  FrameTimes times;
  times.frames = frames;
  times.timestep = timestep;
  times.framestep = framestep;

  return times;
}

bool KX_KetsjiEngine::NextFrame()
{
  m_logger.StartLog(tc_services);

  const FrameTimes times = GetFrameTimes();

  // Exit if zero frame is sheduled.
  if (times.frames == 0) {
    // Start logging time spent outside main loop
    m_logger.StartLog(tc_outside);

    return false;
  }

  for (unsigned short i = 0; i < times.frames; ++i) {
    m_frameTime += times.framestep;

    m_converter->MergeAsyncLoads();

    m_inputDevice->ReleaseMoveEvent();

#ifdef WITH_SDL
    // Handle all SDL Joystick events here to share them for all scenes properly.
    short addrem[JOYINDEX_MAX] = {0};
    if (DEV_Joystick::HandleEvents(addrem)) {
#  ifdef WITH_PYTHON
      updatePythonJoysticks(addrem);
#  endif  // WITH_PYTHON
    }

    // Process Joystick force feedback duration
    for (unsigned short j = 0; j < JOYINDEX_MAX; ++j) {
      DEV_Joystick *joy = DEV_Joystick::GetInstance(j);
      if (joy && joy->Connected() && joy->GetRumbleSupport() && joy->GetRumbleStatus()) {
        joy->ProcessRumbleStatus();
      }
    }
#endif  // WITH_SDL

    // for each scene, call the proceed functions
    for (KX_Scene *scene : m_scenes) {
      /* Suspension holds the physics and logic processing for an
       * entire scene. Objects can be suspended individually, and
       * the settings for that precede the logic and physics
       * update. */
      m_logger.StartLog(tc_logic);

      if (i == 0) {  // No need to UpdateObjectActivity several times
        scene->UpdateObjectActivity();
      }

      m_logger.StartLog(tc_physics);

      // set Python hooks for each scene
      KX_SetActiveScene(scene);

      // Process sensors, and controllers
      m_logger.StartLog(tc_logic);
      scene->LogicBeginFrame(m_frameTime, times.framestep);

      // Scenegraph needs to be updated again, because Logic Controllers
      // can affect the local matrices.
      m_logger.StartLog(tc_scenegraph);
      scene->UpdateParents(m_frameTime);

      // Process actuators

      // Do some cleanup work for this logic frame
      m_logger.StartLog(tc_logic);
      scene->LogicUpdateFrame(m_frameTime);

      scene->LogicEndFrame();

      // Actuators can affect the scenegraph
      m_logger.StartLog(tc_scenegraph);
      scene->UpdateParents(m_frameTime);

      m_logger.StartLog(tc_physics);

      // Perform physics calculations on the scene. This can involve
      // many iterations of the physics solver.
      scene->GetPhysicsEnvironment()->ProceedDeltaTime(
          m_frameTime, times.timestep, times.framestep);  // m_deltatimerealDeltaTime);

      /* No need to call sofbody update more than 1 time */
      if (i == times.frames - 1) {
        scene->GetPhysicsEnvironment()->UpdateSoftBodies();
      }

      m_logger.StartLog(tc_scenegraph);
      scene->UpdateParents(m_frameTime);

      m_logger.StartLog(tc_services);
    }

    m_logger.StartLog(tc_network);
    m_networkMessageManager->ClearMessages();

    // update system devices
    m_logger.StartLog(tc_logic);
    m_inputDevice->ClearInputs();

    // scene management
    ProcessScheduledScenes();
  }

  // Start logging time spent outside main loop
  m_logger.StartLog(tc_outside);

  return m_doRender;
}

KX_KetsjiEngine::CameraRenderData KX_KetsjiEngine::GetCameraRenderData(
    KX_Scene *scene,
    KX_Camera *camera,
    KX_Camera *overrideCullingCam,
    const RAS_Rect &displayArea,
    RAS_Rasterizer::StereoEye eye,
    bool usestereo)
{
  KX_Camera *rendercam;
  /* In case of stereo we must copy the camera because it is used twice with different settings
   * (modelview matrix). This copy use the same transform settings that the original camera
   * and its name is based on with the eye number in addition.
   */
  if (usestereo) {
    rendercam = new KX_Camera();

    rendercam->SetScene(scene);
    rendercam->SetCameraData(*camera->GetCameraData());
    rendercam->SetName("__stereo_" + camera->GetName() + "_" + std::to_string(eye) + "__");
    rendercam->NodeSetGlobalOrientation(camera->NodeGetWorldOrientation());
    rendercam->NodeSetWorldPosition(camera->NodeGetWorldPosition());
    rendercam->NodeSetWorldScale(camera->NodeGetWorldScaling());
    rendercam->NodeUpdateGS(0.0);
    rendercam->MarkForDeletion();
  }
  // Else use the native camera.
  else {
    rendercam = camera;
  }

  KX_Camera *cullingcam = (overrideCullingCam) ? overrideCullingCam : rendercam;

  KX_SetActiveScene(scene);

  RAS_Rect area;
  RAS_Rect viewport;
  // Compute the area and the viewport based on the current display area and the optional camera
  // viewport.
  GetSceneViewport(scene, rendercam, displayArea, area, viewport);

  // Compute the camera matrices: modelview and projection.
  const MT_Matrix4x4 viewmat = m_rasterizer->GetViewMatrix(
      eye, rendercam->GetWorldToCamera(), rendercam->GetCameraData()->m_perspective);
  const MT_Matrix4x4 projmat = GetCameraProjectionMatrix(scene, rendercam, eye, viewport, area);
  rendercam->SetModelviewMatrix(viewmat);
  rendercam->SetProjectionMatrix(projmat);

  CameraRenderData cameraData(rendercam, cullingcam, area, viewport, eye);

  if (usestereo) {
    rendercam->Release();
  }

  return cameraData;
}

static void overlay_cam_at_the_end_of_camera_list_ensure(KX_Scene *scene)
{
  if (scene->GetOverlayCamera()) {
    EXP_ListValue<KX_Camera> *camList = scene->GetCameraList();
    EXP_ListValue<KX_Camera> *sortedList = new EXP_ListValue<KX_Camera>();
    for (KX_Camera *cam : camList) {
      if (cam != scene->GetOverlayCamera()) {
        sortedList->Add(CM_AddRef(cam));
      }
    }
    sortedList->Add(CM_AddRef(scene->GetOverlayCamera()));
    scene->GetCameraList()->Release();
    scene->SetCameraList(sortedList);
  }
}

bool KX_KetsjiEngine::GetFrameRenderData(std::vector<FrameRenderData> &frameDataList)
{
  const RAS_Rasterizer::StereoMode stereomode = m_rasterizer->GetStereoMode();
  const bool usestereo = (stereomode != RAS_Rasterizer::RAS_STEREO_NOSTEREO);
  // Set to true when each eye needs to be rendered in a separated off screen.
  const bool renderpereye = stereomode == RAS_Rasterizer::RAS_STEREO_INTERLACED ||
                            stereomode == RAS_Rasterizer::RAS_STEREO_VINTERLACE ||
                            stereomode == RAS_Rasterizer::RAS_STEREO_ANAGLYPH;

  // The number of eyes to manage in case of stereo.
  const unsigned short numeyes = (usestereo) ? 2 : 1;
  // The number of frames in case of stereo, could be multiple for interlaced or anaglyph stereo.
  const unsigned short numframes = (renderpereye) ? 2 : 1;

  // The off screen corresponding to the frame.
  static const RAS_Rasterizer::FrameBufferType fbType[] = {
      RAS_Rasterizer::RAS_FRAMEBUFFER_EYE_LEFT0,
      RAS_Rasterizer::RAS_FRAMEBUFFER_EYE_RIGHT0,
  };

  // Pre-compute the display area used for stereo or normal rendering.
  std::vector<RAS_Rect> displayAreas;
  for (unsigned short eye = 0; eye < numeyes; ++eye) {
    displayAreas.push_back(m_rasterizer->GetRenderArea(
        m_canvas, RAS_Rasterizer::RAS_STEREO_LEFTEYE /*(RAS_Rasterizer::StereoEye)eye)*/));
  }

  // Prepare override culling camera of each scenes, we don't manage stereo currently.
  for (KX_Scene *scene : m_scenes) {
    KX_Camera *overrideCullingCam = scene->GetOverrideCullingCamera();

    if (overrideCullingCam) {
      RAS_Rect area;
      RAS_Rect viewport;
      // Compute the area and the viewport based on the current display area and the optional
      // camera viewport.
      GetSceneViewport(scene,
                       overrideCullingCam,
                       displayAreas[RAS_Rasterizer::RAS_STEREO_LEFTEYE],
                       area,
                       viewport);
      // Compute the camera matrices: modelview and projection.
      const MT_Matrix4x4 viewmat = m_rasterizer->GetViewMatrix(
          RAS_Rasterizer::RAS_STEREO_LEFTEYE,
          overrideCullingCam->GetWorldToCamera(),
          overrideCullingCam->GetCameraData()->m_perspective);
      const MT_Matrix4x4 projmat = GetCameraProjectionMatrix(
          scene, overrideCullingCam, RAS_Rasterizer::RAS_STEREO_LEFTEYE, viewport, area);
      overrideCullingCam->SetModelviewMatrix(viewmat);
      overrideCullingCam->SetProjectionMatrix(projmat);
    }
  }

  for (unsigned short frame = 0; frame < numframes; ++frame) {
    frameDataList.emplace_back(fbType[frame]);
    FrameRenderData &frameData = frameDataList.back();

    // Get the eyes managed per frame.
    std::vector<RAS_Rasterizer::StereoEye> eyes;
    // One eye per frame but different.
    if (renderpereye) {
      eyes = {(RAS_Rasterizer::StereoEye)frame};
    }
    // Two eyes for unique frame.
    else if (usestereo) {
      eyes = {RAS_Rasterizer::RAS_STEREO_LEFTEYE, RAS_Rasterizer::RAS_STEREO_RIGHTEYE};
    }
    // Only one eye for unique frame.
    else {
      eyes = {RAS_Rasterizer::RAS_STEREO_LEFTEYE};
    }

    for (KX_Scene *scene : m_scenes) {
      frameData.m_sceneDataList.emplace_back(scene);
      SceneRenderData &sceneFrameData = frameData.m_sceneDataList.back();

      KX_Camera *activecam = scene->GetActiveCamera();

      /* Ensure the overlay camera is always at the end of cameras list
       * which will be rendered.
       */
      if (scene->GetOverlayCamera()) {
        overlay_cam_at_the_end_of_camera_list_ensure(scene);
      }

      m_renderingCameras.clear();

      KX_Camera *overrideCullingCam = scene->GetOverrideCullingCamera();
      for (KX_Camera *cam : scene->GetCameraList()) {
        if ((cam != activecam && cam != scene->GetOverlayCamera()) && !cam->GetViewport()) {
          continue;
        }

        m_renderingCameras.push_back(cam);

        for (RAS_Rasterizer::StereoEye eye : eyes) {
          sceneFrameData.m_cameraDataList.push_back(GetCameraRenderData(
              scene, cam, overrideCullingCam, displayAreas[eye], eye, usestereo));
        }
      }
    }
  }

  return renderpereye;
}

void KX_KetsjiEngine::Render()
{
  m_logger.StartLog(tc_rasterizer);

  BeginFrame();

  RAS_FrameBuffer *background_fb = m_rasterizer->GetFrameBuffer(
      RAS_Rasterizer::RAS_FRAMEBUFFER_EYE_RIGHT0);
  const int width = m_canvas->GetWidth();
  const int height = m_canvas->GetHeight();
  background_fb->UpdateSize(width + 1, height + 1);

  std::vector<FrameRenderData> frameDataList;
  GetFrameRenderData(frameDataList);

  KX_Scene *firstscene = m_scenes->GetFront();
  const RAS_FrameSettings &framesettings = firstscene->GetFramingType();

  // Used to detect when a camera is the first rendered an then doesn't request a depth clear.
  unsigned short pass = 0;

  for (FrameRenderData &frameData : frameDataList) {
    GPU_framebuffer_bind(background_fb->GetFrameBuffer());
    // Use the framing bar color set in the Blender scenes
    const float clear_col[4] = {
        framesettings.BarRed(), framesettings.BarGreen(), framesettings.BarBlue(), 1.0f};
    GPU_framebuffer_clear_color(background_fb->GetFrameBuffer(), clear_col);
    GPU_depth_mask(true);
    GPU_framebuffer_clear_depth(background_fb->GetFrameBuffer(), 1.0f);

    // for each scene, call the proceed functions
    for (unsigned short i = 0, size = frameData.m_sceneDataList.size(); i < size; ++i) {
      const SceneRenderData &sceneFrameData = frameData.m_sceneDataList[i];
      KX_Scene *scene = sceneFrameData.m_scene;

      m_rasterizer->SetAuxilaryClientInfo(scene);

      // Draw the scene once for each camera with an enabled viewport or an active camera.
      for (const CameraRenderData &cameraFrameData : sceneFrameData.m_cameraDataList) {
        // do the rendering
        RenderCamera(scene, background_fb, cameraFrameData, pass++);
      }
    }
  }

  if (!UseViewportRender()) {
    /* Clear the entire screen (draw a black background rect before drawing) */
    ARegion *region = CTX_wm_region(m_context);
    wmWindowViewport(CTX_wm_window(m_context));

    blender::draw::command::StateSet::set();
    GPU_depth_test(GPU_DEPTH_ALWAYS);

    uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    const float clear_col[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    immUniform4fv("color", clear_col);
    immRectf(pos,
             region->winrct.xmin,
             region->winrct.ymin,
             region->winrct.xmin + BLI_rcti_size_x(&region->winrct),
             region->winrct.ymin + BLI_rcti_size_y(&region->winrct));
    immUnbindProgram();

    /* Draw to the result of render loop on window backbuffer */
    rcti winrect = {0, m_canvas->GetWidth(), 0, m_canvas->GetHeight()};

    int v[4] = {m_canvas->GetViewportArea().GetLeft(),
                m_canvas->GetViewportArea().GetBottom(),
                m_canvas->GetWidth() + 1,
                m_canvas->GetHeight() + 1};

    GPU_viewport(v[0], v[1], v[2], v[3]);
    //if (GPU_backend_get_type() != GPU_BACKEND_VULKAN) {
      GPU_scissor_test(true);
      GPU_scissor(v[0], v[1], v[2], v[3]);
    //}

    GPU_matrix_ortho_set(0, width, 0, height, -100, 100);
    GPU_matrix_identity_set();

    DRW_transform_to_display(DRW_game_gpu_viewport_get(),
                             GPU_framebuffer_color_texture(background_fb->GetFrameBuffer()),
                             CTX_wm_view3d(m_context),
                             CTX_data_scene(m_context),
                             &winrect);

    /* Draw last remaining debug drawings + few stuff + swapBuffer */
    EndFrame();
  }
  else {
    EndFrameViewportRender();
  }
}

void KX_KetsjiEngine::RequestExit(KX_ExitRequest exitrequestmode)
{
  m_exitcode = exitrequestmode;
}

void KX_KetsjiEngine::SetNameNextGame(const std::string &nextgame)
{
  m_exitstring = nextgame;
}

KX_ExitRequest KX_KetsjiEngine::GetExitCode()
{
  // if a game actuator has set an exit code or if there are no scenes left
  if (m_exitcode == KX_ExitRequest::NO_REQUEST) {
    if (m_scenes->GetCount() == 0)
      m_exitcode = KX_ExitRequest::NO_SCENES_LEFT;
  }

  return m_exitcode;
}

const std::string &KX_KetsjiEngine::GetExitString()
{
  return m_exitstring;
}

void KX_KetsjiEngine::SetCameraZoom(float camzoom)
{
  m_cameraZoom = camzoom;
}

void KX_KetsjiEngine::SetCameraOverrideZoom(float camzoom)
{
  m_overrideCamZoom = camzoom;
}

float KX_KetsjiEngine::GetCameraZoom(KX_Camera *camera) const
{
  KX_Scene *scene = camera->GetScene();
  const bool overrideCamera = (m_flags & CAMERA_OVERRIDE) &&
                              (scene->GetName() == m_overrideSceneName) &&
                              (camera->GetName() == "__default__cam__");

  return overrideCamera ? m_overrideCamZoom : m_cameraZoom;
}

void KX_KetsjiEngine::EnableCameraOverride(const std::string &forscene,
                                           const MT_Matrix4x4 &projmat,
                                           const MT_Matrix4x4 &viewmat,
                                           const RAS_CameraData &camdata)
{
  SetFlag(CAMERA_OVERRIDE, true);
  m_overrideSceneName = forscene;
  m_overrideCamProjMat = projmat;
  m_overrideCamViewMat = viewmat;
  m_overrideCamData = camdata;
}

void KX_KetsjiEngine::GetSceneViewport(KX_Scene *scene,
                                       KX_Camera *cam,
                                       const RAS_Rect &displayArea,
                                       RAS_Rect &area,
                                       RAS_Rect &viewport)
{
  // In this function we make sure the rasterizer settings are up-to-date.
  // We compute the viewport so that logic using this information is up-to-date.

  // Note we postpone computation of the projection matrix
  // so that we are using the latest camera position.
  if (cam->GetViewport()) {
    RAS_Rect userviewport;

    userviewport.SetLeft(cam->GetViewportLeft());
    userviewport.SetBottom(cam->GetViewportBottom());
    userviewport.SetRight(cam->GetViewportRight());
    userviewport.SetTop(cam->GetViewportTop());

    // Don't do bars on user specified viewport
    RAS_FrameSettings settings = scene->GetFramingType();
    if (settings.FrameType() == RAS_FrameSettings::e_frame_bars)
      settings.SetFrameType(RAS_FrameSettings::e_frame_extend);

    RAS_FramingManager::ComputeViewport(scene->GetFramingType(), userviewport, viewport);

    area = userviewport;
  }
  else if (!(m_flags & CAMERA_OVERRIDE) || (scene->GetName() != m_overrideSceneName) ||
           !m_overrideCamData.m_perspective) {
    RAS_FramingManager::ComputeViewport(scene->GetFramingType(), displayArea, viewport);

    area = displayArea;
  }
  else {
    viewport.SetLeft(0);
    viewport.SetBottom(0);
    viewport.SetRight(int(m_canvas->GetWidth()));
    viewport.SetTop(int(m_canvas->GetHeight()));

    area = displayArea;
  }
}

void KX_KetsjiEngine::UpdateAnimations(KX_Scene *scene)
{
  // Handle the animations independently of the logic time step
  if (m_flags & RESTRICT_ANIMATION) {
    double anim_timestep = 1.0 / scene->GetAnimationFPS();
    if (m_frameTime - m_previousAnimTime > anim_timestep || m_frameTime == m_previousAnimTime) {
      // Sanity/debug print to make sure we're actually going at the fps we want (should be close
      // to anim_timestep) CM_Debug("Anim fps: " << 1.0/(m_frameTime - m_previousAnimTime));
      m_previousAnimTime = m_frameTime;
      for (KX_Scene *scene : m_scenes) {
        scene->UpdateAnimations(m_frameTime);
      }
    }
  }
  else
    scene->UpdateAnimations(m_frameTime);
}

MT_Matrix4x4 KX_KetsjiEngine::GetCameraProjectionMatrix(KX_Scene *scene,
                                                        KX_Camera *cam,
                                                        RAS_Rasterizer::StereoEye eye,
                                                        const RAS_Rect &viewport,
                                                        const RAS_Rect &area) const
{
  if (cam->hasValidProjectionMatrix()) {
    return cam->GetProjectionMatrix();
  }

  const bool override_camera = (m_flags & CAMERA_OVERRIDE) &&
                               (scene->GetName() == m_overrideSceneName) &&
                               (cam->GetName() == "__default__cam__");

  MT_Matrix4x4 projmat;
  if (override_camera && !m_overrideCamData.m_perspective) {
    // needed to get frustum planes for culling
    projmat = m_overrideCamProjMat;
  }
  else {
    RAS_FrameFrustum frustum;
    const bool orthographic = !cam->GetCameraData()->m_perspective;
    const float nearfrust = cam->GetCameraNear();
    const float farfrust = cam->GetCameraFar();
    const float focallength = cam->GetFocalLength();

    const float camzoom = override_camera ? m_overrideCamZoom : m_cameraZoom;
    if (orthographic) {

      RAS_FramingManager::ComputeOrtho(scene->GetFramingType(),
                                       area,
                                       viewport,
                                       cam->GetScale(),
                                       nearfrust,
                                       farfrust,
                                       cam->GetSensorFit(),
                                       cam->GetShiftHorizontal(),
                                       cam->GetShiftVertical(),
                                       frustum);

      if (!cam->GetViewport()) {
        frustum.x1 *= camzoom;
        frustum.x2 *= camzoom;
        frustum.y1 *= camzoom;
        frustum.y2 *= camzoom;
      }
      projmat = m_rasterizer->GetOrthoMatrix(
          frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar);
    }
    else {
      RAS_FramingManager::ComputeFrustum(scene->GetFramingType(),
                                         area,
                                         viewport,
                                         cam->GetLens(),
                                         cam->GetSensorWidth(),
                                         cam->GetSensorHeight(),
                                         cam->GetSensorFit(),
                                         cam->GetShiftHorizontal(),
                                         cam->GetShiftVertical(),
                                         nearfrust,
                                         farfrust,
                                         frustum);

      if (!cam->GetViewport()) {
        frustum.x1 *= camzoom;
        frustum.x2 *= camzoom;
        frustum.y1 *= camzoom;
        frustum.y2 *= camzoom;
      }
      projmat = m_rasterizer->GetFrustumMatrix(eye,
                                               frustum.x1,
                                               frustum.x2,
                                               frustum.y1,
                                               frustum.y2,
                                               frustum.camnear,
                                               frustum.camfar,
                                               focallength);
    }
  }

  return projmat;
}

// update graphics
void KX_KetsjiEngine::RenderCamera(KX_Scene *scene,
                                   RAS_FrameBuffer *background_fb,
                                   const CameraRenderData &cameraFrameData,
                                   unsigned short pass)
{
  KX_Camera *rendercam = cameraFrameData.m_renderCamera;
  // KX_Camera *cullingcam = cameraFrameData.m_cullingCamera;
  // const RAS_Rect &area = cameraFrameData.m_area;
  const RAS_Rect &viewport = cameraFrameData.m_viewport;

  KX_SetActiveScene(scene);

  /* Clear the depth after setting the scene viewport/scissor
   * if it's not the first render pass. */
  if (pass > 0) {
    GPU_depth_mask(true);
    GPU_framebuffer_clear_depth(background_fb->GetFrameBuffer(), 1.0f);
  }

  m_rasterizer->SetEye(RAS_Rasterizer::RAS_STEREO_LEFTEYE /*cameraFrameData.m_eye*/);

  m_logger.StartLog(tc_scenegraph);

  m_logger.StartLog(tc_animations);
  UpdateAnimations(scene);

  m_logger.StartLog(tc_rasterizer);

#ifdef WITH_PYTHON
  // Run any pre-drawing python callbacks
  scene->RunDrawingCallbacks(KX_Scene::PRE_DRAW, rendercam);
#endif

  if (scene->GetInitMaterialsGPUViewport()) {
    scene->SetInitMaterialsGPUViewport(nullptr);
  }

  bool is_overlay_pass = rendercam == scene->GetOverlayCamera();
  if (is_overlay_pass || (rendercam != scene->GetActiveCamera() && rendercam->GetViewport())) {
    GPU_blend(GPU_BLEND_ALPHA_PREMULT);
  }

  bool is_last_render_pass = rendercam == m_renderingCameras.back();

  scene->RenderAfterCameraSetup(rendercam, background_fb, viewport, is_overlay_pass, is_last_render_pass);

  if (scene->GetPhysicsEnvironment()) {
    scene->GetPhysicsEnvironment()->DebugDrawWorld();
  }
}

/*
 * To run once per scene
 */
RAS_FrameBuffer *KX_KetsjiEngine::PostRenderScene(KX_Scene *scene,
                                                  RAS_FrameBuffer *inputfb,
                                                  RAS_FrameBuffer *targetfb)
{
  KX_SetActiveScene(scene);

  m_rasterizer->FlushDebugDraw(m_canvas);

  // We need to first make sure our viewport is correct (enabling multiple viewports can mess this
  // up), only for filters.
  const int width = m_canvas->GetWidth();
  const int height = m_canvas->GetHeight();
  m_rasterizer->SetViewport(0, 0, width + 1, height + 1);
  m_rasterizer->SetScissor(0, 0, width + 1, height + 1);

  RAS_FrameBuffer *frameBuffer = scene->Render2DFilters(m_rasterizer, m_canvas, inputfb, targetfb);

#ifdef WITH_PYTHON
  /* We can't deduce what camera should be passed to the python callbacks
   * because the post draw callbacks are per scenes and not per cameras.
   */
  scene->RunDrawingCallbacks(KX_Scene::POST_DRAW, nullptr);

  // Python draw callback can also call debug draw functions, so we have to clear debug shapes.
  m_rasterizer->FlushDebugDraw(m_canvas);
#endif

  return frameBuffer;
}

void KX_KetsjiEngine::StopEngine()
{
  if (m_bInitialized) {
    m_converter->FinalizeAsyncLoads();

    while (m_scenes->GetCount() > 0) {
      KX_Scene *scene = m_scenes->GetFront();
      m_converter->RemoveScene(scene);
      // WARNING: here the scene is a dangling pointer.
      m_scenes->Remove(0);
    }

    // cleanup all the stuff
    m_rasterizer->Exit();
  }
}

// Scene Management is able to switch between scenes
// and have several scenes running in parallel
void KX_KetsjiEngine::AddScene(KX_Scene *scene)
{
  m_scenes->Add(CM_AddRef(scene));
  PostProcessScene(scene);
}

void KX_KetsjiEngine::PostProcessScene(KX_Scene *scene)
{
  bool override_camera = ((m_flags & CAMERA_OVERRIDE) &&
                          (scene->GetName() == m_overrideSceneName));

  // if there is no activecamera, or the camera is being
  // overridden we need to construct a temporary camera
  if (!scene->GetActiveCamera() || override_camera) {
    KX_Camera *activecam = nullptr;

    activecam = new KX_Camera();

    activecam->SetScene(scene);
    activecam->SetBlenderObject(scene->GetGameDefaultCamera());
    activecam->SetCameraData(override_camera ? m_overrideCamData : RAS_CameraData());
    activecam->SetName("__default__cam__");

    scene->GetBlenderSceneConverter()->RegisterGameObject(activecam,
                                                          activecam->GetBlenderObject());

    // set transformation
    if (override_camera) {
      MT_Transform trans = m_overrideCamViewMat.toTransform();
      MT_Transform camtrans;
      camtrans.invert(trans);

      activecam->NodeSetLocalPosition(camtrans.getOrigin());
      activecam->NodeSetLocalOrientation(camtrans.getBasis());
      activecam->NodeUpdateGS(0.0f);
    }
    else {
      bContext *C = KX_GetActiveEngine()->GetContext();
      RegionView3D *rv3d = CTX_wm_region_view3d(C);
      MT_Vector3 pos = MT_Vector3(rv3d->viewinv[3]);
      activecam->NodeSetLocalPosition(pos);
      activecam->NodeSetLocalOrientation(MT_Matrix3x3(&rv3d->viewinv[0][0]));
      activecam->NodeUpdateGS(0.0f);
    }

    scene->GetCameraList()->Add(CM_AddRef(activecam));
    scene->SetActiveCamera(activecam);
    scene->GetObjectList()->Add(CM_AddRef(activecam));
    scene->GetRootParentList()->Add(CM_AddRef(activecam));
    // done with activecam
    activecam->Release();
  }

  scene->UpdateParents(0.0f);
}

void KX_KetsjiEngine::RenderDebugProperties()
{

  short profile_size = KX_GetActiveScene()->GetBlenderScene()->gm.profileSize;

  std::string debugtxt;
  int title_xmargin = -7;
  int title_y_top_margin = 4;
  int title_y_bottom_margin = 2;

  int const_xindent = 4;
  int const_ysize = 14;

  int xcoord = 12;  // mmmm, these constants were taken from blender source
  int ycoord = 17;  // to 'mimic' behavior

  int profile_indent = 72;

  switch (profile_size) {
    case 0: // Change nothing to default values
      break;
    case 1:
      title_y_top_margin = 0.5;
      title_y_bottom_margin = -0.5;
      const_ysize = 21;
      xcoord = 18;
      ycoord = 25.5;
      profile_indent = 108;
      break;
    case 2: {
      title_y_top_margin = -3;
      title_y_bottom_margin = -3;
      const_ysize = 32;
      xcoord = 24;
      ycoord = 34;
      profile_indent = 172;
      break;
    }
    default:
      break;
  }

  float tottime = m_logger.GetAverage();
  if (tottime < 1e-6f) {
    tottime = 1e-6f;
  }

  static const MT_Vector4 white(1.0f, 1.0f, 1.0f, 1.0f);

  // Use nullptrfor no scene.
  RAS_DebugDraw &debugDraw = m_rasterizer->GetDebugDraw();

  if (m_flags & (SHOW_FRAMERATE | SHOW_PROFILE)) {
    // Title for profiling("Profile")
    // Adds the constant x indent (0 for now) to the title x margin
    debugDraw.RenderText2D(
        "Profile", MT_Vector2(xcoord + const_xindent + title_xmargin, ycoord), white);

    // Increase the indent by default increase
    ycoord += const_ysize;
    // Add the title indent afterwards
    ycoord += title_y_bottom_margin;
  }

  // Framerate display
  if (m_flags & SHOW_FRAMERATE) {
    debugDraw.RenderText2D("Frametime:", MT_Vector2(xcoord + const_xindent, ycoord), white);

    debugtxt = fmt::format("{:>5.2f}ms ({:.1f}fps)", (tottime * 1000.0f), (1.0f / tottime));
    debugDraw.RenderText2D(
        debugtxt, MT_Vector2(xcoord + const_xindent + profile_indent, ycoord), white);
    // Increase the indent by default increase
    ycoord += const_ysize;
  }

  // Profile display
  if (m_flags & SHOW_PROFILE) {
    for (int j = tc_first; j < tc_numCategories; j++) {
      debugDraw.RenderText2D(
          m_profileLabels[j], MT_Vector2(xcoord + const_xindent, ycoord), white);

      double time = m_logger.GetAverage((KX_TimeCategory)j);

      debugtxt = fmt::format("{:>5.2f}ms | {}%", (time * 1000.f), (int)(time / tottime * 100.f));
      debugDraw.RenderText2D(
          debugtxt, MT_Vector2(xcoord + const_xindent + profile_indent, ycoord), white);

      const MT_Vector2 boxSize(50 * (time / tottime), 10);
      debugDraw.RenderBox2D(
          MT_Vector2(xcoord + (int)(2.2 * profile_indent), ycoord), boxSize, white);
      ycoord += const_ysize;
    }
  }
  // Add the ymargin for titles below the other section of debug info
  ycoord += title_y_top_margin;

  /* Property display */
  if (m_flags & SHOW_DEBUG_PROPERTIES) {
    // Title for debugging("Debug properties")
    // Adds the constant x indent (0 for now) to the title x margin
    debugDraw.RenderText2D(
        "Debug Properties", MT_Vector2(xcoord + const_xindent + title_xmargin, ycoord), white);

    // Increase the indent by default increase
    ycoord += const_ysize;
    // Add the title indent afterwards
    ycoord += title_y_bottom_margin;

    /* Calculate amount of properties that can displayed. */
    const unsigned short propsMax = (m_canvas->GetHeight() - ycoord) / const_ysize;

    for (KX_Scene *scene : m_scenes) {
      scene->RenderDebugProperties(
          debugDraw, const_xindent, const_ysize, xcoord, ycoord, propsMax);
    }
  }
}

void KX_KetsjiEngine::DrawDebugCameraFrustum(KX_Scene *scene,
                                             RAS_DebugDraw &debugDraw,
                                             const CameraRenderData &cameraFrameData)
{
  if (m_showCameraFrustum == KX_DebugOption::DISABLE) {
    return;
  }

  for (KX_Camera *cam : scene->GetCameraList()) {
    if (cam != cameraFrameData.m_renderCamera &&
        (m_showCameraFrustum == KX_DebugOption::FORCE || cam->GetShowCameraFrustum())) {
      const MT_Matrix4x4 viewmat = m_rasterizer->GetViewMatrix(
          cameraFrameData.m_eye, cam->GetWorldToCamera(), cam->GetCameraData()->m_perspective);
      const MT_Matrix4x4 projmat = GetCameraProjectionMatrix(
          scene, cam, cameraFrameData.m_eye, cameraFrameData.m_viewport, cameraFrameData.m_area);
      debugDraw.DrawCameraFrustum(projmat * viewmat);
    }
  }
}

void KX_KetsjiEngine::DrawDebugShadowFrustum(KX_Scene *scene, RAS_DebugDraw &debugDraw)
{
  if (m_showShadowFrustum == KX_DebugOption::DISABLE) {
    return;
  }

  /*for (KX_LightObject *light : scene->GetLightList()) {
    if (m_showShadowFrustum == KX_DebugOption::FORCE || light->GetShowShadowFrustum()) {
      debugDraw.DrawCameraFrustum(light->GetShadowFrustumMatrix().inverse());
    }
  }*/
}

EXP_ListValue<KX_Scene> *KX_KetsjiEngine::CurrentScenes()
{
  return m_scenes;
}

KX_Scene *KX_KetsjiEngine::FindScene(const std::string &scenename)
{
  return m_scenes->FindValue(scenename);
}

void KX_KetsjiEngine::RemoveScene(const std::string &scenename)
{
  /****************EEVEE INTEGRATION*****************/
  // DISABLE REMOVE SCENES FOR NOW
  std::cout << "KX_KetsjiEngine::RemoveScene: Remove Scenes is temporarly disabled during eevee "
               "integration"
            << std::endl;
  return;
  /**************************************************/
  if (FindScene(scenename)) {
    m_removingScenes.push_back(scenename);
  }
  else {
    CM_Warning("scene " << scenename << " does not exist, not removed!");
  }
}

void KX_KetsjiEngine::RemoveScheduledScenes()
{
  if (m_removingScenes.size()) {
    std::vector<std::string>::iterator scenenameit;
    for (scenenameit = m_removingScenes.begin(); scenenameit != m_removingScenes.end();
         scenenameit++) {
      std::string scenename = *scenenameit;

      KX_Scene *scene = FindScene(scenename);
      if (scene) {
        m_converter->RemoveScene(scene);
        m_scenes->RemoveValue(scene);
      }
    }
    m_removingScenes.clear();
  }
}

KX_Scene *KX_KetsjiEngine::CreateScene(Scene *scene, bool libloading)
{
  KX_Scene *tmpscene = new KX_Scene(
      m_inputDevice, scene->id.name + 2, scene, m_canvas, m_networkMessageManager);

  m_converter->ConvertScene(tmpscene, m_rasterizer, m_canvas, libloading);

  return tmpscene;
}

KX_Scene *KX_KetsjiEngine::CreateScene(const std::string &scenename)
{
  Scene *scene = m_converter->GetBlenderSceneForName(scenename);
  if (!scene)
    return nullptr;

  return CreateScene(scene, false);
}

bool KX_KetsjiEngine::ReplaceScene(const std::string &oldscene, const std::string &newscene)
{
  // Don't allow replacement if the new scene doesn't exist.
  // Allows smarter game design (used to have no check here).
  // Note that it creates a small backward compatbility issue
  // for a game that did a replace followed by a lib load with the
  // new scene in the lib => it won't work anymore, the lib
  // must be loaded before doing the replace.
  if (m_converter->GetBlenderSceneForName(newscene) != nullptr) {
    m_replace_scenes.push_back(std::make_pair(oldscene, newscene));
    return true;
  }

  return false;
}

// replace scene is not the same as removing and adding because the
// scene must be in exact the same place (to maintain drawingorder)
// (nzc) - should that not be done with a scene-display list? It seems
// stupid to rely on the mem allocation order...
void KX_KetsjiEngine::ReplaceScheduledScenes()
{
  if (m_replace_scenes.size()) {
    std::vector<std::pair<std::string, std::string>>::iterator scenenameit;

    for (scenenameit = m_replace_scenes.begin(); scenenameit != m_replace_scenes.end();
         scenenameit++) {
      std::string oldscenename = (*scenenameit).first;
      std::string newscenename = (*scenenameit).second;
      /* Scenes are not supposed to be included twice... I think */
      for (unsigned int sce_idx = 0; sce_idx < m_scenes->GetCount(); ++sce_idx) {
        KX_Scene *scene = m_scenes->GetValue(sce_idx);
        if (scene->GetName() == oldscenename) {
          // avoid crash if the new scene doesn't exist, just do nothing
          Scene *blScene = m_converter->GetBlenderSceneForName(newscenename);
          if (blScene) {
            m_converter->RemoveScene(scene);

            KX_Scene *tmpscene = CreateScene(blScene, false);
            m_scenes->SetValue(sce_idx, CM_AddRef(tmpscene));
            PostProcessScene(tmpscene);
            tmpscene->Release();
          }
          else {
            CM_Warning("scene " << newscenename << " could not be found, not replaced!");
          }
        }
      }
    }
    m_replace_scenes.clear();
  }
}

double KX_KetsjiEngine::GetTicRate()
{
  return m_ticrate;
}

void KX_KetsjiEngine::SetTicRate(double ticrate)
{
  m_ticrate = ticrate;
}

double KX_KetsjiEngine::GetTimeScale() const
{
  return m_timescale;
}

void KX_KetsjiEngine::SetTimeScale(double timescale)
{
  m_timescale = timescale;
}

int KX_KetsjiEngine::GetMaxLogicFrame()
{
  return m_maxLogicFrame;
}

void KX_KetsjiEngine::SetMaxLogicFrame(int frame)
{
  m_maxLogicFrame = frame;
}

int KX_KetsjiEngine::GetMaxPhysicsFrame()
{
  return m_maxPhysicsFrame;
}

void KX_KetsjiEngine::SetMaxPhysicsFrame(int frame)
{
  m_maxPhysicsFrame = frame;
}

double KX_KetsjiEngine::GetAnimFrameRate()
{
  return m_anim_framerate;
}

bool KX_KetsjiEngine::GetFlag(FlagType flag) const
{
  return (m_flags & flag);
}

void KX_KetsjiEngine::SetFlag(FlagType flag, bool enable)
{
  if (enable) {
    m_flags = (FlagType)(m_flags | flag);
  }
  else {
    m_flags = (FlagType)(m_flags & ~flag);
  }
}

double KX_KetsjiEngine::GetClockTime(void) const
{
  return m_clockTime;
}

void KX_KetsjiEngine::SetClockTime(double externalClockTime)
{
  m_clockTime = externalClockTime;
}

double KX_KetsjiEngine::GetFrameTime(void) const
{
  return m_frameTime;
}

double KX_KetsjiEngine::GetRealTime(void) const
{
  return m_clock.GetTimeSecond();
}

void KX_KetsjiEngine::SetAnimFrameRate(double framerate)
{
  m_anim_framerate = framerate;
}

double KX_KetsjiEngine::GetAverageFrameRate()
{
  return m_average_framerate;
}

void KX_KetsjiEngine::SetExitKey(short key)
{
  m_exitkey = key;
}

short KX_KetsjiEngine::GetExitKey()
{
  return m_exitkey;
}

void KX_KetsjiEngine::SetRender(bool render)
{
  m_doRender = render;
}

bool KX_KetsjiEngine::GetRender()
{
  return m_doRender;
}

void KX_KetsjiEngine::ProcessScheduledScenes(void)
{
  // Check whether there will be changes to the list of scenes
  if (m_replace_scenes.size() || m_removingScenes.size()) {

    // Change the scene list
    ReplaceScheduledScenes();
    RemoveScheduledScenes();
  }
}

void KX_KetsjiEngine::SetShowBoundingBox(KX_DebugOption mode)
{
  m_showBoundingBox = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowBoundingBox() const
{
  return m_showBoundingBox;
}

void KX_KetsjiEngine::SetShowArmatures(KX_DebugOption mode)
{
  m_showArmature = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowArmatures() const
{
  return m_showArmature;
}

void KX_KetsjiEngine::SetShowCameraFrustum(KX_DebugOption mode)
{
  m_showCameraFrustum = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowCameraFrustum() const
{
  return m_showCameraFrustum;
}

void KX_KetsjiEngine::SetShowShadowFrustum(KX_DebugOption mode)
{
  m_showShadowFrustum = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowShadowFrustum() const
{
  return m_showShadowFrustum;
}

void KX_KetsjiEngine::Resize()
{
  /* extended mode needs to recalculate camera frusta when */
  KX_Scene *firstscene = m_scenes->GetFront();
  const RAS_FrameSettings &framesettings = firstscene->GetFramingType();

  if (framesettings.FrameType() == RAS_FrameSettings::e_frame_extend) {
    for (KX_Scene *scene : m_scenes) {
      KX_Camera *cam = scene->GetActiveCamera();
      cam->InvalidateProjectionMatrix();
      if (scene->GetOverlayCamera()) {
        scene->GetOverlayCamera()->InvalidateProjectionMatrix();
      }
    }
  }
}

void KX_KetsjiEngine::SetGlobalSettings(GlobalSettings *gs)
{
  m_globalsettings.glslflag = gs->glslflag;
}

GlobalSettings *KX_KetsjiEngine::GetGlobalSettings(void)
{
  return &m_globalsettings;
}
