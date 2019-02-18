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

#include "BL_Texture.h"

#include "DNA_texture_types.h"

#include "GPU_texture.h"
#include "GPU_draw.h"

#include "KX_PyMath.h"

#include "BLI_math.h"

extern "C" {
#  include "gpu/intern/gpu_codegen.h"
}

BL_Texture::BL_Texture(GPUInput *input)
	:CValue(),
	m_isCubeMap(false),
	m_gpuTex(nullptr),
	m_input(input)
{
	m_isCubeMap = false;
	m_name = input->ima->id.name;

	m_gpuTex = GPU_texture_from_blender(input->ima, input->iuser, GL_TEXTURE_2D, input->image_isdata);

	if (m_gpuTex) {
		m_bindCode = GPU_texture_opengl_bindcode(m_gpuTex);
		m_savedData.bindcode = m_bindCode;
		GPU_texture_ref(m_gpuTex);
	}
}

BL_Texture::~BL_Texture()
{
	if (m_gpuTex) {
		GPU_texture_set_opengl_bindcode(m_gpuTex, m_savedData.bindcode);
		GPU_texture_free(m_gpuTex);
	}
}

void BL_Texture::CheckValidTexture()
{
	if (!m_gpuTex) {
		return;
	}

	/* Test if the gpu texture is the same in the image which own it, if it's not
	 * the case then it means that no materials use it anymore and that we have to
	 * get a pointer of the updated gpu texture used by materials.
	 * The gpu texture in the image can be nullptr or an already different loaded
	 * gpu texture. In both cases we call GPU_texture_from_blender.
	 */
	int target = m_isCubeMap ? TEXTARGET_TEXTURE_CUBE_MAP : TEXTARGET_TEXTURE_2D;
	if (m_gpuTex != m_input->ima->gputexture[target]) {
		// Restore gpu texture original bind cdoe to make sure we will delete the right opengl texture.
		GPU_texture_set_opengl_bindcode(m_gpuTex, m_savedData.bindcode);
		GPU_texture_free(m_gpuTex);

		m_gpuTex = (m_input->ima ? GPU_texture_from_blender(m_input->ima, m_input->iuser, GL_TEXTURE_2D, false) : nullptr);

		if (m_gpuTex) {
			int bindCode = GPU_texture_opengl_bindcode(m_gpuTex);
			// If our bind code was the same as the previous gpu texture bind code, then we update it to the new bind code.
			if (m_bindCode == m_savedData.bindcode) {
				m_bindCode = bindCode;
			}
			m_savedData.bindcode = bindCode;
			GPU_texture_ref(m_gpuTex);
		}
	}
}

bool BL_Texture::Ok() const
{
	return (m_gpuTex != nullptr);
}

bool BL_Texture::IsCubeMap() const
{
	return m_isCubeMap;
}

MTex *BL_Texture::GetMTex() const
{
	return m_mtex; //deprecated
}

Tex *BL_Texture::GetTex() const
{
	return m_mtex->tex; //deprecated
}

Image *BL_Texture::GetImage() const
{
	return m_input->ima;
}

GPUTexture *BL_Texture::GetGPUTexture() const
{
	return m_gpuTex;
}

unsigned int BL_Texture::GetTextureType()
{
	return GL_TEXTURE_2D;
}

void BL_Texture::ActivateTexture(int unit)
{
	/* Since GPUTexture can be shared between material textures (MTex),
	 * we should reapply the bindcode in case of VideoTexture owned texture.
	 * Without that every material that use this GPUTexture will then use
	 * the VideoTexture texture, it's not wanted. */
	GPU_texture_set_opengl_bindcode(m_gpuTex, m_bindCode);
	GPU_texture_bind(m_gpuTex, unit);
}

void BL_Texture::DisableTexture()
{
	GPU_texture_unbind(m_gpuTex);
}

// stuff for cvalue related things
std::string BL_Texture::GetName()
{
	return RAS_Texture::GetName();
}

#ifdef WITH_PYTHON

