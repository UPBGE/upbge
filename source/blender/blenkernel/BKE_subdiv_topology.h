/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2019 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct Subdiv;

int BKE_subdiv_topology_num_fvar_layers_get(const struct Subdiv *subdiv);

#ifdef __cplusplus
}
#endif
