/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_tex_environment_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>(N_("Vector")).hide_value();
  b.add_output<decl::Color>(N_("Color")).no_muted_links();
}

static void node_shader_init_tex_environment(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeTexEnvironment *tex = MEM_cnew<NodeTexEnvironment>("NodeTexEnvironment");
  BKE_texture_mapping_default(&tex->base.tex_mapping, TEXMAP_TYPE_POINT);
  BKE_texture_colormapping_default(&tex->base.color_mapping);
  tex->projection = SHD_PROJ_EQUIRECTANGULAR;
  BKE_imageuser_default(&tex->iuser);

  node->storage = tex;
}

static int node_shader_gpu_tex_environment(GPUMaterial *mat,
                                           bNode *node,
                                           bNodeExecData *UNUSED(execdata),
                                           GPUNodeStack *in,
                                           GPUNodeStack *out)
{
  Image *ima = (Image *)node->id;
  NodeTexEnvironment *tex = (NodeTexEnvironment *)node->storage;

  /* We get the image user from the original node, since GPU image keeps
   * a pointer to it and the dependency refreshes the original. */
  bNode *node_original = node->original ? node->original : node;
  NodeTexImage *tex_original = (NodeTexImage *)node_original->storage;
  ImageUser *iuser = &tex_original->iuser;
  eGPUSamplerState sampler = GPU_SAMPLER_REPEAT | GPU_SAMPLER_ANISO | GPU_SAMPLER_FILTER;
  /* TODO(@fclem): For now assume mipmap is always enabled. */
  if (true) {
    sampler |= GPU_SAMPLER_MIPMAP;
  }

  GPUNodeLink *outalpha;

  /* HACK(@fclem): For lookdev mode: do not compile an empty environment and just create an empty
   * texture entry point. We manually bind to it after #DRW_shgroup_add_material_resources(). */
  if (!ima && !GPU_material_flag_get(mat, GPU_MATFLAG_LOOKDEV_HACK)) {
    return GPU_stack_link(mat, node, "node_tex_environment_empty", in, out);
  }

  node_shader_gpu_default_tex_coord(mat, node, &in[0].link);
  node_shader_gpu_tex_mapping(mat, node, in, out);

  /* Compute texture coordinate. */
  if (tex->projection == SHD_PROJ_EQUIRECTANGULAR) {
    GPU_link(mat, "node_tex_environment_equirectangular", in[0].link, &in[0].link);
    /* To fix pole issue we clamp the v coordinate. */
    sampler &= ~GPU_SAMPLER_REPEAT_T;
    /* Force the highest mipmap and don't do anisotropic filtering.
     * This is to fix the artifact caused by derivatives discontinuity. */
    sampler &= ~(GPU_SAMPLER_MIPMAP | GPU_SAMPLER_ANISO);
  }
  else {
    GPU_link(mat, "node_tex_environment_mirror_ball", in[0].link, &in[0].link);
    /* Fix pole issue. */
    sampler &= ~GPU_SAMPLER_REPEAT;
  }

  const char *gpu_fn;
  static const char *names[] = {
      "node_tex_image_linear",
      "node_tex_image_cubic",
  };

  switch (tex->interpolation) {
    case SHD_INTERP_LINEAR:
      gpu_fn = names[0];
      break;
    case SHD_INTERP_CLOSEST:
      sampler &= ~(GPU_SAMPLER_FILTER | GPU_SAMPLER_MIPMAP);
      gpu_fn = names[0];
      break;
    default:
      gpu_fn = names[1];
      break;
  }

  /* Sample texture with correct interpolation. */
  GPU_link(mat, gpu_fn, in[0].link, GPU_image(mat, ima, iuser, sampler), &out[0].link, &outalpha);

  if (out[0].hasoutput && ima) {
    if (ELEM(ima->alpha_mode, IMA_ALPHA_IGNORE, IMA_ALPHA_CHANNEL_PACKED) ||
        IMB_colormanagement_space_name_is_data(ima->colorspace_settings.name)) {
      /* Don't let alpha affect color output in these cases. */
      GPU_link(mat, "color_alpha_clear", out[0].link, &out[0].link);
    }
    else {
      /* Always output with premultiplied alpha. */
      if (ima->alpha_mode == IMA_ALPHA_PREMUL) {
        GPU_link(mat, "color_alpha_clear", out[0].link, &out[0].link);
      }
      else {
        GPU_link(mat, "color_alpha_premultiply", out[0].link, &out[0].link);
      }
    }
  }

  return true;
}

}  // namespace blender::nodes::node_shader_tex_environment_cc

/* node type definition */
void register_node_type_sh_tex_environment()
{
  namespace file_ns = blender::nodes::node_shader_tex_environment_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_TEX_ENVIRONMENT, "Environment Texture", NODE_CLASS_TEXTURE);
  ntype.declare = file_ns::node_declare;
  node_type_init(&ntype, file_ns::node_shader_init_tex_environment);
  node_type_storage(
      &ntype, "NodeTexEnvironment", node_free_standard_storage, node_copy_standard_storage);
  node_type_gpu(&ntype, file_ns::node_shader_gpu_tex_environment);
  ntype.labelfunc = node_image_label;
  node_type_size_preset(&ntype, NODE_SIZE_LARGE);

  nodeRegisterType(&ntype);
}
