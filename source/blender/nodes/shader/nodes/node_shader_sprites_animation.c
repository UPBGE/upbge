/*
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
 */

#include "../node_shader_util.h"

/* **************** SPRITES ANIMATION - UPBGE **************** */

static bNodeSocketTemplate sh_node_sprites_animation_in[] = {
    {SOCK_FLOAT, N_("Frames"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 10000.0f, PROP_NONE},
    {SOCK_FLOAT, N_("Columns"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1024.0f, PROP_NONE},
    {SOCK_FLOAT, N_("Rows"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1024.0f, PROP_NONE},
    {SOCK_FLOAT, N_("Columns Offset"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 10000.0f, PROP_NONE},
    {SOCK_FLOAT, N_("Rows Offset"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 10000.0f, PROP_NONE},
    {-1, ""}};

static bNodeSocketTemplate sh_node_sprites_animation_out[] = {
    {SOCK_VECTOR, N_("Location")},
    {SOCK_VECTOR, N_("Scale")},
    {-1, ""},
};

static int gpu_shader_sprites_animation(GPUMaterial *mat,
                                        bNode *node,
                                        bNodeExecData *UNUSED(execdata),
                                        GPUNodeStack *in,
                                        GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "node_sprites_animation", in, out);
}

void register_node_type_sh_sprites_animation(void)
{
  static bNodeType ntype;

  sh_fn_node_type_base(&ntype, SH_NODE_SPRITES_ANIMATION, "Sprites Animation", NODE_CLASS_SHADER, 0);
  node_type_socket_templates(&ntype, sh_node_sprites_animation_in, sh_node_sprites_animation_out);
  node_type_init(&ntype, NULL);
  node_type_storage(&ntype, "", NULL, NULL);
  node_type_gpu(&ntype, gpu_shader_sprites_animation);

  nodeRegisterType(&ntype);
}