PyTypeObject BL_Texture::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
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
	{ nullptr, nullptr } //Sentinel
};

PyAttributeDef BL_Texture::Attributes[] = {
	KX_PYATTRIBUTE_RW_FUNCTION("diffuseIntensity", BL_Texture, pyattr_get_diffuse_intensity, pyattr_set_diffuse_intensity),
	KX_PYATTRIBUTE_RW_FUNCTION("diffuseFactor", BL_Texture, pyattr_get_diffuse_factor, pyattr_set_diffuse_factor),
	KX_PYATTRIBUTE_RW_FUNCTION("alpha", BL_Texture, pyattr_get_alpha, pyattr_set_alpha),
	KX_PYATTRIBUTE_RW_FUNCTION("specularIntensity", BL_Texture, pyattr_get_specular_intensity, pyattr_set_specular_intensity),
	KX_PYATTRIBUTE_RW_FUNCTION("specularFactor", BL_Texture, pyattr_get_specular_factor, pyattr_set_specular_factor),
	KX_PYATTRIBUTE_RW_FUNCTION("hardness", BL_Texture, pyattr_get_hardness, pyattr_set_hardness),
	KX_PYATTRIBUTE_RW_FUNCTION("emit", BL_Texture, pyattr_get_emit, pyattr_set_emit),
	KX_PYATTRIBUTE_RW_FUNCTION("mirror", BL_Texture, pyattr_get_mirror, pyattr_set_mirror),
	KX_PYATTRIBUTE_RW_FUNCTION("normal", BL_Texture, pyattr_get_normal, pyattr_set_normal),
	KX_PYATTRIBUTE_RW_FUNCTION("bindCode", BL_Texture, pyattr_get_bind_code, pyattr_set_bind_code),
	KX_PYATTRIBUTE_RW_FUNCTION("uvRotation", BL_Texture, pyattr_get_uv_rotation, pyattr_set_uv_rotation),
	KX_PYATTRIBUTE_RW_FUNCTION("uvOffset", BL_Texture, pyattr_get_uv_offset, pyattr_set_uv_offset),
	KX_PYATTRIBUTE_RW_FUNCTION("uvSize", BL_Texture, pyattr_get_uv_size, pyattr_set_uv_size),
	KX_PYATTRIBUTE_NULL    //Sentinel
};

PyObject *BL_Texture::pyattr_get_diffuse_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->difffac);
}

int BL_Texture::pyattr_set_diffuse_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->difffac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_diffuse_factor(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->colfac);
}

int BL_Texture::pyattr_set_diffuse_factor(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->colfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_alpha(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->alphafac);
}

int BL_Texture::pyattr_set_alpha(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->alphafac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_specular_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->specfac);
}

int BL_Texture::pyattr_set_specular_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->specfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_specular_factor(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->colspecfac);
}

int BL_Texture::pyattr_set_specular_factor(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->colspecfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_hardness(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->hardfac);
}

int BL_Texture::pyattr_set_hardness(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->hardfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_emit(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->emitfac);
}

int BL_Texture::pyattr_set_emit(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->emitfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_mirror(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->mirrfac);
}

int BL_Texture::pyattr_set_mirror(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->mirrfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_normal(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->norfac);
}

