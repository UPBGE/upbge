/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2014 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup GHOST
 *
 * Definition of GHOST_ContextSDL class.
 */

#include "GHOST_ContextSDL.h"

#include <vector>

#include <cassert>
#include <cstdio>
#include <cstring>

SDL_GLContext GHOST_ContextSDL::s_sharedContext = nullptr;
int GHOST_ContextSDL::s_sharedCount = 0;

GHOST_ContextSDL::GHOST_ContextSDL(bool stereoVisual,
                                   SDL_Window *window,
                                   int contextProfileMask,
                                   int contextMajorVersion,
                                   int contextMinorVersion,
                                   int contextFlags,
                                   int contextResetNotificationStrategy)
    : GHOST_Context(stereoVisual),
      m_window(window),
      m_hidden_window(nullptr),
      m_contextProfileMask(contextProfileMask),
      m_contextMajorVersion(contextMajorVersion),
      m_contextMinorVersion(contextMinorVersion),
      m_contextFlags(contextFlags),
      m_contextResetNotificationStrategy(contextResetNotificationStrategy),
      m_context(nullptr)
{
  // assert(m_window  != nullptr);
}

GHOST_ContextSDL::~GHOST_ContextSDL()
{
  if (m_context == nullptr) {
    return;
  }

  if (m_window != nullptr && m_context == SDL_GL_GetCurrentContext()) {
    SDL_GL_MakeCurrent(m_window, nullptr);
  }
  if (m_context != s_sharedContext || s_sharedCount == 1) {
    assert(s_sharedCount > 0);

    s_sharedCount--;

    if (s_sharedCount == 0) {
      s_sharedContext = nullptr;
    }
    SDL_GL_DeleteContext(m_context);
  }

  if (m_hidden_window != nullptr) {
    SDL_DestroyWindow(m_hidden_window);
  }
}

GHOST_TSuccess GHOST_ContextSDL::swapBuffers()
{
  SDL_GL_SwapWindow(m_window);

  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextSDL::activateDrawingContext()
{
  if (m_context == nullptr) {
    return GHOST_kFailure;
  }
  return SDL_GL_MakeCurrent(m_window, m_context) ? GHOST_kSuccess : GHOST_kFailure;
}

GHOST_TSuccess GHOST_ContextSDL::releaseDrawingContext()
{
  if (m_context == nullptr) {
    return GHOST_kFailure;
  }
  /* Untested, may not work. */
  return SDL_GL_MakeCurrent(nullptr, nullptr) ? GHOST_kSuccess : GHOST_kFailure;
}

GHOST_TSuccess GHOST_ContextSDL::initializeDrawingContext()
{
#ifdef GHOST_OPENGL_ALPHA
  const bool needAlpha = true;
#else
  const bool needAlpha = false;
#endif

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, m_contextProfileMask);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, m_contextMajorVersion);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, m_contextMinorVersion);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, m_contextFlags);

  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

  if (needAlpha) {
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  }

  if (m_stereoVisual) {
    SDL_GL_SetAttribute(SDL_GL_STEREO, 1);
  }

  if (m_window == nullptr) {
    m_hidden_window = SDL_CreateWindow("Offscreen Context Windows",
                                       SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED,
                                       1,
                                       1,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS |
                                           SDL_WINDOW_HIDDEN);

    m_window = m_hidden_window;
  }

  m_context = SDL_GL_CreateContext(m_window);

  GHOST_TSuccess success;

  if (m_context != nullptr) {
    if (!s_sharedContext) {
      s_sharedContext = m_context;
    }
    s_sharedCount++;

    success = (SDL_GL_MakeCurrent(m_window, m_context) < 0) ? GHOST_kFailure : GHOST_kSuccess;

    initClearGL();
    SDL_GL_SwapWindow(m_window);

    success = GHOST_kSuccess;
  }
  else {
    success = GHOST_kFailure;
  }

  return success;
}

GHOST_TSuccess GHOST_ContextSDL::releaseNativeHandles()
{
  m_window = nullptr;

  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextSDL::setSwapInterval(int interval)
{
  if (SDL_GL_SetSwapInterval(interval) == -1) {
    return GHOST_kFailure;
  }
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextSDL::getSwapInterval(int &intervalOut)
{
  intervalOut = SDL_GL_GetSwapInterval();
  return GHOST_kSuccess;
}
