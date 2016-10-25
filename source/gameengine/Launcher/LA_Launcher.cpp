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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Launcher/LA_Launcher.cpp
 *  \ingroup launcher
 */

#ifdef WIN32
#  include <Windows.h>
#endif

#include "LA_Launcher.h"
#include "LA_System.h"
#include "LA_SystemCommandLine.h"

#include "RAS_ICanvas.h"
#include "RAS_OpenGLRasterizer.h"

#include "GPG_Canvas.h"

#include "KX_KetsjiEngine.h"
#include "KX_Globals.h"
#include "KX_PythonInit.h"
#include "KX_PythonMain.h"
#include "KX_PyConstraintBinding.h"

#include "KX_BlenderSceneConverter.h"
#include "BL_BlenderDataConversion.h"

#include "KX_NetworkMessageManager.h"

#ifdef WITH_PYTHON
#  include "Texture.h" // For FreeAllTextures.
#endif  // WITH_PYTHON

#include "GHOST_ISystem.h"
#include "GHOST_IWindow.h"

#include "DEV_EventConsumer.h"
#include "DEV_InputDevice.h"

#include "DEV_Joystick.h"

#include "CM_Message.h"

extern "C" {
#  include "GPU_extensions.h"

#  include "BKE_sound.h"
#  include "BKE_main.h"

#  include "DNA_scene_types.h"
#  include "DNA_material_types.h"

#  include "MEM_guardedalloc.h"

#  include "wm_event_types.h"
}

#ifdef WITH_AUDASPACE
#  include AUD_DEVICE_H
#endif

LA_Launcher::LA_Launcher(GHOST_ISystem *system, Main *maggie, Scene *scene, GlobalSettings *gs,
						 RAS_IRasterizer::StereoMode stereoMode, int samples, int argc, char **argv)
	:m_startSceneName(scene->id.name + 2), 
	m_startScene(scene),
	m_maggie(maggie),
	m_kxStartScene(NULL),
	m_exitRequested(KX_EXIT_REQUEST_NO_REQUEST),
	m_globalSettings(gs),
	m_system(system),
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
#endif  // WITH_PYTHON
	m_samples(samples),
	m_stereoMode(stereoMode),
	m_argc(argc),
	m_argv(argv)
{
	m_pythonConsole.use = false;
}

LA_Launcher::~LA_Launcher()
{
}

#ifdef WITH_PYTHON
void LA_Launcher::SetPythonGlobalDict(PyObject *globalDict)
{
	m_globalDict = globalDict;
}
#endif  // WITH_PYTHON

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

