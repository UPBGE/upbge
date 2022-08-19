/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup texnodes
 */

#include "NOD_texture.h"
#include "node_texture_util.h"

static bNodeSocketTemplate inputs[] = {
    {SOCK_FLOAT, N_("Red"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, PROP_UNSIGNED},
    {SOCK_FLOAT, N_("Green"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, PROP_UNSIGNED},
    {SOCK_FLOAT, N_("Blue"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, PROP_UNSIGNED},
    {SOCK_FLOAT, N_("Alpha"), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, PROP_UNSIGNED},
    {-1, ""},
};
static bNodeSocketTemplate outputs[] = {
    {SOCK_RGBA, N_("Color")},
    {-1, ""},
};

static void colorfn(float *out, TexParams *p, bNode *UNUSED(node), bNodeStack **in, short thread)
{
  int i;
  for (i = 0; i < 4; i++) {
    out[i] = tex_input_value(in[i], p, thread);
  }
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

void register_node_type_tex_compose(void)
{
  static bNodeType ntype;

  tex_node_type_base(&ntype, TEX_NODE_COMPOSE_LEGACY, "Combine RGBA", NODE_CLASS_OP_COLOR);
  node_type_socket_templates(&ntype, inputs, outputs);
  node_type_exec(&ntype, NULL, NULL, exec);

  nodeRegisterType(&ntype);
}
