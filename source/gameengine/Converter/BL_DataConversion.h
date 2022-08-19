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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file BL_DataConversion.h
 *  \ingroup bgeconv
 */

#pragma once

#include <string>

#include "EXP_Python.h"
#include "KX_PhysicsEngineEnums.h"
#include "SCA_IInputDevice.h"

class RAS_MeshObject *BL_ConvertMesh(struct Mesh *mesh,
                                     struct Object *lightobj,
                                     class KX_Scene *scene,
                                     class RAS_Rasterizer *rasty,
                                     class BL_SceneConverter *converter,
                                     bool libloading,
                                     bool converting_during_runtime);

void BL_ConvertBlenderObjects(struct Main *maggie,
                              struct Depsgraph *depsgraph,
                              class KX_Scene *kxscene,
                              class KX_KetsjiEngine *ketsjiEngine,
                              e_PhysicsEngine physics_engine,
                              class RAS_Rasterizer *rendertools,
                              class RAS_ICanvas *canvas,
                              class BL_SceneConverter *sceneconverter,
                              struct Object *single_obj,
                              bool alwaysUseExpandFraming,
                              bool libloading = false);

SCA_IInputDevice::SCA_EnumInputs BL_ConvertKeyCode(int key_code);