void LA_Launcher::InitEngine()
{
	// Get and set the preferences.
	SYS_SystemHandle syshandle = SYS_GetSystem();

	GameData *gm = &m_startScene->gm;
	bool properties = (SYS_GetCommandLineInt(syshandle, "show_properties", 0) != 0);
	bool profile = (SYS_GetCommandLineInt(syshandle, "show_profile", 0) != 0);

	bool showPhysics = (gm->flag & GAME_SHOW_PHYSICS);
	SYS_WriteCommandLineInt(syshandle, "show_physics", showPhysics);

	// WARNING: Fixed time is the opposite of fixed framerate.
	bool fixed_framerate = (SYS_GetCommandLineInt(syshandle, "fixedtime", (gm->flag & GAME_ENABLE_ALL_FRAMES)) == 0);
	bool frameRate = (SYS_GetCommandLineInt(syshandle, "show_framerate", 0) != 0);
	bool showBoundingBox = (SYS_GetCommandLineInt(syshandle, "show_bounding_box", gm->flag & GAME_SHOW_BOUNDING_BOX) != 0);
	bool showArmatures = (SYS_GetCommandLineInt(syshandle, "show_armatures", gm->flag & GAME_SHOW_ARMATURES) != 0);
	bool nodepwarnings = (SYS_GetCommandLineInt(syshandle, "ignore_deprecation_warnings", 1) != 0);
	bool restrictAnimFPS = (gm->flag & GAME_RESTRICT_ANIM_UPDATES) != 0;

	// Setup python console keys used as shortcut.
	for (unsigned short i = 0; i < 4; ++i) {
		if (gm->pythonkeys[i] != EVENT_NONE) {
			m_pythonConsole.keys.push_back(ConvertKeyCode(gm->pythonkeys[i]));
		}
	}
	m_pythonConsole.use = (gm->flag & GAME_PYTHON_CONSOLE);

	m_rasterizer = new RAS_OpenGLRasterizer();

	// Stereo parameters - Eye Separation from the UI - stereomode from the command-line/UI
	m_rasterizer->SetStereoMode(m_stereoMode);
	m_rasterizer->SetEyeSeparation(m_startScene->gm.eyeseparation);
	m_rasterizer->SetDrawingMode(GetRasterizerDrawMode());

	// Copy current anisotropic level to restore it at the game end.
	m_savedData.anisotropic = m_rasterizer->GetAnisotropicFiltering();
	// Copy current mipmap mode to restore at the game end.
	m_savedData.mipmap = m_rasterizer->GetMipmapping();

	// Create the canvas, rasterizer and rendertools.
	m_canvas = CreateCanvas(m_rasterizer);

	// Copy current vsync mode to restore at the game end.
	m_canvas->GetSwapInterval(m_savedData.vsync);

	if (gm->vsync == VSYNC_ADAPTIVE) {
		m_canvas->SetSwapInterval(-1);
	}
	else {
		m_canvas->SetSwapInterval((gm->vsync == VSYNC_ON) ? 1 : 0);
	}

	// Set canvas multisamples.
	m_canvas->SetSamples(m_samples);

	RAS_IRasterizer::HdrType hdrtype = RAS_IRasterizer::RAS_HDR_NONE;
	switch (gm->hdr) {
		case GAME_HDR_NONE:
		{
			hdrtype = RAS_IRasterizer::RAS_HDR_NONE;
			break;
		}
		case GAME_HDR_HALF_FLOAT:
		{
			hdrtype = RAS_IRasterizer::RAS_HDR_HALF_FLOAT;
			break;
		}
		case GAME_HDR_FULL_FLOAT:
		{
			hdrtype = RAS_IRasterizer::RAS_HDR_FULL_FLOAT;
			break;
		}
	}
	m_canvas->SetHdrType(hdrtype);

	m_canvas->Init();
	if (gm->flag & GAME_SHOW_MOUSE) {
		m_canvas->SetMouseState(RAS_ICanvas::MOUSE_NORMAL);
	}
	else {
		m_canvas->SetMouseState(RAS_ICanvas::MOUSE_INVISIBLE);
	}

	// Create the inputdevices.
	m_inputDevice = new DEV_InputDevice();
	m_eventConsumer = new DEV_EventConsumer(m_system, m_inputDevice, m_canvas);
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

	DEV_Joystick::Init();

	KX_KetsjiEngine::SetExitKey(ConvertKeyCode(gm->exitkey));
#ifdef WITH_PYTHON
	CValue::SetDeprecationWarnings(nodepwarnings);
#else
	(void)nodepwarnings;
#endif

	m_ketsjiEngine->SetUseFixedFramerate(fixed_framerate);
	m_ketsjiEngine->SetTimingDisplay(frameRate, profile, properties);
	m_ketsjiEngine->SetRender(true);
	m_ketsjiEngine->SetRestrictAnimationFPS(restrictAnimFPS);
	m_ketsjiEngine->SetShowBoundingBox(showBoundingBox);
	m_ketsjiEngine->SetShowArmatures(showArmatures);

	// Set the global settings (carried over if restart/load new files).
	m_ketsjiEngine->SetGlobalSettings(m_globalSettings);

	InitCamera();

#ifdef WITH_PYTHON
	KX_SetMainPath(STR_String(m_maggie->name));
#endif

	// Create a scene converter, create and convert the stratingscene.
	m_sceneConverter = new KX_BlenderSceneConverter(m_maggie, m_ketsjiEngine);
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
	setupGamePython(m_ketsjiEngine, m_maggie, m_globalDict, &m_gameLogic, m_argc, m_argv);
#endif  // WITH_PYTHON

#ifdef WITH_AUDASPACE
	// Initialize 3D Audio Settings.
	AUD_Device *device = BKE_sound_get_device();
	AUD_Device_setSpeedOfSound(device, m_startScene->audio.speed_of_sound);
	AUD_Device_setDopplerFactor(device, m_startScene->audio.doppler_factor);
	AUD_Device_setDistanceModel(device, AUD_DistanceModel(m_startScene->audio.distance_model));
#endif  // WITH_AUDASPACE

	m_sceneConverter->SetAlwaysUseExpandFraming(GetUseAlwaysExpandFraming());

	m_sceneConverter->ConvertScene(
		m_kxStartScene,
		m_rasterizer,
		m_canvas);
	m_ketsjiEngine->AddScene(m_kxStartScene);
	m_kxStartScene->Release();

	m_rasterizer->Init();
	m_ketsjiEngine->StartEngine(true);

	/* Set the animation playback rate for ipo's and actions the 
	 * framerate below should patch with FPS macro defined in blendef.h 
	 * Could be in StartEngine set the framerate, we need the scene to do this.
	 */
	Scene *scene = m_kxStartScene->GetBlenderScene(); // needed for macro
	m_ketsjiEngine->SetAnimFrameRate(FPS);
}


