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
#include "LA_SystemCommandLine.h"

#include "RAS_ICanvas.h"
#include "RAS_OverrideShader.h"

#include "GPG_Canvas.h"

#include "KX_KetsjiEngine.h"
#include "KX_Scene.h"
#include "KX_Globals.h"
#include "KX_PythonInit.h"
#include "KX_PythonMain.h"
#include "KX_PyConstraintBinding.h"

#include "BL_Converter.h"
#include "BL_SceneConverter.h"
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

#  include "wm_event_types.h"
}

#ifdef WITH_AUDASPACE
#  include AUD_DEVICE_H
#endif

#ifdef WITH_PYTHON

struct PythonMainLoopState
{
	KX_ExitInfo m_exitInfo;
	LA_Launcher *m_launcher;
};

#endif

LA_Launcher::LA_Launcher(GHOST_ISystem *system, Main *maggie, Scene *scene, GlobalSettings *gs,
                         RAS_Rasterizer::StereoMode stereoMode, int samples, bool alwaysUseExpandFraming, int argc, char **argv)
	:m_startSceneName(scene->id.name + 2),
	m_startScene(scene),
	m_maggie(maggie),
	m_kxStartScene(nullptr),
	m_globalSettings(gs),
	m_system(system),
	m_ketsjiEngine(nullptr),
	m_inputDevice(nullptr),
	m_eventConsumer(nullptr),
	m_canvas(nullptr),
	m_rasterizer(nullptr),
	m_converter(nullptr),
#ifdef WITH_PYTHON
	m_globalDict(nullptr),
#endif  // WITH_PYTHON
	m_alwaysUseExpandFraming(alwaysUseExpandFraming),
	m_camZoom(1.0f),
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

GlobalSettings *LA_Launcher::GetGlobalSettings()
{
	return m_ketsjiEngine->GetGlobalSettings();
}

