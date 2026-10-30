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
 *
 */

/** \file KX_KetsjiEngine.h
 *  \ingroup ketsji
 */

#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

#include "CM_Clock.h"
#include "EXP_Python.h"
#include "KX_FrameScheduler.h"
#include "KX_ISystem.h"
#include "KX_Scene.h"
#include "KX_TimeCategoryLogger.h"
#include "MT_Matrix4x4.h"
#include "RAS_CameraData.h"
#include "RAS_Rasterizer.h"

class KX_ISystem;
class BL_Converter;
class KX_NetworkMessageManager;
class RAS_ICanvas;
class RAS_FrameBuffer;
class SCA_IInputDevice;
class LN_GameStateLoadPayload;
struct LN_GameStateCompletionTarget;
enum class LN_GameStateResult : uint8_t;

enum class KX_ExitRequest {
  NO_REQUEST = 0,
  QUIT_GAME,
  RESTART_GAME,
  START_OTHER_GAME,
  NO_SCENES_LEFT,
  BLENDER_ESC,
  OUTSIDE,
  MAX
};

enum class KX_DebugOption { DISABLE = 0, FORCE, ALLOW };

typedef struct {
  short glslflag;
} GlobalSettings;


/**
 * KX_KetsjiEngine is the core game engine class.
 */
class KX_KetsjiEngine {
 public:
  enum FlagType {
    FLAG_NONE = 0,
    /// Show profiling info on the game display?
    SHOW_PROFILE = (1 << 0),
    /// Show the framerate on the game display?
    SHOW_FRAMERATE = (1 << 1),
    /// Show debug properties on the game display.
    SHOW_DEBUG_PROPERTIES = (1 << 2),
    /// Whether or not to lock animation updates to the animation framerate?
    RESTRICT_ANIMATION = (1 << 3),
    /// Display of fixed frames?
    FIXED_FRAMERATE = (1 << 4),
    /// BGE relies on a external clock or its own internal clock?
    USE_EXTERNAL_CLOCK = (1 << 5),
    /// Automatic add debug properties to the debug list.
    AUTO_ADD_DEBUG_PROPERTIES = (1 << 6),
    /// Use override camera?
    CAMERA_OVERRIDE = (1 << 7)
  };

  static constexpr int FRAME_TIME_GRAPH_SLOT_COUNT = 4;
  static constexpr int FRAME_TIME_GRAPH_CATEGORY_COUNT = 11;
  /**
   * Hard upper bound for catch-up work accepted from scene settings and the Python API.
   * This matches the RNA range and prevents malformed runtime input from turning spiral-of-death
   * protection into an unbounded main-thread loop.
  */
  static constexpr int MAX_SCHEDULED_STEPS = 10000;

  static constexpr int ClampScheduledStepLimit(const int steps)
  {
    return steps < 1 ? 1 : (steps > MAX_SCHEDULED_STEPS ? MAX_SCHEDULED_STEPS : steps);
  }

 private:
  struct CameraRenderData {
    CameraRenderData(KX_Camera *rendercam,
                     KX_Camera *cullingcam,
                     const RAS_Rect &area,
                     const RAS_Rect &viewport,
                     RAS_Rasterizer::StereoEye eye);
    CameraRenderData(const CameraRenderData &other);
    ~CameraRenderData();

    /// Rendered camera, could be a temporary camera in case of stereo.
    KX_Camera *m_renderCamera;
    KX_Camera *m_cullingCamera;
    RAS_Rect m_area;
    RAS_Rect m_viewport;
    RAS_Rasterizer::StereoEye m_eye;
  };

  struct SceneRenderData {
    SceneRenderData(KX_Scene *scene);

    KX_Scene *m_scene;
    std::vector<CameraRenderData> m_cameraDataList;
  };

  /// Data used to render a frame.
  struct FrameRenderData {
    FrameRenderData(RAS_Rasterizer::FrameBufferType fbType);

    RAS_Rasterizer::FrameBufferType m_fbType;
    std::vector<SceneRenderData> m_sceneDataList;
  };

