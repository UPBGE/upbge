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

/** \file KX_V8Engine.h
 *  \ingroup ketsji
 *  \brief V8 JavaScript Engine wrapper
 */

#pragma once

#ifdef WITH_JAVASCRIPT

#include <string>
#include <memory>

namespace v8 {
class Isolate;
class Context;
class String;
class Platform;
namespace platform {
class Platform;
}
namespace ArrayBuffer {
class Allocator;
}
template <class T>
class Local;
class TryCatch;
}  // namespace v8

class KX_V8Engine {
 public:
  KX_V8Engine();
  ~KX_V8Engine();

  // Initialize V8 engine (singleton)
  static bool Initialize();
  static void Shutdown();

  // Get singleton instance
  static KX_V8Engine *GetInstance();

  // Create a new context for script execution
  v8::Local<v8::Context> CreateContext();

  // Execute JavaScript code
  bool ExecuteString(const std::string &source,
                     const std::string &name,
                     v8::Local<v8::Value> *result = nullptr,
                     bool report_exceptions = true);

  // Execute JavaScript code in a specific context
  bool ExecuteStringInContext(v8::Local<v8::Context> context,
                              const std::string &source,
                              const std::string &name,
                              v8::Local<v8::Value> *result = nullptr,
                              bool report_exceptions = true);

  // Get current isolate
  v8::Isolate *GetIsolate() const
  {
    return m_isolate;
  }

  // Get default context
  v8::Local<v8::Context> GetDefaultContext() const;

  // Report exceptions
  void ReportException(v8::TryCatch *try_catch);

 private:
  static KX_V8Engine *s_instance;
  static bool s_initialized;

  v8::Isolate *m_isolate;
  v8::Local<v8::Context> m_default_context;

  // Platform and array buffer allocator (managed by V8)
  v8::platform::Platform *m_platform;
  v8::ArrayBuffer::Allocator *m_array_buffer_allocator;

  // Initialize platform
  bool InitializePlatform();

  // Shutdown platform
  void ShutdownPlatform();
};

#endif  // WITH_JAVASCRIPT
