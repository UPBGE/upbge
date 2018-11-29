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

/** \file blender/nodes/shader/nodes/node_shader_parallax.c
 *  \ingroup shdnodes
 */


#include "node_shader_util.h"

/* **************** OBJECT INFO  ******************** */
static bNodeSocketTemplate sh_node_mapping_in[] = {
	{	SOCK_VECTOR, 1, N_("UV")},
	{   SOCK_FLOAT, 1, N_("Steps"), 10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1000.0f, PROP_NONE, 0 },
	{   SOCK_FLOAT, 1, N_("Bump Scale"), 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 1000.0f, PROP_NONE, 0 },
	{	-1, 0, ""	}
};

static bNodeSocketTemplate sh_node_parallax_out[] = {
	{	SOCK_VECTOR, 0, N_("UV")},
	{	-1, 0, ""	}
};

static int gpu_shader_parallax(GPUMaterial *mat, bNode *node, bNodeExecData *UNUSED(execdata), GPUNodeStack *in, GPUNodeStack *out)
{
	Tex *tex = (Tex *)node->id;

	if (tex && tex->ima && (tex->type == TEX_IMAGE)) {
		GPUNodeLink *texlink = GPU_image(tex->ima, &tex->iuser, false);
		GPUNodeLink *texco;
		GPUNodeLink *norm;
		GPUNodeLink *outuv;
		float one[3] = { 1.0f, 1.0f, 1.0f };

		for (unsigned short i = 0; i < 3; ++i) {
			if (!in[i].link) {
				in[i].link = GPU_uniform(in[i].vec);
			}
		}

		GPU_link(mat, "texco_norm", GPU_material_builtin(mat, GPU_VIEW_NORMAL), &norm);
		GPU_link(mat, "mtex_2d_mapping", in[0].link, &texco);

		float comp = (float) node->custom1;
		float discard = (float) node->custom2;
		GPU_link(mat, "mtex_parallax", texco, GPU_material_builtin(mat, GPU_VIEW_POSITION), GPU_attribute(CD_TANGENT, ""), norm, texlink,
			in[1].link, in[2].link, GPU_uniform(one), GPU_uniform(&discard), GPU_uniform(&comp), &outuv);

		GPU_link(mat, "parallax_uv_attribute", outuv, &out[0].link);

		return true;
	}

	return false;
}

void register_node_type_sh_parallax(void)
{
	static bNodeType ntype;

	sh_node_type_base(&ntype, SH_NODE_PARALLAX, "Parallax", NODE_CLASS_INPUT, 0);
	node_type_compatibility(&ntype, NODE_OLD_SHADING | NODE_NEW_SHADING);
	node_type_socket_templates(&ntype, sh_node_mapping_in, sh_node_parallax_out);
	node_type_label(&ntype, node_parallax_label);
	node_type_gpu(&ntype, gpu_shader_parallax);

	nodeRegisterType(&ntype);
}