  /***************EEVEE INTEGRATION*****************/
  blender::bContext *m_context;
  bool m_useViewportRender;
  /** The game can move the mouse cursor (not a shown cursor on Wayland). */
  bool m_cursorMovable = true;
  /** Wayland: a program can move the cursor only while it holds a hidden grab of the pointer, so
   * the grab follows the cursor being shown or hidden (CursorShown). */
  bool m_cursorMovesOnlyHidden = false;
  int m_shadingTypeRuntime;
  std::vector<KX_Camera *> m_renderingCameras;
  /** Counts drawn frames (Render): the camera passes of one frame see the same poses. */
  uint64_t m_renderFrame = 0;
  /*************************************************/

  /// 2D Canvas (2D Rendering Device Context)
  RAS_ICanvas *m_canvas;
  /// 3D Rasterizer (3D Rendering)
  RAS_Rasterizer *m_rasterizer;
  KX_ISystem *m_kxsystem;
  BL_Converter *m_converter;
  KX_NetworkMessageManager *m_networkMessageManager;
#ifdef WITH_PYTHON
  PyObject *m_pyprofiledict;
#endif
  SCA_IInputDevice *m_inputDevice;

  struct FrameTimeGraphSample {
    float timeline_seconds = 0.0f;
    float raw_delta_seconds = 0.0f;
    float raw_frame_ms = 0.0f;
    float engine_delta_seconds = 0.0f;
    float engine_frame_ms = 0.0f;
    std::array<float, FRAME_TIME_GRAPH_CATEGORY_COUNT> category_ms = {};
    /** Game time the frame moved on since the frame before (its steps, the Steps domain). */
    float game_advance_ms = 0.0f;
  };

  struct FrameTimes {
    // ===== Logic Frame Timing (BOTH MODES) =====
    /// Number of logic frames to execute this render frame
    int frames;
    /// Duration of each logic frame (seconds)
    double timestep;
    /// Scaled duration for logic frame (timestep * m_timescale)
    double framestep;

    /// Wall-clock time consumed by this outer/render frame.
    double realDelta;
    /// Scaled game time admitted to the fixed accumulator for this outer frame.
    double scaledDelta;
    /// Time scale snapshot used to schedule this frame.
    double schedulingTimeScale;
    
    // ===== Physics Frame Timing (MODE-SPECIFIC) =====
    /// FIXED MODE: Can be 0, 1, 2, 3... (independent of logic frames)
    /// VARIABLE MODE: Always equals 'frames' (physics coupled to logic)
    int physicsFrames;
    /// FIXED MODE: Constant value (1.0 / tickRate)
    /// VARIABLE MODE: Same as 'timestep' (varies with framerate)
    double physicsTimestep;
    /// Approximate wall-clock duration represented by one fixed simulation step.
    double physicsUnscaledTimestep;
    /// FIXED MODE: Fractional progress between last completed physics step and visual frame
    /// VARIABLE MODE: Always 0.0 (no interpolation needed)
    double interpolationFraction;

    // ===== Mode Flag (for dispatch logic) =====
    /// True if using fixed physics timestep mode
    bool useFixedPhysicsTimestep;
  };

  CM_Clock m_clock;

  /// Lists of scenes scheduled to be removed at the end of the frame.
  std::vector<std::string> m_removingScenes;
  /// Lists of scenes scheduled to be replaced at the end of the frame.
  std::vector<std::pair<std::string, std::string>> m_replace_scenes;
  struct PendingGameLoad;
  std::unique_ptr<PendingGameLoad> m_pendingGameLoad;

  /// The current list of scenes.
  EXP_ListValue<KX_Scene> *m_scenes;

  bool m_bInitialized;

  FlagType m_flags;

