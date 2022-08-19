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

#include "BKE_image.h"
#include "BKE_image_format.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "DNA_space_types.h"
#include "GHOST_ISystem.h"
#include "GPU_framebuffer.h"
#include "MEM_guardedalloc.h"

#include "KX_Globals.h"

GPG_Canvas::GPG_Canvas(RAS_Rasterizer *rasty, GHOST_IWindow *window)
    : RAS_ICanvas(rasty), m_window(window), m_width(0), m_height(0), m_nativePixelSize(1)
{
  if (m_window) {
    GHOST_Rect bnds;
    m_window->getClientBounds(bnds);
    m_nativePixelSize = window->getNativePixelSize();
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
  m_viewportArea = RAS_Rect(width * m_nativePixelSize, height * m_nativePixelSize);
  m_windowArea = RAS_Rect(width, height);
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
    GPU_clear_color(0.0f, 0.0f, 0.0f, 0.0f);
    GPU_clear_depth(1.0f);
    m_window->setDrawingContextType(GHOST_kDrawingContextTypeOpenGL);
    BLI_assert(m_window->getDrawingContextType() == GHOST_kDrawingContextTypeOpenGL);
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

void GPG_Canvas::ConvertMousePosition(int x, int y, int &r_x, int &r_y, bool UNUSED(screen))
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
