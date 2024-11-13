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

#pragma once

#ifdef WIN32
#  include <windows.h>
#endif

#include "RAS_ICanvas.h"
#include "RAS_Rect.h"

struct ARegion;
struct rcti;
struct wmWindow;
struct wmWindowManager;

/**
 * 2D Blender device context abstraction.
 * The connection from 3d rendercontext to 2d Blender surface embedding.
 */

class KX_BlenderCanvas : public RAS_ICanvas {
 private:
  int m_viewport[4];

  wmWindowManager *m_wm;
  wmWindow *m_win;
  RAS_Rect m_area_rect;
  ARegion *m_ar;

  bool m_useViewportRender;

 public:
  /* Construct a new canvas.
   *
   * \param area The Blender ARegion to run the game within.
   */
  KX_BlenderCanvas(
      RAS_Rasterizer *rasty, wmWindowManager *wm, wmWindow *win, rcti *viewport, ARegion *ar, bool useViewportRender);
  virtual ~KX_BlenderCanvas();

  virtual void Init();

  virtual void SwapBuffers();
  virtual void SetSwapInterval(int interval);
  virtual bool GetSwapInterval(int &intervalOut);

  virtual void GetDisplayDimensions(blender::int2 &scr_size);
  virtual void ResizeWindow(int width, int height);
  virtual void Resize(int width, int height);

  virtual void SetFullScreen(bool enable);
  virtual bool GetFullScreen();

  virtual void BeginFrame();
  virtual void EndFrame();

  virtual void ConvertMousePosition(int x, int y, int &r_x, int &r_y, bool screen);

  virtual void SetMouseState(RAS_MouseState mousestate);
  virtual void SetMousePosition(int x, int y);

  virtual void MakeScreenShot(const std::string &filename);

  virtual void BeginDraw();
  virtual void EndDraw();

  virtual bool IsBlenderPlayer();
};
