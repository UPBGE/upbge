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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/BL_Texture.cpp
 *  \ingroup ketsji
 */

#include "glew-mx.h"

#include "BL_Material.h"
#include "BL_Texture.h"

#include "DNA_texture_types.h"

#include "EXP_PyObjectPlus.h"

#include "GPU_texture.h"

BL_Texture::BL_Texture(MTex *mtex, bool cubemap, bool mipmap)
	:CValue(),
	m_bindcode(0),
	m_mtex(NULL),
	m_gputex(NULL)
{
	Tex *tex = mtex->tex;
	Image *ima = tex->ima;
	ImageUser& iuser = tex->iuser;
	const int gltextarget = cubemap ? GL_TEXTURE_CUBE_MAP_ARB : GL_TEXTURE_2D;

	m_gputex = ima ? GPU_texture_from_blender(ima, &iuser, gltextarget, false, 0.0, mipmap) : NULL;

	// Initialize saved data.
	m_mtex = mtex;
	m_mtexname = (STR_String)(mtex->tex->id.name + 2);
	m_savedData.colintensfac = mtex->difffac;
	m_savedData.colfac = mtex->colfac;
	m_savedData.alphafac = mtex->alphafac;
	m_savedData.specintensfac = mtex->specfac;
	m_savedData.speccolorfac = mtex->colspecfac;
	m_savedData.spechardnessfac = mtex->hardfac;
	m_savedData.emitfac = mtex->emitfac;
	m_savedData.mirrorfac = mtex->mirrfac;
	m_savedData.normalfac = mtex->norfac;
	m_savedData.parallaxbumpfac = mtex->parallaxbumpsc;
	m_savedData.parallaxstepfac = mtex->parallaxsteps;
	
	if (m_gputex) {
		m_bindcode = GPU_texture_opengl_bindcode(m_gputex);
		m_savedData.bindcode = m_bindcode;
	}
}

BL_Texture::~BL_Texture()
{
	// Restore saved data.
	if (m_savedData.colintensfac)
		m_mtex->difffac = m_savedData.colintensfac;
	if (m_savedData.colfac)
		m_mtex->colfac = m_savedData.colfac;
	if (m_savedData.alphafac)
		m_mtex->alphafac = m_savedData.alphafac;
	if (m_savedData.specintensfac)
		m_mtex->specfac = m_savedData.specintensfac;
	if (m_savedData.speccolorfac)
		m_mtex->colspecfac = m_savedData.speccolorfac;
	if (m_savedData.spechardnessfac)
		m_mtex->hardfac = m_savedData.spechardnessfac;
	if (m_savedData.emitfac)
		m_mtex->emitfac = m_savedData.emitfac;
	if (m_savedData.mirrorfac)
		m_mtex->mirrfac = m_savedData.mirrorfac;
	if (m_savedData.normalfac)
		m_mtex->norfac = m_savedData.normalfac;
	if (m_savedData.parallaxbumpfac)
		m_mtex->parallaxbumpsc = m_savedData.parallaxbumpfac;
	if (m_savedData.parallaxstepfac)
		m_mtex->parallaxsteps = m_savedData.parallaxstepfac;

	if (m_gputex) {
		GPU_texture_set_opengl_bindcode(m_gputex, m_savedData.bindcode);
	}
}

bool BL_Texture::Ok()
{
	return (m_gputex != NULL);
}

unsigned int BL_Texture::GetTextureType() const
{
	return GPU_texture_target(m_gputex);
}

int BL_Texture::GetMaxUnits()
{
	return MAXTEX;
}

void BL_Texture::ActivateTexture(int unit)
{
	/* Since GPUTexture can be shared between material textures (MTex),
	 * we should reapply the bindcode in case of VideoTexture owned texture.
	 * Without that every material that use this GPUTexture will then use
	 * the VideoTexture texture, it's not wanted. */
	GPU_texture_set_opengl_bindcode(m_gputex, m_bindcode);
	GPU_texture_bind(m_gputex, unit);
}

void BL_Texture::DisableTexture()
{
	GPU_texture_unbind(m_gputex);
}

unsigned int BL_Texture::swapTexture(unsigned int bindcode)
{
	// swap texture codes
	unsigned int tmp = m_bindcode;
	m_bindcode = bindcode;
	// return original texture code
	return tmp;
}

PyTypeObject BL_Texture::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"BL_Texture",
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

