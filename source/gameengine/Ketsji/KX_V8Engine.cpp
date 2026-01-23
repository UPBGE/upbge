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

#  include "v8_include.h"
#  include <libplatform/libplatform.h>

#  include "KX_V8Engine.h"
#  include "CM_Message.h"

using namespace v8;

struct KX_V8EngineImpl {
  v8::Global<v8::Context> default_context;
};

KX_V8Engine *KX_V8Engine::s_instance = nullptr;
bool KX_V8Engine::s_initialized = false;

KX_V8Engine::KX_V8Engine()
    : m_isolate(nullptr),
      m_platform(nullptr),
      m_array_buffer_allocator(nullptr),
      m_impl(std::make_unique<KX_V8EngineImpl>())
{
}

KX_V8Engine::~KX_V8Engine()
{
  m_impl->default_context.Reset();  /* release before isolate dispose */
  if (m_isolate) {
    m_isolate->Exit();
    m_isolate->Dispose();
  }
  m_isolate = nullptr;
}

bool KX_V8Engine::Initialize()
{
  // Restart: V8/platform still initialized from a prior game session (ESC), but
  // isolate was disposed. V8 cannot be re-initialized (kPlatformDisposed is
  // terminal), so we only create a new isolate and default context.
  if (s_initialized && s_instance && s_instance->m_platform && !s_instance->m_isolate) {
    Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    Isolate *isolate = Isolate::New(create_params);
    isolate->Enter();
    s_instance->m_isolate = isolate;
    s_instance->m_array_buffer_allocator = create_params.array_buffer_allocator;
    s_instance->CreateDefaultContext();
    return true;
  }

  if (s_initialized) {
    return true;
  }

  // First run: full V8 init. V8 does not support re-init after Dispose/DisposePlatform.
  V8::InitializeICUDefaultLocation(".");
  // Do NOT call InitializeExternalStartupData: the NuGet/prebuilt V8 has
  // the snapshot embedded. Calling it with "." would make V8 look for
  // snapshot_blob.bin in CWD; if missing or from another build, heap
  // corruption and "tagged-impl" / Context::Enter crashes can occur.
  v8::Platform *platform = v8::platform::NewDefaultPlatform().release();
  V8::InitializePlatform(platform);
  V8::Initialize();

  Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  Isolate *isolate = Isolate::New(create_params);
  isolate->Enter();

  s_instance = new KX_V8Engine();
  s_instance->m_isolate = isolate;
  s_instance->m_array_buffer_allocator = create_params.array_buffer_allocator;
  s_instance->m_platform = platform;

  s_instance->CreateDefaultContext();

  s_initialized = true;
  return true;
}

void KX_V8Engine::Shutdown()
{
  if (!s_initialized) {
    return;
  }

  // Only tear down the isolate and allocator. Do NOT call V8::Dispose() or
  // V8::DisposePlatform(): V8 cannot be re-initialized after that (kPlatformDisposed
  // is terminal). Keeping the platform allows the next P (Initialize) to create
  // a new isolate and run the game again. s_instance and m_platform are kept.
  if (s_instance) {
    s_instance->m_impl->default_context.Reset();
    if (s_instance->m_isolate) {
      s_instance->m_isolate->Exit();
      s_instance->m_isolate->Dispose();
      s_instance->m_isolate = nullptr;
    }
    if (s_instance->m_array_buffer_allocator) {
      delete static_cast<v8::ArrayBuffer::Allocator *>(s_instance->m_array_buffer_allocator);
      s_instance->m_array_buffer_allocator = nullptr;
    }
  }
  // Keep s_initialized = true and s_instance so Initialize() can restart by
  // creating a new isolate when the user presses P again.
}

void KX_V8Engine::FinalShutdown()
{
  if (!s_initialized) {
    return;
  }

  // Complete cleanup for final Blender exit - free all resources
  // First, reset all V8 handles while isolate is still valid
  v8::Platform *platform_to_delete = nullptr;
  if (s_instance) {
    // Reset default context first (while isolate is still valid)
    s_instance->m_impl->default_context.Reset();
    
    // Now dispose isolate (this will invalidate all handles)
    if (s_instance->m_isolate) {
      s_instance->m_isolate->Exit();
      s_instance->m_isolate->Dispose();
      s_instance->m_isolate = nullptr;
    }
    
    // Clean up allocator
    if (s_instance->m_array_buffer_allocator) {
      delete static_cast<v8::ArrayBuffer::Allocator *>(s_instance->m_array_buffer_allocator);
      s_instance->m_array_buffer_allocator = nullptr;
    }
    
    // Save platform pointer before deleting instance
    platform_to_delete = s_instance->m_platform;
    s_instance->m_platform = nullptr;
    delete s_instance;
    s_instance = nullptr;
  }

  // Cleanup V8 platform and engine (only safe on final exit)
  // Order matters: Dispose() first, then DisposePlatform(), then delete platform
  V8::Dispose();
  V8::DisposePlatform();
  if (platform_to_delete) {
    delete platform_to_delete;
  }

  s_initialized = false;
}

KX_V8Engine *KX_V8Engine::GetInstance()
{
  return s_instance;
}

void KX_V8Engine::CreateDefaultContext()
{
  Isolate::Scope isolate_scope(m_isolate);
  HandleScope handle_scope(m_isolate);

  Local<ObjectTemplate> global = ObjectTemplate::New(m_isolate);
  Local<Context> context = Context::New(m_isolate, nullptr, global);
  // Store in Global so the context outlives this HandleScope. A Local would
  // become invalid when the scope is destroyed, causing use-after-free in
  // Context::Enter (SnapshotCreator/tagged-impl crash).
  m_impl->default_context.Reset(m_isolate, context);
}

Local<Context> KX_V8Engine::CreateContext()
{
  if (!m_isolate) {
    return Local<Context>();
  }
  Isolate::Scope isolate_scope(m_isolate);
  EscapableHandleScope handle_scope(m_isolate);

  Local<ObjectTemplate> global = ObjectTemplate::New(m_isolate);
  Local<Context> context = Context::New(m_isolate, nullptr, global);
  if (context.IsEmpty()) {
    return Local<Context>();
  }
  return handle_scope.Escape(context);
}

bool KX_V8Engine::ExecuteString(const std::string &source,
                                 const std::string &name,
                                 Local<Value> *result,
                                 bool report_exceptions)
{
  return ExecuteStringInContext(GetDefaultContext(), source, name, result, report_exceptions);
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

  ScriptOrigin origin(resource_name);
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
  return m_impl->default_context.Get(m_isolate);
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
