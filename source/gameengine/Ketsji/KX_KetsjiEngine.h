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

#ifndef __KX_KETSJIENGINE_H__
#define __KX_KETSJIENGINE_H__

#include "MT_Matrix4x4.h"
#include <string>
#include "KX_ISystem.h"
#include "KX_Scene.h"
#include "KX_TimeCategoryLogger.h"
#include "EXP_Python.h"
#include "RAS_CameraData.h"
#include "RAS_Rasterizer.h"
#include <vector>

struct TaskScheduler;
class KX_ISystem;
class KX_BlenderConverter;
class KX_NetworkMessageManager;
class RAS_ICanvas;
class RAS_FrameBuffer;
class SCA_IInputDevice;
struct EEVEE_ViewLayerData;

enum class KX_ExitRequest
{
	NO_REQUEST = 0,
	QUIT_GAME,
	RESTART_GAME,
	START_OTHER_GAME,
	NO_SCENES_LEFT,
	BLENDER_ESC,
	OUTSIDE,
	MAX
};

enum class KX_DebugOption
{
	DISABLE = 0,
	FORCE,
	ALLOW
};

typedef struct {
	short glslflag;
} GlobalSettings;

/**
 * KX_KetsjiEngine is the core game engine class.
 */
class KX_KetsjiEngine
{
public:
	enum FlagType
	{
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

	struct CameraRenderData
	{
		CameraRenderData(KX_Camera *rendercam, KX_Camera *cullingcam, const RAS_Rect& area, const RAS_Rect& viewport, RAS_Rasterizer::StereoEye eye);
		CameraRenderData(const CameraRenderData& other);
		~CameraRenderData();

		/// Rendered camera, could be a temporary camera in case of stereo.
		KX_Camera *m_renderCamera;
		KX_Camera *m_cullingCamera;
		RAS_Rect m_area;
		RAS_Rect m_viewport;
		RAS_Rasterizer::StereoEye m_eye;
	};

	struct SceneRenderData
	{
		SceneRenderData(KX_Scene *scene);

		KX_Scene *m_scene;
		std::vector<CameraRenderData> m_cameraDataList;
	};

	/// Data used to render a frame.
	struct FrameRenderData
	{
		FrameRenderData(RAS_Rasterizer::FrameBufferType fbType);

		RAS_Rasterizer::FrameBufferType m_fbType;
		std::vector<SceneRenderData> m_sceneDataList;
	};

	/// 2D Canvas (2D Rendering Device Context)
	RAS_ICanvas *m_canvas;
	/// 3D Rasterizer (3D Rendering)
	RAS_Rasterizer *m_rasterizer;
	KX_ISystem *m_kxsystem;
	KX_BlenderConverter *m_converter;
	KX_NetworkMessageManager *m_networkMessageManager;
#ifdef WITH_PYTHON
	PyObject *m_pyprofiledict;
#endif
	SCA_IInputDevice *m_inputDevice;

	/// Lists of scenes scheduled to be removed at the end of the frame.
	std::vector<std::string> m_removingScenes;
	/// Lists of overley scenes scheduled to be added at the end of the frame.
	std::vector<std::string> m_addingOverlayScenes;
	/// Lists of background scenes scheduled to be added at the end of the frame.
	std::vector<std::string> m_addingBackgroundScenes;
	/// Lists of scenes scheduled to be replaced at the end of the frame.
	std::vector<std::pair<std::string, std::string> >  m_replace_scenes;

	/// The current list of scenes.
	CListValue<KX_Scene> *m_scenes;

	bool m_bInitialized;

	FlagType m_flags;

	/// current logic game time
	double m_frameTime;
	/// game time for the next rendering step
	double m_clockTime;
	///game time when the animations were last updated
	double m_previousAnimTime;
	double m_remainingTime;
	/// time scaling parameter. if > 1.0, time goes faster than real-time. If < 1.0, times goes slower than real-time.
	double m_timescale;
	double m_previousRealTime;

	/// maximum number of consecutive logic frame
	int m_maxLogicFrame;
	/// maximum number of consecutive physics frame
	int m_maxPhysicsFrame;
	double m_ticrate;
	/// for animation playback only - ipo and action
	double m_anim_framerate;

	bool m_doRender;  /* whether or not the scene should be rendered after the logic frame */

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
		tc_network,
		tc_scenegraph,
		tc_rasterizer,
		tc_services, // time spent in miscelaneous activities
		tc_overhead, // profile info drawing overhead
		tc_outside, // time spent outside main loop
		tc_latency, // time spent waiting on the gpu
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

	/// Task scheduler for multi-threading
	TaskScheduler *m_taskscheduler;

