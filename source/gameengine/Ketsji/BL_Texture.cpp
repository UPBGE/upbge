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

#include <iostream>
#include <map>
#include <stdlib.h>

#include "BL_Material.h"
#include "BL_Texture.h"
#include "MT_Matrix4x4.h"
#include "BLI_utildefines.h"

#include "DNA_texture_types.h"
#include "DNA_image_types.h"
#include "IMB_imbuf_types.h"
#include "BKE_image.h"
#include "BLI_blenlib.h"

#include "RAS_ICanvas.h"
#include "RAS_Rect.h"

#include "KX_GameObject.h"

#include "MEM_guardedalloc.h"
#include "GPU_draw.h"
#include "GPU_extensions.h"
#include "GPU_texture.h"

BL_Texture::BL_Texture()
	:m_type(0),
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
		m_savedData.bindcode = GPU_texture_opengl_bindcode(m_gputex);
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

void BL_Texture::ActivateFirst()
{
	if (GLEW_ARB_multitexture)
		glActiveTextureARB(GL_TEXTURE0_ARB);
}

void BL_Texture::DisableAllTextures()
{
	for (int i = 0; i < MAXTEX; i++) {
		if (GLEW_ARB_multitexture)
			glActiveTextureARB(GL_TEXTURE0_ARB + i);

		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();
		glMatrixMode(GL_MODELVIEW);
		glDisable(GL_TEXTURE_2D);
		glDisable(GL_TEXTURE_GEN_S);
		glDisable(GL_TEXTURE_GEN_T);
		glDisable(GL_TEXTURE_GEN_R);
		glDisable(GL_TEXTURE_GEN_Q);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	}

	if (GLEW_ARB_multitexture)
		glActiveTextureARB(GL_TEXTURE0_ARB);
}

void BL_Texture::ActivateTexture(int unit)
{
	GPU_texture_bind(m_gputex, unit);
}

unsigned int BL_Texture::swapTexture(unsigned int bindcode)
{
	// swap texture codes
	unsigned int tmp = GPU_texture_opengl_bindcode(m_gputex);
	GPU_texture_set_opengl_bindcode(m_gputex, bindcode);
	// return original texture code
	return tmp;
}