void LA_Launcher::ExitEngine()
{
#ifdef WITH_PYTHON
	Texture::FreeAllTextures(NULL);
#endif  // WITH_PYTHON

	DEV_Joystick::Close();
	m_ketsjiEngine->StopEngine();

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

	// Do we will stop ?
	if ((m_exitRequested != KX_EXIT_REQUEST_RESTART_GAME) && (m_exitRequested != KX_EXIT_REQUEST_START_OTHER_GAME)) {
		// Then set the cursor back to normal here to avoid set the cursor visible between two game load.
		m_canvas->SetMouseState(RAS_ICanvas::MOUSE_NORMAL);
	}

	// Set anisotropic settign back to its original value.
	m_rasterizer->SetAnisotropicFiltering(m_savedData.anisotropic);

	// Set mipmap setting back to its original value.
	m_rasterizer->SetMipmapping(m_savedData.mipmap);

	// Set vsync mode back to original value.
	m_canvas->SetSwapInterval(m_savedData.vsync);

	if (m_sceneConverter) {
		delete m_sceneConverter;
		m_sceneConverter = NULL;
	}
	if (m_ketsjiEngine) {
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

#ifdef WITH_AUDASPACE
	// Stop all remaining playing sounds.
	AUD_Device_stopAll(BKE_sound_get_device());
#endif  // WITH_AUDASPACE

	m_exitRequested = KX_EXIT_REQUEST_NO_REQUEST;
}

#ifdef WITH_PYTHON

void LA_Launcher::HandlePythonConsole()
{
#ifndef WITH_GAMEENGINE_SECURITY
	if (!m_pythonConsole.use) {
		return;
	}

	for (unsigned short i = 0, size = m_pythonConsole.keys.size(); i < size; ++i) {
		if (!m_inputDevice->GetInput(m_pythonConsole.keys[i]).Find(SCA_InputEvent::ACTIVE)) {
			return;
		}
	}

#ifdef WIN32 // We Use this function to avoid Blender window freeze when we launch python console from Windows.
       DisableProcessWindowsGhosting();
#endif

	// Pop the console window for windows.
	m_system->toggleConsole(1);

	createPythonConsole();

	// Hide the console window for windows.
	m_system->toggleConsole(0);

	// Set the game engine windows on top of all other and active.
	SetWindowOrder(1);


	/* As we show the console, the release events of the shortcut keys can be not handled by the engine.
	 * We simulate they them.
	 */
	for (unsigned short i = 0, size = m_pythonConsole.keys.size(); i < size; ++i) {
		m_inputDevice->ConvertEvent(m_pythonConsole.keys[i], 0, 0);
	}
#endif
}

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
			CM_Error("Exit code " << exitcode << ": " << launcher->GetExitString());
		}
		return 1;
	}
}