	/** Set scene's total pause duration for animations process.
	 * This is done in a separate loop to get the proper state of each scenes.
	 * eg: There's 2 scenes, the first is suspended and the second is active.
	 * If the second scene resume the first, the first scene will be not proceed
	 * in 'NextFrame' for one frame, but set as active.
	 * The render functions, called after and which update animations,
	 * will see the first scene as active and will proceed to it,
	 * but it will cause some negative current frame on actions because of the
	 * total pause duration not set.
	 */
	void UpdateSuspendedScenes(double framestep);

	/// Update and return the projection matrix of a camera depending on the viewport.
	MT_Matrix4x4 GetCameraProjectionMatrix(KX_Scene *scene, KX_Camera *cam, RAS_Rasterizer::StereoEye eye,
										   const RAS_Rect& viewport, const RAS_Rect& area) const;
	CameraRenderData GetCameraRenderData(KX_Scene *scene, KX_Camera *camera, KX_Camera *overrideCullingCam, const RAS_Rect& displayArea,
											  RAS_Rasterizer::StereoEye eye, bool usestereo);
	/// Compute frame render data per eyes (in case of stereo), scenes and camera.
	bool GetFrameRenderData(std::vector<FrameRenderData>& frameDataList);

	/// EEVEE scene rendering
	void RenderCamera(KX_Scene *scene, const CameraRenderData& cameraFrameData, unsigned short pass);
	void RenderDebugProperties();
	/// Debug draw cameras frustum of a scene.
	void DrawDebugCameraFrustum(KX_Scene *scene, RAS_DebugDraw& debugDraw, const CameraRenderData& cameraFrameData);
	/// Debug draw lights shadow frustum of a scene.
	void DrawDebugShadowFrustum(KX_Scene *scene, RAS_DebugDraw& debugDraw);

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
	void AddScheduledScenes(void);
	void ReplaceScheduledScenes(void);
	void PostProcessScene(KX_Scene *scene);

	void BeginFrame();
	

public:
	KX_KetsjiEngine(KX_ISystem *system);
	virtual ~KX_KetsjiEngine();

	void EndFrame();

	RAS_FrameBuffer *PostRenderScene(KX_Scene *scene, RAS_FrameBuffer *inputfb, RAS_FrameBuffer *targetfb);

	/// set the devices and stuff. the client must take care of creating these
	void SetInputDevice(SCA_IInputDevice *inputDevice);
	void SetCanvas(RAS_ICanvas *canvas);
	void SetRasterizer(RAS_Rasterizer *rasterizer);
	void SetNetworkMessageManager(KX_NetworkMessageManager *manager);
#ifdef WITH_PYTHON
	PyObject *GetPyProfileDict();
#endif
	void SetConverter(KX_BlenderConverter *converter);
	KX_BlenderConverter *GetConverter()
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

	TaskScheduler *GetTaskScheduler()
	{
		return m_taskscheduler;
	}

	/// returns true if an update happened to indicate -> Render
	bool NextFrame();
	void Render();

	void StartEngine();
	void StopEngine();

	void RequestExit(KX_ExitRequest exitrequestmode);
	void SetNameNextGame(const std::string& nextgame);
	KX_ExitRequest GetExitCode();
	const std::string& GetExitString();

	CListValue<KX_Scene> *CurrentScenes();
	KX_Scene *FindScene(const std::string& scenename);
	void AddScene(KX_Scene *scene);
	void ConvertAndAddScene(const std::string& scenename, bool overlay);

	void RemoveScene(const std::string& scenename);
	bool ReplaceScene(const std::string& oldscene, const std::string& newscene);
	void SuspendScene(const std::string& scenename);
	void ResumeScene(const std::string& scenename);

	void GetSceneViewport(KX_Scene *scene, KX_Camera *cam, const RAS_Rect& displayArea, RAS_Rect& area, RAS_Rect& viewport);

	/// Sets zoom for camera objects, useful only with extend and scale framing mode.
	void SetCameraZoom(float camzoom);
	/// Sets zoom for default camera, = 2 in embedded mode.
	void SetCameraOverrideZoom(float camzoom);
	/// Get the camera zoom for the passed camera.
	float GetCameraZoom(KX_Camera *camera) const;

	void EnableCameraOverride(const std::string& forscene, const MT_Matrix4x4& projmat, const MT_Matrix4x4& viewmat, const RAS_CameraData& camdata);

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
	 * Gets the maximum number of physics frame before render frame
	 */
	int GetMaxPhysicsFrame();
	/**
	 * Sets the maximum number of physics frame before render frame
	 */
	void SetMaxPhysicsFrame(int frame);

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

	KX_Scene *CreateScene(const std::string& scenename);
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

#endif  /* __KX_KETSJIENGINE_H__ */
