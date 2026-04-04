/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2024 UPBGE contributors. */

/** \file RAS_NullCanvas.h
 *  \ingroup bgerast
 *
 *  Headless (no-GPU) canvas stub.
 *  Used when Blender runs with --background so the BGE can validate
 *  scene data, physics, logic bricks and scripts without a GHOST window.
 *
 *  All GPU-touching methods are no-ops.
 *  Dimensions default to 1280x720 (overridable via env BGE_HEADLESS_W/H).
 */

#pragma once

#include "RAS_ICanvas.h"

class RAS_NullCanvas : public RAS_ICanvas {
 public:
  /** Default headless resolution. Can be overridden at construction. */
  RAS_NullCanvas(RAS_Rasterizer *rasty, int width = 1280, int height = 720);
  virtual ~RAS_NullCanvas() = default;

  /* --- Lifecycle --- */
  void Init() override;
  void BeginFrame() override {}
  void EndFrame() override {}
  void BeginDraw() override {}
  void EndDraw() override {}

  /* --- Buffer / swap --- */
  void SwapBuffers() override {}
  void SetSwapInterval(int /*interval*/) override {}
  bool GetSwapInterval(int &intervalOut) override
  {
    intervalOut = 0;
    return true;
  }

  /* --- Mouse (no window, all no-ops) --- */
  void SetMouseState(RAS_MouseState /*mousestate*/) override {}
  void SetMousePosition(int /*x*/, int /*y*/) override {}
  void ConvertMousePosition(int x, int y, int &r_x, int &r_y, bool /*screen*/) override
  {
    r_x = x;
    r_y = y;
  }

  /* --- Display / resize --- */
  void GetDisplayDimensions(blender::int2 &scr_size) override;
  void ResizeWindow(int width, int height) override;
  void Resize(int width, int height) override;
  void SetFullScreen(bool /*enable*/) override {}
  bool GetFullScreen() override { return false; }

  /* --- Screenshot (silently ignored in headless mode) --- */
  void MakeScreenShot(const std::string & /*filename*/) override {}
};
