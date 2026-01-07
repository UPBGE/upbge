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
 * Contributor(s): Benoit Bolsee
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_PythonMain.cpp
 *  \ingroup ketsji
 */

#include "KX_PythonMain.h"

#include "BKE_idprop.hh"
#include "BKE_main.hh"
#include "BKE_text.h"
#include "BLI_listbase.h"
#include "DNA_scene_types.h"

using namespace blender;

std::string KX_GetPythonMain(blender::Scene *scene)
{
  // Examine custom scene properties.
  if (scene->id.properties) {
    blender::IDProperty *item = IDP_GetPropertyTypeFromGroup(scene->id.properties, "__main__", IDP_STRING);
    if (item) {
      return IDP_string_get(item);
    }
  }

  return "";
}

std::string KX_GetPythonCode(blender::Main *bmain, const std::string &python_main)
{
  blender::Text *text = (blender::Text *)BLI_findstring(&bmain->texts, python_main.c_str(), offsetof(blender::ID, name) + 2);
  if (text) {
    size_t buf_len_dummy;
    return txt_to_buf(text, &buf_len_dummy);
  }

  return "";
}
