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

#include "node_shader_util.hh"

/* **************** SPRITES ANIMATION - UPBGE **************** */

namespace blender::nodes::node_shader_sprites_animation_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("Frames"))
      .default_value(0.0f)
      .min(0.0f)
      .max(10000.0f)
      .subtype(PROP_NONE);
  b.add_input<decl::Float>(N_("Columns"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1024.0f)
      .subtype(PROP_NONE);
  b.add_input<decl::Float>(N_("Rows"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1024.0f)
      .subtype(PROP_NONE);
  b.add_input<decl::Float>(N_("Columns Offset"))
      .default_value(0.0f)
      .min(0.0f)
      .max(10000.0f)
      .subtype(PROP_NONE);
  b.add_input<decl::Float>(N_("Rows Offset"))
      .default_value(0.0f)
      .min(0.0f)
      .max(10000.0f)
      .subtype(PROP_NONE);
  b.add_output<decl::Vector>(N_("Location")).hide_value();
  b.add_output<decl::Vector>(N_("Scale")).hide_value();
}

static int gpu_shader_sprites_animation(GPUMaterial *mat,
                                        bNode *node,
                                        bNodeExecData */*execdata*/,
                                        GPUNodeStack *in,
                                        GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "node_sprites_animation", in, out);
}

}  // namespace blender::nodes::node_shader_sprites_animation_cc {


void register_node_type_sh_sprites_animation()
{

  namespace file_ns = blender::nodes::node_shader_sprites_animation_cc;

  static blender::bke::bNodeType ntype;

  sh_fn_node_type_base(&ntype, "ShaderNodeSpritesAnimation", SH_NODE_SPRITES_ANIMATION);
  ntype.ui_name = "SpritesAnimation";
  ntype.ui_description = "To animate sprites";
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::gpu_shader_sprites_animation;

  node_register_type(&ntype);
}
