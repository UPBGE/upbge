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

/** \file blender/nodes/shader/nodes/node_shader_time.c
 *  \ingroup shdnodes
 */


#include "node_shader_util.h"

/* **************** OBJECT INFO  ******************** */
static bNodeSocketTemplate sh_node_time_out[] = {
	{	SOCK_FLOAT, 1, N_("Time"),       0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, PROP_NONE},
	{	-1, 0, ""	}
};

static int gpu_shader_time(GPUMaterial *mat, bNode *UNUSED(node), bNodeExecData *UNUSED(execdata), GPUNodeStack *in, GPUNodeStack *out)
{
	GPUNodeLink *time = GPU_material_builtin(mat, GPU_TIME);

	return GPU_stack_link(mat, "set_value", in, out, time);
}

void register_node_type_sh_time(void)
{
	static bNodeType ntype;

	sh_node_type_base(&ntype, SH_NODE_TIME, "Time", NODE_CLASS_INPUT, 0);
	node_type_compatibility(&ntype, NODE_OLD_SHADING | NODE_NEW_SHADING);
	node_type_socket_templates(&ntype, NULL, sh_node_time_out);
	node_type_storage(&ntype, "", NULL, NULL);
	node_type_gpu(&ntype, gpu_shader_time);

	nodeRegisterType(&ntype);
}
