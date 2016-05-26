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
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GamePlayer/ghost/LA_Launcher.cpp
 *  \ingroup player
 */

#include "LA_Launcher.h"
#include "LA_System.h"
#include "LA_SystemCommandLine.h"

#include "RAS_ICanvas.h"
#include "RAS_OpenGLRasterizer.h"

#include "GPG_Canvas.h"

#include "KX_KetsjiEngine.h"
#include "KX_Globals.h"
#include "KX_PythonInit.h"

#include "KX_BlenderSceneConverter.h"
#include "BL_BlenderDataConversion.h"

#include "KX_NetworkMessageManager.h"

#include "GHOST_ISystem.h"
#include "GHOST_IWindow.h"

#include "GH_EventConsumer.h"
#include "GH_InputDevice.h"

extern "C" {
#  include "GPU_extensions.h"

#  include "BKE_sound.h"

#  include "DNA_scene_types.h"
}

#ifdef WITH_AUDASPACE
#  include AUD_DEVICE_H
#endif

LA_Launcher::LA_Launcher(GHOST_ISystem *system, Main *maggie, Scene *scene, GlobalSettings *gs, int argc, char **argv)
	:m_startSceneName(scene->id.name + 2), 
	m_startScene(scene),
	m_maggie(maggie),
	m_kxStartScene(NULL),
	m_exitRequested(KX_EXIT_REQUEST_NO_REQUEST),
	m_globalSettings(gs),
	m_system(system), 
	m_engineInitialized(false), 
	m_engineRunning(false), 
	m_isEmbedded(false),
	m_ketsjiEngine(NULL),
	m_kxsystem(NULL), 
	m_inputDevice(NULL),
	m_eventConsumer(NULL),
	m_canvas(NULL),
	m_rasterizer(NULL), 
	m_sceneConverter(NULL),
#ifdef WITH_PYTHON
	m_globalDict(NULL),
	m_gameLogic(NULL),
	m_gameLogicKeys(NULL),
#endif  // WITH_PYTHON
	m_argc(argc),
	m_argv(argv)
{
#ifdef WITH_PYTHON
	m_globalDict = PyDict_New();
#endif  // WITH_PYTHON
}

LA_Launcher::~LA_Launcher()
{
	ExitEngine();
#ifdef WITH_PYTHON
	PyDict_Clear(m_globalDict);
	Py_DECREF(m_globalDict);
#endif  // WITH_PYTHON
}

int LA_Launcher::GetExitRequested()
{
	return m_exitRequested;
}

GlobalSettings *LA_Launcher::GetGlobalSettings()
{
	return m_ketsjiEngine->GetGlobalSettings();
}

const STR_String& LA_Launcher::GetExitString()
{
	return m_exitString;
}

