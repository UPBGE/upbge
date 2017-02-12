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

/** \file RAS_CubeMap.h
*  \ingroup bgerast
*/

#ifndef __RAS_PROBE_H__
#define __RAS_PROBE_H__

#include "RAS_TextureRenderer.h"

class RAS_CubeMap : public virtual RAS_TextureRenderer
{
public:
	enum {
		NUM_FACES = 6
	};

public:
	/// Face view matrices in 3x3 matrices.
	static const MT_Matrix3x3 faceViewMatrices3x3[NUM_FACES];

	RAS_CubeMap();
	virtual ~RAS_CubeMap();
};

#endif  // __RAS_PROBE_H__
