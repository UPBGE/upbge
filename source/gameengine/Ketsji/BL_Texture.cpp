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
#include "KX_CubeMap.h"

#include "DNA_texture_types.h"

#include "GPU_texture.h"
#include "GPU_draw.h"

BL_Texture::BL_Texture(MTex *mtex)
	:CValue(),
	m_isCubeMap(false),
	m_mtex(mtex),
	m_gpuTex(NULL)
{
	Tex *tex = m_mtex->tex;
	EnvMap *env = tex->env;
	m_isCubeMap = (env && tex->type == TEX_ENVMAP && (env->stype == ENV_LOAD || env->stype == ENV_REALT));

	Image *ima = tex->ima;
	ImageUser& iuser = tex->iuser;
	const int gltextarget = m_isCubeMap ? GetCubeMapTextureType() : GetTexture2DType();

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
	 * The gpu texture in the image can be NULL or an already different loaded
	 * gpu texture. In both cases we call GPU_texture_from_blender.
	 */
	int target = m_isCubeMap ? TEXTARGET_TEXTURE_CUBE_MAP : TEXTARGET_TEXTURE_2D;
	if (m_gpuTex != m_mtex->tex->ima->gputexture[target]) {
		Tex *tex = m_mtex->tex;
		Image *ima = tex->ima;
		ImageUser& iuser = tex->iuser;

		const int gltextarget = m_isCubeMap ? GetCubeMapTextureType() : GetTexture2DType();

		// Restore gpu texture original bind cdoe to make sure we will delete the right opengl texture.
		GPU_texture_set_opengl_bindcode(m_gpuTex, m_savedData.bindcode);
		GPU_texture_free(m_gpuTex);

		m_gpuTex = (ima ? GPU_texture_from_blender(ima, &iuser, gltextarget, false, 0.0, true) : NULL);

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
	return (m_gpuTex != NULL);
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

unsigned int BL_Texture::swapTexture(unsigned int bindcode)
{
	// swap texture codes
	unsigned int tmp = m_bindCode;
	m_bindCode = bindcode;
	// return original texture code
	return tmp;
}

// stuff for cvalue related things
STR_String &BL_Texture::GetName()
{
	return RAS_Texture::GetName();
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
	KX_PYATTRIBUTE_RO_FUNCTION("cubeMap", BL_Texture, pyattr_get_cube_map),
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
	self->CheckValidTexture();
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

PyObject *BL_Texture::pyattr_get_cube_map(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_Texture *self = static_cast<BL_Texture *>(self_v);
	KX_CubeMap *cubeMap = (KX_CubeMap *)self->GetCubeMap();
	if (cubeMap) {
		return cubeMap->GetProxy();
	}

	Py_RETURN_NONE;
}

#endif  // WITH_PYTHON