bool LA_Launcher::InitEngine(const int stereoMode)
{
	if (!m_engineInitialized) {
		// Get and set the preferences.
		SYS_SystemHandle syshandle = SYS_GetSystem();
		if (!syshandle) {
			return false;
		}

		GameData *gm = &m_startScene->gm;
		bool properties = (SYS_GetCommandLineInt(syshandle, "show_properties", 0) != 0);
		bool profile = (SYS_GetCommandLineInt(syshandle, "show_profile", 0) != 0);

		bool showPhysics = (gm->flag & GAME_SHOW_PHYSICS);
		SYS_WriteCommandLineInt(syshandle, "show_physics", showPhysics);

		bool fixed_framerate = (SYS_GetCommandLineInt(syshandle, "fixedtime", (gm->flag & GAME_ENABLE_ALL_FRAMES)) != 0);
		bool frameRate = (SYS_GetCommandLineInt(syshandle, "show_framerate", 0) != 0);
		bool useLists = (SYS_GetCommandLineInt(syshandle, "displaylists", gm->flag & GAME_DISPLAY_LISTS) != 0) && GPU_display_list_support();
		bool showBoundingBox = (SYS_GetCommandLineInt(syshandle, "show_bounding_box", gm->flag & GAME_SHOW_BOUNDING_BOX) != 0);
		bool showArmatures = (SYS_GetCommandLineInt(syshandle, "show_armatures", gm->flag & GAME_SHOW_ARMATURES) != 0);
		bool nodepwarnings = (SYS_GetCommandLineInt(syshandle, "ignore_deprecation_warnings", 1) != 0);
		bool restrictAnimFPS = (gm->flag & GAME_RESTRICT_ANIM_UPDATES) != 0;

		RAS_STORAGE_TYPE raster_storage = RAS_AUTO_STORAGE;
		int storageInfo = RAS_STORAGE_INFO_NONE;

		if (gm->raster_storage == RAS_STORE_VBO) {
			raster_storage = RAS_VBO;
		}
		else if (gm->raster_storage == RAS_STORE_VA) {
			raster_storage = RAS_VA;
		}

		if (useLists) {
			storageInfo |= RAS_STORAGE_USE_DISPLAY_LIST;
		}

		m_rasterizer = new RAS_OpenGLRasterizer(raster_storage, storageInfo);

		// Stereo parameters - Eye Separation from the UI - stereomode from the command-line/UI
		m_rasterizer->SetStereoMode((RAS_IRasterizer::StereoMode) stereoMode);
		m_rasterizer->SetEyeSeparation(m_startScene->gm.eyeseparation);
		m_rasterizer->SetDrawingMode(GetRasterizerDrawMode());

		// Create the canvas, rasterizer and rendertools.
		m_canvas = CreateCanvas(m_rasterizer);

		if (gm->vsync == VSYNC_ADAPTIVE) {
			m_canvas->SetSwapInterval(-1);
		}
		else {
			m_canvas->SetSwapInterval((gm->vsync == VSYNC_ON) ? 1 : 0);
		}

		m_canvas->Init();
		if (gm->flag & GAME_SHOW_MOUSE) {
			m_canvas->SetMouseState(RAS_ICanvas::MOUSE_NORMAL);
		}

		// Create the inputdevices.
		m_inputDevice = new GH_InputDevice();
		m_eventConsumer = new GH_EventConsumer(m_inputDevice);
		m_system->addEventConsumer(m_eventConsumer);

		// Create a ketsjisystem (only needed for timing and stuff).
		m_kxsystem = new LA_System();

		m_networkMessageManager = new KX_NetworkMessageManager();
		
		// Create the ketsjiengine.
		m_ketsjiEngine = new KX_KetsjiEngine(m_kxsystem);
		
		// Set the devices.
		m_ketsjiEngine->SetInputDevice(m_inputDevice);
		m_ketsjiEngine->SetCanvas(m_canvas);
		m_ketsjiEngine->SetRasterizer(m_rasterizer);
		m_ketsjiEngine->SetNetworkMessageManager(m_networkMessageManager);

		KX_KetsjiEngine::SetExitKey(ConvertKeyCode(gm->exitkey));
#ifdef WITH_PYTHON
		CValue::SetDeprecationWarnings(nodepwarnings);
#else
		(void)nodepwarnings;
#endif

		m_ketsjiEngine->SetUseFixedTime(fixed_framerate);
		m_ketsjiEngine->SetTimingDisplay(frameRate, profile, properties);
		m_ketsjiEngine->SetRestrictAnimationFPS(restrictAnimFPS);
		m_ketsjiEngine->SetShowBoundingBox(showBoundingBox);
		m_ketsjiEngine->SetShowArmatures(showArmatures);

		// Set the global settings (carried over if restart/load new files).
		m_ketsjiEngine->SetGlobalSettings(m_globalSettings);

		InitCamera();

		m_engineInitialized = true;
	}

	return m_engineInitialized;
}

bool LA_Launcher::StartEngine()
{
	if (m_engineRunning) {
		return false;
	}

	// Create a scene converter, create and convert the stratingscene.
	m_sceneConverter = new KX_BlenderSceneConverter(m_maggie, m_ketsjiEngine);
	if (m_sceneConverter) {
		STR_String m_kxStartScenename = m_startSceneName.Ptr();
		m_ketsjiEngine->SetSceneConverter(m_sceneConverter);

		m_kxStartScene = new KX_Scene(m_inputDevice,
			m_kxStartScenename,
			m_startScene,
			m_canvas,
			m_networkMessageManager);

		KX_SetActiveScene(m_kxStartScene);
		KX_SetActiveEngine(m_ketsjiEngine);

#ifdef WITH_PYTHON
		// Some python things.
		setupGamePython(m_ketsjiEngine, m_maggie, m_globalDict, &m_gameLogic, &m_gameLogicKeys, m_argc, m_argv);
#endif  // WITH_PYTHON

		// Initialize Dome Settings.
		if (m_startScene->gm.stereoflag == STEREO_DOME) {
			m_ketsjiEngine->InitDome(m_startScene->gm.dome.res, m_startScene->gm.dome.mode, m_startScene->gm.dome.angle, m_startScene->gm.dome.resbuf, m_startScene->gm.dome.tilt, m_startScene->gm.dome.warptext);
		}

		// Initialize 3D Audio Settings.
		AUD_Device *device = BKE_sound_get_device();
		AUD_Device_setSpeedOfSound(device, m_startScene->audio.speed_of_sound);
		AUD_Device_setDopplerFactor(device, m_startScene->audio.doppler_factor);
		AUD_Device_setDistanceModel(device, AUD_DistanceModel(m_startScene->audio.distance_model));

		m_sceneConverter->SetAlwaysUseExpandFraming(GetUseAlwaysExpandFraming());

		m_sceneConverter->ConvertScene(
			m_kxStartScene,
			m_rasterizer,
			m_canvas);
		m_ketsjiEngine->AddScene(m_kxStartScene);
		m_kxStartScene->Release();

		m_rasterizer->Init();
		m_ketsjiEngine->StartEngine(true);
		m_engineRunning = true;
		
		/* Set the animation playback rate for ipo's and actions the 
		 * framerate below should patch with FPS macro defined in blendef.h 
		 * Could be in StartEngine set the framerate, we need the scene to do this.
		 */
		Scene *scene = m_kxStartScene->GetBlenderScene(); // needed for macro
		m_ketsjiEngine->SetAnimFrameRate(FPS);
	}
	
	if (!m_engineRunning) {
		StopEngine();
	}
	
	return m_engineRunning;
}


