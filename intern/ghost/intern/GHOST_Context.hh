/* SPDX-FileCopyrightText: 2013 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_Context class.
 */

#pragma once

#include "GHOST_IContext.hh"
#include "GHOST_Types.h"

#include <cstdlib>  // for nullptr

class GHOST_Context : public GHOST_IContext {
 protected:
  static thread_local inline GHOST_Context *active_context_;

 public:
  /**
   * Constructor.
   * \param stereoVisual: Stereo visual for quad buffered stereo.
   */
  GHOST_Context(bool stereoVisual) : m_stereoVisual(stereoVisual) {}

  /**
   * Destructor.
   */
  ~GHOST_Context() override
  {
    if (active_context_ == this) {
      active_context_ = nullptr;
    }
  };

  /**
   * Returns the thread's currently active drawing context.
   */
  static inline GHOST_Context *getActiveDrawingContext()
  {
    return active_context_;
  }

  /** \copydoc #GHOST_IContext::swapBuffers */
  GHOST_TSuccess swapBuffers() override = 0;

  /** \copydoc #GHOST_IContext::activateDrawingContext */
  GHOST_TSuccess activateDrawingContext() override = 0;

  /** \copydoc #GHOST_IContext::releaseDrawingContext */
  GHOST_TSuccess releaseDrawingContext() override = 0;

  /**
   * Call immediately after new to initialize.  If this fails then immediately delete the object.
   * \return Indication as to whether initialization has succeeded.
   */
  virtual GHOST_TSuccess initializeDrawingContext() = 0;

  /**
   * Updates the drawing context of this window. Needed
   * whenever the window is changed.
   * \return Indication of success.
   */
  virtual GHOST_TSuccess updateDrawingContext()
  {
    return GHOST_kFailure;
  }

  /**
   * Checks if it is OK for a remove the native display
   * \return Indication as to whether removal has succeeded.
   */
  virtual GHOST_TSuccess releaseNativeHandles() = 0;

  /**
   * Sets the swap interval for #swapBuffers.
   * \param interval: The swap interval to use.
   * \return A boolean success indicator.
   */
  virtual GHOST_TSuccess setSwapInterval(int /*interval*/)
  {
    return GHOST_kFailure;
  }

  /**
   * Gets the current swap interval for #swapBuffers.
   * \param intervalOut: Variable to store the swap interval if it can be read.
   * \return Whether the swap interval can be read.
   */
  virtual GHOST_TSuccess getSwapInterval(int & /*interval*/)
  {
    return GHOST_kFailure;
  }

  /**
   * Get user data.
   */
  void *getUserData()
  {
    return m_user_data;
  }

  /**
   * Set user data (intended for the caller to use as needed).
   */
  void setUserData(void *user_data)
  {
    m_user_data = user_data;
  }

  /**
   * Stereo visual created. Only necessary for 'real' stereo support,
   * ie quad buffered stereo. This is not always possible, depends on
   * the graphics h/w
   */
  bool isStereoVisual() const
  {
    return m_stereoVisual;
  }

  /**
   * Returns if the context is rendered upside down compared to OpenGL.
   */
  virtual bool isUpsideDown() const
  {
    return false;
  }

  /** \copydoc #GHOST_IContext::getDefaultFramebuffer */
  unsigned int getDefaultFramebuffer() override
  {
    return 0;
  }

#ifdef WITH_VULKAN_BACKEND
  /** \copydoc #GHOST_IContext::getVulkanHandles */
  virtual GHOST_TSuccess getVulkanHandles(GHOST_VulkanHandles & /* r_handles */) override
  {
    return GHOST_kFailure;
  };
  /** \copydoc #GHOST_IContext::getVulkanSwapChainFormat */
  virtual GHOST_TSuccess getVulkanSwapChainFormat(
      GHOST_VulkanSwapChainData * /*r_swap_chain_data*/) override
  {
    return GHOST_kFailure;
  }

  /** \copydoc #GHOST_IContext::setVulkanSwapBuffersCallbacks */
  virtual GHOST_TSuccess setVulkanSwapBuffersCallbacks(
      std::function<void(const GHOST_VulkanSwapChainData *)> /*swap_buffers_pre_callback*/,
      std::function<void(void)> /*swap_buffers_post_callback*/,
      std::function<void(GHOST_VulkanOpenXRData *)> /*openxr_acquire_framebuffer_image_callback*/,
      std::function<void(GHOST_VulkanOpenXRData *)> /*openxr_release_framebuffer_image_callback*/)
      override
  {
    return GHOST_kFailure;
  }
#endif

 protected:
  bool m_stereoVisual;

  /** Caller specified, not for internal use. */
  void *m_user_data = nullptr;

#ifdef WITH_OPENGL_BACKEND
  static void initClearGL();
#endif

  /** For performance measurements with VSync disabled. */
  static const char *getEnvVarVSyncString();

  MEM_CXX_CLASS_ALLOC_FUNCS("GHOST:GHOST_Context")
};

#ifdef _WIN32
bool win32_chk(bool result, const char *file = nullptr, int line = 0, const char *text = nullptr);
bool win32_silent_chk(bool result);

#  ifndef NDEBUG
#    define WIN32_CHK(x) win32_chk((x), __FILE__, __LINE__, #x)
#  else
#    define WIN32_CHK(x) win32_chk(x)
#  endif

#  define WIN32_CHK_SILENT(x, silent) ((silent) ? win32_silent_chk(x) : WIN32_CHK(x))
#endif /* _WIN32 */
