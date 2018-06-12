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
 * The Original Code is Copyright (C) 2006 Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/nodes/logic/logic_composite_util.h
 *  \ingroup nodes
 */


#ifndef __LOGIC_COMPOSITE_UTIL_H__
#define __LOGIC_COMPOSITE_UTIL_H__

#include "DNA_node_types.h"
#include "node_util.h"

#define CMP_SCALE_MAX	12000

int logic_node_poll_default(struct bNodeType *ntype, struct bNodeTree *ntree);
void logic_node_update_default(struct bNodeTree *UNUSED(ntree), struct bNode *node);
void logic_node_type_base(struct bNodeType *ntype, int type, const char *name, short nclass, short flag);

#endif  /* __LOGIC_COMPOSITE_UTIL_H__ */

