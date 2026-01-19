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

/** \file KX_V8Engine.cpp
 *  \ingroup ketsji
 */

#ifdef WITH_JAVASCRIPT

#include "KX_V8Engine.h"

#include "CM_Message.h"

#include <v8.h>
#include <libplatform/libplatform.h>

using namespace v8;

KX_V8Engine *KX_V8Engine::s_instance = nullptr;
bool KX_V8Engine::s_initialized = false;

KX_V8Engine::KX_V8Engine() : m_isolate(nullptr), m_platform(nullptr), m_array_buffer_allocator(nullptr)
{
}

KX_V8Engine::~KX_V8Engine()
{
  if (m_isolate) {
    m_isolate->Exit();
    m_isolate->Dispose();
  }
  m_isolate = nullptr;
}

bool KX_V8Engine::Initialize()
{
  if (s_initialized) {
    return true;
  }

  // Initialize V8 platform
  V8::InitializeICUDefaultLocation(".");
  V8::InitializeExternalStartupData(".");
  v8::Platform *platform = v8::platform::NewDefaultPlatform().release();
  V8::InitializePlatform(platform);
  V8::Initialize();

  // Create isolate
  Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  
  Isolate *isolate = Isolate::New(create_params);
  isolate->Enter();

  // Create instance
  s_instance = new KX_V8Engine();
  s_instance->m_isolate = isolate;
  s_instance->m_array_buffer_allocator = create_params.array_buffer_allocator;
  s_instance->m_platform = platform;

  // Create default context
  s_instance->m_default_context = s_instance->CreateContext();

  s_initialized = true;
  return true;
}

void KX_V8Engine::Shutdown()
{
  if (!s_initialized) {
    return;
  }

  if (s_instance) {
    if (s_instance->m_array_buffer_allocator) {
      delete s_instance->m_array_buffer_allocator;
    }
    if (s_instance->m_platform) {
      V8::ShutdownPlatform();
      delete s_instance->m_platform;
    }
    delete s_instance;
    s_instance = nullptr;
  }

  V8::Dispose();
  s_initialized = false;
}

KX_V8Engine *KX_V8Engine::GetInstance()
{
  return s_instance;
}

Local<Context> KX_V8Engine::CreateContext()
{
  Isolate::Scope isolate_scope(m_isolate);
  HandleScope handle_scope(m_isolate);

  Local<ObjectTemplate> global = ObjectTemplate::New(m_isolate);
  Local<Context> context = Context::New(m_isolate, nullptr, global);
  return context;
}

bool KX_V8Engine::ExecuteString(const std::string &source,
                                 const std::string &name,
                                 Local<Value> *result,
                                 bool report_exceptions)
{
  return ExecuteStringInContext(m_default_context, source, name, result, report_exceptions);
}

bool KX_V8Engine::ExecuteStringInContext(Local<Context> context,
                                          const std::string &source,
                                          const std::string &name,
                                          Local<Value> *result,
                                          bool report_exceptions)
{
  Isolate::Scope isolate_scope(m_isolate);
  HandleScope handle_scope(m_isolate);
  Context::Scope context_scope(context);

  TryCatch try_catch(m_isolate);

  Local<String> source_string;
  if (!String::NewFromUtf8(m_isolate, source.c_str()).ToLocal(&source_string)) {
    if (report_exceptions) {
      CM_Error("Failed to convert source to V8 string");
    }
    return false;
  }

  Local<String> resource_name;
  if (!String::NewFromUtf8(m_isolate, name.c_str()).ToLocal(&resource_name)) {
    if (report_exceptions) {
      CM_Error("Failed to convert script name to V8 string");
    }
    return false;
  }

  ScriptOrigin origin(m_isolate, resource_name);
  Local<Script> script;
  if (!Script::Compile(context, source_string, &origin).ToLocal(&script)) {
    if (report_exceptions) {
      ReportException(&try_catch);
    }
    return false;
  }

  Local<Value> script_result;
  if (!script->Run(context).ToLocal(&script_result)) {
    if (report_exceptions) {
      ReportException(&try_catch);
    }
    return false;
  }

  if (result) {
    *result = script_result;
  }

  return true;
}

Local<Context> KX_V8Engine::GetDefaultContext() const
{
  return m_default_context;
}

void KX_V8Engine::ReportException(TryCatch *try_catch)
{
  HandleScope handle_scope(m_isolate);
  String::Utf8Value exception(m_isolate, try_catch->Exception());
  const char *exception_string = *exception;
  
  Local<Message> message = try_catch->Message();
  if (message.IsEmpty()) {
    CM_Error("JavaScript error: " << exception_string);
  }
  else {
    Local<Context> context = m_isolate->GetCurrentContext();
    String::Utf8Value filename(m_isolate, message->GetScriptResourceName());
    const char *filename_string = *filename;
    int line_number = message->GetLineNumber(context).FromMaybe(0);
    
    Local<String> source_line = message->GetSourceLine(context).ToLocalChecked();
    String::Utf8Value sourceline(m_isolate, source_line);
    const char *sourceline_string = *sourceline;
    
    CM_Error("JavaScript error in " << filename_string << ":" << line_number << ": " << exception_string);
    CM_Error("  " << sourceline_string);
  }
}

#endif  // WITH_JAVASCRIPT
