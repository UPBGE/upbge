/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2014 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup GHOST
 */

#pragma once

#include "GHOST_Context.h"

extern "C" {
#include "SDL.h"
}

#ifndef GHOST_OPENGL_SDL_CONTEXT_FLAGS
#  ifdef WITH_GPU_DEBUG
#    define GHOST_OPENGL_SDL_CONTEXT_FLAGS SDL_GL_CONTEXT_DEBUG_FLAG
#  else
#    define GHOST_OPENGL_SDL_CONTEXT_FLAGS 0
#  endif
#endif

#ifndef GHOST_OPENGL_SDL_RESET_NOTIFICATION_STRATEGY
#  define GHOST_OPENGL_SDL_RESET_NOTIFICATION_STRATEGY 0
#endif

class GHOST_ContextSDL : public GHOST_Context {
 public:
  /**
   * Constructor.
   */
  GHOST_ContextSDL(bool stereoVisual,
                   SDL_Window *window,
                   int contextProfileMask,
                   int contextMajorVersion,
                   int contextMinorVersion,
                   int contextFlags,
                   int contextResetNotificationStrategy);

  /**
   * Destructor.
   */
  ~GHOST_ContextSDL();

  /**
   * Swaps front and back buffers of a window.
   * \return A boolean success indicator.
   */
  GHOST_TSuccess swapBuffers();

  /**
   * Activates the drawing context of this window.
   * \return A boolean success indicator.
   */
  GHOST_TSuccess activateDrawingContext();

  /**
   * Release the drawing context of the calling thread.
   * \return A boolean success indicator.
   */
  GHOST_TSuccess releaseDrawingContext();

  /**
   * Call immediately after new to initialize.  If this fails then immediately delete the object.
   * \return Indication as to whether initialization has succeeded.
   */
  GHOST_TSuccess initializeDrawingContext();

  /**
   * Removes references to native handles from this context and then returns
   * \return GHOST_kSuccess if it is OK for the parent to release the handles and
   * GHOST_kFailure if releasing the handles will interfere with sharing
   */
  GHOST_TSuccess releaseNativeHandles();

  /**
   * Sets the swap interval for #swapBuffers.
   * \param interval: The swap interval to use.
   * \return A boolean success indicator.
   */
  GHOST_TSuccess setSwapInterval(int interval);

  /**
   * Gets the current swap interval for #swapBuffers.
   * \param intervalOut: Variable to store the swap interval if it can be read.
   * \return Whether the swap interval can be read.
   */
  GHOST_TSuccess getSwapInterval(int &intervalOut);

 private:
  SDL_Window *m_window;
  SDL_Window *m_hidden_window;

  const int m_contextProfileMask;
  const int m_contextMajorVersion;
  const int m_contextMinorVersion;
  const int m_contextFlags;
  const int m_contextResetNotificationStrategy;

  SDL_GLContext m_context; /* m_sdl_glcontext */

  /** The first created OpenGL context (for sharing display lists) */
  static SDL_GLContext s_sharedContext;
  static int s_sharedCount;
};
