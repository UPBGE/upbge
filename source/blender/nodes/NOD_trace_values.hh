/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_compute_context_cache_fwd.hh"

#include "BLI_compute_context.hh"

#include "DNA_node_types.h"

#include "NOD_geometry_nodes_bundle_signature.hh"
#include "NOD_geometry_nodes_closure_location.hh"
#include "NOD_geometry_nodes_closure_signature.hh"

namespace blender::nodes {

/**
 * Attempts to find a compute context that the closure is evaluated in. If none is found, null is
 * returned. If multiple are found, it currently picks the first one it finds which is somewhat
 * arbitrary.
 */
[[nodiscard]] const ComputeContext *compute_context_for_closure_evaluation(
    const ComputeContext *closure_socket_context,
    const bNodeSocket &closure_socket,
    bke::ComputeContextCache &compute_context_cache,
    const std::optional<ClosureSourceLocation> &source_location);

Vector<BundleSignature> gather_linked_target_bundle_signatures(
    const ComputeContext *bundle_socket_context,
    const bNodeSocket &bundle_socket,
    bke::ComputeContextCache &compute_context_cache);
Vector<BundleSignature> gather_linked_origin_bundle_signatures(
    const ComputeContext *bundle_socket_context,
    const bNodeSocket &bundle_socket,
    bke::ComputeContextCache &compute_context_cache);
Vector<ClosureSignature> gather_linked_target_closure_signatures(
    const ComputeContext *closure_socket_context,
    const bNodeSocket &closure_socket,
    bke::ComputeContextCache &compute_context_cache);
Vector<ClosureSignature> gather_linked_origin_closure_signatures(
    const ComputeContext *closure_socket_context,
    const bNodeSocket &closure_socket,
    bke::ComputeContextCache &compute_context_cache);

}  // namespace blender::nodes
