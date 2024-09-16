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

/** \file RAS_ICanvas.h
 *  \ingroup bgerast
 */

#pragma once

#include "RAS_Rasterizer.h"

#include "BLI_math_vector_types.hh"

class RAS_Rect;

struct ImageFormatData;
struct TaskPool;

/**
 * 2D rendering device context. The connection from 3d rendercontext to 2d surface.
 */
class RAS_ICanvas {
 public:
  enum RAS_MouseState { MOUSE_INVISIBLE = 1, MOUSE_WAIT, MOUSE_NORMAL };

  RAS_ICanvas(RAS_Rasterizer *rasty);
  virtual ~RAS_ICanvas();

  virtual void Init() = 0;

  virtual void BeginFrame() = 0;
  virtual void EndFrame() = 0;

  /**
   * Initializes the canvas for drawing.  Drawing to the canvas is
   * only allowed between BeginDraw() and EndDraw().
   *
   * \retval false Acquiring the canvas failed.
   * \retval true Acquiring the canvas succeeded.
   *
   */
  virtual void BeginDraw() = 0;

  /**
   * Unitializes the canvas for drawing.
   */
  virtual void EndDraw() = 0;

  virtual bool IsBlenderPlayer() = 0;

  /// probably needs some arguments for PS2 in future
  virtual void SwapBuffers() = 0;
  virtual void SetSwapInterval(int interval) = 0;
  virtual bool GetSwapInterval(int &intervalOut) = 0;

  void SetSamples(int samples);
  int GetSamples() const;

  int GetWidth() const;
  int GetHeight() const;

  /** Convert mouse coordinates from screen or client window to render area coordinates.
   * \param x The input X coordinate.
   * \param y The input Y coordinate.
   * \param r_x The mouse render area X coordinate.
   * \param r_y The mouse render area Y coordinate.
   * \param screen Set to true when the input coordinates come from the screen and not the client
   * window.
   */
  virtual void ConvertMousePosition(int x, int y, int &r_x, int &r_y, bool screen) = 0;

  float GetMouseNormalizedX(int x);
  float GetMouseNormalizedY(int y);

  /**
   * Used to get canvas area within blender.
   */
  const RAS_Rect &GetWindowArea() const;

  const RAS_Rect &GetViewportArea() const;

  virtual void SetMouseState(RAS_MouseState mousestate) = 0;
  virtual void SetMousePosition(int x, int y) = 0;

  virtual RAS_MouseState GetMouseState()
  {
    return m_mousestate;
  }

  virtual void MakeScreenShot(const std::string &filename) = 0;
  /// Proceed the actual screenshot at the frame end.
  void FlushScreenshots();

  virtual void GetDisplayDimensions(blender::int2 &scr_size) = 0;

  virtual void ResizeWindow(int width, int height) = 0;

  /// Resize the canvas without resizing the window.
  virtual void Resize(int width, int height) = 0;

  virtual void SetFullScreen(bool enable) = 0;
  virtual bool GetFullScreen() = 0;

 protected:
  RAS_Rasterizer *m_rasterizer;

  struct Screenshot {
    std::string path;
    int x;
    int y;
    int width;
    int height;
    ImageFormatData *format;
  };

  std::vector<Screenshot> m_screenshots;

  int m_samples;

  RAS_MouseState m_mousestate;
  /// frame number for screenshots.
  int m_frame;
  TaskPool *m_taskpool;

  RAS_Rect m_windowArea;
  RAS_Rect m_viewportArea;

  /** Delay the screenshot to the frame end to use a valid buffer and avoid copy from an invalid
   * buffer at the frame begin after the buffer swap. The screenshot are proceeded in \see
   * FlushScreenshots.
   */
  void AddScreenshot(
      const std::string &path, int x, int y, int width, int height, ImageFormatData *format);

  /**
   * Saves screenshot data to a file. The actual compression and disk I/O is performed in
   * a separate thread.
   */
  void SaveScreeshot(const Screenshot &screenshot);
};
