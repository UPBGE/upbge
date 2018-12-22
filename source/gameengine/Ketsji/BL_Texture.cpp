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
#include "KX_TextureRenderer.h"

#include "DNA_texture_types.h"

#include "GPU_texture.h"
#include "GPU_draw.h"

#include "KX_PyMath.h"

#include "BLI_math.h"

BL_Texture::BL_Texture(MTex *mtex)
	:EXP_Value(),
	m_isCubeMap(false),
	m_mtex(mtex),
	m_gpuTex(nullptr),
	m_imaTarget(0)
{
	Tex *tex = m_mtex->tex;
	EnvMap *env = tex->env;
	m_isCubeMap = (env && tex->type == TEX_ENVMAP &&
	               (env->stype == ENV_LOAD ||
	                (env->stype == ENV_REALT && env->type == ENV_CUBE)));

	m_imaTarget = m_isCubeMap ? TEXTARGET_TEXTURE_CUBE_MAP : TEXTARGET_TEXTURE_2D;
	m_target = m_isCubeMap ? GetCubeMapTextureType() : GetTexture2DType();

	Image *ima = tex->ima;
	ImageUser& iuser = tex->iuser;

	m_gpuTex = (ima ? GPU_texture_from_blender(ima, &iuser, m_target, false, 0.0, true) : nullptr);

	// Initialize saved data.
	m_name = std::string(m_mtex->tex->id.name + 2);
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
	m_savedData.ior = m_mtex->ior;
	m_savedData.ratio = m_mtex->refrratio;
	m_savedData.uvrot = m_mtex->rot;
	copy_v3_v3(m_savedData.uvoffset, m_mtex->ofs);
	copy_v3_v3(m_savedData.uvsize, m_mtex->size);

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
	m_mtex->ior = m_savedData.ior;
	m_mtex->refrratio = m_savedData.ratio;
	m_mtex->rot = m_savedData.uvrot;
	copy_v3_v3(m_mtex->ofs, m_savedData.uvoffset);
	copy_v3_v3(m_mtex->size, m_savedData.uvsize);

	if (m_gpuTex) {
		SetGPUBindCode(m_savedData.bindcode);
		GPU_texture_free(m_gpuTex);
	}
}

