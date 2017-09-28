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
#  pragma warning (disable:4786)
#endif

#include "CM_Message.h"

#include <boost/format.hpp>

#include "BLI_task.h"

#include "KX_KetsjiEngine.h"

#include "EXP_ListValue.h"
#include "EXP_IntValue.h"
#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"

#include "RAS_BucketManager.h"
#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_OffScreen.h"
#include "RAS_ILightObject.h"
#include "RAS_SceneLayerData.h"
#include "MT_Vector3.h"
#include "MT_Transform.h"
#include "SCA_IInputDevice.h"
#include "KX_Camera.h"
#include "KX_Light.h"
#include "KX_Globals.h"
#include "KX_PyConstraintBinding.h"
#include "PHY_IPhysicsEnvironment.h"

#include "KX_NetworkMessageScene.h"

#include "DEV_Joystick.h" // for DEV_Joystick::HandleEvents
#include "KX_PythonInit.h" // for updatePythonJoysticks

#include "KX_WorldInfo.h"
#include "KX_BlenderConverter.h"

#include "RAS_FramingManager.h"
#include "DNA_lightprobe_types.h"
#include "DNA_world_types.h"
#include "DNA_scene_types.h"

#include "KX_NavMeshObject.h"

#define DEFAULT_LOGIC_TIC_RATE 60.0

#ifdef FREE_WINDOWS /* XXX mingw64 (gcc 4.7.0) defines a macro for DrawText that translates to DrawTextA. Not good */
#  ifdef DrawText
#    undef DrawText
#  endif
#endif

extern "C" {
#  include "DRW_render.h"
}

KX_KetsjiEngine::CameraRenderData::CameraRenderData(KX_Camera *rendercam, KX_Camera *cullingcam, const RAS_Rect& area, const RAS_Rect& viewport,
															  RAS_Rasterizer::StereoEye eye)
	:m_renderCamera(rendercam),
	m_cullingCamera(cullingcam),
	m_area(area),
	m_viewport(viewport),
	m_eye(eye)
{
	m_renderCamera->AddRef();
}

KX_KetsjiEngine::CameraRenderData::CameraRenderData(const CameraRenderData& other)
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

KX_KetsjiEngine::SceneRenderData::SceneRenderData(KX_Scene *scene)
	:m_scene(scene)
{
}

KX_KetsjiEngine::FrameRenderData::FrameRenderData(RAS_Rasterizer::OffScreenType ofsType)
	:m_ofsType(ofsType)
{
}

const std::string KX_KetsjiEngine::m_profileLabels[tc_numCategories] = {
	"Physics:", // tc_physics
	"Logic:", // tc_logic
	"Animations:", // tc_animations
	"Network:", // tc_network
	"Scenegraph:", // tc_scenegraph
	"Rasterizer:", // tc_rasterizer
	"Services:", // tc_services
	"Overhead:", // tc_overhead
	"Outside:", // tc_outside
	"GPU Latency:" // tc_latency
};

/**
 * Constructor of the Ketsji Engine
 */
KX_KetsjiEngine::KX_KetsjiEngine(KX_ISystem *system)
	:m_canvas(nullptr),
	m_rasterizer(nullptr),
	m_kxsystem(system),
	m_converter(nullptr),
	m_inputDevice(nullptr),
	m_bInitialized(false),
	m_flags(AUTO_ADD_DEBUG_PROPERTIES),
	m_frameTime(0.0f),
	m_clockTime(0.0f),
	m_previousClockTime(0.0f),
	m_previousAnimTime(0.0f),
	m_timescale(1.0f),
	m_previousRealTime(0.0f),
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
	m_logger(KX_TimeCategoryLogger(25)),
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

	m_taskscheduler = BLI_task_scheduler_create(TASK_SCHEDULER_AUTO_THREADS);

	m_scenes = new CListValue<KX_Scene>();
}

/**
 *	Destructor of the Ketsji Engine, release all memory
 */
KX_KetsjiEngine::~KX_KetsjiEngine()
{
#ifdef WITH_PYTHON
	Py_CLEAR(m_pyprofiledict);
#endif

	if (m_taskscheduler)
		BLI_task_scheduler_free(m_taskscheduler);

	m_scenes->Release();
}

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

void KX_KetsjiEngine::SetConverter(KX_BlenderConverter *converter)
{
	BLI_assert(converter);
	m_converter = converter;
}

void KX_KetsjiEngine::StartEngine()
{
	m_clockTime = m_kxsystem->GetTimeInSeconds();
	m_frameTime = m_kxsystem->GetTimeInSeconds();
	m_previousClockTime = m_kxsystem->GetTimeInSeconds();
	m_previousRealTime = m_kxsystem->GetTimeInSeconds();

	m_bInitialized = true;
}

void KX_KetsjiEngine::BeginFrame()
{
	m_rasterizer->BeginFrame(m_frameTime);

	m_canvas->BeginDraw();
}

void KX_KetsjiEngine::EndFrame()
{
	//m_rasterizer->MotionBlur();

	// Show profiling info
	m_logger.StartLog(tc_overhead, m_kxsystem->GetTimeInSeconds());
	if (m_flags & (SHOW_PROFILE | SHOW_FRAMERATE | SHOW_DEBUG_PROPERTIES)) {
		/* TEMP: DISABLE TEXT DRAWING in 2.8 WAITING FOR REFACTOR */
		RenderDebugProperties();
	}

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
	m_logger.NextMeasurement(m_kxsystem->GetTimeInSeconds());

	m_logger.StartLog(tc_rasterizer, m_kxsystem->GetTimeInSeconds());
	m_rasterizer->EndFrame();

	m_logger.StartLog(tc_logic, m_kxsystem->GetTimeInSeconds());
	m_canvas->FlushScreenshots();

	// swap backbuffer (drawing into this buffer) <-> front/visible buffer
	m_logger.StartLog(tc_latency, m_kxsystem->GetTimeInSeconds());
	m_canvas->SwapBuffers();
	m_logger.StartLog(tc_rasterizer, m_kxsystem->GetTimeInSeconds());

	m_canvas->EndDraw();
}

bool KX_KetsjiEngine::NextFrame()
{
	m_logger.StartLog(tc_services, m_kxsystem->GetTimeInSeconds());

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
	 * XXX The logic over computation framestep is definitively not clear (and
	 * I'm not even sure it is correct). If needed frame is strictly greater
	 * than max_physics_frame, we are doing a jump in game time, but keeping
	 * framestep = 1 / ticrate, while if frames is greater than
	 * max_logic_frame, we increase framestep.
	 *
	 * XXX render.fps is not considred anywhere.
	 */

	double timestep = m_timescale / m_ticrate;
	if (!(m_flags & USE_EXTERNAL_CLOCK)) {
		double current_time = m_kxsystem->GetTimeInSeconds();
		double dt = current_time - m_previousRealTime;
		m_previousRealTime = current_time;
		m_clockTime += dt * m_timescale;

		if (!(m_flags & FIXED_FRAMERATE)) {
			timestep = dt * m_timescale;
		}
	}

	double deltatime = m_clockTime - m_frameTime;
	if (deltatime < 0.0) {
		// We got here too quickly, which means there is nothing to do, just return and don't render.
		// Not sure if this is the best fix, but it seems to stop the jumping framerate issue (#33088)
		return false;
	}

	// In case of non-fixed framerate, we always proceed one frame.
	int frames = 1;

	// Compute the number of logic frames to do each update in case of fixed framerate.
	if (m_flags & FIXED_FRAMERATE) {
		frames = int(deltatime * m_ticrate / m_timescale + 1e-6);
	}

//	CM_Debug("dt = " << dt << ", deltatime = " << deltatime << ", frames = " << frames);

	double framestep = timestep;

	if (frames > m_maxPhysicsFrame) {
		m_frameTime += (frames - m_maxPhysicsFrame) * timestep;
		frames = m_maxPhysicsFrame;
	}

	bool doRender = frames > 0;

	if (frames > m_maxLogicFrame) {
		framestep = (frames * timestep) / m_maxLogicFrame;
		frames = m_maxLogicFrame;
	}

	while (frames) {
		m_frameTime += framestep;

		m_converter->MergeAsyncLoads();

		if (m_inputDevice) {
			m_inputDevice->ReleaseMoveEvent();
		}
#ifdef WITH_SDL
		// Handle all SDL Joystick events here to share them for all scenes properly.
		short addrem[JOYINDEX_MAX] = {0};
		if (DEV_Joystick::HandleEvents(addrem)) {
#  ifdef WITH_PYTHON
			updatePythonJoysticks(addrem);
#  endif  // WITH_PYTHON
		}
#endif  // WITH_SDL

		// for each scene, call the proceed functions
		for (KX_Scene *scene : m_scenes) {
			/* Suspension holds the physics and logic processing for an
			 * entire scene. Objects can be suspended individually, and
			 * the settings for that precede the logic and physics
			 * update. */
			m_logger.StartLog(tc_logic, m_kxsystem->GetTimeInSeconds());

			scene->UpdateObjectActivity();

			if (!scene->IsSuspended()) {
				m_logger.StartLog(tc_physics, m_kxsystem->GetTimeInSeconds());
				// set Python hooks for each scene
#ifdef WITH_PYTHON
				PHY_SetActiveEnvironment(scene->GetPhysicsEnvironment());
#endif
				KX_SetActiveScene(scene);

				scene->GetPhysicsEnvironment()->EndFrame();

				// Process sensors, and controllers
				m_logger.StartLog(tc_logic, m_kxsystem->GetTimeInSeconds());
				scene->LogicBeginFrame(m_frameTime, framestep);

				// Scenegraph needs to be updated again, because Logic Controllers
				// can affect the local matrices.
				m_logger.StartLog(tc_scenegraph, m_kxsystem->GetTimeInSeconds());
				scene->UpdateParents(m_frameTime);

				// Process actuators

				// Do some cleanup work for this logic frame
				m_logger.StartLog(tc_logic, m_kxsystem->GetTimeInSeconds());
				scene->LogicUpdateFrame(m_frameTime);

				scene->LogicEndFrame();

				// Actuators can affect the scenegraph
				m_logger.StartLog(tc_scenegraph, m_kxsystem->GetTimeInSeconds());
				scene->UpdateParents(m_frameTime);

				m_logger.StartLog(tc_physics, m_kxsystem->GetTimeInSeconds());
				scene->GetPhysicsEnvironment()->BeginFrame();

				// Perform physics calculations on the scene. This can involve
				// many iterations of the physics solver.
				scene->GetPhysicsEnvironment()->ProceedDeltaTime(m_frameTime, timestep, framestep);//m_deltatimerealDeltaTime);

				m_logger.StartLog(tc_scenegraph, m_kxsystem->GetTimeInSeconds());
				scene->UpdateParents(m_frameTime);
			}

			m_logger.StartLog(tc_services, m_kxsystem->GetTimeInSeconds());
		}

		m_logger.StartLog(tc_network, m_kxsystem->GetTimeInSeconds());
		m_networkMessageManager->ClearMessages();

		m_logger.StartLog(tc_services, m_kxsystem->GetTimeInSeconds());

		// update system devices
		m_logger.StartLog(tc_logic, m_kxsystem->GetTimeInSeconds());
		if (m_inputDevice) {
			m_inputDevice->ClearInputs();
		}

		UpdateSuspendedScenes(framestep);
		// scene management
		ProcessScheduledScenes();

		frames--;
	}

	// Start logging time spent outside main loop
	m_logger.StartLog(tc_outside, m_kxsystem->GetTimeInSeconds());

	return doRender && m_doRender;
}

void KX_KetsjiEngine::UpdateSuspendedScenes(double framestep)
{
	for (KX_Scene *scene : m_scenes) {
		if (scene->IsSuspended()) {
			scene->SetSuspendedDelta(scene->GetSuspendedDelta() + framestep);
		}
	}
}

KX_KetsjiEngine::CameraRenderData KX_KetsjiEngine::GetCameraRenderData(KX_Scene *scene, KX_Camera *camera, KX_Camera *overrideCullingCam,
		const RAS_Rect& displayArea, RAS_Rasterizer::StereoEye eye, bool usestereo)
{
	KX_Camera *rendercam;
	/* In case of stereo we must copy the camera because it is used twice with different settings
	 * (modelview matrix). This copy use the same transform settings that the original camera
	 * and its name is based on with the eye number in addition.
	 */
	if (usestereo) {
		rendercam = new KX_Camera(scene, scene->m_callbacks, *camera->GetCameraData(), true, true);
		rendercam->SetName("__stereo_" + camera->GetName() + "_" + std::to_string(eye) + "__");
		rendercam->NodeSetGlobalOrientation(camera->NodeGetWorldOrientation());
		rendercam->NodeSetWorldPosition(camera->NodeGetWorldPosition());
		rendercam->NodeSetWorldScale(camera->NodeGetWorldScaling());
		rendercam->NodeUpdateGS(0.0);
	}
	// Else use the native camera.
	else {
		rendercam = camera;
	}

	KX_Camera *cullingcam = (overrideCullingCam) ? overrideCullingCam : rendercam;

	KX_SetActiveScene(scene);
#ifdef WITH_PYTHON
	scene->RunDrawingCallbacks(KX_Scene::PRE_DRAW_SETUP, rendercam);
#endif

	RAS_Rect area;
	RAS_Rect viewport;
	// Compute the area and the viewport based on the current display area and the optional camera viewport.
	GetSceneViewport(scene, rendercam, displayArea, area, viewport);
	// Compute the camera matrices: modelview and projection.
	const MT_Matrix4x4 viewmat = m_rasterizer->GetViewMatrix(eye, rendercam->GetWorldToCamera(), rendercam->GetCameraData()->m_perspective);
	const MT_Matrix4x4 projmat = GetCameraProjectionMatrix(scene, rendercam, eye, viewport, area);
	rendercam->SetModelviewMatrix(viewmat);
	rendercam->SetProjectionMatrix(projmat);

	CameraRenderData cameraData(rendercam, cullingcam, area, viewport, eye);

	if (usestereo) {
		rendercam->Release();
	}

	return cameraData;
}

bool KX_KetsjiEngine::GetFrameRenderData(std::vector<FrameRenderData>& frameDataList)
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
	static const RAS_Rasterizer::OffScreenType ofsType[] = {
		RAS_Rasterizer::RAS_OFFSCREEN_EYE_LEFT0,
		RAS_Rasterizer::RAS_OFFSCREEN_EYE_RIGHT0
	};

	// Pre-compute the display area used for stereo or normal rendering.
	std::vector<RAS_Rect> displayAreas;
	for (unsigned short eye = 0; eye < numeyes; ++eye) {
		displayAreas.push_back(m_rasterizer->GetRenderArea(m_canvas, (RAS_Rasterizer::StereoEye)eye));
	}

	// Prepare override culling camera of each scenes, we don't manage stereo currently.
	for (KX_Scene *scene : m_scenes) {
		KX_Camera *overrideCullingCam = scene->GetOverrideCullingCamera();

		if (overrideCullingCam) {
			RAS_Rect area;
			RAS_Rect viewport;
			// Compute the area and the viewport based on the current display area and the optional camera viewport.
			GetSceneViewport(scene, overrideCullingCam, displayAreas[RAS_Rasterizer::RAS_STEREO_LEFTEYE], area, viewport);
			// Compute the camera matrices: modelview and projection.
			const MT_Matrix4x4 viewmat = m_rasterizer->GetViewMatrix(RAS_Rasterizer::RAS_STEREO_LEFTEYE, overrideCullingCam->GetWorldToCamera(),
																	 overrideCullingCam->GetCameraData()->m_perspective);
			const MT_Matrix4x4 projmat = GetCameraProjectionMatrix(scene, overrideCullingCam, RAS_Rasterizer::RAS_STEREO_LEFTEYE, viewport, area);
			overrideCullingCam->SetModelviewMatrix(viewmat);
			overrideCullingCam->SetProjectionMatrix(projmat);
		}
	}

	for (unsigned short frame = 0; frame < numframes; ++frame) {
		frameDataList.emplace_back(ofsType[frame]);
		FrameRenderData& frameData = frameDataList.back();

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
			SceneRenderData& sceneFrameData = frameData.m_sceneDataList.back();

			KX_Camera *activecam = scene->GetActiveCamera();
			/* TEMP -> needs to be optimised */
			activecam->UpdateViewVecs(scene->GetEeveeData()->stl);

			KX_Camera *overrideCullingCam = scene->GetOverrideCullingCamera();
			for (KX_Camera *cam : scene->GetCameraList()) {
				if (cam != activecam && !cam->GetViewport()) {
					continue;
				}

				for (RAS_Rasterizer::StereoEye eye : eyes) {
					sceneFrameData.m_cameraDataList.push_back(GetCameraRenderData(scene, cam, overrideCullingCam, displayAreas[eye], eye, usestereo));
				}
			}
		}
	}

	return renderpereye;
}

