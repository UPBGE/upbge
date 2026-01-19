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

/** \file KX_TypeScriptCompiler.h
 *  \ingroup ketsji
 *  \brief TypeScript to JavaScript compiler
 */

#pragma once

#ifdef WITH_TYPESCRIPT

#include <string>

class KX_TypeScriptCompiler {
 public:
  // Compile TypeScript source to JavaScript
  // Returns true on success, false on failure
  static bool Compile(const std::string &typescript_source,
                     const std::string &source_name,
                     std::string &javascript_output);

  // Check if TypeScript compiler is available
  static bool IsAvailable();

 private:
  // Compile using external tsc process
  static bool CompileWithTSC(const std::string &typescript_source,
                              const std::string &source_name,
                              std::string &javascript_output);
};

#endif  // WITH_TYPESCRIPT
