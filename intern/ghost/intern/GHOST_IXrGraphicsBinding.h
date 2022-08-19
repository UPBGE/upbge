/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "GHOST_Xr_openxr_includes.h"

class GHOST_IXrGraphicsBinding {
 public:
  union {
#if defined(WITH_GHOST_X11)
    XrGraphicsBindingEGLMNDX egl;
    XrGraphicsBindingOpenGLXlibKHR glx;
#elif defined(WIN32)
    XrGraphicsBindingOpenGLWin32KHR wgl;
    XrGraphicsBindingD3D11KHR d3d11;
#endif
#if defined(WITH_GHOST_WAYLAND)
    XrGraphicsBindingOpenGLWaylandKHR wl;
#endif
  } oxr_binding;

  virtual ~GHOST_IXrGraphicsBinding() = default;

  /**
   * Does __not__ require this object is initialized (can be called prior to
   * #initFromGhostContext). It's actually meant to be called first.
   *
   * \param r_requirement_info: Return argument to retrieve an informal string on the requirements.
   * to be met. Useful for error/debug messages.
   */
  virtual bool checkVersionRequirements(class GHOST_Context &ghost_ctx,
                                        XrInstance instance,
                                        XrSystemId system_id,
                                        std::string *r_requirement_info) const = 0;
  virtual void initFromGhostContext(class GHOST_Context &ghost_ctx) = 0;
  virtual std::optional<int64_t> chooseSwapchainFormat(const std::vector<int64_t> &runtime_formats,
                                                       GHOST_TXrSwapchainFormat &r_format,
                                                       bool &r_is_rgb_format) const = 0;
  virtual std::vector<XrSwapchainImageBaseHeader *> createSwapchainImages(
      uint32_t image_count) = 0;
  virtual void submitToSwapchainImage(XrSwapchainImageBaseHeader &swapchain_image,
                                      const GHOST_XrDrawViewInfo &draw_info) = 0;
  virtual bool needsUpsideDownDrawing(GHOST_Context &ghost_ctx) const = 0;

 protected:
  /* Use GHOST_XrGraphicsBindingCreateFromType! */
  GHOST_IXrGraphicsBinding() = default;
};

std::unique_ptr<GHOST_IXrGraphicsBinding> GHOST_XrGraphicsBindingCreateFromType(
    GHOST_TXrGraphicsBinding type, GHOST_Context &ghost_ctx);