/***********************EEVEE PROBES SYSTEM*************************/

#define PROBE_RT_SIZE 512 /* Cube render target */
#define PROBE_OCTAHEDRON_SIZE 1024
#define IRRADIANCE_POOL_SIZE 1024

/* TODO find a nice name to push it to math_matrix.c */
static void scale_m4_v3(float R[4][4], float v[3])
{
	for (int i = 0; i < 4; ++i)
		mul_v3_v3(R[i], v);
}

static KX_GameObject *find_probe(KX_Scene *scene, Object *ob) {
	for (KX_GameObject *gameobj : scene->GetProbeList()) {
		if (gameobj->GetBlenderObject() == ob) {
			return gameobj;
		}
	}
	return nullptr;
}

static void EEVEE_planar_reflections_updates(EEVEE_SceneLayerData *sldata, EEVEE_PassList *psl, EEVEE_TextureList *txl, KX_Camera *cam, KX_Scene *scene)
{
	EEVEE_LightProbesInfo *pinfo = sldata->probes;
	Object *ob;
	float mtx[4][4], normat[4][4], imat[4][4], rangemat[4][4];

	float viewmat[4][4], winmat[4][4];
	MT_Matrix4x4 view(cam->GetModelviewMatrix());
	view.getValue(&viewmat[0][0]);
	MT_Matrix4x4 proj(cam->GetProjectionMatrix());
	proj.getValue(&winmat[0][0]);

	zero_m4(rangemat);
	rangemat[0][0] = rangemat[1][1] = rangemat[2][2] = 0.5f;
	rangemat[3][0] = rangemat[3][1] = rangemat[3][2] = 0.5f;
	rangemat[3][3] = 1.0f;

	/* PLANAR REFLECTION */
	for (int i = 0; (ob = pinfo->probes_planar_ref[i]) && (i < MAX_PLANAR); i++) {
		LightProbe *probe = (LightProbe *)ob->data;
		EEVEE_PlanarReflection *eplanar = &pinfo->planar_data[i];
		EEVEE_LightProbeEngineData *ped = EEVEE_lightprobe_data_get(ob);

		float obmat[4][4];
		KX_GameObject *pro = find_probe(scene, ob);
		pro->NodeGetWorldTransform().getValue(&obmat[0][0]);

		/* Computing mtx : matrix that mirror position around object's XY plane. */
		normalize_m4_m4(normat, obmat);  /* object > world */
		invert_m4_m4(imat, normat); /* world > object */

		float reflect[3] = { 1.0f, 1.0f, -1.0f }; /* XY reflection plane */
		scale_m4_v3(imat, reflect); /* world > object > mirrored obj */
		mul_m4_m4m4(mtx, normat, imat); /* world > object > mirrored obj > world */

		/* Reflect Camera Matrix. */
		mul_m4_m4m4(ped->viewmat, viewmat, mtx);

		/* TODO FOV margin */
		float winmat_fov[4][4];
		copy_m4_m4(winmat_fov, winmat);

		/* Apply Perspective Matrix. */
		mul_m4_m4m4(ped->persmat, winmat_fov, ped->viewmat);

		/* This is the matrix used to reconstruct texture coordinates.
		* We use the original view matrix because it does not create
		* visual artifacts if receiver is not perfectly aligned with
		* the planar reflection probe. */
		mul_m4_m4m4(eplanar->reflectionmat, winmat_fov, viewmat); /* TODO FOV margin */
		/* Convert from [-1, 1] to [0, 1] (NDC to Texture coord). */
		mul_m4_m4m4(eplanar->reflectionmat, rangemat, eplanar->reflectionmat);

		/* TODO frustum check. */
		ped->need_update = true;

		/* Compute clip plane equation / normal. */
		float refpoint[3];
		copy_v3_v3(eplanar->plane_equation, obmat[2]);
		normalize_v3(eplanar->plane_equation); /* plane normal */
		eplanar->plane_equation[3] = -dot_v3v3(eplanar->plane_equation, obmat[3]);

		/* Compute offset plane equation (fix missing texels near reflection plane). */
		copy_v3_v3(ped->planer_eq_offset, eplanar->plane_equation);
		mul_v3_v3fl(refpoint, eplanar->plane_equation, -probe->clipsta);
		add_v3_v3(refpoint, obmat[3]);
		ped->planer_eq_offset[3] = -dot_v3v3(eplanar->plane_equation, refpoint);

		/* Compute XY clip planes. */
		normalize_v3_v3(eplanar->clip_vec_x, obmat[0]);
		normalize_v3_v3(eplanar->clip_vec_y, obmat[1]);

		float vec[3] = { 0.0f, 0.0f, 0.0f };
		vec[0] = 1.0f; vec[1] = 0.0f; vec[2] = 0.0f;
		mul_m4_v3(obmat, vec); /* Point on the edge */
		eplanar->clip_edge_x_pos = dot_v3v3(eplanar->clip_vec_x, vec);

		vec[0] = 0.0f; vec[1] = 1.0f; vec[2] = 0.0f;
		mul_m4_v3(obmat, vec); /* Point on the edge */
		eplanar->clip_edge_y_pos = dot_v3v3(eplanar->clip_vec_y, vec);

		vec[0] = -1.0f; vec[1] = 0.0f; vec[2] = 0.0f;
		mul_m4_v3(obmat, vec); /* Point on the edge */
		eplanar->clip_edge_x_neg = dot_v3v3(eplanar->clip_vec_x, vec);

		vec[0] = 0.0f; vec[1] = -1.0f; vec[2] = 0.0f;
		mul_m4_v3(obmat, vec); /* Point on the edge */
		eplanar->clip_edge_y_neg = dot_v3v3(eplanar->clip_vec_y, vec);

		/* Facing factors */
		float max_angle = max_ff(1e-2f, probe->falloff) * M_PI * 0.5f;
		float min_angle = 0.0f;
		eplanar->facing_scale = 1.0f / max_ff(1e-8f, cosf(min_angle) - cosf(max_angle));
		eplanar->facing_bias = -min_ff(1.0f - 1e-8f, cosf(max_angle)) * eplanar->facing_scale;

		/* Distance factors */
		float max_dist = probe->distinf;
		float min_dist = min_ff(1.0f - 1e-8f, 1.0f - probe->falloff) * probe->distinf;
		eplanar->attenuation_scale = -1.0f / max_ff(1e-8f, max_dist - min_dist);
		eplanar->attenuation_bias = max_dist * -eplanar->attenuation_scale;
	}
}

static void EEVEE_lightprobes_updates(EEVEE_SceneLayerData *sldata, EEVEE_PassList *psl, EEVEE_StorageList *stl, KX_Scene *scene)
{
	EEVEE_LightProbesInfo *pinfo = sldata->probes;
	Object *ob;

	/* CUBE REFLECTION */
	for (int i = 1; (ob = pinfo->probes_cube_ref[i]) && (i < MAX_PROBE); i++) {
		LightProbe *probe = (LightProbe *)ob->data;
		EEVEE_LightProbe *eprobe = &pinfo->probe_data[i];
		EEVEE_LightProbeEngineData *ped = EEVEE_lightprobe_data_get(ob);

		float obmat[4][4];
		KX_GameObject *pro = find_probe(scene, ob);
		pro->NodeGetWorldTransform().getValue(&obmat[0][0]);

		/* Update transforms */
		copy_v3_v3(eprobe->position, obmat[3]);

		/* Attenuation */
		eprobe->attenuation_type = probe->attenuation_type;
		eprobe->attenuation_fac = 1.0f / max_ff(1e-8f, probe->falloff);

		unit_m4(eprobe->attenuationmat);
		scale_m4_fl(eprobe->attenuationmat, probe->distinf);
		mul_m4_m4m4(eprobe->attenuationmat, obmat, eprobe->attenuationmat);
		invert_m4(eprobe->attenuationmat);

		/* Parallax */
		float dist;
		if ((probe->flag & LIGHTPROBE_FLAG_CUSTOM_PARALLAX) != 0) {
			eprobe->parallax_type = probe->parallax_type;
			dist = probe->distpar;
		}
		else {
			eprobe->parallax_type = probe->attenuation_type;
			dist = probe->distinf;
		}

		unit_m4(eprobe->parallaxmat);
		scale_m4_fl(eprobe->parallaxmat, dist);
		mul_m4_m4m4(eprobe->parallaxmat, obmat, eprobe->parallaxmat);
		invert_m4(eprobe->parallaxmat);
	}

	/* IRRADIANCE GRID */
	int offset = 1; /* to account for the world probe */
	for (int i = 1; (ob = pinfo->probes_grid_ref[i]) && (i < MAX_GRID); i++) {
		LightProbe *probe = (LightProbe *)ob->data;
		EEVEE_LightGrid *egrid = &pinfo->grid_data[i];
		EEVEE_LightProbeEngineData *ped = EEVEE_lightprobe_data_get(ob);

		float obmat[4][4];
		KX_GameObject *pro = find_probe(scene, ob);
		pro->NodeGetWorldTransform().getValue(&obmat[0][0]);

		egrid->offset = offset;
		float fac = 1.0f / max_ff(1e-8f, probe->falloff);
		egrid->attenuation_scale = fac / max_ff(1e-8f, probe->distinf);
		egrid->attenuation_bias = fac;

		/* Set offset for the next grid */
		offset += ped->num_cell;

		/* Update transforms */
		float cell_dim[3], half_cell_dim[3];
		cell_dim[0] = 2.0f / (float)(probe->grid_resolution_x);
		cell_dim[1] = 2.0f / (float)(probe->grid_resolution_y);
		cell_dim[2] = 2.0f / (float)(probe->grid_resolution_z);

		mul_v3_v3fl(half_cell_dim, cell_dim, 0.5f);

		/* Matrix converting world space to cell ranges. */
		invert_m4_m4(egrid->mat, obmat);

		/* First cell. */
		copy_v3_fl(egrid->corner, -1.0f);
		add_v3_v3(egrid->corner, half_cell_dim);
		mul_m4_v3(obmat, egrid->corner);

		/* Opposite neighbor cell. */
		copy_v3_fl3(egrid->increment_x, cell_dim[0], 0.0f, 0.0f);
		add_v3_v3(egrid->increment_x, half_cell_dim);
		add_v3_fl(egrid->increment_x, -1.0f);
		mul_m4_v3(obmat, egrid->increment_x);
		sub_v3_v3(egrid->increment_x, egrid->corner);

		copy_v3_fl3(egrid->increment_y, 0.0f, cell_dim[1], 0.0f);
		add_v3_v3(egrid->increment_y, half_cell_dim);
		add_v3_fl(egrid->increment_y, -1.0f);
		mul_m4_v3(obmat, egrid->increment_y);
		sub_v3_v3(egrid->increment_y, egrid->corner);

		copy_v3_fl3(egrid->increment_z, 0.0f, 0.0f, cell_dim[2]);
		add_v3_v3(egrid->increment_z, half_cell_dim);
		add_v3_fl(egrid->increment_z, -1.0f);
		mul_m4_v3(obmat, egrid->increment_z);
		sub_v3_v3(egrid->increment_z, egrid->corner);

		copy_v3_v3_int(egrid->resolution, &probe->grid_resolution_x);
	}
}

