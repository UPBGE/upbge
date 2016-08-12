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
* Contributor(s): Ulysse Martin, Tristan Porteries.
*
* ***** END GPL LICENSE BLOCK *****
*/

/** \file RAS_CubeMapManager.h
*  \ingroup bgerast
*/

#ifndef __RAS_CUBEMAPMANAGER_H__
#define __RAS_CUBEMAPMANAGER_H__

#include "MT_Matrix4x4.h"

#include <vector>

class RAS_CubeMap;

class RAS_CubeMapManager
{
protected:
	std::vector<RAS_CubeMap *> m_cubeMaps;

public:
	RAS_CubeMapManager();
	virtual ~RAS_CubeMapManager();

	static MT_Matrix4x4 facesViewMat[6];
	static MT_Matrix3x3 camOri[6];

	void AddCubeMap(RAS_CubeMap *cubeMap);
	void RemoveCubeMap(void *clientobj);

	void RestoreFrameBuffer();
};

#endif  // __RAS_CUBEMAPMANAGER_H__

