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
#include "RAS_Texture.h"
#include "RAS_IRasterizer.h"

#include "GPU_texture.h"
#include "GPU_framebuffer.h"
#include "GPU_draw.h"

#include "BKE_image.h"

#include "DNA_texture_types.h"

#include "glew-mx.h"

static const MT_Matrix3x3 topFaceViewMat(
	1.0f, 0.0f, 0.0f,
	0.0f, -1.0f, 0.0f,
	0.0f, 0.0f, -1.0f);

static const MT_Matrix3x3 bottomFaceViewMat(
	-1.0f, 0.0f, 0.0f,
	0.0f, -1.0f, 0.0f,
	0.0f, 0.0f, 1.0f);

static const MT_Matrix3x3 frontFaceViewMat(
	0.0f, 0.0f, -1.0f,
	0.0f, -1.0f, 0.0f,
	-1.0f, 0.0f, 0.0f);

static const MT_Matrix3x3 backFaceViewMat(
	0.0f, 0.0f, 1.0f,
	0.0f, -1.0f, 0.0f,
	1.0f, 0.0f, 0.0f);

static const MT_Matrix3x3 rightFaceViewMat(
	1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, -1.0f,
	0.0f, 1.0f, 0.0f);

static const MT_Matrix3x3 leftFaceViewMat(
	1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f,
	0.0f, -1.0f, 0.0f);

const MT_Matrix3x3 RAS_CubeMap::faceViewMatrices3x3[RAS_CubeMap::NUM_FACES] = {
	topFaceViewMat,
	bottomFaceViewMat,
	frontFaceViewMat,
	backFaceViewMat,
	rightFaceViewMat,
	leftFaceViewMat
};

static const GLenum cubeMapTargets[RAS_CubeMap::NUM_FACES] = {
	GL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB,
	GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB,
	GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB,
	GL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB,
	GL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB,
	GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB,
};

RAS_CubeMap::RAS_CubeMap()
	:m_gpuTex(nullptr),
	m_useMipmap(false)
{
	for (unsigned short i = 0; i < NUM_FACES; ++i) {
		m_fbos[i] = nullptr;
		m_rbs[i] = nullptr;
	}
}

RAS_CubeMap::~RAS_CubeMap()
{
	DetachTexture();

	/* This call has for side effect to ask regeneration of all textures
	 * depending of this image.
	 */
	for (std::vector<RAS_Texture *>::iterator it = m_textureUsers.begin(), end = m_textureUsers.end(); it != end; ++it) {
		RAS_Texture *texture = *it;
		// Invalidate the cube map in each material texture users.
		texture->SetCubeMap(nullptr);
		/* Use BKE_image_free_buffers to free the bind code and also the cached frames which
		 * freed by image_free_cached_frames.
		 */
		BKE_image_free_buffers(texture->GetImage());
	}
}

void RAS_CubeMap::AttachTexture()
{
	// Increment reference to make sure the gpu texture will not be freed by someone else.
	GPU_texture_ref(m_gpuTex);

	for (unsigned short i = 0; i < NUM_FACES; ++i) {
		m_fbos[i] = GPU_framebuffer_create();
		m_rbs[i] = GPU_renderbuffer_create(GPU_texture_width(m_gpuTex), GPU_texture_height(m_gpuTex),
										   0, GPU_HDR_NONE, GPU_RENDERBUFFER_DEPTH, nullptr);

		GPU_framebuffer_texture_attach_target(m_fbos[i], m_gpuTex, cubeMapTargets[i], 0, nullptr);
		GPU_framebuffer_renderbuffer_attach(m_fbos[i], m_rbs[i], 0, nullptr);
	}
}

void RAS_CubeMap::DetachTexture()
{
	if (!m_gpuTex) {
		return;
	}

	for (unsigned short i = 0; i < NUM_FACES; ++i) {
		if (m_fbos[i]) {
			GPU_framebuffer_texture_detach_target(m_gpuTex, cubeMapTargets[i]);
		}
		if (m_rbs[i]) {
			GPU_framebuffer_renderbuffer_detach(m_rbs[i]);
		}

		if (m_fbos[i]) {
			GPU_framebuffer_free(m_fbos[i]);
			m_fbos[i] = nullptr;
		}
		if (m_rbs[i]) {
			GPU_renderbuffer_free(m_rbs[i]);
			m_rbs[i] = nullptr;
		}
	}

	GPU_texture_free(m_gpuTex);
}

void RAS_CubeMap::GetValidTexture()
{
	BLI_assert(m_textureUsers.size() > 0);

	/* The gpu texture returned by all material textures are the same.
	 * We can so use the first material texture user.
	 */
	RAS_Texture *texture = m_textureUsers[0];
	texture->CheckValidTexture();
	GPUTexture *gputex = texture->GetGPUTexture();

	if (m_gpuTex == gputex) {
		// The gpu texture is the same.
		return;
	}

	DetachTexture();

	m_gpuTex = gputex;

	AttachTexture();

	EnvMap *env = texture->GetMTex()->tex->env;
	m_useMipmap = (env->filtering == ENVMAP_MIPMAP_MIPMAP) && GPU_get_mipmap();

	if (!m_useMipmap) {
		// Disable mipmaping.
		GPU_texture_bind(m_gpuTex, 0);
		GPU_texture_filter_mode(m_gpuTex, false, (env->filtering == ENVMAP_MIPMAP_LINEAR), false);
		GPU_texture_unbind(m_gpuTex);
	}
}

const std::vector<RAS_Texture *>& RAS_CubeMap::GetTextureUsers() const
{
	return m_textureUsers;
}

void RAS_CubeMap::AddTextureUser(RAS_Texture *texture)
{
	m_textureUsers.push_back(texture);
	texture->SetCubeMap(this);
}

void RAS_CubeMap::BeginRender()
{
	GetValidTexture();
}

void RAS_CubeMap::EndRender()
{
	if (m_useMipmap) {
		GPU_texture_bind(m_gpuTex, 0);
		GPU_texture_generate_mipmap(m_gpuTex);
		GPU_texture_unbind(m_gpuTex);
	}
}

void RAS_CubeMap::BindFace(RAS_IRasterizer *rasty, unsigned short index)
{
	GPU_framebuffer_bind_no_save(m_fbos[index], 0);

	rasty->Clear(RAS_IRasterizer::RAS_COLOR_BUFFER_BIT | RAS_IRasterizer::RAS_DEPTH_BUFFER_BIT);
}
