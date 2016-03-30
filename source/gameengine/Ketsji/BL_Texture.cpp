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

#include "GPU_texture.h"

BL_Texture::BL_Texture()
	:m_type(0),
	m_bindcode(0),
	m_mtex(NULL),
	m_gputex(NULL)
{
}

BL_Texture::~BL_Texture()
{
	// Restore saved data.
	if (m_gputex) {
		GPU_texture_set_opengl_bindcode(m_gputex, m_savedData.bindcode);
	}
}

void BL_Texture::Init(MTex *mtex, bool cubemap, bool mipmap)
{
	Tex *tex = mtex->tex;
	Image *ima = tex->ima;
	ImageUser& iuser = tex->iuser;
	const int gltextarget = cubemap ? GL_TEXTURE_CUBE_MAP_ARB : GL_TEXTURE_2D;

	m_gputex = GPU_texture_from_blender(ima, &iuser, gltextarget, false, 0.0, mipmap);

	m_type = gltextarget;
	m_mtex = mtex;

	// Initialize saved data.
	if (m_gputex) {
		m_bindcode = GPU_texture_opengl_bindcode(m_gputex);
		m_savedData.bindcode = m_bindcode;
	}
}

bool BL_Texture::Ok()
{
	return (m_gputex != NULL);
}

unsigned int BL_Texture::GetTextureType() const
{
	return m_type;
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
