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

/** \file gameengine/BlenderRoutines/LA_SystemCommandLine.cpp
 *  \ingroup blroutines
 */

#include "LA_SystemCommandLine.h"

#include <map>
#include <string>

struct SingletonSystem {
  std::map<std::string, int> int_params;
  std::map<std::string, float> float_params;
  std::map<std::string, std::string> string_params;
};

static SingletonSystem *_system_instance = nullptr;

SYS_SystemHandle SYS_GetSystem()
{
  if (!_system_instance)
    _system_instance = new SingletonSystem();

  return (SYS_SystemHandle)_system_instance;
}

void SYS_DeleteSystem(SYS_SystemHandle sys)
{
  if (_system_instance) {
    delete _system_instance;
    _system_instance = nullptr;
  }
}

int SYS_GetCommandLineInt(SYS_SystemHandle sys, const char *paramname, int defaultvalue)
{
  std::map<std::string, int>::iterator it = ((SingletonSystem *)sys)->int_params.find(paramname);
  if (it != ((SingletonSystem *)sys)->int_params.end()) {
    return it->second;
  }

  return defaultvalue;
}

float SYS_GetCommandLineFloat(SYS_SystemHandle sys, const char *paramname, float defaultvalue)
{
  std::map<std::string, float>::iterator it =
      ((SingletonSystem *)sys)->float_params.find(paramname);
  if (it != ((SingletonSystem *)sys)->float_params.end()) {
    return it->second;
  }

  return defaultvalue;
}

const char *SYS_GetCommandLineString(SYS_SystemHandle sys,
                                     const char *paramname,
                                     const char *defaultvalue)
{
  std::map<std::string, std::string>::iterator it =
      ((SingletonSystem *)sys)->string_params.find(paramname);
  if (it != ((SingletonSystem *)sys)->string_params.end()) {
    return it->second.c_str();
  }

  return defaultvalue;
}

void SYS_WriteCommandLineInt(SYS_SystemHandle sys, const char *paramname, int value)
{
  ((SingletonSystem *)sys)->int_params[paramname] = value;
}

void SYS_WriteCommandLineFloat(SYS_SystemHandle sys, const char *paramname, float value)
{
  ((SingletonSystem *)sys)->float_params[paramname] = value;
}

void SYS_WriteCommandLineString(SYS_SystemHandle sys, const char *paramname, const char *value)
{
  ((SingletonSystem *)sys)->string_params[paramname] = value;
}
