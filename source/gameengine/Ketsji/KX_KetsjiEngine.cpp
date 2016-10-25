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

#include "BLI_task.h"

#include "KX_KetsjiEngine.h"

#include "EXP_ListValue.h"
#include "EXP_IntValue.h"
#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"

#include "RAS_BucketManager.h"
#include "RAS_Rect.h"
#include "RAS_IRasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_ILightObject.h"
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
#include "KX_ISceneConverter.h"
#include "KX_TimeCategoryLogger.h"

#include "RAS_FramingManager.h"
#include "DNA_world_types.h"
#include "DNA_scene_types.h"

#include "KX_NavMeshObject.h"

#define DEFAULT_LOGIC_TIC_RATE 60.0

#ifdef FREE_WINDOWS /* XXX mingw64 (gcc 4.7.0) defines a macro for DrawText that translates to DrawTextA. Not good */
#  ifdef DrawText
#    undef DrawText
#  endif
#endif

const char KX_KetsjiEngine::m_profileLabels[tc_numCategories][15] = {
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

double KX_KetsjiEngine::m_ticrate = DEFAULT_LOGIC_TIC_RATE;
int KX_KetsjiEngine::m_maxLogicFrame = 5;
int KX_KetsjiEngine::m_maxPhysicsFrame = 5;
double KX_KetsjiEngine::m_anim_framerate = 25.0;
double KX_KetsjiEngine::m_suspendedtime = 0.0;
double KX_KetsjiEngine::m_suspendeddelta = 0.0;
double KX_KetsjiEngine::m_average_framerate = 0.0;
bool KX_KetsjiEngine::m_restrict_anim_fps = false;
short KX_KetsjiEngine::m_exitkey = 130; // ESC Key
bool KX_KetsjiEngine::m_doRender = true;

/**
 * Constructor of the Ketsji Engine
 */
KX_KetsjiEngine::KX_KetsjiEngine(KX_ISystem *system)
	:m_canvas(NULL),
	m_rasterizer(NULL),
	m_kxsystem(system),
	m_sceneconverter(NULL),
#ifdef WITH_PYTHON
	m_pythondictionary(NULL),
#endif
	m_inputDevice(NULL),
	m_bInitialized(false),
	m_activecam(0),
	m_fixedFramerate(false),
	m_useExternalClock(false),
	m_firstframe(true),
	m_frameTime(0.0f),
	m_clockTime(0.0f),
	m_previousClockTime(0.0f),
	m_previousAnimTime(0.0f),
	m_timescale(1.0f),
	m_previousRealTime(0.0f),
	m_exitcode(KX_EXIT_REQUEST_NO_REQUEST),
	m_exitstring(""),
	m_cameraZoom(1.0f),
	m_overrideCam(false),
	m_overrideCamUseOrtho(false),
	m_overrideCamNear(0.0f),
	m_overrideCamFar(0.0f),
	m_overrideCamZoom(1.0f),
	m_stereo(false),
	m_curreye(0),
	m_logger(NULL),
	// Set up timing info display variables
	m_show_framerate(false),
	m_show_profile(false),
	m_showProperties(false),
	m_showBackground(false),
	m_show_debug_properties(false),
	m_autoAddDebugProperties(true),
	// Default behavior is to hide the cursor every frame.
	m_hideCursor(false),
	m_showBoundingBox(false),
	m_showArmature(false),
	m_overrideFrameColor(false),
	m_overrideFrameColorR(0.0f),
	m_overrideFrameColorG(0.0f),
	m_overrideFrameColorB(0.0f),
	m_overrideFrameColorA(0.0f)
{
	// Initialize the time logger
	m_logger = new KX_TimeCategoryLogger(25);

	for (int i = tc_first; i < tc_numCategories; i++)
		m_logger->AddCategory((KX_TimeCategory)i);

#ifdef WITH_PYTHON
	m_pyprofiledict = PyDict_New();
#endif

	m_taskscheduler = BLI_task_scheduler_create(TASK_SCHEDULER_AUTO_THREADS);

	m_scenes = new CListValue();
}

/**
 *	Destructor of the Ketsji Engine, release all memory
 */
KX_KetsjiEngine::~KX_KetsjiEngine()
{
	delete m_logger;

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

void KX_KetsjiEngine::SetRasterizer(RAS_IRasterizer *rasterizer)
{
	BLI_assert(rasterizer);
	m_rasterizer = rasterizer;
}

void KX_KetsjiEngine::SetNetworkMessageManager(KX_NetworkMessageManager *manager)
{
	m_networkMessageManager = manager;
}

#ifdef WITH_PYTHON
/*
 * At the moment the bge.logic module is imported into 'pythondictionary' after this function is called.
 * if this function ever changes to assign a copy, make sure the game logic module is imported into this dictionary before hand.
 */
void KX_KetsjiEngine::SetPyNamespace(PyObject *pythondictionary)
{
	BLI_assert(pythondictionary);
	m_pythondictionary = pythondictionary;
}

PyObject *KX_KetsjiEngine::GetPyProfileDict()
{
	Py_INCREF(m_pyprofiledict);
	return m_pyprofiledict;
}
#endif

void KX_KetsjiEngine::SetSceneConverter(KX_ISceneConverter *sceneconverter)
{
	BLI_assert(sceneconverter);
	m_sceneconverter = sceneconverter;
}

/**
 * Ketsji Init(), Initializes data-structures and converts data from
 * Blender into Ketsji native (realtime) format also sets up the
 * graphics context
 */
void KX_KetsjiEngine::StartEngine(bool clearIpo)
{
	m_clockTime = m_kxsystem->GetTimeInSeconds();
	m_frameTime = m_kxsystem->GetTimeInSeconds();
	m_previousClockTime = m_kxsystem->GetTimeInSeconds();
	m_previousRealTime = m_kxsystem->GetTimeInSeconds();

	m_firstframe = true;
	m_bInitialized = true;
	// there is always one scene enabled at startup
	Scene *scene = ((KX_Scene *)m_scenes->GetFront())->GetBlenderScene();
	if (scene) {
		m_ticrate = scene->gm.ticrate ? scene->gm.ticrate : DEFAULT_LOGIC_TIC_RATE;
		m_maxLogicFrame = scene->gm.maxlogicstep ? scene->gm.maxlogicstep : 5;
		m_maxPhysicsFrame = scene->gm.maxphystep ? scene->gm.maxlogicstep : 5;
	}
	else {
		m_ticrate = DEFAULT_LOGIC_TIC_RATE;
		m_maxLogicFrame = 5;
		m_maxPhysicsFrame = 5;
	}
}

void KX_KetsjiEngine::BeginFrame()
{
	m_rasterizer->BeginFrame(m_kxsystem->GetTimeInSeconds());

	m_canvas->BeginDraw();
}

void KX_KetsjiEngine::EndFrame()
{
	m_rasterizer->MotionBlur();

	// Show profiling info
	m_logger->StartLog(tc_overhead, m_kxsystem->GetTimeInSeconds(), true);
	if (m_show_framerate || m_show_profile || (m_show_debug_properties)) {
		RenderDebugProperties();
	}

	double tottime = m_logger->GetAverage();
	if (tottime < 1e-6)
		tottime = 1e-6;

#ifdef WITH_PYTHON
	for (int i = tc_first; i < tc_numCategories; ++i) {
		double time = m_logger->GetAverage((KX_TimeCategory)i);
		PyObject *val = PyTuple_New(2);
		PyTuple_SetItem(val, 0, PyFloat_FromDouble(time * 1000.0));
		PyTuple_SetItem(val, 1, PyFloat_FromDouble(time / tottime * 100.0));

		PyDict_SetItemString(m_pyprofiledict, m_profileLabels[i], val);
		Py_DECREF(val);
	}
#endif

	m_average_framerate = 1.0 / tottime;

	// Go to next profiling measurement, time spent after this call is shown in the next frame.
	m_logger->NextMeasurement(m_kxsystem->GetTimeInSeconds());

	m_logger->StartLog(tc_rasterizer, m_kxsystem->GetTimeInSeconds(), true);
	m_rasterizer->EndFrame();
	// swap backbuffer (drawing into this buffer) <-> front/visible buffer
	m_logger->StartLog(tc_latency, m_kxsystem->GetTimeInSeconds(), true);
	m_rasterizer->SwapBuffers(m_canvas);
	m_logger->StartLog(tc_rasterizer, m_kxsystem->GetTimeInSeconds(), true);

	m_canvas->EndDraw();
}

bool KX_KetsjiEngine::NextFrame()
{

	m_logger->StartLog(tc_services, m_kxsystem->GetTimeInSeconds(), true);

	/*
	 * Clock advancement. There is basically two case:
	 *   - m_useExternalClock is true, the user is responsible to advance the time
	 *   manually using setClockTime, so here, we do not do anything.
	 *   - m_useExternalClock is false, we consider how much
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
	if (!m_useExternalClock) {
		double current_time = m_kxsystem->GetTimeInSeconds();
		double dt = current_time - m_previousRealTime;
		m_previousRealTime = current_time;
		m_clockTime += dt * m_timescale;

		if (!m_fixedFramerate) {
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
	if (m_fixedFramerate) {
		frames = int(deltatime * m_ticrate / m_timescale + 1e-6);
	}

//	if (frames>1)
//		printf("****************************************");
//	printf("dt = %f, deltatime = %f, frames = %d\n",dt, deltatime,frames);
	
//	if (!frames)
//		PIL_sleep_ms(1);

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

		m_sceneconverter->MergeAsyncLoads();

		if (m_inputDevice) {
			m_inputDevice->ReleaseMoveEvent();
		}
#ifdef WITH_SDL
		// Handle all SDL Joystick events here to share them for all scenes properly.
		short addrem[JOYINDEX_MAX] = {0};
		if (DEV_Joystick::HandleEvents(addrem)) {
			updatePythonJoysticks(addrem);
		}
#endif

		// for each scene, call the proceed functions
		for (CListValue::iterator sceit = m_scenes->GetBegin(); sceit != m_scenes->GetEnd(); ++sceit) {
			KX_Scene *scene = (KX_Scene *)*sceit;

			/* Suspension holds the physics and logic processing for an
			 * entire scene. Objects can be suspended individually, and
			 * the settings for that precede the logic and physics
			 * update. */
			m_logger->StartLog(tc_logic, m_kxsystem->GetTimeInSeconds(), true);

			scene->UpdateObjectActivity();

			if (!scene->IsSuspended()) {
				m_logger->StartLog(tc_physics, m_kxsystem->GetTimeInSeconds(), true);
				SG_SetActiveStage(SG_STAGE_PHYSICS1);
				// set Python hooks for each scene
#ifdef WITH_PYTHON
				PHY_SetActiveEnvironment(scene->GetPhysicsEnvironment());
#endif
				KX_SetActiveScene(scene);

				scene->GetPhysicsEnvironment()->EndFrame();

				// Update scenegraph after physics step. This maps physics calculations
				// into node positions.
				//m_logger->StartLog(tc_scenegraph, m_kxsystem->GetTimeInSeconds(), true);
				//SG_SetActiveStage(SG_STAGE_PHYSICS1_UPDATE);
				//scene->UpdateParents(m_frameTime);

				// Process sensors, and controllers
				m_logger->StartLog(tc_logic, m_kxsystem->GetTimeInSeconds(), true);
				SG_SetActiveStage(SG_STAGE_CONTROLLER);
				scene->LogicBeginFrame(m_frameTime, framestep);

				// Scenegraph needs to be updated again, because Logic Controllers
				// can affect the local matrices.
				m_logger->StartLog(tc_scenegraph, m_kxsystem->GetTimeInSeconds(), true);
				SG_SetActiveStage(SG_STAGE_CONTROLLER_UPDATE);
				scene->UpdateParents(m_frameTime);

				// Process actuators

				// Do some cleanup work for this logic frame
				m_logger->StartLog(tc_logic, m_kxsystem->GetTimeInSeconds(), true);
				SG_SetActiveStage(SG_STAGE_ACTUATOR);
				scene->LogicUpdateFrame(m_frameTime, true);

				scene->LogicEndFrame();

				// Actuators can affect the scenegraph
				m_logger->StartLog(tc_scenegraph, m_kxsystem->GetTimeInSeconds(), true);
				SG_SetActiveStage(SG_STAGE_ACTUATOR_UPDATE);
				scene->UpdateParents(m_frameTime);

				m_logger->StartLog(tc_physics, m_kxsystem->GetTimeInSeconds(), true);
				SG_SetActiveStage(SG_STAGE_PHYSICS2);
				scene->GetPhysicsEnvironment()->BeginFrame();

				// Perform physics calculations on the scene. This can involve
				// many iterations of the physics solver.
				scene->GetPhysicsEnvironment()->ProceedDeltaTime(m_frameTime, timestep, framestep);//m_deltatimerealDeltaTime);

				m_logger->StartLog(tc_scenegraph, m_kxsystem->GetTimeInSeconds(), true);
				SG_SetActiveStage(SG_STAGE_PHYSICS2_UPDATE);
				scene->UpdateParents(m_frameTime);
			}

			m_logger->StartLog(tc_services, m_kxsystem->GetTimeInSeconds(), true);
		}

		m_logger->StartLog(tc_network, m_kxsystem->GetTimeInSeconds(), true);
		SG_SetActiveStage(SG_STAGE_NETWORK);
		m_networkMessageManager->ClearMessages();

		m_logger->StartLog(tc_services, m_kxsystem->GetTimeInSeconds(), true);

		// update system devices
		m_logger->StartLog(tc_logic, m_kxsystem->GetTimeInSeconds(), true);
		if (m_inputDevice) {
			m_inputDevice->ClearInputs();
		}

		UpdateSuspendedScenes();
		// scene management
		ProcessScheduledScenes();

		frames--;
	}

	// Start logging time spent outside main loop
	m_logger->StartLog(tc_outside, m_kxsystem->GetTimeInSeconds(), true);

	return doRender && m_doRender;
}

