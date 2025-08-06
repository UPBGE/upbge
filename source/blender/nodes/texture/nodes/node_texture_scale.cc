/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "BLI_math_vector.h"

#include "BKE_node.hh"

#include "node_texture_util.hh"

static blender::bke::bNodeSocketTemplate inputs[] = {
    {SOCK_RGBA, N_("Color"), 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_VECTOR, N_("Scale"), 1.0f, 1.0f, 1.0f, 0.0f, -10.0f, 10.0f, PROP_XYZ},
    {-1, ""},
};

static blender::bke::bNodeSocketTemplate outputs[] = {
    {SOCK_RGBA, N_("Color")},
    {-1, ""},
};

static void colorfn(float *out, TexParams *p, bNode * /*node*/, bNodeStack **in, short thread)
{
  float scale[3], new_co[3];
  TexParams np = *p;

  np.co = new_co;

  tex_input_vec(scale, in[1], p, thread);

  mul_v3_v3v3(new_co, p->co, scale);

  tex_input_rgba(out, in[0], &np, thread);
}
static void exec(void *data,
                 int /*thread*/,
                 bNode *node,
                 bNodeExecData *execdata,
                 bNodeStack **in,
                 bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &colorfn, static_cast<TexCallData *>(data));
}

void register_node_type_tex_scale()
{
  static blender::bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeScale", TEX_NODE_SCALE);
  ntype.ui_name = "Scale";
  ntype.enum_name_legacy = "SCALE";
  ntype.nclass = NODE_CLASS_DISTORT;
  blender::bke::node_type_socket_templates(&ntype, inputs, outputs);
  ntype.exec_fn = exec;

  blender::bke::node_register_type(ntype);
}
