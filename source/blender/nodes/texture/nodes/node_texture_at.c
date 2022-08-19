/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup texnodes
 */

#include "NOD_texture.h"
#include "node_texture_util.h"

static bNodeSocketTemplate inputs[] = {
    {SOCK_RGBA, N_("Texture"), 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_VECTOR, N_("Coordinates"), 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, PROP_NONE},
    {-1, ""},
};
static bNodeSocketTemplate outputs[] = {
    {SOCK_RGBA, N_("Texture")},
    {-1, ""},
};

static void colorfn(float *out, TexParams *p, bNode *UNUSED(node), bNodeStack **in, short thread)
{
  TexParams np = *p;
  float new_co[3];
  np.co = new_co;

  tex_input_vec(new_co, in[1], p, thread);
  tex_input_rgba(out, in[0], &np, thread);
}

static void exec(void *data,
                 int UNUSED(thread),
                 bNode *node,
                 bNodeExecData *execdata,
                 bNodeStack **in,
                 bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &colorfn, data);
}

void register_node_type_tex_at(void)
{
  static bNodeType ntype;

  tex_node_type_base(&ntype, TEX_NODE_AT, "At", NODE_CLASS_DISTORT);
  node_type_socket_templates(&ntype, inputs, outputs);
  node_type_size(&ntype, 140, 100, 320);
  node_type_exec(&ntype, NULL, NULL, exec);

  nodeRegisterType(&ntype);
}
