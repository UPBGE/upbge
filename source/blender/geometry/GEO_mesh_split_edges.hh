/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_index_mask.hh"

#include "BKE_attribute.hh"

struct Mesh;

namespace blender::geometry {

void split_edges(Mesh &mesh,
                 IndexMask mask,
                 const bke::AnonymousAttributePropagationInfo &propagation_info);

}  // namespace blender::geometry
