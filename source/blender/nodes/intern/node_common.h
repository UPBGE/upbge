/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup nodes
 */

#pragma once

#include "DNA_listBase.h"

#ifdef __cplusplus
extern "C" {
#endif

struct bNodeTree;

/** Groups display their internal tree name as label. */
void node_group_label(const struct bNodeTree *ntree,
                      const struct bNode *node,
                      char *label,
                      int maxlen);
bool node_group_poll_instance(struct bNode *node,
                              struct bNodeTree *nodetree,
                              const char **r_disabled_hint);

/**
 * Global update function for Reroute node types.
 * This depends on connected nodes, so must be done as a tree-wide update.
 */
void ntree_update_reroute_nodes(struct bNodeTree *ntree);

#ifdef __cplusplus
}
#endif