  /// current logic game time
  double m_frameTime;
  /** Unscaled visual-animation clock used only by objects selected by While Paused. */
  double m_pauseAnimationTime;
  /// game time for the next rendering step
  double m_clockTime;
  /// game time when the animations were last updated
  double m_previousAnimTime;
  double m_previousPauseAnimTime;
  double m_remainingTime;
  /** User-configured game-time multiplier. Game pause never overwrites this value. */
  double m_timescale;
  /** Stops game-time scheduling while keeping the real-time update path alive. */
  bool m_gamePaused;
  /** Stops physics simulation while allowing game-time logic to continue. */
  bool m_physicsPaused;
  /** True while every scene is completing one shared fixed simulation step. */
  bool m_fixedStepInProgress;
  /** True while every scene is completing one shared render-frame update. */
  bool m_frameUpdateInProgress;
  bool m_frameUpdateRan = false;
  /** Pause changes requested during a scene update phase are committed at its atomic boundary. */
  bool m_hasDeferredGamePaused;
  bool m_deferredGamePaused;
  bool m_hasDeferredPhysicsPaused;
  bool m_deferredPhysicsPaused;
  double m_lastDisplayedTime;
  double m_average_framerate;
  bool m_forceInterpolationRender;
  bool m_cameraOverrideActive;
  double m_previousRealTime;
  /// Used to fix deltaTime if there is a large variation in the value
  double m_previous_deltaTime;
  /// Used to control strange behavior in clockTime physics when starting the game
  bool m_firstEngineFrame;

  int m_recordFrameTimeGraphSlot;
  int m_showRecordedFrameTimeGraphSlots;
  int m_frameTimeGraphWindowSeconds;
  int m_frameTimeGraphAxis;
  int m_frameTimeGraphStyle;
  int m_frameTimeGraphVisibleDomains;
  int m_frameTimeGraphMaxSamples;
  int m_frameTimeGraphTimelineFrame;
  double m_frameTimeGraphTimelineSeconds;
  bool m_frameTimeGraphSamplePending;
  bool m_frameTimeGraphWorkTimerRunning;
  double m_frameTimeGraphPendingRawDelta;
  double m_frameTimeGraphPendingWorkSeconds;
  double m_frameTimeGraphWorkStartTime;
  /** Game time at the last graph sample (for the Steps domain); none while not sampling. */
  bool m_frameTimeGraphHasGameTime = false;
  double m_frameTimeGraphLastGameTime = 0.0;
  /** Steps of the frames drawn in the last second, for the overlay's Steps/frame line. */
  KX_FrameStepCounts m_frameStepCounts;
  std::deque<FrameTimeGraphSample> m_liveFrameTimeGraph;
  std::array<std::deque<FrameTimeGraphSample>, FRAME_TIME_GRAPH_SLOT_COUNT>
      m_recordedFrameTimeGraphs;
  static std::array<std::deque<FrameTimeGraphSample>, FRAME_TIME_GRAPH_SLOT_COUNT>
      m_sharedFrameTimeGraphSlots;

  double m_previousFrameTime;
  double m_currentanimsync;
  double m_anim_framerate;

  /********** PHYSICS TIMESTEP MODE MANAGEMENT **********/
  /// Mode selection: true = fixed timestep, false = variable timestep
  bool m_useFixedPhysicsTimestep;
  bool m_enablePhysicsInterpolation;
  double m_currentInterpolationFraction;

  /// Logic update rate (Hz) - used by variable mode and FIXED_FRAMERATE flag
  double m_ticrate;
  /// Maximum logic frames per render frame - used by variable mode
  int m_maxLogicFrame;

  /// Fixed physics mode state (only used when m_useFixedPhysicsTimestep == true)
  double m_fixedPhysicsTimestep;
  int m_physicsTickRate;
  int m_maxPhysicsSteps;
  bool m_useFixedFPSCap;
  int m_fixedRenderCapRate;
  /** The screen paces frames (vsync on or adaptive). */
  bool m_vsyncPacing;
  /** How many steps frame times may run off the clock while vsync paces (0: off). */
  double m_jitterFix;
  /** Plans each pass: frame or not, its frame time, the fixed steps due by it. */
  KX_FrameScheduler m_frameScheduler;
  /** Real/scaled time accumulated since the previous render-frame Logic Nodes update. */
  double m_pendingUpdateRealDelta;
  double m_pendingUpdateScaledDelta;
  /** The clock at the previous frame pass (the frame-time graph shows real frame times). */
  double m_lastFramePassClock;
  /** Cooperative launcher sleep after a scheduler pass that did not render. */
  double m_suggestedIdleSeconds;
  bool m_interpolationInitialized;
  /*****************************************************/