void LA_Launcher::InitEngine()
{
	// Get and set the preferences.
	SYS_SystemHandle syshandle = SYS_GetSystem();

	const GameData& gm = m_startScene->gm;
	bool properties = (SYS_GetCommandLineInt(syshandle, "show_properties", 0) != 0);
	bool profile = (SYS_GetCommandLineInt(syshandle, "show_profile", 0) != 0);

	bool showPhysics = (gm.flag & GAME_SHOW_PHYSICS);
	SYS_WriteCommandLineInt(syshandle, "show_physics", showPhysics);

	// WARNING: Fixed time is the opposite of fixed framerate.
	bool fixed_framerate = (SYS_GetCommandLineInt(syshandle, "fixedtime", (gm.flag & GAME_ENABLE_ALL_FRAMES)) == 0);
	bool frameRate = (SYS_GetCommandLineInt(syshandle, "show_framerate", 0) != 0);
	bool renderQueries = (SYS_GetCommandLineInt(syshandle, "show_render_queries", 0) != 0);
	short showBoundingBox = SYS_GetCommandLineInt(syshandle, "show_bounding_box", gm.showBoundingBox);
	short showArmatures = SYS_GetCommandLineInt(syshandle, "show_armatures", gm.showArmatures);
	short showCameraFrustum = SYS_GetCommandLineInt(syshandle, "show_camera_frustum", gm.showCameraFrustum);
	short showShadowFrustum = SYS_GetCommandLineInt(syshandle, "show_shadow_frustum", gm.showShadowFrustum);
	bool nodepwarnings = (SYS_GetCommandLineInt(syshandle, "ignore_deprecation_warnings", 1) != 0);
	bool restrictAnimFPS = (gm.flag & GAME_RESTRICT_ANIM_UPDATES) != 0;

	const KX_KetsjiEngine::FlagType flags = (KX_KetsjiEngine::FlagType)
	                                        ((fixed_framerate ? KX_KetsjiEngine::FIXED_FRAMERATE : 0) |
	                                         (frameRate ? KX_KetsjiEngine::SHOW_FRAMERATE : 0) |
	                                         (renderQueries ? KX_KetsjiEngine::SHOW_RENDER_QUERIES : 0) |
	                                         (restrictAnimFPS ? KX_KetsjiEngine::RESTRICT_ANIMATION : 0) |
	                                         (properties ? KX_KetsjiEngine::SHOW_DEBUG_PROPERTIES : 0) |
	                                         (profile ? KX_KetsjiEngine::SHOW_PROFILE : 0));

	// Setup python console keys used as shortcut.
	for (unsigned short i = 0; i < 4; ++i) {
		if (gm.pythonkeys[i] != EVENT_NONE) {
			m_pythonConsole.keys.push_back(BL_ConvertKeyCode(gm.pythonkeys[i]));
		}
	}
	m_pythonConsole.use = (gm.flag & GAME_PYTHON_CONSOLE);

	RAS_OverrideShader::InitShaders();

	m_rasterizer = new RAS_Rasterizer();

	// Stereo parameters - Eye Separation from the UI - stereomode from the command-line/UI
	static const RAS_Rasterizer::ColorManagement colorManagementTable[] = {
		RAS_Rasterizer::RAS_COLOR_MANAGEMENT_LINEAR, // GAME_COLOR_MANAGEMENT_LINEAR
		RAS_Rasterizer::RAS_COLOR_MANAGEMENT_SRGB // GAME_COLOR_MANAGEMENT_SRGB
	};
	m_rasterizer->SetColorManagment(colorManagementTable[gm.colorManagement]);
	m_rasterizer->SetStereoMode(m_stereoMode);
	m_rasterizer->SetEyeSeparation(m_startScene->gm.eyeseparation);
	m_rasterizer->SetDrawingMode(GetRasterizerDrawMode());

	// Copy current anisotropic level to restore it at the game end.
	m_savedData.anisotropic = m_rasterizer->GetAnisotropicFiltering();
	// Copy current mipmap mode to restore at the game end.
	m_savedData.mipmap = m_rasterizer->GetMipmapping();

	// Create the canvas, rasterizer and rendertools.
	m_canvas = CreateCanvas(m_rasterizer);

	static const RAS_ICanvas::SwapControl swapControlTable[] = {
		RAS_ICanvas::VSYNC_ON, // VSYNC_ON
		RAS_ICanvas::VSYNC_OFF, // VSYNC_OFF
		RAS_ICanvas::VSYNC_ADAPTIVE // VSYNC_ADAPTIVE
	};

	m_canvas->SetSwapControl(swapControlTable[gm.vsync]);

	// Set canvas multisamples.
	m_canvas->SetSamples(m_samples);

	RAS_Rasterizer::HdrType hdrtype = RAS_Rasterizer::RAS_HDR_NONE;
	switch (gm.hdr) {
		case GAME_HDR_NONE:
		{
			hdrtype = RAS_Rasterizer::RAS_HDR_NONE;
			break;
		}
		case GAME_HDR_HALF_FLOAT:
		{
			hdrtype = RAS_Rasterizer::RAS_HDR_HALF_FLOAT;
			break;
		}
		case GAME_HDR_FULL_FLOAT:
		{
			hdrtype = RAS_Rasterizer::RAS_HDR_FULL_FLOAT;
			break;
		}
	}
	m_canvas->SetHdrType(hdrtype);

	m_canvas->Init();
	if (gm.flag & GAME_SHOW_MOUSE) {
		m_canvas->SetMouseState(RAS_ICanvas::MOUSE_NORMAL);
	}
	else {
		m_canvas->SetMouseState(RAS_ICanvas::MOUSE_INVISIBLE);
	}

	// Create the inputdevices.
	m_inputDevice = new DEV_InputDevice();
	m_eventConsumer = new DEV_EventConsumer(m_system, m_inputDevice, m_canvas);
	m_system->addEventConsumer(m_eventConsumer);

	m_networkMessageManager = new KX_NetworkMessageManager();

	// Create the ketsjiengine.
	m_ketsjiEngine = new KX_KetsjiEngine();
	KX_SetActiveEngine(m_ketsjiEngine);

	// Set the devices.
	m_ketsjiEngine->SetInputDevice(m_inputDevice);
	m_ketsjiEngine->SetCanvas(m_canvas);
	m_ketsjiEngine->SetRasterizer(m_rasterizer);
	m_ketsjiEngine->SetNetworkMessageManager(m_networkMessageManager);

	DEV_Joystick::Init();

	m_ketsjiEngine->SetExitKey(BL_ConvertKeyCode(gm.exitkey));
#ifdef WITH_PYTHON
	EXP_Value::SetDeprecationWarnings(nodepwarnings);
#else
	(void)nodepwarnings;
#endif

	m_ketsjiEngine->SetFlag(flags, true);
	m_ketsjiEngine->SetRender(true);
	m_ketsjiEngine->SetShowBoundingBox((KX_DebugOption)showBoundingBox);
	m_ketsjiEngine->SetShowArmatures((KX_DebugOption)showArmatures);
	m_ketsjiEngine->SetShowCameraFrustum((KX_DebugOption)showCameraFrustum);
	m_ketsjiEngine->SetShowShadowFrustum((KX_DebugOption)showShadowFrustum);

	m_ketsjiEngine->SetTicRate(gm.ticrate);
	m_ketsjiEngine->SetMaxLogicFrame(gm.maxlogicstep);
	m_ketsjiEngine->SetMaxPhysicsFrame(gm.maxphystep);
	m_ketsjiEngine->SetTimeScale(gm.timeScale);

	// Set the global settings (carried over if restart/load new files).
	m_ketsjiEngine->SetGlobalSettings(m_globalSettings);

	InitCamera();

#ifdef WITH_PYTHON
	KX_SetMainPath(std::string(m_maggie->name));
	// Some python things.
	initGamePython(m_maggie, m_globalDict);
#endif  // WITH_PYTHON

	// Create a scene converter, create and convert the stratingscene.
	m_converter = new BL_Converter(m_maggie, m_ketsjiEngine, m_alwaysUseExpandFraming, m_camZoom);
	m_ketsjiEngine->SetConverter(m_converter);

	m_kxStartScene = m_ketsjiEngine->CreateScene(m_startScene);

	KX_SetActiveScene(m_kxStartScene);


#ifdef WITH_AUDASPACE
	// Initialize 3D Audio Settings.
	AUD_Device *device = BKE_sound_get_device();
	AUD_Device_setSpeedOfSound(device, m_startScene->audio.speed_of_sound);
	AUD_Device_setDopplerFactor(device, m_startScene->audio.doppler_factor);
	AUD_Device_setDistanceModel(device, AUD_DistanceModel(m_startScene->audio.distance_model));
#endif  // WITH_AUDASPACE

	// Convert scene data.
	m_converter->ConvertScene(m_kxStartScene);

	m_ketsjiEngine->AddScene(m_kxStartScene);
	m_kxStartScene->Release();

	m_rasterizer->Init();
	m_ketsjiEngine->StartEngine();

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
	Texture::FreeAllTextures(nullptr);
#endif  // WITH_PYTHON

	DEV_Joystick::Close();
	m_ketsjiEngine->StopEngine();

	// Set anisotropic settign back to its original value.
	m_rasterizer->SetAnisotropicFiltering(m_savedData.anisotropic);
	// Set mipmap setting back to its original value.
	m_rasterizer->SetMipmapping(m_savedData.mipmap);

	if (m_converter) {
		delete m_converter;
		m_converter = nullptr;
	}
	if (m_ketsjiEngine) {
		delete m_ketsjiEngine;
		m_ketsjiEngine = nullptr;
	}
	if (m_inputDevice) {
		delete m_inputDevice;
		m_inputDevice = nullptr;
	}
	if (m_eventConsumer) {
		m_system->removeEventConsumer(m_eventConsumer);
		delete m_eventConsumer;
	}
	if (m_rasterizer) {
		delete m_rasterizer;
		m_rasterizer = nullptr;
	}
	if (m_canvas) {
		delete m_canvas;
		m_canvas = nullptr;
	}
	if (m_networkMessageManager) {
		delete m_networkMessageManager;
		m_networkMessageManager = nullptr;
	}

	RAS_OverrideShader::DeinitShaders();

	// Call this after we're sure nothing needs Python anymore (e.g., destructors).
	exitGamePython();

#ifdef WITH_AUDASPACE
	// Stop all remaining playing sounds.
	AUD_Device_stopAll(BKE_sound_get_device());
#endif  // WITH_AUDASPACE
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
	PythonMainLoopState *pstate = (PythonMainLoopState *)state;
	pstate->m_exitInfo = pstate->m_launcher->EngineNextFrame();
	if (pstate->m_exitInfo.m_code == KX_ExitInfo::NO_REQUEST) {
		return 0;
	}
	else {
		CM_Error("Exit code " << (int)pstate->m_exitInfo.m_code << ": " << pstate->m_exitInfo.m_fileName);
		return 1;
	}
}

