/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup gpu
 *
 * Wrap OpenGL features such as textures, shaders and GLSL
 * with checks for drivers and GPU support.
 */

#include "MEM_guardedalloc.h"

#include "BLI_dynstr.h"
#include "BLI_string.h"

#include "GPU_platform.h"

#include "gpu_platform_private.hh"

/* -------------------------------------------------------------------- */
/** \name GPUPlatformGlobal
 * \{ */

namespace blender::gpu {

GPUPlatformGlobal GPG;

static char *create_key(eGPUSupportLevel support_level,
                        const char *vendor,
                        const char *renderer,
                        const char *version)
{
  DynStr *ds = BLI_dynstr_new();
  BLI_dynstr_appendf(ds, "{%s/%s/%s}=", vendor, renderer, version);
  if (support_level == GPU_SUPPORT_LEVEL_SUPPORTED) {
    BLI_dynstr_append(ds, "SUPPORTED");
  }
  else if (support_level == GPU_SUPPORT_LEVEL_LIMITED) {
    BLI_dynstr_append(ds, "LIMITED");
  }
  else {
    BLI_dynstr_append(ds, "UNSUPPORTED");
  }

  char *support_key = BLI_dynstr_get_cstring(ds);
  BLI_dynstr_free(ds);
  BLI_str_replace_char(support_key, '\n', ' ');
  BLI_str_replace_char(support_key, '\r', ' ');
  return support_key;
}

static char *create_gpu_name(const char *vendor, const char *renderer, const char *version)
{
  DynStr *ds = BLI_dynstr_new();
  BLI_dynstr_appendf(ds, "%s %s %s", vendor, renderer, version);

  char *gpu_name = BLI_dynstr_get_cstring(ds);
  BLI_dynstr_free(ds);
  BLI_str_replace_char(gpu_name, '\n', ' ');
  BLI_str_replace_char(gpu_name, '\r', ' ');
  return gpu_name;
}

void GPUPlatformGlobal::init(eGPUDeviceType gpu_device,
                             eGPUOSType os_type,
                             eGPUDriverType driver_type,
                             eGPUSupportLevel gpu_support_level,
                             eGPUBackendType backend,
                             const char *vendor_str,
                             const char *renderer_str,
                             const char *version_str)
{
  this->clear();

  this->initialized = true;

  this->device = gpu_device;
  this->os = os_type;
  this->driver = driver_type;
  this->support_level = gpu_support_level;

  const char *vendor = vendor_str ? vendor_str : "UNKNOWN";
  const char *renderer = renderer_str ? renderer_str : "UNKNOWN";
  const char *version = version_str ? version_str : "UNKNOWN";

  this->vendor = BLI_strdup(vendor);
  this->renderer = BLI_strdup(renderer);
  this->version = BLI_strdup(version);
  this->support_key = create_key(gpu_support_level, vendor, renderer, version);
  this->gpu_name = create_gpu_name(vendor, renderer, version);
  this->backend = backend;
}

void GPUPlatformGlobal::clear()
{
  MEM_SAFE_FREE(vendor);
  MEM_SAFE_FREE(renderer);
  MEM_SAFE_FREE(version);
  MEM_SAFE_FREE(support_key);
  MEM_SAFE_FREE(gpu_name);
  initialized = false;
}

}  // namespace blender::gpu

/** \} */

/* -------------------------------------------------------------------- */
/** \name C-API
 * \{ */

using namespace blender::gpu;

eGPUSupportLevel GPU_platform_support_level()
{
  BLI_assert(GPG.initialized);
  return GPG.support_level;
}

const char *GPU_platform_vendor()
{
  BLI_assert(GPG.initialized);
  return GPG.vendor;
}

const char *GPU_platform_renderer()
{
  BLI_assert(GPG.initialized);
  return GPG.renderer;
}

const char *GPU_platform_version()
{
  BLI_assert(GPG.initialized);
  return GPG.version;
}

const char *GPU_platform_support_level_key()
{
  BLI_assert(GPG.initialized);
  return GPG.support_key;
}

const char *GPU_platform_gpu_name()
{
  BLI_assert(GPG.initialized);
  return GPG.gpu_name;
}

bool GPU_type_matches(eGPUDeviceType device, eGPUOSType os, eGPUDriverType driver)
{
  return GPU_type_matches_ex(device, os, driver, GPU_BACKEND_ANY);
}

bool GPU_type_matches_ex(eGPUDeviceType device,
                         eGPUOSType os,
                         eGPUDriverType driver,
                         eGPUBackendType backend)
{
  BLI_assert(GPG.initialized);
  return (GPG.device & device) && (GPG.os & os) && (GPG.driver & driver) &&
         (GPG.backend & backend);
}

/** \} */
