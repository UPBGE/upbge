/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup texnodes
 */

#include "NOD_texture.h"
#include "node_texture_util.h"

static bNodeSocketTemplate inputs[] = {
    {SOCK_FLOAT, N_("Val"), 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, PROP_NONE},
    {SOCK_FLOAT, N_("Nabla"), 0.025f, 0.0f, 0.0f, 0.0f, 0.001f, 0.1f, PROP_UNSIGNED},
    {-1, ""},
};

static bNodeSocketTemplate outputs[] = {
    {SOCK_VECTOR, N_("Normal")},
    {-1, ""},
};

static void normalfn(float *out, TexParams *p, bNode *UNUSED(node), bNodeStack **in, short thread)
{
  float new_co[3];
  const float *co = p->co;

  float nabla = tex_input_value(in[1], p, thread);
  float val;
  float nor[3];

  TexParams np = *p;
  np.co = new_co;

  val = tex_input_value(in[0], p, thread);

  new_co[0] = co[0] + nabla;
  new_co[1] = co[1];
  new_co[2] = co[2];
  nor[0] = tex_input_value(in[0], &np, thread);

  new_co[0] = co[0];
  new_co[1] = co[1] + nabla;
  nor[1] = tex_input_value(in[0], &np, thread);

  new_co[1] = co[1];
  new_co[2] = co[2] + nabla;
  nor[2] = tex_input_value(in[0], &np, thread);

  out[0] = val - nor[0];
  out[1] = val - nor[1];
  out[2] = val - nor[2];
}
static void exec(void *data,
                 int UNUSED(thread),
                 bNode *node,
                 bNodeExecData *execdata,
                 bNodeStack **in,
                 bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &normalfn, data);
}

void register_node_type_tex_valtonor(void)
{
  static bNodeType ntype;

  tex_node_type_base(&ntype, TEX_NODE_VALTONOR, "Value to Normal", NODE_CLASS_CONVERTER);
  node_type_socket_templates(&ntype, inputs, outputs);
  node_type_exec(&ntype, NULL, NULL, exec);

  nodeRegisterType(&ntype);
}
