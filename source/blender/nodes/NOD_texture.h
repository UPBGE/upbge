/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup nodes
 */

#pragma once

#include "BKE_node.h"

#ifdef __cplusplus
extern "C" {
#endif

extern struct bNodeTreeType *ntreeType_Texture;

/* ****************** types array for all texture nodes ****************** */

void register_node_tree_type_tex(void);

void register_node_type_tex_group(void);

void register_node_type_tex_math(void);
void register_node_type_tex_mix_rgb(void);
void register_node_type_tex_valtorgb(void);
void register_node_type_tex_valtonor(void);
void register_node_type_tex_rgbtobw(void);
void register_node_type_tex_output(void);
void register_node_type_tex_viewer(void);
void register_node_type_tex_checker(void);
void register_node_type_tex_texture(void);
void register_node_type_tex_bricks(void);
void register_node_type_tex_image(void);
void register_node_type_tex_curve_rgb(void);
void register_node_type_tex_curve_time(void);
void register_node_type_tex_invert(void);
void register_node_type_tex_hue_sat(void);
void register_node_type_tex_coord(void);
void register_node_type_tex_distance(void);

void register_node_type_tex_rotate(void);
void register_node_type_tex_translate(void);
void register_node_type_tex_scale(void);
void register_node_type_tex_at(void);

void register_node_type_tex_compose(void);
void register_node_type_tex_decompose(void);
void register_node_type_tex_combine_color(void);
void register_node_type_tex_separate_color(void);

void register_node_type_tex_proc_voronoi(void);
void register_node_type_tex_proc_blend(void);
void register_node_type_tex_proc_magic(void);
void register_node_type_tex_proc_marble(void);
void register_node_type_tex_proc_clouds(void);
void register_node_type_tex_proc_wood(void);
void register_node_type_tex_proc_musgrave(void);
void register_node_type_tex_proc_noise(void);
void register_node_type_tex_proc_stucci(void);
void register_node_type_tex_proc_distnoise(void);

void ntreeTexCheckCyclics(struct bNodeTree *ntree);
struct bNodeTreeExec *ntreeTexBeginExecTree(struct bNodeTree *ntree);
void ntreeTexEndExecTree(struct bNodeTreeExec *exec);
int ntreeTexExecTree(struct bNodeTree *ntree,
                     struct TexResult *target,
                     const float co[3],
                     float dxt[3],
                     float dyt[3],
                     int osatex,
                     short thread,
                     const struct Tex *tex,
                     short which_output,
                     int cfra,
                     int preview,
                     struct MTex *mtex);

#ifdef __cplusplus
}
#endif
