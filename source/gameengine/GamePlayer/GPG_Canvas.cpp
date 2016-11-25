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

#include "RAS_IRasterizer.h"

#include "BLI_string.h"
#include "BLI_path_util.h"
#include "BLI_utildefines.h"

#include "BKE_image.h"
#include "MEM_guardedalloc.h"
#include "DNA_space_types.h"

#include "CM_Message.h"

GPG_Canvas::GPG_Canvas(RAS_IRasterizer *rasty, GHOST_IWindow *window)
	: RAS_ICanvas(rasty),
	m_window(window),
	m_width(0),
	m_height(0)
{
	m_rasterizer->GetViewport(m_viewport);

	if (m_window) {
		GHOST_Rect bnds;
		m_window->getClientBounds(bnds);
		this->Resize(bnds.getWidth(), bnds.getHeight());
	}
}

GPG_Canvas::~GPG_Canvas()
{
}

int GPG_Canvas::GetWidth() const
{
	return m_width;
}

int GPG_Canvas::GetHeight() const
{
	return m_height;
}

const RAS_Rect &GPG_Canvas::GetDisplayArea() const
{
	return m_displayarea;
}

void GPG_Canvas::SetDisplayArea(RAS_Rect *rect)
{
	m_displayarea = *rect;
}

RAS_Rect &GPG_Canvas::GetWindowArea()
{
	return m_displayarea;
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
	m_width = width;
	m_height = height;

	// initialize area so that it's available for game logic on frame 1 (ImageViewport)
	m_displayarea.SetLeft(0);
	m_displayarea.SetBottom(0);
	m_displayarea.SetRight(width);
	m_displayarea.SetTop(height);
}

void GPG_Canvas::SetViewPort(int x1, int y1, int x2, int y2)
{
	/*	x1 and y1 are the min pixel coordinate (e.g. 0)
	    x2 and y2 are the max pixel coordinate
	    the width,height is calculated including both pixels
	    therefore: max - min + 1
	 */

	m_viewport[0] = x1;
	m_viewport[1] = y1;
	m_viewport[2] = x2 - x1 + 1;
	m_viewport[3] = y2 - y1 + 1;
}

void GPG_Canvas::UpdateViewPort(int x1, int y1, int x2, int y2)
{
	m_viewport[0] = x1;
	m_viewport[1] = y1;
	m_viewport[2] = x2;
	m_viewport[3] = y2;
}

const int *GPG_Canvas::GetViewPort()
{
	return m_viewport;
}

void GPG_Canvas::MakeScreenShot(const char *filename)
{
	// copy image data
	unsigned int dumpsx = GetWidth();
	unsigned int dumpsy = GetHeight();

	unsigned int *pixels = m_rasterizer->MakeScreenshot(0, 0, dumpsx, dumpsy);

	if (!pixels) {
		CM_Error("cannot allocate pixels array");
		return;
	}

	// initialize image file format data
	ImageFormatData *im_format = (ImageFormatData *)MEM_mallocN(sizeof(ImageFormatData), "im_format");
	BKE_imformat_defaults(im_format);

	// create file path
	char path[FILE_MAX];
	BLI_strncpy(path, filename, FILE_MAX);
	BLI_path_abs(path, KX_GetMainPath().ReadPtr());

	/* save_screenshot() frees dumprect and im_format */
	save_screenshot(path, dumpsx, dumpsy, pixels, im_format);
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
				m_window->setCursorVisibility(false);
				break;
			case MOUSE_WAIT:
				m_window->setCursorShape(GHOST_kStandardCursorWait);
				m_window->setCursorVisibility(true);
				break;
			case MOUSE_NORMAL:
				m_window->setCursorShape(GHOST_kStandardCursorDefault);
				m_window->setCursorVisibility(true);
				break;
		}
	}
}

void GPG_Canvas::SwapBuffers()
{
	if (m_window) {
		m_window->swapBuffers();
	}
}

void GPG_Canvas::SetSwapInterval(int interval)
{
	if (m_window) {
		m_window->setSwapInterval(interval);
	}
}

bool GPG_Canvas::GetSwapInterval(int& intervalOut)
{
	if (m_window) {
		return (bool)m_window->getSwapInterval(intervalOut);
	}

	return false;
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
	m_window->screenToClient(x, y, r_x, r_y);
}

float GPG_Canvas::GetMouseNormalizedX(int x)
{
	return float(x) / this->GetWidth();
}

float GPG_Canvas::GetMouseNormalizedY(int y)
{
	return float(y) / this->GetHeight();
}