static void downsample_planar(void *vedata, int level)
{
	EEVEE_PassList *psl = ((EEVEE_Data *)vedata)->psl;
	EEVEE_StorageList *stl = ((EEVEE_Data *)vedata)->stl;

	const float *size = DRW_viewport_size_get();
	copy_v2_v2(stl->g_data->texel_size, size);
	for (int i = 0; i < level - 1; ++i) {
		stl->g_data->texel_size[0] /= 2.0f;
		stl->g_data->texel_size[1] /= 2.0f;
		min_ff(floorf(stl->g_data->texel_size[0]), 1.0f);
		min_ff(floorf(stl->g_data->texel_size[1]), 1.0f);
	}
	invert_v2(stl->g_data->texel_size);

	DRW_draw_pass(psl->probe_planar_downsample_ps);
}

/* Glossy filter probe_rt to probe_pool at index probe_idx */
static void glossy_filter_probe(EEVEE_SceneLayerData *sldata, EEVEE_Data *vedata, EEVEE_PassList *psl, int probe_idx)
{
	EEVEE_LightProbesInfo *pinfo = sldata->probes;

	/* Max lod used from the render target probe */
	pinfo->lod_rt_max = floorf(log2f(PROBE_RT_SIZE)) - 2.0f;

	/* 2 - Let gpu create Mipmaps for Filtered Importance Sampling. */
	/* Bind next framebuffer to be able to gen. mips for probe_rt. */
	DRW_framebuffer_bind(sldata->probe_filter_fb);
	EEVEE_downsample_cube_buffer(vedata, sldata->probe_filter_fb, sldata->probe_rt, (int)(pinfo->lod_rt_max));

	/* 3 - Render to probe array to the specified layer, do prefiltering. */
	/* Detach to rebind the right mipmap. */
	DRW_framebuffer_texture_detach(sldata->probe_pool);
	float mipsize = PROBE_OCTAHEDRON_SIZE;
	const int maxlevel = (int)floorf(log2f(PROBE_OCTAHEDRON_SIZE));
	const int min_lod_level = 3;
	for (int i = 0; i < maxlevel - min_lod_level; i++) {
		float bias = (i == 0) ? -1.0f : 1.0f;
		pinfo->texel_size = 1.0f / mipsize;
		pinfo->padding_size = powf(2.0f, (float)(maxlevel - min_lod_level - 1 - i));
		/* XXX : WHY THE HECK DO WE NEED THIS ??? */
		/* padding is incorrect without this! float precision issue? */
		if (pinfo->padding_size > 32) {
			pinfo->padding_size += 5;
		}
		if (pinfo->padding_size > 16) {
			pinfo->padding_size += 4;
		}
		else if (pinfo->padding_size > 8) {
			pinfo->padding_size += 2;
		}
		else if (pinfo->padding_size > 4) {
			pinfo->padding_size += 1;
		}
		pinfo->layer = probe_idx;
		pinfo->roughness = (float)i / ((float)maxlevel - 4.0f);
		pinfo->roughness *= pinfo->roughness; /* Disney Roughness */
		pinfo->roughness *= pinfo->roughness; /* Distribute Roughness accros lod more evenly */
		CLAMP(pinfo->roughness, 1e-8f, 0.99999f); /* Avoid artifacts */

#if 1 /* Variable Sample count (fast) */
		switch (i) {
		case 0: pinfo->samples_ct = 1.0f; break;
		case 1: pinfo->samples_ct = 16.0f; break;
		case 2: pinfo->samples_ct = 32.0f; break;
		case 3: pinfo->samples_ct = 64.0f; break;
		default: pinfo->samples_ct = 128.0f; break;
		}
#else /* Constant Sample count (slow) */
		pinfo->samples_ct = 1024.0f;
#endif

		pinfo->invsamples_ct = 1.0f / pinfo->samples_ct;
		pinfo->lodfactor = bias + 0.5f * log((float)(PROBE_RT_SIZE * PROBE_RT_SIZE) * pinfo->invsamples_ct) / log(2);

		DRW_framebuffer_texture_attach(sldata->probe_filter_fb, sldata->probe_pool, 0, i);
		DRW_framebuffer_viewport_size(sldata->probe_filter_fb, 0, 0, mipsize, mipsize);
		DRW_draw_pass(psl->probe_glossy_compute);
		DRW_framebuffer_texture_detach(sldata->probe_pool);

		mipsize /= 2;
		CLAMP_MIN(mipsize, 1);
	}
	/* For shading, save max level of the octahedron map */
	pinfo->lod_cube_max = (float)(maxlevel - min_lod_level) - 1.0f;

	/* reattach to have a valid framebuffer. */
	DRW_framebuffer_texture_attach(sldata->probe_filter_fb, sldata->probe_pool, 0, 0);
}

/* Diffuse filter probe_rt to irradiance_pool at index probe_idx */
static void diffuse_filter_probe(EEVEE_SceneLayerData *sldata, EEVEE_Data *vedata, EEVEE_PassList *psl, int offset)
{
	EEVEE_LightProbesInfo *pinfo = sldata->probes;

	/* find cell position on the virtual 3D texture */
	/* NOTE : Keep in sync with load_irradiance_cell() */
#if defined(IRRADIANCE_SH_L2)
	int size[2] = { 3, 3 };
#elif defined(IRRADIANCE_CUBEMAP)
	int size[2] = { 8, 8 };
	pinfo->samples_ct = 1024.0f;
#elif defined(IRRADIANCE_HL2)
	int size[2] = { 3, 2 };
	pinfo->samples_ct = 1024.0f;
#endif

	int cell_per_row = IRRADIANCE_POOL_SIZE / size[0];
	int x = size[0] * (offset % cell_per_row);
	int y = size[1] * (offset / cell_per_row);

#ifndef IRRADIANCE_SH_L2
	/* Tweaking parameters to balance perf. vs precision */
	const float bias = 0.0f;
	pinfo->invsamples_ct = 1.0f / pinfo->samples_ct;
	pinfo->lodfactor = bias + 0.5f * log((float)(PROBE_RT_SIZE * PROBE_RT_SIZE) * pinfo->invsamples_ct) / log(2);
	pinfo->lod_rt_max = floorf(log2f(PROBE_RT_SIZE)) - 2.0f;
#else
	pinfo->shres = 32; /* Less texture fetches & reduce branches */
	pinfo->lod_rt_max = 2.0f; /* Improve cache reuse */
#endif

	/* 4 - Compute spherical harmonics */
	DRW_framebuffer_bind(sldata->probe_filter_fb);
	EEVEE_downsample_cube_buffer(vedata, sldata->probe_filter_fb, sldata->probe_rt, (int)(pinfo->lod_rt_max));

	/* Bind the right texture layer (one layer per irradiance grid) */
	DRW_framebuffer_texture_detach(sldata->probe_pool);
	DRW_framebuffer_texture_attach(sldata->probe_filter_fb, sldata->irradiance_rt, 0, 0);

	DRW_framebuffer_viewport_size(sldata->probe_filter_fb, x, y, size[0], size[1]);
	DRW_draw_pass(psl->probe_diffuse_compute);

	/* reattach to have a valid framebuffer. */
	DRW_framebuffer_texture_detach(sldata->irradiance_rt);
	DRW_framebuffer_texture_attach(sldata->probe_filter_fb, sldata->probe_pool, 0, 0);
}

/* Render the scene to the probe_rt texture. */
static void render_scene_to_probe(
	EEVEE_SceneLayerData *sldata, EEVEE_Data *vedata,
	const float pos[3], float clipsta, float clipend, KX_Scene *scene, RAS_Rasterizer *rasty, KX_Camera *cam)
{
	EEVEE_TextureList *txl = vedata->txl;
	EEVEE_PassList *psl = vedata->psl;
	EEVEE_StorageList *stl = vedata->stl;
	EEVEE_LightProbesInfo *pinfo = sldata->probes;
	EEVEE_StaticProbeData *e_data = EEVEE_static_probe_data_get();

	float winmat[4][4], posmat[4][4], tmp_ao_dist, tmp_ao_samples, tmp_ao_settings;

	unit_m4(posmat);

	/* Move to capture position */
	negate_v3_v3(posmat[3], pos);

	/* Disable specular lighting when rendering probes to avoid feedback loops (looks bad). */
	sldata->probes->specular_toggle = false;
	sldata->probes->ssr_toggle = false;

	/* Disable AO until we find a way to hide really bad discontinuities between cubefaces. */
	tmp_ao_dist = stl->effects->ao_dist;
	tmp_ao_samples = stl->effects->ao_samples;
	tmp_ao_settings = stl->effects->ao_settings;
	stl->effects->ao_settings = 0.0f; /* Disable AO */

	/* 1 - Render to each cubeface individually.
	* We do this instead of using geometry shader because a) it's faster,
	* b) it's easier than fixing the nodetree shaders (for view dependant effects). */
	pinfo->layer = 0;
	perspective_m4(winmat, -clipsta, clipsta, -clipsta, clipsta, clipsta, clipend);

	/* Avoid using the texture attached to framebuffer when rendering. */
	/* XXX */
	GPUTexture *tmp_planar_pool = txl->planar_pool;
	GPUTexture *tmp_minz = stl->g_data->minzbuffer;
	GPUTexture *tmp_maxz = txl->maxzbuffer;
	txl->planar_pool = e_data->planar_pool_placeholder;
	stl->g_data->minzbuffer = e_data->depth_placeholder;
	txl->maxzbuffer = e_data->depth_placeholder;

	/* Detach to rebind the right cubeface. */
	DRW_framebuffer_bind(sldata->probe_fb);
	DRW_framebuffer_texture_attach(sldata->probe_fb, e_data->cube_face_depth, 0, 0);
	DRW_framebuffer_texture_detach(sldata->probe_rt);
	for (int i = 0; i < 6; ++i) {
		float viewmat[4][4], persmat[4][4];
		float viewinv[4][4], persinv[4][4];

		/* Setup custom matrices */
		mul_m4_m4m4(viewmat, cubefacemat[i], posmat);
		mul_m4_m4m4(persmat, winmat, viewmat);
		invert_m4_m4(persinv, persmat);
		invert_m4_m4(viewinv, viewmat);

		DRW_viewport_matrix_override_set(persmat, DRW_MAT_PERS);
		DRW_viewport_matrix_override_set(persinv, DRW_MAT_PERSINV);
		DRW_viewport_matrix_override_set(viewmat, DRW_MAT_VIEW);
		DRW_viewport_matrix_override_set(viewinv, DRW_MAT_VIEWINV);
		DRW_viewport_matrix_override_set(winmat, DRW_MAT_WIN);

		/* Be sure that cascaded shadow maps are updated. */
		//EEVEE_draw_shadows(sldata, psl); ////////////////////////////////////////////////////////

		DRW_framebuffer_cubeface_attach(sldata->probe_fb, sldata->probe_rt, 0, i, 0);
		DRW_framebuffer_viewport_size(sldata->probe_fb, 0, 0, PROBE_RT_SIZE, PROBE_RT_SIZE);

		DRW_framebuffer_clear(false, true, false, NULL, 1.0);

		/* Depth prepass */
		DRW_draw_pass(psl->depth_pass);
		DRW_draw_pass(psl->depth_pass_cull);

		DRW_draw_pass(psl->probe_background);

		// EEVEE_create_minmax_buffer(vedata, e_data.cube_face_depth);

		/* Rebind Planar FB */
		DRW_framebuffer_bind(sldata->probe_fb);

		/* Shading pass */
		//EEVEE_draw_default_passes(psl);//////////////////////////////////////////////////////////////
		//DRW_draw_pass(psl->material_pass);/////////////////////////////////////////////////////////////

		MT_Transform camtrans;
		KX_CullingNodeList nodes;
		const SG_Frustum frustum(cam->GetFrustum());
		/* update scene */
		scene->CalculateVisibleMeshes(nodes, frustum, 0);
		// Send a nullptr off screen because the viewport is binding it's using its own private one.
		scene->RenderBuckets(nodes, camtrans, rasty, nullptr);

		DRW_framebuffer_texture_detach(sldata->probe_rt);
	}
	DRW_framebuffer_texture_attach(sldata->probe_fb, sldata->probe_rt, 0, 0);
	DRW_framebuffer_texture_detach(e_data->cube_face_depth);

	DRW_viewport_matrix_override_unset(DRW_MAT_PERS);
	DRW_viewport_matrix_override_unset(DRW_MAT_PERSINV);
	DRW_viewport_matrix_override_unset(DRW_MAT_VIEW);
	DRW_viewport_matrix_override_unset(DRW_MAT_VIEWINV);
	DRW_viewport_matrix_override_unset(DRW_MAT_WIN);

	/* Restore */
	sldata->probes->specular_toggle = true;
	sldata->probes->ssr_toggle = true;
	txl->planar_pool = tmp_planar_pool;
	stl->g_data->minzbuffer = tmp_minz;
	txl->maxzbuffer = tmp_maxz;
	stl->effects->ao_dist = tmp_ao_dist;
	stl->effects->ao_samples = tmp_ao_samples;
	stl->effects->ao_settings = tmp_ao_settings;
}