void LA_Launcher::StopEngine()
{
	m_ketsjiEngine->StopEngine();

	if (m_sceneConverter) {
		delete m_sceneConverter;
		m_sceneConverter = NULL;
	}


#ifdef WITH_PYTHON

	/* Clears the dictionary by hand:
	 * This prevents, extra references to global variables
	 * inside the GameLogic dictionary when the python interpreter is finalized.
	 * which allows the scene to safely delete them :)
	 * see: (space.c)->start_game
	 */

	PyDict_Clear(PyModule_GetDict(m_gameLogic));
	PyDict_SetItemString(PyModule_GetDict(m_gameLogic), "globalDict", m_globalDict);

#endif  // WITH_PYTHON

	m_engineRunning = false;
}

#ifdef WITH_PYTHON

int LA_Launcher::PythonEngineNextFrame(void *state)
{
	LA_Launcher *launcher = (LA_Launcher *)state;
	bool run = launcher->EngineNextFrame();
	if (run) {
		return 0;
	}
	else {
		int exitcode = launcher->GetExitRequested();
		if (exitcode) {
			fprintf(stderr, "Exit code %d: %s\n", exitcode, launcher->GetExitString().ReadPtr());
		}
		return 1;
	}
}

#endif

bool LA_Launcher::EngineNextFrame()
{
	// Update the state of the game engine.
	if (m_kxsystem && !m_exitRequested) {
		// First check if we want to exit.
		m_exitRequested = m_ketsjiEngine->GetExitCode();
		
		// Kick the engine.
		bool renderFrame = m_ketsjiEngine->NextFrame();
		if (renderFrame) {
			// Render the frame.
			m_ketsjiEngine->Render();
		}
		m_system->processEvents(false);
		m_system->dispatchEvents();

		if (m_inputDevice->GetEvent((SCA_IInputDevice::SCA_EnumInputs)m_ketsjiEngine->GetExitKey()).Find(SCA_InputEvent::KX_ACTIVE)) {
			m_exitRequested = KX_EXIT_REQUEST_BLENDER_ESC;
		}
		else if (m_inputDevice->GetEvent(SCA_IInputDevice::KX_WINCLOSE).Find(SCA_InputEvent::KX_ACTIVE) ||
			m_inputDevice->GetEvent(SCA_IInputDevice::KX_WINQUIT).Find(SCA_InputEvent::KX_ACTIVE))
		{
			m_exitRequested = KX_EXIT_REQUEST_OUTSIDE;
		}
		else {
			m_exitRequested = KX_EXIT_REQUEST_NO_REQUEST;
		}
	}
	m_exitString = m_ketsjiEngine->GetExitString();

	if (m_exitRequested != KX_EXIT_REQUEST_NO_REQUEST) {
		return false;
	}
	return true;
}

void LA_Launcher::ExitEngine()
{
	// We only want to kill the engine if it has been initialized.
	if (!m_engineInitialized) {
		return;
	}

	if (m_ketsjiEngine) {
		StopEngine();
		delete m_ketsjiEngine;
		m_ketsjiEngine = NULL;
	}
	if (m_kxsystem) {
		delete m_kxsystem;
		m_kxsystem = NULL;
	}
	if (m_inputDevice) {
		delete m_inputDevice;
		m_inputDevice = NULL;
	}
	if (m_eventConsumer) {
		m_system->removeEventConsumer(m_eventConsumer);
		delete m_eventConsumer;
	}
	if (m_rasterizer) {
		delete m_rasterizer;
		m_rasterizer = NULL;
	}
	if (m_canvas) {
		delete m_canvas;
		m_canvas = NULL;
	}
	if (m_networkMessageManager) {
		delete m_networkMessageManager;
		m_networkMessageManager = NULL;
	}

	// Call this after we're sure nothing needs Python anymore (e.g., destructors).
	ExitPython();

#ifdef WITH_PYTHON
	Py_DECREF(m_gameLogicKeys);
	m_gameLogicKeys = NULL;
#endif  // WITH_PYTHON

	// Stop all remaining playing sounds.
	AUD_Device_stopAll(BKE_sound_get_device());

	m_exitRequested = KX_EXIT_REQUEST_NO_REQUEST;
	m_engineInitialized = false;
}


bool LA_Launcher::StartGameEngine(int stereoMode)
{
	bool success = InitEngine(stereoMode);

	if (success) {
		success = StartEngine();
	}

	return success;
}

void LA_Launcher::StopGameEngine()
{
	ExitEngine();
}
