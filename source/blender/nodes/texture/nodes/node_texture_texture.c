/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup texnodes
 */

#include "NOD_texture.h"
#include "node_texture_util.h"

#include "RE_texture.h"

static bNodeSocketTemplate inputs[] = {
    {SOCK_RGBA, N_("Color1"), 1.0f, 1.0f, 1.0f, 1.0f},
    {SOCK_RGBA, N_("Color2"), 0.0f, 0.0f, 0.0f, 1.0f},
    {-1, ""},
};

static bNodeSocketTemplate outputs[] = {
    {SOCK_RGBA, N_("Color")},
    {-1, ""},
};

static void colorfn(float *out, TexParams *p, bNode *node, bNodeStack **in, short thread)
{
  Tex *nodetex = (Tex *)node->id;
  static float red[] = {1, 0, 0, 1};
  static float white[] = {1, 1, 1, 1};
  float co[3], dxt[3], dyt[3];

  copy_v3_v3(co, p->co);
  if (p->osatex) {
    copy_v3_v3(dxt, p->dxt);
    copy_v3_v3(dyt, p->dyt);
  }
  else {
    zero_v3(dxt);
    zero_v3(dyt);
  }

  if (node->custom2 || node->need_exec == 0) {
    /* this node refers to its own texture tree! */
    copy_v4_v4(out, (fabsf(co[0] - co[1]) < 0.01f) ? white : red);
  }
  else if (nodetex) {
    TexResult texres;
    int textype;
    float col1[4], col2[4];

    tex_input_rgba(col1, in[0], p, thread);
    tex_input_rgba(col2, in[1], p, thread);

    textype = multitex_nodes(nodetex, co, dxt, dyt, p->osatex, &texres, thread, 0, p->mtex, NULL);

    if (textype & TEX_RGB) {
      copy_v4_v4(out, texres.trgba);
    }
    else {
      copy_v4_v4(out, col1);
      ramp_blend(MA_RAMP_BLEND, out, texres.tin, col2);
    }
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

void register_node_type_tex_texture(void)
{
  static bNodeType ntype;

  tex_node_type_base(&ntype, TEX_NODE_TEXTURE, "Texture", NODE_CLASS_INPUT);
  node_type_socket_templates(&ntype, inputs, outputs);
  node_type_exec(&ntype, NULL, NULL, exec);
  ntype.flag |= NODE_PREVIEW;

  nodeRegisterType(&ntype);
}
