/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup texnodes
 */

#include "NOD_texture.h"
#include "node_texture_util.h"
#include <math.h>

static bNodeSocketTemplate inputs[] = {
    {SOCK_RGBA, N_("Color"), 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_VECTOR, N_("Offset"), 0.0f, 0.0f, 0.0f, 0.0f, -10000.0f, 10000.0f, PROP_TRANSLATION},
    {-1, ""},
};

static bNodeSocketTemplate outputs[] = {
    {SOCK_RGBA, N_("Color")},
    {-1, ""},
};

static void colorfn(float *out, TexParams *p, bNode *UNUSED(node), bNodeStack **in, short thread)
{
  float offset[3], new_co[3];
  TexParams np = *p;
  np.co = new_co;

  tex_input_vec(offset, in[1], p, thread);

  new_co[0] = p->co[0] + offset[0];
  new_co[1] = p->co[1] + offset[1];
  new_co[2] = p->co[2] + offset[2];

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

void register_node_type_tex_translate(void)
{
  static bNodeType ntype;

  tex_node_type_base(&ntype, TEX_NODE_TRANSLATE, "Translate", NODE_CLASS_DISTORT);
  node_type_socket_templates(&ntype, inputs, outputs);
  node_type_exec(&ntype, NULL, NULL, exec);

  nodeRegisterType(&ntype);
}
