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

/** \file KX_OffScreen.h
*  \ingroup ketsji
*  \brief Python proxy of RAS_IOffScreen and RAS_OpenGLOffScreen.
*/

#ifndef __KX_OFFSCREEN_H__
#define __KX_OFFSCREEN_H__

#include "EXP_Value.h"
#include "RAS_IOffScreen.h"

class RAS_IRasterizer;
class RAS_ICanvas;

class KX_OffScreen : public CValue
{
	Py_Header
private:
	RAS_IOffScreen *m_ofs;

public:
	KX_OffScreen(RAS_IRasterizer *rasterizer, RAS_ICanvas *canvas, int width, int height, int samples, RAS_IOffScreen::RAS_OFS_RENDER_TARGET target);
	virtual ~KX_OffScreen();

	virtual STR_String& GetName();

	RAS_IOffScreen *GetOffScreen() const;

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_width(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_height(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);

#endif
};

#endif  // __KX_OFFSCREEN_H__