void BL_Texture::SetGPUBindCode(int bindCode)
{
	GPU_texture_set_opengl_bindcode(m_gpuTex, bindCode);
	m_mtex->tex->ima->bindcode[m_imaTarget] = bindCode;
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
	if (m_gpuTex != m_mtex->tex->ima->gputexture[m_imaTarget]) {
		Tex *tex = m_mtex->tex;
		Image *ima = tex->ima;
		ImageUser& iuser = tex->iuser;

		// Restore gpu texture original bind code to make sure we will delete the right opengl texture.
		GPU_texture_set_opengl_bindcode(m_gpuTex, m_savedData.bindcode);
		GPU_texture_free(m_gpuTex);

		m_gpuTex = (ima ? GPU_texture_from_blender(ima, &iuser, m_target, false, 0.0, true) : nullptr);

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
	return m_mtex;
}

Tex *BL_Texture::GetTex() const
{
	return m_mtex->tex;
}

Image *BL_Texture::GetImage() const
{
	return m_mtex->tex->ima;
}

GPUTexture *BL_Texture::GetGPUTexture() const
{
	return m_gpuTex;
}

unsigned int BL_Texture::GetTextureType()
{
	return GPU_texture_target(m_gpuTex);
}

void BL_Texture::UpdateBindCode()
{
	/* Since GPUTexture can be shared (as Image is shared along materials)
	 * between material textures (MTex), we should reapply the bindcode in
	 * case of VideoTexture owned texture. Without that every material that
	 * use this GPUTexture will then use the VideoTexture texture, it's not wanted. */
	SetGPUBindCode(m_bindCode);
}

void BL_Texture::ActivateTexture(int unit)
{
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

PyMethodDef BL_Texture::Methods[] = {
	{ nullptr, nullptr } //Sentinel
};

PyAttributeDef BL_Texture::Attributes[] = {
	EXP_PYATTRIBUTE_RW_FUNCTION("diffuseIntensity", BL_Texture, pyattr_get_diffuse_intensity, pyattr_set_diffuse_intensity),
	EXP_PYATTRIBUTE_RW_FUNCTION("diffuseFactor", BL_Texture, pyattr_get_diffuse_factor, pyattr_set_diffuse_factor),
	EXP_PYATTRIBUTE_RW_FUNCTION("alpha", BL_Texture, pyattr_get_alpha, pyattr_set_alpha),
	EXP_PYATTRIBUTE_RW_FUNCTION("specularIntensity", BL_Texture, pyattr_get_specular_intensity, pyattr_set_specular_intensity),
	EXP_PYATTRIBUTE_RW_FUNCTION("specularFactor", BL_Texture, pyattr_get_specular_factor, pyattr_set_specular_factor),
	EXP_PYATTRIBUTE_RW_FUNCTION("hardness", BL_Texture, pyattr_get_hardness, pyattr_set_hardness),
	EXP_PYATTRIBUTE_RW_FUNCTION("emit", BL_Texture, pyattr_get_emit, pyattr_set_emit),
	EXP_PYATTRIBUTE_RW_FUNCTION("mirror", BL_Texture, pyattr_get_mirror, pyattr_set_mirror),
	EXP_PYATTRIBUTE_RW_FUNCTION("normal", BL_Texture, pyattr_get_normal, pyattr_set_normal),
	EXP_PYATTRIBUTE_RW_FUNCTION("parallaxBump", BL_Texture, pyattr_get_parallax_bump, pyattr_set_parallax_bump),
	EXP_PYATTRIBUTE_RW_FUNCTION("parallaxStep", BL_Texture, pyattr_get_parallax_step, pyattr_set_parallax_step),
	EXP_PYATTRIBUTE_RW_FUNCTION("lodBias", BL_Texture, pyattr_get_lod_bias, pyattr_set_lod_bias),
	EXP_PYATTRIBUTE_RW_FUNCTION("bindCode", BL_Texture, pyattr_get_bind_code, pyattr_set_bind_code),
	EXP_PYATTRIBUTE_RO_FUNCTION("renderer", BL_Texture, pyattr_get_renderer),
	EXP_PYATTRIBUTE_RW_FUNCTION("ior", BL_Texture, pyattr_get_ior, pyattr_set_ior),
	EXP_PYATTRIBUTE_RW_FUNCTION("refractionRatio", BL_Texture, pyattr_get_refraction_ratio, pyattr_set_refraction_ratio),
	EXP_PYATTRIBUTE_RW_FUNCTION("uvRotation", BL_Texture, pyattr_get_uv_rotation, pyattr_set_uv_rotation),
	EXP_PYATTRIBUTE_RW_FUNCTION("uvOffset", BL_Texture, pyattr_get_uv_offset, pyattr_set_uv_offset),
	EXP_PYATTRIBUTE_RW_FUNCTION("uvSize", BL_Texture, pyattr_get_uv_size, pyattr_set_uv_size),
	EXP_PYATTRIBUTE_NULL    //Sentinel
};

PyObject *BL_Texture::pyattr_get_diffuse_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->difffac);
}

int BL_Texture::pyattr_set_diffuse_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
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

PyObject *BL_Texture::pyattr_get_diffuse_factor(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->colfac);
}

int BL_Texture::pyattr_set_diffuse_factor(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
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

PyObject *BL_Texture::pyattr_get_alpha(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->alphafac);
}

int BL_Texture::pyattr_set_alpha(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
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

PyObject *BL_Texture::pyattr_get_specular_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->specfac);
}

int BL_Texture::pyattr_set_specular_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
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

