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

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "CM_Clock.h"
#include "EXP_Python.h"
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
 * State container for Fixed Physics Timestep mode.
 * Contains accumulator and all fixed-mode specific timing variables.
 */
struct FixedPhysicsState {
  /// Accumulator for leftover time between frames
  double accumulator = 0.0;
  /// The fixed timestep duration (1.0 / tickRate)
  double fixedTimestep;
  /// Physics tick rate (Hz) for fixed mode
  int tickRate;
  /// Maximum physics substeps per frame (prevents spiral of death)
  int maxPhysicsStepsPerFrame;
  /// Whether to cap rendering FPS in fixed mode
  bool useFPSCap;
  /// Target FPS cap for rendering
  int fpsCap;
  /// Persistent deadline for precise frame pacing (fixed mode cap only)
  std::chrono::steady_clock::time_point nextFrameDeadline;
  /// Frame start timestamp for deadline pacing
  std::chrono::steady_clock::time_point frameStartSteady;

  FixedPhysicsState(int tickRate = 60, int maxSteps = 5, bool useFPSCap = false, int fpsCap = 60)
      : fixedTimestep(1.0 / static_cast<double>(tickRate)),
        tickRate(tickRate),
        maxPhysicsStepsPerFrame(maxSteps),
        useFPSCap(useFPSCap),
        fpsCap(fpsCap)
  {
  }

  /// Reset accumulator and timing state (called when switching modes or changing settings)
  void Reset()
  {
    accumulator = 0.0;
    nextFrameDeadline = std::chrono::steady_clock::time_point{};
    frameStartSteady = std::chrono::steady_clock::time_point{};
  }

  /// Update tick rate and recalculate fixed timestep
  void SetTickRate(int newTickRate)
  {
    if (newTickRate > 0) {
      tickRate = newTickRate;
      fixedTimestep = 1.0 / static_cast<double>(newTickRate);
      Reset();
    }
  }
};

/**
 * State container for Variable Physics Timestep mode.
 * Currently has no mode-specific state, but exists for symmetry and future extensions.
 */