#endif

void LA_Launcher::RenderEngine()
{
	// Render the frame.
	m_ketsjiEngine->Render();
}

bool LA_Launcher::GetMainLoopPythonCode(char **pythonCode, char **pythonFileName)
{
	*pythonFileName = KX_GetPythonMain(m_startScene);
	if (*pythonFileName) {
		*pythonCode = KX_GetPythonCode(m_maggie, *pythonFileName);
		if (!*pythonCode) {
			CM_Error("Cannot yield control to Python: no Python text data block named '" << *pythonFileName << "'");
			return false;
		}
		return true;
	}
	return false;
}

bool LA_Launcher::EngineNextFrame()
{
	// Update the state of the game engine.
	if (m_kxsystem && !m_exitRequested) {
		// First check if we want to exit.
		m_exitRequested = m_ketsjiEngine->GetExitCode();

#ifdef WITH_PYTHON
		// Check if we can create a python console debugging.
		HandlePythonConsole();
#endif

		// Kick the engine.
		bool renderFrame = m_ketsjiEngine->NextFrame();
		if (renderFrame) {
			RenderEngine();
		}

		m_system->processEvents(false);
		m_system->dispatchEvents();

		if (m_inputDevice->GetInput((SCA_IInputDevice::SCA_EnumInputs)m_ketsjiEngine->GetExitKey()).Find(SCA_InputEvent::ACTIVE) &&
			!m_inputDevice->GetHookExitKey())
		{
			m_inputDevice->ConvertEvent((SCA_IInputDevice::SCA_EnumInputs)m_ketsjiEngine->GetExitKey(), 0, 0);
			m_exitRequested = KX_EXIT_REQUEST_BLENDER_ESC;
		}
		else if (m_inputDevice->GetInput(SCA_IInputDevice::WINCLOSE).Find(SCA_InputEvent::ACTIVE) ||
			m_inputDevice->GetInput(SCA_IInputDevice::WINQUIT).Find(SCA_InputEvent::ACTIVE))
		{
			m_inputDevice->ConvertEvent(SCA_IInputDevice::WINCLOSE, 0, 0);
			m_inputDevice->ConvertEvent(SCA_IInputDevice::WINQUIT, 0, 0);
			m_exitRequested = KX_EXIT_REQUEST_OUTSIDE;
		}
	}
	m_exitString = m_ketsjiEngine->GetExitString();

	if (m_exitRequested != KX_EXIT_REQUEST_NO_REQUEST) {
		return false;
	}
	return true;
}

void LA_Launcher::EngineMainLoop()
{
#ifdef WITH_PYTHON
	char *pythonCode;
	char *pythonFileName;
	if (GetMainLoopPythonCode(&pythonCode, &pythonFileName)) {
		// Set python environement variable.
		KX_SetActiveScene(m_kxStartScene);
		PHY_SetActiveEnvironment(m_kxStartScene->GetPhysicsEnvironment());

		pynextframestate.state = this;
		pynextframestate.func = &PythonEngineNextFrame;

		CM_Debug("Yielding control to Python script '" << pythonFileName << "'...");
		PyRun_SimpleString(pythonCode);
		CM_Debug("Exit Python script '" << pythonFileName << "'");
		MEM_freeN(pythonCode);
	}
	else {
		pynextframestate.state = NULL;
		pynextframestate.func = NULL;
#endif  // WITH_PYTHON

		bool run = true;
		while (run) {
			run  = EngineNextFrame();
		}

#ifdef WITH_PYTHON
	}
#endif
}
