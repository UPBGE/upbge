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
	m_gpuTex(nullptr)
{
	Tex *tex = m_mtex->tex;
	EnvMap *env = tex->env;
	m_isCubeMap = (env && tex->type == TEX_ENVMAP &&
	               (env->stype == ENV_LOAD ||
	                (env->stype == ENV_REALT && env->type == ENV_CUBE)));

	Image *ima = tex->ima;
	ImageUser& iuser = tex->iuser;
	const int gltextarget = m_isCubeMap ? GetCubeMapTextureType() : GetTexture2DType();

	m_gpuTex = (ima ? GPU_texture_from_blender(ima, &iuser, gltextarget, false, 0.0, true) : nullptr);

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
	if (m_gpuTex != m_mtex->tex->ima->gputexture[target]) {
		Tex *tex = m_mtex->tex;
		Image *ima = tex->ima;
		ImageUser& iuser = tex->iuser;

		const int gltextarget = m_isCubeMap ? GetCubeMapTextureType() : GetTexture2DType();

		// Restore gpu texture original bind cdoe to make sure we will delete the right opengl texture.
		GPU_texture_set_opengl_bindcode(m_gpuTex, m_savedData.bindcode);
		GPU_texture_free(m_gpuTex);

		m_gpuTex = (ima ? GPU_texture_from_blender(ima, &iuser, gltextarget, false, 0.0, true) : nullptr);

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
std::string BL_Texture::GetName() const
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

EXP_Attribute BL_Texture::Attributes[] = {
	EXP_ATTRIBUTE_RW_FUNCTION("diffuseIntensity", pyattr_get_diffuse_intensity, pyattr_set_diffuse_intensity),
	EXP_ATTRIBUTE_RW_FUNCTION("diffuseFactor", pyattr_get_diffuse_factor, pyattr_set_diffuse_factor),
	EXP_ATTRIBUTE_RW_FUNCTION("alpha", pyattr_get_alpha, pyattr_set_alpha),
	EXP_ATTRIBUTE_RW_FUNCTION("specularIntensity", pyattr_get_specular_intensity, pyattr_set_specular_intensity),
	EXP_ATTRIBUTE_RW_FUNCTION("specularFactor", pyattr_get_specular_factor, pyattr_set_specular_factor),
	EXP_ATTRIBUTE_RW_FUNCTION("hardness", pyattr_get_hardness, pyattr_set_hardness),
	EXP_ATTRIBUTE_RW_FUNCTION("emit", pyattr_get_emit, pyattr_set_emit),
	EXP_ATTRIBUTE_RW_FUNCTION("mirror", pyattr_get_mirror, pyattr_set_mirror),
	EXP_ATTRIBUTE_RW_FUNCTION("normal", pyattr_get_normal, pyattr_set_normal),
	EXP_ATTRIBUTE_RW_FUNCTION("parallaxBump", pyattr_get_parallax_bump, pyattr_set_parallax_bump),
	EXP_ATTRIBUTE_RW_FUNCTION("parallaxStep", pyattr_get_parallax_step, pyattr_set_parallax_step),
	EXP_ATTRIBUTE_RW_FUNCTION("lodBias", pyattr_get_lod_bias, pyattr_set_lod_bias),
	EXP_ATTRIBUTE_RW_FUNCTION("bindCode", pyattr_get_bind_code, pyattr_set_bind_code),
	EXP_ATTRIBUTE_RO_FUNCTION("renderer", pyattr_get_renderer),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("ior", pyattr_get_ior, pyattr_set_ior, 1.0f, 50.0f, true),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("refractionRatio", pyattr_get_refraction_ratio, pyattr_set_refraction_ratio, 0.0f, 1.0f, true),
	EXP_ATTRIBUTE_RW_FUNCTION("uvRotation", pyattr_get_uv_rotation, pyattr_set_uv_rotation),
	EXP_ATTRIBUTE_RW_FUNCTION("uvOffset", pyattr_get_uv_offset, pyattr_set_uv_offset),
	EXP_ATTRIBUTE_RW_FUNCTION("uvSize", pyattr_get_uv_size, pyattr_set_uv_size),
	EXP_ATTRIBUTE_NULL    //Sentinel
};

float BL_Texture::pyattr_get_diffuse_intensity()
{
	return m_mtex->difffac;
}

void BL_Texture::pyattr_set_diffuse_intensity(float value)
{
	m_mtex->difffac = value;
}

float BL_Texture::pyattr_get_diffuse_factor()
{
	return m_mtex->colfac;
}

void BL_Texture::pyattr_set_diffuse_factor(float value)
{
	m_mtex->colfac = value;
}

float BL_Texture::pyattr_get_alpha()
{
	return m_mtex->alphafac;
}

void BL_Texture::pyattr_set_alpha(float value)
{
	m_mtex->alphafac = value;
}

float BL_Texture::pyattr_get_specular_intensity()
{
	return m_mtex->specfac;
}

void BL_Texture::pyattr_set_specular_intensity(float value)
{
	m_mtex->specfac = value;
}

float BL_Texture::pyattr_get_specular_factor()
{
	return m_mtex->colspecfac;
}

void BL_Texture::pyattr_set_specular_factor(float value)
{
	m_mtex->colspecfac = value;
}

float BL_Texture::pyattr_get_hardness()
{
	return m_mtex->hardfac;
}

void BL_Texture::pyattr_set_hardness(float value)
{
	m_mtex->hardfac = value;
}

float BL_Texture::pyattr_get_emit()
{
	return m_mtex->emitfac;
}

void BL_Texture::pyattr_set_emit(float value)
{
	m_mtex->emitfac = value;
}

float BL_Texture::pyattr_get_mirror()
{
	return m_mtex->mirrfac;
}

void BL_Texture::pyattr_set_mirror(float value)
{
	m_mtex->mirrfac = value;
}

float BL_Texture::pyattr_get_normal()
{
	return m_mtex->norfac;
}

void BL_Texture::pyattr_set_normal(float value)
{
	m_mtex->norfac = value;
}

float BL_Texture::pyattr_get_parallax_bump()
{
	return m_mtex->parallaxbumpsc;
}

void BL_Texture::pyattr_set_parallax_bump(float value)
{
	m_mtex->parallaxbumpsc = value;
}

float BL_Texture::pyattr_get_parallax_step()
{
	return m_mtex->parallaxsteps;
}

void BL_Texture::pyattr_set_parallax_step(float value)
{
	m_mtex->parallaxsteps = value;
}

float BL_Texture::pyattr_get_lod_bias()
{
	return m_mtex->lodbias;
}

void BL_Texture::pyattr_set_lod_bias(float value)
{
	m_mtex->lodbias = value;
}

int BL_Texture::pyattr_get_bind_code()
{
	CheckValidTexture();
	return m_bindCode;
}

void BL_Texture::pyattr_set_bind_code(int value)
{
	m_bindCode = value;
}

KX_TextureRenderer *BL_Texture::pyattr_get_renderer()
{
	return static_cast<KX_TextureRenderer *>(m_renderer);
}

float BL_Texture::pyattr_get_ior()
{
	return m_mtex->ior;
}

void BL_Texture::pyattr_set_ior(float value)
{
	m_mtex->ior = value;
}

float BL_Texture::pyattr_get_refraction_ratio()
{
	return m_mtex->refrratio;
}

void BL_Texture::pyattr_set_refraction_ratio(float value)
{
	m_mtex->refrratio = value;
}

float BL_Texture::pyattr_get_uv_rotation()
{
	return m_mtex->rot;
}

void BL_Texture::pyattr_set_uv_rotation(float value)
{
	m_mtex->rot = value;
}

mt::vec3 BL_Texture::pyattr_get_uv_offset()
{
	return mt::vec3(m_mtex->ofs);
}

void BL_Texture::pyattr_set_uv_offset(const mt::vec3& value)
{
	value.Pack(m_mtex->ofs);
}

mt::vec3 BL_Texture::pyattr_get_uv_size()
{
	return mt::vec3(m_mtex->size);
}

void BL_Texture::pyattr_set_uv_size(const mt::vec3& value)
{
	value.Pack(m_mtex->size);
}

#endif  // WITH_PYTHON
