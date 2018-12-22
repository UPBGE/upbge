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
 * Contributor(s): Ulysse Martin, Tristan Porteries, Martins Upitis.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_TextureRenderer.cpp
 *  \ingroup bgerast
 */

#include "RAS_TextureRenderer.h"
#include "RAS_Texture.h"
#include "RAS_Rasterizer.h"

#include "GPU_texture.h"
#include "GPU_framebuffer.h"
#include "GPU_draw.h"

#include "CM_List.h"

#include "BKE_image.h"

#include "BLI_utildefines.h"

#include "DNA_texture_types.h"

RAS_TextureRenderer::Face::Face(int target)
	:m_fbo(nullptr),
	m_rb(nullptr),
	m_target(target)
{
}

RAS_TextureRenderer::Face::~Face()
{
}

void RAS_TextureRenderer::Face::AttachTexture(GPUTexture *tex)
{
	m_fbo = GPU_framebuffer_create();
	m_rb = GPU_renderbuffer_create(GPU_texture_width(tex), GPU_texture_height(tex),
	                               0, GPU_HDR_NONE, GPU_RENDERBUFFER_DEPTH, nullptr);

	GPU_framebuffer_texture_attach_target(m_fbo, tex, m_target, 0, nullptr);
	GPU_framebuffer_renderbuffer_attach(m_fbo, m_rb, 0, nullptr);
}

void RAS_TextureRenderer::Face::DetachTexture(GPUTexture *tex)
{
	if (m_fbo) {
		GPU_framebuffer_texture_detach_target(tex, m_target);
	}
	if (m_rb) {
		GPU_framebuffer_renderbuffer_detach(m_rb);
	}

	if (m_fbo) {
		GPU_framebuffer_free(m_fbo);
		m_fbo = nullptr;
	}
	if (m_rb) {
		GPU_renderbuffer_free(m_rb);
		m_rb = nullptr;
	}
}

void RAS_TextureRenderer::Face::Bind() const
{
	// Set the viewport in the same time.
	GPU_framebuffer_bind_no_save(m_fbo, 0);
}

RAS_TextureRenderer::Layer::Layer(const std::vector<int>& attachmentTargets, int textureTarget, Image *ima,
		bool mipmap, bool linear)
{
	for (int target : attachmentTargets) {
		m_faces.emplace_back(target);
	}

	GPU_create_gl_tex(&m_bindCode, nullptr, nullptr, ima->gen_x, ima->gen_y, textureTarget, mipmap, false, ima);
	m_gpuTex = GPU_texture_from_bindcode(textureTarget, m_bindCode);

	if (!linear && !mipmap) {
		// Disable filtering.
		GPU_texture_bind(m_gpuTex, 0);
		GPU_texture_filter_mode(m_gpuTex, false, false, false);
		GPU_texture_unbind(m_gpuTex);
	}

	for (Face& face : m_faces) {
		face.AttachTexture(m_gpuTex);
	}
}

RAS_TextureRenderer::Layer::~Layer()
{
	for (Face& face : m_faces) {
		face.DetachTexture(m_gpuTex);
	}

	if (m_gpuTex) {
		GPU_texture_free(m_gpuTex);
	}
}

RAS_TextureRenderer::Layer::Layer(RAS_TextureRenderer::Layer&& other)
	:m_faces(std::move(other.m_faces)),
	m_gpuTex(other.m_gpuTex),
	m_bindCode(other.m_bindCode)
{
	other.m_gpuTex = nullptr;
}

RAS_TextureRenderer::Layer& RAS_TextureRenderer::Layer::operator=(RAS_TextureRenderer::Layer&& other)
{
	m_faces = std::move(other.m_faces);
	m_gpuTex = other.m_gpuTex;
	other.m_gpuTex = nullptr;
	m_bindCode = other.m_bindCode;

	return *this;
}

unsigned short RAS_TextureRenderer::Layer::GetNumFaces() const
{
	return m_faces.size();
}

unsigned int RAS_TextureRenderer::Layer::GetBindCode() const
{
	return m_bindCode;
}

void RAS_TextureRenderer::Layer::BindFace(unsigned short index) const
{
	m_faces[index].Bind();
}

void RAS_TextureRenderer::Layer::Bind() const
{
}

void RAS_TextureRenderer::Layer::Unbind(bool mipmap) const
{
	if (mipmap) {
		GPU_texture_bind(m_gpuTex, 0);
		GPU_texture_generate_mipmap(m_gpuTex);
		GPU_texture_unbind(m_gpuTex);
	}
}

RAS_TextureRenderer::RAS_TextureRenderer(bool mipmap, bool linear, LayerUsage layerUsage)
	:m_useMipmap(mipmap),
	m_useLinear(linear),
	m_layerUsage(layerUsage)
{
}

RAS_TextureRenderer::~RAS_TextureRenderer()
{
}

unsigned short RAS_TextureRenderer::GetNumFaces(unsigned short layer) const
{
	return m_layers[layer].GetNumFaces();
}

unsigned int RAS_TextureRenderer::GetBindCode(unsigned short index)
{
	/// If the layer is shared, use the unique first layer.
	return m_layers[m_layerUsage * index].GetBindCode();
}

void RAS_TextureRenderer::BeginRender(RAS_Rasterizer *rasty, unsigned short layer)
{
	m_layers[layer].Bind();
}

void RAS_TextureRenderer::EndRender(RAS_Rasterizer *rasty, unsigned short layer)
{
	m_layers[layer].Unbind(m_useMipmap);
}

void RAS_TextureRenderer::BeginRenderFace(RAS_Rasterizer *rasty, unsigned short layer, unsigned short face)
{
	m_layers[layer].BindFace(face);
	// Clear only the depth texture because the background render will override the color texture.
	rasty->Clear(RAS_Rasterizer::RAS_DEPTH_BUFFER_BIT);
}

void RAS_TextureRenderer::ReloadTexture()
{
	// Destruct all layers to force the recreation of the textures.
	m_layers.clear();
}

