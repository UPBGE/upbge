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

/** \file gameengine/Rasterizer/RAS_2DFilterFrameBuffer.cpp
 *  \ingroup bgerast
 */

#include "RAS_2DFilterFrameBuffer.h"
#include "RAS_ICanvas.h"

#include "GPU_framebuffer.h"
#include "GPU_texture.h"

extern "C" {
#  include "DRW_render.h"
#  include "eevee_private.h"
}

RAS_2DFilterFrameBuffer::RAS_2DFilterFrameBuffer(unsigned short colorSlots, Flag flag, unsigned int width, unsigned int height,
											 RAS_Rasterizer::HdrType hdr)
	:m_flag(flag),
	m_colorSlots(colorSlots),
	m_hdr(hdr),
	m_width(width),
	m_height(height),
	m_depthTexture(nullptr),
	m_frameBuffer(nullptr)
{
	for (unsigned short i = 0; i < NUM_COLOR_SLOTS; ++i) {
		m_colorTextures[i] = nullptr;
	}

	if (!(m_flag & RAS_VIEWPORT_SIZE)) {
		Construct();
	}
}

RAS_2DFilterFrameBuffer::~RAS_2DFilterFrameBuffer()
{
	GPU_framebuffer_free(m_frameBuffer);
	for (unsigned short i = 0; i < NUM_COLOR_SLOTS; ++i) {
		GPUTexture *texture = m_colorTextures[i];
		if (texture) {
			GPU_texture_free(texture);
			GPU_texture_free(m_depthTexture);
		}
	}
}

/* WARNING: Always respect the order from RAS_Rasterizer::HdrType.
* RAS_HDR_NONE can use RGBA8 as the tonemapping is applied before the filters. */
static const DRWTextureFormat dataTypeEnums[] = {
	DRW_TEX_RGB_11_11_10, // RAS_HDR_NONE
	DRW_TEX_RGBA_16, // RAS_HDR_HALF_FLOAT
	DRW_TEX_RGBA_32 // RAS_HDR_FULL_FLOAT
};

void RAS_2DFilterFrameBuffer::Construct()
{
	GPUTexture *colorTex[NUM_COLOR_SLOTS];
	for (unsigned short i = 0; i < m_colorSlots; ++i) {
		colorTex[i] = m_colorTextures[i];
		if (colorTex[i]) {
			GPU_framebuffer_texture_detach(colorTex[i]);
			GPU_texture_free(colorTex[i]);
		}

		colorTex[i] = DRW_texture_create_2D(m_width, m_height, dataTypeEnums[m_hdr], DRW_TEX_FILTER, nullptr);
		m_colorTextures[i] = colorTex[i];
	}

	if (m_depthTexture) {
		GPU_framebuffer_texture_detach(m_depthTexture);
		GPU_texture_free(m_depthTexture);
	}

	GPUTexture *depthtex = DRW_texture_create_2D(m_width, m_height, DRW_TEX_DEPTH_24, DRW_TEX_FILTER, nullptr);
	GPU_texture_compare_mode(depthtex, false);

	/* TODO: Handle more than 1 color texture slot */
	DRWFboTexture fbtex[2] = { { &m_colorTextures[0], dataTypeEnums[m_hdr], DRWTextureFlag(DRW_TEX_FILTER) },
							   { &depthtex, DRW_TEX_DEPTH_24, DRWTextureFlag(0) } };
	DRW_framebuffer_init(&m_frameBuffer, &draw_engine_eevee_type, m_width, m_height, fbtex, ARRAY_SIZE(fbtex));
	GPU_framebuffer_set_bge_type(m_frameBuffer, GPU_FRAMEBUFFER_FILTER0);

	m_depthTexture = depthtex;
}

void RAS_2DFilterFrameBuffer::MipmapTexture()
{
	for (unsigned short i = 0; i < m_colorSlots; ++i) {
		GPUTexture *texture = m_colorTextures[i];
		GPU_texture_bind(texture, 0);
		GPU_texture_filter_mode(texture, true);
		GPU_texture_mipmap_mode(texture, true, false);
		GPU_texture_generate_mipmap(texture);
		GPU_texture_unbind(texture);
	}
}

bool RAS_2DFilterFrameBuffer::Update(RAS_ICanvas *canvas)
{
	if (m_flag & RAS_VIEWPORT_SIZE) {
		const unsigned int width = canvas->GetWidth() + 1;
		const unsigned int height = canvas->GetHeight() + 1;
		if (m_width != width || m_height != height) {
			m_width = width;
			m_height = height;

			Construct();
		}
	}

	return GetValid();
}

void RAS_2DFilterFrameBuffer::Bind(RAS_Rasterizer *rasty)
{
	GPU_framebuffer_bind_all_attachments(m_frameBuffer);

	if (!(m_flag & RAS_VIEWPORT_SIZE)) {
		rasty->SetViewport(0, 0, m_width + 1, m_height + 1);
		rasty->SetScissor(0, 0, m_width + 1, m_height + 1);
	}
}

void RAS_2DFilterFrameBuffer::Unbind(RAS_Rasterizer *rasty, RAS_ICanvas *canvas)
{
	if (m_flag & RAS_MIPMAP) {
		MipmapTexture();
	}

	if (!(m_flag & RAS_VIEWPORT_SIZE)) {
		const int width = canvas->GetWidth();
		const int height = canvas->GetHeight();
		rasty->SetViewport(0, 0, width + 1, height + 1);
		rasty->SetScissor(0, 0, width + 1, height + 1);
	}
}

bool RAS_2DFilterFrameBuffer::GetValid() const
{
	return GPU_framebuffer_check_valid(m_frameBuffer, nullptr);
}

int RAS_2DFilterFrameBuffer::GetColorBindCode(unsigned short index) const
{
	if (!m_colorTextures[index]) {
		return -1;
	}

	return GPU_texture_opengl_bindcode(m_colorTextures[index]);
}

int RAS_2DFilterFrameBuffer::GetDepthBindCode() const
{
	if (!m_depthTexture) {
		return -1;
	}

	return GPU_texture_opengl_bindcode(m_depthTexture);
}

unsigned int RAS_2DFilterFrameBuffer::GetWidth() const
{
	return m_width;
}

unsigned int RAS_2DFilterFrameBuffer::GetHeight() const
{
	return m_height;
}