struct VariablePhysicsState {
  // Variable mode couples physics to logic frames and uses dt directly.
  // No accumulator or additional state needed.
};

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
  struct bContext *m_context;
  bool m_useViewportRender;
  int m_shadingTypeRuntime;
  std::vector<KX_Camera *> m_renderingCameras;
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

  struct FrameTimes {
    // ===== Logic Frame Timing (BOTH MODES) =====
    /// Number of logic frames to execute this render frame
    int frames;
    /// Duration of each logic frame (seconds)
    double timestep;
    /// Scaled duration for logic frame (timestep * m_timescale)
    double framestep;
    
    // ===== Physics Frame Timing (MODE-SPECIFIC) =====
    /// FIXED MODE: Can be 0, 1, 2, 3... (independent of logic frames)
    /// VARIABLE MODE: Always equals 'frames' (physics coupled to logic)
    int physicsFrames;
    /// FIXED MODE: Constant value (1.0 / tickRate)
    /// VARIABLE MODE: Same as 'timestep' (varies with framerate)
    double physicsTimestep;
    
    // ===== Mode Flag (for dispatch logic) =====
    /// True if using fixed physics timestep mode
    bool useFixedPhysicsTimestep;
  };

  CM_Clock m_clock;

  /// Lists of scenes scheduled to be removed at the end of the frame.
  std::vector<std::string> m_removingScenes;
  /// Lists of scenes scheduled to be replaced at the end of the frame.
  std::vector<std::pair<std::string, std::string>> m_replace_scenes;

  /// The current list of scenes.
  EXP_ListValue<KX_Scene> *m_scenes;

  bool m_bInitialized;

  FlagType m_flags;

  /// current logic game time
  double m_frameTime;
  /// game time for the next rendering step
  double m_clockTime;
  /// game time when the animations were last updated
  double m_previousAnimTime;
  double m_remainingTime;
  
  /// Time scaling parameter (BOTH MODES)
  /// Values: < 1.0 = slow motion, 1.0 = realtime, > 1.0 = fast forward
  /// Affects BOTH logic and physics timing in both fixed and variable modes
  double m_timescale;
  
  double m_previousRealTime;
  /// Used to fix deltaTime if there is a large variation in the value
  double m_previous_deltaTime;
  /// Used to control strange behavior in clockTime physics when starting the game
  bool m_firstEngineFrame;

  /// Maximum number of consecutive logic frames (BOTH MODES)
  /// Prevents logic from falling too far behind if frame takes too long
  int m_maxLogicFrame;
  
  /// Logic tick rate in Hz (BOTH MODES)
  /// Controls how often logic updates when FIXED_FRAMERATE flag is set
  /// Note: In fixed physics mode, physics runs at FixedPhysicsState::tickRate,
  ///       which is independent of this logic rate
  double m_ticrate;
  
  /// Animation playback framerate for IPO and actions (BOTH MODES)
  double m_anim_framerate;

  /********** PHYSICS TIMESTEP MODE MANAGEMENT **********/
  /// Mode selection: true = fixed timestep, false = variable timestep
  bool m_useFixedPhysicsTimestep;

  /// Fixed mode state (only allocated and used when m_useFixedPhysicsTimestep is true)
  std::unique_ptr<FixedPhysicsState> m_fixedPhysicsState;

  /// Variable mode state (only allocated and used when m_useFixedPhysicsTimestep is false)
  std::unique_ptr<VariablePhysicsState> m_variablePhysicsState;
  /****************************************************/

  bool m_doRender; /* whether or not the scene should be rendered after the logic frame */

  /// Key used to exit the BGE
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

  /// Time logger.
  KX_TimeCategoryLogger m_logger;

  /// Labels for profiling display.
  static const std::string m_profileLabels[tc_numCategories];
  /// Last estimated framerate
  double m_average_framerate;

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

  /// Update and return the projection matrix of a camera depending on the viewport.
  MT_Matrix4x4 GetCameraProjectionMatrix(KX_Scene *scene,
                                         KX_Camera *cam,
                                         RAS_Rasterizer::StereoEye eye,
                                         const RAS_Rect &viewport,
                                         const RAS_Rect &area) const;
  CameraRenderData GetCameraRenderData(KX_Scene *scene,
                                       KX_Camera *camera,
                                       KX_Camera *overrideCullingCam,
                                       const RAS_Rect &displayArea,
                                       RAS_Rasterizer::StereoEye eye,
                                       bool usestereo);
  /// Compute frame render data per eyes (in case of stereo), scenes and camera.
  bool GetFrameRenderData(std::vector<FrameRenderData> &frameDataList);

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
  
  /********** FIXED PHYSICS MODE FUNCTIONS **********/
  /// Calculate frame timing for fixed physics timestep mode (uses accumulator pattern)
  FrameTimes GetFrameTimesFixed(double dt);
  /// Execute physics for fixed timestep mode (multiple fixed steps per frame)
  void ExecutePhysicsFixed(KX_Scene *scene, const FrameTimes &times, int logicFrameIndex);
  
  /********** VARIABLE PHYSICS MODE FUNCTIONS **********/
  /// Calculate frame timing for variable physics timestep mode (couples physics to framerate)
  FrameTimes GetFrameTimesVariable(double dt);
  /// Execute physics for variable timestep mode (physics follows logic frames)
  void ExecutePhysicsVariable(KX_Scene *scene, const FrameTimes &times, int logicFrameIndex);

 public:
  KX_KetsjiEngine(KX_ISystem *system,
                  struct bContext *C,
                  bool useViewportRender,
                  int shadingTypeRuntime);
  virtual ~KX_KetsjiEngine();

  /******** EEVEE integration *********/
  struct bContext *GetContext();
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

  void GetSceneViewport(KX_Scene *scene,
                        KX_Camera *cam,
                        const RAS_Rect &displayArea,
                        RAS_Rect &area,
                        RAS_Rect &viewport);

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

  /*
   * Returns next render frame game time
   */
  double GetClockTime(void) const;

  /**
   * Set the next render frame game time. It will impact also frame time, as
   * this one is derived from clocktime
   */
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
   * Gets the maximum number of physics steps per frame
   * FIXED MODE: Returns maxPhysicsStepsPerFrame from FixedPhysicsState
   * VARIABLE MODE: Returns m_maxLogicFrame (physics coupled to logic)
   */
  int GetMaxPhysicsFrame();
  /**
   * Sets the maximum number of physics steps per frame
   * FIXED MODE: Sets maxPhysicsStepsPerFrame in FixedPhysicsState
   * VARIABLE MODE: Ignored (uses m_maxLogicFrame)
   */
  void SetMaxPhysicsFrame(int frame);

  /**
   * Gets whether fixed physics timestep is enabled
   */
  bool GetUseFixedPhysicsTimestep();
  /**
   * Sets whether to use fixed physics timestep
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

  /* Fixed-physics render cap */
  bool GetUseFixedFPSCap();
  void SetUseFixedFPSCap(bool useFixed);
  int GetFixedFPSCap();
  void SetFixedFPSCap(int fps);

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

  /**
   * Sets the time scale multiplier
   */
  void SetTimeScale(double scale);

  void SetExitKey(short key);

  short GetExitKey();

  /**
   * Activate or deactivates the render of the scene after the logic frame
   * \param render	true (render) or false (do not render)
   */
  void SetRender(bool render);
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
  KX_Scene *CreateScene(Scene *scene, bool libloading);

  GlobalSettings *GetGlobalSettings(void);
  void SetGlobalSettings(GlobalSettings *gs);

  /**
   * Invalidate all the camera matrices and handle other
   * needed changes when resized.
   * It's only called from Blenderplayer.
   */
  void Resize();
};
