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

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

#include <fmt/format.h>

#include "../draw/intern/draw_command.hh"
#include "BKE_global.hh"
#include "BLI_rect.h"
#include "BLI_time.h"
#include "DNA_scene_types.h"
#include "GPU_immediate.hh"
#include "GPU_viewport.hh"
#include "WM_api.hh"

#include "BL_Converter.h"
#include "BL_SceneConverter.h"
#include "DEV_InputDevice.h"
#include "DEV_Joystick.h"  // for DEV_Joystick::HandleEvents
#include "KX_Camera.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_NetworkMessageScene.h"
#include "KX_PythonInit.h"  // for updatePythonJoysticks
#include "PHY_IPhysicsEnvironment.h"
#include "RAS_FrameBuffer.h"
#include "RAS_ICanvas.h"
#include "SCA_IInputDevice.h"

#ifdef WITH_GAMEENGINE_LOGICNODES
#  include "BKE_game_persistence.hh"
#  include "BKE_main.hh"
#  include "BLI_uuid.h"
#  include "LN_GameState.h"
#  include "LN_InputMap.h"
#  include "LN_Manager.h"
#endif

using namespace blender;

#define DEFAULT_LOGIC_TIC_RATE 60.0

namespace {

constexpr double MIN_TIME_SCALE = 0.0;
constexpr double MAX_TIME_SCALE = 10000.0;

}  // namespace

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

KX_KetsjiEngine::SceneRenderData::SceneRenderData(KX_Scene *scene) : m_scene(scene) {}

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

std::array<std::deque<KX_KetsjiEngine::FrameTimeGraphSample>,
           KX_KetsjiEngine::FRAME_TIME_GRAPH_SLOT_COUNT>
    KX_KetsjiEngine::m_sharedFrameTimeGraphSlots = {};

namespace {
constexpr int frame_time_graph_default_target_fps = 60;
constexpr int frame_time_graph_default_window_seconds = 2;
constexpr int frame_time_graph_min_window_seconds = 1;
constexpr int frame_time_graph_max_window_seconds = 10;
constexpr int frame_time_graph_default_max_samples = 10000;
constexpr int frame_time_graph_min_max_samples = 120;
constexpr int frame_time_graph_max_max_samples = 60000;

int sanitize_frame_time_graph_window_seconds(const int window_seconds)
{
  switch (window_seconds) {
    case 1:
    case 2:
    case 5:
    case 10:
      return window_seconds;
    default:
      return frame_time_graph_default_window_seconds;
  }
}

int sanitize_frame_time_graph_axis(const int axis)
{
  return (axis == GAME_FRAME_TIME_GRAPH_AXIS_SECONDS) ? GAME_FRAME_TIME_GRAPH_AXIS_SECONDS :
                                                        GAME_FRAME_TIME_GRAPH_AXIS_FRAMES;
}

int sanitize_frame_time_graph_style(const int style)
{
  return (style == GAME_FRAME_TIME_GRAPH_STYLE_BARS) ? GAME_FRAME_TIME_GRAPH_STYLE_BARS :
                                                       GAME_FRAME_TIME_GRAPH_STYLE_LINE;
}

int sanitize_frame_time_graph_visible_domains(const int domains)
{
  return domains & GAME_FRAME_TIME_GRAPH_DOMAIN_ALL;
}

int sanitize_frame_time_graph_max_samples(const int max_samples)
{
  return std::clamp(
      max_samples, frame_time_graph_min_max_samples, frame_time_graph_max_max_samples);
}

}  // namespace

struct KX_KetsjiEngine::PendingGameLoad {
#ifdef WITH_GAMEENGINE_LOGICNODES
  KX_Scene *source_scene = nullptr;
  blender::Scene *target_scene = nullptr;
  blender::bUUID target_scene_uuid = {};
  std::string source_scene_name;
  std::string target_scene_name;
  std::string filepath;
  LN_GameStateCompletionTarget completion_target;
  std::unique_ptr<LN_GameStateLoadPayload> payload;
#endif
};

/**
 * Constructor of the Ketsji Engine
 */
KX_KetsjiEngine::KX_KetsjiEngine(KX_ISystem *system,
                                 blender::bContext *C,
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
      m_pauseAnimationTime(0.0f),
      m_clockTime(0.0f),
      m_previousAnimTime(0.0f),
      m_previousPauseAnimTime(0.0f),
      m_timescale(1.0f),
      m_gamePaused(false),
      m_physicsPaused(false),
      m_fixedStepInProgress(false),
      m_frameUpdateInProgress(false),
      m_hasDeferredGamePaused(false),
      m_deferredGamePaused(false),
      m_hasDeferredPhysicsPaused(false),
      m_deferredPhysicsPaused(false),
      m_average_framerate(0.0),
      m_forceInterpolationRender(false),
      m_cameraOverrideActive(false),
      m_previousRealTime(0.0f),
      m_previous_deltaTime(0.0f),
      m_firstEngineFrame(true),
      m_recordFrameTimeGraphSlot(-1),
      m_showRecordedFrameTimeGraphSlots(0),
      m_frameTimeGraphWindowSeconds(frame_time_graph_default_window_seconds),
      m_frameTimeGraphAxis(GAME_FRAME_TIME_GRAPH_AXIS_FRAMES),
      m_frameTimeGraphStyle(GAME_FRAME_TIME_GRAPH_STYLE_LINE),
      m_frameTimeGraphVisibleDomains(GAME_FRAME_TIME_GRAPH_DOMAIN_FRAME),
      m_frameTimeGraphMaxSamples(frame_time_graph_default_max_samples),
      m_frameTimeGraphTimelineFrame(0),
      m_frameTimeGraphTimelineSeconds(0.0),
      m_frameTimeGraphSamplePending(false),
      m_frameTimeGraphWorkTimerRunning(false),
      m_frameTimeGraphPendingRawDelta(0.0),
      m_frameTimeGraphPendingWorkSeconds(0.0),
      m_frameTimeGraphWorkStartTime(0.0),
      m_anim_framerate(25.0),
      m_useFixedPhysicsTimestep(false),
      m_enablePhysicsInterpolation(false),
      m_currentInterpolationFraction(0.0),
      m_ticrate(60.0),
      m_maxLogicFrame(5),
      m_fixedPhysicsTimestep(1.0 / 60.0),
      m_physicsTickRate(60),
      m_maxPhysicsSteps(5),
      m_useFixedFPSCap(false),
      m_fixedRenderCapRate(60),
      m_vsyncPacing(true),
      m_jitterFix(0.5),
      m_pendingUpdateRealDelta(0.0),
      m_pendingUpdateScaledDelta(0.0),
      m_lastFramePassClock(0.0),
      m_suggestedIdleSeconds(0.0),
      m_interpolationInitialized(false),
      m_doRender(true),
      m_exitkey(130),
      m_exitcode(KX_ExitRequest::NO_REQUEST),
      m_exitstring(""),
      m_cameraZoom(1.0f),
      m_overrideCamZoom(1.0f),
      m_logger(KX_TimeCategoryLogger(m_clock, 25)),
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
blender::bContext *KX_KetsjiEngine::GetContext()
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

/* include blender::Depsgraph update time in tc_depsgraph category
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
  m_clockTime = m_clock.GetTimeSecond();
  m_previousRealTime = m_clockTime;
  ConfigureFrameScheduler();
  m_frameScheduler.ResetTick();
  ResetFixedPhysicsTiming();

  // Pause state is runtime-only. A restarted player session must start simulation normally even
  // if the preceding session stopped while a pause command was active.
  m_gamePaused = false;
  m_physicsPaused = false;
  m_fixedStepInProgress = false;
  m_frameUpdateInProgress = false;
  m_hasDeferredGamePaused = false;
  m_hasDeferredPhysicsPaused = false;
  m_rasterizer->GetDebugDraw().Reset();

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

  m_rasterizer->FlushDebugDraw(m_canvas, RAS_DebugDraw::FixedSnapshotMode::All, nullptr);

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
  if (m_frameTimeGraphSamplePending) {
    PauseFrameTimeGraphWorkTimer();
  }
  m_canvas->FlushScreenshots();
  if (m_frameTimeGraphSamplePending) {
    ResumeFrameTimeGraphWorkTimer();
  }

  // swap backbuffer (drawing into this buffer) <-> front/visible buffer
  m_logger.StartLog(tc_latency);
  if (m_frameTimeGraphSamplePending) {
    PauseFrameTimeGraphWorkTimer();
  }
  m_canvas->SwapBuffers();
  if (m_frameTimeGraphSamplePending) {
    ResumeFrameTimeGraphWorkTimer();
  }
  m_logger.StartLog(tc_rasterizer);

  m_canvas->EndDraw();
  if (m_frameTimeGraphSamplePending) {
    FinishFrameTimeGraphSample();
  }
}

void KX_KetsjiEngine::EndFrameViewportRender()
{
  // Show profiling info
  m_logger.StartLog(tc_overhead);
  if (m_flags & (SHOW_PROFILE | SHOW_FRAMERATE | SHOW_DEBUG_PROPERTIES)) {
    RenderDebugProperties();
  }

  /* Viewport render draws BGE debug geometry during wm_draw_update. Keep end-frame debug text in
   * RAS_DebugDraw; it is flushed before the next viewport draw. */

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
  if (m_frameTimeGraphSamplePending) {
    PauseFrameTimeGraphWorkTimer();
  }
  m_canvas->FlushScreenshots();
  if (m_frameTimeGraphSamplePending) {
    ResumeFrameTimeGraphWorkTimer();
  }

  // swap backbuffer (drawing into this buffer) <-> front/visible buffer
  m_logger.StartLog(tc_latency);
  // m_canvas->SwapBuffers();
  m_logger.StartLog(tc_rasterizer);

  m_canvas->EndDraw();
  if (m_frameTimeGraphSamplePending) {
    FinishFrameTimeGraphSample();
  }
}

/******************************************************************************
 * PHYSICS TIMESTEP MODE ARCHITECTURE
 *
 * UPBGE supports two physics timestep modes, each with distinct characteristics:
 *
 * 1. FIXED PHYSICS MODE (m_useFixedPhysicsTimestep = true)
 *    - Uses accumulator pattern ("Fix Your Timestep" by Glenn Fiedler)
 *    - Physics runs at constant rate regardless of framerate
 *    - Provides deterministic physics simulation
 *    - State stored in: FixedPhysicsState struct
 *    - Key variables: accumulator, fixedTimestep, tickRate
 *    - Optional FPS cap for rendering
 *
 * 2. VARIABLE PHYSICS MODE (m_useFixedPhysicsTimestep = false)
 *    - Physics coupled to framerate (original BGE behavior)
 *    - Physics timestep varies with frame duration
 *    - Simpler but less stable at varying framerates
 *    - State stored in: VariablePhysicsState struct
 *    - No accumulator needed
 *
 * Mode-specific state is encapsulated in separate structs to prevent
 * cross-contamination and clearly document ownership of variables.
 *
 * Only one mode's state struct is allocated at any time based on
 * m_useFixedPhysicsTimestep flag.
 ******************************************************************************/

/********** MAIN FRAME TIMING DISPATCHER **********/

double KX_KetsjiEngine::GetSchedulingTimeScale() const
{
  return m_gamePaused ? 0.0 : m_timescale;
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
  const double raw_dt = dt;

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

  /* Variable mode only: fixed mode plans its passes with m_frameScheduler (NextFrame). */
  const FrameTimes times = GetFrameTimesVariable(dt);

  if (IsFrameTimeGraphSamplingEnabled() && (times.frames > 0 || times.physicsFrames > 0)) {
    BeginFrameTimeGraphSample(raw_dt);
  }

  // Game pause consumes wall-clock time just like a fixed-step pause. Otherwise resuming after
  // a long pause would feed the entire pause duration back into variable-rate simulation.
  if (times.frames > 0 || times.physicsFrames > 0 || m_gamePaused) {
    m_previousRealTime = m_clockTime;
  }

  return times;
}

/********** FIXED PHYSICS MODE IMPLEMENTATION **********/

KX_KetsjiEngine::FrameTimes KX_KetsjiEngine::FrameTimesOfPass(
    const KX_FrameScheduler::Pass &pass) const
{
  /* Game speed = time scale, independent of the frame rate and the step rate: the scheduler
   * admits clock time times the time scale and runs whole fixed steps of game time. A frame pass
   * advances the game to its frame time (see KX_FrameScheduler). */
  const double schedulingTimeScale = GetSchedulingTimeScale();
  FrameTimes times = {};
  times.useFixedPhysicsTimestep = true;
  times.physicsFrames = pass.steps;
  times.physicsTimestep = m_fixedPhysicsTimestep;
  times.frames = pass.steps;
  times.timestep = m_fixedPhysicsTimestep;
  times.framestep = m_fixedPhysicsTimestep;
  times.realDelta = pass.real_delta;
  times.scaledDelta = pass.scaled_delta;
  times.schedulingTimeScale = schedulingTimeScale;
  times.physicsUnscaledTimestep = schedulingTimeScale > 0.0 ?
                                      m_fixedPhysicsTimestep / schedulingTimeScale :
                                      0.0;
  times.interpolationFraction = pass.interpolation;
  return times;
}

void KX_KetsjiEngine::ConfigureFrameScheduler()
{
  KX_FrameScheduler::Settings settings;
  settings.step = m_fixedPhysicsTimestep;
  settings.max_steps = m_maxPhysicsSteps;
  settings.limit = m_useFixedFPSCap && m_fixedRenderCapRate > 0;
  settings.limit_rate = double(std::max(1, m_fixedRenderCapRate));
  settings.vsync = m_vsyncPacing;
  settings.jitter_fix = m_jitterFix;
  m_frameScheduler.Configure(settings);
}

/********** VARIABLE PHYSICS MODE IMPLEMENTATION **********/

