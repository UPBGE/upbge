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
* Contributor(s): Ulysse Martin, Tristan Porteries.
*
* ***** END GPL LICENSE BLOCK *****
*/

/** \file RAS_CubeMap.cpp
*  \ingroup bgerast
*/

#include "RAS_CubeMap.h"
#include "RAS_CubeMapManager.h"
#include "RAS_Texture.h"
#include "RAS_IRasterizer.h"

#include "GPU_texture.h"
#include "GPU_framebuffer.h"
#include "GPU_draw.h"

#include "DNA_texture_types.h"

#include "glew-mx.h"

RAS_CubeMap::RAS_CubeMap(void *clientobj, RAS_Texture *texture, RAS_IRasterizer *rasty)
	:m_texture(texture),
	m_cubeMapTexture(NULL),
	m_clientobj(clientobj)
{
	m_fbo = GPU_framebuffer_create();

	MTex *mtex = m_texture->GetMTex();
	float clipend = mtex->tex->env->clipend;
	m_layer = mtex->tex->env->notlay;

	m_cubeMapTexture = m_texture->GetGPUTexture();
	// Increment reference to make sure the gpu texture will not be freed by someone else.
	GPU_texture_ref(m_cubeMapTexture);
	// Disable mipmaping.
	GPU_texture_bind(m_cubeMapTexture, 0);
	GPU_texture_filter_mode(m_cubeMapTexture, false, false);
	GPU_texture_unbind(m_cubeMapTexture);

	m_proj = rasty->GetFrustumMatrix(-0.001f, 0.001f, -0.001f, 0.001f, 0.001f, clipend, 1.0f);
}

RAS_CubeMap::~RAS_CubeMap()
{
	GPU_texture_free(m_cubeMapTexture);
	GPU_free_image(m_texture->GetImage());
	GPU_framebuffer_free(m_fbo);
}

void *RAS_CubeMap::GetClientObject()
{
	return m_clientobj;
}

const MT_Matrix4x4& RAS_CubeMap::GetProjection()
{
	return m_proj;
}

short RAS_CubeMap::GetLayer()
{
	return m_layer;
}

void RAS_CubeMap::BeginRender()
{
}

void RAS_CubeMap::EndRender()
{
}

void RAS_CubeMap::BindFace(RAS_IRasterizer *rasty, unsigned short index, const MT_Vector3& objpos)
{
	static const GLenum m_cube_map_target[6] = {
		GL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB,
		GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB,
		GL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB,
	};

	GPU_framebuffer_texture_attach_target(m_fbo, m_cubeMapTexture, m_cube_map_target[index], 0, NULL);
	GPU_texture_bind_as_framebuffer(m_cubeMapTexture);

	rasty->Clear(RAS_IRasterizer::RAS_COLOR_BUFFER_BIT);

	const MT_Matrix4x4 posmat = MT_Matrix4x4(1.0f, 0.0f, 0.0f, -objpos[0],
									   0.0f, 1.0f, 0.0f, -objpos[1],
									   0.0f, 0.0f, 1.0f, -objpos[2],
									   0.0f, 0.0f, 0.0f, 1.0f);
	const MT_Matrix4x4 viewmat = RAS_CubeMapManager::facesViewMat[index] * posmat;
	const MT_Matrix3x3& rot = RAS_CubeMapManager::camOri[index];

	rasty->SetViewMatrix(viewmat, rot, objpos, MT_Vector3(1.0f, 1.0f, 1.0f), true);
}

void RAS_CubeMap::UnbindFace()
{
	GPU_framebuffer_texture_detach(m_cubeMapTexture);
	GPU_framebuffer_texture_unbind(m_fbo, m_cubeMapTexture);
}