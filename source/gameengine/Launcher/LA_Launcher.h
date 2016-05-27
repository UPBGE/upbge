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

/** \file LA_Launcher.h
 *  \ingroup player
 */

#ifndef __LA_LAUNCHER_H__
#define __LA_LAUNCHER_H__

#include "KX_KetsjiEngine.h"
#include "KX_ISystem.h"

#include "RAS_IRasterizer.h"

#include "STR_String.h"

class KX_Scene;
class KX_ISystem;
class KX_ISceneConverter;
class KX_NetworkMessageManager;
class RAS_ICanvas;
class RAS_IRasterizer;
class GH_EventConsumer;
class GH_InputDevice;
class GHOST_ISystem;
class GHOST_IWindow;
struct Scene;
struct Main;

class LA_Launcher
{
protected:
	short m_exitkey;

	/// \section The game data.
	STR_String m_startSceneName;
	Scene *m_startScene;
	Main *m_maggie;
	KX_Scene *m_kxStartScene;

	/// \section Exit state.
	int m_exitRequested;
	STR_String m_exitString;
	GlobalSettings *m_globalSettings;

	/// GHOST system abstraction.
	GHOST_ISystem *m_system;

	/// Engine construction state.
	bool m_engineInitialized;
	/// Engine state.
	bool m_engineRunning;
	/// Running on embedded window.
	bool m_isEmbedded;

	/// The gameengine itself.
	KX_KetsjiEngine* m_ketsjiEngine;
	/// The game engine's system abstraction.
	KX_ISystem* m_kxsystem;
	/// The game engine's input device abstraction.
	GH_InputDevice *m_inputDevice;
	GH_EventConsumer *m_eventConsumer;
	/// The game engine's canvas abstraction.
	RAS_ICanvas *m_canvas;
	/// The rasterizer.
	RAS_IRasterizer *m_rasterizer;
	/// Converts Blender data files.
	KX_ISceneConverter *m_sceneConverter;
	/// Manage messages.
	KX_NetworkMessageManager *m_networkMessageManager;

#ifdef WITH_PYTHON
	PyObject *m_globalDict;
	PyObject *m_gameLogic;
	PyObject *m_gameLogicKeys;
#endif  // WITH_PYTHON

	/// The render stereo mode passed in constructor.
	RAS_IRasterizer::StereoMode m_stereoMode;

	/// argc and argv need to be passed on to python
	int m_argc;
	char **m_argv;

	/// Initializes the game engine.
	virtual bool InitEngine();
	/// Shuts the game engine down.
	virtual void ExitEngine();

	/// Starts the game engine.
	bool StartEngine();

	/// Stop the game engine.
	void StopEngine();

	virtual RAS_ICanvas *CreateCanvas(RAS_IRasterizer *rasty) = 0;
	virtual RAS_IRasterizer::DrawType GetRasterizerDrawMode() { return RAS_IRasterizer::RAS_TEXTURED; } 
	virtual bool GetUseAlwaysExpandFraming() { return false; }
	virtual void InitCamera() {}
	virtual void InitPython() = 0;
	virtual void ExitPython() = 0;

public:
	LA_Launcher(GHOST_ISystem *system, Main *maggie, Scene *scene, GlobalSettings *gs, RAS_IRasterizer::StereoMode stereoMode, int argc, char **argv);
	virtual ~LA_Launcher();

	int GetExitRequested();
	const STR_String& GetExitString();
	GlobalSettings *GetGlobalSettings();

	inline KX_Scene *GetStartScene() const
	{
		return m_kxStartScene;
	}

	bool StartGameEngine();
	void StopGameEngine();
	bool EngineNextFrame();

#ifdef WITH_PYTHON
	static int PythonEngineNextFrame(void *state);
#endif  // WITH_PYTHON
};

#endif  // __LA_LAUNCHER_H__
