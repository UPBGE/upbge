/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#include "blender/device.h"
#include "blender/session.h"
#include "blender/util.h"

#include "util/foreach.h"

CCL_NAMESPACE_BEGIN

enum ComputeDevice {
  COMPUTE_DEVICE_CPU = 0,
  COMPUTE_DEVICE_CUDA = 1,
  COMPUTE_DEVICE_OPTIX = 3,
  COMPUTE_DEVICE_HIP = 4,
  COMPUTE_DEVICE_METAL = 5,
  COMPUTE_DEVICE_ONEAPI = 6,

  COMPUTE_DEVICE_NUM
};

int blender_device_threads(BL::Scene &b_scene)
{
  BL::RenderSettings b_r = b_scene.render();

  if (b_r.threads_mode() == BL::RenderSettings::threads_mode_FIXED)
    return b_r.threads();
  else
    return 0;
}

DeviceInfo blender_device_info(BL::Preferences &b_preferences, BL::Scene &b_scene, bool background)
{
  PointerRNA cscene = RNA_pointer_get(&b_scene.ptr, "cycles");

  /* Find cycles preferences. */
  PointerRNA cpreferences;
  for (BL::Addon &b_addon : b_preferences.addons) {
    if (b_addon.module() == "cycles") {
      cpreferences = b_addon.preferences().ptr;
      break;
    }
  }

  /* Default to CPU device. */
  DeviceInfo device = Device::available_devices(DEVICE_MASK_CPU).front();

  if (BlenderSession::device_override != DEVICE_MASK_ALL) {
    vector<DeviceInfo> devices = Device::available_devices(BlenderSession::device_override);

    if (devices.empty()) {
      device = Device::dummy_device("Found no Cycles device of the specified type");
    }
    else {
      int threads = blender_device_threads(b_scene);
      device = Device::get_multi_device(devices, threads, background);
    }
  }
  else if (get_enum(cscene, "device") == 1) {
    /* Test if we are using GPU devices. */
    ComputeDevice compute_device = (ComputeDevice)get_enum(
        cpreferences, "compute_device_type", COMPUTE_DEVICE_NUM, COMPUTE_DEVICE_CPU);

    if (compute_device != COMPUTE_DEVICE_CPU) {
      /* Query GPU devices with matching types. */
      uint mask = DEVICE_MASK_CPU;
      if (compute_device == COMPUTE_DEVICE_CUDA) {
        mask |= DEVICE_MASK_CUDA;
      }
      else if (compute_device == COMPUTE_DEVICE_OPTIX) {
        mask |= DEVICE_MASK_OPTIX;
      }
      else if (compute_device == COMPUTE_DEVICE_HIP) {
        mask |= DEVICE_MASK_HIP;
      }
      else if (compute_device == COMPUTE_DEVICE_METAL) {
        mask |= DEVICE_MASK_METAL;
      }
      else if (compute_device == COMPUTE_DEVICE_ONEAPI) {
        mask |= DEVICE_MASK_ONEAPI;
      }
      vector<DeviceInfo> devices = Device::available_devices(mask);

      /* Match device preferences and available devices. */
      vector<DeviceInfo> used_devices;
      RNA_BEGIN (&cpreferences, device, "devices") {
        if (get_boolean(device, "use")) {
          string id = get_string(device, "id");
          foreach (DeviceInfo &info, devices) {
            if (info.id == id) {
              used_devices.push_back(info);
              break;
            }
          }
        }
      }
      RNA_END;

      if (!used_devices.empty()) {
        int threads = blender_device_threads(b_scene);
        device = Device::get_multi_device(used_devices, threads, background);
      }
      /* Else keep using the CPU device that was set before. */
    }
  }

  if (!get_boolean(cpreferences, "peer_memory")) {
    device.has_peer_memory = false;
  }

  if (get_boolean(cpreferences, "use_metalrt")) {
    device.use_metalrt = true;
  }

  return device;
}

CCL_NAMESPACE_END
