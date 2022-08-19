/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup texnodes
 */

#include "NOD_texture.h"
#include "node_texture_util.h"
#include <math.h>

static bNodeSocketTemplate inputs[] = {
    {SOCK_VECTOR, N_("Coordinate 1"), 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, PROP_NONE},
    {SOCK_VECTOR, N_("Coordinate 2"), 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, PROP_NONE},
    {-1, ""},
};

static bNodeSocketTemplate outputs[] = {
    {SOCK_FLOAT, N_("Value")},
    {-1, ""},
};

static void valuefn(float *out, TexParams *p, bNode *UNUSED(node), bNodeStack **in, short thread)
{
  float co1[3], co2[3];

  tex_input_vec(co1, in[0], p, thread);
  tex_input_vec(co2, in[1], p, thread);

  *out = len_v3v3(co2, co1);
}

static void exec(void *data,
                 int UNUSED(thread),
                 bNode *node,
                 bNodeExecData *execdata,
                 bNodeStack **in,
                 bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &valuefn, data);
}

void register_node_type_tex_distance(void)
{
  static bNodeType ntype;

  tex_node_type_base(&ntype, TEX_NODE_DISTANCE, "Distance", NODE_CLASS_CONVERTER);
  node_type_socket_templates(&ntype, inputs, outputs);
  node_type_exec(&ntype, NULL, NULL, exec);

  nodeRegisterType(&ntype);
}
