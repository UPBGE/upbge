/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2022 NVIDIA Corporation
 * Copyright 2022 Blender Foundation */

#pragma once

#include "graph/node.h"
#include "hydra/config.h"

#include <pxr/base/vt/value.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

void SetNodeValue(CCL_NS::Node *node, const CCL_NS::SocketType &socket, const VtValue &value);

VtValue GetNodeValue(const CCL_NS::Node *node, const CCL_NS::SocketType &socket);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
