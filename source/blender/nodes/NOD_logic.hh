/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

#include "BKE_node.hh"

#include "NOD_logic_descriptors.hh"

namespace blender {

extern bke::bNodeTreeType *ntreeType_Logic;

void register_node_tree_type_logic();

namespace nodes::logic {

Span<NodeMetadata> node_metadata();
const NodeMetadata *node_metadata(StringRef idname);

}  // namespace nodes::logic
}  // namespace blender
