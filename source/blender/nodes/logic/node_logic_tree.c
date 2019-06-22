/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version. 
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2007 Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s):
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/nodes/logic/node_logic_tree.c
 *  \ingroup nodes
 */

#include <string.h>
#include <stdio.h>

#include "BKE_context.h"
#include "BKE_node.h"

#include "DNA_object_types.h"

#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "node_common.h"
#include "node_util.h"

#include "RNA_access.h"

#include "NOD_logic.h"
#include "node_logic_util.h"

static void logic_get_from_context(const bContext *C, bNodeTreeType *UNUSED(treetype), bNodeTree **r_ntree, ID **r_id, ID **r_from)
{
	Object *object = CTX_data_active_object(C);
	
	*r_from = NULL;
	*r_id = &object->id;
	*r_ntree = object->logicNodeTree;
}

static void foreach_nodeclass(Scene *UNUSED(scene), void *calldata, bNodeClassCallback func)
{
	func(calldata, NODE_CLASS_INPUT, N_("Input"));
	func(calldata, NODE_CLASS_OUTPUT, N_("Output"));
	func(calldata, NODE_CLASS_OP_COLOR, N_("Color"));
	func(calldata, NODE_CLASS_OP_VECTOR, N_("Vector"));
	func(calldata, NODE_CLASS_OP_FILTER, N_("Filter"));
	func(calldata, NODE_CLASS_CONVERTOR, N_("Convertor"));
	func(calldata, NODE_CLASS_MATTE, N_("Matte"));
	func(calldata, NODE_CLASS_DISTORT, N_("Distort"));
	func(calldata, NODE_CLASS_GROUP, N_("Group"));
	func(calldata, NODE_CLASS_INTERFACE, N_("Interface"));
	func(calldata, NODE_CLASS_LAYOUT, N_("Layout"));
}

static void free_node_cache(bNodeTree *UNUSED(ntree), bNode *node)
{
	bNodeSocket *sock;
	
	for (sock = node->outputs.first; sock; sock = sock->next) {
		if (sock->cache) {
			sock->cache = NULL;
		}
	}
}

static void free_cache(bNodeTree *ntree)
{
	bNode *node;
	for (node = ntree->nodes.first; node; node = node->next)
		free_node_cache(ntree, node);
}

/* local tree then owns all compbufs */
static void localize(bNodeTree *UNUSED(localtree), bNodeTree *ntree)
{
	bNode *node;
	bNodeSocket *sock;

	for (node = ntree->nodes.first; node; node = node->next) {
		/* ensure new user input gets handled ok */
		node->need_exec = 0;
		node->new_node->original = node;

		for (sock = node->outputs.first; sock; sock = sock->next) {
			sock->new_sock->cache = sock->cache;
			sock->cache = NULL;
			sock->new_sock->new_sock = sock;
		}
	}
}

static void local_sync(bNodeTree *localtree, bNodeTree *ntree)
{
	BKE_node_preview_sync_tree(ntree, localtree);
}

static void local_merge(bNodeTree *localtree, bNodeTree *ntree)
{
	bNode *lnode;
	bNodeSocket *lsock;
	
	/* move over the compbufs and previews */
	BKE_node_preview_merge_tree(ntree, localtree, true);
	
	for (lnode = localtree->nodes.first; lnode; lnode = lnode->next) {
		if (ntreeNodeExists(ntree, lnode->new_node)) {
			for (lsock = lnode->outputs.first; lsock; lsock = lsock->next) {
				if (ntreeOutputExists(lnode->new_node, lsock->new_sock)) {
					lsock->new_sock->cache = lsock->cache;
					lsock->cache = NULL;
					lsock->new_sock = NULL;
				}
			}
		}
	}
}

static void update(bNodeTree *ntree)
{
	ntreeSetOutput(ntree);

	ntree_update_reroute_nodes(ntree);
	
	if (ntree->update & NTREE_UPDATE_NODES) {
		/* clean up preview cache, in case nodes have been removed */
		BKE_node_preview_remove_unused(ntree);
	}
}

static void logic_node_add_init(bNodeTree *UNUSED(bnodetree), bNode *bnode)
{
	/* Logic node will only show previews for input classes 
	 * by default, other will be hidden 
	 * but can be made visible with the show_preview option */
	if (bnode->typeinfo->nclass != NODE_CLASS_INPUT) {
		bnode->flag &= ~NODE_PREVIEW;
	}	
}

bNodeTreeType *ntreeType_Logic;

void register_node_tree_type_logic(void)
{
	bNodeTreeType *tt = ntreeType_Logic = MEM_callocN(sizeof(bNodeTreeType), "logic node tree type");
	
	tt->type = NTREE_LOGIC;
	strcpy(tt->idname, "LogicNodeTree");
	strcpy(tt->ui_name, "Logic");
	tt->ui_icon = 0;    /* defined in drawnode.c */
	strcpy(tt->ui_description, "Logic nodes");
	
	tt->free_cache = free_cache;
	tt->free_node_cache = free_node_cache;
	tt->foreach_nodeclass = foreach_nodeclass;
	tt->localize = localize;
	tt->local_sync = local_sync;
	tt->local_merge = local_merge;
	tt->update = update;
	tt->get_from_context = logic_get_from_context;
	tt->node_add_init = logic_node_add_init;
	
	tt->ext.srna = &RNA_LogicNodeTree;
	
	ntreeTypeAdd(tt);
}