void KX_KetsjiEngine::UpdateSuspendedScenes()
{
	for (CListValue::iterator sceneit = m_scenes->GetBegin(); sceneit != m_scenes->GetEnd(); ++sceneit) {
		KX_Scene *scene = (KX_Scene *)*sceneit;
		if (scene->IsSuspended()) {
			if (scene->getSuspendedTime() == 0.0f) {
				scene->setSuspendedTime(m_clockTime);
			}
		}
		else {
			// if the scene was suspended recalcutlate the delta to "curtime"
			if (scene->getSuspendedTime() != 0.0f) {
				scene->setSuspendedDelta(scene->getSuspendedDelta() + m_clockTime - scene->getSuspendedTime());
			}
			scene->setSuspendedTime(0.0f);
		}
	}
}

void KX_KetsjiEngine::Render()
{
	KX_Scene *firstscene = (KX_Scene *)m_scenes->GetFront();
	const RAS_FrameSettings &framesettings = firstscene->GetFramingType();

	const int width = m_canvas->GetWidth();
	const int height = m_canvas->GetHeight();

	m_logger->StartLog(tc_rasterizer, m_kxsystem->GetTimeInSeconds(), true);
	SG_SetActiveStage(SG_STAGE_RENDER);

	// hiding mouse cursor each frame
	// (came back when going out of focus and then back in again)
	if (m_hideCursor)
		m_canvas->SetMouseState(RAS_ICanvas::MOUSE_INVISIBLE);

	BeginFrame();

	for (CListValue::iterator sceit = m_scenes->GetBegin(), sceend = m_scenes->GetEnd(); sceit != sceend; ++sceit) {
		KX_Scene *scene = (KX_Scene *)*sceit;
		// shadow buffers
		RenderShadowBuffers(scene);
		// cubemaps
		scene->RenderCubeMaps(m_rasterizer);
	}

	// Update all off screen to the current canvas size.
	m_rasterizer->UpdateOffScreens(m_canvas);
	// Bind render off screen as default.
	m_rasterizer->BindOffScreen(RAS_IRasterizer::RAS_OFFSCREEN_RENDER);

	// clear the entire game screen with the border color
	// only once per frame
	m_rasterizer->SetViewport(0, 0, width + 1, height + 1);
	m_rasterizer->SetScissor(0, 0, width + 1, height + 1);
	if (m_overrideFrameColor) {
		// Do not use the framing bar color set in the Blender scenes
		m_rasterizer->SetClearColor(
			m_overrideFrameColorR,
			m_overrideFrameColorG,
			m_overrideFrameColorB,
			m_overrideFrameColorA
			);
	}
	else {
		// Use the framing bar color set in the Blender scenes
		m_rasterizer->SetClearColor(
		    framesettings.BarRed(),
		    framesettings.BarGreen(),
		    framesettings.BarBlue(),
		    1.0f);
	}
	// clear the -whole- viewport
	m_rasterizer->Clear(RAS_IRasterizer::RAS_COLOR_BUFFER_BIT | RAS_IRasterizer::RAS_DEPTH_BUFFER_BIT);

	const RAS_IRasterizer::StereoMode stereomode = m_rasterizer->GetStereoMode();
	// Set to true when each eye needs to be rendered in a separated off screen.
	const bool renderpereye = stereomode == RAS_IRasterizer::RAS_STEREO_INTERLACED ||
							  stereomode == RAS_IRasterizer::RAS_STEREO_VINTERLACE ||
							  stereomode == RAS_IRasterizer::RAS_STEREO_ANAGLYPH;

	const unsigned short numeyepass = (stereomode != RAS_IRasterizer::RAS_STEREO_NOSTEREO) ? 2 : 1;

	// The current bound eye off screen if we are using per eye stereo.
	int eyefboindex[2] = {RAS_IRasterizer::RAS_OFFSCREEN_EYE_LEFT0, RAS_IRasterizer::RAS_OFFSCREEN_EYE_RIGHT0};

	// Used to detect when a camera is the first rendered an then doesn't request a depth clear.
	unsigned short pass = 0;

	// for each scene, call the proceed functions
	for (CListValue::iterator sceit = m_scenes->GetBegin(), sceend = m_scenes->GetEnd(); sceit != sceend; ++sceit) {
		KX_Scene *scene = (KX_Scene *)*sceit;
		KX_Camera *activecam = scene->GetActiveCamera();
		CListValue *cameras = scene->GetCameraList();

		const bool firstscene = (scene == m_scenes->GetFront());
		const bool lastscene = (scene == m_scenes->GetBack());

		// pass the scene's worldsettings to the rasterizer
		scene->GetWorldInfo()->UpdateWorldSettings(m_rasterizer);

		m_rasterizer->SetAuxilaryClientInfo(scene);

		for (unsigned short eyepass = 0; eyepass < numeyepass; ++eyepass) {
			m_rasterizer->SetEye((eyepass == 0) ? RAS_IRasterizer::RAS_STEREO_LEFTEYE : RAS_IRasterizer::RAS_STEREO_RIGHTEYE);
			// set the area used for rendering (stereo can assign only a subset)
			m_rasterizer->SetRenderArea(m_canvas);

			// Choose unique off screen per eyes in case of stereo.
			if (renderpereye) {
				m_rasterizer->BindOffScreen(eyefboindex[eyepass]);
				// Clear eye off screen only before the first scene render.
				if (firstscene) {
					m_rasterizer->Clear(RAS_IRasterizer::RAS_COLOR_BUFFER_BIT | RAS_IRasterizer::RAS_DEPTH_BUFFER_BIT);
				}
			}

			// Avoid drawing the scene with the active camera twice when its viewport is enabled
			if (activecam && !activecam->GetViewport()) {
				// do the rendering
				RenderFrame(scene, activecam, pass++);
			}

			// Draw the scene once for each camera with an enabled viewport
			for (CListValue::iterator it = cameras->GetBegin(), end = cameras->GetEnd(); it != end; ++it) {
				KX_Camera *cam = (KX_Camera*)(*it);
				if (cam->GetViewport()) {
					// do the rendering
					RenderFrame(scene, cam, pass++);
				}
			}

			// Process filters per eye off screen.
			if (renderpereye) {
				int target;
				if (m_rasterizer->GetOffScreenSamples(eyefboindex[eyepass]) > 0) {
					/* Only RAS_OFFSCREEN_EYE_[LEFT/RIGHT]0 has possible multisamples so we target
					 * RAS_OFFSCREEN_EYE_[LEFT/RIGHT]1 if it's the last scene. */
					if (lastscene) {
						target = RAS_IRasterizer::NextEyeOffScreen(eyefboindex[eyepass]);
					}
					/* In case of multisamples and filters we're sure that a blit to RAS_OFFSCREEN_FILTER0
					 * will be done so we can target the same off screen than in input of the filter prossesing. */
					else {
						target = eyefboindex[eyepass];
					}
				}
				else {
					target = RAS_IRasterizer::NextEyeOffScreen(eyefboindex[eyepass]);
				}

				PostRenderScene(scene, target);

				// If no filter was rendered the current used off screen can be unchanged.
				eyefboindex[eyepass] = m_rasterizer->GetCurrentOffScreenIndex();
			}
		}

		// Process filters for non-per eye off screen render.
		if (!renderpereye) {
			/* Choose final off screen target. This operation as effect only for multisamples render off screen.
			 * If it's the last scene, we can render the last filter to a non-multisamples off screen.
			 * Else reuse the (maybe) multisamples off screen for the next scene renders.
			 */
			const int target = lastscene ? RAS_IRasterizer::RAS_OFFSCREEN_FINAL : RAS_IRasterizer::RAS_OFFSCREEN_RENDER;
			PostRenderScene(scene, target);
		}
	}

	m_canvas->SetViewPort(0, 0, width, height);

	// Compositing per eye off screens to screen.
	if (renderpereye) {
		m_rasterizer->DrawStereoOffScreen(m_canvas, eyefboindex[RAS_IRasterizer::RAS_STEREO_LEFTEYE], eyefboindex[RAS_IRasterizer::RAS_STEREO_RIGHTEYE]);
	}
	// Else simply draw the off screen to screen.
	else {
		const short fboindex = m_rasterizer->GetCurrentOffScreenIndex();
		m_rasterizer->DrawOffScreen(m_canvas, fboindex);
	}

	EndFrame();
}

