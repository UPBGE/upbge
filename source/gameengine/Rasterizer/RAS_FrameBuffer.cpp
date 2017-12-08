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

/** \file gameengine/Rasterizer/RAS_FrameBuffer.cpp
 *  \ingroup bgerast
 */

#include "RAS_FrameBuffer.h"

extern "C" {
#  include "GPU_framebuffer.h"
#  include "GPU_texture.h"
#  include "DRW_render.h"
#  include "eevee_private.h"
}

// WARNING: Always respect the order from RAS_Rasterizer::HdrType.
static const DRWTextureFormat dataTypeEnums[] = {
	DRW_TEX_RGB_11_11_10, // RAS_HDR_NONE
	DRW_TEX_RGBA_16, // RAS_HDR_HALF_FLOAT
	DRW_TEX_RGBA_32 // RAS_HDR_FULL_FLOAT
};

RAS_FrameBuffer::RAS_FrameBuffer(unsigned int width, unsigned int height, RAS_Rasterizer::HdrType hdrtype, RAS_Rasterizer::FrameBufferType fbtype)
	:m_frameBufferType(fbtype),
	m_hdrType(hdrtype),
	m_frameBuffer(nullptr)
{
	GPUFrameBuffer *fb = nullptr;
	GPUTexture *tex = DRW_texture_create_2D(width, height, dataTypeEnums[hdrtype], DRWTextureFlag(DRW_TEX_FILTER | DRW_TEX_MIPMAP), nullptr);
	GPUTexture *depthTex = DRW_texture_create_2D(width, height, DRW_TEX_DEPTH_24, DRWTextureFlag(0), NULL);
	DRWFboTexture fbtex[2] = { { &tex, dataTypeEnums[m_hdrType], DRWTextureFlag(DRW_TEX_FILTER | DRW_TEX_MIPMAP) },
							   { &depthTex, DRW_TEX_DEPTH_24, DRWTextureFlag(0) } };
	DRW_framebuffer_init(&fb, &draw_engine_eevee_type, width, height, fbtex, ARRAY_SIZE(fbtex));
	m_colorAttachment = tex;
	m_depthAttachment = depthTex;
	m_frameBuffer = fb;
}

RAS_FrameBuffer::~RAS_FrameBuffer()
{
	GPU_framebuffer_free(m_frameBuffer);
}

GPUFrameBuffer *RAS_FrameBuffer::GetFrameBuffer()
{
	return m_frameBuffer;
}

unsigned int RAS_FrameBuffer::GetWidth() const
{
	return GPU_texture_width(m_colorAttachment);
}

unsigned int RAS_FrameBuffer::GetHeight() const
{
	return GPU_texture_height(m_colorAttachment);
}

RAS_Rasterizer::FrameBufferType RAS_FrameBuffer::GetType() const
{
	return m_frameBufferType;
}
