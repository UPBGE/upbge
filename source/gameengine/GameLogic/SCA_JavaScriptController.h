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
 * The Original Code is Copyright (C) 2024 UPBGE Contributors
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SCA_JavaScriptController.h
 *  \ingroup gamelogic
 *  \brief Execute JavaScript/TypeScript scripts
 */

#pragma once

#include <memory>
#include <vector>
#include <string>

#include "EXP_BoolValue.h"
#include "SCA_IController.h"
#include "SCA_LogicManager.h"

class SCA_IObject;
class KX_Scene;

#ifdef WITH_JAVASCRIPT
struct SCA_JavaScriptControllerV8;
#endif

class SCA_JavaScriptController : public SCA_IController {
#ifdef WITH_JAVASCRIPT
  std::unique_ptr<SCA_JavaScriptControllerV8> m_v8;
#endif
  int m_function_argc;
  bool m_bModified;
  bool m_debug;  /* use with SCA_JSEXEC_MODULE for reloading every logic run */
  int m_mode;
  bool m_use_typescript;

 protected:
  std::string m_scriptText;
  std::string m_scriptName;
  std::vector<class SCA_ISensor *> m_triggeredSensors;

 public:
  enum SCA_JSExecMode { SCA_JSEXEC_SCRIPT = 0, SCA_JSEXEC_MODULE, SCA_JSEXEC_MAX };

  static SCA_JavaScriptController *m_sCurrentController;  // protected !!!

  SCA_JavaScriptController(SCA_IObject *gameobj, int mode);
  SCA_JavaScriptController(const SCA_JavaScriptController &other);
  virtual ~SCA_JavaScriptController();

  virtual EXP_Value *GetReplica();
  virtual void Trigger(class SCA_LogicManager *logicmgr);

  void SetScriptText(const std::string &text);
  void SetScriptName(const std::string &name);
  void SetDebug(bool debug)
  {
    m_debug = debug;
  }
  void SetUseTypeScript(bool use_ts)
  {
    m_use_typescript = use_ts;
  }
  int GetMode() const
  {
    return m_mode;
  }
  void AddTriggeredSensor(class SCA_ISensor *sensor)
  {
    m_triggeredSensors.push_back(sensor);
  }
  bool IsTriggered(class SCA_ISensor *sensor);
  bool Compile();
  bool Import();

  KX_Scene *GetScene();
};

// Global reference for bindings
extern SCA_JavaScriptController *g_currentJavaScriptController;
