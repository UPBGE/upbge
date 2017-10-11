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

/** \file BL_Texture.h
 *  \ingroup ketsji
 */

#ifndef __BL_TEXTURE_H__
#define __BL_TEXTURE_H__

#include "RAS_Texture.h"
#include "EXP_Value.h"

class KX_TextureRenderer;

class BL_Texture : public EXP_Value, public RAS_Texture
{
	Py_Header(BL_Texture)
private:
	bool m_isCubeMap;
	MTex *m_mtex;
	GPUTexture *m_gpuTex;

	struct {
		unsigned int bindcode;
		float colintensfac;
		float colfac;
		float alphafac;
		float specintensfac;
		float speccolorfac;
		float hardnessfac;
		float emitfac;
		float mirrorfac;
		float normalfac;
		float parallaxbumpfac;
		float parallaxstepfac;
		float lodbias;
		float ior;
		float ratio;
		float uvrot;
		float uvoffset[3];
		float uvsize[3];
	} m_savedData;

public:
	BL_Texture(MTex *mtex);
	virtual ~BL_Texture();

	// stuff for cvalue related things
	virtual std::string GetName() const;

	virtual bool Ok() const;
	virtual bool IsCubeMap() const;

	virtual MTex *GetMTex() const;
	virtual Tex *GetTex() const;
	virtual Image *GetImage() const;
	virtual GPUTexture *GetGPUTexture() const;

	virtual unsigned int GetTextureType();

	enum {MaxUnits = 8};

	virtual void CheckValidTexture();
	virtual void ActivateTexture(int unit);
	virtual void DisableTexture();

#ifdef WITH_PYTHON

	float pyattr_get_diffuse_intensity();
	void pyattr_set_diffuse_intensity(float value);
	float pyattr_get_diffuse_factor();
	void pyattr_set_diffuse_factor(float value);
	float pyattr_get_alpha();
	void pyattr_set_alpha(float value);
	float pyattr_get_specular_intensity();
	void pyattr_set_specular_intensity(float value);
	float pyattr_get_specular_factor();
	void pyattr_set_specular_factor(float value);
	float pyattr_get_hardness();
	void pyattr_set_hardness(float value);
	float pyattr_get_emit();
	void pyattr_set_emit(float value);
	float pyattr_get_mirror();
	void pyattr_set_mirror(float value);
	float pyattr_get_normal();
	void pyattr_set_normal(float value);
	float pyattr_get_parallax_bump();
	void pyattr_set_parallax_bump(float value);
	float pyattr_get_parallax_step();
	void pyattr_set_parallax_step(float value);
	float pyattr_get_lod_bias();
	void pyattr_set_lod_bias(float value);
	int pyattr_get_bind_code();
	void pyattr_set_bind_code(int value);
	KX_TextureRenderer *pyattr_get_renderer();
	float pyattr_get_ior();
	void pyattr_set_ior(float value);
	float pyattr_get_refraction_ratio();
	void pyattr_set_refraction_ratio(float value);
	float pyattr_get_uv_rotation();
	void pyattr_set_uv_rotation(float value);
	mt::vec3 pyattr_get_uv_offset();
	void pyattr_set_uv_offset(const mt::vec3& value);
	mt::vec3 pyattr_get_uv_size();
	void pyattr_set_uv_size(const mt::vec3& value);

#endif  // WITH_PYTHON
};

#endif // __BL_TEXTURE_H__
