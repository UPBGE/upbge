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

BL_Texture::BL_Texture(MTex *mtex, bool cubemap)
	:CValue(),
	m_cubeMap(cubemap),
	m_mtex(mtex)
{
	Tex *tex = m_mtex->tex;
	Image *ima = tex->ima;
	ImageUser& iuser = tex->iuser;
	const int gltextarget = m_cubeMap ? GetCubeMapTextureType() : GetTexture2DType();

	m_gpuTex = (ima ? GPU_texture_from_blender(ima, &iuser, gltextarget, false, 0.0, true) : NULL);

	// Initialize saved data.
	m_name = STR_String(m_mtex->tex->id.name + 2);
	m_savedData.colintensfac = m_mtex->difffac;
	m_savedData.colfac = m_mtex->colfac;
	m_savedData.alphafac = m_mtex->alphafac;
	m_savedData.specintensfac = m_mtex->specfac;
	m_savedData.speccolorfac = m_mtex->colspecfac;
	m_savedData.hardnessfac = m_mtex->hardfac;
	m_savedData.emitfac = m_mtex->emitfac;
	m_savedData.mirrorfac = m_mtex->mirrfac;
	m_savedData.normalfac = m_mtex->norfac;
	m_savedData.parallaxbumpfac = m_mtex->parallaxbumpsc;
	m_savedData.parallaxstepfac = m_mtex->parallaxsteps;
	m_savedData.lodbias = m_mtex->lodbias;

	if (m_gpuTex) {
		m_bindCode = GPU_texture_opengl_bindcode(m_gpuTex);
		m_savedData.bindcode = m_bindCode;
		GPU_texture_ref(m_gpuTex);
	}
}

BL_Texture::~BL_Texture()
{
	// Restore saved data.
	m_mtex->difffac = m_savedData.colintensfac;
	m_mtex->colfac = m_savedData.colfac;
	m_mtex->alphafac = m_savedData.alphafac;
	m_mtex->specfac = m_savedData.specintensfac;
	m_mtex->colspecfac = m_savedData.speccolorfac;
	m_mtex->hardfac = m_savedData.hardnessfac;
	m_mtex->emitfac = m_savedData.emitfac;
	m_mtex->mirrfac = m_savedData.mirrorfac;
	m_mtex->norfac = m_savedData.normalfac;
	m_mtex->parallaxbumpsc = m_savedData.parallaxbumpfac;
	m_mtex->parallaxsteps = m_savedData.parallaxstepfac;
	m_mtex->lodbias = m_savedData.lodbias;

	GPUTexture *gputex = GetGPUTexture();

	if (gputex) {
		GPU_texture_set_opengl_bindcode(gputex, m_savedData.bindcode);
		GPU_texture_free(m_gpuTex);
	}
}

GPUTexture *BL_Texture::GetGPUTexture()
{
	/* Test if the texture is owned only by us, if it's the case then it means
	 * that no materials use it anymore and that we have to get a pointer of 
	 * the updated gpu texture used by materials.
	 */
	if (m_gpuTex && GPU_texture_ref_count(m_gpuTex) == 1) {
		Tex *tex = m_mtex->tex;
		Image *ima = tex->ima;
		ImageUser& iuser = tex->iuser;

		const int gltextarget = m_cubeMap ? GetCubeMapTextureType() : GetTexture2DType();

		GPU_free_image(ima);

		m_gpuTex = (ima ? GPU_texture_from_blender(ima, &iuser, gltextarget, false, 0.0, true) : NULL);

		if (m_gpuTex) {
			m_savedData.bindcode = GPU_texture_opengl_bindcode(m_gpuTex);
			GPU_texture_ref(m_gpuTex);
		}
	}
	return m_gpuTex;
}

bool BL_Texture::Ok()
{
	GPUTexture *gputex = GetGPUTexture();
	return (gputex != NULL);
}

MTex *BL_Texture::GetMTex()
{
	return m_mtex;
}

Image *BL_Texture::GetImage()
{
	return m_mtex->tex->ima;
}

unsigned int BL_Texture::GetTextureType()
{
	GPUTexture *gputex = GetGPUTexture();
	return GPU_texture_target(gputex);
}

void BL_Texture::ActivateTexture(int unit)
{
	/* Since GPUTexture can be shared between material textures (MTex),
	 * we should reapply the bindcode in case of VideoTexture owned texture.
	 * Without that every material that use this GPUTexture will then use
	 * the VideoTexture texture, it's not wanted. */
	GPUTexture *gputex = GetGPUTexture();
	GPU_texture_set_opengl_bindcode(gputex, m_bindCode);
	GPU_texture_bind(gputex, unit);
}

void BL_Texture::DisableTexture()
{
	GPUTexture *gputex = GetGPUTexture();
	GPU_texture_unbind(gputex);
}

unsigned int BL_Texture::swapTexture(unsigned int bindcode)
{
	// swap texture codes
	unsigned int tmp = m_bindCode;
	m_bindCode = bindcode;
	// return original texture code
	return tmp;
}

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
	return GetName();
}

void BL_Texture::SetName(const char *name)
{
}

CValue *BL_Texture::GetReplica()
{
	return NULL;
}

#ifdef WITH_PYTHON

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
	{ NULL, NULL } //Sentinel
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
	KX_PYATTRIBUTE_RW_FUNCTION("parallaxBump", BL_Texture, pyattr_get_parallax_bump, pyattr_set_parallax_bump),
	KX_PYATTRIBUTE_RW_FUNCTION("parallaxStep", BL_Texture, pyattr_get_parallax_step, pyattr_set_parallax_step),
	KX_PYATTRIBUTE_RW_FUNCTION("lodBias", BL_Texture, pyattr_get_lod_bias, pyattr_set_lod_bias),
	KX_PYATTRIBUTE_RW_FUNCTION("bindCode", BL_Texture, pyattr_get_bind_code, pyattr_set_bind_code),
	{ NULL }    //Sentinel
};

PyObject *BL_Texture::pyattr_get_diffuse_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->difffac);
}

int BL_Texture::pyattr_set_diffuse_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->difffac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_diffuse_factor(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->colfac);
}

int BL_Texture::pyattr_set_diffuse_factor(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->colfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->alphafac);
}

int BL_Texture::pyattr_set_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->alphafac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_specular_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->specfac);
}

int BL_Texture::pyattr_set_specular_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->specfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_specular_factor(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->colspecfac);
}

int BL_Texture::pyattr_set_specular_factor(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->colspecfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_hardness(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->hardfac);
}

int BL_Texture::pyattr_set_hardness(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->hardfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_emit(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->emitfac);
}

int BL_Texture::pyattr_set_emit(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->emitfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_mirror(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->mirrfac);
}

int BL_Texture::pyattr_set_mirror(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->mirrfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_normal(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->norfac);
}

int BL_Texture::pyattr_set_normal(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->norfac = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_parallax_bump(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->parallaxbumpsc);
}

int BL_Texture::pyattr_set_parallax_bump(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->parallaxbumpsc = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_parallax_step(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->parallaxsteps);
}

int BL_Texture::pyattr_set_parallax_step(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->parallaxsteps = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_lod_bias(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->lodbias);
}

int BL_Texture::pyattr_set_lod_bias(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->lodbias = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_bind_code(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyLong_FromLong(self->m_bindCode);
}

int BL_Texture::pyattr_set_bind_code(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	int val = PyLong_AsLong(value);

	if (val < 0 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = int: BL_Texture, expected a unsigned int", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	self->m_bindCode = val;
	return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON
