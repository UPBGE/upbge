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

extern struct bNodeTreeType *ntreeType_Shader;

/* the type definitions array */
/* ****************** types array for all shaders ****************** */

void register_node_tree_type_sh(void);

void register_node_type_sh_group(void);

void register_node_type_sh_camera(void);
void register_node_type_sh_value(void);
void register_node_type_sh_rgb(void);
void register_node_type_sh_mix_rgb(void);
void register_node_type_sh_valtorgb(void);
void register_node_type_sh_rgbtobw(void);
void register_node_type_sh_shadertorgb(void);
void register_node_type_sh_normal(void);
void register_node_type_sh_gamma(void);
void register_node_type_sh_brightcontrast(void);
void register_node_type_sh_mapping(void);
void register_node_type_sh_curve_float(void);
void register_node_type_sh_curve_vec(void);
void register_node_type_sh_curve_rgb(void);
void register_node_type_sh_map_range(void);
void register_node_type_sh_clamp(void);
void register_node_type_sh_math(void);
void register_node_type_sh_vect_math(void);
void register_node_type_sh_squeeze(void);
void register_node_type_sh_dynamic(void);
void register_node_type_sh_invert(void);
void register_node_type_sh_sepcolor(void);
void register_node_type_sh_combcolor(void);
void register_node_type_sh_seprgb(void);
void register_node_type_sh_combrgb(void);
void register_node_type_sh_sephsv(void);
void register_node_type_sh_combhsv(void);
void register_node_type_sh_sepxyz(void);
void register_node_type_sh_combxyz(void);
void register_node_type_sh_hue_sat(void);
void register_node_type_sh_tex_brick(void);
void register_node_type_sh_tex_pointdensity(void);

void register_node_type_sh_attribute(void);
void register_node_type_sh_bevel(void);
void register_node_type_sh_displacement(void);
void register_node_type_sh_vector_displacement(void);
void register_node_type_sh_geometry(void);
void register_node_type_sh_light_path(void);
void register_node_type_sh_light_falloff(void);
void register_node_type_sh_object_info(void);
void register_node_type_sh_fresnel(void);
void register_node_type_sh_wireframe(void);
void register_node_type_sh_wavelength(void);
void register_node_type_sh_blackbody(void);
void register_node_type_sh_layer_weight(void);
void register_node_type_sh_tex_coord(void);
void register_node_type_sh_particle_info(void);
void register_node_type_sh_hair_info(void);
void register_node_type_sh_point_info(void);
void register_node_type_sh_volume_info(void);
void register_node_type_sh_script(void);
void register_node_type_sh_normal_map(void);
void register_node_type_sh_tangent(void);
void register_node_type_sh_vector_rotate(void);
void register_node_type_sh_vect_transform(void);
void register_node_type_sh_vertex_color(void);

void register_node_type_sh_ambient_occlusion(void);
void register_node_type_sh_background(void);
void register_node_type_sh_bsdf_diffuse(void);
void register_node_type_sh_bsdf_glossy(void);
void register_node_type_sh_bsdf_glass(void);
void register_node_type_sh_bsdf_refraction(void);
void register_node_type_sh_bsdf_translucent(void);
void register_node_type_sh_bsdf_transparent(void);
void register_node_type_sh_bsdf_velvet(void);
void register_node_type_sh_bsdf_toon(void);
void register_node_type_sh_bsdf_anisotropic(void);
void register_node_type_sh_bsdf_principled(void);
void register_node_type_sh_emission(void);
void register_node_type_sh_holdout(void);
void register_node_type_sh_volume_absorption(void);
void register_node_type_sh_volume_scatter(void);
void register_node_type_sh_volume_principled(void);
void register_node_type_sh_bsdf_hair(void);
void register_node_type_sh_bsdf_hair_principled(void);
void register_node_type_sh_subsurface_scattering(void);
void register_node_type_sh_mix_shader(void);
void register_node_type_sh_add_shader(void);
void register_node_type_sh_uvmap(void);
void register_node_type_sh_uvalongstroke(void);
void register_node_type_sh_eevee_metallic(void);
void register_node_type_sh_eevee_specular(void);

void register_node_type_sh_output_light(void);
void register_node_type_sh_output_material(void);
void register_node_type_sh_output_eevee_material(void);
void register_node_type_sh_output_world(void);
void register_node_type_sh_output_linestyle(void);
void register_node_type_sh_output_aov(void);

void register_node_type_sh_tex_image(void);
void register_node_type_sh_tex_environment(void);
void register_node_type_sh_tex_sky(void);
void register_node_type_sh_tex_voronoi(void);
void register_node_type_sh_tex_gradient(void);
void register_node_type_sh_tex_magic(void);
void register_node_type_sh_tex_wave(void);
void register_node_type_sh_tex_musgrave(void);
void register_node_type_sh_tex_noise(void);
void register_node_type_sh_tex_checker(void);
void register_node_type_sh_bump(void);
void register_node_type_sh_tex_ies(void);
void register_node_type_sh_tex_white_noise(void);

/* UPBGE */
void register_node_type_sh_sprites_animation(void);

void register_node_type_sh_custom_group(bNodeType *ntype);

struct bNodeTreeExec *ntreeShaderBeginExecTree(struct bNodeTree *ntree);
void ntreeShaderEndExecTree(struct bNodeTreeExec *exec);

/**
 * Find an output node of the shader tree.
 *
 * \note it will only return output which is NOT in the group, which isn't how
 * render engines works but it's how the GPU shader compilation works. This we
 * can change in the future and make it a generic function, but for now it stays
 * private here.
 */
struct bNode *ntreeShaderOutputNode(struct bNodeTree *ntree, int target);

/**
 * This one needs to work on a local tree.
 */
void ntreeGPUMaterialNodes(struct bNodeTree *localtree, struct GPUMaterial *mat);

#ifdef __cplusplus
}
#endif
