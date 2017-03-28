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

/** \file RAS_OffScreen.h
 *  \ingroup bgerast
 */

#ifndef __RAS_OFFSCREEN_H__
#define __RAS_OFFSCREEN_H__

#include "RAS_Rasterizer.h"

#include "GPU_framebuffer.h"

struct GPUOffScreen;
struct GPUTexture;

class RAS_OffScreen
{
private:
	/// All the off screens used.
	GPUOffScreen *m_offScreen;
	/// The off screen type, render, final, filter ect...
	RAS_Rasterizer::OffScreenType m_type;

	/// The last bound off screen, set to nullptr in RestoreScreen().
	static RAS_OffScreen *lastOffScreen;

public:
	RAS_OffScreen(unsigned int width, unsigned int height, int samples, GPUHDRType hdrType, GPUOffScreenMode mode, char errOut[256],
				  RAS_Rasterizer::OffScreenType type);
	~RAS_OffScreen();

	bool GetValid() const;

	void Bind();
	/// NOTE: This function has the side effect to leave the destination off screen bound.
	RAS_OffScreen *Blit(RAS_OffScreen *dstOffScreen, bool color, bool depth);

	void BindColorTexture(unsigned short slot);
	void BindDepthTexture(unsigned short slot);
	void UnbindColorTexture();
	void UnbindDepthTexture();

	void MipmapTexture();
	void UnmipmapTexture();

	int GetColorBindCode() const;

	int GetSamples() const;
	unsigned GetWidth() const;
	unsigned GetHeight() const;
	RAS_Rasterizer::OffScreenType GetType() const;

	GPUTexture *GetDepthTexture();

	static RAS_OffScreen *GetLastOffScreen();
	static void RestoreScreen();
};

#endif  // __RAS_OFFSCREEN_H__