void KX_KetsjiEngine::RequestExit(int exitrequestmode)
{
	m_exitcode = exitrequestmode;
}

void KX_KetsjiEngine::SetNameNextGame(const STR_String& nextgame)
{
	m_exitstring = nextgame;
}

int KX_KetsjiEngine::GetExitCode()
{
	// if a game actuator has set an exit code or if there are no scenes left
	if (!m_exitcode) {
		if (m_scenes->GetCount() == 0)
			m_exitcode = KX_EXIT_REQUEST_NO_SCENES_LEFT;
	}

	return m_exitcode;
}

const STR_String& KX_KetsjiEngine::GetExitString()
{
	return m_exitstring;
}

void KX_KetsjiEngine::EnableCameraOverride(const STR_String& forscene)
{
	m_overrideCam = true;
	m_overrideSceneName = forscene;
}

void KX_KetsjiEngine::SetCameraZoom(float camzoom)
{
	m_cameraZoom = camzoom;
}

void KX_KetsjiEngine::SetCameraOverrideUseOrtho(bool useOrtho)
{
	m_overrideCamUseOrtho = useOrtho;
}

void KX_KetsjiEngine::SetCameraOverrideProjectionMatrix(const MT_CmMatrix4x4& mat)
{
	m_overrideCamProjMat = mat;
}

