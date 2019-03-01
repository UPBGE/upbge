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

#include "KX_PyMath.h"

#include "BLI_math.h"

extern "C" {
#  include "DNA_texture_types.h"
#  include "GPU_draw.h"
#  include "GPU_texture.h"
#  include "gpu/intern/gpu_codegen.h"
}

BL_Texture::BL_Texture(GPUInput *input)
	:CValue(),
	m_isCubeMap(false),
	m_gpuTex(nullptr),
	m_input(input)
{
	/* Normally input->textype is Kept in sync with GPU_DATATYPE_STR */
	m_isCubeMap = (input->textype == GPU_TEXCUBE);
	m_name = input->ima->id.name;

	m_gpuTex = GPU_texture_from_blender(input->ima, input->iuser, m_isCubeMap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, input->image_isdata);

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
	return m_isCubeMap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
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
	KX_PYATTRIBUTE_RW_FUNCTION("bindCode", BL_Texture, pyattr_get_bind_code, pyattr_set_bind_code),
	KX_PYATTRIBUTE_NULL    //Sentinel
};

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

#endif  // WITH_PYTHON
