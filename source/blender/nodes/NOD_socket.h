/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup nodes
 */

#pragma once

#include "DNA_listBase.h"

#include "BLI_utildefines.h"

#include "BKE_node.h"

#include "RNA_types.h"

struct bNode;
struct bNodeTree;

#ifdef __cplusplus
extern "C" {
#endif

struct bNodeSocket *node_add_socket_from_template(struct bNodeTree *ntree,
                                                  struct bNode *node,
                                                  struct bNodeSocketTemplate *stemp,
                                                  eNodeSocketInOut in_out);

void node_verify_sockets(struct bNodeTree *ntree, struct bNode *node, bool do_id_user);

void node_socket_init_default_value(struct bNodeSocket *sock);
void node_socket_copy_default_value(struct bNodeSocket *to, const struct bNodeSocket *from);
void node_socket_skip_reroutes(struct ListBase *links,
                               struct bNode *node,
                               struct bNodeSocket *socket,
                               struct bNode **r_node,
                               struct bNodeSocket **r_socket);
void register_standard_node_socket_types(void);

#ifdef __cplusplus
}
#endif