void KX_KetsjiEngine::SetCameraOverrideViewMatrix(const MT_CmMatrix4x4& mat)
{
	m_overrideCamViewMat = mat;
}

void KX_KetsjiEngine::SetCameraOverrideClipping(float nearfrust, float farfrust)
{
	m_overrideCamNear = nearfrust;
	m_overrideCamFar = farfrust;
}

void KX_KetsjiEngine::SetCameraOverrideLens(float lens)
{
	m_overrideCamLens = lens;
}

void KX_KetsjiEngine::SetCameraOverrideZoom(float camzoom)
{
	m_overrideCamZoom = camzoom;
}

void KX_KetsjiEngine::GetSceneViewport(KX_Scene *scene, KX_Camera *cam, RAS_Rect& area, RAS_Rect& viewport)
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
	else if (!m_overrideCam || (scene->GetName() != m_overrideSceneName) ||  m_overrideCamUseOrtho) {
		RAS_FramingManager::ComputeViewport(
		    scene->GetFramingType(),
		    m_canvas->GetDisplayArea(),
		    viewport);

		area = m_canvas->GetDisplayArea();
	}
	else {
		viewport.SetLeft(0);
		viewport.SetBottom(0);
		viewport.SetRight(int(m_canvas->GetWidth()));
		viewport.SetTop(int(m_canvas->GetHeight()));

		area = m_canvas->GetDisplayArea();
	}
}

