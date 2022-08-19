/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

#include "util/string.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

class Device;
class DeviceInfo;
class Profiler;
class Stats;

Device *device_cpu_create(const DeviceInfo &info, Stats &stats, Profiler &profiler);

void device_cpu_info(vector<DeviceInfo> &devices);

string device_cpu_capabilities();

CCL_NAMESPACE_END