KX_KetsjiEngine::FrameTimes KX_KetsjiEngine::GetFrameTimesVariable(double dt)
{
  /* Variable timestep mode - physics coupled to framerate (original BGE behavior).
   * Preserved exactly for backward compatibility.
   *
   * PHYSICS TIMING: Coupled to logic timing (no independent physics steps).
   * LOGIC TIMING: Delegated to CalculateLogicFrameTiming() - same as fixed mode.
   */

  const double realDelta = std::max(0.0, dt);
  const double schedulingTimeScale = GetSchedulingTimeScale();

  /* A dedicated game pause is stronger than setting a variable-mode time scale to zero:
   * it prevents both logic and physics from running, while the outer render/input phase remains
   * available to resume. Consume wall-clock time in GetFrameTimes() so there is no backlog. */
  if (m_gamePaused) {
    FrameTimes times = {};
    times.realDelta = realDelta;
    times.schedulingTimeScale = schedulingTimeScale;
    times.useFixedPhysicsTimestep = false;
    m_currentInterpolationFraction = 0.0;
    return times;
  }

  // ═══════════════════════════════════════════════════════════════════════════
  // LOGIC TIMING CALCULATION (Shared - Independent of Physics Mode)
  // ═══════════════════════════════════════════════════════════════════════════

  // Variable mode uses existing BGE variables (ticrate, maxlogicstep)
  // These are accessed via existing GetTicRate() and GetMaxLogicFrame() methods
  // NOTE: Keep variables separate - don't query via polymorphism to avoid coupling
  auto logicTiming = CalculateLogicFrameTiming(dt, GetTicRate(), GetMaxLogicFrame());

  // ═══════════════════════════════════════════════════════════════════════════
  // PHYSICS TIMING (Variable Mode - Coupled to Logic)
  // ═══════════════════════════════════════════════════════════════════════════

  // In variable mode, physics is tightly coupled to logic:
  // - Physics frames = Logic frames (no independent physics steps)
  // - Physics timestep = Logic timestep (varies with framerate)
  int physicsFrames = logicTiming.frames;
  double physicsTimestep = logicTiming.timestep;

  // ═══════════════════════════════════════════════════════════════════════════
  // ASSEMBLE FRAME TIMING RESULTS
  // ═══════════════════════════════════════════════════════════════════════════

  FrameTimes times = {};
  times.frames = logicTiming.frames;
  times.timestep = logicTiming.timestep;
  times.framestep = logicTiming.timestep * schedulingTimeScale;
  times.realDelta = realDelta;
  times.scaledDelta = std::max(0.0, times.framestep * double(times.frames));
  times.schedulingTimeScale = schedulingTimeScale;
  times.useFixedPhysicsTimestep = false;
  times.physicsFrames = physicsFrames;
  times.physicsTimestep = physicsTimestep;
  times.physicsUnscaledTimestep = physicsTimestep;
  times.interpolationFraction = 0.0;
  m_currentInterpolationFraction = 0.0;

  return times;
}

void KX_KetsjiEngine::ExecutePhysicsFixed(KX_Scene *scene,
                                          const FrameTimes &times,
                                          const int logicFrameIndex,
                                          const bool physicsStepEnabled)
{
  if (!physicsStepEnabled || logicFrameIndex >= times.physicsFrames) {
    return;
  }

  const double physicsDt = times.physicsTimestep;
  scene->GetPhysicsEnvironment()->ProceedDeltaTime(m_frameTime, physicsDt, physicsDt);

  if (logicFrameIndex == times.physicsFrames - 1) {
    scene->GetPhysicsEnvironment()->UpdateSoftBodies();
  }
}

void KX_KetsjiEngine::ExecutePhysicsVariable(KX_Scene *scene,
                                             const FrameTimes &times,
                                             int logicFrameIndex)
{
  if (m_gamePaused || m_physicsPaused || logicFrameIndex >= times.physicsFrames) {
    return;
  }

  // Variable timestep mode: physics coupled to framerate (original BGE behavior)
  scene->GetPhysicsEnvironment()->ProceedDeltaTime(m_frameTime, times.timestep, times.framestep);

  // No need to call softbody update more than 1 time
  if (logicFrameIndex == times.frames - 1) {
    scene->GetPhysicsEnvironment()->UpdateSoftBodies();
  }
}

/********** SHARED FRAME HELPERS **********/

void KX_KetsjiEngine::ProcessInputDevices()
{
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
#ifdef WITH_GAMEENGINE_LOGICNODES
  /* Gamepads pressing a button join their player (On Player Join), for every scene. */
  LN_InputMap::Get().UpdatePlayers();
#endif
  KeepMouseInPlayerView();
}

void KX_KetsjiEngine::CursorShown(const bool shown)
{
  /* Elsewhere the game moves the cursor whether it is shown or not. */
  if (!m_cursorMovesOnlyHidden || m_cursorMovable == !shown) {
    return;
  }
  /* As the launcher grabs it at start (Show Mouse): hidden, the pointer is locked and the game
   * moves its position; shown, it is free inside the window and stays where it is. With --debug
   * the window manager grabs nothing, so the game never moves it. */
  WM_cursor_grab_enable(CTX_wm_window(m_context), WM_CURSOR_WRAP_NONE, nullptr, !shown);
  m_cursorMovable = !shown && (G.debug & G_DEBUG) == 0;
}

void KX_KetsjiEngine::KeepMouseInPlayerView()
{
  /* Free while paused (a pause menu over the whole window) and while another window has the
   * focus. */
  DEV_InputDevice *input_device = dynamic_cast<DEV_InputDevice *>(m_inputDevice);
  if (input_device == nullptr || m_canvas == nullptr || m_rasterizer == nullptr ||
      !m_cursorMovable || IsGamePaused() || !input_device->IsWindowFocused())
  {
    return;
  }
  const SCA_InputEvent &x_event = m_inputDevice->GetInput(SCA_IInputDevice::MOUSEX);
  const SCA_InputEvent &y_event = m_inputDevice->GetInput(SCA_IInputDevice::MOUSEY);
  if (x_event.m_values.empty() || y_event.m_values.empty()) {
    return;
  }
  /* Pointer pixels are from the top left, views from the bottom left (as LN_CameraUnderPointer
   * compares them). Only inside the game's canvas: the mouse may still leave the window (or, in
   * embedded play, go to Blender's editors). */
  const int x = x_event.m_values.back();
  const int y = y_event.m_values.back();
  const int height = m_canvas->GetHeight();
  if (x < 0 || y < 0 || x >= m_canvas->GetWidth() || y >= height) {
    return;
  }
  /* The window as it is now: after a resize, the views are placed again only at the next draw. */
  const RAS_Rect window = m_rasterizer->GetRenderArea(m_canvas, RAS_Rasterizer::RAS_STEREO_LEFTEYE);
  /* One mouse: the first scene split between players decides. */
  for (KX_Scene *scene : m_scenes) {
    const KX_PlayerViews &views = scene->GetPlayerViews();
    RAS_Rect view;
    if (!views.KeepsMouse() || !views.GetMouseView(scene->GetActiveCamera(), window, view)) {
      continue;
    }
    const int kept_x = std::clamp(x, view.GetLeft(), view.GetRight());
    const int kept_y = std::clamp(y, height - view.GetTop(), height - view.GetBottom());
    if (kept_x != x || kept_y != y) {
      m_canvas->SetMousePosition(kept_x, kept_y);
      input_device->ConvertMoveEvent(kept_x, kept_y);
    }
    return;
  }
}

void KX_KetsjiEngine::ProcessSceneUpdate(KX_Scene *scene, const FrameTimes &times)
{
  m_logger.StartLog(tc_logic);
  KX_SetActiveScene(scene);

#ifdef WITH_GAMEENGINE_LOGICNODES
  /* Complete asynchronous services and run true per-render-frame logic before fixed simulation.
   * This phase deliberately remains alive when scaled time and fixed-step scheduling stop. */
  scene->LogicNodeBeginFrame();
#endif

  scene->UpdateObjectActivity();

#ifdef WITH_GAMEENGINE_LOGICNODES
  const bool trackInterpolationUpdates = times.useFixedPhysicsTimestep &&
                                         SceneUsesSmoothMotion(scene);
  if (trackInterpolationUpdates) {
    scene->BeginPhysicsInterpolationFrameUpdate();
  }

  scene->LogicNodeUpdateFrame(m_frameTime,
                              times.scaledDelta,
                              times.realDelta,
                              times.useFixedPhysicsTimestep,
                              m_gamePaused);

  m_logger.StartLog(tc_scenegraph);
  scene->UpdateParents(m_frameTime);

  if (trackInterpolationUpdates) {
    scene->EndPhysicsInterpolationFrameUpdate();
  }
#endif
}

void KX_KetsjiEngine::ProcessSceneLogic(KX_Scene *scene, const FrameTimes &times, int frameIndex)
{
  /* Suspension holds the physics and logic processing for an
   * entire scene. Objects can be suspended individually, and
   * the settings for that precede the logic and physics
   * update. */
  m_logger.StartLog(tc_logic);
  (void)frameIndex;

  m_logger.StartLog(tc_physics);

  // set Python hooks for each scene (also needed for non-zero subframes)
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
  scene->LogicUpdateFrame(m_frameTime,
                          times.useFixedPhysicsTimestep,
                          times.physicsTimestep,
                          times.physicsUnscaledTimestep);

  scene->LogicEndFrame();

  // Actuators can affect the scenegraph
  m_logger.StartLog(tc_scenegraph);
  scene->UpdateParents(m_frameTime);

  m_logger.StartLog(tc_physics);
}

void KX_KetsjiEngine::FinalizeFrame(const bool clearInputs, const bool clearNetwork)
{
  m_logger.StartLog(tc_network);
  if (clearNetwork) {
    m_networkMessageManager->ClearMessages();
  }

  // update system devices
  m_logger.StartLog(tc_logic);
  if (clearInputs) {
    m_inputDevice->ClearInputs();
  }

  // scene management
  ProcessScheduledScenes();
}

void KX_KetsjiEngine::ResetFixedPhysicsTiming()
{
  InvalidatePhysicsInterpolationState();
  m_currentInterpolationFraction = 0.0;

  if (!m_useFixedPhysicsTimestep) {
    return;
  }

  /* No leftover time; the next pass draws a frame and the steps line up with it. */
  m_frameScheduler.Reset(m_clockTime);
  m_interpolationInitialized = false;
  m_pendingUpdateRealDelta = 0.0;
  m_pendingUpdateScaledDelta = 0.0;
  m_lastFramePassClock = m_clockTime;
  m_suggestedIdleSeconds = 0.0;
  m_previousRealTime = m_clockTime;
}

void KX_KetsjiEngine::InvalidatePhysicsInterpolationState()
{
  m_interpolationInitialized = false;
  for (KX_Scene *scene : m_scenes) {
    scene->ClearPhysicsInterpolationState();
  }
}

/********** LOGIC TIMING CALCULATION (SHARED BY BOTH PHYSICS MODES) **********/

KX_KetsjiEngine::LogicFrameTiming KX_KetsjiEngine::CalculateLogicFrameTiming(double dt,
                                                                             double logicRate,
                                                                             int maxLogicFrames)
{
  /* Logic frame timing calculation - independent of physics mode.
   *
   * This function handles the FIXED_FRAMERATE flag, which controls whether
   * logic updates run at a fixed rate or variable rate (once per frame).
   *
   * IMPORTANT: This is a separate concern from physics timestep mode.
   * - FIXED_FRAMERATE = flag controlling logic update frequency
   * - Fixed Physics Mode = using accumulator pattern for physics
   *
   * These are orthogonal concepts:
   * - You can have Fixed Physics + Variable Logic
   * - You can have Variable Physics + Fixed Logic
   * - etc.
   */

  LogicFrameTiming result;

  // Check if logic updates should run at fixed rate or variable rate
  if (m_flags & FIXED_FRAMERATE) {
    // Fixed logic rate: Decouple logic from framerate
    // Run multiple logic updates per frame if needed to maintain target rate
    result.timestep = 1.0 / logicRate;
    result.frames = int(dt * logicRate);
  }
  else {
    // Variable logic rate: One update per render frame
    // Logic timestep varies with frame duration
    result.timestep = dt;
    result.frames = 1;
  }

  // Limit logic frames to prevent falling too far behind
  // If we've exceeded the max, increase timestep to "skip" some updates
  if (result.frames > maxLogicFrames) {
    result.timestep = dt / maxLogicFrames;
    result.frames = maxLogicFrames;
  }

  return result;
}

/********** FIXED PHYSICS MODE: COMPLETE FRAME EXECUTION **********/

bool KX_KetsjiEngine::RunFixedTick(const FrameTimes &times, const int stepIndex)
{
  /* A fixed step is atomic across every active scene. Pause commands issued by fixed logic take
   * effect after the current step, never halfway through it. */
  const bool physicsStepEnabled = !m_physicsPaused;
  m_fixedStepInProgress = true;
  m_rasterizer->GetDebugDraw().BeginFixedStepSnapshot();

  /* Snapshot immediately before every fixed step, not merely before the first one. On a
   * catch-up frame this leaves the interpolation interval at penultimate -> final pose. */
  if (!m_gamePaused && !m_physicsPaused) {
    for (KX_Scene *scene : m_scenes) {
      if (SceneUsesSmoothMotion(scene)) {
        scene->StorePhysicsInterpolationState(m_enablePhysicsInterpolation);
      }
    }
    m_interpolationInitialized = true;
  }

  m_frameTime += times.framestep;

  // for each scene, call the proceed functions
  for (KX_Scene *scene : m_scenes) {
    m_rasterizer->GetDebugDraw().SetFixedStepSnapshotContext(scene);
    ProcessSceneLogic(scene, times, stepIndex);

    // Perform physics calculations (fixed mode)
    ExecutePhysicsFixed(scene, times, stepIndex, physicsStepEnabled);

    m_logger.StartLog(tc_scenegraph);
    scene->UpdateParents(m_frameTime);

    m_logger.StartLog(tc_services);
  }

  m_rasterizer->GetDebugDraw().PublishFixedStepSnapshot();
  m_fixedStepInProgress = false;
  ApplyDeferredPauseState();

  /* Update soft bodies when a command makes this the final physics step earlier than the
   * scheduler predicted. ExecutePhysicsFixed handles the normal final scheduled step. */
  if (physicsStepEnabled && stepIndex < times.physicsFrames - 1 &&
      (m_gamePaused || m_physicsPaused))
  {
    for (KX_Scene *scene : m_scenes) {
      if (PHY_IPhysicsEnvironment *physicsEnvironment = scene->GetPhysicsEnvironment()) {
        physicsEnvironment->UpdateSoftBodies();
      }
    }
  }

  /* Preserve edge-triggered device state through the first fixed step, then age it exactly
   * once for this scheduler pass. Logic Nodes retain independent accumulated snapshots for
   * frame-update and fixed consumers. */
  FinalizeFrame(stepIndex == 0);
  return !m_gamePaused;
}