void KX_KetsjiEngine::UpdateAnimations(KX_Scene *scene)
{
	if (scene->IsSuspended()) {
		return;
	}

	// Set scene total pause duration, used for animations played on scene which was suspended.
	m_suspendeddelta = scene->getSuspendedDelta();
	// Handle the animations independently of the logic time step
	if (GetRestrictAnimationFPS()) {
		double anim_timestep = 1.0 / scene->GetAnimationFPS();
		if (m_frameTime - m_previousAnimTime > anim_timestep || m_frameTime == m_previousAnimTime) {
			// Sanity/debug print to make sure we're actually going at the fps we want (should be close to anim_timestep)
			// printf("Anim fps: %f\n", 1.0/(m_frameTime - m_previousAnimTime));
			m_previousAnimTime = m_frameTime;
			for (CListValue::iterator sceneit = m_scenes->GetBegin(); sceneit != m_scenes->GetEnd(); ++sceneit)
				((KX_Scene *)*sceneit)->UpdateAnimations(m_frameTime);
		}
	}
	else
		scene->UpdateAnimations(m_frameTime);
}

void KX_KetsjiEngine::RenderShadowBuffers(KX_Scene *scene)
{
	CListValue *lightlist = scene->GetLightList();
	int i;

	m_rasterizer->SetAuxilaryClientInfo(scene);

	for (i = 0; i < lightlist->GetCount(); i++) {
		KX_GameObject *gameobj = (KX_GameObject *)lightlist->GetValue(i);

		KX_LightObject *light = (KX_LightObject *)gameobj;
		RAS_ILightObject *raslight = light->GetLightData();

		raslight->Update();

		if (light->GetVisible() && m_rasterizer->GetDrawingMode() == RAS_IRasterizer::RAS_TEXTURED &&
			raslight->HasShadowBuffer() && raslight->NeedShadowUpdate())
		{
			/* make temporary camera */
			RAS_CameraData camdata = RAS_CameraData();
			KX_Camera *cam = new KX_Camera(scene, scene->m_callbacks, camdata, true, true);
			cam->SetName("__shadow__cam__");

			MT_Transform camtrans;

			/* switch drawmode for speed */
			RAS_IRasterizer::DrawType drawmode = m_rasterizer->GetDrawingMode();
			m_rasterizer->SetDrawingMode(RAS_IRasterizer::RAS_SHADOW);

			/* binds framebuffer object, sets up camera .. */
			raslight->BindShadowBuffer(m_canvas, cam, camtrans);

			/* update scene */
			scene->CalculateVisibleMeshes(m_rasterizer, cam, raslight->GetShadowLayer());

			m_logger->StartLog(tc_animations, m_kxsystem->GetTimeInSeconds(), true);
			SG_SetActiveStage(SG_STAGE_ANIMATION_UPDATE);
			UpdateAnimations(scene);
			m_logger->StartLog(tc_rasterizer, m_kxsystem->GetTimeInSeconds(), true);
			SG_SetActiveStage(SG_STAGE_RENDER);

			/* render */
			m_rasterizer->Clear(RAS_IRasterizer::RAS_DEPTH_BUFFER_BIT | RAS_IRasterizer::RAS_COLOR_BUFFER_BIT);
			scene->RenderBuckets(camtrans, m_rasterizer);

			/* unbind framebuffer object, restore drawmode, free camera */
			raslight->UnbindShadowBuffer();
			m_rasterizer->SetDrawingMode(drawmode);
			cam->Release();
		}
	}
}

// update graphics
void KX_KetsjiEngine::RenderFrame(KX_Scene *scene, KX_Camera *cam, unsigned short pass)
{
	bool override_camera;
	RAS_Rect viewport, area;
	float nearfrust, farfrust, focallength;

	if (!cam)
		return;

	bool isfirstscene = (scene == m_scenes->GetFront());

	KX_SetActiveScene(scene);

#ifdef WITH_PYTHON
	scene->RunDrawingCallbacks(KX_Scene::PRE_DRAW_SETUP, cam);
#endif

	GetSceneViewport(scene, cam, area, viewport);

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
		m_rasterizer->Clear(RAS_IRasterizer::RAS_DEPTH_BUFFER_BIT);
	}

	// see KX_BlenderMaterial::Activate
	//m_rasterizer->SetAmbient();
	m_rasterizer->DisplayFog();

	override_camera = m_overrideCam && (scene->GetName() == m_overrideSceneName) &&
		(cam->GetName() == "__default__cam__");

	if (override_camera && m_overrideCamUseOrtho) {
		m_rasterizer->SetProjectionMatrix(m_overrideCamProjMat);
		if (!cam->hasValidProjectionMatrix()) {
			// needed to get frustum planes for culling
			MT_Matrix4x4 projmat;
			projmat.setValue(m_overrideCamProjMat.getPointer());
			cam->SetProjectionMatrix(projmat);
		}
	}
	else if (cam->hasValidProjectionMatrix()) {
		m_rasterizer->SetProjectionMatrix(cam->GetProjectionMatrix());
	}
	else {
		RAS_FrameFrustum frustum;
		bool orthographic = !cam->GetCameraData()->m_perspective;
		nearfrust = cam->GetCameraNear();
		farfrust = cam->GetCameraFar();
		focallength = cam->GetFocalLength();
		MT_Matrix4x4 projmat;

		if (override_camera) {
			nearfrust = m_overrideCamNear;
			farfrust = m_overrideCamFar;
		}

		float camzoom = override_camera ? m_overrideCamZoom : m_cameraZoom;
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
			    frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar, focallength);
		}
		cam->SetProjectionMatrix(projmat);

		// Otherwise the projection matrix for each eye will be the same...
		if (!orthographic && m_rasterizer->Stereo())
			cam->InvalidateProjectionMatrix();
	}

	MT_Transform camtrans(cam->GetWorldToCamera());
	MT_Matrix4x4 viewmat(camtrans);

	m_rasterizer->SetViewMatrix(viewmat, cam->NodeGetWorldOrientation(), cam->NodeGetWorldPosition(), cam->NodeGetLocalScaling(), cam->GetCameraData()->m_perspective);
	cam->SetModelviewMatrix(viewmat);

	if (isfirstscene) {
		KX_WorldInfo *worldInfo = scene->GetWorldInfo();
		// Update background and render it.
		worldInfo->UpdateBackGround(m_rasterizer);
		worldInfo->RenderBackground(m_rasterizer);
	}

	// The following actually reschedules all vertices to be
	// redrawn. There is a cache between the actual rescheduling
	// and this call though. Visibility is imparted when this call
	// runs through the individual objects.

	m_logger->StartLog(tc_scenegraph, m_kxsystem->GetTimeInSeconds(), true);
	SG_SetActiveStage(SG_STAGE_CULLING);

	// update levels of detail
	scene->UpdateObjectLods();

	scene->CalculateVisibleMeshes(m_rasterizer, cam);

	m_logger->StartLog(tc_animations, m_kxsystem->GetTimeInSeconds(), true);
	SG_SetActiveStage(SG_STAGE_ANIMATION_UPDATE);
	UpdateAnimations(scene);

	m_logger->StartLog(tc_rasterizer, m_kxsystem->GetTimeInSeconds(), true);
	SG_SetActiveStage(SG_STAGE_RENDER);

	// Draw debug infos like bouding box, armature ect.. if enabled.
	scene->DrawDebug(m_rasterizer);

