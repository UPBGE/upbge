/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2024 UPBGE contributors. */

/** \file gameengine/Rasterizer/RAS_NullCanvas.cpp
 *  \ingroup bgerast
 */

#include "RAS_NullCanvas.h"

#include "BLI_math_vector_types.hh"
#include "RAS_Rect.h"

#include <cstdlib>  /* std::getenv */
#include <cstring>  /* std::atoi    */

/* Read optional env overrides for headless resolution. */
static int env_int(const char *name, int fallback)
{
  const char *v = std::getenv(name);
  return (v && v[0]) ? std::atoi(v) : fallback;
}

RAS_NullCanvas::RAS_NullCanvas(RAS_Rasterizer *rasty, int width, int height)
    : RAS_ICanvas(rasty)
{
  /* Allow override via environment, e.g. for CI with non-default resolution. */
  const int w = env_int("BGE_HEADLESS_W", width);
  const int h = env_int("BGE_HEADLESS_H", height);

  m_windowArea.SetLeft(0);
  m_windowArea.SetBottom(0);
  m_windowArea.SetRight(w);
  m_windowArea.SetTop(h);

  m_viewportArea = m_windowArea;
}

void RAS_NullCanvas::Init()
{
  /* Nothing to initialise without a GPU context. */
}

void RAS_NullCanvas::GetDisplayDimensions(blender::int2 &scr_size)
{
  scr_size[0] = m_windowArea.GetRight();
  scr_size[1] = m_windowArea.GetTop();
}

void RAS_NullCanvas::ResizeWindow(int width, int height)
{
  Resize(width, height);
}

void RAS_NullCanvas::Resize(int width, int height)
{
  m_windowArea.SetRight(width);
  m_windowArea.SetTop(height);
  m_viewportArea = m_windowArea;
}
