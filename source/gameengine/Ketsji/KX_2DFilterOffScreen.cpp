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

/** \file gameengine/Ketsji/KX_2DFilterOffScreen.cpp
 *  \ingroup ketsji
 */

#include "KX_2DFilterOffScreen.h"

#include "EXP_ListWrapper.h"

KX_2DFilterOffScreen::KX_2DFilterOffScreen(unsigned short colorSlots, Flag flag, unsigned int width, unsigned int height,
                                           RAS_Rasterizer::HdrType hdr)
	:RAS_2DFilterOffScreen(colorSlots, flag, width, height, hdr)
{
}

KX_2DFilterOffScreen::~KX_2DFilterOffScreen()
{
}

std::string KX_2DFilterOffScreen::GetName() const
{
	return "KX_2DFilterOffScreen";
}

#ifdef WITH_PYTHON

PyTypeObject KX_2DFilterOffScreen::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_2DFilterOffScreen",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_2DFilterOffScreen::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

EXP_Attribute KX_2DFilterOffScreen::Attributes[] = {
	EXP_ATTRIBUTE_RO("width", m_width),
	EXP_ATTRIBUTE_RO("height", m_height),
	EXP_ATTRIBUTE_RO_FUNCTION("colorBindCodes", pyattr_get_colorBindCodes),
	EXP_ATTRIBUTE_RO_FUNCTION("depthBindCode", pyattr_get_depthBindCode),
	EXP_ATTRIBUTE_NULL // Sentinel
};

unsigned int KX_2DFilterOffScreen::py_get_textures_size()
{
	return RAS_2DFilterOffScreen::NUM_COLOR_SLOTS;
}

PyObject *KX_2DFilterOffScreen::py_get_textures_item(unsigned int index)
{
	const int bindCode = GetColorBindCode(index);
	return PyLong_FromLong(bindCode);
}

EXP_BaseListWrapper *KX_2DFilterOffScreen::pyattr_get_colorBindCodes()
{
	return (new EXP_ListWrapper<KX_2DFilterOffScreen, &KX_2DFilterOffScreen::py_get_textures_size, &KX_2DFilterOffScreen::py_get_textures_item>(this));
}

int KX_2DFilterOffScreen::pyattr_get_depthBindCode()
{
	return GetDepthBindCode();
}

#endif  // WITH_PYTHON
