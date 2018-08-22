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

/** \file gameengine/Ketsji/KX_2DFilter.cpp
 *  \ingroup ketsji
 */

#include "KX_2DFilter.h"
#include "KX_2DFilterOffScreen.h"
#include "RAS_Texture.h" // for RAS_Texture::MaxUnits

#include "CM_Message.h"

KX_2DFilter::KX_2DFilter(RAS_2DFilterData& data)
	:RAS_2DFilter(data)
{
}

KX_2DFilter::~KX_2DFilter()
{
}

bool KX_2DFilter::LinkProgram()
{
	return RAS_2DFilter::LinkProgram();
}

#ifdef WITH_PYTHON

bool KX_2DFilter::CheckTexture(int index, int bindCode, const std::string& prefix) const
{
	if (!m_shader) {
		PyErr_Format(PyExc_ValueError, "%s: KX_2DFilter, No valid shader found", prefix.c_str());
		return false;
	}
	if (index < 0 || index >= RAS_Texture::MaxUnits) {
		PyErr_Format(PyExc_ValueError, "%s: KX_2DFilter, index out of range [0, %i]", prefix.c_str(), (RAS_Texture::MaxUnits - 1));
		return false;
	}
	if (bindCode < 0) {
		PyErr_Format(PyExc_ValueError, "%s: KX_2DFilter, bindCode negative", prefix.c_str());
		return false;
	}

	return true;
}

bool KX_2DFilter::SetTextureUniform(int index, const char *samplerName)
{
	if (samplerName) {
		if (GetError()) {
			return false;
		}
		int loc = GetUniformLocation(samplerName);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT, &index, (sizeof(int)), 1);
#else
			SetUniform(loc, index);
#endif
		}
	}

	return true;
}

PyTypeObject KX_2DFilter::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_2DFilter",
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
	&KX_Shader::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_2DFilter::Methods[] = {
	EXP_PYMETHODTABLE(KX_2DFilter, setTexture),
	EXP_PYMETHODTABLE(KX_2DFilter, setCubeMap),
	EXP_PYMETHODTABLE_KEYWORDS(KX_2DFilter, addOffScreen),
	EXP_PYMETHODTABLE_NOARGS(KX_2DFilter, removeOffScreen),
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef KX_2DFilter::Attributes[] = {
	EXP_PYATTRIBUTE_RW_FUNCTION("mipmap", KX_2DFilter, pyattr_get_mipmap, pyattr_set_mipmap),
	EXP_PYATTRIBUTE_RO_FUNCTION("offScreen", KX_2DFilter, pyattr_get_offScreen),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *KX_2DFilter::pyattr_get_mipmap(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_2DFilter *self = static_cast<KX_2DFilter *>(self_v);
	return PyBool_FromLong(self->GetMipmap());
}

int KX_2DFilter::pyattr_set_mipmap(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_2DFilter *self = static_cast<KX_2DFilter *>(self_v);
	int param = PyObject_IsTrue(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "shader.enabled = bool: KX_Shader, expected True or False");
		return PY_SET_ATTR_FAIL;
	}

	self->SetMipmap(param);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_2DFilter::pyattr_get_offScreen(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_2DFilter *self = static_cast<KX_2DFilter *>(self_v);
	RAS_2DFilterOffScreen *offScreen = self->GetOffScreen();
	return offScreen ? static_cast<KX_2DFilterOffScreen *>(offScreen)->GetProxy() : Py_None;
}

EXP_PYMETHODDEF_DOC(KX_2DFilter, setTexture, "setTexture(index, bindCode, samplerName)")
{
	int index = 0;
	int bindCode = 0;
	char *samplerName = nullptr;

	if (!PyArg_ParseTuple(args, "ii|s:setTexture", &index, &bindCode, &samplerName)) {
		return nullptr;
	}

	if (!CheckTexture(index, bindCode, "setTexture(index, bindCode, samplerName)")) {
		return nullptr;
	}

	if (!SetTextureUniform(index, samplerName)) {
		return nullptr;
	}

	m_textures[index] = {RAS_Texture::GetTexture2DType(), bindCode};
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_2DFilter, setCubeMap, "setCubeMap(index, bindCode, samplerName)")
{
	int index = 0;
	int bindCode = 0;
	char *samplerName = nullptr;

	if (!PyArg_ParseTuple(args, "ii|s:setCubeMap", &index, &bindCode, &samplerName)) {
		return nullptr;
	}

	if (!CheckTexture(index, bindCode, "setCubeMap(index, bindCode, samplerName)")) {
		return nullptr;
	}

	if (!SetTextureUniform(index, samplerName)) {
		return nullptr;
	}

	m_textures[index] = {RAS_Texture::GetCubeMapTextureType(), bindCode};
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_2DFilter, addOffScreen, " addOffScreen(slots, depth, width, height, hdr,  mipmap)")
{
	int slots;
	int depth = 0;
	int width = -1;
	int height = -1;
	int hdr = RAS_Rasterizer::RAS_HDR_NONE;
	int mipmap = 0;
	int flag = 0;

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "i|iiiii:addOffScreen",
	                                   {"slots", "depth", "width", "height", "hdr", "mipmap", 0},
	                                   &slots, &depth, &width, &height, &hdr, &mipmap)) {
		return nullptr;
	}

	if (GetOffScreen()) {
		PyErr_SetString(PyExc_TypeError, "filter.addOffScreen(...): KX_2DFilter, custom off screen already exists.");
		return nullptr;
	}

	if (slots < 0 || slots >= 8) {
		PyErr_SetString(PyExc_TypeError, "filter.addOffScreen(...): KX_2DFilter, slots must be between 0 and 8 excluded.");
		return nullptr;
	}

	if (hdr < 0 || hdr > RAS_Rasterizer::RAS_HDR_MAX) {
		PyErr_SetString(PyExc_TypeError, "filter.addOffScreen(...): KX_2DFilter, invalid hdr value.");
		return nullptr;
	}

	if (width < -1 || height < -1 || width == 0 || height == 0) {
		PyErr_SetString(PyExc_TypeError, "filter.addOffScreen(...): KX_2DFilter, invalid size values.");
		return nullptr;
	}

	if (slots == 0 && !depth) {
		PyErr_SetString(PyExc_TypeError, "filter.addOffScreen(...): KX_2DFilter, empty off screen, slots must be at least to 1 or depth to True.");
		return nullptr;
	}

	if (width == -1 || height == -1) {
		flag |= RAS_2DFilterOffScreen::RAS_VIEWPORT_SIZE;
	}

	if (mipmap) {
		flag |= RAS_2DFilterOffScreen::RAS_MIPMAP;
	}

	if (depth) {
		flag |= RAS_2DFilterOffScreen::RAS_DEPTH;
	}

	KX_2DFilterOffScreen *offScreen = new KX_2DFilterOffScreen(slots, (RAS_2DFilterOffScreen::Flag)flag, width, height,
	                                                           (RAS_Rasterizer::HdrType)hdr);

	SetOffScreen(offScreen);

	return offScreen->GetProxy();
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_2DFilter, removeOffScreen, " removeOffScreen()")
{
	SetOffScreen(nullptr);
	Py_RETURN_NONE;
}

#endif  // WITH_PYTHON