  bool m_doRender; /* whether or not the scene should be rendered after the logic frame */

  /// blender::Key used to exit the BGE
  short m_exitkey;

  KX_ExitRequest m_exitcode;
  std::string m_exitstring;

  float m_cameraZoom;

  std::string m_overrideSceneName;
  RAS_CameraData m_overrideCamData;
  MT_Matrix4x4 m_overrideCamProjMat;
  MT_Matrix4x4 m_overrideCamViewMat;
  /// Default camera zoom.
  float m_overrideCamZoom;

  /// Categories for profiling display.
  typedef enum {
    tc_first = 0,
    tc_physics = 0,
    tc_logic,
    tc_animations,
    tc_depsgraph,
    tc_network,
    tc_scenegraph,
    tc_rasterizer,
    tc_services,  // time spent in miscelaneous activities
    tc_overhead,  // profile info drawing overhead
    tc_outside,   // time spent outside main loop
    tc_latency,   // time spent waiting on the gpu
    tc_numCategories
  } KX_TimeCategory;

  /// Labels for profiling display.
  static const std::string m_profileLabels[tc_numCategories];

  /// Time logger.
  KX_TimeCategoryLogger m_logger;

  /// Enable debug draw of culling bounding boxes.
  KX_DebugOption m_showBoundingBox;
  /// Enable debug draw armatures.
  KX_DebugOption m_showArmature;
  /// Enable debug draw of camera frustum.
  KX_DebugOption m_showCameraFrustum;
  /// Enable debug light shadow frustum.
  KX_DebugOption m_showShadowFrustum;

  /// Settings that doesn't go away with Game Actuator
  GlobalSettings m_globalsettings;

  CameraRenderData GetCameraRenderData(KX_Scene *scene,
                                       KX_Camera *camera,
                                       KX_Camera *overrideCullingCam,
                                       const RAS_Rect &displayArea,
                                       RAS_Rasterizer::StereoEye eye,
                                       bool usestereo);
  /// Compute frame render data per eyes (in case of stereo), scenes and camera.
  bool GetFrameRenderData(std::vector<FrameRenderData> &frameDataList);
  int GetFrameTimeGraphTargetFPS() const;
  int GetFrameTimeGraphSampleFPS() const;
  int GetFrameTimeGraphWindowSeconds() const;
  int GetFrameTimeGraphFrameWindow() const;
  int GetFrameTimeGraphVisibleDomains() const;
  int GetFrameTimeGraphMaxSamples() const;
  float GetFrameTimeGraphDomainValue(const FrameTimeGraphSample &sample, int domain) const;
  bool IsFrameTimeGraphSamplingEnabled() const;
  void BeginFrameTimeGraphSample(double raw_delta_seconds);
  void PauseFrameTimeGraphWorkTimer();
  void ResumeFrameTimeGraphWorkTimer();
  void FinishFrameTimeGraphSample();
  void StoreFrameTimeGraphSample(double raw_delta_seconds, double engine_delta_seconds);
  void RenderFrameTimeGraph(RAS_DebugDraw &debugDraw,
                            int xcoord,
                            int ycoord,
                            int profile_indent,
                            short profile_size) const;

  /// EEVEE scene rendering
  void RenderCamera(KX_Scene *scene, class RAS_FrameBuffer *background_fb, const CameraRenderData &cameraFrameData, unsigned short pass);
  void RenderDebugProperties();
  /// Debug draw cameras frustum of a scene.
  void DrawDebugCameraFrustum(KX_Scene *scene,
                              RAS_DebugDraw &debugDraw,
                              const CameraRenderData &cameraFrameData);
  /// Debug draw lights shadow frustum of a scene.
  void DrawDebugShadowFrustum(KX_Scene *scene, RAS_DebugDraw &debugDraw);

  /**
   * Processes all scheduled scene activity.
   * At the end, if the scene lists have changed,
   * SceneListsChanged(void) is called.
   * \see SceneListsChanged(void).
   */
  void ProcessScheduledScenes(void);

  /**
   * This method is invoked when the scene lists have changed.
   */
  void RemoveScheduledScenes(void);
  void ReplaceScheduledScenes(void);
  void PostProcessScene(KX_Scene *scene);

