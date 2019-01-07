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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_BlenderCanvas.h
 *  \ingroup blroutines
 */

#ifndef __KX_BLENDERCANVAS_H__
#define __KX_BLENDERCANVAS_H__

#ifdef WIN32
#include <windows.h>
#endif

#include "RAS_ICanvas.h"
#include "RAS_Rect.h"

struct wmWindow;
struct wmWindowManager;

/**
 * 2D Blender device context abstraction.
 * The connection from 3d rendercontext to 2d Blender surface embedding.
 */

class KX_BlenderCanvas : public RAS_ICanvas
{
private:
	wmWindowManager *m_wm;
	wmWindow *m_win;

public:
	KX_BlenderCanvas(RAS_Rasterizer *rasty, const RAS_OffScreen::AttachmentList& attachments,
			wmWindowManager *wm, wmWindow *win, RAS_Rect &rect, int numSamples);
	virtual ~KX_BlenderCanvas();

	virtual void Init();

	virtual void SwapBuffers();
	virtual void SetSwapControl(SwapControl control);

	virtual void GetDisplayDimensions(int &width, int &height);
	virtual void ResizeWindow(int width, int height);
	virtual void Resize(int width, int height);

	virtual void SetFullScreen(bool enable);
	virtual bool GetFullScreen();

	virtual void BeginFrame();
	virtual void EndFrame();

	virtual void ConvertMousePosition(int x, int y, int &r_x, int &r_y, bool screen);

	virtual void SetViewPort(int x, int y, int width, int height);
	virtual void UpdateViewPort(int x, int y, int width, int height);

	virtual void SetMouseState(RAS_MouseState mousestate);
	virtual void SetMousePosition(int x, int y);

	virtual void MakeScreenShot(const std::string& filename);

	virtual void BeginDraw();
	virtual void EndDraw();
};

#endif  // __KX_BLENDERCANVAS_H__
