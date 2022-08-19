/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup texnodes
 */

#include "NOD_texture.h"
#include "node_texture_util.h"

/* **************** CURVE Time  ******************** */

/* custom1 = start-frame, custom2 = end-frame. */
static bNodeSocketTemplate time_outputs[] = {{SOCK_FLOAT, N_("Value")}, {-1, ""}};

static void time_colorfn(
    float *out, TexParams *p, bNode *node, bNodeStack **UNUSED(in), short UNUSED(thread))
{
  /* stack order output: fac */
  float fac = 0.0f;

  if (node->custom1 < node->custom2) {
    fac = (p->cfra - node->custom1) / (float)(node->custom2 - node->custom1);
  }

  BKE_curvemapping_init(node->storage);
  fac = BKE_curvemapping_evaluateF(node->storage, 0, fac);
  out[0] = CLAMPIS(fac, 0.0f, 1.0f);
}

static void time_exec(void *data,
                      int UNUSED(thread),
                      bNode *node,
                      bNodeExecData *execdata,
                      bNodeStack **in,
                      bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &time_colorfn, data);
}

static void time_init(bNodeTree *UNUSED(ntree), bNode *node)
{
  node->custom1 = 1;
  node->custom2 = 250;
  node->storage = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
}

void register_node_type_tex_curve_time(void)
{
  static bNodeType ntype;

  tex_node_type_base(&ntype, TEX_NODE_CURVE_TIME, "Time", NODE_CLASS_INPUT);
  node_type_socket_templates(&ntype, NULL, time_outputs);
  node_type_size_preset(&ntype, NODE_SIZE_LARGE);
  node_type_init(&ntype, time_init);
  node_type_storage(&ntype, "CurveMapping", node_free_curves, node_copy_curves);
  node_type_exec(&ntype, node_initexec_curves, NULL, time_exec);

  nodeRegisterType(&ntype);
}

/* **************** CURVE RGB  ******************** */
static bNodeSocketTemplate rgb_inputs[] = {
    {SOCK_RGBA, N_("Color"), 0.0f, 0.0f, 0.0f, 1.0f},
    {-1, ""},
};

static bNodeSocketTemplate rgb_outputs[] = {
    {SOCK_RGBA, N_("Color")},
    {-1, ""},
};

static void rgb_colorfn(float *out, TexParams *p, bNode *node, bNodeStack **in, short thread)
{
  float cin[4];
  tex_input_rgba(cin, in[0], p, thread);

  BKE_curvemapping_evaluateRGBF(node->storage, out, cin);
  out[3] = cin[3];
}

static void rgb_exec(void *data,
                     int UNUSED(thread),
                     bNode *node,
                     bNodeExecData *execdata,
                     bNodeStack **in,
                     bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &rgb_colorfn, data);
}

static void rgb_init(bNodeTree *UNUSED(ntree), bNode *node)
{
  node->storage = BKE_curvemapping_add(4, 0.0f, 0.0f, 1.0f, 1.0f);
}

void register_node_type_tex_curve_rgb(void)
{
  static bNodeType ntype;

  tex_node_type_base(&ntype, TEX_NODE_CURVE_RGB, "RGB Curves", NODE_CLASS_OP_COLOR);
  node_type_socket_templates(&ntype, rgb_inputs, rgb_outputs);
  node_type_size_preset(&ntype, NODE_SIZE_LARGE);
  node_type_init(&ntype, rgb_init);
  node_type_storage(&ntype, "CurveMapping", node_free_curves, node_copy_curves);
  node_type_exec(&ntype, node_initexec_curves, NULL, rgb_exec);

  nodeRegisterType(&ntype);
}