bool KX_KetsjiEngine::NextFrameFixed(const FrameTimes &times, const bool render_update_due)
{
  BLI_assert(m_useFixedPhysicsTimestep);
  m_pendingUpdateRealDelta += std::max(0.0, times.realDelta);
  m_pendingUpdateScaledDelta += std::max(0.0, times.scaledDelta);

  /********** RESTORE AUTHORITATIVE SIMULATION STATE **********/
  /* Rendering temporarily writes interpolated world poses into SG nodes. Restore the exact
   * simulation poses before any activity query or per-render-frame logic can observe or mutate
   * them. Pause does not bypass this: render-frame logic remains active while simulation stops.
   * Only the objects drawn moved are restored, so this costs nothing without Smooth Motion. */
  for (KX_Scene *scene : m_scenes) {
    scene->ClearPhysicsInterpolationState();
  }

  /********** REAL-TIME UPDATE PHASE **********/
  ProcessInputDevices();
  /* Game time since the last drawn frame (Smooth Motion's blend fades). */
  double frameSeconds = 0.0;
  if (render_update_due) {
    FrameTimes update_times = times;
    update_times.realDelta = m_pendingUpdateRealDelta;
    update_times.scaledDelta = m_pendingUpdateScaledDelta;
    frameSeconds = m_pendingUpdateScaledDelta;
    if (m_gamePaused) {
      m_pauseAnimationTime += std::max(0.0, update_times.realDelta);
    }
    /* The graph shows when frames really came: the clock between frame passes, not the planned
     * frame times (those are even by design). */
    const double measuredFrameSeconds = m_clockTime - m_lastFramePassClock;
    m_lastFramePassClock = m_clockTime;
    if (IsFrameTimeGraphSamplingEnabled()) {
      BeginFrameTimeGraphSample(measuredFrameSeconds);
    }
    /* A render-frame update is atomic across active scenes, just like a fixed step. A pause
     * command may affect the fixed loop below, but sibling scenes must all observe the same
     * pre-update pause state. */
    m_frameUpdateInProgress = true;
    for (KX_Scene *scene : m_scenes) {
      ProcessSceneUpdate(scene, update_times);
    }
    m_frameUpdateInProgress = false;
    m_frameUpdateRan = true;
    ApplyDeferredPauseState();
    m_pendingUpdateRealDelta = 0.0;
    m_pendingUpdateScaledDelta = 0.0;
  }
  else {
#ifdef WITH_GAMEENGINE_LOGICNODES
    /* Input must be sampled before every possible fixed step even when no frame update/render is
     * due. Each Logic Nodes manager retains separate frame and fixed transient queues. */
    for (KX_Scene *scene : m_scenes) {
      scene->LogicNodeCaptureInput();
    }
#endif
  }

  /********** INTERPOLATION STATE MANAGEMENT **********/
  const bool useInterpolation = GetUseFixedPhysicsTimestep() && !m_gamePaused && !m_physicsPaused;
  if (useInterpolation && !m_interpolationInitialized && times.frames == 0) {
    /* Initialize both endpoints on a sub-timestep frame. This also covers enabling interpolation
     * at runtime without forcing an artificial physics step. */
    for (KX_Scene *scene : m_scenes) {
      if (SceneUsesSmoothMotion(scene)) {
        scene->StorePhysicsInterpolationState(m_enablePhysicsInterpolation);
      }
    }
    m_interpolationInitialized = true;
  }

  /********** LOGIC AND PHYSICS LOOP **********/
  /* Frame-update pause commands prevent entry into this loop entirely; one asked for during a
   * step stops the rest of the pass after that step. */
  int executedFrames = 0;
  for (int i = 0; i < times.frames && !m_gamePaused; ++i) {
    executedFrames++;
    if (!RunFixedTick(times, i)) {
      break;
    }
  }
  m_frameScheduler.EndPass(executedFrames);
  if (render_update_due && IsFrameTimeGraphSamplingEnabled()) {
    /* The steps since the frame before, also those run by passes that drew nothing. */
    m_frameStepCounts.AddFrame(m_frameScheduler.GetLastFrameTime(),
                               m_frameScheduler.GetLastFrameSteps());
  }

  if (executedFrames == 0) {
    /* Retain legacy input/network transients across scheduler passes that contain no fixed logic
     * step. Paused/zero-scale simulation deliberately discards them so a later resume cannot
     * replay an arbitrarily old backlog. Scene services still run on every scheduler pass. */
    const bool discard_without_fixed = m_gamePaused || times.schedulingTimeScale <= 0.0;
    FinalizeFrame(discard_without_fixed, discard_without_fixed);
  }

  const bool forceInterpolationFrame = ShouldForceInterpolationRender();

  if (forceInterpolationFrame) {
    ClearForcedInterpolation();
  }

  /********** APPLY INTERPOLATION AFTER PHYSICS LOOP **********/
  /* Pause and interpolation commands can change state during either update phase, so validate the
   * live state here rather than relying on the pre-loop snapshot. A fraction of 0 is drawn too:
   * Interpolate shows the step before the last one there, one step late like every other frame
   * (skipping it showed the last step, a step ahead, whenever 0 came between other fractions). */
  const bool applyInterpolation = render_update_due && GetUseFixedPhysicsTimestep() &&
                                  !m_gamePaused && !m_physicsPaused &&
                                  m_interpolationInitialized &&
                                  std::isfinite(m_currentInterpolationFraction);
  if (applyInterpolation) {
    for (KX_Scene *scene : m_scenes) {
      if (SceneUsesSmoothMotion(scene)) {
        scene->ApplyPhysicsInterpolation(
            m_currentInterpolationFraction, m_enablePhysicsInterpolation, frameSeconds);
      }
    }
  }

  // Start logging time spent outside main loop
  m_logger.StartLog(tc_outside);

  const bool render = render_update_due && m_doRender;
  /* After a pass that draws nothing, the launcher waits until the next step or frame is due. An
   * external clock decides itself when time moves on, so nothing waits for it. */
  m_suggestedIdleSeconds = 0.0;
  if (!render && !(m_flags & USE_EXTERNAL_CLOCK)) {
    const double now = m_clock.GetTimeSecond();
    m_suggestedIdleSeconds = std::max(0.0, m_frameScheduler.NextWorkTime(now) - now);
  }
  return render;
}

/********** VARIABLE PHYSICS MODE: COMPLETE FRAME EXECUTION **********/

bool KX_KetsjiEngine::NextFrameVariable(const FrameTimes &times)
{
  BLI_assert(!m_useFixedPhysicsTimestep);
  m_suggestedIdleSeconds = 0.0;

  // Variable mode has no FPS cap - runs uncapped

  /* Keep scene activity and the render-frame Logic Nodes phase independent from the number of
   * simulation substeps in this mode too. Logic Nodes currently reject variable physics mode,
   * but ProcessSceneUpdate still owns the once-per-frame activity update. */
  ProcessInputDevices();
  m_frameUpdateInProgress = true;
  for (KX_Scene *scene : m_scenes) {
    ProcessSceneUpdate(scene, times);
  }
  m_frameUpdateInProgress = false;
  m_frameUpdateRan = true;
  ApplyDeferredPauseState();

  /********** LOGIC AND PHYSICS LOOP **********/
  int executedFrames = 0;
  for (int i = 0; i < times.frames && !m_gamePaused; ++i) {
    m_frameTime += times.framestep;

    // for each scene, call the proceed functions
    for (KX_Scene *scene : m_scenes) {
      if (m_gamePaused) {
        break;
      }
      ProcessSceneLogic(scene, times, i);

      // Perform physics calculations (variable mode)
      ExecutePhysicsVariable(scene, times, i);

      m_logger.StartLog(tc_scenegraph);
      scene->UpdateParents(m_frameTime);

      m_logger.StartLog(tc_services);
    }

    FinalizeFrame(i == 0);
    executedFrames++;
  }

  if (executedFrames == 0) {
    // Dedicated game pause still ages input and processes scene services once per render frame.
    FinalizeFrame();
  }
  if (IsFrameTimeGraphSamplingEnabled()) {
    m_frameStepCounts.AddFrame(m_clockTime, executedFrames);
  }

  // Start logging time spent outside main loop
  m_logger.StartLog(tc_outside);

  return m_doRender;
}

/********** MAIN FRAME DISPATCHER **********/

