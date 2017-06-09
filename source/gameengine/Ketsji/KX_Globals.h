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

/** \file KX_Globals.h
 *  \ingroup ketsji
 */

#ifndef __KX_GLOBALS_H__
#define __KX_GLOBALS_H__

#include "mathfu.h"
#include <string>

class KX_KetsjiEngine;
class KX_Scene;

void KX_SetActiveEngine(KX_KetsjiEngine *engine);
void KX_SetActiveScene(KX_Scene *scene);
void KX_SetMainPath(const std::string& path);
void KX_SetOrigPath(const std::string& path);

KX_KetsjiEngine *KX_GetActiveEngine();
KX_Scene *KX_GetActiveScene();
const std::string& KX_GetMainPath();
const std::string& KX_GetOrigPath();

void KX_RasterizerDrawDebugLine(const mt::vec3 &from,const mt::vec3 &to,const mt::vec4 &color);
void KX_RasterizerDrawDebugCircle(const mt::vec3 &center, const float radius, const mt::vec4 &color,
                                  const mt::vec3 &normal, int nsector);

#endif // __KX_GLOBALS_H__
