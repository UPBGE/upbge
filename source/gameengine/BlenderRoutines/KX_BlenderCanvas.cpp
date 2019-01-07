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

#include "RAS_Rasterizer.h"

#include "GHOST_IWindow.h"

extern "C" {
#  include "WM_api.h"
#  include "wm_cursors.h"
#  include "wm_window.h"
}

KX_BlenderCanvas::KX_BlenderCanvas(RAS_Rasterizer *rasty, const RAS_OffScreen::AttachmentList& attachments,
		wmWindowManager *wm, wmWindow *win, RAS_Rect &rect, int numSamples)
	:RAS_ICanvas(attachments, numSamples),
	m_wm(wm),
	m_win(win)
{
	m_area = rect;
	rasty->GetViewport(m_viewport);
	UpdateOffScreens();
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

void KX_BlenderCanvas::SetSwapControl(SwapControl control)
{
	wm_window_set_swap_interval(m_win, swapInterval[control]);
	RAS_ICanvas::SetSwapControl(control);
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

void KX_BlenderCanvas::ConvertMousePosition(int x, int y, int &r_x, int &r_y, bool screen)
{
	if (screen) {
		wm_cursor_position_from_ghost(m_win, &x, &y);
	}

	r_x = x - m_area.GetLeft();
	r_y = -y + m_area.GetTop();
}

void KX_BlenderCanvas::SetViewPort(int x, int y, int width, int height)
{
	int minx = m_area.GetLeft();
	int miny = m_area.GetBottom();

	m_area.SetLeft(minx + x);
	m_area.SetBottom(miny + y);
	m_area.SetRight(minx + x + width - 1);
	m_area.SetTop(miny + y + height - 1);

	m_viewport[0] = minx + x;
	m_viewport[1] = miny + y;
	m_viewport[2] = width;
	m_viewport[3] = height;
}

void KX_BlenderCanvas::UpdateViewPort(int x, int y, int width, int height)
{
	m_viewport[0] = x;
	m_viewport[1] = y;
	m_viewport[2] = width;
	m_viewport[3] = height;
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

void KX_BlenderCanvas::SetMousePosition(int x, int y)
{
	int winX = m_area.GetLeft();
	int winY = m_area.GetBottom();
	int winMaxY = m_area.GetMaxY();

	WM_cursor_warp(m_win, winX + x, winY + (winMaxY - y));
}

void KX_BlenderCanvas::MakeScreenShot(const std::string& filename)
{
	bScreen *screen = m_win->screen;

	int x = m_area.GetLeft();
	int y = m_area.GetBottom();
	int width = m_area.GetWidth();
	int height = m_area.GetHeight();

	/* initialize image file format data */
	Scene *scene = (screen) ? screen->scene : nullptr;
	ImageFormatData *im_format = (ImageFormatData *)MEM_mallocN(sizeof(ImageFormatData), "im_format");

	if (scene) {
		*im_format = scene->r.im_format;
	}
	else {
		BKE_imformat_defaults(im_format);
	}

	// create file path
	char path[FILE_MAX];
	BLI_strncpy(path, filename.c_str(), FILE_MAX);
	BLI_path_abs(path, KX_GetMainPath().c_str());

	AddScreenshot(path, x, y, width, height, im_format);
}
