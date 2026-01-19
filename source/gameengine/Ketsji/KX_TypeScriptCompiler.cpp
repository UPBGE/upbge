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

/** \file KX_TypeScriptCompiler.cpp
 *  \ingroup ketsji
 */

#ifdef WITH_TYPESCRIPT

#include "KX_TypeScriptCompiler.h"
#include "CM_Message.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifdef WIN32
#  include <windows.h>
#  include <io.h>
#  define popen _popen
#  define pclose _pclose
#else
#  include <unistd.h>
#endif

bool KX_TypeScriptCompiler::Compile(const std::string &typescript_source,
                                    const std::string &source_name,
                                    std::string &javascript_output)
{
  if (!IsAvailable()) {
    CM_Error("TypeScript compiler (tsc) is not available");
    return false;
  }

  return CompileWithTSC(typescript_source, source_name, javascript_output);
}

bool KX_TypeScriptCompiler::IsAvailable()
{
  // Try to run tsc --version
  FILE *pipe = popen("tsc --version", "r");
  if (!pipe) {
    return false;
  }

  char buffer[128];
  std::string result = "";
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    result += buffer;
  }
  pclose(pipe);

  // If we got output, tsc is available
  return !result.empty();
}

bool KX_TypeScriptCompiler::CompileWithTSC(const std::string &typescript_source,
                                           const std::string &source_name,
                                           std::string &javascript_output)
{
  // Create temporary TypeScript file
  std::string temp_ts_file = source_name + ".ts";
  std::ofstream ts_file(temp_ts_file);
  if (!ts_file.is_open()) {
    CM_Error("Failed to create temporary TypeScript file: " << temp_ts_file);
    return false;
  }
  ts_file << typescript_source;
  ts_file.close();

  // Compile with tsc
  std::string command = "tsc --target ES2020 --module none " + temp_ts_file;
  FILE *pipe = popen(command.c_str(), "r");
  if (!pipe) {
    CM_Error("Failed to execute TypeScript compiler");
    return false;
  }

  char buffer[128];
  std::string error_output = "";
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    error_output += buffer;
  }
  int status = pclose(pipe);

  // Check if compilation succeeded
  if (status != 0) {
    CM_Error("TypeScript compilation failed: " << error_output);
    // Clean up temp file
    remove(temp_ts_file.c_str());
    return false;
  }

  // Read compiled JavaScript file
  std::string js_file = source_name + ".js";
  std::ifstream js_stream(js_file);
  if (!js_stream.is_open()) {
    CM_Error("Failed to read compiled JavaScript file: " << js_file);
    remove(temp_ts_file.c_str());
    return false;
  }

  std::stringstream js_buffer;
  js_buffer << js_stream.rdbuf();
  javascript_output = js_buffer.str();
  js_stream.close();

  // Clean up temporary files
  remove(temp_ts_file.c_str());
  remove(js_file.c_str());

  return true;
}

#endif  // WITH_TYPESCRIPT
