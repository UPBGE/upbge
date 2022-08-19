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

#include "KX_BlenderCanvas.h"

#include "BKE_context.h"
#include "BKE_image.h"
#include "BKE_image_format.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "DNA_scene_types.h"
#include "GHOST_IWindow.h"
#include "MEM_guardedalloc.h"
#include "WM_api.h"
#include "wm_window.h"

#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"

KX_BlenderCanvas::KX_BlenderCanvas(
    RAS_Rasterizer *rasty, wmWindowManager *wm, wmWindow *win, rcti *viewport, struct ARegion *ar)
    : RAS_ICanvas(rasty), m_wm(wm), m_win(win), m_ar(ar)
{
  m_frame = 1;

  m_viewportArea = RAS_Rect(viewport->xmin, viewport->ymin, viewport->xmax, viewport->ymax);
  m_windowArea = RAS_Rect(ar->winrct.xmin, ar->winrct.ymin, ar->winrct.xmax, ar->winrct.ymax);
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

void KX_BlenderCanvas::ConvertMousePosition(int x, int y, int &r_x, int &r_y, bool screen)
{
  if (screen) {
    int _x, _y;
    ((GHOST_IWindow *)m_win->ghostwin)->screenToClient(x, y, _x, _y);
    x = _x;
    y = _y;
  }

  r_x = x - m_viewportArea.GetLeft() - 1;
  r_y = -y + m_viewportArea.GetTop() - 1;
}

void KX_BlenderCanvas::SetMouseState(RAS_MouseState mousestate)
{
  m_mousestate = mousestate;

  switch (mousestate) {
    case MOUSE_INVISIBLE: {
      WM_cursor_set(m_win, WM_CURSOR_NONE);
      break;
    }
    case MOUSE_WAIT: {
      WM_cursor_set(m_win, WM_CURSOR_WAIT);
      break;
    }
    case MOUSE_NORMAL: {
      WM_cursor_set(m_win, WM_CURSOR_DEFAULT);
      break;
    }
    default: {
    }
  }
}

//	(0,0) is top left, (width,height) is bottom right
void KX_BlenderCanvas::SetMousePosition(int x, int y)
{
  int winX = m_viewportArea.GetLeft();
  int winY = m_viewportArea.GetBottom();
  int winH = m_viewportArea.GetHeight();

  WM_cursor_warp(m_win, winX + x + 1, winY + (winH - y - 1));
}

void KX_BlenderCanvas::MakeScreenShot(const std::string &filename)
{
  int x = m_viewportArea.GetLeft();
  int y = m_viewportArea.GetBottom();
  int width = m_viewportArea.GetWidth();
  int height = m_viewportArea.GetHeight();

  /* initialize image file format data */
  bContext *C = KX_GetActiveEngine()->GetContext();
  Scene *scene = CTX_data_scene(C);
  ImageFormatData *im_format = (ImageFormatData *)MEM_mallocN(sizeof(ImageFormatData),
                                                              "im_format");

  if (scene) {
    *im_format = scene->r.im_format;
  }
  else {
    BKE_image_format_init(im_format, false);
  }

  // create file path
  char path[FILE_MAX];
  BLI_strncpy(path, filename.c_str(), FILE_MAX);
  BLI_path_abs(path, KX_GetMainPath().c_str());

  AddScreenshot(path, x, y, width, height, im_format);
}

bool KX_BlenderCanvas::IsBlenderPlayer()
{
  return false;
}
