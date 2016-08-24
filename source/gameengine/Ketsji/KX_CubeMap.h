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

/** \file KX_CubeMap.h
*  \ingroup ketsji
*/

#ifndef __KX_CUBEMAP_H__
#define __KX_CUBEMAP_H__

#include "RAS_CubeMap.h"
#include "EXP_Value.h"

class KX_BlenderSceneConverter;
class KX_GameObject;

class KX_CubeMap : public RAS_CubeMap, public CValue
{
	Py_Header
private:
	KX_GameObject *m_object;

public:
	KX_CubeMap(KX_BlenderSceneConverter *converter, KX_GameObject *gameobj, RAS_Texture *texture, RAS_IRasterizer *rasty);
	virtual ~KX_CubeMap();

	virtual STR_String& GetName();

	KX_GameObject *GetGameObject() const;
};

#endif  // __KX_CUBEMAP_H__