bool KX_KetsjiEngine::NextFrame()
{
  m_logger.StartLog(tc_services);
  m_frameUpdateRan = false;

  if (m_useFixedPhysicsTimestep) {
    /* One clock sample decides the whole pass: whether a frame is drawn, its frame time and the
     * steps due by it (KX_FrameScheduler). An external clock is set by the game instead. */
    if (!(m_flags & USE_EXTERNAL_CLOCK)) {
      m_clockTime = m_clock.GetTimeSecond();
    }
    const KX_FrameScheduler::Pass pass = m_frameScheduler.BeginPass(m_clockTime,
                                                                     GetSchedulingTimeScale());
    const FrameTimes times = FrameTimesOfPass(pass);
    m_currentInterpolationFraction = pass.interpolation;
    return NextFrameFixed(times, pass.frame);
  }

  // Calculate frame timing for current mode
  const FrameTimes times = GetFrameTimes();

  // Variable mode normally skips a render when no simulation frame is scheduled. A dedicated
  // game pause is different: it must keep the real-time input/UI phase alive so it can resume.
  if (times.frames == 0 && !m_gamePaused) {
    m_logger.StartLog(tc_outside);
    return false;
  }
  return NextFrameVariable(times);
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

  m_renderFrame++;
  BeginFrame();

  RAS_FrameBuffer *background_fb = m_rasterizer->GetFrameBuffer(
      RAS_Rasterizer::RAS_FRAMEBUFFER_EYE_RIGHT0);
  const int width = m_canvas->GetWidth();
  const int height = m_canvas->GetHeight();
  background_fb->UpdateSize(width + 1, height + 1);

  std::vector<FrameRenderData> frameDataList;
  /* Ensure animations (including camera IPOs that modify lens/clip)
   * are up-to-date before computing camera projection matrices.
   * This avoids a race where GetFrameRenderData computes projection
   * using stale camera data and later animation updates change the
   * lens, causing flickering depending on actuator execution order. */
  m_logger.StartLog(tc_animations);
  for (KX_Scene *scene : m_scenes) {
    UpdateAnimations(scene);
  }
  m_logger.StartLog(tc_rasterizer);

  /* Split screen and custom viewports follow the window size: place them for this frame. */
  const RAS_Rect window = m_rasterizer->GetRenderArea(m_canvas,
                                                      RAS_Rasterizer::RAS_STEREO_LEFTEYE);
  for (KX_Scene *scene : m_scenes) {
    scene->GetPlayerViews().Apply(*scene, window, !UseViewportRender());
  }

  GetFrameRenderData(frameDataList);

  KX_Scene *firstscene = m_scenes->GetFront();
  const RAS_FrameSettings &framesettings = firstscene->GetFramingType();

  // Used to detect when a camera is the first rendered an then doesn't request a depth clear.
  unsigned short pass = 0;

  for (FrameRenderData &frameData : frameDataList) {
    GPU_framebuffer_bind(background_fb->GetFrameBuffer());
    // Use the framing bar color set in the Blender scenes
    GPU_framebuffer_clear_color(
        background_fb->GetFrameBuffer(),
        {framesettings.BarRed(), framesettings.BarGreen(), framesettings.BarBlue(), 1.0});
    GPU_depth_mask(true);
    GPU_framebuffer_clear_depth(background_fb->GetFrameBuffer(), 1.0f);

    // for each scene, call the proceed functions
    for (unsigned short i = 0, size = frameData.m_sceneDataList.size(); i < size; ++i) {
      const SceneRenderData &sceneFrameData = frameData.m_sceneDataList[i];
      KX_Scene *scene = sceneFrameData.m_scene;

      m_rasterizer->SetAuxilaryClientInfo(scene);
      scene->SetLodUpdateDue();

      // Draw the scene once for each camera with an enabled viewport or an active camera.
      // Interpolation has already been applied in NextFrameFixed(), so we just render.
      for (const CameraRenderData &cameraFrameData : sceneFrameData.m_cameraDataList) {
        // do the rendering
        RenderCamera(scene, background_fb, cameraFrameData, pass++);
      }
    }
  }

  // Interpolation clearing now happens at the START of NextFrameFixed() before state storage.
  // This ensures we always capture clean physics positions.

  if (ShouldForceInterpolationRender()) {
    ClearForcedInterpolation();
  }

  /* Give back the renderers of cameras that stopped drawing (a minimap turned off, a closed
   * menu). Each scene's current renderer stays: the screen draw below uses the first one. */
  {
    std::set<blender::GPUViewport *> in_use;
    for (KX_Scene *scene : m_scenes) {
      if (scene->GetCurrentGPUViewport()) {
        in_use.insert(scene->GetCurrentGPUViewport());
      }
    }
    const double now = BLI_time_now_seconds();
    for (KX_Scene *scene : m_scenes) {
      scene->FreeIdleGPUViewports(now, in_use);
    }
  }

  if (!UseViewportRender()) {
    /* Clear the entire screen (draw a black background rect before drawing) */
    blender::ARegion *region = CTX_wm_region(m_context);
    wmWindowViewport(CTX_wm_window(m_context));

    blender::draw::command::StateSet::set();
    GPU_depth_test(GPU_DEPTH_ALWAYS);

    uint pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
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
    int v[4] = {m_canvas->GetViewportArea().GetLeft(),
                m_canvas->GetViewportArea().GetBottom(),
                m_canvas->GetWidth() + 1,
                m_canvas->GetHeight() + 1};

    /* blender::rcti xmin, xmax, ymin, ymax */
    const blender::rcti rect = {v[0], v[2] + v[0] - 1, v[1], v[3] + v[1] - 1};
    blender::GPUViewport *gpu_viewport = m_scenes->GetFront()->GetCurrentGPUViewport();
    blender::gpu::Texture *backup = GPU_viewport_color_texture(gpu_viewport, 0);
    GPU_viewport_switch_color_tex(gpu_viewport,
                                  GPU_framebuffer_color_texture(background_fb->GetFrameBuffer()));
    GPU_viewport_draw_to_screen_ex(gpu_viewport, 0, &rect, true, false);
    GPU_viewport_switch_color_tex(gpu_viewport, backup);

    GPU_viewport(v[0], v[1], v[2], v[3]);
    GPU_scissor_test(true);
    GPU_scissor(v[0], v[1], v[2], v[3]);

    GPU_matrix_ortho_set(0, width, 0, height, -100, 100);
    GPU_matrix_identity_set();

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
  m_cameraOverrideActive = true;
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
           !m_overrideCamData.m_perspective)
  {
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
  const double animation_time = m_gamePaused ? m_pauseAnimationTime : m_frameTime;
  double &previous_animation_time = m_gamePaused ? m_previousPauseAnimTime : m_previousAnimTime;
  // Handle the animations independently of the logic time step
  if (m_flags & RESTRICT_ANIMATION) {
    double anim_timestep = 1.0 / scene->GetAnimationFPS();
    if (animation_time - previous_animation_time > anim_timestep ||
        animation_time == previous_animation_time)
    {
      // Sanity/debug print to make sure we're actually going at the fps we want (should be close
      // to anim_timestep) CM_Debug("Anim fps: " << 1.0/(m_frameTime - m_previousAnimTime));
      previous_animation_time = animation_time;
#ifdef WITH_GAMEENGINE_LOGICNODES
      if (m_gamePaused) {
        scene->UpdatePausedAnimations(m_pauseAnimationTime, m_frameTime);
      }
      else
#endif
      {
        scene->UpdateAnimations(m_frameTime);
      }
    }
  }
  else {
#ifdef WITH_GAMEENGINE_LOGICNODES
    if (m_gamePaused) {
      scene->UpdatePausedAnimations(m_pauseAnimationTime, m_frameTime);
    }
    else
#endif
    {
      scene->UpdateAnimations(m_frameTime);
    }
  }
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

    /* A split-screen view starts from the picture its camera has over the whole window, then
     * keeps part of it (KX_PlayerViews::FitFrustumToView). Other cameras fit their own area. */
    const KX_PlayerViews::FieldOfView field_of_view = scene->GetPlayerViews().GetFieldOfView(cam);
    const bool from_window = field_of_view != KX_PlayerViews::FieldOfView::Fit;
    const bool split_view = scene->GetPlayerViews().IsSplitView(cam, scene->GetActiveCamera());
    RAS_Rect window_area, window_viewport;
    if (from_window || split_view) {
      window_area = m_rasterizer->GetRenderArea(m_canvas, RAS_Rasterizer::RAS_STEREO_LEFTEYE);
      RAS_FramingManager::ComputeViewport(scene->GetFramingType(), window_area, window_viewport);
    }
    const RAS_Rect &frustum_area = from_window ? window_area : area;
    const RAS_Rect &frustum_viewport = from_window ? window_viewport : viewport;

    const auto compute_frustum = [&](const RAS_Rect &frame_area,
                                     const RAS_Rect &frame_viewport,
                                     const bool zoom,
                                     RAS_FrameFrustum &r_frustum) {
      if (orthographic) {
        RAS_FramingManager::ComputeOrtho(scene->GetFramingType(),
                                         frame_area,
                                         frame_viewport,
                                         cam->GetScale(),
                                         nearfrust,
                                         farfrust,
                                         cam->GetSensorFit(),
                                         cam->GetShiftHorizontal(),
                                         cam->GetShiftVertical(),
                                         r_frustum);
      }
      else {
        RAS_FramingManager::ComputeFrustum(scene->GetFramingType(),
                                           frame_area,
                                           frame_viewport,
                                           cam->GetLens(),
                                           cam->GetSensorWidth(),
                                           cam->GetSensorHeight(),
                                           cam->GetSensorFit(),
                                           cam->GetShiftHorizontal(),
                                           cam->GetShiftVertical(),
                                           nearfrust,
                                           farfrust,
                                           r_frustum);
      }
      /* The editor's camera view zoom (embedded play); a split view keeps it too. */
      if (zoom) {
        r_frustum.x1 *= camzoom;
        r_frustum.x2 *= camzoom;
        r_frustum.y1 *= camzoom;
        r_frustum.y2 *= camzoom;
      }
    };
    compute_frustum(frustum_area, frustum_viewport, !cam->GetViewport() || from_window, frustum);

    /* How big objects look in a split view compared with the whole window (Level of Detail by
     * View Size): pixels per frustum unit in the view and over the whole window. */
    RAS_FrameFrustum window_frustum = frustum;
    if (split_view && !from_window) {
      compute_frustum(window_area, window_viewport, true, window_frustum);
    }
    KX_PlayerViews::FitFrustumToView(field_of_view, window_viewport, viewport, frustum);
    float view_size_scale = 1.0f;
    if (split_view && frustum.y2 > frustum.y1 && window_frustum.y2 > window_frustum.y1 &&
        window_viewport.GetHeight() > 0)
    {
      view_size_scale = (float(viewport.GetHeight()) / (frustum.y2 - frustum.y1)) /
                        (float(window_viewport.GetHeight()) /
                         (window_frustum.y2 - window_frustum.y1));
    }
    cam->SetViewSizeScale(view_size_scale);

    if (orthographic) {
      projmat = m_rasterizer->GetOrthoMatrix(
          frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar);
    }
    else {
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

  m_rasterizer->SetEye(RAS_Rasterizer::RAS_STEREO_LEFTEYE /*cameraFrameData.m_eye*/);

  m_logger.StartLog(tc_rasterizer);

#ifdef WITH_PYTHON
  // Run any pre-drawing python callbacks
  scene->RunDrawingCallbacks(KX_Scene::PRE_DRAW, rendercam);
#endif

  bool is_overlay_pass = rendercam == scene->GetOverlayCamera();
  if (is_overlay_pass || (rendercam != scene->GetActiveCamera() && rendercam->GetViewport())) {
    GPU_blend(GPU_BLEND_ALPHA_PREMULT);
  }

  bool is_last_render_pass = rendercam == m_renderingCameras.back();

  /* Viewport Render draws (and flushes debug lines for) its first camera only: lines added in a
   * skipped pass would show one frame late, twice. */
  if (UseViewportRender() && scene->GetPhysicsEnvironment() &&
      (m_renderingCameras.empty() || rendercam == m_renderingCameras.front()))
  {
    scene->GetPhysicsEnvironment()->DebugDrawWorld();
  }

  scene->RenderAfterCameraSetup(
      rendercam, background_fb, viewport, is_overlay_pass, is_last_render_pass);

  if (!UseViewportRender() && scene->GetPhysicsEnvironment()) {
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

  m_rasterizer->FlushDebugDraw(m_canvas, RAS_DebugDraw::FixedSnapshotMode::Context, scene);

  // We need to first make sure our viewport is correct (enabling multiple viewports can mess this
  // up), only for filters.
  const int width = m_canvas->GetWidth();
  const int height = m_canvas->GetHeight();
  GPU_viewport(0, 0, width + 1, height + 1);
  GPU_scissor(0, 0, width + 1, height + 1);

  RAS_FrameBuffer *frameBuffer = scene->Render2DFilters(m_rasterizer, m_canvas, inputfb, targetfb);

#ifdef WITH_PYTHON
  /* We can't deduce what camera should be passed to the python callbacks
   * because the post draw callbacks are per scenes and not per cameras.
   */
  scene->RunDrawingCallbacks(KX_Scene::POST_DRAW, nullptr);

  // Python draw callback can also call debug draw functions, so we have to clear debug shapes.
  m_rasterizer->FlushDebugDraw(m_canvas, RAS_DebugDraw::FixedSnapshotMode::None, nullptr);
#endif

  return frameBuffer;
}

void KX_KetsjiEngine::StopEngine()
{
  m_pendingGameLoad.reset();
  m_rasterizer->GetDebugDraw().Reset();

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

// blender::Scene Management is able to switch between scenes
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
      blender::bContext *C = KX_GetActiveEngine()->GetContext();
      blender::RegionView3D *rv3d = CTX_wm_region_view3d(C);
      MT_Vector3 pos = MT_Vector3(rv3d->viewinv[3]);
      activecam->NodeSetLocalPosition(pos);
      activecam->NodeSetLocalOrientation(MT_Matrix3x3(&rv3d->viewinv[0][0]));
      activecam->NodeUpdateGS(0.0f);
    }

    scene->GetCameraList()->Add(CM_AddRef(activecam));
    scene->SetActiveCamera(activecam);
    scene->GetObjectList()->Add(CM_AddRef(activecam));
    scene->NoteObjectListChanged();
    scene->GetRootParentList()->Add(CM_AddRef(activecam));
    // done with activecam
    activecam->Release();
  }

  scene->UpdateParents(0.0f);
#ifdef WITH_GAMEENGINE_LOGICNODES
  /* A scene can be added or replaced by frame logic while the game is paused. Populate its
   * While Paused scope before the first render so UI actions and animations do not lose a frame.
   */
  if (m_gamePaused) {
    if (LN_Manager *manager = scene->GetLogicNodeManager()) {
      manager->RefreshPauseProcessingObjects();
    }
  }
#endif
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
    case 0:  // Change nothing to default values
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

  float avg_frame_seconds = m_logger.GetAverage();
  if (avg_frame_seconds < 1e-6f) {
    avg_frame_seconds = 1e-6f;
  }

  const float raw_frame_ms = !m_liveFrameTimeGraph.empty() ?
                                 std::max(m_liveFrameTimeGraph.back().raw_frame_ms, 0.001f) :
                                 (avg_frame_seconds * 1000.0f);
  const float raw_frame_seconds = std::max(raw_frame_ms * 0.001f, 1e-6f);

  static const MT_Vector4 white(1.0f, 1.0f, 1.0f, 1.0f);

  // Use nullptrfor no scene.
  RAS_DebugDraw &debugDraw = m_rasterizer->GetDebugDraw();

  if (m_flags & (SHOW_FRAMERATE | SHOW_PROFILE)) {
    RenderFrameTimeGraph(debugDraw, xcoord, ycoord, profile_indent, profile_size);
  }

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

    debugtxt = fmt::format("{:>5.2f}ms ({:.1f}fps)", raw_frame_ms, (1.0f / raw_frame_seconds));
    debugDraw.RenderText2D(
        debugtxt, MT_Vector2(xcoord + const_xindent + profile_indent, ycoord), white);
    // Increase the indent by default increase
    ycoord += const_ysize;

    debugDraw.RenderText2D("Avg frame:", MT_Vector2(xcoord + const_xindent, ycoord), white);

    debugtxt = fmt::format(
        "{:>5.2f}ms ({:.1f}fps)", (avg_frame_seconds * 1000.0f), (1.0f / avg_frame_seconds));
    debugDraw.RenderText2D(
        debugtxt, MT_Vector2(xcoord + const_xindent + profile_indent, ycoord), white);
    // Increase the indent by default increase
    ycoord += const_ysize;

    /* How many frames of the last second showed 0, 1, 2 or more steps: even frame times can
     * hide uneven steps (stutter), which only this shows. Then what paces the frames, which says
     * whether an alternating pattern is natural (60 steps on a 100 Hz screen: 0:40 1:60). */
    debugDraw.RenderText2D("Steps/frame:", MT_Vector2(xcoord + const_xindent, ycoord), white);
    /* The space keeps the value off the label, which is as wide as the column (as GPU Latency's
     * numbers do with their padding). */
    debugDraw.RenderText2D(" " + m_frameStepCounts.Text(),
                           MT_Vector2(xcoord + const_xindent + profile_indent, ycoord),
                           white);
    ycoord += const_ysize;

    const FramePacingInfo pacing = GetFramePacingInfo();
    debugDraw.RenderText2D("Pacing:", MT_Vector2(xcoord + const_xindent, ycoord), white);
    debugDraw.RenderText2D(
        KX_FramePacingText(pacing.pacing, pacing.frame_limit, pacing.refresh_rate),
        MT_Vector2(xcoord + const_xindent + profile_indent, ycoord),
        white);
    ycoord += const_ysize;
  }

  // Profile display
  if (m_flags & SHOW_PROFILE) {
    for (int j = tc_first; j < tc_numCategories; j++) {
      debugDraw.RenderText2D(
          m_profileLabels[j], MT_Vector2(xcoord + const_xindent, ycoord), white);

      double time = m_logger.GetAverage((KX_TimeCategory)j);

      debugtxt = fmt::format(
          "{:>5.2f}ms | {}%", (time * 1000.f), (int)(time / avg_frame_seconds * 100.f));
      debugDraw.RenderText2D(
          debugtxt, MT_Vector2(xcoord + const_xindent + profile_indent, ycoord), white);

      const MT_Vector2 boxSize(50 * (time / avg_frame_seconds), 10);
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

int KX_KetsjiEngine::GetFrameTimeGraphTargetFPS() const
{
  if (m_useFixedPhysicsTimestep && m_useFixedFPSCap && m_fixedRenderCapRate > 0) {
    return std::clamp(m_fixedRenderCapRate, 1, 480);
  }
  if (!m_useFixedPhysicsTimestep && m_ticrate > 0.0) {
    return std::clamp(int(std::lround(m_ticrate)), 1, 10000);
  }
  return frame_time_graph_default_target_fps;
}

int KX_KetsjiEngine::GetFrameTimeGraphSampleFPS() const
{
  int sample_fps = GetFrameTimeGraphTargetFPS();

  if (m_liveFrameTimeGraph.size() >= 2) {
    const double seconds = double(m_liveFrameTimeGraph.back().timeline_seconds -
                                  m_liveFrameTimeGraph.front().timeline_seconds);
    if (seconds > 0.0) {
      const double observed_fps = double(m_liveFrameTimeGraph.size() - 1) / seconds;
      sample_fps = std::max(sample_fps, int(std::ceil(observed_fps)));
    }
  }

  return std::clamp(sample_fps, 1, frame_time_graph_max_max_samples);
}

int KX_KetsjiEngine::GetFrameTimeGraphWindowSeconds() const
{
  return sanitize_frame_time_graph_window_seconds(m_frameTimeGraphWindowSeconds);
}

int KX_KetsjiEngine::GetFrameTimeGraphFrameWindow() const
{
  return std::clamp(GetFrameTimeGraphSampleFPS() * GetFrameTimeGraphWindowSeconds(),
                    2,
                    frame_time_graph_max_max_samples);
}

int KX_KetsjiEngine::GetFrameTimeGraphMaxSamples() const
{
  return sanitize_frame_time_graph_max_samples(m_frameTimeGraphMaxSamples);
}

int KX_KetsjiEngine::GetFrameTimeGraphVisibleDomains() const
{
  return sanitize_frame_time_graph_visible_domains(m_frameTimeGraphVisibleDomains);
}

float KX_KetsjiEngine::GetFrameTimeGraphDomainValue(const FrameTimeGraphSample &sample,
                                                    const int domain) const
{
  switch (domain) {
    case GAME_FRAME_TIME_GRAPH_DOMAIN_FRAME:
      return sample.raw_frame_ms;
    case GAME_FRAME_TIME_GRAPH_DOMAIN_WORK:
      return sample.engine_frame_ms;
    case GAME_FRAME_TIME_GRAPH_DOMAIN_PHYSICS:
      return sample.category_ms[tc_physics];
    case GAME_FRAME_TIME_GRAPH_DOMAIN_LOGIC:
      return sample.category_ms[tc_logic];
    case GAME_FRAME_TIME_GRAPH_DOMAIN_ANIMATIONS:
      return sample.category_ms[tc_animations];
    case GAME_FRAME_TIME_GRAPH_DOMAIN_DEPSGRAPH:
      return sample.category_ms[tc_depsgraph];
    case GAME_FRAME_TIME_GRAPH_DOMAIN_NETWORK:
      return sample.category_ms[tc_network];
    case GAME_FRAME_TIME_GRAPH_DOMAIN_SCENEGRAPH:
      return sample.category_ms[tc_scenegraph];
    case GAME_FRAME_TIME_GRAPH_DOMAIN_RASTERIZER:
      return sample.category_ms[tc_rasterizer];
    case GAME_FRAME_TIME_GRAPH_DOMAIN_SERVICES:
      return sample.category_ms[tc_services];
    case GAME_FRAME_TIME_GRAPH_DOMAIN_OVERHEAD:
      return sample.category_ms[tc_overhead];
    case GAME_FRAME_TIME_GRAPH_DOMAIN_OUTSIDE:
      return sample.category_ms[tc_outside];
    case GAME_FRAME_TIME_GRAPH_DOMAIN_GPU_LATENCY:
      return sample.category_ms[tc_latency];
    case GAME_FRAME_TIME_GRAPH_DOMAIN_STEPS:
      return sample.game_advance_ms;
    default:
      return sample.raw_frame_ms;
  }
}

bool KX_KetsjiEngine::IsFrameTimeGraphSamplingEnabled() const
{
  return (m_flags & (SHOW_FRAMERATE | SHOW_PROFILE)) != 0;
}

void KX_KetsjiEngine::BeginFrameTimeGraphSample(const double raw_delta_seconds)
{
  m_frameTimeGraphSamplePending = false;
  m_frameTimeGraphWorkTimerRunning = false;
  m_frameTimeGraphPendingRawDelta = 0.0;
  m_frameTimeGraphPendingWorkSeconds = 0.0;
  m_frameTimeGraphWorkStartTime = 0.0;

  if (!IsFrameTimeGraphSamplingEnabled() || raw_delta_seconds <= 0.0) {
    return;
  }

  m_frameTimeGraphSamplePending = true;
  m_frameTimeGraphWorkTimerRunning = true;
  m_frameTimeGraphPendingRawDelta = raw_delta_seconds;
  m_frameTimeGraphWorkStartTime = m_clock.GetTimeSecond();
}

void KX_KetsjiEngine::PauseFrameTimeGraphWorkTimer()
{
  if (!m_frameTimeGraphSamplePending || !m_frameTimeGraphWorkTimerRunning) {
    return;
  }

  const double now = m_clock.GetTimeSecond();
  m_frameTimeGraphPendingWorkSeconds += std::max(0.0, now - m_frameTimeGraphWorkStartTime);
  m_frameTimeGraphWorkTimerRunning = false;
}

void KX_KetsjiEngine::ResumeFrameTimeGraphWorkTimer()
{
  if (!m_frameTimeGraphSamplePending || m_frameTimeGraphWorkTimerRunning) {
    return;
  }

  m_frameTimeGraphWorkStartTime = m_clock.GetTimeSecond();
  m_frameTimeGraphWorkTimerRunning = true;
}

void KX_KetsjiEngine::FinishFrameTimeGraphSample()
{
  if (!m_frameTimeGraphSamplePending) {
    return;
  }

  PauseFrameTimeGraphWorkTimer();
  StoreFrameTimeGraphSample(m_frameTimeGraphPendingRawDelta, m_frameTimeGraphPendingWorkSeconds);

  m_frameTimeGraphSamplePending = false;
  m_frameTimeGraphPendingRawDelta = 0.0;
  m_frameTimeGraphPendingWorkSeconds = 0.0;
  m_frameTimeGraphWorkStartTime = 0.0;
}

void KX_KetsjiEngine::StoreFrameTimeGraphSample(const double raw_delta_seconds,
                                                const double engine_delta_seconds)
{
  const int graph_frame_window = GetFrameTimeGraphFrameWindow();

  const float raw_seconds = std::max(0.0f, float(raw_delta_seconds));
  if (raw_seconds <= 0.0f) {
    return;
  }
  const float engine_seconds = std::max(0.0f, float(engine_delta_seconds));

  m_frameTimeGraphTimelineSeconds += raw_seconds;

  FrameTimeGraphSample sample;
  sample.timeline_seconds = float(m_frameTimeGraphTimelineSeconds);
  sample.raw_delta_seconds = raw_seconds;
  sample.raw_frame_ms = raw_seconds * 1000.0f;
  sample.engine_delta_seconds = engine_seconds;
  sample.engine_frame_ms = engine_seconds * 1000.0f;
  for (int category = tc_first; category < tc_numCategories; ++category) {
    sample.category_ms[category] = float(m_logger.GetPrevious(KX_TimeCategory(category)) * 1000.0);
  }
  /* Samples are stored after the frame is drawn, so this is the game time of the steps run since
   * the frame before (by this frame's pass and any pass in between): steps x step length. */
  sample.game_advance_ms = m_frameTimeGraphHasGameTime ?
                               float(std::max(0.0, m_frameTime - m_frameTimeGraphLastGameTime) *
                                     1000.0) :
                               0.0f;
  m_frameTimeGraphLastGameTime = m_frameTime;
  m_frameTimeGraphHasGameTime = true;

  ++m_frameTimeGraphTimelineFrame;

  m_liveFrameTimeGraph.push_back(sample);
  while (m_liveFrameTimeGraph.size() > graph_frame_window && !m_liveFrameTimeGraph.empty()) {
    m_liveFrameTimeGraph.pop_front();
  }

  const int record_slot = m_recordFrameTimeGraphSlot;
  if (record_slot < 0 || record_slot >= FRAME_TIME_GRAPH_SLOT_COUNT) {
    return;
  }

  m_recordedFrameTimeGraphs[record_slot].push_back(sample);
  m_sharedFrameTimeGraphSlots[record_slot].push_back(sample);

  const size_t max_samples = size_t(GetFrameTimeGraphMaxSamples());
  if (m_recordedFrameTimeGraphs[record_slot].size() > max_samples) {
    while (m_recordedFrameTimeGraphs[record_slot].size() > max_samples) {
      m_recordedFrameTimeGraphs[record_slot].pop_front();
    }
  }
  if (m_sharedFrameTimeGraphSlots[record_slot].size() > max_samples) {
    while (m_sharedFrameTimeGraphSlots[record_slot].size() > max_samples) {
      m_sharedFrameTimeGraphSlots[record_slot].pop_front();
    }
  }
}

void KX_KetsjiEngine::RenderFrameTimeGraph(RAS_DebugDraw &debugDraw,
                                           const int xcoord,
                                           const int ycoord,
                                           const int profile_indent,
                                           const short profile_size) const
{
  const int graph_target_fps = GetFrameTimeGraphTargetFPS();
  const int graph_window_seconds = GetFrameTimeGraphWindowSeconds();
  const int graph_frame_window = GetFrameTimeGraphFrameWindow();
  const float graph_window_seconds_f = float(graph_window_seconds);
  const float graph_budget_ms = 1000.0f / float(graph_target_fps);
  const float graph_max_ms = graph_budget_ms * 2.0f;
  const int graph_axis = sanitize_frame_time_graph_axis(m_frameTimeGraphAxis);
  const int graph_style = sanitize_frame_time_graph_style(m_frameTimeGraphStyle);
  const int visible_domains = GetFrameTimeGraphVisibleDomains();
  const bool use_seconds_axis = graph_axis == GAME_FRAME_TIME_GRAPH_AXIS_SECONDS;
  const bool use_bars = graph_style == GAME_FRAME_TIME_GRAPH_STYLE_BARS;

  if (m_canvas == nullptr || m_liveFrameTimeGraph.empty()) {
    return;
  }

  /* Steps last: drawn over Frame, where it lies while every frame shows the same steps. */
  const std::array<int, 14> graph_domains = {
      GAME_FRAME_TIME_GRAPH_DOMAIN_FRAME,
      GAME_FRAME_TIME_GRAPH_DOMAIN_WORK,
      GAME_FRAME_TIME_GRAPH_DOMAIN_PHYSICS,
      GAME_FRAME_TIME_GRAPH_DOMAIN_LOGIC,
      GAME_FRAME_TIME_GRAPH_DOMAIN_ANIMATIONS,
      GAME_FRAME_TIME_GRAPH_DOMAIN_DEPSGRAPH,
      GAME_FRAME_TIME_GRAPH_DOMAIN_NETWORK,
      GAME_FRAME_TIME_GRAPH_DOMAIN_SCENEGRAPH,
      GAME_FRAME_TIME_GRAPH_DOMAIN_RASTERIZER,
      GAME_FRAME_TIME_GRAPH_DOMAIN_SERVICES,
      GAME_FRAME_TIME_GRAPH_DOMAIN_OVERHEAD,
      GAME_FRAME_TIME_GRAPH_DOMAIN_OUTSIDE,
      GAME_FRAME_TIME_GRAPH_DOMAIN_GPU_LATENCY,
      GAME_FRAME_TIME_GRAPH_DOMAIN_STEPS,
  };
  std::vector<int> selected_domains;
  selected_domains.reserve(graph_domains.size());
  for (const int domain : graph_domains) {
    if (visible_domains & domain) {
      selected_domains.push_back(domain);
    }
  }
  const int selected_domain_count = int(selected_domains.size());

  auto domain_label = [](const int domain) {
    switch (domain) {
      case GAME_FRAME_TIME_GRAPH_DOMAIN_FRAME:
        return "Frame";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_WORK:
        return "Work";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_PHYSICS:
        return "Physics";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_LOGIC:
        return "Logic";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_ANIMATIONS:
        return "Animations";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_DEPSGRAPH:
        return "Depsgraph";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_NETWORK:
        return "Network";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_SCENEGRAPH:
        return "Scenegraph";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_RASTERIZER:
        return "Rasterizer";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_SERVICES:
        return "Services";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_OVERHEAD:
        return "Overhead";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_OUTSIDE:
        return "Outside";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_GPU_LATENCY:
        return "GPU Latency";
      case GAME_FRAME_TIME_GRAPH_DOMAIN_STEPS:
        return "Steps";
      default:
        return "Unknown";
    }
  };

  auto pixel_center = [](const float value) { return std::floor(value) + 0.5f; };
  auto pixel_edge = [](const float value) { return std::floor(value); };

  const float graph_left = pixel_center(
      xcoord + (2.2f * profile_indent) +
      ((profile_size == 2) ? 150.0f : ((profile_size == 1) ? 110.0f : 105.0f)));
  const float graph_right = pixel_center(float(m_canvas->GetWidth()) -
                                         ((profile_size == 2) ? 24.0f : 18.0f));
  const float graph_width = graph_right - graph_left;
  if (graph_width < 96.0f) {
    return;
  }

  const float graph_top = pixel_center(std::max(
      6.0f,
      float(ycoord) - ((profile_size == 2) ? 22.0f : ((profile_size == 1) ? 17.0f : 11.0f))));
  const float canvas_height = float(m_canvas->GetHeight());
  const float profile_target_height = (profile_size == 2) ?
                                          430.0f :
                                          ((profile_size == 1) ? 380.0f : 330.0f);
  const float footer_first_row_offset = (profile_size == 2) ?
                                            50.0f :
                                            ((profile_size == 1) ? 38.0f : 28.0f);
  const float footer_row_gap = (profile_size == 2) ? 32.0f : ((profile_size == 1) ? 24.0f : 18.0f);
  const float legend_row_gap = (profile_size == 2) ? 30.0f : ((profile_size == 1) ? 23.0f : 18.0f);
  const float legend_box_size = (profile_size == 2) ? 12.0f : ((profile_size == 1) ? 10.0f : 8.0f);
  const float legend_text_gap = (profile_size == 2) ? 8.0f : ((profile_size == 1) ? 6.0f : 5.0f);
  const float legend_item_gap = (profile_size == 2) ? 28.0f :
                                                      ((profile_size == 1) ? 22.0f : 16.0f);
  const float legend_char_width = (profile_size == 2) ? 10.0f :
                                                        ((profile_size == 1) ? 8.0f : 7.0f);

  int legend_rows = 1;
  float legend_row_width = 0.0f;
  for (const int domain : selected_domains) {
    const char *label = domain_label(domain);
    const float item_width = legend_box_size + legend_text_gap +
                             (std::strlen(label) * legend_char_width) + legend_item_gap;
    if (legend_row_width > 0.0f && legend_row_width + item_width > graph_width) {
      ++legend_rows;
      legend_row_width = 0.0f;
    }
    legend_row_width += std::min(item_width, graph_width);
  }

  const float footer_height = footer_first_row_offset + (footer_row_gap * 3.0f) +
                              (legend_row_gap * float(legend_rows + 1)) + 8.0f;
  const float target_height = std::max(profile_target_height, canvas_height * 0.36f);
  const float graph_bottom = pixel_center(
      graph_top + std::min(target_height, canvas_height - graph_top - footer_height));
  const float graph_height = graph_bottom - graph_top;
  if (graph_height < 56.0f) {
    return;
  }

  const float label_offset = (profile_size == 2) ? 94.0f : ((profile_size == 1) ? 74.0f : 64.0f);
  const float label_baseline_offset = (profile_size == 2) ? 8.0f :
                                                            ((profile_size == 1) ? 6.0f : 4.0f);

  static constexpr float live_line_width = 1.0f;
  const float recorded_marker_size = (profile_size == 2) ? 5.0f :
                                                           ((profile_size == 1) ? 4.0f : 3.0f);

  const MT_Vector4 grid_color(1.0f, 1.0f, 1.0f, 0.72f);
  const MT_Vector4 label_color(1.0f, 1.0f, 1.0f, 1.0f);
  const MT_Vector4 recorded_marker_color(0.82f, 0.82f, 0.82f, 0.9f);

  auto domain_color = [](const int domain, const bool recorded) {
    const float alpha = recorded ? 0.9f : 0.95f;
    switch (domain) {
      case GAME_FRAME_TIME_GRAPH_DOMAIN_FRAME:
        return MT_Vector4(0.0f, 1.0f, 0.0f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_WORK:
        return MT_Vector4(0.15f, 0.85f, 1.0f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_PHYSICS:
        return MT_Vector4(1.0f, 0.58f, 0.12f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_LOGIC:
        return MT_Vector4(1.0f, 0.25f, 0.75f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_ANIMATIONS:
        return MT_Vector4(0.68f, 0.45f, 1.0f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_DEPSGRAPH:
        return MT_Vector4(1.0f, 0.88f, 0.18f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_NETWORK:
        return MT_Vector4(0.25f, 0.55f, 1.0f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_SCENEGRAPH:
        return MT_Vector4(0.35f, 1.0f, 0.55f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_RASTERIZER:
        return MT_Vector4(1.0f, 0.18f, 0.12f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_SERVICES:
        return MT_Vector4(0.72f, 0.72f, 0.72f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_OVERHEAD:
        return MT_Vector4(1.0f, 1.0f, 1.0f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_OUTSIDE:
        return MT_Vector4(0.55f, 0.7f, 0.85f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_GPU_LATENCY:
        return MT_Vector4(1.0f, 0.4f, 0.45f, alpha);
      case GAME_FRAME_TIME_GRAPH_DOMAIN_STEPS:
        /* Magenta: far from Frame's green, which it lies on while frames are even. */
        return MT_Vector4(0.95f, 0.2f, 1.0f, alpha);
      default:
        return MT_Vector4(0.0f, 1.0f, 0.0f, alpha);
    }
  };

  auto draw_pixel_rect = [&](const float x, const float y, const float width, const float height) {
    debugDraw.RenderRect2D(MT_Vector2(pixel_edge(x), pixel_edge(y)),
                           MT_Vector2(std::max(1.0f, width), std::max(1.0f, height)),
                           grid_color);
  };

  draw_pixel_rect(graph_left, graph_top, graph_width + 1.0f, 1.0f);
  draw_pixel_rect(graph_left, graph_bottom, graph_width + 1.0f, 1.0f);
  draw_pixel_rect(graph_left, graph_top, 1.0f, graph_height + 1.0f);
  draw_pixel_rect(graph_right, graph_top, 1.0f, graph_height + 1.0f);

  for (const float guide_ms : {graph_budget_ms, graph_max_ms}) {
    const float y = pixel_center(graph_bottom -
                                 std::clamp(guide_ms / graph_max_ms, 0.0f, 1.0f) * graph_height);
    if (guide_ms < graph_max_ms) {
      draw_pixel_rect(graph_left, y, graph_width + 1.0f, 1.0f);
    }
    debugDraw.RenderText2D(fmt::format("{:>4.1f} ms", guide_ms),
                           MT_Vector2(graph_left - label_offset, y + label_baseline_offset),
                           label_color);
  }
  debugDraw.RenderText2D(
      fmt::format("{:>4.1f} ms", 0.0f),
      MT_Vector2(graph_left - label_offset, graph_bottom + label_baseline_offset),
      label_color);

  for (int divider = 1; divider < graph_window_seconds; ++divider) {
    const float x = pixel_center(graph_left + (divider / graph_window_seconds_f) * graph_width);
    draw_pixel_rect(x, graph_top, 1.0f, graph_height + 1.0f);
  }

  const int window_end_frame = m_frameTimeGraphTimelineFrame - 1;
  const int window_start_frame = std::max(0, window_end_frame - (graph_frame_window - 1));
  const float window_end_seconds = std::max(0.0f, m_liveFrameTimeGraph.back().timeline_seconds);
  const float window_start_seconds = std::max(0.0f, window_end_seconds - graph_window_seconds_f);

  auto sample_axis_unit = [&](const int sample_frame, const float sample_seconds) {
    if (use_seconds_axis) {
      return (sample_seconds - window_start_seconds) / graph_window_seconds_f;
    }
    return (sample_frame - window_start_frame) / float(graph_frame_window - 1);
  };

  float min_selected_ms = 0.0f;
  float max_selected_ms = 0.0f;
  bool has_visible_stats = false;
  const int first_live_sample_frame = m_frameTimeGraphTimelineFrame -
                                      int(m_liveFrameTimeGraph.size());
  for (int index = 0; index < int(m_liveFrameTimeGraph.size()); ++index) {
    const FrameTimeGraphSample &sample = m_liveFrameTimeGraph[index];
    const float axis_unit = sample_axis_unit(first_live_sample_frame + index,
                                             sample.timeline_seconds);
    if (axis_unit < 0.0f || axis_unit > 1.0f) {
      continue;
    }

    for (const int domain : selected_domains) {
      const float sample_ms = GetFrameTimeGraphDomainValue(sample, domain);
      if (!has_visible_stats) {
        min_selected_ms = max_selected_ms = sample_ms;
        has_visible_stats = true;
      }
      else {
        min_selected_ms = std::min(min_selected_ms, sample_ms);
        max_selected_ms = std::max(max_selected_ms, sample_ms);
      }
    }
  }
  if (!has_visible_stats) {
    min_selected_ms = max_selected_ms = 0.0f;
  }

  const float footer_first_row = graph_bottom + footer_first_row_offset;
  debugDraw.RenderText2D(
      fmt::format("Selected min/max: {:.2f}/{:.2f} ms", min_selected_ms, max_selected_ms),
      MT_Vector2(graph_left, footer_first_row),
      label_color);
  debugDraw.RenderText2D(fmt::format("Real time: {:.2f} s", m_clockTime),
                         MT_Vector2(graph_left, footer_first_row + footer_row_gap),
                         label_color);
  debugDraw.RenderText2D(fmt::format("Game time: {:.2f} s", m_frameTime),
                         MT_Vector2(graph_left, footer_first_row + (footer_row_gap * 2.0f)),
                         label_color);

  const float legend_title_y = footer_first_row + (footer_row_gap * 3.0f);
  debugDraw.RenderText2D("Domains:", MT_Vector2(graph_left, legend_title_y), label_color);

  float legend_x = graph_left;
  float legend_y = legend_title_y + legend_row_gap;
  if (selected_domains.empty()) {
    debugDraw.RenderText2D("None", MT_Vector2(legend_x, legend_y), label_color);
  }
  else {
    for (const int domain : selected_domains) {
      const char *label = domain_label(domain);
      const float item_width = legend_box_size + legend_text_gap +
                               (std::strlen(label) * legend_char_width) + legend_item_gap;
      if (legend_x > graph_left && legend_x + item_width > graph_right) {
        legend_x = graph_left;
        legend_y += legend_row_gap;
      }

      debugDraw.RenderRect2D(
          MT_Vector2(pixel_edge(legend_x),
                     pixel_edge(legend_y - legend_box_size + label_baseline_offset)),
          MT_Vector2(legend_box_size, legend_box_size),
          domain_color(domain, false));
      debugDraw.RenderText2D(
          label, MT_Vector2(legend_x + legend_box_size + legend_text_gap, legend_y), label_color);
      legend_x += std::min(item_width, graph_width);
    }
  }

  auto graph_point = [&](const float axis_unit, const float frame_ms) {
    const float x = pixel_center(graph_left + std::clamp(axis_unit, 0.0f, 1.0f) * graph_width);
    const float y = pixel_center(graph_bottom -
                                 std::clamp(frame_ms / graph_max_ms, 0.0f, 1.0f) * graph_height);
    return MT_Vector2(x, y);
  };

  auto draw_clipped_line = [&](const float from_axis,
                               const float from_ms,
                               const float to_axis,
                               const float to_ms,
                               const MT_Vector4 &color,
                               const float width) {
    if (from_axis == to_axis || to_axis < 0.0f || from_axis > 1.0f) {
      return;
    }

    float axis0 = from_axis;
    float axis1 = to_axis;
    float ms0 = from_ms;
    float ms1 = to_ms;
    if (axis0 > axis1) {
      std::swap(axis0, axis1);
      std::swap(ms0, ms1);
    }
    if (axis1 < 0.0f || axis0 > 1.0f || axis0 == axis1) {
      return;
    }

    const float original_axis0 = axis0;
    const float original_axis1 = axis1;
    const float original_ms0 = ms0;
    const float original_ms1 = ms1;

    if (axis0 < 0.0f) {
      const float factor = (0.0f - original_axis0) / (original_axis1 - original_axis0);
      axis0 = 0.0f;
      ms0 = original_ms0 + factor * (original_ms1 - original_ms0);
    }
    if (axis1 > 1.0f) {
      const float factor = (1.0f - original_axis0) / (original_axis1 - original_axis0);
      axis1 = 1.0f;
      ms1 = original_ms0 + factor * (original_ms1 - original_ms0);
    }

    debugDraw.RenderLine2D(graph_point(axis0, ms0), graph_point(axis1, ms1), color, width);
  };

  auto draw_bar = [&](const float axis_unit,
                      const float frame_ms,
                      const MT_Vector4 &color,
                      const int domain_index) {
    if (axis_unit < 0.0f || axis_unit > 1.0f) {
      return;
    }

    const float sample_width = std::max(1.0f, std::floor(graph_width / float(graph_frame_window)));
    const float bar_width = std::max(1.0f, (sample_width * 0.9f) / float(selected_domain_count));
    const float bar_offset = (float(domain_index) -
                              ((float(selected_domain_count) - 1.0f) * 0.5f)) *
                             bar_width;
    const MT_Vector2 point = graph_point(axis_unit, frame_ms);
    debugDraw.RenderRect2D(
        MT_Vector2(pixel_edge(point.x() + bar_offset - (bar_width * 0.5f)), pixel_edge(point.y())),
        MT_Vector2(bar_width, std::max(1.0f, graph_bottom - point.y())),
        color);
  };

  auto draw_marker = [&](const float axis_unit, const float frame_ms) {
    if (axis_unit < 0.0f || axis_unit > 1.0f) {
      return;
    }

    const MT_Vector2 point = graph_point(axis_unit, frame_ms);
    const float half_size = recorded_marker_size * 0.5f;
    debugDraw.RenderRect2D(
        MT_Vector2(pixel_edge(point.x() - half_size), pixel_edge(point.y() - half_size)),
        MT_Vector2(recorded_marker_size, recorded_marker_size),
        recorded_marker_color);
  };

  auto draw_series = [&](const std::deque<FrameTimeGraphSample> &samples,
                         const bool recorded,
                         const int domain,
                         const int domain_index,
                         const MT_Vector4 &constant_color) {
    if (samples.empty() || m_frameTimeGraphTimelineFrame <= 0) {
      return;
    }

    const bool draw_bars = use_bars && !recorded;
    bool has_previous = false;
    float previous_axis = 0.0f;
    float previous_ms = 0.0f;
    const int sample_count = int(samples.size());
    const int first_sample_frame = recorded ? 0 : m_frameTimeGraphTimelineFrame - sample_count;
    int start_index = 0;
    int end_index = sample_count;

    if (use_seconds_axis) {
      const auto lower = std::lower_bound(
          samples.begin(),
          samples.end(),
          window_start_seconds,
          [](const FrameTimeGraphSample &sample, const float seconds) {
            return sample.timeline_seconds < seconds;
          });
      const auto upper = std::upper_bound(
          samples.begin(),
          samples.end(),
          window_end_seconds,
          [](const float seconds, const FrameTimeGraphSample &sample) {
            return seconds < sample.timeline_seconds;
          });
      start_index = int(lower - samples.begin());
      end_index = int(upper - samples.begin());
      if (!recorded && start_index > 0) {
        --start_index;
      }
    }
    else {
      start_index = std::clamp(window_start_frame - first_sample_frame, 0, sample_count);
      end_index = std::clamp(window_end_frame - first_sample_frame + 1, 0, sample_count);
      if (!recorded && start_index > 0) {
        --start_index;
      }
    }

    if (start_index >= end_index) {
      return;
    }

    for (int index = start_index; index < end_index; ++index) {
      const FrameTimeGraphSample &sample = samples[index];
      const int sample_frame = first_sample_frame + index;
      const float axis_unit = sample_axis_unit(sample_frame, sample.timeline_seconds);
      const float sample_ms = GetFrameTimeGraphDomainValue(sample, domain);

      if (recorded) {
        draw_marker(axis_unit, sample_ms);
      }
      else if (draw_bars) {
        draw_bar(axis_unit, sample_ms, constant_color, domain_index);
      }
      else if (has_previous) {
        draw_clipped_line(
            previous_axis, previous_ms, axis_unit, sample_ms, constant_color, live_line_width);
      }

      previous_axis = axis_unit;
      previous_ms = sample_ms;
      has_previous = true;
    }
  };

  for (int domain_index = 0; domain_index < selected_domain_count; ++domain_index) {
    const int domain = selected_domains[domain_index];
    draw_series(m_liveFrameTimeGraph, false, domain, domain_index, domain_color(domain, false));
  }

  for (int slot = 0; slot < FRAME_TIME_GRAPH_SLOT_COUNT; ++slot) {
    if ((m_showRecordedFrameTimeGraphSlots & (1 << slot)) == 0) {
      continue;
    }
    if (slot == m_recordFrameTimeGraphSlot) {
      continue;
    }

    const std::deque<FrameTimeGraphSample> &samples = m_recordedFrameTimeGraphs[slot];
    for (int domain_index = 0; domain_index < selected_domain_count; ++domain_index) {
      const int domain = selected_domains[domain_index];
      draw_series(samples, true, domain, domain_index, domain_color(domain, true));
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
        (m_showCameraFrustum == KX_DebugOption::FORCE || cam->GetShowCameraFrustum()))
    {
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
  std::cout << "KX_KetsjiEngine::RemoveScene: Remove Scenes is temporarily disabled during eevee "
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
         scenenameit++)
    {
      std::string scenename = *scenenameit;

      KX_Scene *scene = FindScene(scenename);
      if (scene) {
        m_rasterizer->GetDebugDraw().DiscardFixedStepSnapshotContext(scene);
        m_converter->RemoveScene(scene);
        m_scenes->RemoveValue(scene);
      }
    }
    m_removingScenes.clear();
  }
}

KX_Scene *KX_KetsjiEngine::CreateScene(blender::Scene *scene,
                                       bool libloading,
                                       bool merge_source)
{
  KX_Scene *tmpscene = new KX_Scene(
      m_inputDevice, scene->id.name + 2, scene, m_canvas, m_networkMessageManager);
  /* LibLoad merges every scene it converts. */
  tmpscene->SetMergeSource(merge_source || libloading);

  m_converter->ConvertScene(tmpscene, m_rasterizer, m_canvas, libloading);

  return tmpscene;
}

KX_KetsjiEngine::SceneMergeResult KX_KetsjiEngine::MergeScene(KX_Scene *target,
                                                              const std::string &scenename)
{
  blender::Scene *blender_scene = m_converter->GetBlenderSceneForName(scenename);
  if (blender_scene == nullptr) {
    return SceneMergeResult::NotFound;
  }
  /* The scene built for the merge switches Blender's context and the editor's window to its
   * Blender scene: both are put back as they were. */
  const KX_Scene::EditorViewState view_state = KX_Scene::SaveEditorViewState();
  KX_Scene *load_scene = CreateScene(blender_scene, false, true);
  const bool merged = target->MergeScene(load_scene);
  if (merged) {
    /* Its contents are the target's now (the converter's share included). */
    delete load_scene;
  }
  else {
    m_converter->RemoveScene(load_scene);
  }
  KX_Scene::RestoreEditorViewState(view_state);
  return merged ? SceneMergeResult::Merged : SceneMergeResult::Failed;
}

KX_Scene *KX_KetsjiEngine::CreateScene(const std::string &scenename)
{
  blender::Scene *scene = m_converter->GetBlenderSceneForName(scenename);
  if (!scene)
    return nullptr;

  return CreateScene(scene, false);
}

bool KX_KetsjiEngine::ReplaceScene(const std::string &oldscene, const std::string &newscene)
{
#ifdef WITH_GAMEENGINE_LOGICNODES
  if (m_pendingGameLoad && oldscene == m_pendingGameLoad->source_scene_name &&
      newscene != m_pendingGameLoad->target_scene_name)
  {
    CM_Warning("scene replacement '" << oldscene << "' -> '" << newscene
                                     << "' conflicts with a pending Load Game transition");
    return false;
  }
#endif

  // Don't allow replacement if the new scene doesn't exist.
  // Allows smarter game design (used to have no check here).
  // Note that it creates a small backward compatibility issue
  // for a game that did a replace followed by a lib load with the
  // new scene in the lib => it won't work anymore, the lib
  // must be loaded before doing the replace.
  if (m_converter->GetBlenderSceneForName(newscene) != nullptr) {
    /* Multiple logic sources can request the same deferred transition in one frame. */
    const auto duplicate = std::find_if(
        m_replace_scenes.begin(),
        m_replace_scenes.end(),
        [&](const std::pair<std::string, std::string> &replacement) {
          return replacement.first == oldscene && replacement.second == newscene;
        });
    if (duplicate == m_replace_scenes.end()) {
      m_replace_scenes.emplace_back(oldscene, newscene);
    }
    return true;
  }

  return false;
}

#ifdef WITH_GAMEENGINE_LOGICNODES
LN_GameStateResult KX_KetsjiEngine::RequestGameLoadSceneTransition(
    KX_Scene &source_scene,
    const LN_GameStateCompletionTarget &target,
    const std::string &filepath,
    std::unique_ptr<LN_GameStateLoadPayload> payload,
    std::string &r_error)
{
  r_error.clear();
  if (!payload) {
    r_error = "Load Game scene transition has no decoded checkpoint";
    return LN_GameStateResult::InvalidRequest;
  }
  if (m_pendingGameLoad) {
    r_error = "another Load Game scene transition is already pending";
    return LN_GameStateResult::Busy;
  }
  if (m_converter == nullptr || m_converter->GetMain() == nullptr || m_scenes == nullptr) {
    r_error = "the current Blender file or runtime scene list is unavailable";
    return LN_GameStateResult::InternalError;
  }

  const std::string &scene_uuid_text = payload->GetSceneUuid();
  blender::bUUID scene_uuid = {};
  if (scene_uuid_text.size() != UUID_STRING_SIZE - 1 ||
      !blender::BLI_uuid_parse_string(&scene_uuid, scene_uuid_text.c_str()) ||
      blender::BLI_uuid_is_nil(scene_uuid))
  {
    r_error = "Load Game checkpoint has an invalid destination Scene UUID";
    return LN_GameStateResult::CorruptData;
  }

  blender::Main *bmain = m_converter->GetMain();
  blender::Scene *destination = nullptr;
  size_t destination_count = 0;
  bool source_belongs_to_main = false;
  for (blender::Scene &candidate : bmain->scenes) {
    source_belongs_to_main |= &candidate == source_scene.GetBlenderScene();
    blender::bUUID candidate_uuid = {};
    if (BKE_game_persistence_identity_read(candidate.id, candidate_uuid) !=
        BKE_GamePersistenceIdentityState::Valid)
    {
      continue;
    }
    if (blender::BLI_uuid_equal(candidate_uuid, scene_uuid)) {
      destination = &candidate;
      destination_count++;
    }
  }
  if (!source_belongs_to_main) {
    r_error = "Load Game can only switch between scenes in the currently loaded Blender file";
    return LN_GameStateResult::Incompatible;
  }
  if (destination_count == 0) {
    r_error = "checkpoint scene '" + payload->GetSceneName() + "' (" + scene_uuid_text +
              ") is not present in the currently loaded Blender file";
    return LN_GameStateResult::Incompatible;
  }
  if (destination_count != 1) {
    r_error =
        "checkpoint destination Scene UUID is duplicated in the currently loaded Blender "
        "file";
    return LN_GameStateResult::Incompatible;
  }
  if (destination == source_scene.GetBlenderScene()) {
    r_error = "Load Game requested a scene transition to its current runtime scene";
    return LN_GameStateResult::InternalError;
  }

  size_t source_runtime_count = 0;
  for (unsigned int index = 0; index < m_scenes->GetCount(); index++) {
    KX_Scene *runtime_scene = m_scenes->GetValue(index);
    if (runtime_scene == &source_scene) {
      source_runtime_count++;
    }
    else if (runtime_scene != nullptr && runtime_scene->GetBlenderScene() == destination) {
      r_error = "checkpoint destination scene is already active as another runtime scene";
      return LN_GameStateResult::Busy;
    }
  }
  if (source_runtime_count != 1) {
    r_error = "the Load Game source scene is no longer uniquely active";
    return LN_GameStateResult::InvalidRequest;
  }

  const std::string source_name = source_scene.GetName();
  const std::string destination_name = destination->id.name + 2;
  for (const std::pair<std::string, std::string> &replacement : m_replace_scenes) {
    if (replacement.first == source_name && replacement.second != destination_name) {
      r_error = "another scene replacement is already pending for the Load Game source scene";
      return LN_GameStateResult::Busy;
    }
  }

  try {
    std::unique_ptr<PendingGameLoad> transaction = std::make_unique<PendingGameLoad>();
    transaction->source_scene = &source_scene;
    transaction->target_scene = destination;
    transaction->target_scene_uuid = scene_uuid;
    transaction->source_scene_name = source_name;
    transaction->target_scene_name = destination_name;
    transaction->filepath = filepath;
    transaction->completion_target = target;
    transaction->payload = std::move(payload);
    if (!ReplaceScene(source_name, destination_name)) {
      r_error = "checkpoint destination scene could not be scheduled for replacement";
      return LN_GameStateResult::Incompatible;
    }
    m_pendingGameLoad = std::move(transaction);
  }
  catch (const std::exception &exception) {
    r_error = std::string("could not schedule Load Game scene transition: ") + exception.what();
    return LN_GameStateResult::InternalError;
  }
  catch (...) {
    r_error = "could not schedule Load Game scene transition";
    return LN_GameStateResult::InternalError;
  }

  return LN_GameStateResult::Success;
}
#endif

// replace scene is not the same as removing and adding because the
// scene must be in exact the same place (to maintain drawingorder)
// (nzc) - should that not be done with a scene-display list? It seems
// stupid to rely on the mem allocation order...
void KX_KetsjiEngine::ReplaceScheduledScenes()
{
  if (m_replace_scenes.size()) {
    std::vector<std::pair<std::string, std::string>>::iterator scenenameit;

    for (scenenameit = m_replace_scenes.begin(); scenenameit != m_replace_scenes.end();
         scenenameit++)
    {
      std::string oldscenename = (*scenenameit).first;
      std::string newscenename = (*scenenameit).second;
#ifdef WITH_GAMEENGINE_LOGICNODES
      const bool is_game_load_transition = m_pendingGameLoad &&
                                           oldscenename == m_pendingGameLoad->source_scene_name &&
                                           newscenename == m_pendingGameLoad->target_scene_name;
      bool game_load_transition_completed = false;
#endif
      /* Scenes are not supposed to be included twice... I think */
      for (unsigned int sce_idx = 0; sce_idx < m_scenes->GetCount(); ++sce_idx) {
        KX_Scene *scene = m_scenes->GetValue(sce_idx);
        if (scene->GetName() == oldscenename) {
#ifdef WITH_GAMEENGINE_LOGICNODES
          if (is_game_load_transition && scene != m_pendingGameLoad->source_scene) {
            continue;
          }
#endif
          // avoid crash if the new scene doesn't exist, just do nothing
          blender::Scene *blScene = nullptr;
#ifdef WITH_GAMEENGINE_LOGICNODES
          if (is_game_load_transition) {
            blender::Main *bmain = m_converter != nullptr ? m_converter->GetMain() : nullptr;
            size_t destination_count = 0;
            bool source_blender_scene_present = false;
            if (bmain != nullptr) {
              for (blender::Scene &candidate : bmain->scenes) {
                source_blender_scene_present |= &candidate == scene->GetBlenderScene();
                blender::bUUID candidate_uuid = {};
                if (BKE_game_persistence_identity_read(candidate.id, candidate_uuid) ==
                        BKE_GamePersistenceIdentityState::Valid &&
                    blender::BLI_uuid_equal(candidate_uuid, m_pendingGameLoad->target_scene_uuid))
                {
                  destination_count++;
                  if (&candidate == m_pendingGameLoad->target_scene) {
                    blScene = &candidate;
                  }
                }
              }
            }

            size_t source_runtime_count = 0;
            bool destination_is_active = false;
            for (unsigned int index = 0; index < m_scenes->GetCount(); index++) {
              KX_Scene *runtime_scene = m_scenes->GetValue(index);
              source_runtime_count += runtime_scene == m_pendingGameLoad->source_scene;
              destination_is_active |= blScene != nullptr && runtime_scene != nullptr &&
                                       runtime_scene != m_pendingGameLoad->source_scene &&
                                       runtime_scene->GetBlenderScene() == blScene;
            }
            if (!source_blender_scene_present || destination_count != 1 ||
                source_runtime_count != 1 || destination_is_active)
            {
              blScene = nullptr;
            }
          }
          else {
#endif
            blScene = m_converter->GetBlenderSceneForName(newscenename);
#ifdef WITH_GAMEENGINE_LOGICNODES
          }
#endif
          if (blScene) {
            m_rasterizer->GetDebugDraw().DiscardFixedStepSnapshotContext(scene);
            m_converter->RemoveScene(scene);

            KX_Scene *tmpscene = CreateScene(blScene, false);
            m_scenes->SetValue(sce_idx, CM_AddRef(tmpscene));
            PostProcessScene(tmpscene);
#ifdef WITH_GAMEENGINE_LOGICNODES
            if (is_game_load_transition) {
              LN_GameStateResult load_result = LN_GameStateResult::InternalError;
              std::string load_error;
              LN_Manager *manager = tmpscene->GetLogicNodeManager();
              if (manager == nullptr) {
                load_error = "checkpoint destination scene has no C++ Logic Nodes manager";
              }
              else {
                load_result = manager->ApplyTransferredGameState(*m_pendingGameLoad->payload,
                                                                 load_error);
                manager->NotifyGameLoadFinished(LN_GameStateSucceeded(load_result));
              }
              if (!LN_GameStateSucceeded(load_result)) {
                CM_Warning("Logic Nodes Load Game failed after switching to scene '"
                           << newscenename << "' for '" << m_pendingGameLoad->filepath
                           << "': " << load_error);
              }
              game_load_transition_completed = true;
              m_pendingGameLoad.reset();
            }
#endif
            tmpscene->Release();
#ifdef WITH_GAMEENGINE_LOGICNODES
            if (is_game_load_transition) {
              break;
            }
#endif
          }
          else {
            CM_Warning("scene " << newscenename << " could not be found, not replaced!");
          }
        }
      }
#ifdef WITH_GAMEENGINE_LOGICNODES
      if (is_game_load_transition && !game_load_transition_completed && m_pendingGameLoad) {
        const std::string failure =
            "scheduled checkpoint scene replacement could not find its source or destination";
        KX_Scene *source_scene = m_pendingGameLoad->source_scene;
        bool source_is_active = false;
        for (unsigned int index = 0; index < m_scenes->GetCount(); index++) {
          source_is_active |= m_scenes->GetValue(index) == source_scene;
        }
        if (source_is_active && source_scene->GetLogicNodeManager() != nullptr) {
          source_scene->GetLogicNodeManager()->CompleteTransferredGameStateFailure(
              m_pendingGameLoad->completion_target, m_pendingGameLoad->filepath, failure);
        }
        CM_Warning("Logic Nodes Load Game failed for '" << m_pendingGameLoad->filepath
                                                        << "': " << failure);
        m_pendingGameLoad.reset();
      }
#endif
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
  if (!std::isfinite(timescale)) {
    CM_Warning("Ignoring non-finite time scale " << timescale);
    return;
  }

  m_timescale = timescale <= MIN_TIME_SCALE ? MIN_TIME_SCALE : std::min(timescale, MAX_TIME_SCALE);
}

bool KX_KetsjiEngine::IsGamePaused() const
{
  return m_gamePaused;
}

bool KX_KetsjiEngine::IsGamePauseRequested() const
{
  return m_hasDeferredGamePaused ? m_deferredGamePaused : m_gamePaused;
}

void KX_KetsjiEngine::SetGamePaused(const bool paused)
{
  if (m_fixedStepInProgress || m_frameUpdateInProgress) {
    m_hasDeferredGamePaused = true;
    m_deferredGamePaused = paused;
    return;
  }

  if (m_gamePaused == paused) {
    return;
  }

  // Keep m_timescale intact so pause/resume preserves slow motion and other time-scale effects.
  if (paused) {
    m_pauseAnimationTime = m_frameTime;
    m_previousPauseAnimTime = m_pauseAnimationTime;
#ifdef WITH_GAMEENGINE_LOGICNODES
    for (KX_Scene *scene : m_scenes) {
      if (LN_Manager *manager = scene->GetLogicNodeManager()) {
        manager->RefreshPauseProcessingObjects();
      }
    }
#endif
  }
  else {
#ifdef WITH_GAMEENGINE_LOGICNODES
    for (KX_Scene *scene : m_scenes) {
      scene->RebasePausedAnimations(m_pauseAnimationTime, m_frameTime);
      if (LN_Manager *manager = scene->GetLogicNodeManager()) {
        manager->ClearPauseProcessingObjects();
      }
    }
#endif
  }
  m_gamePaused = paused;
  /* A resume lines the steps up with the frames again: the leftover time of the pause moment
   * would put them anywhere between two frames (the knife edge behind stutter after a resume). */
  m_frameScheduler.SetPaused(paused);
  InvalidatePhysicsInterpolationState();
  if (!paused && !m_physicsPaused) {
    /* Frame-update logic may deliberately keep running during pause. Rebase
     * transform-driven bodies so that paused displacement is a teleport, not
     * one fixed step of artificial collision velocity on resume. */
    for (KX_Scene *scene : m_scenes) {
      scene->RebaseKinematicPhysics();
    }
  }
}

bool KX_KetsjiEngine::IsPhysicsPaused() const
{
  return m_physicsPaused;
}

void KX_KetsjiEngine::SetPhysicsPaused(const bool paused)
{
  if (m_fixedStepInProgress || m_frameUpdateInProgress) {
    m_hasDeferredPhysicsPaused = true;
    m_deferredPhysicsPaused = paused;
    return;
  }

  if (m_physicsPaused == paused) {
    return;
  }

  // Physics time continues to be scheduled and consumed while paused. This intentionally avoids
  // a burst of fixed Jolt steps when the simulation resumes.
  m_physicsPaused = paused;
  InvalidatePhysicsInterpolationState();
  if (!paused && !m_gamePaused) {
    for (KX_Scene *scene : m_scenes) {
      scene->RebaseKinematicPhysics();
    }
  }
}

void KX_KetsjiEngine::ApplyDeferredPauseState()
{
  BLI_assert(!m_fixedStepInProgress && !m_frameUpdateInProgress);

  const bool apply_game_paused = m_hasDeferredGamePaused;
  const bool game_paused = m_deferredGamePaused;
  const bool apply_physics_paused = m_hasDeferredPhysicsPaused;
  const bool physics_paused = m_deferredPhysicsPaused;
  m_hasDeferredGamePaused = false;
  m_hasDeferredPhysicsPaused = false;

  if (apply_game_paused) {
    SetGamePaused(game_paused);
  }
  if (apply_physics_paused) {
    SetPhysicsPaused(physics_paused);
  }
}

int KX_KetsjiEngine::GetMaxLogicFrame()
{
  return m_maxLogicFrame;
}

void KX_KetsjiEngine::SetMaxLogicFrame(int frame)
{
  m_maxLogicFrame = ClampScheduledStepLimit(frame);
}

int KX_KetsjiEngine::GetMaxPhysicsFrame()
{
  // In fixed mode, returns maxPhysicsSteps; in variable mode, returns maxLogicFrame
  return m_useFixedPhysicsTimestep ? m_maxPhysicsSteps : m_maxLogicFrame;
}

void KX_KetsjiEngine::SetMaxPhysicsFrame(int frame)
{
  m_maxPhysicsSteps = ClampScheduledStepLimit(frame);
  ConfigureFrameScheduler();
}

/********** PHYSICS MODE SWITCHING **********/

bool KX_KetsjiEngine::GetUseFixedPhysicsTimestep()
{
  return m_useFixedPhysicsTimestep;
}

void KX_KetsjiEngine::SetUseFixedPhysicsTimestep(bool useFixed)
{
  if (m_useFixedPhysicsTimestep == useFixed) {
    return;  // No change
  }

  m_useFixedPhysicsTimestep = useFixed;

  if (useFixed) {
    ResetFixedPhysicsTiming();
  }
  else {
    InvalidatePhysicsInterpolationState();
    m_previousRealTime = m_clockTime;
    m_currentInterpolationFraction = 0.0;
  }
}

/********** FIXED MODE GETTERS/SETTERS **********/

int KX_KetsjiEngine::GetPhysicsTickRate()
{
  return m_physicsTickRate;
}

void KX_KetsjiEngine::SetPhysicsTickRate(int tickRate)
{
  if (tickRate > 0) {
    m_physicsTickRate = tickRate;
    m_fixedPhysicsTimestep = 1.0 / static_cast<double>(tickRate);
    ConfigureFrameScheduler();
    ResetFixedPhysicsTiming();
  }
}

bool KX_KetsjiEngine::GetUseFixedFPSCap()
{
  return m_useFixedFPSCap;
}

void KX_KetsjiEngine::SetUseFixedFPSCap(bool useFixed)
{
  m_useFixedFPSCap = useFixed;
  /* A new rhythm starts at the next pass. */
  ConfigureFrameScheduler();
}

int KX_KetsjiEngine::GetFixedRenderCapRate()
{
  return m_fixedRenderCapRate;
}

void KX_KetsjiEngine::SetFixedRenderCapRate(int rate)
{
  if (rate > 0) {
    m_fixedRenderCapRate = rate;
    ConfigureFrameScheduler();
  }
}

void KX_KetsjiEngine::SetVsyncPacing(const bool vsync)
{
  m_vsyncPacing = vsync;
  ConfigureFrameScheduler();
}

void KX_KetsjiEngine::SetJitterFix(const double steps)
{
  m_jitterFix = std::isfinite(steps) ? std::clamp(steps, 0.0, 2.0) : 0.0;
  ConfigureFrameScheduler();
}

double KX_KetsjiEngine::GetJitterFix() const
{
  return m_jitterFix;
}

uint64_t KX_KetsjiEngine::GetSimulationTick() const
{
  return m_frameScheduler.GetTick();
}

KX_KetsjiEngine::FramePacingInfo KX_KetsjiEngine::GetFramePacingInfo() const
{
  FramePacingInfo info;
  info.tick = m_frameScheduler.GetTick();
  if (!m_useFixedPhysicsTimestep) {
    return info;
  }
  info.frame_steps = m_frameScheduler.GetLastFrameSteps();
  info.interpolation = m_currentInterpolationFraction;
  info.frame_time = m_frameScheduler.GetLastFrameTime();
  info.clock_offset = m_frameScheduler.GetLastFrameClockOffset();
  switch (m_frameScheduler.GetPacing()) {
    case KX_FrameScheduler::Pacing::Limit:
      info.pacing = "LIMIT";
      info.frame_limit = m_frameScheduler.GetSettings().limit_rate;
      break;
    case KX_FrameScheduler::Pacing::Vsync:
      info.pacing = "VSYNC";
      break;
    case KX_FrameScheduler::Pacing::Free:
      info.pacing = "FREE";
      break;
  }
  info.refresh_rate = m_frameScheduler.GetRefreshRate();
  return info;
}

double KX_KetsjiEngine::GetAnimFrameRate()
{
  return m_anim_framerate;
}

void KX_KetsjiEngine::SetAnimFrameRate(double framerate)
{
  if (framerate > 0.0) {
    m_anim_framerate = framerate;
  }
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
    if (flag == CAMERA_OVERRIDE) {
      m_cameraOverrideActive = false;
    }
  }

  if ((flag & (SHOW_PROFILE | SHOW_FRAMERATE)) && !IsFrameTimeGraphSamplingEnabled()) {
    m_recordFrameTimeGraphSlot = -1;
    m_frameTimeGraphSamplePending = false;
    m_frameTimeGraphWorkTimerRunning = false;
    m_frameTimeGraphPendingRawDelta = 0.0;
    m_frameTimeGraphPendingWorkSeconds = 0.0;
    m_frameTimeGraphWorkStartTime = 0.0;
    m_liveFrameTimeGraph.clear();
    /* Turned on again later, the first sample must not count the game time in between. */
    m_frameTimeGraphHasGameTime = false;
    m_frameStepCounts.Reset();
  }
}

void KX_KetsjiEngine::SetFrameTimeGraphSettings(const int window_seconds,
                                                const int axis,
                                                const int style,
                                                const int max_samples,
                                                const int visible_domains)
{
  m_frameTimeGraphWindowSeconds = sanitize_frame_time_graph_window_seconds(window_seconds);
  m_frameTimeGraphAxis = sanitize_frame_time_graph_axis(axis);
  m_frameTimeGraphStyle = sanitize_frame_time_graph_style(style);
  m_frameTimeGraphMaxSamples = sanitize_frame_time_graph_max_samples(max_samples);
  m_frameTimeGraphVisibleDomains = sanitize_frame_time_graph_visible_domains(visible_domains);
}

void KX_KetsjiEngine::SetFrameTimeGraphSlots(const int record_slot, const int visible_slots)
{
  m_showRecordedFrameTimeGraphSlots = visible_slots & ((1 << FRAME_TIME_GRAPH_SLOT_COUNT) - 1);
  m_recordFrameTimeGraphSlot = (record_slot >= 0 && record_slot < FRAME_TIME_GRAPH_SLOT_COUNT) ?
                                   record_slot :
                                   -1;

  for (int slot = 0; slot < FRAME_TIME_GRAPH_SLOT_COUNT; ++slot) {
    const size_t max_samples = size_t(GetFrameTimeGraphMaxSamples());
    while (m_sharedFrameTimeGraphSlots[slot].size() > max_samples) {
      m_sharedFrameTimeGraphSlots[slot].pop_front();
    }

    const bool show_slot = (m_showRecordedFrameTimeGraphSlots & (1 << slot)) != 0;
    if (!show_slot || slot == m_recordFrameTimeGraphSlot) {
      m_recordedFrameTimeGraphs[slot].clear();
      continue;
    }

    m_recordedFrameTimeGraphs[slot] = m_sharedFrameTimeGraphSlots[slot];
  }

  if (m_recordFrameTimeGraphSlot >= 0) {
    m_recordedFrameTimeGraphs[m_recordFrameTimeGraphSlot].clear();
    m_sharedFrameTimeGraphSlots[m_recordFrameTimeGraphSlot].clear();
  }
}

void KX_KetsjiEngine::ForceInterpolationRender()
{
  m_forceInterpolationRender = true;
}

void KX_KetsjiEngine::ClearForcedInterpolation()
{
  m_forceInterpolationRender = false;
}

double KX_KetsjiEngine::GetClockTime(void) const
{
  return m_clockTime;
}

void KX_KetsjiEngine::SetPhysicsInterpolationEnabled(bool enable)
{
  if (m_enablePhysicsInterpolation == enable) {
    return;
  }

  m_enablePhysicsInterpolation = enable;
  InvalidatePhysicsInterpolationState();

  if (!m_enablePhysicsInterpolation) {
    m_currentInterpolationFraction = 0.0;
  }
}

bool KX_KetsjiEngine::SceneUsesSmoothMotion(const KX_Scene *scene) const
{
  return m_enablePhysicsInterpolation || scene->HasSmoothMotion();
}

void KX_KetsjiEngine::RestoreSimulatedPoses()
{
  for (KX_Scene *scene : m_scenes) {
    scene->ClearPhysicsInterpolationState();
  }
}

double KX_KetsjiEngine::GetCurrentInterpolationFraction() const
{
  return m_currentInterpolationFraction;
}

double KX_KetsjiEngine::GetAverageFrameRate()
{
  return m_average_framerate;
}

double KX_KetsjiEngine::GetFrameTime(void) const
{
  return m_clockTime;
}

bool KX_KetsjiEngine::UsesPauseAnimationClock(KX_GameObject *gameobj) const
{
#ifdef WITH_GAMEENGINE_LOGICNODES
  if (m_gamePaused && gameobj != nullptr) {
    KX_Scene *scene = gameobj->GetScene();
    LN_Manager *manager = scene != nullptr ? scene->GetLogicNodeManager() : nullptr;
    return manager != nullptr && manager->IsObjectPauseProcessed(gameobj);
  }
#else
  (void)gameobj;
#endif
  return false;
}

double KX_KetsjiEngine::GetCurrentAnimationTime(KX_GameObject *gameobj) const
{
  /* Logic, physics, and `BL_Action::Update` advance this timeline; it must match `m_starttime` in
   * `BL_Action::Play` so frame ranges are not skipped in a single tick. */
  return UsesPauseAnimationClock(gameobj) ? m_pauseAnimationTime : m_frameTime;
}

double KX_KetsjiEngine::GetRealTime(void) const
{
  return m_previousRealTime;
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

void KX_KetsjiEngine::SetClockTime(double externalClockTime)
{
  const double delta = externalClockTime - m_clockTime;
  m_clockTime = externalClockTime;
  /* Fixed mode: the scheduler admits the time the clock moved on (a jump back of more than a second
   * starts it again). Resetting here used to drop every step, so a game driving its own clock
   * never stepped. */
  if (!m_useFixedPhysicsTimestep) {
    m_previousRealTime += delta;
  }
  m_currentanimsync += delta;
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
      /* Split screen can leave a scene without an active camera (its views are placed per
       * frame, see KX_PlayerViews::Apply). */
      if (KX_Camera *cam = scene->GetActiveCamera()) {
        cam->InvalidateProjectionMatrix();
      }
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
