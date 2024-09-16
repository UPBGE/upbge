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

/** \file GPG_Canvas.h
 *  \ingroup player
 */

#pragma once

#ifdef WIN32
#  pragma warning(disable : 4786)
#endif  // WIN32

#include "GHOST_IWindow.hh"

#include "RAS_ICanvas.h"
#include "RAS_Rect.h"

class RAS_Rasterizer;

class GPG_Canvas : public RAS_ICanvas {
 protected:
  /// Our customized bge bContext
  struct bContext *m_context;
  /// GHOST window.
  GHOST_IWindow *m_window;
  /// Width of the context.
  int m_width;
  /// Height of the context.
  int m_height;
  /** Rect that defines the area used for rendering,
   * relative to the context.
   */
  RAS_Rect m_area;

  int m_viewport[4];

  bool m_useViewportRender;

  /// Native pixel size (for Retina hidpi displays)
  int m_nativePixelSize;

 public:
  GPG_Canvas(struct bContext *C, RAS_Rasterizer *rasty, GHOST_IWindow *window, bool useViewportRender);
  virtual ~GPG_Canvas();

  /**
   * \section Methods inherited from abstract base class RAS_ICanvas.
   */

  virtual void BeginFrame();

  /// Draws overlay banners and progress bars.
  virtual void EndFrame();

  virtual void MakeScreenShot(const std::string &filename);

  virtual void Init();
  virtual void SetMousePosition(int x, int y);
  virtual void SetMouseState(RAS_MouseState mousestate);
  virtual void SwapBuffers();
  virtual void SetSwapInterval(int interval);
  virtual bool GetSwapInterval(int &intervalOut);

  virtual void ConvertMousePosition(int x, int y, int &r_x, int &r_y, bool screen);

  virtual void GetDisplayDimensions(blender::int2 &scr_size);

  virtual void Resize(int width, int height);
  virtual void ResizeWindow(int width, int height);
  virtual void SetFullScreen(bool enable);
  virtual bool GetFullScreen();

  virtual void BeginDraw();
  virtual void EndDraw();

  virtual bool IsBlenderPlayer();
};
