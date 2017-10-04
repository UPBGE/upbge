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
#include "GPU_framebuffer.h"

RAS_FrameBuffer::RAS_FrameBuffer(GPUFrameBuffer *framebuffer, RAS_Rasterizer::FrameBufferType type)
	:m_frameBuffer(framebuffer),
	m_type(type)
{
}

RAS_FrameBuffer::~RAS_FrameBuffer()
{
	if (m_frameBuffer) {
		GPU_framebuffer_free(m_frameBuffer);
	}
}

GPUFrameBuffer *RAS_FrameBuffer::GetFrameBuffer()
{
	return m_frameBuffer;
}

unsigned int RAS_FrameBuffer::GetWidth() const
{
	return GPU_texture_width(GPU_framebuffer_color_texture(m_frameBuffer));
}

unsigned int RAS_FrameBuffer::GetHeight() const
{
	return GPU_texture_height(GPU_framebuffer_color_texture(m_frameBuffer));
}

RAS_Rasterizer::FrameBufferType RAS_FrameBuffer::GetType() const
{
	return m_type;
}