static void render_scene_to_planar(
	EEVEE_SceneLayerData *sldata, EEVEE_Data *vedata, int layer,
	float(*viewmat)[4], float(*persmat)[4],
	float clip_plane[4], KX_Scene *scene, RAS_Rasterizer *rasty, KX_Camera *cam, RAS_OffScreen *inputofs)
{
	EEVEE_FramebufferList *fbl = vedata->fbl;
	EEVEE_TextureList *txl = vedata->txl;
	EEVEE_PassList *psl = vedata->psl;
	EEVEE_StaticProbeData *e_data = EEVEE_static_probe_data_get();

	float viewinv[4][4];
	float persinv[4][4];

	invert_m4_m4(viewinv, viewmat);
	invert_m4_m4(persinv, persmat);

	DRW_viewport_matrix_override_set(persmat, DRW_MAT_PERS);
	DRW_viewport_matrix_override_set(persinv, DRW_MAT_PERSINV);
	DRW_viewport_matrix_override_set(viewmat, DRW_MAT_VIEW);
	DRW_viewport_matrix_override_set(viewinv, DRW_MAT_VIEWINV);

	/* Since we are rendering with an inverted view matrix, we need
	* to invert the facing for backface culling to be the same. */
	DRW_state_invert_facing();

	/* Be sure that cascaded shadow maps are updated. */
	//EEVEE_draw_shadows(sldata, psl);//////////////////////////////////////////////////////

	DRW_state_clip_planes_add(clip_plane);

	/* Attach depth here since it's a DRW_TEX_TEMP */
	DRW_framebuffer_texture_layer_attach(fbl->planarref_fb, txl->planar_depth, 0, layer, 0);
	DRW_framebuffer_texture_layer_attach(fbl->planarref_fb, txl->planar_pool, 0, layer, 0);
	DRW_framebuffer_bind(fbl->planarref_fb);

	DRW_framebuffer_clear(false, true, false, NULL, 1.0);

	/* Turn off ssr to avoid black specular */
	/* TODO : Enable SSR in planar reflections? (Would be very heavy) */
	sldata->probes->ssr_toggle = false;

	/* Avoid using the texture attached to framebuffer when rendering. */
	/* XXX */
	GPUTexture *tmp_planar_pool = txl->planar_pool;
	GPUTexture *tmp_planar_depth = txl->planar_depth;
	txl->planar_pool = e_data->planar_pool_placeholder;
	txl->planar_depth = e_data->depth_array_placeholder;

	/* Depth prepass */
	DRW_draw_pass(psl->depth_pass_clip);
	DRW_draw_pass(psl->depth_pass_clip_cull);

	/* Background */
	DRW_draw_pass(psl->probe_background);

	EEVEE_create_minmax_buffer(vedata, tmp_planar_depth, layer);

	/* Compute GTAO Horizons */
	EEVEE_effects_do_gtao(sldata, vedata);

	/* Rebind Planar FB */
	DRW_framebuffer_bind(fbl->planarref_fb);
	inputofs->Bind();

	/* Shading pass */
	//EEVEE_draw_default_passes(psl);
	//DRW_draw_pass(psl->material_pass);
	/* render */
	MT_Transform camtrans;
	KX_CullingNodeList nodes;
	const SG_Frustum frustum(cam->GetFrustum());
	/* update scene */
	scene->CalculateVisibleMeshes(nodes, frustum, 0);
	// Send a nullptr off screen because the viewport is binding it's using its own private one.
	scene->RenderBuckets(nodes, camtrans, rasty, nullptr);

	DRW_state_invert_facing();
	DRW_state_clip_planes_reset();

	/* Restore */
	sldata->probes->ssr_toggle = true;
	txl->planar_pool = tmp_planar_pool;
	txl->planar_depth = tmp_planar_depth;
	DRW_viewport_matrix_override_unset(DRW_MAT_PERS);
	DRW_viewport_matrix_override_unset(DRW_MAT_PERSINV);
	DRW_viewport_matrix_override_unset(DRW_MAT_VIEW);
	DRW_viewport_matrix_override_unset(DRW_MAT_VIEWINV);

	DRW_framebuffer_texture_detach(txl->planar_pool);
	DRW_framebuffer_texture_detach(txl->planar_depth);
}

static void render_world_to_probe(EEVEE_SceneLayerData *sldata, EEVEE_PassList *psl)
{
	EEVEE_LightProbesInfo *pinfo = sldata->probes;
	float winmat[4][4];

	/* 1 - Render to cubemap target using geometry shader. */
	/* For world probe, we don't need to clear since we render the background directly. */
	pinfo->layer = 0;

	perspective_m4(winmat, -0.1f, 0.1f, -0.1f, 0.1f, 0.1f, 1.0f);

	/* Detach to rebind the right cubeface. */
	DRW_framebuffer_bind(sldata->probe_fb);
	DRW_framebuffer_texture_detach(sldata->probe_rt);
	for (int i = 0; i < 6; ++i) {
		float viewmat[4][4], persmat[4][4];
		float viewinv[4][4], persinv[4][4];

		DRW_framebuffer_cubeface_attach(sldata->probe_fb, sldata->probe_rt, 0, i, 0);
		DRW_framebuffer_viewport_size(sldata->probe_fb, 0, 0, PROBE_RT_SIZE, PROBE_RT_SIZE);

		/* Setup custom matrices */
		copy_m4_m4(viewmat, cubefacemat[i]);
		mul_m4_m4m4(persmat, winmat, viewmat);
		invert_m4_m4(persinv, persmat);
		invert_m4_m4(viewinv, viewmat);

		DRW_viewport_matrix_override_set(persmat, DRW_MAT_PERS);
		DRW_viewport_matrix_override_set(persinv, DRW_MAT_PERSINV);
		DRW_viewport_matrix_override_set(viewmat, DRW_MAT_VIEW);
		DRW_viewport_matrix_override_set(viewinv, DRW_MAT_VIEWINV);
		DRW_viewport_matrix_override_set(winmat, DRW_MAT_WIN);

		DRW_draw_pass(psl->probe_background);

		DRW_framebuffer_texture_detach(sldata->probe_rt);
	}
	DRW_framebuffer_texture_attach(sldata->probe_fb, sldata->probe_rt, 0, 0);

	DRW_viewport_matrix_override_unset(DRW_MAT_PERS);
	DRW_viewport_matrix_override_unset(DRW_MAT_PERSINV);
	DRW_viewport_matrix_override_unset(DRW_MAT_VIEW);
	DRW_viewport_matrix_override_unset(DRW_MAT_VIEWINV);
	DRW_viewport_matrix_override_unset(DRW_MAT_WIN);
}

static void lightprobe_cell_location_get(EEVEE_LightGrid *egrid, int cell_idx, float r_pos[3])
{
	float tmp[3], local_cell[3];
	/* Keep in sync with lightprobe_grid_display_vert */
	local_cell[2] = (float)(cell_idx % egrid->resolution[2]);
	local_cell[1] = (float)((cell_idx / egrid->resolution[2]) % egrid->resolution[1]);
	local_cell[0] = (float)(cell_idx / (egrid->resolution[2] * egrid->resolution[1]));

	copy_v3_v3(r_pos, egrid->corner);
	mul_v3_v3fl(tmp, egrid->increment_x, local_cell[0]);
	add_v3_v3(r_pos, tmp);
	mul_v3_v3fl(tmp, egrid->increment_y, local_cell[1]);
	add_v3_v3(r_pos, tmp);
	mul_v3_v3fl(tmp, egrid->increment_z, local_cell[2]);
	add_v3_v3(r_pos, tmp);
}

void KX_KetsjiEngine::EEVEE_lightprobes_refresh_bge(EEVEE_SceneLayerData *sldata, EEVEE_Data *vedata,
	KX_Scene *scene, RAS_Rasterizer *rasty, KX_Camera *cam, RAS_OffScreen *inputofs)
{
	EEVEE_TextureList *txl = vedata->txl;
	EEVEE_PassList *psl = vedata->psl;
	EEVEE_LightProbesInfo *pinfo = sldata->probes;
	Object *ob;
	const DRWContextState *draw_ctx = DRW_context_state_get();
	RegionView3D *rv3d = draw_ctx->rv3d;
	EEVEE_StaticProbeData *e_data = EEVEE_static_probe_data_get();

	/* Render world in priority */
	if (e_data->update_world) {
		render_world_to_probe(sldata, psl);
		glossy_filter_probe(sldata, vedata, psl, 0);
		diffuse_filter_probe(sldata, vedata, psl, 0);

		/* Swap and redo prefiltering for other rendertarget.
		* This way we have world lighting waiting for irradiance grids to catch up. */
		SWAP(GPUTexture *, sldata->irradiance_pool, sldata->irradiance_rt);
		diffuse_filter_probe(sldata, vedata, psl, 0);

		e_data->update_world = false;

		if (!e_data->world_ready_to_shade) {
			e_data->world_ready_to_shade = true;
			pinfo->num_render_cube = 1;
			pinfo->num_render_grid = 1;
		}
	}
	else if (true) { /* TODO if at least one probe needs refresh */

		/* Reflection probes depend on diffuse lighting thus on irradiance grid */
		const int max_bounce = 3;
		while (pinfo->updated_bounce < max_bounce) {
			pinfo->num_render_grid = pinfo->num_grid;

			for (int i = 1; (ob = pinfo->probes_grid_ref[i]) && (i < MAX_GRID); i++) {
				EEVEE_LightProbeEngineData *ped = EEVEE_lightprobe_data_get(ob);


				EEVEE_LightGrid *egrid = &pinfo->grid_data[i];
				LightProbe *prb = (LightProbe *)ob->data;
				int cell_id = ped->updated_cells;

				SWAP(GPUTexture *, sldata->irradiance_pool, sldata->irradiance_rt);

				/* Temporary Remove all probes. */
				int tmp_num_render_grid = pinfo->num_render_grid;
				int tmp_num_render_cube = pinfo->num_render_cube;
				int tmp_num_planar = pinfo->num_planar;
				pinfo->num_render_cube = 0;
				pinfo->num_planar = 0;

				/* Use light from previous bounce when capturing radiance. */
				if (pinfo->updated_bounce == 0) {
					pinfo->num_render_grid = 0;
				}

				float pos[3];
				lightprobe_cell_location_get(egrid, cell_id, pos);

				render_scene_to_probe(sldata, vedata, pos, prb->clipsta, prb->clipend, scene, rasty, cam);
				diffuse_filter_probe(sldata, vedata, psl, egrid->offset + cell_id);

				/* Restore */
				pinfo->num_render_grid = tmp_num_render_grid;
				pinfo->num_render_cube = tmp_num_render_cube;
				pinfo->num_planar = tmp_num_planar;

				/* To see what is going on. */
				SWAP(GPUTexture *, sldata->irradiance_pool, sldata->irradiance_rt);
				goto update_planar;
			}

			pinfo->updated_bounce++;
			pinfo->num_render_grid = pinfo->num_grid;

			if (pinfo->updated_bounce < max_bounce) {
				/* Retag all grids to update for next bounce */
				for (int i = 1; (ob = pinfo->probes_grid_ref[i]) && (i < MAX_GRID); i++) {
					EEVEE_LightProbeEngineData *ped = EEVEE_lightprobe_data_get(ob);
					ped->need_update = true;
					ped->updated_cells = 0;
				}
				SWAP(GPUTexture *, sldata->irradiance_pool, sldata->irradiance_rt);
			}
		}

		for (int i = 1; (ob = pinfo->probes_cube_ref[i]) && (i < MAX_PROBE); i++) {
			EEVEE_LightProbeEngineData *ped = EEVEE_lightprobe_data_get(ob);
			KX_GameObject *probe = find_probe(scene, ob);
			float obmat[4][4];
			probe->NodeGetWorldTransform().getValue(&obmat[0][0]);

			LightProbe *prb = (LightProbe *)ob->data;

			render_scene_to_probe(sldata, vedata, obmat[3], prb->clipsta, prb->clipend, scene, rasty, cam);
			glossy_filter_probe(sldata, vedata, psl, i);

			ped->need_update = false;
			ped->probe_id = i;

			if (!ped->ready_to_shade) {
				pinfo->num_render_cube++;
				ped->ready_to_shade = true;
			}
			/* Only do one probe per frame */
			goto update_planar;
		}
	}

update_planar:

	for (int i = 0; (ob = pinfo->probes_planar_ref[i]) && (i < MAX_PLANAR); i++) {
		EEVEE_LightProbeEngineData *ped = EEVEE_lightprobe_data_get(ob);

		/* Temporary Remove all planar reflections (avoid lag effect). */
		int tmp_num_planar = pinfo->num_planar;
		pinfo->num_planar = 0;

		render_scene_to_planar(sldata, vedata, i, ped->viewmat, ped->persmat, ped->planer_eq_offset, scene, rasty, cam, inputofs);

		/* Restore */
		pinfo->num_planar = tmp_num_planar;

		ped->probe_id = i;
	}

	/* If there is at least one planar probe */
	if (pinfo->num_planar > 0 && (vedata->stl->effects->enabled_effects & EFFECT_SSR) != 0) {
		const int max_lod = 9;
		DRW_framebuffer_recursive_downsample(vedata->fbl->downsample_fb, txl->planar_pool, max_lod, &downsample_planar, vedata);
		/* For shading, save max level of the planar map */
		pinfo->lod_planar_max = (float)(max_lod);
	}
}

