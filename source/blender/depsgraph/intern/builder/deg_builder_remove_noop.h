/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup depsgraph
 */

#pragma once

namespace blender::deg {

struct Depsgraph;

/* Remove all no-op nodes that have zero outgoing relations. */
void deg_graph_remove_unused_noops(Depsgraph *graph);

}  // namespace blender::deg