  void BeginFrame();
  FrameTimes GetFrameTimes();
  /** The time scale admitted to simulation scheduling after applying game pause. */
  double GetSchedulingTimeScale() const;
  
  /********** SHARED FRAME HELPERS **********/
  /// Process input devices and joystick events
  void ProcessInputDevices();
  /** Split screen: moves the mouse cursor back into player 0's view (Set Split Screen's Keep
   * Mouse in Player 0's View), except while the game is paused. */
  void KeepMouseInPlayerView();
  /// Process the real/render-frame Logic Nodes update phase for a scene.
  void ProcessSceneUpdate(KX_Scene *scene, const FrameTimes &times);
  /// Process logic updates for a scene
  void ProcessSceneLogic(KX_Scene *scene, const FrameTimes &times, int frameIndex);
  /// Finalize frame (network, input clear, scene management)
  void FinalizeFrame(bool clearInputs = true, bool clearNetwork = true);
  /** Restore authoritative SG poses and require fresh fixed-step interpolation history. */
  void InvalidatePhysicsInterpolationState();
  void ResetFixedPhysicsTiming();
  /** Commit the final pause values requested during the current atomic update phase. */
  void ApplyDeferredPauseState();

  /// Logic frame timing calculation result
  struct LogicFrameTiming {
    int frames;        // Number of logic frames to execute
    double timestep;   // Duration of each logic frame
  };
  
  /// Calculate logic frame timing based on FIXED_FRAMERATE flag
  /// This is shared by both physics modes - logic timing is independent of physics timing
  /// @param dt Delta time since last frame
  /// @param logicRate Target logic update rate (Hz) when FIXED_FRAMERATE is enabled
  /// @param maxLogicFrames Maximum logic frames per render frame
  /// @return Calculated logic timing parameters
  LogicFrameTiming CalculateLogicFrameTiming(double dt, double logicRate, int maxLogicFrames);
  
  /********** FIXED PHYSICS MODE FUNCTIONS **********/
  /** The frame scheduler's settings from the engine's (step, FPS Limit, vsync, jitter fix). */
  void ConfigureFrameScheduler();
  /** Frame timing of a planned fixed-mode pass. */
  FrameTimes FrameTimesOfPass(const KX_FrameScheduler::Pass &pass) const;
  /// Execute physics for fixed timestep mode (multiple fixed steps per frame)
  void ExecutePhysicsFixed(KX_Scene *scene,
                           const FrameTimes &times,
                           int logicFrameIndex,
                           bool physicsStepEnabled);
  /** One fixed step for every scene: logic, physics and parents. The unit a network clock could
   * run again (prediction); returns false when a pause asked for during it stops the pass. */
  bool RunFixedTick(const FrameTimes &times, int stepIndex);
  /// Execute one complete pass in fixed physics mode, drawing a frame when `renderUpdateDue`.
  bool NextFrameFixed(const FrameTimes &times, bool renderUpdateDue);
  
  /********** VARIABLE PHYSICS MODE FUNCTIONS **********/
  /// Calculate frame timing for variable physics timestep mode (couples physics to framerate)
  FrameTimes GetFrameTimesVariable(double dt);
  /// Execute physics for variable timestep mode (physics follows logic frames)
  void ExecutePhysicsVariable(KX_Scene *scene, const FrameTimes &times, int logicFrameIndex);
  /// Execute one complete frame in variable physics mode (no FPS cap)
  bool NextFrameVariable(const FrameTimes &times);

public:
  KX_KetsjiEngine(KX_ISystem *system,
                  blender::bContext *C,
                  bool useViewportRender,
                  int shadingTypeRuntime);
  virtual ~KX_KetsjiEngine();
  void SetPhysicsInterpolationEnabled(bool enable);
  /** The scene's Physics Interpolation: the Smooth Motion of objects left at Scene Default. */
  bool IsPhysicsInterpolationEnabled() const
  {
    return m_enablePhysicsInterpolation;
  }
  /** Whether a scene draws objects with Smooth Motion: by the Physics Interpolation, or because
   * an object chose it. Scenes that don't spend nothing on it. */
  bool SceneUsesSmoothMotion(const KX_Scene *scene) const;
  /** After a frame is drawn: objects drawn between their steps get their simulated poses back,
   * so what runs before the next pass (a Python main loop) reads and moves the real ones. */
  void RestoreSimulatedPoses();

