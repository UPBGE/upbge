/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup depsgraph
 */

#pragma once

namespace blender::deg {

struct Depsgraph;

/* Aggregate operation timings to overall component and ID nodes timing. */
void deg_eval_stats_aggregate(Depsgraph *graph);

}  // namespace blender::deg
