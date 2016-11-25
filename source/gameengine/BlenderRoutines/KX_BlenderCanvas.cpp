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

/** \file gameengine/BlenderRoutines/KX_BlenderCanvas.cpp
 *  \ingroup blroutines
 */

#include "MEM_guardedalloc.h"

#include "KX_BlenderCanvas.h"
#include "KX_Globals.h"

#include "DNA_screen_types.h"
#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_string.h"
#include "BLI_path_util.h"

#include "BKE_image.h"

#include "RAS_IRasterizer.h"

#include "GHOST_IWindow.h"

#include <assert.h>
#include <iostream>

extern "C" {
#  include "WM_api.h"
#  include "wm_cursors.h"
#  include "wm_window.h"
}

KX_BlenderCanvas::KX_BlenderCanvas(RAS_IRasterizer *rasty, wmWindowManager *wm, wmWindow *win, RAS_Rect &rect, struct ARegion *ar)
	:RAS_ICanvas(rasty),
	m_wm(wm),
	m_win(win),
	m_area_rect(rect), // initialize area so that it's available for game logic on frame 1 (ImageViewport)
	m_ar(ar)
{
	m_frame = 1;

	m_rasterizer->GetViewport(m_viewport);
}

KX_BlenderCanvas::~KX_BlenderCanvas()
{
}

void KX_BlenderCanvas::Init()
{
}

void KX_BlenderCanvas::SwapBuffers()
{
	wm_window_swap_buffers(m_win);
}

void KX_BlenderCanvas::SetSwapInterval(int interval)
{
	wm_window_set_swap_interval(m_win, interval);
}

bool KX_BlenderCanvas::GetSwapInterval(int &intervalOut)
{
	return wm_window_get_swap_interval(m_win, &intervalOut);
}

void KX_BlenderCanvas::GetDisplayDimensions(int &width, int &height)
{
	wm_get_screensize(&width, &height);
}

void KX_BlenderCanvas::ResizeWindow(int width, int height)
{
	// Not implemented for the embedded player
}

void KX_BlenderCanvas::Resize(int width, int height)
{
	// Not implemented for the embedded player
}

void KX_BlenderCanvas::SetFullScreen(bool enable)
{
	// Not implemented for the embedded player
}

bool KX_BlenderCanvas::GetFullScreen()
{
	// Not implemented for the embedded player
	return false;
}

void KX_BlenderCanvas::BeginDraw()
{
	// in case of multi-window we need to ensure we are drawing to the correct
	// window always, because it may change in window event handling
	wm_window_make_drawable(m_wm, m_win);
}

void KX_BlenderCanvas::EndDraw()
{
	// nothing needs to be done here
}

void KX_BlenderCanvas::BeginFrame()
{
}

void KX_BlenderCanvas::EndFrame()
{
}

int KX_BlenderCanvas::GetWidth() const
{
	return m_area_rect.GetWidth();
}

int KX_BlenderCanvas::GetHeight() const
{
	return m_area_rect.GetHeight();
}

void KX_BlenderCanvas::ConvertMousePosition(int x, int y, int &r_x, int &r_y, bool screen)
{
	if (screen) {
		int _x, _y;
		((GHOST_IWindow *)m_win->ghostwin)->screenToClient(x, y, _x, _y);
		x = _x;
		y = _y;
	}

	r_x = x - m_area_rect.GetLeft() - 1;
	r_y = -y + m_area_rect.GetTop() - 1;
}

float KX_BlenderCanvas::GetMouseNormalizedX(int x)
{
	return float(x) / this->GetWidth();
}

float KX_BlenderCanvas::GetMouseNormalizedY(int y)
{
	return float(y) / this->GetHeight();
}

const RAS_Rect &KX_BlenderCanvas::GetDisplayArea() const
{
	return m_displayarea;
}

void KX_BlenderCanvas::SetDisplayArea(RAS_Rect *rect)
{
	m_displayarea = *rect;
}

RAS_Rect &KX_BlenderCanvas::GetWindowArea()
{
	return m_area_rect;
}

void KX_BlenderCanvas::SetViewPort(int x1, int y1, int x2, int y2)
{
	/* x1 and y1 are the min pixel coordinate (e.g. 0)
	 * x2 and y2 are the max pixel coordinate
	 * the width,height is calculated including both pixels
	 * therefore: max - min + 1
	 */
	int vp_width = (x2 - x1) + 1;
	int vp_height = (y2 - y1) + 1;
	int minx = m_area_rect.GetLeft();
	int miny = m_area_rect.GetBottom();

	m_area_rect.SetLeft(minx + x1);
	m_area_rect.SetBottom(miny + y1);
	m_area_rect.SetRight(minx + x2);
	m_area_rect.SetTop(miny + y2);

	m_viewport[0] = minx + x1;
	m_viewport[1] = miny + y1;
	m_viewport[2] = vp_width;
	m_viewport[3] = vp_height;
}

void KX_BlenderCanvas::UpdateViewPort(int x1, int y1, int x2, int y2)
{
	m_viewport[0] = x1;
	m_viewport[1] = y1;
	m_viewport[2] = x2;
	m_viewport[3] = y2;
}

const int *KX_BlenderCanvas::GetViewPort()
{
	return m_viewport;
}

void KX_BlenderCanvas::SetMouseState(RAS_MouseState mousestate)
{
	m_mousestate = mousestate;

	switch (mousestate) {
		case MOUSE_INVISIBLE:
		{
			WM_cursor_set(m_win, CURSOR_NONE);
			break;
		}
		case MOUSE_WAIT:
		{
			WM_cursor_set(m_win, CURSOR_WAIT);
			break;
		}
		case MOUSE_NORMAL:
		{
			WM_cursor_set(m_win, CURSOR_STD);
			break;
		}
		default:
		{
		}
	}
}

//	(0,0) is top left, (width,height) is bottom right
void KX_BlenderCanvas::SetMousePosition(int x, int y)
{
	int winX = m_area_rect.GetLeft();
	int winY = m_area_rect.GetBottom();
	int winH = m_area_rect.GetHeight();

	WM_cursor_warp(m_win, winX + x + 1, winY + (winH - y - 1));
}

void KX_BlenderCanvas::MakeScreenShot(const char *filename)
{
	unsigned int *pixeldata;
	bScreen *screen = m_win->screen;

	int x = m_area_rect.GetLeft();
	int y = m_area_rect.GetBottom();
	int width = m_area_rect.GetWidth();
	int height = m_area_rect.GetHeight();

	pixeldata = m_rasterizer->MakeScreenshot(x, y, width, height);
	if (!pixeldata) {
		std::cerr << "KX_BlenderCanvas: Unable to take screenshot!" << std::endl;
		return;
	}

	/* initialize image file format data */
	Scene *scene = (screen) ? screen->scene : NULL;
	ImageFormatData *im_format = (ImageFormatData *)MEM_mallocN(sizeof(ImageFormatData), "im_format");

	if (scene) {
		*im_format = scene->r.im_format;
	}
	else {
		BKE_imformat_defaults(im_format);
	}

	// create file path
	char path[FILE_MAX];
	BLI_strncpy(path, filename, FILE_MAX);
	BLI_path_abs(path, KX_GetMainPath().ReadPtr());

	/* save_screenshot() frees dumprect and im_format */
	save_screenshot(path, width, height, pixeldata, im_format);
}