  void ForceInterpolationRender();
  void ClearForcedInterpolation();
  bool ShouldForceInterpolationRender() const
  {
    return m_forceInterpolationRender;
  }

  double GetCurrentInterpolationFraction() const;

  /******** EEVEE integration *********/
  blender::bContext *GetContext();
  /** The frame being drawn, never 0 while drawing (KX_GameObject::TagForTransformUpdate compares
   * an object's pose once a frame). */
  uint64_t GetRenderFrame() const
  {
    return m_renderFrame;
  }
  bool UseViewportRender();
  int ShadingTypeRuntime();
  // include depsgraph time in tc_depsgraph category
  void CountDepsgraphTime();
  void EndCountDepsgraphTime();
  void EndFrameViewportRender();
  std::vector<KX_Camera *> GetRenderingCameras();
  /***** End of EEVEE integration *****/

  void EndFrame();

  RAS_FrameBuffer *PostRenderScene(KX_Scene *scene,
                                   RAS_FrameBuffer *inputfb,
                                   RAS_FrameBuffer *targetfb);

  /// set the devices and stuff. the client must take care of creating these
  void SetInputDevice(SCA_IInputDevice *inputDevice);
  void SetCanvas(RAS_ICanvas *canvas);
  void SetRasterizer(RAS_Rasterizer *rasterizer);
  void SetNetworkMessageManager(KX_NetworkMessageManager *manager);
#ifdef WITH_PYTHON
  PyObject *GetPyProfileDict();
#endif
  void SetConverter(BL_Converter *converter);
  BL_Converter *GetConverter()
  {
    return m_converter;
  }

  RAS_Rasterizer *GetRasterizer()
  {
    return m_rasterizer;
  }
  RAS_ICanvas *GetCanvas()
  {
    return m_canvas;
  }
  SCA_IInputDevice *GetInputDevice()
  {
    return m_inputDevice;
  }
  KX_NetworkMessageManager *GetNetworkMessageManager() const
  {
    return m_networkMessageManager;
  }

  /// returns true if an update happened to indicate -> Render
  bool NextFrame();
  /** The last NextFrame ran a render-frame update, where UI can use keys such as Esc. */
  bool FrameUpdateRan() const
  {
    return m_frameUpdateRan;
  }
  void Render();

  void StartEngine();
  void StopEngine();

  void RequestExit(KX_ExitRequest exitrequestmode);
  void SetNameNextGame(const std::string &nextgame);
  KX_ExitRequest GetExitCode();
  const std::string &GetExitString();

  EXP_ListValue<KX_Scene> *CurrentScenes();
  KX_Scene *FindScene(const std::string &scenename);
  void AddScene(KX_Scene *scene);

  void RemoveScene(const std::string &scenename);
  bool ReplaceScene(const std::string &oldscene, const std::string &newscene);
  LN_GameStateResult RequestGameLoadSceneTransition(
      KX_Scene &source_scene,
      const LN_GameStateCompletionTarget &target,
      const std::string &filepath,
      std::unique_ptr<LN_GameStateLoadPayload> payload,
      std::string &r_error);

  void GetSceneViewport(KX_Scene *scene,
                        KX_Camera *cam,
                        const RAS_Rect &displayArea,
                        RAS_Rect &area,
                        RAS_Rect &viewport);
  /// Update and return the projection matrix of a camera depending on the viewport.
  MT_Matrix4x4 GetCameraProjectionMatrix(KX_Scene *scene,
                                         KX_Camera *cam,
                                         RAS_Rasterizer::StereoEye eye,
                                         const RAS_Rect &viewport,
                                         const RAS_Rect &area) const;

  /// Sets zoom for camera objects, useful only with extend and scale framing mode.
  void SetCameraZoom(float camzoom);
  /// Sets zoom for default camera, = 2 in embedded mode.
  void SetCameraOverrideZoom(float camzoom);
  /// Get the camera zoom for the passed camera.
  float GetCameraZoom(KX_Camera *camera) const;

