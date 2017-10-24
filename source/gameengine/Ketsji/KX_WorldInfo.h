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

/** \file KX_WorldInfo.h
 *  \ingroup ketsji
 */

#ifndef __KX_WORLDINFO_H__
#define __KX_WORLDINFO_H__

#include "MT_Scalar.h"
#include "MT_Vector4.h"
#include "KX_KetsjiEngine.h"
#include "EXP_PyObjectPlus.h"

struct Scene;
struct World;

class KX_WorldInfo : public PyObjectPlus
{
	Py_Header

	std::string m_name;
	Scene *m_scene;

public:

	KX_WorldInfo(Scene *blenderscene, World *blenderworld);
	~KX_WorldInfo();

	void RenderBackground();

	const std::string& GetName();

#ifdef WITH_PYTHON
	virtual PyObject *py_repr(void);
#endif
};

#endif  /* __KX_WORLDINFO_H__ */