/***********************END OF EEVEE PROBE SYSTEM*******************/

void KX_KetsjiEngine::Render()
{
	m_logger.StartLog(tc_rasterizer, m_kxsystem->GetTimeInSeconds());

	BeginFrame();

	for (KX_Scene *scene : m_scenes) {
		// shadow buffers
		RenderShadowBuffers(scene);
		// Render only independent texture renderers here.
// 		scene->RenderTextureRenderers(KX_TextureRendererManager::VIEWPORT_INDEPENDENT, m_rasterizer, nullptr, nullptr, RAS_Rect(), RAS_Rect());
	}

	std::vector<FrameRenderData> frameDataList;
	const bool renderpereye = GetFrameRenderData(frameDataList);

	// Update all off screen to the current canvas size.
	m_rasterizer->UpdateOffScreens(m_canvas);

	const int width = m_canvas->GetWidth();
	const int height = m_canvas->GetHeight();

	// clear the entire game screen with the border color
	// only once per frame
	m_rasterizer->SetViewport(0, 0, width + 1, height + 1);
	m_rasterizer->SetScissor(0, 0, width + 1, height + 1);

	KX_Scene *firstscene = m_scenes->GetFront();
	const RAS_FrameSettings &framesettings = firstscene->GetFramingType();
	// Use the framing bar color set in the Blender scenes
	m_rasterizer->SetClearColor(framesettings.BarRed(), framesettings.BarGreen(), framesettings.BarBlue(), 1.0f);

	// Used to detect when a camera is the first rendered an then doesn't request a depth clear.
	unsigned short pass = 0;

	for (FrameRenderData& frameData : frameDataList) {
		// Current bound off screen.
		RAS_OffScreen *offScreen = m_rasterizer->GetOffScreen(frameData.m_ofsType);
		offScreen->Bind();

		// Clear off screen only before the first scene render.
		m_rasterizer->Clear(RAS_Rasterizer::RAS_COLOR_BUFFER_BIT | RAS_Rasterizer::RAS_DEPTH_BUFFER_BIT);

		// for each scene, call the proceed functions
		for (unsigned short i = 0, size = frameData.m_sceneDataList.size(); i < size; ++i) {
			const SceneRenderData& sceneFrameData = frameData.m_sceneDataList[i];
			KX_Scene *scene = sceneFrameData.m_scene;

			const bool isfirstscene = (i == 0);
			const bool islastscene = (i == (size - 1));

			// pass the scene's worldsettings to the rasterizer
			scene->GetWorldInfo()->UpdateWorldSettings(m_rasterizer);

			m_rasterizer->SetAuxilaryClientInfo(scene);

			// Draw the scene once for each camera with an enabled viewport or an active camera.
			for (const CameraRenderData& cameraFrameData : sceneFrameData.m_cameraDataList) {
				// do the rendering
				RenderCamera(scene, cameraFrameData, offScreen, pass++, isfirstscene);
			}

			/*****************************PROBES*****************************/
			EEVEE_Data *vedata = EEVEE_engine_data_get();
			EEVEE_SceneLayerData *sldata = EEVEE_scene_layer_data_get();
			EEVEE_lightprobes_updates(sldata, vedata->psl, vedata->stl, scene);
			EEVEE_planar_reflections_updates(sldata, vedata->psl, vedata->txl, scene->GetActiveCamera(), scene);

			DRW_uniformbuffer_update(sldata->probe_ubo, &sldata->probes->probe_data);
			DRW_uniformbuffer_update(sldata->grid_ubo, &sldata->probes->grid_data);
			DRW_uniformbuffer_update(sldata->planar_ubo, &sldata->probes->planar_data);
			EEVEE_lightprobes_refresh_bge(sldata, vedata, scene, m_rasterizer, scene->GetActiveCamera(), offScreen);
			/*************************END OF PROBES**************************/


			// Choose final render off screen target.
			RAS_Rasterizer::OffScreenType target;
			if (offScreen->GetSamples() > 0) {
				/* If the last scene is rendered it's useless to specify a multisamples off screen, we use then
				 * a non-multisamples off screen and avoid an extra off screen blit. */
				if (islastscene) {
					target = RAS_Rasterizer::NextRenderOffScreen(frameData.m_ofsType);
				}
				/* If the current off screen is using multisamples we are sure that it will be copied to a
				 * non-multisamples off screen before render the filters.
				 * In this case the targeted off screen is the same as the current off screen. */
				else {
					target = frameData.m_ofsType;
				}
			}
			/* In case of non-multisamples a ping pong per scene render is made between a potentially multisamples
			 * off screen and a non-multisamples off screen as the both doesn't use multisamples. */
			else {
				target = RAS_Rasterizer::NextRenderOffScreen(frameData.m_ofsType);
			}

			// Render EEVEE effects before tonemapping and custom filters
			scene->SetIsLastScene(scene == m_scenes->GetBack());
			offScreen = PostRenderEevee(scene, offScreen);
			target = RAS_Rasterizer::NextRenderOffScreen(offScreen->GetType());
			// Render filters and get output off screen.
			offScreen = PostRenderScene(scene, offScreen, m_rasterizer->GetOffScreen(target));
			frameData.m_ofsType = offScreen->GetType();
		}
	}

	// Compositing per eye off screens to screen.
	if (renderpereye) {
		RAS_OffScreen *leftofs = m_rasterizer->GetOffScreen(frameDataList[0].m_ofsType);
		RAS_OffScreen *rightofs = m_rasterizer->GetOffScreen(frameDataList[1].m_ofsType);
		m_rasterizer->DrawStereoOffScreen(m_canvas, leftofs, rightofs);
	}
	// Else simply draw the off screen to screen.
	else {
		m_rasterizer->DrawOffScreen(m_canvas, m_rasterizer->GetOffScreen(frameDataList[0].m_ofsType));
	}

// 	m_rasterizer->BindViewport(m_canvas);
// 	m_rasterizer->UnbindViewport(m_canvas);

	EndFrame();
}

void KX_KetsjiEngine::RequestExit(KX_ExitRequest exitrequestmode)
{
	m_exitcode = exitrequestmode;
}

void KX_KetsjiEngine::SetNameNextGame(const std::string& nextgame)
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

const std::string& KX_KetsjiEngine::GetExitString()
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
	const bool overrideCamera = (m_flags & CAMERA_OVERRIDE) && (scene->GetName() == m_overrideSceneName) &&
		(camera->GetName() == "__default__cam__");

	return overrideCamera ? m_overrideCamZoom : m_cameraZoom;
}

void KX_KetsjiEngine::EnableCameraOverride(const std::string& forscene, const MT_Matrix4x4& projmat,
		const MT_Matrix4x4& viewmat, const RAS_CameraData& camdata)
{
	SetFlag(CAMERA_OVERRIDE, true);
	m_overrideSceneName = forscene;
	m_overrideCamProjMat = projmat;
	m_overrideCamViewMat = viewmat;
	m_overrideCamData = camdata;
}


void KX_KetsjiEngine::GetSceneViewport(KX_Scene *scene, KX_Camera *cam, const RAS_Rect& displayArea, RAS_Rect& area, RAS_Rect& viewport)
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

		RAS_FramingManager::ComputeViewport(
		    scene->GetFramingType(),
		    userviewport,
		    viewport
		    );

		area = userviewport;
	}
	else if (!(m_flags & CAMERA_OVERRIDE) || (scene->GetName() != m_overrideSceneName) || !m_overrideCamData.m_perspective) {
		RAS_FramingManager::ComputeViewport(
		    scene->GetFramingType(),
			displayArea,
		    viewport);

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
	if (scene->IsSuspended()) {
		return;
	}

	// Handle the animations independently of the logic time step
	if (m_flags & RESTRICT_ANIMATION) {
		double anim_timestep = 1.0 / scene->GetAnimationFPS();
		if (m_frameTime - m_previousAnimTime > anim_timestep || m_frameTime == m_previousAnimTime) {
			// Sanity/debug print to make sure we're actually going at the fps we want (should be close to anim_timestep)
			// CM_Debug("Anim fps: " << 1.0/(m_frameTime - m_previousAnimTime));
			m_previousAnimTime = m_frameTime;
			for (KX_Scene *scene : m_scenes) {
				scene->UpdateAnimations(m_frameTime);
			}
		}
	}
	else
		scene->UpdateAnimations(m_frameTime);
}

/***********************EEVEE SHADOWS SYSTEM************************/

typedef struct EEVEE_LightData {
	short light_id, shadow_id;
} EEVEE_LightData;

typedef struct EEVEE_ShadowCubeData {
	short light_id, shadow_id, cube_id, layer_id;
} EEVEE_ShadowCubeData;

typedef struct EEVEE_ShadowCascadeData {
	short light_id, shadow_id, cascade_id, layer_id;
	float viewprojmat[MAX_CASCADE_NUM][4][4]; /* World->Lamp->NDC : used for rendering the shadow map. */
	float radius[MAX_CASCADE_NUM];
} EEVEE_ShadowCascadeData;

#define LERP(t, a, b) ((a) + (t) * ((b) - (a)))

enum LightShadowType {
	SHADOW_CUBE = 0,
	SHADOW_CASCADE
};

/* Update buffer with lamp data */
static void eevee_light_setup(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led)
{
	/* TODO only update if data changes */
	EEVEE_LightData *evld = (EEVEE_LightData *)led->storage;
	EEVEE_Light *evli = linfo->light_data + evld->light_id;
	Object *ob = kxlight->GetBlenderObject();
	Lamp *la = (Lamp *)ob->data;
	float mat[4][4], scale[3], power, obmat[4][4];

	kxlight->NodeGetWorldTransform().getValue(&obmat[0][0]);

	/* Position */
	copy_v3_v3(evli->position, obmat[3]);

	/* Color */
	copy_v3_v3(evli->color, &la->r);

	/* Influence Radius */
	evli->dist = la->dist;

	/* Vectors */
	normalize_m4_m4_ex(mat, obmat, scale);
	copy_v3_v3(evli->forwardvec, mat[2]);
	normalize_v3(evli->forwardvec);
	negate_v3(evli->forwardvec);

	copy_v3_v3(evli->rightvec, mat[0]);
	normalize_v3(evli->rightvec);

	copy_v3_v3(evli->upvec, mat[1]);
	normalize_v3(evli->upvec);

	/* Spot size & blend */
	if (la->type == LA_SPOT) {
		evli->sizex = scale[0] / scale[2];
		evli->sizey = scale[1] / scale[2];
		evli->spotsize = cosf(la->spotsize * 0.5f);
		evli->spotblend = (1.0f - evli->spotsize) * la->spotblend;
		evli->radius = max_ff(0.001f, la->area_size);
	}
	else if (la->type == LA_AREA) {
		evli->sizex = max_ff(0.0001f, la->area_size * scale[0] * 0.5f);
		if (la->area_shape == LA_AREA_RECT) {
			evli->sizey = max_ff(0.0001f, la->area_sizey * scale[1] * 0.5f);
		}
		else {
			evli->sizey = max_ff(0.0001f, la->area_size * scale[1] * 0.5f);
		}
	}
	else {
		evli->radius = max_ff(0.001f, la->area_size);
	}

	/* Make illumination power constant */
	if (la->type == LA_AREA) {
		power = 1.0f / (evli->sizex * evli->sizey * 4.0f * M_PI) /* 1/(w*h*Pi) */
			* 80.0f; /* XXX : Empirical, Fit cycles power */
	}
	else if (la->type == LA_SPOT || la->type == LA_LOCAL) {
		power = 1.0f / (4.0f * evli->radius * evli->radius * M_PI * M_PI) /* 1/(4*r²*Pi²) */
			* M_PI * M_PI * M_PI * 10.0; /* XXX : Empirical, Fit cycles power */

		/* for point lights (a.k.a radius == 0.0) */
		// power = M_PI * M_PI * 0.78; /* XXX : Empirical, Fit cycles power */
	}
	else {
		power = 1.0f;
	}
	mul_v3_fl(evli->color, power * la->energy);

	/* Lamp Type */
	evli->lamptype = (float)la->type;

	/* No shadow by default */
	evli->shadowid = -1.0f;
}