  void EnableCameraOverride(const std::string &forscene,
                            const MT_Matrix4x4 &projmat,
                            const MT_Matrix4x4 &viewmat,
                            const RAS_CameraData &camdata);

  // Update animations for object in this scene
  void UpdateAnimations(KX_Scene *scene);

  bool GetFlag(FlagType flag) const;
  /// Enable or disable a set of flags.
  void SetFlag(FlagType flag, bool enable);
  void SetFrameTimeGraphSettings(int window_seconds,
                                 int axis,
                                 int style,
                                 int max_samples,
                                 int visible_domains);
  void SetFrameTimeGraphSlots(int record_slot, int visible_slots);

  double GetClockTime(void) const;
  void SetClockTime(double externalClockTime);

  /**
   * Returns current logic frame game time
   */
  double GetFrameTime(void) const;

  /**
   * Returns the real (system) time
   */
  double GetRealTime(void) const;

  /**
   * Gets the number of logic updates per second.
   */
  double GetTicRate();
  /**
   * Sets the number of logic updates per second.
   */
  void SetTicRate(double ticrate);
  /**
   * Gets the maximum number of logic frame before render frame
   */
  int GetMaxLogicFrame();
  /**
   * Sets the maximum number of logic frame before render frame
   */
  void SetMaxLogicFrame(int frame);
  /**
   * Gets the maximum number of physics steps per frame.
   * In fixed mode, returns the fixed physics max steps.
   * In variable mode, returns max logic frames (physics coupled to logic).
   */
  int GetMaxPhysicsFrame();
  /**
   * Sets the maximum number of physics steps per frame.
   * Only affects fixed physics mode.
   */
  void SetMaxPhysicsFrame(int frame);

  /**
   * Gets whether fixed physics timestep is enabled.
   */
  bool GetUseFixedPhysicsTimestep();
  /**
   * Sets whether to use fixed physics timestep.
   */
  void SetUseFixedPhysicsTimestep(bool useFixed);
  /**
   * Gets the physics tick rate for fixed timestep mode
   */
  int GetPhysicsTickRate();
  /**
   * Sets the physics tick rate for fixed timestep mode
   */
  void SetPhysicsTickRate(int tickRate);

  /**
   * Gets whether render FPS capping is enabled in fixed mode.
   */
  bool GetUseFixedFPSCap();
  /**
   * Enables or disables render FPS capping in fixed mode.
   */
  void SetUseFixedFPSCap(bool useFixed);

  /**
   * Gets the render cap rate for fixed physics mode
   */
  int GetFixedRenderCapRate();
  /**
   * Sets the render cap rate for fixed physics mode
   */
  void SetFixedRenderCapRate(int rate);

  /**
   * Suggested launcher sleep after a non-rendering scheduler pass. The value is always bounded by
   * the next fixed simulation step, so a low render cap cannot increase gameplay input latency.
   */
  double GetSuggestedIdleSeconds() const
  {
    return m_suggestedIdleSeconds;
  }

  /** The screen paces frames: vsync on or adaptive (frame times follow its refresh). */
  void SetVsyncPacing(bool vsync);
  /** How many steps frame times may run off the clock while vsync paces (0 turns it off). */
  void SetJitterFix(double steps);
  double GetJitterFix() const;
  /** Fixed steps run since the game started (each step's number, for a network layer). */
  uint64_t GetSimulationTick() const;

  /** How the last frames were paced (bge.logic.getFramePacingInfo). */
  struct FramePacingInfo {
    /** Fixed steps run before the last frame was drawn. */
    int frame_steps = 0;
    uint64_t tick = 0;
    /** Share of a step since the last step at the last frame's time. */
    double interpolation = 0.0;
    /** The last frame's time on the engine clock, and how far it was from the clock. */
    double frame_time = 0.0;
    double clock_offset = 0.0;
    /** What paces frames: "LIMIT", "VSYNC", "FREE", or "VARIABLE" without fixed steps. */
    const char *pacing = "VARIABLE";
    /** The FPS Limit (0 when off) and the measured refresh rate (0 while unknown). */
    double frame_limit = 0.0;
    double refresh_rate = 0.0;
  };
  FramePacingInfo GetFramePacingInfo() const;