int BL_Texture::pyattr_set_normal(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->norfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_bind_code(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	self->CheckValidTexture();
	return PyLong_FromLong(self->m_bindCode);
}

int BL_Texture::pyattr_set_bind_code(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	int val = PyLong_AsLong(value);

	if (val < 0 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = int: BL_Texture, expected a unsigned int", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->m_bindCode = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_uv_rotation(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->rot);
}

int BL_Texture::pyattr_set_uv_rotation(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}
	self->GetMTex()->rot = val;
	return PY_SET_ATTR_SUCCESS;
}

#ifdef USE_MATHUTILS

#define MATHUTILS_VEC_CB_TEXTURE_UV_OFFSET_VECTOR 1
#define MATHUTILS_VEC_CB_TEXTURE_UV_SIZE_VECTOR   2

static unsigned char mathutils_bltexture_cb_index = -1; // Index for our callbacks

static int mathutils_bltexture_generic_check(BaseMathObject *bmo)
{
	BL_Texture *self = static_cast<BL_Texture *>BGE_PROXY_REF(bmo->cb_user);
	if (!self) {
		return -1;
	}

	return 0;
}

static int mathutils_bltexture_get(BaseMathObject *bmo, int subtype)
{
	BL_Texture *self = static_cast<BL_Texture *>BGE_PROXY_REF(bmo->cb_user);
	if (!self) {
		return -1;
	}

	switch (subtype) {
		case MATHUTILS_VEC_CB_TEXTURE_UV_OFFSET_VECTOR:
		{
			copy_v3_v3(bmo->data, self->GetMTex()->ofs);
			break;
		}
		case MATHUTILS_VEC_CB_TEXTURE_UV_SIZE_VECTOR:
		{
			copy_v3_v3(bmo->data, self->GetMTex()->size);
			break;
		}
	}

	return 0;
}

static int mathutils_bltexture_set(BaseMathObject *bmo, int subtype)
{
	BL_Texture *self = static_cast<BL_Texture *>BGE_PROXY_REF(bmo->cb_user);
	if (!self) {
		return -1;
	}

	switch (subtype) {
		case MATHUTILS_VEC_CB_TEXTURE_UV_OFFSET_VECTOR:
		{
			copy_v3_v3(self->GetMTex()->ofs, bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_TEXTURE_UV_SIZE_VECTOR:
		{
			copy_v3_v3(self->GetMTex()->size, bmo->data);
			break;
		}
	}

	return 0;
}

static int mathutils_bltexture_get_index(BaseMathObject *bmo, int subtype, int index)
{
	if (mathutils_bltexture_get(bmo, subtype) == -1) {
		return -1;
	}
	return 0;
}

static int mathutils_bltexture_set_index(BaseMathObject *bmo, int subtype, int index)
{
	float f = bmo->data[index];

	if (mathutils_bltexture_get(bmo, subtype) == -1) {
		return -1;
	}

	bmo->data[index] = f;
	return mathutils_bltexture_set(bmo, subtype);
}

static Mathutils_Callback mathutils_bltexture_cb = {
	mathutils_bltexture_generic_check,
	mathutils_bltexture_get,
	mathutils_bltexture_set,
	mathutils_bltexture_get_index,
	mathutils_bltexture_set_index
};


void BL_Texture_Mathutils_Callback_Init()
{
	// Register mathutils callbacks, ok to run more than once.
	mathutils_bltexture_cb_index = Mathutils_RegisterCallback(&mathutils_bltexture_cb);
}

#endif  // USE_MATHUTILS

PyObject *BL_Texture::pyattr_get_uv_offset(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(BGE_PROXY_FROM_REF(self_v), 3, mathutils_bltexture_cb_index, MATHUTILS_VEC_CB_TEXTURE_UV_OFFSET_VECTOR);
#else
	BL_Texture *self = static_cast<BL_Texture *>(self_v);

	return PyObjectFrom(MT_Vector3(self->GetMTex()->ofs));
#endif
}

int BL_Texture::pyattr_set_uv_offset(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	MT_Vector3 offset;
	if (!PyVecTo(value, offset)) {
		return PY_SET_ATTR_FAIL;
	}

	offset.getValue(self->GetMTex()->ofs);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_uv_size(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(BGE_PROXY_FROM_REF(self_v), 3, mathutils_bltexture_cb_index, MATHUTILS_VEC_CB_TEXTURE_UV_SIZE_VECTOR);
#else
	BL_Texture *self = static_cast<BL_Texture *>(self_v);

	return PyObjectFrom(MT_Vector3(self->GetMTex()->size));
#endif
}

int BL_Texture::pyattr_set_uv_size(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	MT_Vector3 size;
	if (!PyVecTo(value, size)) {
		return PY_SET_ATTR_FAIL;
	}

	size.getValue(self->GetMTex()->size);

	return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON
