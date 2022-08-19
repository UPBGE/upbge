/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BKE_global.h"
#include "CLG_log.h"

namespace blender {
namespace gpu {
namespace debug {

extern CLG_LogRef LOG;

/* Initialize debugging. */
void mtl_debug_init();

/* Using Macro's instead of variadic template due to non-string-literal
 * warning for CLG_logf when indirectly passing format string. */
#define EXPAND_ARGS(...) , ##__VA_ARGS__
#define MTL_LOG_ERROR(info, ...) \
  { \
    if (G.debug & G_DEBUG_GPU) { \
      CLG_logf(debug::LOG.type, \
               CLG_SEVERITY_ERROR, \
               "[Metal Viewport Error]", \
               "", \
               info EXPAND_ARGS(__VA_ARGS__)); \
    } \
    BLI_assert(false); \
  }

#define MTL_LOG_WARNING(info, ...) \
  { \
    if (G.debug & G_DEBUG_GPU) { \
      CLG_logf(debug::LOG.type, \
               CLG_SEVERITY_WARN, \
               "[Metal Viewport Warning]", \
               "", \
               info EXPAND_ARGS(__VA_ARGS__)); \
    } \
  }

#define MTL_LOG_INFO(info, ...) \
  { \
    if (G.debug & G_DEBUG_GPU) { \
      CLG_logf(debug::LOG.type, \
               CLG_SEVERITY_INFO, \
               "[Metal Viewport Info]", \
               "", \
               info EXPAND_ARGS(__VA_ARGS__)); \
    } \
  }

}  // namespace debug
}  // namespace gpu
}  // namespace blender
