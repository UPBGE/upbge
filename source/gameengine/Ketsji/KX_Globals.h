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

#pragma once

#include <string>

#include "MT_Vector3.h"
#include "MT_Vector4.h"

class KX_KetsjiEngine;
class KX_Scene;
class PHY_IPhysicsEnvironment;

void KX_SetActiveEngine(KX_KetsjiEngine *engine);
void KX_SetActiveScene(KX_Scene *scene);
void KX_SetMainPath(const std::string &path);
void KX_SetOrigPath(const std::string &path);

KX_KetsjiEngine *KX_GetActiveEngine();
KX_Scene *KX_GetActiveScene();
PHY_IPhysicsEnvironment *KX_GetPhysicsEnvironment();
const std::string &KX_GetMainPath();
const std::string &KX_GetOrigPath();

void KX_RasterizerDrawDebugLine(const MT_Vector3 &from,
                                const MT_Vector3 &to,
                                const MT_Vector4 &color);
void KX_RasterizerDrawDebugCircle(const MT_Vector3 &center,
                                  const MT_Scalar radius,
                                  const MT_Vector4 &color,
                                  const MT_Vector3 &normal,
                                  int nsector);