static void eevee_shadow_cube_setup(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led)
{
	EEVEE_ShadowCubeData *sh_data = (EEVEE_ShadowCubeData *)led->storage;
	EEVEE_Light *evli = linfo->light_data + sh_data->light_id;
	EEVEE_Shadow *ubo_data = linfo->shadow_data + sh_data->shadow_id;
	EEVEE_ShadowCube *cube_data = linfo->shadow_cube_data + sh_data->cube_id;
	Object *ob = kxlight->GetBlenderObject();
	Lamp *la = (Lamp *)ob->data;

	float obmat[4][4];
	kxlight->NodeGetWorldTransform().getValue(&obmat[0][0]);

	int sh_nbr = 1; /* TODO: MSM */

	for (int i = 0; i < sh_nbr; ++i) {
		/* TODO : choose MSM sample point here. */
		copy_v3_v3(cube_data->position, obmat[3]);
	}

	ubo_data->bias = 0.05f * la->bias;
	ubo_data->nearf = la->clipsta;
	ubo_data->farf = la->clipend;
	ubo_data->exp = (linfo->shadow_method == SHADOW_VSM) ? la->bleedbias : la->bleedexp;

	evli->shadowid = (float)(sh_data->shadow_id);
	ubo_data->shadow_start = (float)(sh_data->layer_id);
	ubo_data->data_start = (float)(sh_data->cube_id);
	ubo_data->multi_shadow_count = (float)(sh_nbr);
}

static void frustum_min_bounding_sphere(const float corners[8][4], float r_center[3], float *r_radius)
{
#if 0 /* Simple solution but waist too much space. */
	float minvec[3], maxvec[3];

	/* compute the bounding box */
	INIT_MINMAX(minvec, maxvec);
	for (int i = 0; i < 8; ++i)	{
		minmax_v3v3_v3(minvec, maxvec, corners[i]);
	}

	/* compute the bounding sphere of this box */
	r_radius = len_v3v3(minvec, maxvec) * 0.5f;
	add_v3_v3v3(r_center, minvec, maxvec);
	mul_v3_fl(r_center, 0.5f);
#else
	/* Make the bouding sphere always centered on the front diagonal */
	add_v3_v3v3(r_center, corners[4], corners[7]);
	mul_v3_fl(r_center, 0.5f);
	*r_radius = len_v3v3(corners[0], r_center);

	/* Search the largest distance between the sphere center
	* and the front plane corners. */
	for (int i = 0; i < 4; ++i) {
		float rad = len_v3v3(corners[4 + i], r_center);
		if (rad > *r_radius) {
			*r_radius = rad;
		}
	}
#endif
}