#ifdef WITH_PYTHON
	PHY_SetActiveEnvironment(scene->GetPhysicsEnvironment());
	// Run any pre-drawing python callbacks
	scene->RunDrawingCallbacks(KX_Scene::PRE_DRAW, cam);
#endif

	scene->RenderBuckets(camtrans, m_rasterizer);

	if (scene->GetPhysicsEnvironment())
		scene->GetPhysicsEnvironment()->DebugDrawWorld();
}

/*
 * To run once per scene
 */
void KX_KetsjiEngine::PostRenderScene(KX_Scene *scene, unsigned short target)
{
	KX_SetActiveScene(scene);

	m_rasterizer->FlushDebugShapes(scene);

	// We need to first make sure our viewport is correct (enabling multiple viewports can mess this up), only for filters.
	const int width = m_canvas->GetWidth();
	const int height = m_canvas->GetHeight();
	m_rasterizer->SetViewport(0, 0, width + 1, height + 1);
	m_rasterizer->SetScissor(0, 0, width + 1, height + 1);

	scene->Render2DFilters(m_rasterizer, m_canvas, target);

#ifdef WITH_PYTHON
	PHY_SetActiveEnvironment(scene->GetPhysicsEnvironment());
	/* We can't deduce what camera should be passed to the python callbacks
	 * because the post draw callbacks are per scenes and not per cameras.
	 */
	scene->RunDrawingCallbacks(KX_Scene::POST_DRAW, NULL);

	// Python draw callback can also call debug draw functions, so we have to clear debug shapes.
	m_rasterizer->FlushDebugShapes(scene);
#endif
}

