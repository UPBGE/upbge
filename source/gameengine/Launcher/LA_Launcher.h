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

/** \file LA_Launcher.h
 *  \ingroup launcher
 */

#pragma once

#include <string>

#include "KX_ISystem.h"
#include "KX_KetsjiEngine.h"
#include "RAS_Rasterizer.h"
#include "SCA_IInputDevice.h"

class KX_Scene;
class KX_ISystem;
class BL_Converter;
class KX_NetworkMessageManager;
class RAS_ICanvas;
class DEV_EventConsumer;
class DEV_InputDevice;
class GHOST_ISystem;
struct Scene;
struct Main;

class LA_Launcher {
 protected:
  /// \section The game data.
  std::string m_startSceneName;
  Scene *m_startScene;
  Main *m_maggie;
  struct bContext *m_context;
  KX_Scene *m_kxStartScene;
  bool m_useViewportRender;
  int m_shadingTypeRuntime;

  /// \section Exit state.
  KX_ExitRequest m_exitRequested;
  std::string m_exitString;
  GlobalSettings *m_globalSettings;

  /// GHOST system abstraction.
  GHOST_ISystem *m_system;

  /// The gameengine itself.
  KX_KetsjiEngine *m_ketsjiEngine;
  /// The game engine's system abstraction.
  KX_ISystem *m_kxsystem;
  /// The game engine's input device abstraction.
  DEV_InputDevice *m_inputDevice;
  DEV_EventConsumer *m_eventConsumer;
  /// The game engine's canvas abstraction.
  RAS_ICanvas *m_canvas;
  /// The rasterizer.
  RAS_Rasterizer *m_rasterizer;
  /// Converts Blender data files.
  BL_Converter *m_converter;
  /// Manage messages.
  KX_NetworkMessageManager *m_networkMessageManager;

#ifdef WITH_PYTHON
  PyObject *m_globalDict;
  PyObject *m_gameLogic;
#endif  // WITH_PYTHON

  /// The number of render samples.
  int m_samples;

  /// The render stereo mode passed in constructor.
  RAS_Rasterizer::StereoMode m_stereoMode;

  /// argc and argv need to be passed on to python
  int m_argc;
  char **m_argv;

  /// avoid to run audaspace code if audio device fails to initialize
  bool m_audioDeviceIsInitialized;

  /// Saved data to restore at the game end.
  struct SavedData {
    int vsync;
    RAS_Rasterizer::MipmapOption mipmap;
    int anisotropic;
  } m_savedData;

  struct PythonConsole {
    bool use;
    std::vector<SCA_IInputDevice::SCA_EnumInputs> keys;
  } m_pythonConsole;

#ifdef WITH_PYTHON
  void HandlePythonConsole();
#endif  // WITH_PYTHON

  /// Execute engine render, overrided to render background.
  virtual void RenderEngine();

#ifdef WITH_PYTHON
  /** Return true if the user use a valid python script for main loop and copy the python code
   * to pythonCode and file name to pythonFileName. Else return false.
   * This function print itself error messages for invalid script name and only pythonCode
   * value must be freed.
   */
  virtual bool GetPythonMainLoopCode(std::string &pythonCode, std::string &pythonFileName);
  virtual void RunPythonMainLoop(const std::string &pythonCode);
#endif  // WITH_PYTHON

  virtual RAS_ICanvas *CreateCanvas() = 0;
  virtual bool GetUseAlwaysExpandFraming() = 0;
  virtual void InitCamera() = 0;
  virtual void InitPython() = 0;
  virtual void ExitPython() = 0;

 public:
  LA_Launcher(GHOST_ISystem *system,
              Main *maggie,
              Scene *scene,
              GlobalSettings *gs,
              RAS_Rasterizer::StereoMode stereoMode,
              int samples,
              int argc,
              char **argv,
              struct bContext *C,
              bool useViewportRender,
              int shadingTypeRuntime);
  virtual ~LA_Launcher();

#ifdef WITH_PYTHON
  /// Setup python global dictionnary, used outside constructor to compile without python.
  void SetPythonGlobalDict(PyObject *globalDict);
#endif  // WITH_PYTHON

  KX_ExitRequest GetExitRequested();
  const std::string &GetExitString();
  GlobalSettings *GetGlobalSettings();

  inline KX_Scene *GetStartScene() const
  {
    return m_kxStartScene;
  }

  /// Initializes the game engine.
  virtual void InitEngine();
  /// Shuts the game engine down.
  virtual void ExitEngine();

  /// Compute next frame.
  virtual bool EngineNextFrame();
  /// Execute the loop of the engine, return when receive a exit request from the engine.
  void EngineMainLoop();

#ifdef WITH_PYTHON
  static int PythonEngineNextFrame(void *state);
#endif  // WITH_PYTHON
};
