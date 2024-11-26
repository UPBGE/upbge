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

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_format.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"
#include "GHOST_ISystem.hh"
#include "GPU_context.hh"
#include "GPU_framebuffer.hh"
#include "MEM_guardedalloc.h"
#include "wm_window.hh"

#include "KX_Globals.h"

GPG_Canvas::GPG_Canvas(bContext *C, RAS_Rasterizer *rasty, GHOST_IWindow *window, bool useViewportRender)
    : RAS_ICanvas(rasty), m_context(C), m_window(window), m_width(0), m_height(0), m_useViewportRender(useViewportRender), m_nativePixelSize(1)
{
  if (m_window) {
    GHOST_Rect bnds;
    m_window->getClientBounds(bnds);
    m_nativePixelSize = window->getNativePixelSize();
    m_viewportArea = RAS_Rect(bnds.getWidth() * m_nativePixelSize,
                              bnds.getHeight() * m_nativePixelSize);
    m_windowArea = RAS_Rect(bnds.getWidth(), bnds.getHeight());
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
  if (!m_useViewportRender) {
    wmWindow *win = CTX_wm_window(m_context);
    /* See wm_draw_update for "chronology" */
    GPU_context_begin_frame((GPUContext *)win->gpuctx);
  }
}

void GPG_Canvas::EndDraw()
{
}

void GPG_Canvas::Resize(int width, int height)
{
  if (m_windowArea.GetWidth() != width || m_windowArea.GetHeight() != height) {
    m_viewportArea = RAS_Rect(width * m_nativePixelSize, height * m_nativePixelSize);
    m_windowArea = RAS_Rect(width, height);
    /* Following code needed to properly resize window with VULKAN backend */
    wmWindow *win = CTX_wm_window(m_context);
    win->sizex = width;
    win->sizey = height;
    wm_window_set_size(win, win->sizex, win->sizey);
    wm_window_update_size_position(win);
  }
}

void GPG_Canvas::MakeScreenShot(const std::string &filename)
{
  // copy image data
  unsigned int dumpsx = GetWidth();
  unsigned int dumpsy = GetHeight();

  // initialize image file format data
  ImageFormatData *im_format = (ImageFormatData *)MEM_mallocN(sizeof(ImageFormatData),
                                                              "im_format");
  BKE_image_format_init(im_format, false);

  // create file path
  char path[FILE_MAX];
  BLI_strncpy(path, filename.c_str(), FILE_MAX);
  BLI_path_abs(path, KX_GetMainPath().c_str());

  AddScreenshot(path, 0, 0, dumpsx, dumpsy, im_format);
}

void GPG_Canvas::Init()
{
  if (m_window) {
    const float clear_col[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GPU_framebuffer_clear_color_depth(GPU_framebuffer_active_get(), clear_col, 1.0f);
    /* This code below is already called at window creation */
    //m_window->setDrawingContextType(GHOST_kDrawingContextTypeOpenGL);
    //BLI_assert(m_window->getDrawingContextType() == GHOST_kDrawingContextTypeOpenGL);
  }
}

void GPG_Canvas::SetMousePosition(int x, int y)
{
  GHOST_ISystem *system = GHOST_ISystem::getSystem();
  if (system && m_window) {
    int32_t gx = (int32_t)x / m_nativePixelSize;
    int32_t gy = (int32_t)y / m_nativePixelSize;
    int32_t cx;
    int32_t cy;
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
    if (!m_useViewportRender) { // Not needed but for readability
      wmWindow *win = CTX_wm_window(m_context);
      /* See wm_draw_update for "chronology" */
      GPU_context_end_frame((GPUContext *)win->gpuctx);
    }
    m_window->swapBuffers();
  }
}

void GPG_Canvas::SetSwapInterval(int interval)
{
  if (m_window) {
    m_window->setSwapInterval(interval);
  }
}

bool GPG_Canvas::GetSwapInterval(int &intervalOut)
{
  if (m_window) {
    return (bool)m_window->getSwapInterval(intervalOut);
  }

  return false;
}

void GPG_Canvas::GetDisplayDimensions(blender::int2 &scr_size)
{
  unsigned int uiwidth;
  unsigned int uiheight;

  GHOST_ISystem *system = GHOST_ISystem::getSystem();
  system->getMainDisplayDimensions(uiwidth, uiheight);

  scr_size[0] = uiwidth;
  scr_size[1] = uiheight;
}

void GPG_Canvas::ResizeWindow(int width, int height)
{
  if (m_window->getState() == GHOST_kWindowStateFullScreen) {
    GHOST_ISystem *system = GHOST_ISystem::getSystem();
    GHOST_DisplaySetting setting;
    setting.xPixels = width;
    setting.yPixels = height;
    // XXX allow these to be changed or kept from previous state
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

void GPG_Canvas::ConvertMousePosition(int x, int y, int &r_x, int &r_y, bool /*screen*/)
{
  GHOST_ISystem *system = GHOST_ISystem::getSystem();
  if (system && m_window) {
    m_window->screenToClient(x, y, r_x, r_y);
    r_x *= m_nativePixelSize;
    r_y *= m_nativePixelSize;
  }
}

bool GPG_Canvas::IsBlenderPlayer()
{
  return true;
}