PyObject *BL_Texture::pyattr_get_specular_factor(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->colspecfac);
}

int BL_Texture::pyattr_set_specular_factor(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
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

PyObject *BL_Texture::pyattr_get_hardness(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->hardfac);
}

int BL_Texture::pyattr_set_hardness(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
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

PyObject *BL_Texture::pyattr_get_emit(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->emitfac);
}

int BL_Texture::pyattr_set_emit(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
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

PyObject *BL_Texture::pyattr_get_mirror(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->mirrfac);
}

int BL_Texture::pyattr_set_mirror(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
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

PyObject *BL_Texture::pyattr_get_normal(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->norfac);
}

int BL_Texture::pyattr_set_normal(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
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

PyObject *BL_Texture::pyattr_get_parallax_bump(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->parallaxbumpsc);
}

int BL_Texture::pyattr_set_parallax_bump(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->parallaxbumpsc = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_parallax_step(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->parallaxsteps);
}

int BL_Texture::pyattr_set_parallax_step(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->parallaxsteps = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_lod_bias(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->lodbias);
}

int BL_Texture::pyattr_set_lod_bias(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->GetMTex()->lodbias = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_bind_code(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	self->CheckValidTexture();
	return PyLong_FromLong(self->m_bindCode);
}

int BL_Texture::pyattr_set_bind_code(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
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

PyObject *BL_Texture::pyattr_get_renderer(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	KX_TextureRenderer *renderer = static_cast<KX_TextureRenderer *>(self->GetRenderer());
	if (renderer) {
		return renderer->GetProxy();
	}

	Py_RETURN_NONE;
}

PyObject *BL_Texture::pyattr_get_ior(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->ior);
}

int BL_Texture::pyattr_set_ior(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 1.0, 50.0);
	self->GetMTex()->ior = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_refraction_ratio(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->refrratio);
}

int BL_Texture::pyattr_set_refraction_ratio(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "texture.%s = float: BL_Texture, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}
	CLAMP(val, 0.0, 1.0);
	self->GetMTex()->refrratio = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_uv_rotation(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	return PyFloat_FromDouble(self->GetMTex()->rot);
}

int BL_Texture::pyattr_set_uv_rotation(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
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
	BL_Texture *self = static_cast<BL_Texture *>EXP_PROXY_REF(bmo->cb_user);
	if (!self) {
		return -1;
	}

	return 0;
}

static int mathutils_bltexture_get(BaseMathObject *bmo, int subtype)
{
	BL_Texture *self = static_cast<BL_Texture *>EXP_PROXY_REF(bmo->cb_user);
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
	BL_Texture *self = static_cast<BL_Texture *>EXP_PROXY_REF(bmo->cb_user);
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

PyObject *BL_Texture::pyattr_get_uv_offset(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF(self_v), 3, mathutils_bltexture_cb_index, MATHUTILS_VEC_CB_TEXTURE_UV_OFFSET_VECTOR);
#else
	BL_Texture *self = static_cast<BL_Texture *>(self_v);

	return PyObjectFrom(self->GetMTex()->ofs);
#endif
}

int BL_Texture::pyattr_set_uv_offset(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	mt::vec3 offset;
	if (!PyVecTo(value, offset)) {
		return PY_SET_ATTR_FAIL;
	}

	offset.Pack(self->GetMTex()->ofs);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Texture::pyattr_get_uv_size(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF(self_v), 3, mathutils_bltexture_cb_index, MATHUTILS_VEC_CB_TEXTURE_UV_SIZE_VECTOR);
#else
	BL_Texture *self = static_cast<BL_Texture *>(self_v);

	return PyObjectFrom(self->GetMTex()->size);
#endif
}

int BL_Texture::pyattr_set_uv_size(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	mt::vec3 size;
	if (!PyVecTo(value, size)) {
		return PY_SET_ATTR_FAIL;
	}

	size.Pack(self->GetMTex()->size);

	return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON
