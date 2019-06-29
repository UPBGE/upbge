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

struct GPUFrameBuffer;
struct GPURenderBuffer;
struct GPUTexture;

class RAS_OffScreen
{
public:
	enum Type {
		RAS_OFFSCREEN_FILTER0,
		RAS_OFFSCREEN_FILTER1,
		RAS_OFFSCREEN_EYE_LEFT0,
		RAS_OFFSCREEN_EYE_RIGHT0,
		RAS_OFFSCREEN_EYE_LEFT1,
		RAS_OFFSCREEN_EYE_RIGHT1,
		RAS_OFFSCREEN_BLIT_DEPTH,

		RAS_OFFSCREEN_CUSTOM,

		RAS_OFFSCREEN_MAX,
	};

	struct Attachment
	{
		unsigned short size;
		RAS_Rasterizer::HdrType hdr;
	};

	using AttachmentList = std::vector<Attachment>;

private:
	enum {
		NUM_COLOR_SLOTS = 8
	};

	unsigned int m_width;
	unsigned int m_height;
	unsigned short m_samples;
	unsigned short m_numColorSlots;

	/// All the off screens used.
	GPUFrameBuffer *m_frameBuffer;

	union Slot
	{
		Slot();

		GPURenderBuffer *m_rb;
		GPUTexture *m_tex;
	};

	Slot m_colorSlots[NUM_COLOR_SLOTS];
	Slot m_depthSlot;

	/// The off screen type, render, final, filter ect...
	Type m_type;

	/// The last bound off screen, set to nullptr in RestoreScreen().
	static RAS_OffScreen *lastOffScreen;

public:
	RAS_OffScreen(unsigned int width, unsigned int height, unsigned short samples,
			const AttachmentList& attachments, Type type);
	~RAS_OffScreen();

	bool GetValid() const;

	void Bind();
	/// NOTE: This function has the side effect to leave the destination off screen bound.
	RAS_OffScreen *Blit(RAS_OffScreen *dstOffScreen, bool depth);

	void BindColorTexture(unsigned short slot, unsigned short unit);
	void BindDepthTexture(unsigned short unit);
	void UnbindColorTexture(unsigned short slot);
	void UnbindDepthTexture();

	void MipmapTextures();
	void UnmipmapTextures();

	int GetColorBindCode() const;

	unsigned short GetSamples() const;
	unsigned GetWidth() const;
	unsigned GetHeight() const;
	Type GetType() const;
	unsigned short GetNumColorSlot() const;

	GPUTexture *GetDepthTexture();

	static RAS_OffScreen *GetLastOffScreen();
	static void RestoreScreen();

	/** Return the output frame buffer normally used for the input frame buffer
	 * index in case of filters render.
	 * \param index The input frame buffer, can be a non-filter frame buffer.
	 * \return The output filter frame buffer.
	 */
	static Type NextFilterOffScreen(Type index);

	/** Return the output frame buffer normally used for the input frame buffer
	 * index in case of simple render.
	 * \param index The input render frame buffer, can be a eye frame buffer.
	 * \return The output render frame buffer.
	 */
	static Type NextRenderOffScreen(Type index); // TODO RAS_NamedOffScreen
};

#endif  // __RAS_OFFSCREEN_H__