static void eevee_shadow_cascade_setup(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led, KX_Scene *scene)
{
	Object *ob = kxlight->GetBlenderObject();
	Lamp *la = (Lamp *)ob->data;

	float obmat[4][4];
	kxlight->NodeGetWorldTransform().getValue(&obmat[0][0]);

	/* Camera Matrices */
	float persmat[4][4], persinv[4][4];
	float viewprojmat[4][4], projinv[4][4];
	float view_near, view_far;
	float near_v[4] = { 0.0f, 0.0f, -1.0f, 1.0f };
	float far_v[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	bool is_persp = DRW_viewport_is_persp_get();

	KX_Camera *cam = scene->GetActiveCamera();

	MT_Matrix4x4 proj(cam->GetProjectionMatrix());
	MT_Matrix4x4 mvp(proj * cam->GetModelviewMatrix());

	proj.getValue(&persmat[0][0]);

	invert_m4_m4(persinv, persmat);
	/* FIXME : Get near / far from Draw manager? */

	mvp.getValue(&viewprojmat[0][0]);

	invert_m4_m4(projinv, viewprojmat);
	mul_m4_v4(projinv, near_v);
	mul_m4_v4(projinv, far_v);
	view_near = near_v[2];
	view_far = far_v[2]; /* TODO: Should be a shadow parameter */
	if (is_persp) {
		view_near /= near_v[3];
		view_far /= far_v[3];
	}

	/* Lamps Matrices */
	float viewmat[4][4], projmat[4][4];
	int sh_nbr = 1; /* TODO : MSM */
	int cascade_nbr = la->cascade_count;

	EEVEE_ShadowCascadeData *sh_data = (EEVEE_ShadowCascadeData *)led->storage;
	EEVEE_Light *evli = linfo->light_data + sh_data->light_id;
	EEVEE_Shadow *ubo_data = linfo->shadow_data + sh_data->shadow_id;
	EEVEE_ShadowCascade *cascade_data = linfo->shadow_cascade_data + sh_data->cascade_id;

	/* The technique consists into splitting
	* the view frustum into several sub-frustum
	* that are individually receiving one shadow map */

	float csm_start, csm_end;

	if (is_persp) {
		csm_start = view_near;
		csm_end = max_ff(view_far, -la->cascade_max_dist);
		/* Avoid artifacts */
		csm_end = min_ff(view_near, csm_end);
	}
	else {
		csm_start = -view_far;
		csm_end = view_far;
	}

	/* init near/far */
	for (int c = 0; c < MAX_CASCADE_NUM; ++c) {
		cascade_data->split_start[c] = csm_end;
		cascade_data->split_end[c] = csm_end;
	}

	/* Compute split planes */
	float splits_start_ndc[MAX_CASCADE_NUM];
	float splits_end_ndc[MAX_CASCADE_NUM];

	{
		/* Nearest plane */
		float p[4] = { 1.0f, 1.0f, csm_start, 1.0f };
		/* TODO: we don't need full m4 multiply here */
		mul_m4_v4(viewprojmat, p);
		splits_start_ndc[0] = p[2];
		if (is_persp) {
			splits_start_ndc[0] /= p[3];
		}
	}

	{
		/* Farthest plane */
		float p[4] = { 1.0f, 1.0f, csm_end, 1.0f };
		/* TODO: we don't need full m4 multiply here */
		mul_m4_v4(viewprojmat, p);
		splits_end_ndc[cascade_nbr - 1] = p[2];
		if (is_persp) {
			splits_end_ndc[cascade_nbr - 1] /= p[3];
		}
	}

	cascade_data->split_start[0] = csm_start;
	cascade_data->split_end[cascade_nbr - 1] = csm_end;

	for (int c = 1; c < cascade_nbr; ++c) {
		/* View Space */
		float linear_split = LERP(((float)(c) / (float)cascade_nbr), csm_start, csm_end);
		float exp_split = csm_start * powf(csm_end / csm_start, (float)(c) / (float)cascade_nbr);

		if (is_persp) {
			cascade_data->split_start[c] = LERP(la->cascade_exponent, linear_split, exp_split);
		}
		else {
			cascade_data->split_start[c] = linear_split;
		}
		cascade_data->split_end[c - 1] = cascade_data->split_start[c];

		/* Add some overlap for smooth transition */
		cascade_data->split_start[c] = LERP(la->cascade_fade, cascade_data->split_end[c - 1],
			(c > 1) ? cascade_data->split_end[c - 2] : cascade_data->split_start[0]);

		/* NDC Space */
		{
			float p[4] = { 1.0f, 1.0f, cascade_data->split_start[c], 1.0f };
			/* TODO: we don't need full m4 multiply here */
			mul_m4_v4(viewprojmat, p);
			splits_start_ndc[c] = p[2];

			if (is_persp) {
				splits_start_ndc[c] /= p[3];
			}
		}

		{
			float p[4] = { 1.0f, 1.0f, cascade_data->split_end[c - 1], 1.0f };
			/* TODO: we don't need full m4 multiply here */
			mul_m4_v4(viewprojmat, p);
			splits_end_ndc[c - 1] = p[2];

			if (is_persp) {
				splits_end_ndc[c - 1] /= p[3];
			}
		}
	}

	/* Set last cascade split fade distance into the first split_start. */
	float prev_split = (cascade_nbr > 1) ? cascade_data->split_end[cascade_nbr - 2] : cascade_data->split_start[0];
	cascade_data->split_start[0] = LERP(la->cascade_fade, cascade_data->split_end[cascade_nbr - 1], prev_split);

	/* For each cascade */
	for (int c = 0; c < cascade_nbr; ++c) {
		/* Given 8 frustum corners */
		float corners[8][4] = {
			/* Near Cap */
			{ -1.0f, -1.0f, splits_start_ndc[c], 1.0f },
			{ 1.0f, -1.0f, splits_start_ndc[c], 1.0f },
			{ -1.0f, 1.0f, splits_start_ndc[c], 1.0f },
			{ 1.0f, 1.0f, splits_start_ndc[c], 1.0f },
			/* Far Cap */
			{ -1.0f, -1.0f, splits_end_ndc[c], 1.0f },
			{ 1.0f, -1.0f, splits_end_ndc[c], 1.0f },
			{ -1.0f, 1.0f, splits_end_ndc[c], 1.0f },
			{ 1.0f, 1.0f, splits_end_ndc[c], 1.0f }
		};

		/* Transform them into world space */
		for (int i = 0; i < 8; ++i)	{
			mul_m4_v4(persinv, corners[i]);
			mul_v3_fl(corners[i], 1.0f / corners[i][3]);
			corners[i][3] = 1.0f;
		}


		/* Project them into light space */
		invert_m4_m4(viewmat, obmat);
		normalize_v3(viewmat[0]);
		normalize_v3(viewmat[1]);
		normalize_v3(viewmat[2]);

		for (int i = 0; i < 8; ++i)	{
			mul_m4_v4(viewmat, corners[i]);
		}

		float center[3];
		frustum_min_bounding_sphere(corners, center, &(sh_data->radius[c]));

		/* Snap projection center to nearest texel to cancel shimmering. */
		float shadow_origin[2], shadow_texco[2];
		mul_v2_v2fl(shadow_origin, center, linfo->shadow_size / (2.0f * sh_data->radius[c])); /* Light to texture space. */

		/* Find the nearest texel. */
		shadow_texco[0] = round(shadow_origin[0]);
		shadow_texco[1] = round(shadow_origin[1]);

		/* Compute offset. */
		sub_v2_v2(shadow_texco, shadow_origin);
		mul_v2_fl(shadow_texco, (2.0f * sh_data->radius[c]) / linfo->shadow_size); /* Texture to light space. */

		/* Apply offset. */
		add_v2_v2(center, shadow_texco);

		/* Expand the projection to cover frustum range */
		orthographic_m4(projmat,
			center[0] - sh_data->radius[c],
			center[0] + sh_data->radius[c],
			center[1] - sh_data->radius[c],
			center[1] + sh_data->radius[c],
			la->clipsta, la->clipend);

		mul_m4_m4m4(sh_data->viewprojmat[c], projmat, viewmat);
		mul_m4_m4m4(cascade_data->shadowmat[c], texcomat, sh_data->viewprojmat[c]);
	}

	ubo_data->bias = 0.05f * la->bias;
	ubo_data->nearf = la->clipsta;
	ubo_data->farf = la->clipend;
	ubo_data->exp = (linfo->shadow_method == SHADOW_VSM) ? la->bleedbias : la->bleedexp;

	evli->shadowid = (float)(sh_data->shadow_id);
	ubo_data->shadow_start = (float)(sh_data->layer_id);
	ubo_data->data_start = (float)(sh_data->cascade_id);
	ubo_data->multi_shadow_count = (float)(sh_nbr);
}

void KX_KetsjiEngine::RenderShadowBuffers(KX_Scene *scene)
{
	CListValue<KX_LightObject> *lightlist = scene->GetLightList();

	m_rasterizer->SetAuxilaryClientInfo(scene);
	m_rasterizer->Disable(RAS_Rasterizer::RAS_SCISSOR_TEST);

	RAS_SceneLayerData *layerData = scene->GetSceneLayerData();
	const EEVEE_SceneLayerData *sldata = &scene->GetSceneLayerData()->GetData();
	EEVEE_PassList *psl = EEVEE_engine_data_get()->psl;

	const bool textured = (m_rasterizer->GetDrawingMode() == RAS_Rasterizer::RAS_TEXTURED);

	EEVEE_LampsInfo *linfo = scene->GetSceneLayerData()->GetData().lamps;
	
	float clear_col[4] = { FLT_MAX };

	for (KX_LightObject *light : lightlist) {
		if (!light->GetVisible()) {
			continue;
		}

		RAS_ILightObject *raslight = light->GetLightData();
		Object *ob = light->GetBlenderObject();
		EEVEE_LampsInfo *linfo = layerData->GetData().lamps;
		EEVEE_LampEngineData *led = EEVEE_lamp_data_get(ob);
		LightShadowType shadowtype = linfo->shadow_cascade_ref[linfo->cpu_cascade_ct] == ob ? SHADOW_CASCADE : SHADOW_CUBE;

		eevee_light_setup(light, linfo, led);

		const bool useShadow = (textured && raslight->HasShadow());

		if (useShadow && raslight->NeedShadowUpdate()) {

			if (shadowtype == SHADOW_CUBE) {

				EEVEE_ShadowCubeData *sh_data = (EEVEE_ShadowCubeData *)led->storage;

				/* switch drawmode for speed */
				RAS_Rasterizer::DrawType drawmode = m_rasterizer->GetDrawingMode();
				m_rasterizer->SetDrawingMode(RAS_Rasterizer::RAS_SHADOW);

				/* Cube Shadow Maps */
				DRW_framebuffer_texture_attach(sldata->shadow_target_fb, sldata->shadow_cube_target, 0, 0);

				eevee_shadow_cube_setup(light, linfo, led);

				Lamp *la = (Lamp *)ob->data;

				float cube_projmat[4][4];
				perspective_m4(cube_projmat, -la->clipsta, la->clipsta, -la->clipsta, la->clipsta, la->clipsta, la->clipend);

				float obmat[4][4];
				light->NodeGetWorldTransform().getValue(&obmat[0][0]);

				EEVEE_ShadowRender *srd = &linfo->shadow_render_data;

				srd->clip_near = la->clipsta;
				srd->clip_far = la->clipend;
				copy_v3_v3(srd->position, obmat[3]);
				for (int j = 0; j < 6; j++) {
					float tmp[4][4];

					unit_m4(tmp);
					negate_v3_v3(tmp[3], obmat[3]);
					mul_m4_m4m4(srd->viewmat[j], cubefacemat[j], tmp);

					mul_m4_m4m4(srd->shadowmat[j], cube_projmat, srd->viewmat[j]);
				}
				DRW_uniformbuffer_update(sldata->shadow_render_ubo, srd);

				DRW_framebuffer_bind(sldata->shadow_target_fb);
				DRW_framebuffer_clear(true, true, false, clear_col, 1.0f);

				/* render */
				MT_Transform camtrans;
				KX_CullingNodeList nodes;
				const SG_Frustum frustum(light->GetShadowFrustumMatrix().inverse());
				/* update scene */
				scene->CalculateVisibleMeshes(nodes, frustum, raslight->GetShadowLayer());
				// Send a nullptr off screen because the viewport is binding it's using its own private one.
				scene->RenderBuckets(nodes, camtrans, m_rasterizer, nullptr);

				/* 0.001f is arbitrary, but it should be relatively small so that filter size is not too big. */
				float filter_texture_size = la->soft * 0.001f;
				float filter_pixel_size = ceil(filter_texture_size / linfo->shadow_render_data.cube_texel_size);
				linfo->filter_size = linfo->shadow_render_data.cube_texel_size * ((filter_pixel_size > 1.0f) ? 1.5f : 0.0f);

				/* TODO: OPTI: Filter all faces in one/two draw call */
				for (linfo->current_shadow_face = 0;
					linfo->current_shadow_face < 6;
					linfo->current_shadow_face++)
				{
					/* Copy using a small 3x3 box filter */
					DRW_framebuffer_cubeface_attach(sldata->shadow_store_fb, sldata->shadow_cube_blur, 0, linfo->current_shadow_face, 0);
					DRW_framebuffer_bind(sldata->shadow_store_fb);
					DRW_draw_pass(psl->shadow_cube_copy_pass);
					DRW_framebuffer_texture_detach(sldata->shadow_cube_blur);
				}

				/* Push it to shadowmap array */

				/* Adjust constants if concentric samples change. */
				const float max_filter_size = 7.5f;
				const float previous_box_filter_size = 9.0f; /* Dunno why but that works. */
				const int max_sample = 256;

				if (filter_pixel_size > 2.0f) {
					linfo->filter_size = linfo->shadow_render_data.cube_texel_size * max_filter_size * previous_box_filter_size;
					filter_pixel_size = max_ff(0.0f, filter_pixel_size - 3.0f);
					/* Compute number of concentric samples. Depends directly on filter size. */
					float pix_size_sqr = filter_pixel_size * filter_pixel_size;
					srd->shadow_samples_ct = min_ii(max_sample, 4 + 8 * (int)filter_pixel_size + 4 * (int)(pix_size_sqr));
				}
				else {
					linfo->filter_size = 0.0f;
					srd->shadow_samples_ct = 4;
				}
				srd->shadow_inv_samples_ct = 1.0f / (float)srd->shadow_samples_ct;
				DRW_uniformbuffer_update(sldata->shadow_render_ubo, srd);

				DRW_framebuffer_texture_layer_attach(sldata->shadow_store_fb, sldata->shadow_pool, 0, sh_data->layer_id, 0);
				DRW_framebuffer_bind(sldata->shadow_store_fb);
				DRW_draw_pass(psl->shadow_cube_store_pass);
				m_rasterizer->SetDrawingMode(drawmode);
				DRW_framebuffer_texture_detach(sldata->shadow_cube_target);
			}

			else if (shadowtype == SHADOW_CASCADE) {

				/* Cascaded Shadow Maps */
				DRW_framebuffer_texture_attach(sldata->shadow_target_fb, sldata->shadow_cascade_target, 0, 0);
				Lamp *la = (Lamp *)ob->data;

				EEVEE_ShadowCascadeData *evscd = (EEVEE_ShadowCascadeData *)led->storage;
				EEVEE_ShadowRender *srd = &linfo->shadow_render_data;

				/* switch drawmode for speed */
				RAS_Rasterizer::DrawType drawmode = m_rasterizer->GetDrawingMode();
				m_rasterizer->SetDrawingMode(RAS_Rasterizer::RAS_SHADOW);

				eevee_shadow_cascade_setup(light, linfo, led, scene);

				srd->clip_near = la->clipsta;
				srd->clip_far = la->clipend;
				for (int j = 0; j < la->cascade_count; ++j) {
					copy_m4_m4(srd->shadowmat[j], evscd->viewprojmat[j]);
				}
				DRW_uniformbuffer_update(sldata->shadow_render_ubo, &linfo->shadow_render_data);

				DRW_framebuffer_bind(sldata->shadow_target_fb);
				DRW_framebuffer_clear(false, true, false, NULL, 1.0);

				/* render */
				MT_Transform camtrans;
				KX_CullingNodeList nodes;
				const SG_Frustum frustum(light->GetShadowFrustumMatrix().inverse());
				/* update scene */
				scene->CalculateVisibleMeshes(nodes, frustum, raslight->GetShadowLayer());
				// Send a nullptr off screen because the viewport is binding it's using its own private one.
				scene->RenderBuckets(nodes, camtrans, m_rasterizer, nullptr);

				/* TODO: OPTI: Filter all cascade in one/two draw call */
				for (linfo->current_shadow_cascade = 0;
					linfo->current_shadow_cascade < la->cascade_count;
					++linfo->current_shadow_cascade)
				{
					/* 0.01f factor to convert to percentage */
					float filter_texture_size = la->soft * 0.01f / evscd->radius[linfo->current_shadow_cascade];
					float filter_pixel_size = ceil(linfo->shadow_size * filter_texture_size);

					/* Copy using a small 3x3 box filter */
					linfo->filter_size = linfo->shadow_render_data.stored_texel_size * ((filter_pixel_size > 1.0f) ? 1.0f : 0.0f);
					DRW_framebuffer_texture_layer_attach(sldata->shadow_store_fb, sldata->shadow_cascade_blur, 0, linfo->current_shadow_cascade, 0);
					DRW_framebuffer_bind(sldata->shadow_store_fb);
					DRW_draw_pass(psl->shadow_cascade_copy_pass);
					DRW_framebuffer_texture_detach(sldata->shadow_cascade_blur);

					/* Push it to shadowmap array and blur more */

					/* Adjust constants if concentric samples change. */
					const float max_filter_size = 7.5f;
					const float previous_box_filter_size = 3.2f; /* Arbitrary: less banding */
					const int max_sample = 256;

					if (filter_pixel_size > 2.0f) {
						linfo->filter_size = linfo->shadow_render_data.stored_texel_size * max_filter_size * previous_box_filter_size;
						filter_pixel_size = max_ff(0.0f, filter_pixel_size - 3.0f);
						/* Compute number of concentric samples. Depends directly on filter size. */
						float pix_size_sqr = filter_pixel_size * filter_pixel_size;
						srd->shadow_samples_ct = min_ii(max_sample, 4 + 8 * (int)filter_pixel_size + 4 * (int)(pix_size_sqr));
					}
					else {
						linfo->filter_size = 0.0f;
						srd->shadow_samples_ct = 4;
					}
					srd->shadow_inv_samples_ct = 1.0f / (float)srd->shadow_samples_ct;
					DRW_uniformbuffer_update(sldata->shadow_render_ubo, &linfo->shadow_render_data);

					int layer = evscd->layer_id + linfo->current_shadow_cascade;
					DRW_framebuffer_texture_layer_attach(sldata->shadow_store_fb, sldata->shadow_pool, 0, layer, 0);
					DRW_framebuffer_bind(sldata->shadow_store_fb);
					DRW_draw_pass(psl->shadow_cascade_store_pass);
				}
				m_rasterizer->SetDrawingMode(drawmode);
				DRW_framebuffer_texture_detach(sldata->shadow_cascade_target);
			}
		}
	}
	DRW_uniformbuffer_update(sldata->light_ubo, &linfo->light_data);
	DRW_uniformbuffer_update(sldata->shadow_ubo, &linfo->shadow_data);
	m_rasterizer->Enable(RAS_Rasterizer::RAS_SCISSOR_TEST);
}

/***********************END OF EEVEE SHADOWS SYSTEM************************/

MT_Matrix4x4 KX_KetsjiEngine::GetCameraProjectionMatrix(KX_Scene *scene, KX_Camera *cam, RAS_Rasterizer::StereoEye eye,
											const RAS_Rect& viewport, const RAS_Rect& area) const
{
	if (cam->hasValidProjectionMatrix()) {
		return cam->GetProjectionMatrix();
	}

	const bool override_camera = (m_flags & CAMERA_OVERRIDE) && (scene->GetName() == m_overrideSceneName) &&
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

			RAS_FramingManager::ComputeOrtho(
			    scene->GetFramingType(),
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
			RAS_FramingManager::ComputeFrustum(
			    scene->GetFramingType(),
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
			projmat = m_rasterizer->GetFrustumMatrix(
				eye, frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar, focallength);
		}
	}

	return projmat;
}

// update graphics
void KX_KetsjiEngine::RenderCamera(KX_Scene *scene, const CameraRenderData& cameraFrameData, RAS_OffScreen *offScreen,
								  unsigned short pass, bool isFirstScene)
{
	KX_Camera *rendercam = cameraFrameData.m_renderCamera;
	KX_Camera *cullingcam = cameraFrameData.m_cullingCamera;
	const RAS_Rect &area = cameraFrameData.m_area;
	const RAS_Rect &viewport = cameraFrameData.m_viewport;

	KX_SetActiveScene(scene);

	/* Render texture probes depending of the the current viewport and area, these texture probes are commonly the planar map
	 * which need to be recomputed by each view in case of multi-viewport or stereo.
	 */
// 	scene->RenderTextureRenderers(KX_TextureRendererManager::VIEWPORT_DEPENDENT, m_rasterizer, offScreen, rendercam, viewport, area);

	// set the viewport for this frame and scene
	const int left = viewport.GetLeft();
	const int bottom = viewport.GetBottom();
	const int width = viewport.GetWidth();
	const int height = viewport.GetHeight();
	m_rasterizer->SetViewport(left, bottom, width + 1, height + 1);
	m_rasterizer->SetScissor(left, bottom, width + 1, height + 1);

	/* Clear the depth after setting the scene viewport/scissor
	 * if it's not the first render pass. */
	if (pass > 0) {
		m_rasterizer->Clear(RAS_Rasterizer::RAS_DEPTH_BUFFER_BIT);
	}

	m_rasterizer->SetEye(cameraFrameData.m_eye);

	m_rasterizer->SetMatrix(rendercam->GetModelviewMatrix(), rendercam->GetProjectionMatrix(),
							rendercam->NodeGetWorldPosition(), rendercam->NodeGetLocalScaling());

	if (isFirstScene) {
		KX_WorldInfo *worldInfo = scene->GetWorldInfo();
		// Update background and render it.
		worldInfo->UpdateBackGround(m_rasterizer);
		worldInfo->RenderBackground(m_rasterizer);
	}

	// The following actually reschedules all vertices to be
	// redrawn. There is a cache between the actual rescheduling
	// and this call though. Visibility is imparted when this call
	// runs through the individual objects.

	m_logger.StartLog(tc_scenegraph, m_kxsystem->GetTimeInSeconds());

	KX_CullingNodeList nodes;
	scene->CalculateVisibleMeshes(nodes, cullingcam, 0);

	// update levels of detail
	scene->UpdateObjectLods(cullingcam, nodes);

	m_logger.StartLog(tc_animations, m_kxsystem->GetTimeInSeconds());
	UpdateAnimations(scene);

	m_logger.StartLog(tc_rasterizer, m_kxsystem->GetTimeInSeconds());

	RAS_DebugDraw& debugDraw = m_rasterizer->GetDebugDraw(scene);
	// Draw debug infos like bouding box, armature ect.. if enabled.
	scene->DrawDebug(debugDraw, nodes);
	// Draw debug camera frustum.
	DrawDebugCameraFrustum(scene, debugDraw, cameraFrameData);
	DrawDebugShadowFrustum(scene, debugDraw);

#ifdef WITH_PYTHON
	PHY_SetActiveEnvironment(scene->GetPhysicsEnvironment());
	// Run any pre-drawing python callbacks
	scene->RunDrawingCallbacks(KX_Scene::PRE_DRAW, rendercam);
#endif

	scene->RenderBuckets(nodes, rendercam->GetWorldToCamera(), m_rasterizer, offScreen);

	if (scene->GetPhysicsEnvironment())
		scene->GetPhysicsEnvironment()->DebugDrawWorld();
}

/*
 * To run once per scene
 */
RAS_OffScreen *KX_KetsjiEngine::PostRenderScene(KX_Scene *scene, RAS_OffScreen *inputofs, RAS_OffScreen *targetofs)
{
	KX_SetActiveScene(scene);

	m_rasterizer->FlushDebugDraw(scene, m_canvas);

	// We need to first make sure our viewport is correct (enabling multiple viewports can mess this up), only for filters.
	const int width = m_canvas->GetWidth();
	const int height = m_canvas->GetHeight();
	m_rasterizer->SetViewport(0, 0, width + 1, height + 1);
	m_rasterizer->SetScissor(0, 0, width + 1, height + 1);

	RAS_OffScreen *offScreen = scene->Render2DFilters(m_rasterizer, m_canvas, inputofs, targetofs);

#ifdef WITH_PYTHON
	PHY_SetActiveEnvironment(scene->GetPhysicsEnvironment());
	/* We can't deduce what camera should be passed to the python callbacks
	 * because the post draw callbacks are per scenes and not per cameras.
	 */
	scene->RunDrawingCallbacks(KX_Scene::POST_DRAW, nullptr);

	// Python draw callback can also call debug draw functions, so we have to clear debug shapes.
	m_rasterizer->FlushDebugDraw(scene, m_canvas);
#endif

	return offScreen;
}

RAS_OffScreen *KX_KetsjiEngine::PostRenderEevee(KX_Scene *scene, RAS_OffScreen *inputofs)
{
	KX_SetActiveScene(scene);

	// We need to first make sure our viewport is correct (enabling multiple viewports can mess this up), only for filters.
	const int width = m_canvas->GetWidth();
	const int height = m_canvas->GetHeight();
	m_rasterizer->SetViewport(0, 0, width + 1, height + 1);
	m_rasterizer->Disable(RAS_Rasterizer::RAS_SCISSOR_TEST);

	RAS_OffScreen *offScreen = scene->RenderEeveeEffects(m_rasterizer, inputofs);

	m_rasterizer->Enable(RAS_Rasterizer::RAS_SCISSOR_TEST);

	return offScreen;
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
	bool override_camera = ((m_flags & CAMERA_OVERRIDE) && (scene->GetName() == m_overrideSceneName));

	// if there is no activecamera, or the camera is being
	// overridden we need to construct a temporary camera
	if (!scene->GetActiveCamera() || override_camera) {
		KX_Camera *activecam = nullptr;

		activecam = new KX_Camera(scene, KX_Scene::m_callbacks, override_camera ? m_overrideCamData : RAS_CameraData());
		activecam->SetName("__default__cam__");

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
			activecam->NodeSetLocalPosition(MT_Vector3(0.0f, 0.0f, 0.0f));
			activecam->NodeSetLocalOrientation(MT_Matrix3x3(MT_Vector3(0.0f, 0.0f, 0.0f)));
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
	std::string debugtxt;
	int title_xmargin = -7;
	int title_y_top_margin = 4;
	int title_y_bottom_margin = 2;

	int const_xindent = 4;
	int const_ysize = 14;

	int xcoord = 12;    // mmmm, these constants were taken from blender source
	int ycoord = 17;    // to 'mimic' behavior

	int profile_indent = 72;

	float tottime = m_logger.GetAverage();
	if (tottime < 1e-6f) {
		tottime = 1e-6f;
	}

	static const MT_Vector4 white(1.0f, 1.0f, 1.0f, 1.0f);

	// Use nullptrfor no scene.
	RAS_DebugDraw& debugDraw = m_rasterizer->GetDebugDraw(nullptr);

	if (m_flags & (SHOW_FRAMERATE | SHOW_PROFILE)) {
		// Title for profiling("Profile")
		// Adds the constant x indent (0 for now) to the title x margin
		debugDraw.RenderText2D("Profile", MT_Vector2(xcoord + const_xindent + title_xmargin, ycoord), white);

		// Increase the indent by default increase
		ycoord += const_ysize;
		// Add the title indent afterwards
		ycoord += title_y_bottom_margin;
	}

	// Framerate display
	if (m_flags & SHOW_FRAMERATE) {
		debugDraw.RenderText2D("Frametime :",
		                           MT_Vector2(xcoord + const_xindent,
		                           ycoord), white);

		debugtxt = (boost::format("%5.2fms (%.1ffps)") %  (tottime * 1000.0f) % (1.0f / tottime)).str();
		debugDraw.RenderText2D(debugtxt, MT_Vector2(xcoord + const_xindent + profile_indent, ycoord), white);
		// Increase the indent by default increase
		ycoord += const_ysize;
	}

	// Profile display
	if (m_flags & SHOW_PROFILE) {
		for (int j = tc_first; j < tc_numCategories; j++) {
			debugDraw.RenderText2D(m_profileLabels[j], MT_Vector2(xcoord + const_xindent, ycoord), white);

			double time = m_logger.GetAverage((KX_TimeCategory)j);

			debugtxt = (boost::format("%5.2fms | %d%%") % (time*1000.f) % (int)(time/tottime * 100.f)).str();
			debugDraw.RenderText2D(debugtxt, MT_Vector2(xcoord + const_xindent + profile_indent, ycoord), white);

			const MT_Vector2 boxSize(50 * (time / tottime), 10);
			debugDraw.RenderBox2D(MT_Vector2(xcoord + (int)(2.2 * profile_indent), ycoord), boxSize, white);
			ycoord += const_ysize;
		}
	}
	// Add the ymargin for titles below the other section of debug info
	ycoord += title_y_top_margin;

	/* Property display */
	if (m_flags & SHOW_DEBUG_PROPERTIES) {
		// Title for debugging("Debug properties")
		// Adds the constant x indent (0 for now) to the title x margin
		debugDraw.RenderText2D("Debug Properties", MT_Vector2(xcoord + const_xindent + title_xmargin, ycoord), white);

		// Increase the indent by default increase
		ycoord += const_ysize;
		// Add the title indent afterwards
		ycoord += title_y_bottom_margin;

		/* Calculate amount of properties that can displayed. */
		const unsigned short propsMax = (m_canvas->GetHeight() - ycoord) / const_ysize;

		for (KX_Scene *scene : m_scenes) {
			scene->RenderDebugProperties(debugDraw, const_xindent, const_ysize, xcoord, ycoord, propsMax);
		}
	}

	m_rasterizer->FlushDebugDraw(nullptr, m_canvas);
}

void KX_KetsjiEngine::DrawDebugCameraFrustum(KX_Scene *scene, RAS_DebugDraw& debugDraw, const CameraRenderData& cameraFrameData)
{
	if (m_showCameraFrustum == KX_DebugOption::DISABLE) {
		return;
	}

	for (KX_Camera *cam : scene->GetCameraList()) {
		if (cam != cameraFrameData.m_renderCamera && (m_showCameraFrustum == KX_DebugOption::FORCE || cam->GetShowCameraFrustum())) {
			const MT_Matrix4x4 viewmat = m_rasterizer->GetViewMatrix(cameraFrameData.m_eye, cam->GetWorldToCamera(), cam->GetCameraData()->m_perspective);
			const MT_Matrix4x4 projmat = GetCameraProjectionMatrix(scene, cam, cameraFrameData.m_eye, cameraFrameData.m_viewport, cameraFrameData.m_area);
			debugDraw.DrawCameraFrustum(projmat * viewmat);
		}
	}
}

void KX_KetsjiEngine::DrawDebugShadowFrustum(KX_Scene *scene, RAS_DebugDraw& debugDraw)
{
	if (m_showShadowFrustum == KX_DebugOption::DISABLE) {
		return;
	}

	for (KX_LightObject *light : scene->GetLightList()) {
		RAS_ILightObject *raslight = light->GetLightData();
		if (m_showShadowFrustum == KX_DebugOption::FORCE || light->GetShowShadowFrustum()) {
			debugDraw.DrawCameraFrustum(light->GetShadowFrustumMatrix().inverse());
		}
	}
}

CListValue<KX_Scene> *KX_KetsjiEngine::CurrentScenes()
{
	return m_scenes;
}

KX_Scene *KX_KetsjiEngine::FindScene(const std::string& scenename)
{
	return m_scenes->FindValue(scenename);
}

void KX_KetsjiEngine::ConvertAndAddScene(const std::string& scenename, bool overlay)
{
	// only add scene when it doesn't exist!
	if (FindScene(scenename)) {
		CM_Warning("scene " << scenename << " already exists, not added!");
	}
	else {
		if (overlay) {
			m_addingOverlayScenes.push_back(scenename);
		}
		else {
			m_addingBackgroundScenes.push_back(scenename);
		}
	}
}

void KX_KetsjiEngine::RemoveScene(const std::string& scenename)
{
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
		for (scenenameit = m_removingScenes.begin(); scenenameit != m_removingScenes.end(); scenenameit++) {
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
	KX_Scene *tmpscene = new KX_Scene(m_inputDevice,
	                                  scene->id.name + 2,
	                                  scene,
	                                  m_canvas,
									  m_networkMessageManager);

	m_converter->ConvertScene(tmpscene, m_rasterizer, m_canvas, libloading);

	return tmpscene;
}

KX_Scene *KX_KetsjiEngine::CreateScene(const std::string& scenename)
{
	Scene *scene = m_converter->GetBlenderSceneForName(scenename);
	if (!scene)
		return nullptr;

	return CreateScene(scene, false);
}

void KX_KetsjiEngine::AddScheduledScenes()
{
	std::vector<std::string>::iterator scenenameit;

	if (m_addingOverlayScenes.size()) {
		for (scenenameit = m_addingOverlayScenes.begin();
		     scenenameit != m_addingOverlayScenes.end();
		     scenenameit++)
		{
			std::string scenename = *scenenameit;
			KX_Scene *tmpscene = CreateScene(scenename);
			if (tmpscene) {
				m_scenes->Add(CM_AddRef(tmpscene));
				PostProcessScene(tmpscene);
				tmpscene->Release();
			}
			else {
				CM_Warning("scene " << scenename << " could not be found, not added!");
			}
		}
		m_addingOverlayScenes.clear();
	}

	if (m_addingBackgroundScenes.size()) {
		for (scenenameit = m_addingBackgroundScenes.begin();
		     scenenameit != m_addingBackgroundScenes.end();
		     scenenameit++)
		{
			std::string scenename = *scenenameit;
			KX_Scene *tmpscene = CreateScene(scenename);
			if (tmpscene) {
				m_scenes->Insert(0, CM_AddRef(tmpscene));
				PostProcessScene(tmpscene);
				tmpscene->Release();
			}
			else {
				CM_Warning("scene " << scenename << " could not be found, not added!");
			}
		}
		m_addingBackgroundScenes.clear();
	}
}

bool KX_KetsjiEngine::ReplaceScene(const std::string& oldscene, const std::string& newscene)
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
		std::vector<std::pair<std::string, std::string> >::iterator scenenameit;

		for (scenenameit = m_replace_scenes.begin();
		     scenenameit != m_replace_scenes.end();
		     scenenameit++)
		{
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

void KX_KetsjiEngine::SuspendScene(const std::string& scenename)
{
	KX_Scene *scene = FindScene(scenename);
	if (scene) {
		scene->Suspend();
	}
}

void KX_KetsjiEngine::ResumeScene(const std::string& scenename)
{
	KX_Scene *scene = FindScene(scenename);
	if (scene) {
		scene->Resume();
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
	return m_kxsystem->GetTimeInSeconds();
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
	if (m_addingOverlayScenes.size() ||
	    m_addingBackgroundScenes.size() ||
	    m_replace_scenes.size() ||
	    m_removingScenes.size()) {

		// Change the scene list
		ReplaceScheduledScenes();
		RemoveScheduledScenes();
		AddScheduledScenes();
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
