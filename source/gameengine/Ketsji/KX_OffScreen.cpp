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

/** \file gameengine/Ketsji/KX_OffScreen.cpp
*  \ingroup ketsji
*/

#include "KX_OffScreen.h"
#include "RAS_IRasterizer.h"

KX_OffScreen::KX_OffScreen(RAS_IRasterizer *rasterizer, RAS_ICanvas *canvas, int width, int height, int samples, RAS_IOffScreen::RAS_OFS_RENDER_TARGET target)
{
	m_ofs = rasterizer->CreateOffScreen(canvas, width, height, samples, target);
}

KX_OffScreen::~KX_OffScreen()
{
	delete m_ofs;
}

STR_String& KX_OffScreen::GetName()
{
	static STR_String offscreenname = "KX_OffScreen";
	return offscreenname;
}

RAS_IOffScreen *KX_OffScreen::GetOffScreen() const
{
	return m_ofs;
}

#ifdef WITH_PYTHON

PyTypeObject KX_OffScreen::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_OffScreen",
	sizeof(PyObjectPlus_Proxy),
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
	&CValue::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_OffScreen::Methods[] = {
	{NULL, NULL} // Sentinel
};

PyAttributeDef KX_OffScreen::Attributes[] = {
	KX_PYATTRIBUTE_RO_FUNCTION("width", KX_OffScreen, pyattr_get_width),
	KX_PYATTRIBUTE_RO_FUNCTION("height", KX_OffScreen, pyattr_get_height),
	KX_PYATTRIBUTE_RO_FUNCTION("color", KX_OffScreen, pyattr_get_color),
	{NULL} // Sentinel
};

PyObject *KX_OffScreen::pyattr_get_width(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_OffScreen *self = static_cast<KX_OffScreen *>(self_v);

	return PyLong_FromLong(self->GetOffScreen()->GetWidth());
}

PyObject *KX_OffScreen::pyattr_get_height(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_OffScreen *self = static_cast<KX_OffScreen *>(self_v);

	return PyLong_FromLong(self->GetOffScreen()->GetHeight());
}

PyObject *KX_OffScreen::pyattr_get_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_OffScreen *self = static_cast<KX_OffScreen *>(self_v);

	return PyLong_FromLong(self->GetOffScreen()->GetColor());
}

#endif  // WITH_PYTHON
