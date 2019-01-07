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

/** \file gameengine/GamePlayer/GPG_Canvas.cpp
 *  \ingroup player
 */

#include "GPG_Canvas.h"
#include "GHOST_ISystem.h"

#include "KX_Globals.h"

#include "RAS_Rasterizer.h"

#include "BLI_string.h"
#include "BLI_path_util.h"
#include "BLI_utildefines.h"

#include "BKE_image.h"
#include "MEM_guardedalloc.h"
#include "DNA_space_types.h"

GPG_Canvas::GPG_Canvas(RAS_Rasterizer *rasty, const RAS_OffScreen::AttachmentList& attachments, GHOST_IWindow *window, int numSamples)
	:RAS_ICanvas(attachments, numSamples),
	m_window(window)
{
	rasty->GetViewport(m_viewport);

	if (m_window) {
		GHOST_Rect bnds;
		m_window->getClientBounds(bnds);
		this->Resize(bnds.getWidth(), bnds.getHeight());
	}
}

GPG_Canvas::~GPG_Canvas()
{
}

void GPG_Canvas::BeginFrame()
{
}

void GPG_Canvas::EndFrame()
{
}

void GPG_Canvas::BeginDraw()
{
}

void GPG_Canvas::EndDraw()
{
}

void GPG_Canvas::Resize(int width, int height)
{
	// initialize area so that it's available for game logic on frame 1 (ImageViewport)
	m_area.SetLeft(0);
	m_area.SetBottom(0);
	m_area.SetRight(width - 1);
	m_area.SetTop(height - 1);

	UpdateOffScreens();
}

void GPG_Canvas::SetViewPort(int x, int y, int width, int height)
{
	m_viewport[0] = x;
	m_viewport[1] = y;
	m_viewport[2] = width;
	m_viewport[3] = height;
}

void GPG_Canvas::UpdateViewPort(int x, int y, int width, int height)
{
	m_viewport[0] = x;
	m_viewport[1] = y;
	m_viewport[2] = width;
	m_viewport[3] = height;
}

void GPG_Canvas::MakeScreenShot(const std::string& filename)
{
	// copy image data
	unsigned int dumpsx = GetWidth();
	unsigned int dumpsy = GetHeight();

	// initialize image file format data
	ImageFormatData *im_format = (ImageFormatData *)MEM_mallocN(sizeof(ImageFormatData), "im_format");
	BKE_imformat_defaults(im_format);

	// create file path
	char path[FILE_MAX];
	BLI_strncpy(path, filename.c_str(), FILE_MAX);
	BLI_path_abs(path, KX_GetMainPath().c_str());

	AddScreenshot(path, 0, 0, dumpsx, dumpsy, im_format);
}

void GPG_Canvas::Init()
{
	if (m_window) {
		m_window->setDrawingContextType(GHOST_kDrawingContextTypeOpenGL);
		BLI_assert(m_window->getDrawingContextType() == GHOST_kDrawingContextTypeOpenGL);
	}
}

void GPG_Canvas::SetMousePosition(int x, int y)
{
	GHOST_ISystem *system = GHOST_ISystem::getSystem();
	if (system && m_window) {
		GHOST_TInt32 gx = (GHOST_TInt32)x;
		GHOST_TInt32 gy = (GHOST_TInt32)y;
		GHOST_TInt32 cx;
		GHOST_TInt32 cy;
		m_window->clientToScreen(gx, gy, cx, cy);
		system->setCursorPosition(cx, cy);
	}
}


void GPG_Canvas::SetMouseState(RAS_MouseState mousestate)
{
	m_mousestate = mousestate;

	if (m_window) {
		switch (mousestate) {
			case MOUSE_INVISIBLE:
			{
				m_window->setCursorVisibility(false);
				break;
			}
			case MOUSE_WAIT:
			{
				m_window->setCursorShape(GHOST_kStandardCursorWait);
				m_window->setCursorVisibility(true);
				break;
			}
			case MOUSE_NORMAL:
			{
				m_window->setCursorShape(GHOST_kStandardCursorDefault);
				m_window->setCursorVisibility(true);
				break;
			}
		}
	}
}

void GPG_Canvas::SwapBuffers()
{
	if (m_window) {
		m_window->swapBuffers();
	}
}

void GPG_Canvas::SetSwapControl(SwapControl control)
{
	if (m_window) {
		m_window->setSwapInterval(swapInterval[control]);
	}
	RAS_ICanvas::SetSwapControl(control);
}

void GPG_Canvas::GetDisplayDimensions(int &width, int &height)
{
	unsigned int uiwidth;
	unsigned int uiheight;

	GHOST_ISystem *system = GHOST_ISystem::getSystem();
	system->getMainDisplayDimensions(uiwidth, uiheight);

	width = uiwidth;
	height = uiheight;
}

void GPG_Canvas::ResizeWindow(int width, int height)
{
	if (m_window->getState() == GHOST_kWindowStateFullScreen) {
		GHOST_ISystem *system = GHOST_ISystem::getSystem();
		GHOST_DisplaySetting setting;
		setting.xPixels = width;
		setting.yPixels = height;
		//XXX allow these to be changed or kept from previous state
		setting.bpp = 32;
		setting.frequency = 60;

		system->updateFullScreen(setting, &m_window);
	}

	m_window->setClientSize(width, height);

	Resize(width, height);
}

void GPG_Canvas::SetFullScreen(bool enable)
{
	if (enable) {
		m_window->setState(GHOST_kWindowStateFullScreen);
	}
	else {
		m_window->setState(GHOST_kWindowStateNormal);
	}
}

bool GPG_Canvas::GetFullScreen()
{
	return (m_window->getState() == GHOST_kWindowStateFullScreen);
}

void GPG_Canvas::ConvertMousePosition(int x, int y, int &r_x, int &r_y, bool UNUSED(screen))
{
	int _x;
	int _y;
	m_window->screenToClient(x, y, _x, _y);

	const float fac = m_window->getNativePixelSize();

	r_x = _x * fac;
	r_y = _y * fac;
}