  /**
   * Gets the framerate for playing animations. (actions and ipos)
   */
  double GetAnimFrameRate();
  /**
   * Sets the framerate for playing animations. (actions and ipos)
   */
  void SetAnimFrameRate(double framerate);

  /**
   * Gets the last estimated average framerate
   */
  double GetAverageFrameRate();

  /**
   * Gets the time scale multiplier
   */
  double GetTimeScale() const;
  void SetTimeScale(double scale);

  /**
   * Pause or resume game time without losing the configured time scale.
   * While paused, per-render-frame updates continue but fixed simulation steps do not run.
   */
  bool IsGamePaused() const;
  /** Paused at the end of this update: a pause or resume asked for during it already counts. */
  bool IsGamePauseRequested() const;
  void SetGamePaused(bool paused);

  /**
   * Pause or resume physics only. Fixed logic continues and scheduled simulation time is
   * consumed, so resuming never causes a catch-up burst.
   */
  bool IsPhysicsPaused() const;
  void SetPhysicsPaused(bool paused);

  void SetExitKey(short key);
  short GetExitKey();

  /**
   * Activate or deactivates the render of the scene after the logic frame
   * \param render	true (render) or false (do not render)
   */
  void SetRender(bool render);
  /** `moves_only_hidden`: Wayland, where only a cursor grabbed hidden can be moved; `movable`:
   * the cursor's grab at start allows it. */
  void SetCursorMovable(const bool movable, const bool moves_only_hidden)
  {
    m_cursorMovable = movable;
    m_cursorMovesOnlyHidden = moves_only_hidden;
  }
  /** Whether the game can move the mouse cursor now (Mouse Look holds it in the middle). */
  bool IsCursorMovable() const
  {
    return m_cursorMovable;
  }
  /** The canvas showed or hid the cursor (RAS_ICanvas::SetMouseState): on Wayland the pointer is
   * grabbed hidden while hidden (so it can be moved), and normally while shown. */
  void CursorShown(bool shown);
  /**
   * Get the current render flag value
   */
  bool GetRender();

  /// Allow debug bounding box debug.
  void SetShowBoundingBox(KX_DebugOption mode);
  /// Returns the current setting for bounding box debug.
  KX_DebugOption GetShowBoundingBox() const;

  /// Allow debug armatures.
  void SetShowArmatures(KX_DebugOption mode);
  /// Returns the current setting for armatures debug.
  KX_DebugOption GetShowArmatures() const;

  /// Allow debug camera frustum.
  void SetShowCameraFrustum(KX_DebugOption mode);
  /// Returns the current setting for camera frustum debug.
  KX_DebugOption GetShowCameraFrustum() const;

  /// Allow debug light shadow frustum.
  void SetShowShadowFrustum(KX_DebugOption mode);
  /// Returns the current setting for light shadow frustum debug.
  KX_DebugOption GetShowShadowFrustum() const;

  KX_Scene *CreateScene(const std::string &scenename);
  /** `merge_source`: built only to be merged into another scene (KX_Scene::IsMergeSource). */
  KX_Scene *CreateScene(blender::Scene *scene, bool libloading, bool merge_source = false);

  enum class SceneMergeResult { Merged, NotFound, Failed };
  /** Load Scene: converts the Blender scene `scenename` and merges its objects into `target`,
   * which then draws them as its own; the editor's window is left as it was. */
  SceneMergeResult MergeScene(KX_Scene *target, const std::string &scenename);

  GlobalSettings *GetGlobalSettings(void);
  void SetGlobalSettings(GlobalSettings *gs);

  bool UsesPauseAnimationClock(KX_GameObject *gameobj) const;
  double GetCurrentAnimationTime(KX_GameObject *gameobj = nullptr) const;
  KX_Scene *GetCurrentScene() const;

  /**
   * Invalidate all the camera matrices and handle other
   * needed changes when resized.
   * It's only called from Blenderplayer.
   */
  void Resize();
};
