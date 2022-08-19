/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup texnodes
 */

#include "NOD_texture.h"
#include "node_texture_util.h"
#include <math.h>

static bNodeSocketTemplate inputs[] = {
    {SOCK_RGBA, N_("Color"), 1.0f, 0.0f, 0.0f, 1.0f},
    {-1, ""},
};

static void exec(void *data,
                 int UNUSED(thread),
                 bNode *UNUSED(node),
                 bNodeExecData *UNUSED(execdata),
                 bNodeStack **in,
                 bNodeStack **UNUSED(out))
{
  TexCallData *cdata = (TexCallData *)data;

  if (cdata->do_preview) {
    TexParams params;
    float col[4];
    params_from_cdata(&params, cdata);

    tex_input_rgba(col, in[0], &params, cdata->thread);
  }
}

void register_node_type_tex_viewer(void)
{
  static bNodeType ntype;

  tex_node_type_base(&ntype, TEX_NODE_VIEWER, "Viewer", NODE_CLASS_OUTPUT);
  node_type_socket_templates(&ntype, inputs, NULL);
  node_type_exec(&ntype, NULL, NULL, exec);

  ntype.no_muting = true;
  ntype.flag |= NODE_PREVIEW;

  nodeRegisterType(&ntype);
}
