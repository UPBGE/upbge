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

/** \file gameengine/Rasterizer/RAS_OffScreen.cpp
 *  \ingroup bgerast
 */

#include "RAS_OffScreen.h"

RAS_OffScreen *RAS_OffScreen::lastOffScreen = nullptr;

RAS_OffScreen::RAS_OffScreen(unsigned int width, unsigned int height, int samples, GPUHDRType hdrType, GPUOffScreenMode mode, char errOut[256],
							 RAS_Rasterizer::OffScreenType type)
	:m_offScreen(GPU_offscreen_create(width, height, samples, hdrType, mode, errOut)),
	m_type(type)
{
}

RAS_OffScreen::~RAS_OffScreen()
{
	if (GetValid()) {
		GPU_offscreen_free(m_offScreen);
	}
}

bool RAS_OffScreen::GetValid() const
{
	return (m_offScreen != nullptr);
}

void RAS_OffScreen::Bind()
{
	GPU_offscreen_bind_simple(m_offScreen);
	lastOffScreen = this;
}

RAS_OffScreen *RAS_OffScreen::Blit(RAS_OffScreen *dstOffScreen, bool color, bool depth)
{
	GPU_offscreen_blit(m_offScreen, dstOffScreen->m_offScreen, color, depth);

	return dstOffScreen;
}

void RAS_OffScreen::BindColorTexture(unsigned short slot)
{
	GPU_texture_bind(GPU_offscreen_texture(m_offScreen), slot);
}

void RAS_OffScreen::BindDepthTexture(unsigned short slot)
{
	GPU_texture_bind(GPU_offscreen_depth_texture(m_offScreen), slot);
}

void RAS_OffScreen::UnbindColorTexture()
{
	GPU_texture_unbind(GPU_offscreen_texture(m_offScreen));
}

void RAS_OffScreen::UnbindDepthTexture()
{
	GPU_texture_unbind(GPU_offscreen_depth_texture(m_offScreen));
}

void RAS_OffScreen::MipmapTexture()
{
	GPUTexture *tex = GPU_offscreen_texture(m_offScreen);
	GPU_texture_filter_mode(tex, false, true, true);
	GPU_texture_generate_mipmap(tex);
}

void RAS_OffScreen::UnmipmapTexture()
{
	GPU_texture_filter_mode(GPU_offscreen_texture(m_offScreen), false, true, false);
}

int RAS_OffScreen::GetColorBindCode() const
{
	return GPU_offscreen_color_texture(m_offScreen);
}

int RAS_OffScreen::GetSamples() const
{
	return GPU_offscreen_samples(m_offScreen);
}

unsigned int RAS_OffScreen::GetWidth() const
{
	return GPU_offscreen_width(m_offScreen);
}

unsigned int RAS_OffScreen::GetHeight() const
{
	return GPU_offscreen_height(m_offScreen);
}

RAS_Rasterizer::OffScreenType RAS_OffScreen::GetType() const
{
	return m_type;
}

GPUTexture *RAS_OffScreen::GetDepthTexture()
{
	return GPU_offscreen_depth_texture(m_offScreen);
}

RAS_OffScreen *RAS_OffScreen::GetLastOffScreen()
{
	return lastOffScreen;
}

void RAS_OffScreen::RestoreScreen()
{
	GPU_framebuffer_restore();
	lastOffScreen = nullptr;
}
