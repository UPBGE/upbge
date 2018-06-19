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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_2DFilterOffScreen.cpp
 *  \ingroup bgerast
 */

#include "RAS_2DFilterOffScreen.h"
#include "RAS_ICanvas.h"

#include "GPU_framebuffer.h"
#include "GPU_texture.h"

RAS_2DFilterOffScreen::RAS_2DFilterOffScreen(unsigned short colorSlots, Flag flag, unsigned int width, unsigned int height,
                                             RAS_Rasterizer::HdrType hdr)
	:m_flag(flag),
	m_colorSlots(colorSlots),
	m_hdr(hdr),
	m_width(width),
	m_height(height),
	m_frameBuffer(GPU_framebuffer_create()),
	m_depthTexture(nullptr)
{
	for (unsigned short i = 0; i < NUM_COLOR_SLOTS; ++i) {
		m_colorTextures[i] = nullptr;
	}

	if (!(m_flag & RAS_VIEWPORT_SIZE)) {
		Construct();
	}
}

RAS_2DFilterOffScreen::~RAS_2DFilterOffScreen()
{
	GPU_framebuffer_free(m_frameBuffer);
	for (unsigned short i = 0; i < NUM_COLOR_SLOTS; ++i) {
		GPUTexture *texture = m_colorTextures[i];
		if (texture) {
			GPU_texture_free(texture);
		}
	}
	if (m_depthTexture) {
		GPU_texture_free(m_depthTexture);
	}
}

void RAS_2DFilterOffScreen::Construct()
{
	for (unsigned short i = 0; i < m_colorSlots; ++i) {
		GPUTexture *texture = m_colorTextures[i];
		if (texture) {
			GPU_framebuffer_texture_detach(texture);
			GPU_texture_free(texture);
		}

		// WARNING: Always respect the order from RAS_Rasterizer::HdrType.
		static const GPUHDRType hdrEnums[] = {
			GPU_HDR_NONE, // RAS_HDR_NONE
			GPU_HDR_HALF_FLOAT, // RAS_HDR_HALF_FLOAT
			GPU_HDR_FULL_FLOAT // RAS_HDR_FULL_FLOAT
		};

		texture = GPU_texture_create_2D(m_width, m_height, nullptr, hdrEnums[m_hdr], nullptr);
		if (!GPU_framebuffer_texture_attach(m_frameBuffer, texture, i, nullptr)) {
			GPU_texture_free(texture);
			texture = nullptr;
		}
		m_colorTextures[i] = texture;
	}

	if (m_flag & RAS_DEPTH) {
		if (m_depthTexture) {
			GPU_framebuffer_texture_detach(m_depthTexture);
			GPU_texture_free(m_depthTexture);
		}

		GPUTexture *texture = GPU_texture_create_depth(m_width, m_height, false, nullptr);
		if (!GPU_framebuffer_texture_attach(m_frameBuffer, texture, 0, nullptr)) {
			GPU_texture_free(texture);
			texture = nullptr;
		}
		m_depthTexture = texture;
	}
}

void RAS_2DFilterOffScreen::MipmapTexture()
{
	for (unsigned short i = 0; i < m_colorSlots; ++i) {
		GPUTexture *texture = m_colorTextures[i];
		GPU_texture_bind(texture, 0);
		GPU_texture_filter_mode(texture, false, true, true);
		GPU_texture_generate_mipmap(texture);
		GPU_texture_unbind(texture);
	}
}

bool RAS_2DFilterOffScreen::Update(RAS_ICanvas *canvas)
{
	if (m_flag & RAS_VIEWPORT_SIZE) {
		const unsigned int width = canvas->GetWidth();
		const unsigned int height = canvas->GetHeight();
		if (m_width != width || m_height != height) {
			m_width = width;
			m_height = height;

			Construct();
		}
	}

	return GetValid();
}

void RAS_2DFilterOffScreen::Bind(RAS_Rasterizer *rasty)
{
	GPU_framebuffer_bind_all_attachments(m_frameBuffer, m_colorSlots);

	if (!(m_flag & RAS_VIEWPORT_SIZE)) {
		rasty->SetViewport(0, 0, m_width, m_height);
		rasty->SetScissor(0, 0, m_width, m_height);
	}
}

void RAS_2DFilterOffScreen::Unbind(RAS_Rasterizer *rasty, RAS_ICanvas *canvas)
{
	if (m_flag & RAS_MIPMAP) {
		MipmapTexture();
	}

	if (!(m_flag & RAS_VIEWPORT_SIZE)) {
		const int width = canvas->GetWidth();
		const int height = canvas->GetHeight();
		rasty->SetViewport(0, 0, width, height);
		rasty->SetScissor(0, 0, width, height);
	}
}

bool RAS_2DFilterOffScreen::GetValid() const
{
	return GPU_framebuffer_check_valid(m_frameBuffer, nullptr);
}

int RAS_2DFilterOffScreen::GetColorBindCode(unsigned short index) const
{
	if (!m_colorTextures[index]) {
		return -1;
	}

	return GPU_texture_opengl_bindcode(m_colorTextures[index]);
}

int RAS_2DFilterOffScreen::GetDepthBindCode() const
{
	if (!m_depthTexture) {
		return -1;
	}

	return GPU_texture_opengl_bindcode(m_depthTexture);
}

unsigned int RAS_2DFilterOffScreen::GetWidth() const
{
	return m_width;
}

unsigned int RAS_2DFilterOffScreen::GetHeight() const
{
	return m_height;
}