void KX_KetsjiEngine::StopEngine()
{
	if (m_bInitialized) {
		m_sceneconverter->FinalizeAsyncLoads();

		while (m_scenes->GetCount() > 0) {
			KX_Scene *scene = (KX_Scene *)m_scenes->GetFront();
			m_sceneconverter->RemoveScene(scene);
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
	m_scenes->Add(scene->AddRef());
	PostProcessScene(scene);
}

void KX_KetsjiEngine::PostProcessScene(KX_Scene *scene)
{
	bool override_camera = (m_overrideCam && (scene->GetName() == m_overrideSceneName));

	SG_SetActiveStage(SG_STAGE_SCENE);

	// if there is no activecamera, or the camera is being
	// overridden we need to construct a temporary camera
	if (!scene->GetActiveCamera() || override_camera) {
		KX_Camera *activecam = NULL;

		RAS_CameraData camdata = RAS_CameraData();
		if (override_camera) {
			camdata.m_lens = m_overrideCamLens;
			camdata.m_clipstart = m_overrideCamNear;
			camdata.m_clipend = m_overrideCamFar;

			camdata.m_perspective = !m_overrideCamUseOrtho;
		}
		activecam = new KX_Camera(scene, KX_Scene::m_callbacks, camdata);
		activecam->SetName("__default__cam__");

		// set transformation
		if (override_camera) {
			const MT_CmMatrix4x4& cammatdata = m_overrideCamViewMat;
			MT_Transform trans = MT_Transform(cammatdata.getPointer());
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

		scene->GetCameraList()->Add(activecam->AddRef());
		scene->SetActiveCamera(activecam);
		scene->GetObjectList()->Add(activecam->AddRef());
		scene->GetRootParentList()->Add(activecam->AddRef());
		// done with activecam
		activecam->Release();
	}

	scene->UpdateParents(0.0f);
}

void KX_KetsjiEngine::RenderDebugProperties()
{
	STR_String debugtxt;
	int title_xmargin = -7;
	int title_y_top_margin = 4;
	int title_y_bottom_margin = 2;

	int const_xindent = 4;
	int const_ysize = 14;

	int xcoord = 12;    // mmmm, these constants were taken from blender source
	int ycoord = 17;    // to 'mimic' behavior

	int profile_indent = 72;

	float tottime = m_logger->GetAverage();
	if (tottime < 1e-6f) {
		tottime = 1e-6f;
	}

	if (m_show_framerate || m_show_profile) {
		// Title for profiling("Profile")
		m_rasterizer->RenderText2D(RAS_IRasterizer::RAS_TEXT_PADDED,
		                           "Profile",
		                           xcoord + const_xindent + title_xmargin,  // Adds the constant x indent (0 for now) to the title x margin
		                           ycoord,
		                           m_canvas->GetWidth() /* RdV, TODO ?? */,
		                           m_canvas->GetHeight() /* RdV, TODO ?? */);

		// Increase the indent by default increase
		ycoord += const_ysize;
		// Add the title indent afterwards
		ycoord += title_y_bottom_margin;
	}

	// Framerate display
	if (m_show_framerate) {
		m_rasterizer->RenderText2D(RAS_IRasterizer::RAS_TEXT_PADDED,
		                           "Frametime :",
		                           xcoord + const_xindent,
		                           ycoord,
		                           m_canvas->GetWidth() /* RdV, TODO ?? */,
		                           m_canvas->GetHeight() /* RdV, TODO ?? */);

		debugtxt.Format("%5.2fms (%.1ffps)", tottime * 1000.0f, 1.0f / tottime);
		m_rasterizer->RenderText2D(RAS_IRasterizer::RAS_TEXT_PADDED,
		                           debugtxt.ReadPtr(),
		                           xcoord + const_xindent + profile_indent,
		                           ycoord,
		                           m_canvas->GetWidth() /* RdV, TODO ?? */,
		                           m_canvas->GetHeight() /* RdV, TODO ?? */);
		// Increase the indent by default increase
		ycoord += const_ysize;
	}

	// Profile display
	if (m_show_profile) {
		for (int j = tc_first; j < tc_numCategories; j++) {
			m_rasterizer->RenderText2D(RAS_IRasterizer::RAS_TEXT_PADDED,
			                           m_profileLabels[j],
			                           xcoord + const_xindent,
			                           ycoord,
			                           m_canvas->GetWidth(),
			                           m_canvas->GetHeight());

			double time = m_logger->GetAverage((KX_TimeCategory)j);

			debugtxt.Format("%5.2fms | %d%%", time*1000.f, (int)(time/tottime * 100.f));
			m_rasterizer->RenderText2D(RAS_IRasterizer::RAS_TEXT_PADDED,
			                           debugtxt.ReadPtr(),
			                           xcoord + const_xindent + profile_indent, ycoord,
			                           m_canvas->GetWidth(),
			                           m_canvas->GetHeight());

			m_rasterizer->RenderBox2D(xcoord + (int)(2.2 * profile_indent), ycoord, m_canvas->GetWidth(), m_canvas->GetHeight(), time/tottime);
			ycoord += const_ysize;
		}
	}
	// Add the ymargin for titles below the other section of debug info
	ycoord += title_y_top_margin;

	/* Property display */
	if (m_show_debug_properties) {

		/* Title for debugging("Debug properties") */
		m_rasterizer->RenderText2D(RAS_IRasterizer::RAS_TEXT_PADDED,
		                           "Debug Properties",
		                           xcoord + const_xindent + title_xmargin,  // Adds the constant x indent (0 for now) to the title x margin
		                           ycoord,
		                           m_canvas->GetWidth() /* RdV, TODO ?? */,
		                           m_canvas->GetHeight() /* RdV, TODO ?? */);

		// Increase the indent by default increase
		ycoord += const_ysize;
		// Add the title indent afterwards
		ycoord += title_y_bottom_margin;

		/* Calculate amount of properties that can displayed. */
		unsigned propsAct = 0;
		unsigned propsMax = (m_canvas->GetHeight() - ycoord) / const_ysize;

		for (CListValue::iterator sceit = m_scenes->GetBegin(); sceit != m_scenes->GetEnd(); ++sceit) {
			KX_Scene *scene = (KX_Scene *)*sceit;
			/* the 'normal' debug props */
			vector<SCA_DebugProp *>& debugproplist = scene->GetDebugProperties();

			for (unsigned i = 0; i < debugproplist.size() && propsAct < propsMax; i++) {
				CValue *propobj = debugproplist[i]->m_obj;
				STR_String objname = propobj->GetName();
				STR_String propname = debugproplist[i]->m_name;
				propsAct++;
				if (propname == "__state__") {
					// reserve name for object state
					KX_GameObject *gameobj = static_cast<KX_GameObject *>(propobj);
					unsigned int state = gameobj->GetState();
					debugtxt = objname + "." + propname + " = ";
					bool first = true;
					for (int statenum = 1; state; state >>= 1, statenum++) {
						if (state & 1) {
							if (!first) {
								debugtxt += ",";
							}
							debugtxt += STR_String(statenum);
							first = false;
						}
					}
					m_rasterizer->RenderText2D(RAS_IRasterizer::RAS_TEXT_PADDED,
					                           debugtxt.ReadPtr(),
					                           xcoord + const_xindent,
					                           ycoord,
					                           m_canvas->GetWidth(),
					                           m_canvas->GetHeight());
					ycoord += const_ysize;
				}
				else {
					CValue *propval = propobj->GetProperty(propname);
					if (propval) {
						STR_String text = propval->GetText();
						debugtxt = objname + ": '" + propname + "' = " + text;
						m_rasterizer->RenderText2D(RAS_IRasterizer::RAS_TEXT_PADDED,
						                           debugtxt.ReadPtr(),
						                           xcoord + const_xindent,
						                           ycoord,
						                           m_canvas->GetWidth(),
						                           m_canvas->GetHeight());
						ycoord += const_ysize;
					}
				}
			}
		}
	}
}

CListValue *KX_KetsjiEngine::CurrentScenes()
{
	return m_scenes;
}

KX_Scene *KX_KetsjiEngine::FindScene(const STR_String& scenename)
{
	return (KX_Scene *)m_scenes->FindValue(scenename);
}

void KX_KetsjiEngine::ConvertAndAddScene(const STR_String& scenename, bool overlay)
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

void KX_KetsjiEngine::RemoveScene(const STR_String& scenename)
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
		vector<STR_String>::iterator scenenameit;
		for (scenenameit = m_removingScenes.begin(); scenenameit != m_removingScenes.end(); scenenameit++) {
			STR_String scenename = *scenenameit;

			KX_Scene *scene = FindScene(scenename);
			if (scene) {
				m_sceneconverter->RemoveScene(scene);
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

	m_sceneconverter->ConvertScene(tmpscene,
	                               m_rasterizer,
	                               m_canvas,
	                               libloading);

	return tmpscene;
}

KX_Scene *KX_KetsjiEngine::CreateScene(const STR_String& scenename)
{
	Scene *scene = m_sceneconverter->GetBlenderSceneForName(scenename);
	if (!scene)
		return NULL;

	return CreateScene(scene);
}

void KX_KetsjiEngine::AddScheduledScenes()
{
	vector<STR_String>::iterator scenenameit;

	if (m_addingOverlayScenes.size()) {
		for (scenenameit = m_addingOverlayScenes.begin();
		     scenenameit != m_addingOverlayScenes.end();
		     scenenameit++)
		{
			STR_String scenename = *scenenameit;
			KX_Scene *tmpscene = CreateScene(scenename);
			if (tmpscene) {
				m_scenes->Add(tmpscene->AddRef());
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
			STR_String scenename = *scenenameit;
			KX_Scene *tmpscene = CreateScene(scenename);
			if (tmpscene) {
				m_scenes->Insert(0, tmpscene->AddRef());
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

bool KX_KetsjiEngine::ReplaceScene(const STR_String& oldscene, const STR_String& newscene)
{
	// Don't allow replacement if the new scene doesn't exist.
	// Allows smarter game design (used to have no check here).
	// Note that it creates a small backward compatbility issue
	// for a game that did a replace followed by a lib load with the
	// new scene in the lib => it won't work anymore, the lib
	// must be loaded before doing the replace.
	if (m_sceneconverter->GetBlenderSceneForName(newscene) != NULL) {
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
		vector<pair<STR_String, STR_String> >::iterator scenenameit;

		for (scenenameit = m_replace_scenes.begin();
		     scenenameit != m_replace_scenes.end();
		     scenenameit++)
		{
			STR_String oldscenename = (*scenenameit).first;
			STR_String newscenename = (*scenenameit).second;
			/* Scenes are not supposed to be included twice... I think */
			for (unsigned int sce_idx = 0; sce_idx < m_scenes->GetCount(); ++sce_idx) {
				KX_Scene *scene = (KX_Scene *)m_scenes->GetValue(sce_idx);
				if (scene->GetName() == oldscenename) {
					// avoid crash if the new scene doesn't exist, just do nothing
					Scene *blScene = m_sceneconverter->GetBlenderSceneForName(newscenename);
					if (blScene) {
						m_sceneconverter->RemoveScene(scene);

						KX_Scene *tmpscene = CreateScene(blScene);
						m_scenes->SetValue(sce_idx, tmpscene->AddRef());
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

void KX_KetsjiEngine::SuspendScene(const STR_String& scenename)
{
	KX_Scene *scene = FindScene(scenename);
	if (scene) {
		scene->Suspend();
	}
}

void KX_KetsjiEngine::ResumeScene(const STR_String& scenename)
{
	KX_Scene *scene = FindScene(scenename);
	if (scene) {
		scene->Resume();
	}
}

void KX_KetsjiEngine::SetUseFixedFramerate(bool fixedFramerate)
{
	m_fixedFramerate = fixedFramerate;
}

void KX_KetsjiEngine::SetUseExternalClock(bool useExternalClock)
{
	m_useExternalClock = useExternalClock;
}

bool KX_KetsjiEngine::GetUseFixedFramerate(void) const
{
	return m_fixedFramerate;
}

bool KX_KetsjiEngine::GetUseExternalClock(void) const
{
	return m_useExternalClock;
}

double KX_KetsjiEngine::GetSuspendedDelta()
{
	return m_suspendeddelta;
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

bool KX_KetsjiEngine::GetRestrictAnimationFPS()
{
	return m_restrict_anim_fps;
}

void KX_KetsjiEngine::SetRestrictAnimationFPS(bool bRestrictAnimFPS)
{
	m_restrict_anim_fps = bRestrictAnimFPS;
}

double KX_KetsjiEngine::GetAnimFrameRate()
{
	return m_anim_framerate;
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

void KX_KetsjiEngine::SetShowFramerate(bool frameRate)
{
	m_show_framerate = frameRate;
}

bool KX_KetsjiEngine::GetShowFramerate()
{
	return m_show_framerate;
}

void KX_KetsjiEngine::SetShowProfile(bool profile)
{
	m_show_profile = profile;
}

bool KX_KetsjiEngine::GetShowProfile()
{
	return m_show_profile;
}

void KX_KetsjiEngine::SetShowProperties(bool properties)
{
	m_show_debug_properties = properties;
}

bool KX_KetsjiEngine::GetShowProperties()
{
	return m_show_debug_properties;
}

void KX_KetsjiEngine::SetAutoAddDebugProperties(bool add)
{
	m_autoAddDebugProperties = add;
}

bool KX_KetsjiEngine::GetAutoAddDebugProperties()
{
	return m_autoAddDebugProperties;
}

void KX_KetsjiEngine::SetTimingDisplay(bool frameRate, bool profile, bool properties)
{
	m_show_framerate = frameRate;
	m_show_profile = profile;
	m_show_debug_properties = properties;
}

void KX_KetsjiEngine::GetTimingDisplay(bool& frameRate, bool& profile, bool& properties) const
{
	frameRate = m_show_framerate;
	profile = m_show_profile;
	properties = m_show_debug_properties;
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

void KX_KetsjiEngine::SetHideCursor(bool hideCursor)
{
	m_hideCursor = hideCursor;
}

bool KX_KetsjiEngine::GetHideCursor(void) const
{
	return m_hideCursor;
}

void KX_KetsjiEngine::SetShowBoundingBox(bool show)
{
	m_showBoundingBox = show;
}

bool KX_KetsjiEngine::GetShowBoundingBox() const
{
	return m_showBoundingBox;
}

void KX_KetsjiEngine::SetShowArmatures(bool show)
{
	m_showArmature = show;
}

bool KX_KetsjiEngine::GetShowArmatures() const
{
	return m_showArmature;
}

void KX_KetsjiEngine::SetUseOverrideFrameColor(bool overrideFrameColor)
{
	m_overrideFrameColor = overrideFrameColor;
}

bool KX_KetsjiEngine::GetUseOverrideFrameColor(void) const
{
	return m_overrideFrameColor;
}

void KX_KetsjiEngine::SetOverrideFrameColor(float r, float g, float b, float a)
{
	m_overrideFrameColorR = r;
	m_overrideFrameColorG = g;
	m_overrideFrameColorB = b;
	m_overrideFrameColorA = a;
}

void KX_KetsjiEngine::GetOverrideFrameColor(float& r, float& g, float& b, float& a) const
{
	r = m_overrideFrameColorR;
	g = m_overrideFrameColorG;
	b = m_overrideFrameColorB;
	a = m_overrideFrameColorA;
}

void KX_KetsjiEngine::Resize()
{
	KX_SceneList::iterator sceneit;

	/* extended mode needs to recalculate camera frusta when */
	KX_Scene *firstscene = (KX_Scene *)m_scenes->GetFront();
	const RAS_FrameSettings &framesettings = firstscene->GetFramingType();

	if (framesettings.FrameType() == RAS_FrameSettings::e_frame_extend) {
		for (CListValue::iterator sceit = m_scenes->GetBegin(); sceit != m_scenes->GetEnd(); ++sceit) {
			KX_Scene *scene = (KX_Scene *)*sceit;
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
