/*
 * Execute JavaScript/TypeScript scripts
 *
 *
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

/** \file gameengine/GameLogic/SCA_JavaScriptController.cpp
 *  \ingroup gamelogic
 */

#include "SCA_JavaScriptController.h"

#ifdef WITH_JAVASCRIPT

#include "KX_V8Engine.h"
#include "KX_V8Bindings.h"
#include "KX_TypeScriptCompiler.h"
#include "KX_Scene.h"
#include "CM_Message.h"

#include <v8.h>

using namespace v8;
using namespace blender;

// initialize static member variables
SCA_JavaScriptController *SCA_JavaScriptController::m_sCurrentController = nullptr;
SCA_JavaScriptController *g_currentJavaScriptController = nullptr;

SCA_JavaScriptController::SCA_JavaScriptController(SCA_IObject *gameobj, int mode)
    : SCA_IController(gameobj),
      m_function_argc(0),
      m_bModified(true),
      m_debug(false),
      m_mode(mode),
      m_use_typescript(false)
{
}

SCA_JavaScriptController::~SCA_JavaScriptController()
{
}

EXP_Value *SCA_JavaScriptController::GetReplica()
{
  SCA_JavaScriptController *replica = new SCA_JavaScriptController(*this);
  replica->m_bModified = true;  // Force recompilation
  replica->ProcessReplica();
  return replica;
}

void SCA_JavaScriptController::SetScriptText(const std::string &text)
{
  m_scriptText = text;
  m_bModified = true;
}

void SCA_JavaScriptController::SetScriptName(const std::string &name)
{
  m_scriptName = name;
}

bool SCA_JavaScriptController::IsTriggered(class SCA_ISensor *sensor)
{
  return (std::find(m_triggeredSensors.begin(), m_triggeredSensors.end(), sensor) !=
          m_triggeredSensors.end());
}

bool SCA_JavaScriptController::Compile()
{
  m_bModified = false;

  KX_V8Engine *engine = KX_V8Engine::GetInstance();
  if (!engine) {
    CM_Error("V8 engine not initialized");
    return false;
  }

  std::string script_to_compile = m_scriptText;

  // Compile TypeScript if needed
  if (m_use_typescript) {
    std::string compiled_js;
    if (!KX_TypeScriptCompiler::Compile(m_scriptText, m_scriptName, compiled_js)) {
      CM_Error("TypeScript compilation failed");
      return false;
    }
    script_to_compile = compiled_js;
  }

  // Create context for this controller
  m_context = engine->CreateContext();
  Context::Scope context_scope(m_context);

  // Initialize bindings in this context
  KX_V8Bindings::InitializeBindings(m_context);

  // Compile script
  Isolate *isolate = engine->GetIsolate();
  Local<String> source_string;
  if (!String::NewFromUtf8(isolate, script_to_compile.c_str()).ToLocal(&source_string)) {
    CM_Error("Failed to convert script to V8 string");
    return false;
  }

  Local<String> resource_name;
  if (!String::NewFromUtf8(isolate, m_scriptName.c_str()).ToLocal(&resource_name)) {
    CM_Error("Failed to convert script name to V8 string");
    return false;
  }

  ScriptOrigin origin(isolate, resource_name);
  Local<Script> script;
  if (!Script::Compile(m_context, source_string, &origin).ToLocal(&script)) {
    CM_Error("JavaScript compilation failed");
    return false;
  }

  m_compiled_script = script;
  return true;
}

bool SCA_JavaScriptController::Import()
{
  m_bModified = false;

  // For module mode, we would load and execute a module
  // This is a simplified version - full implementation would need module system
  CM_Error("Module import not yet fully implemented");
  return false;
}

void SCA_JavaScriptController::Trigger(SCA_LogicManager *logicmgr)
{
#ifdef WITH_JAVASCRIPT
  m_sCurrentController = this;
  g_currentJavaScriptController = this;

  KX_V8Engine *engine = KX_V8Engine::GetInstance();
  if (!engine) {
    CM_Error("V8 engine not initialized");
    m_sCurrentController = nullptr;
    g_currentJavaScriptController = nullptr;
    return;
  }

  switch (m_mode) {
    case SCA_JSEXEC_SCRIPT: {
      if (m_bModified) {
        if (Compile() == false) {
          m_sCurrentController = nullptr;
          g_currentJavaScriptController = nullptr;
          return;
        }
      }

      if (m_compiled_script.IsEmpty()) {
        m_sCurrentController = nullptr;
        g_currentJavaScriptController = nullptr;
        return;
      }

      Context::Scope context_scope(m_context);
      Local<Value> result;
      if (!m_compiled_script->Run(m_context).ToLocal(&result)) {
        CM_Error("JavaScript script execution failed");
      }
      break;
    }
    case SCA_JSEXEC_MODULE: {
      if (m_bModified || m_debug) {
        if (Import() == false) {
          m_sCurrentController = nullptr;
          g_currentJavaScriptController = nullptr;
          return;
        }
      }
      // Module execution would go here
      break;
    }
  }

  m_triggeredSensors.clear();
  m_sCurrentController = nullptr;
  g_currentJavaScriptController = nullptr;
#else
  /* intentionally blank */
#endif
}

KX_Scene *SCA_JavaScriptController::GetScene() const
{
  SCA_IObject *obj = GetParent();
  if (obj) {
    return obj->GetScene();
  }
  return nullptr;
}

#else  // WITH_JAVASCRIPT

void SCA_JavaScriptController::Trigger(SCA_LogicManager *logicmgr)
{
  /* intentionally blank */
}

#endif  // WITH_JAVASCRIPT

/* eof */
