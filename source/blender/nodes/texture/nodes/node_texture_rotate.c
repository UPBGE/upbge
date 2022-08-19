/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup texnodes
 */

#include <math.h>

#include "NOD_texture.h"
#include "node_texture_util.h"

static bNodeSocketTemplate inputs[] = {
    {SOCK_RGBA, N_("Color"), 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_FLOAT, N_("Turns"), 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, PROP_NONE},
    {SOCK_VECTOR, N_("Axis"), 0.0f, 0.0f, 1.0f, 0.0f, -1.0f, 1.0f, PROP_DIRECTION},
    {-1, ""},
};

static bNodeSocketTemplate outputs[] = {
    {SOCK_RGBA, N_("Color")},
    {-1, ""},
};

static void rotate(float new_co[3], float a, const float ax[3], const float co[3])
{
  float para[3];
  float perp[3];
  float cp[3];

  float cos_a = cosf(a * (float)(2 * M_PI));
  float sin_a = sinf(a * (float)(2 * M_PI));

  /* `x' = xcosa + n(n.x)(1-cosa) + (x*n)sina`. */

  mul_v3_v3fl(perp, co, cos_a);
  mul_v3_v3fl(para, ax, dot_v3v3(co, ax) * (1 - cos_a));

  cross_v3_v3v3(cp, ax, co);
  mul_v3_fl(cp, sin_a);

  new_co[0] = para[0] + perp[0] + cp[0];
  new_co[1] = para[1] + perp[1] + cp[1];
  new_co[2] = para[2] + perp[2] + cp[2];
}

static void colorfn(float *out, TexParams *p, bNode *UNUSED(node), bNodeStack **in, short thread)
{
  float new_co[3], new_dxt[3], new_dyt[3], a, ax[3];

  a = tex_input_value(in[1], p, thread);
  tex_input_vec(ax, in[2], p, thread);

  rotate(new_co, a, ax, p->co);
  if (p->osatex) {
    rotate(new_dxt, a, ax, p->dxt);
    rotate(new_dyt, a, ax, p->dyt);
  }

  {
    TexParams np = *p;
    np.co = new_co;
    np.dxt = new_dxt;
    np.dyt = new_dyt;
    tex_input_rgba(out, in[0], &np, thread);
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

void register_node_type_tex_rotate(void)
{
  static bNodeType ntype;

  tex_node_type_base(&ntype, TEX_NODE_ROTATE, "Rotate", NODE_CLASS_DISTORT);
  node_type_socket_templates(&ntype, inputs, outputs);
  node_type_exec(&ntype, NULL, NULL, exec);

  nodeRegisterType(&ntype);
}