PyMethodDef BL_Texture::Methods[] = {
	//{ "getTextureName", (PyCFunction)BL_Texture::sPyGetTextureName, METH_VARARGS },
	{ NULL, NULL } //Sentinel
};

PyAttributeDef BL_Texture::Attributes[] = {
	KX_PYATTRIBUTE_RW_FUNCTION("colorIntensFac", BL_Texture, pyattr_get_colorintensfac, pyattr_set_colorintensfac),
	KX_PYATTRIBUTE_RW_FUNCTION("colorFac", BL_Texture, pyattr_get_colorfac, pyattr_set_colorfac),
	KX_PYATTRIBUTE_RW_FUNCTION("alphaFac", BL_Texture, pyattr_get_alphafac, pyattr_set_alphafac),
	KX_PYATTRIBUTE_RW_FUNCTION("specIntensFac", BL_Texture, pyattr_get_specintensfac, pyattr_set_specintensfac),
	KX_PYATTRIBUTE_RW_FUNCTION("specColorFac", BL_Texture, pyattr_get_speccolorfac, pyattr_set_speccolorfac),
	KX_PYATTRIBUTE_RW_FUNCTION("specHardnessFac", BL_Texture, pyattr_get_spechardnessfac, pyattr_set_spechardnessfac),
	KX_PYATTRIBUTE_RW_FUNCTION("emitFac", BL_Texture, pyattr_get_emitfac, pyattr_set_emitfac),
	KX_PYATTRIBUTE_RW_FUNCTION("mirrorFac", BL_Texture, pyattr_get_mirrorfac, pyattr_set_mirrorfac),
	KX_PYATTRIBUTE_RW_FUNCTION("normalFac", BL_Texture, pyattr_get_normalfac, pyattr_set_normalfac),
	KX_PYATTRIBUTE_RW_FUNCTION("parallaxBumpFac", BL_Texture, pyattr_get_parallaxbumpfac, pyattr_set_parallaxbumpfac),
	KX_PYATTRIBUTE_RW_FUNCTION("parallaxStepFac", BL_Texture, pyattr_get_parallaxstepfac, pyattr_set_parallaxstepfac),
	{ NULL }    //Sentinel
};

// stuff for cvalue related things
CValue *BL_Texture::Calc(VALUE_OPERATOR op, CValue *val)
{
	return NULL;
}

CValue *BL_Texture::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val)
{
	return NULL;
}

const STR_String &BL_Texture::GetText()
{
	return GetName();
}

double BL_Texture::GetNumber()
{
	return -1.0;
}

STR_String &BL_Texture::GetName()
{
	return m_mtexname;
}

void BL_Texture::SetName(const char *name)
{
}

CValue *BL_Texture::GetReplica()
{
	return NULL;
}

PyObject *BL_Texture::pyattr_get_colorintensfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->difffac);
}

int BL_Texture::pyattr_set_colorintensfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, -1.0f, 1.0f);
	self->GetMTex()->difffac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_colorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->colfac);
}

int BL_Texture::pyattr_set_colorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);
	self->GetMTex()->colfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_alphafac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->alphafac);
}

int BL_Texture::pyattr_set_alphafac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, -1.0f, 1.0f);

	self->GetMTex()->alphafac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_specintensfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->specfac);
}

int BL_Texture::pyattr_set_specintensfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 10.0f);

	self->GetMTex()->specfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_speccolorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->colspecfac);
}

int BL_Texture::pyattr_set_speccolorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetMTex()->colspecfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_spechardnessfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->hardfac);
}

int BL_Texture::pyattr_set_spechardnessfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 10.0f);

	self->GetMTex()->hardfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_emitfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->emitfac);
}

int BL_Texture::pyattr_set_emitfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 10.0f);

	self->GetMTex()->emitfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_mirrorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->mirrfac);
}

int BL_Texture::pyattr_set_mirrorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetMTex()->mirrfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_normalfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->norfac);
}

int BL_Texture::pyattr_set_normalfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, -5.0f, 5.0f);

	self->GetMTex()->norfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_parallaxbumpfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->parallaxbumpsc);
}

int BL_Texture::pyattr_set_parallaxbumpfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 0.5f);

	self->GetMTex()->parallaxbumpsc = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_parallaxstepfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->parallaxsteps);
}

int BL_Texture::pyattr_set_parallaxstepfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 100.0f);

	self->GetMTex()->parallaxsteps = val;
	return PY_SET_ATTR_SUCCESS;
}


