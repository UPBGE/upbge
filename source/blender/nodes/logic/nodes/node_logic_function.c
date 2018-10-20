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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/nodes/shader/nodes/node_logic_function.c
 *  \ingroup lognodes
 */

#include "node_logic_util.h"

static bNodeSocketTemplate node_function_in[] = {
	{ -1, 0, "" }
};

static bNodeSocketTemplate node_function_out[] = {
	{SOCK_LOGIC, 0, "Trigger Out"},
	{ -1, 0, "" }
};

void register_node_type_logic_function(void)
{
	static bNodeType ntype;

	logic_node_type_base(&ntype, LOGIC_NODE_FUNCTION, "Function", NODE_CLASS_INPUT, 0);
	node_type_socket_templates(&ntype, node_function_in, node_function_out);
	node_type_storage(&ntype, "", NULL, NULL);

	nodeRegisterType(&ntype);
}