#endif

void LA_Launcher::RenderEngine()
{
	// Render the frame.
	m_ketsjiEngine->Render();
}

#ifdef WITH_PYTHON

bool LA_Launcher::GetPythonMainLoopCode(std::string& pythonCode, std::string& pythonFileName)
{
	pythonFileName = KX_GetPythonMain(m_startScene);
	if (pythonFileName.empty()) {
		return false;
	}

	pythonCode = KX_GetPythonCode(m_maggie, pythonFileName);
	if (pythonCode.empty()) {
		CM_Error("cannot yield control to Python: no Python text data block named '" << pythonFileName << "'");
		return false;
	}
	return true;
}

void LA_Launcher::RunPythonMainLoop(const std::string& pythonCode)
{
	PyRun_SimpleString(pythonCode.c_str());
}

#endif  // WITH_PYTHON

KX_ExitInfo LA_Launcher::EngineNextFrame()
{
#ifdef WITH_PYTHON
	// Check if we can create a python console debugging.
	HandlePythonConsole();
#endif
	// Kick the engine.
	bool renderFrame = m_ketsjiEngine->NextFrame();

	// First check if we want to exit.
	KX_ExitInfo exitInfo = m_ketsjiEngine->GetExitInfo();

	if (exitInfo.m_code == KX_ExitInfo::NO_REQUEST) {
		if (renderFrame) {
			RenderEngine();
		}
	}

	m_system->processEvents(false);
	m_system->dispatchEvents();

	SCA_IInputDevice::SCA_EnumInputs exitKey = m_ketsjiEngine->GetExitKey();
	if (m_inputDevice->GetInput(exitKey).Find(SCA_InputEvent::ACTIVE) &&
	    !m_inputDevice->GetHookExitKey()) {
		m_inputDevice->ConvertEvent(exitKey, 0, 0);
		exitInfo.m_code = KX_ExitInfo::BLENDER_ESC;
	}
	else if (m_inputDevice->GetInput(SCA_IInputDevice::WINCLOSE).Find(SCA_InputEvent::ACTIVE) ||
	         m_inputDevice->GetInput(SCA_IInputDevice::WINQUIT).Find(SCA_InputEvent::ACTIVE)) {
		m_inputDevice->ConvertEvent(SCA_IInputDevice::WINCLOSE, 0, 0);
		m_inputDevice->ConvertEvent(SCA_IInputDevice::WINQUIT, 0, 0);
		exitInfo.m_code = KX_ExitInfo::OUTSIDE;
	}

	return exitInfo;
}

KX_ExitInfo LA_Launcher::EngineMainLoop()
{
#ifdef WITH_PYTHON
	std::string pythonCode;
	std::string pythonFileName;
	if (GetPythonMainLoopCode(pythonCode, pythonFileName)) {
		// Set python environement variable.
		KX_SetActiveScene(m_kxStartScene);

		PythonMainLoopState state;
		state.m_launcher = this;
		pynextframestate.state = &state;
		pynextframestate.func = &PythonEngineNextFrame;

		CM_Debug("Yielding control to Python script '" << pythonFileName << "'...");
		RunPythonMainLoop(pythonCode);
		CM_Debug("Exit Python script '" << pythonFileName << "'");

		return state.m_exitInfo;
	}
	else {
		pynextframestate.state = nullptr;
		pynextframestate.func = nullptr;
#endif  // WITH_PYTHON

	KX_ExitInfo exitInfo;
	while (exitInfo.m_code == KX_ExitInfo::NO_REQUEST) {
		exitInfo = EngineNextFrame();
	}

	return exitInfo;

#ifdef WITH_PYTHON
}
#endif
}
