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

/** Whether a link from an output socket of type `from` into an input socket of type `to` (two
 * different types) converts by itself, as in Geometry Nodes (2026-11-16): true/false and numbers
 * into each other and into a vector; a vector into true/false, a rotation or a color; a rotation
 * into a vector. The engine's LN_ImplicitlyConvertible says the same for its value types. Flow
 * sockets are boolean sockets: callers that see sockets keep them out. */
bool link_types_convert(eNodeSocketDatatype from, eNodeSocketDatatype to);

}  // namespace nodes::logic
}  // namespace blender
