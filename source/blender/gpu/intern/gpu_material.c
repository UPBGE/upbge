/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2006 Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Brecht Van Lommel.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/gpu/intern/gpu_material.c
 *  \ingroup gpu
 *
 * Manages materials, lights and textures.
 */

#include <math.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_lamp_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_world_types.h"

#include "BLI_math.h"
#include "BLI_blenlib.h"
#include "BLI_utildefines.h"

#include "BKE_anim.h"
#include "BKE_colorband.h"
#include "BKE_colortools.h"
#include "BKE_global.h"
#include "BKE_image.h"
#include "BKE_main.h"
#include "BKE_node.h"
#include "BKE_scene.h"
#include "BKE_group.h"

#include "IMB_imbuf_types.h"

#include "GPU_extensions.h"
#include "GPU_framebuffer.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_texture.h"

#include "gpu_codegen.h"

#ifdef WITH_OPENSUBDIV
#  include "BKE_DerivedMesh.h"
#endif

/* Structs */

typedef enum DynMatProperty {
	DYN_LAMP_CO = 1,
	DYN_LAMP_VEC = 2,
	DYN_LAMP_IMAT = 4,
	DYN_LAMP_PERSMAT = 8,
} DynMatProperty;

static struct GPUWorld {
	float mistenabled;
	float mistype;
	float miststart;
	float mistdistance;
	float mistintensity;
	float mistcol[4];
	float horicol[3];
	float ambcol[4];
	float zencol[3];
	float logfac;
	float linfac;
	float envlightenergy;
} GPUWorld;

struct GPUMaterial {
	Scene *scene;
	Material *ma;

	/* material for mesh surface, worlds or something else.
	 * some code generation is done differently depending on the use case */
	int type;

	/* for creating the material */
	ListBase nodes;
	GPUNodeLink *outlink;

	/* for binding the material */
	GPUPass *pass;
	GPUVertexAttribs attribs;
	int builtins;
	int alpha, obcolalpha;
	int dynproperty;

	/* for passing uniforms */
	int viewmatloc, invviewmatloc;
	int obmatloc, invobmatloc;
	int localtoviewmatloc, invlocaltoviewmatloc;
	int obcolloc, obautobumpscaleloc;
	int cameratexcofacloc;
	int timeloc;

	int partscalarpropsloc;
	int partcoloc;
	int partvel;
	int partangvel;

	int objectinfoloc;
	int objectlayloc;

	int ininstposloc;
	int ininstmatloc;
	int ininstcolloc;
	int ininstlayloc;

	ListBase lamps;
	bool bound;

	GPUMaterialFlag flags;

	float har;
};

struct GPULamp {
	Scene *scene;
	Object *ob;
	Object *par;
	Lamp *la;

	int type, mode, lay, hide;

	int dynlayer;
	float dynenergy, dyncol[3];
	float energy, col[3];
	float cutoff, radius;

	float co[3], vec[3];
	float dynco[3], dynvec[3];
	float obmat[4][4];
	float imat[4][4];
	float dynimat[4][4];

	float spotsi, spotbl, k;
	float spotvec[2];
	float dyndist, dynatt1, dynatt2;
	float dist, att1, att2;
	float coeff_const, coeff_lin, coeff_quad;
	float shadow_color[3];

	float bias, slopebias, d, clipend;
	int size;

	int falloff_type;
	struct CurveMapping *curfalloff;

	float winmat[4][4];
	float viewmat[4][4];
	float persmat[4][4];
	float dynpersmat[4][4];

	GPUFrameBuffer *fb;
	GPUFrameBuffer *blurfb;
	GPUTexture *tex;
	GPUTexture *depthtex;
	GPUTexture *blurtex;

	ListBase materials;
};

/* Forward declaration so shade_light_textures() can use this, while still keeping the code somewhat organized */
static void texture_rgb_blend(
        GPUMaterial *mat, GPUNodeLink *tex, GPUNodeLink *out, GPUNodeLink *fact, GPUNodeLink *facg,
        int blendtype, GPUNodeLink **in);

/* Functions */

static bool tex_do_color_management(GPUMaterial *mat, MTex *mtex, Tex *tex)
{
	const bool mtexDoColorManagement = (mtex->colorManagement == GAME_COLOR_MANAGEMENT_SRGB) && GPU_material_do_color_management(mat);

	if (tex->type == TEX_IMAGE) {
		return mtexDoColorManagement;
	}
	else if (tex->type == TEX_ENVMAP) {
		// Realtime textures are rendered from game engine without sRGB conversion.
		if (tex->env && tex->env->stype == ENV_REALT) {
			return !(mat->flags & GPU_MATERIAL_NO_COLOR_MANAGEMENT);
		}
		else {
			return mtexDoColorManagement;
		}
	}

	return false;
}

/// Possibly translate builtin to instancing builtin if instancing enabled and return node.
static GPUNodeLink *material_builtin(GPUMaterial *mat, GPUBuiltin builtin)
{
	const bool instancing = GPU_material_use_instancing(mat);

	if (instancing) {
		switch (builtin) {
			case GPU_OBJECT_MATRIX:
			{
				builtin = GPU_INSTANCING_MATRIX;
				break;
			}
			case GPU_INVERSE_OBJECT_MATRIX:
			{
				builtin = GPU_INSTANCING_INVERSE_MATRIX;
				break;
			}
			case GPU_OBCOLOR:
			{
				builtin = GPU_INSTANCING_COLOR;
				break;
			}
			case GPU_OBJECT_LAY:
			{
				builtin = GPU_INSTANCING_LAYER;
				break;
			}
			default:
			{
				break;
			}
		}
	}

	return GPU_builtin(builtin);
}

static GPUMaterial *GPU_material_construct_begin(Material *ma)
{
	GPUMaterial *material = MEM_callocN(sizeof(GPUMaterial), "GPUMaterial");

	material->ma = ma;

	return material;
}

static void gpu_material_set_attrib_id(GPUMaterial *material)
{
	GPUVertexAttribs *attribs = &material->attribs;
	GPUPass *pass = material->pass;
	if (!pass) {
		attribs->totlayer = 0;
		return;
	}

	GPUShader *shader = GPU_pass_shader(pass);
	if (!shader) {
		attribs->totlayer = 0;
		return;
	}

	/* convert from attribute number to the actual id assigned by opengl,
	 * in case the attrib does not get a valid index back, it was probably
	 * removed by the glsl compiler by dead code elimination */

	int b = 0;
	for (int a = 0; a < attribs->totlayer; a++) {
		char name[32];
		BLI_snprintf(name, sizeof(name), "att%d", attribs->layer[a].attribid);
		attribs->layer[a].glindex = GPU_shader_get_attribute(shader, name);

		BLI_snprintf(name, sizeof(name), "att%d_info", attribs->layer[a].attribid);
		attribs->layer[a].glinfoindoex = GPU_shader_get_uniform(shader, name);

		if (attribs->layer[a].glindex >= 0) {
			attribs->layer[b] = attribs->layer[a];
			b++;
		}
	}

	attribs->totlayer = b;
}

static int gpu_material_construct_end(GPUMaterial *material, const char *passname)
{
	if (material->outlink) {
		GPUNodeLink *outlink = material->outlink;
		material->pass = GPU_generate_pass(&material->nodes, outlink,
			&material->attribs, &material->builtins, material->type,
			passname,
			material->flags & GPU_MATERIAL_OPENSUBDIV,
			material->flags & GPU_MATERIAL_INSTANCING,
			GPU_material_use_new_shading_nodes(material));

		if (!material->pass)
			return 0;

		gpu_material_set_attrib_id(material);

		GPUShader *shader = GPU_pass_shader(material->pass);

		if (material->builtins & GPU_VIEW_MATRIX)
			material->viewmatloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_VIEW_MATRIX));
		if (material->builtins & GPU_INVERSE_VIEW_MATRIX)
			material->invviewmatloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_INVERSE_VIEW_MATRIX));
		if (material->builtins & GPU_OBJECT_MATRIX)
			material->obmatloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_OBJECT_MATRIX));
		if (material->builtins & GPU_INVERSE_OBJECT_MATRIX)
			material->invobmatloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_INVERSE_OBJECT_MATRIX));
		if (material->builtins & GPU_LOC_TO_VIEW_MATRIX)
			material->localtoviewmatloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_LOC_TO_VIEW_MATRIX));
		if (material->builtins & GPU_INVERSE_LOC_TO_VIEW_MATRIX)
			material->invlocaltoviewmatloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_INVERSE_LOC_TO_VIEW_MATRIX));
		if (material->builtins & GPU_OBCOLOR)
			material->obcolloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_OBCOLOR));
		if (material->builtins & GPU_AUTO_BUMPSCALE)
			material->obautobumpscaleloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_AUTO_BUMPSCALE));
		if (material->builtins & GPU_CAMERA_TEXCO_FACTORS)
			material->cameratexcofacloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_CAMERA_TEXCO_FACTORS));
		if (material->builtins & GPU_TIME)
			material->timeloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_TIME));
		if (material->builtins & GPU_PARTICLE_SCALAR_PROPS)
			material->partscalarpropsloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_PARTICLE_SCALAR_PROPS));
		if (material->builtins & GPU_PARTICLE_LOCATION)
			material->partcoloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_PARTICLE_LOCATION));
		if (material->builtins & GPU_PARTICLE_VELOCITY)
			material->partvel = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_PARTICLE_VELOCITY));
		if (material->builtins & GPU_PARTICLE_ANG_VELOCITY)
			material->partangvel = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_PARTICLE_ANG_VELOCITY));
		if (GPU_material_use_instancing(material)) {
			material->ininstposloc = GPU_shader_get_attribute(shader, GPU_builtin_name(GPU_INSTANCING_POSITION_ATTRIB));
			material->ininstmatloc = GPU_shader_get_attribute(shader, GPU_builtin_name(GPU_INSTANCING_MATRIX_ATTRIB));
			material->ininstcolloc = GPU_shader_get_attribute(shader, GPU_builtin_name(GPU_INSTANCING_COLOR_ATTRIB));
			material->ininstlayloc = GPU_shader_get_attribute(shader, GPU_builtin_name(GPU_INSTANCING_LAYER_ATTRIB));
		}
		if (material->builtins & GPU_OBJECT_INFO) {
			material->objectinfoloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_OBJECT_INFO));
		}
		if (material->builtins & GPU_OBJECT_LAY) {
			material->objectlayloc = GPU_shader_get_uniform(shader, GPU_builtin_name(GPU_OBJECT_LAY));
		}
		return 1;
	}
	else {
		GPU_pass_free_nodes(&material->nodes);
	}

	return 0;
}

void GPU_material_free(ListBase *gpumaterial)
{
	for (LinkData *link = gpumaterial->first; link; link = link->next) {
		GPUMaterial *material = link->data;

		if (material->pass)
			GPU_pass_free(material->pass);

		for (LinkData *nlink = material->lamps.first; nlink; nlink = nlink->next) {
			GPULamp *lamp = nlink->data;

			if (material->ma) {
				Material *ma = material->ma;

				LinkData *next = NULL;
				for (LinkData *mlink = lamp->materials.first; mlink; mlink = next) {
					next = mlink->next;
					if (mlink->data == ma)
						BLI_freelinkN(&lamp->materials, mlink);
				}
			}
		}

		BLI_freelistN(&material->lamps);

		MEM_freeN(material);
	}

	BLI_freelistN(gpumaterial);
}

bool GPU_lamp_visible(GPULamp *lamp, SceneRenderLayer *srl, Material *ma)
{
	if (lamp->hide)
		return false;
	else if (srl && srl->light_override)
		return BKE_group_object_exists(srl->light_override, lamp->ob);
	else if (ma && ma->group)
		return BKE_group_object_exists(ma->group, lamp->ob);
	else
		return true;
}

bool GPU_material_use_instancing(GPUMaterial *material)
{
	return material->flags & GPU_MATERIAL_INSTANCING;
}

void GPU_material_bind_instancing_attrib(GPUMaterial *material, void *matrixoffset, void *positionoffset, void *coloroffset, void *layeroffset)
{
	// Matrix
	if (material->ininstmatloc != -1) {
		glEnableVertexAttribArray(material->ininstmatloc);
		glEnableVertexAttribArray(material->ininstmatloc + 1);
		glEnableVertexAttribArray(material->ininstmatloc + 2);

		const unsigned short stride = sizeof(float) * 9;
		glVertexAttribPointer(material->ininstmatloc, 3, GL_FLOAT, GL_FALSE, stride, matrixoffset);
		glVertexAttribPointer(material->ininstmatloc + 1, 3, GL_FLOAT, GL_FALSE, stride, ((char *)matrixoffset) + 3 * sizeof(float));
		glVertexAttribPointer(material->ininstmatloc + 2, 3, GL_FLOAT, GL_FALSE, stride, ((char *)matrixoffset) + 6 * sizeof(float));

		glVertexAttribDivisorARB(material->ininstmatloc, 1);
		glVertexAttribDivisorARB(material->ininstmatloc + 1, 1);
		glVertexAttribDivisorARB(material->ininstmatloc + 2, 1);
	}

	// Position
	if (material->ininstposloc != -1) {
		glEnableVertexAttribArray(material->ininstposloc);
		glVertexAttribPointer(material->ininstposloc, 3, GL_FLOAT, GL_FALSE, 0, positionoffset);
		glVertexAttribDivisorARB(material->ininstposloc, 1);
	}

	// Color
	if (material->ininstcolloc != -1) {
		glEnableVertexAttribArray(material->ininstcolloc);
		glVertexAttribPointer(material->ininstcolloc, 4, GL_FLOAT, GL_FALSE, 0, coloroffset);
		glVertexAttribDivisorARB(material->ininstcolloc, 1);
	}

	// Layer
	if (material->ininstlayloc != -1) {
		glEnableVertexAttribArray(material->ininstlayloc);
		glVertexAttribIPointer(material->ininstlayloc, 1, GL_INT, 0, layeroffset);
		glVertexAttribDivisorARB(material->ininstlayloc, 1);
	}
}

void GPU_material_update_lamps(GPUMaterial *material, float viewmat[4][4], float viewinv[4][4])
{
	for (LinkData *nlink = material->lamps.first; nlink; nlink = nlink->next) {
		GPULamp *lamp = nlink->data;

		lamp->dynenergy = lamp->energy;
		copy_v3_v3(lamp->dyncol, lamp->col);

		if (material->dynproperty & DYN_LAMP_VEC) {
			copy_v3_v3(lamp->dynvec, lamp->vec);
			normalize_v3(lamp->dynvec);
			negate_v3(lamp->dynvec);
			mul_mat3_m4_v3(viewmat, lamp->dynvec);
		}

		if (material->dynproperty & DYN_LAMP_CO) {
			copy_v3_v3(lamp->dynco, lamp->co);
			mul_m4_v3(viewmat, lamp->dynco);
		}

		if (material->dynproperty & DYN_LAMP_IMAT) {
			mul_m4_m4m4(lamp->dynimat, lamp->imat, viewinv);
		}

		if (material->dynproperty & DYN_LAMP_PERSMAT) {
			/* The lamp matrices are already updated if we're using shadow buffers */
			if (!GPU_lamp_has_shadow_buffer(lamp)) {
				GPU_lamp_update_buffer_mats(lamp);
			}
			mul_m4_m4m4(lamp->dynpersmat, lamp->persmat, viewinv);
		}
	}
}

void GPU_material_bind(
        GPUMaterial *material, int viewlay, double time, int mipmap,
        float viewmat[4][4], float viewinv[4][4], float camerafactors[4], bool scenelock)
{
	if (material->pass) {
		GPUShader *shader = GPU_pass_shader(material->pass);

		SceneRenderLayer *srl = scenelock ? BLI_findlink(&material->scene->r.layers, material->scene->r.actlay) : NULL;

		if (srl)
			viewlay &= srl->lay;

		/* handle layer lamps */
		if (material->type == GPU_MATERIAL_TYPE_MESH) {
			for (LinkData *nlink = material->lamps.first; nlink; nlink = nlink->next) {
				GPULamp *lamp = nlink->data;
				/* If the lamp is hidden, disable all layers or if the lamp is not
				 * in the same layer than the view, disable the lamp. */
				if (!(lamp->lay & viewlay) || !GPU_lamp_visible(lamp, srl, material->ma)) {
					lamp->dynlayer = 0;
				}
				// If the lamp isn't selecting a layer, enable all layers.
				else if (!(lamp->mode & LA_LAYER)) {
					lamp->dynlayer = (1 << 20) - 1;
				}
				// Let the layer as it to check with object layer.
				else {
					lamp->dynlayer = lamp->lay;
				}
			}
		}

		if (material->ma) {
			material->har = material->ma->har;
		}

		/* note material must be bound before setting uniforms */
		GPU_pass_bind(material->pass, time, mipmap);

		/* handle per material built-ins */
		if (material->builtins & GPU_VIEW_MATRIX) {
			GPU_shader_uniform_vector(shader, material->viewmatloc, 16, 1, (float *)viewmat);
		}
		if (material->builtins & GPU_INVERSE_VIEW_MATRIX) {
			GPU_shader_uniform_vector(shader, material->invviewmatloc, 16, 1, (float *)viewinv);
		}
		if (material->builtins & GPU_CAMERA_TEXCO_FACTORS) {
			if (camerafactors) {
				GPU_shader_uniform_vector(shader, material->cameratexcofacloc, 4, 1, (float *)camerafactors);
			}
			else {
				/* use default, no scaling no offset */
				float borders[4] = {1.0f, 1.0f, 0.0f, 0.0f};
				GPU_shader_uniform_vector(shader, material->cameratexcofacloc, 4, 1, (float *)borders);
			}
		}
		if (material->builtins & GPU_TIME) {
			float ftime = (float)time;
			GPU_shader_uniform_vector(shader, material->timeloc, 1, 1, &ftime);
		}

		GPU_pass_update_uniforms(material->pass);

		material->bound = 1;
	}
}

GPUBuiltin GPU_get_material_builtins(GPUMaterial *material)
{
	return material->builtins;
}

void GPU_material_bind_uniforms(
        GPUMaterial *material, float obmat[4][4], float viewmat[4][4], const float obcol[4], int oblay,
        float autobumpscale, GPUParticleInfo *pi, float object_info[3])
{
	if (material->pass) {
		GPUShader *shader = GPU_pass_shader(material->pass);
		float invmat[4][4], col[4];
		float localtoviewmat[4][4];
		float invlocaltoviewmat[4][4];

		/* handle per object builtins */
		if (material->builtins & GPU_OBJECT_MATRIX) {
			GPU_shader_uniform_vector(shader, material->obmatloc, 16, 1, (float *)obmat);
		}
		if (material->builtins & GPU_INVERSE_OBJECT_MATRIX) {
			invert_m4_m4(invmat, obmat);
			GPU_shader_uniform_vector(shader, material->invobmatloc, 16, 1, (float *)invmat);
		}
		if (material->builtins & GPU_LOC_TO_VIEW_MATRIX) {
			if (viewmat) {
				mul_m4_m4m4(localtoviewmat, viewmat, obmat);
				GPU_shader_uniform_vector(shader, material->localtoviewmatloc, 16, 1, (float *)localtoviewmat);
			}
		}
		if (material->builtins & GPU_INVERSE_LOC_TO_VIEW_MATRIX) {
			if (viewmat) {
				mul_m4_m4m4(localtoviewmat, viewmat, obmat);
				invert_m4_m4(invlocaltoviewmat, localtoviewmat);
				GPU_shader_uniform_vector(shader, material->invlocaltoviewmatloc, 16, 1, (float *)invlocaltoviewmat);
			}
		}
		if (material->builtins & GPU_OBCOLOR) {
			copy_v4_v4(col, obcol);
			CLAMP(col[3], 0.0f, 1.0f);
			GPU_shader_uniform_vector(shader, material->obcolloc, 4, 1, col);
		}
		if (material->builtins & GPU_AUTO_BUMPSCALE) {
			GPU_shader_uniform_vector(shader, material->obautobumpscaleloc, 1, 1, &autobumpscale);
		}
		if (material->builtins & GPU_PARTICLE_SCALAR_PROPS) {
			GPU_shader_uniform_vector(shader, material->partscalarpropsloc, 4, 1, pi->scalprops);
		}
		if (material->builtins & GPU_PARTICLE_LOCATION) {
			GPU_shader_uniform_vector(shader, material->partcoloc, 4, 1, pi->location);
		}
		if (material->builtins & GPU_PARTICLE_VELOCITY) {
			GPU_shader_uniform_vector(shader, material->partvel, 3, 1, pi->velocity);
		}
		if (material->builtins & GPU_PARTICLE_ANG_VELOCITY) {
			GPU_shader_uniform_vector(shader, material->partangvel, 3, 1, pi->angular_velocity);
		}
		if (material->builtins & GPU_OBJECT_INFO) {
			GPU_shader_uniform_vector(shader, material->objectinfoloc, 3, 1, object_info);
		}
		if (material->builtins & GPU_OBJECT_LAY) {
			GPU_shader_uniform_vector_int(shader, material->objectlayloc, 1, 1, &oblay);
		}
	}
}

void GPU_material_unbind(GPUMaterial *material)
{
	if (material->pass) {
		material->bound = 0;
		GPU_pass_unbind(material->pass);
	}
}

bool GPU_material_bound(GPUMaterial *material)
{
	return material->bound;
}

Scene *GPU_material_scene(GPUMaterial *material)
{
	return material->scene;
}

GPUMatType GPU_Material_get_type(GPUMaterial *material)
{
	return material->type;
}

void GPU_material_vertex_attributes(GPUMaterial *material, GPUVertexAttribs *attribs)
{
	*attribs = material->attribs;
}

void GPU_material_output_link(GPUMaterial *material, GPUNodeLink *link)
{
	if (!material->outlink)
		material->outlink = link;
}

void GPU_material_enable_alpha(GPUMaterial *material)
{
	material->alpha = 1;
}

GPUBlendMode GPU_material_alpha_blend(GPUMaterial *material, const float obcol[4])
{
	if (material->alpha || (material->obcolalpha && obcol[3] < 1.0f))
		return GPU_BLEND_ALPHA;
	else
		return GPU_BLEND_SOLID;
}

void gpu_material_add_node(GPUMaterial *material, GPUNode *node)
{
	BLI_addtail(&material->nodes, node);
}

/* Code generation */

bool GPU_material_do_color_management(GPUMaterial *mat)
{
	return BKE_scene_check_color_management_enabled(mat->scene);
}

bool GPU_material_use_new_shading_nodes(GPUMaterial *mat)
{
	return BKE_scene_use_new_shading_nodes(mat->scene);
}

bool GPU_material_use_world_space_shading(GPUMaterial *mat)
{
	return BKE_scene_use_world_space_shading(mat->scene);
}

static GPUNodeLink *lamp_get_visibility(GPUMaterial *mat, GPULamp *lamp, GPUNodeLink **lv, GPUNodeLink **dist)
{
	Material *ma = mat->ma;
	GPUNodeLink *visifac;

	/* from get_lamp_visibility */
	if (lamp->type == LA_SUN || lamp->type == LA_HEMI) {
		mat->dynproperty |= DYN_LAMP_VEC;
		GPU_link(mat, "lamp_visibility_sun_hemi",
		         GPU_dynamic_uniform(lamp->dynvec, GPU_DYNAMIC_LAMP_DYNVEC, lamp->ob), lv, dist, &visifac);
		return visifac;
	}
	else {
		mat->dynproperty |= DYN_LAMP_CO;
		GPU_link(mat, "lamp_visibility_other",
		         material_builtin(mat, GPU_VIEW_POSITION),
		         GPU_dynamic_uniform(lamp->dynco, GPU_DYNAMIC_LAMP_DYNCO, lamp->ob), lv, dist, &visifac);

		if (lamp->type == LA_AREA)
			return visifac;

		switch (lamp->falloff_type) {
			case LA_FALLOFF_CONSTANT:
				break;
			case LA_FALLOFF_INVLINEAR:
				GPU_link(mat, "lamp_falloff_invlinear",
				         GPU_select_uniform(&lamp->dist, GPU_DYNAMIC_LAMP_DISTANCE, lamp->ob, ma), *dist, &visifac);
				break;
			case LA_FALLOFF_INVSQUARE:
				GPU_link(mat, "lamp_falloff_invsquare",
				         GPU_select_uniform(&lamp->dist, GPU_DYNAMIC_LAMP_DISTANCE, lamp->ob, ma), *dist, &visifac);
				break;
			case LA_FALLOFF_SLIDERS:
				GPU_link(mat, "lamp_falloff_sliders",
				         GPU_select_uniform(&lamp->dist, GPU_DYNAMIC_LAMP_DISTANCE, lamp->ob, ma),
				         GPU_select_uniform(&lamp->att1, GPU_DYNAMIC_LAMP_ATT1, lamp->ob, ma),
				         GPU_select_uniform(&lamp->att2, GPU_DYNAMIC_LAMP_ATT2, lamp->ob, ma), *dist, &visifac);
				break;
			case LA_FALLOFF_INVCOEFFICIENTS:
				GPU_link(mat, "lamp_falloff_invcoefficients",
					     GPU_select_uniform(&lamp->coeff_const, GPU_DYNAMIC_LAMP_COEFFCONST, lamp->ob, ma),
					     GPU_select_uniform(&lamp->coeff_lin, GPU_DYNAMIC_LAMP_COEFFLIN, lamp->ob, ma),
					     GPU_select_uniform(&lamp->coeff_quad, GPU_DYNAMIC_LAMP_COEFFQUAD, lamp->ob, ma), *dist, &visifac);
				break;
			case LA_FALLOFF_CURVE:
			{
				float *array;
				int size;

				curvemapping_initialize(lamp->curfalloff);
				curvemapping_table_RGBA(lamp->curfalloff, &array, &size);
				GPU_link(mat, "lamp_falloff_curve",
				         GPU_select_uniform(&lamp->dist, GPU_DYNAMIC_LAMP_DISTANCE, lamp->ob, ma),
				         GPU_texture(size, array), *dist, &visifac);

				break;
			}
			case LA_FALLOFF_INVSQUARE_CUTOFF:
			{
				GPU_link(mat, "lamp_falloff_invsquarecutoff",
						 GPU_select_uniform(&lamp->radius, GPU_DYNAMIC_LAMP_DISTANCE, lamp->ob, ma),
						 *dist,
						 GPU_select_uniform(&lamp->cutoff, GPU_DYNAMIC_LAMP_CUTOFF, lamp->ob, ma),
						 &visifac);
				break;
			}
		}

		if (lamp->mode & LA_SPHERE)
			GPU_link(mat, "lamp_visibility_sphere",
			         GPU_select_uniform(&lamp->dist, GPU_DYNAMIC_LAMP_DISTANCE, lamp->ob, ma),
			         *dist, visifac, &visifac);

		if (lamp->type == LA_SPOT) {
			GPUNodeLink *inpr;

			if (lamp->mode & LA_SQUARE) {
				mat->dynproperty |= DYN_LAMP_VEC | DYN_LAMP_IMAT;
				GPU_link(mat, "lamp_visibility_spot_square",
				         GPU_dynamic_uniform(lamp->dynvec, GPU_DYNAMIC_LAMP_DYNVEC, lamp->ob),
				         GPU_dynamic_uniform((float *)lamp->dynimat, GPU_DYNAMIC_LAMP_DYNIMAT, lamp->ob),
				         GPU_dynamic_uniform((float *)lamp->spotvec, GPU_DYNAMIC_LAMP_DYNSPOTSCALE, lamp->ob), *lv, &inpr);
			}
			else {
				mat->dynproperty |= DYN_LAMP_VEC | DYN_LAMP_IMAT;
				GPU_link(mat, "lamp_visibility_spot_circle",
				         GPU_dynamic_uniform(lamp->dynvec, GPU_DYNAMIC_LAMP_DYNVEC, lamp->ob),
				         GPU_dynamic_uniform((float *)lamp->dynimat, GPU_DYNAMIC_LAMP_DYNIMAT, lamp->ob),
				         GPU_dynamic_uniform((float *)lamp->spotvec, GPU_DYNAMIC_LAMP_DYNSPOTSCALE, lamp->ob), *lv, &inpr);
			}

			GPU_link(mat, "lamp_visibility_spot",
			         GPU_select_uniform(&lamp->spotsi, GPU_DYNAMIC_LAMP_SPOTSIZE, lamp->ob, ma),
			         GPU_select_uniform(&lamp->spotbl, GPU_DYNAMIC_LAMP_SPOTBLEND, lamp->ob, ma),
			         inpr, visifac, &visifac);
		}

		GPU_link(mat, "lamp_visibility_clamp", visifac, &visifac);

		return visifac;
	}
}

#if 0
static void area_lamp_vectors(LampRen *lar)
{
	float xsize = 0.5f * lar->area_size, ysize = 0.5f * lar->area_sizey;

	/* make it smaller, so area light can be multisampled */
	float multifac = 1.0f / sqrtf((float)lar->ray_totsamp);
	xsize *= multifac;
	ysize *= multifac;

	/* corner vectors */
	lar->area[0][0] = lar->co[0] - xsize * lar->mat[0][0] - ysize * lar->mat[1][0];
	lar->area[0][1] = lar->co[1] - xsize * lar->mat[0][1] - ysize * lar->mat[1][1];
	lar->area[0][2] = lar->co[2] - xsize * lar->mat[0][2] - ysize * lar->mat[1][2];

	/* corner vectors */
	lar->area[1][0] = lar->co[0] - xsize * lar->mat[0][0] + ysize * lar->mat[1][0];
	lar->area[1][1] = lar->co[1] - xsize * lar->mat[0][1] + ysize * lar->mat[1][1];
	lar->area[1][2] = lar->co[2] - xsize * lar->mat[0][2] + ysize * lar->mat[1][2];

	/* corner vectors */
	lar->area[2][0] = lar->co[0] + xsize * lar->mat[0][0] + ysize * lar->mat[1][0];
	lar->area[2][1] = lar->co[1] + xsize * lar->mat[0][1] + ysize * lar->mat[1][1];
	lar->area[2][2] = lar->co[2] + xsize * lar->mat[0][2] + ysize * lar->mat[1][2];

	/* corner vectors */
	lar->area[3][0] = lar->co[0] + xsize * lar->mat[0][0] - ysize * lar->mat[1][0];
	lar->area[3][1] = lar->co[1] + xsize * lar->mat[0][1] - ysize * lar->mat[1][1];
	lar->area[3][2] = lar->co[2] + xsize * lar->mat[0][2] - ysize * lar->mat[1][2];
	/* only for correction button size, matrix size works on energy */
	lar->areasize = lar->dist * lar->dist / (4.0f * xsize * ysize);
}
#endif

static void ramp_blend(
        GPUMaterial *mat, GPUNodeLink *fac, GPUNodeLink *col1, GPUNodeLink *col2, int type,
        GPUNodeLink **r_col)
{
	static const char *names[] = {"mix_blend", "mix_add", "mix_mult", "mix_sub",
		"mix_screen", "mix_div", "mix_diff", "mix_dark", "mix_light",
		"mix_overlay", "mix_dodge", "mix_burn", "mix_hue", "mix_sat",
		"mix_val", "mix_color", "mix_soft", "mix_linear"};

	GPU_link(mat, names[type], fac, col1, col2, r_col);
}

static void BKE_colorband_eval_blend(
        GPUMaterial *mat, ColorBand *coba, GPUNodeLink *fac, float rampfac, int type,
        GPUNodeLink *incol, GPUNodeLink **r_col)
{
	GPUNodeLink *tmp, *alpha, *col;
	float *array;
	int size;

	/* do colorband */
	BKE_colorband_evaluate_table_rgba(coba, &array, &size);
	GPU_link(mat, "valtorgb", fac, GPU_texture(size, array), &col, &tmp);

	/* use alpha in fac */
	GPU_link(mat, "mtex_alpha_from_col", col, &alpha);
	GPU_link(mat, "math_multiply", alpha, GPU_uniform(&rampfac), &fac);

	/* blending method */
	ramp_blend(mat, fac, incol, col, type, r_col);
}

static void ramp_diffuse_result(GPUShadeInput *shi, GPUNodeLink **diff)
{
	Material *ma = shi->mat;
	GPUMaterial *mat = shi->gpumat;

	if (!(mat->scene->gm.flag & GAME_GLSL_NO_RAMPS)) {
		if (ma->ramp_col) {
			if (ma->rampin_col == MA_RAMP_IN_RESULT) {
				GPUNodeLink *fac;
				GPU_link(mat, "ramp_rgbtobw", *diff, &fac);

				/* colorband + blend */
				BKE_colorband_eval_blend(mat, ma->ramp_col, fac, ma->rampfac_col, ma->rampblend_col, *diff, diff);
			}
		}
	}
}

static void add_to_diffuse(
        GPUMaterial *mat, Material *ma, GPUShadeInput *shi, GPUNodeLink *is, GPUNodeLink *rgb,
        GPUNodeLink **r_diff)
{
	GPUNodeLink *fac, *tmp, *addcol;

	if (!(mat->scene->gm.flag & GAME_GLSL_NO_RAMPS) &&
	    ma->ramp_col && (ma->mode & MA_RAMP_COL))
	{
		/* MA_RAMP_IN_RESULT is exceptional */
		if (ma->rampin_col == MA_RAMP_IN_RESULT) {
			addcol = shi->rgb;
		}
		else {
			/* input */
			switch (ma->rampin_col) {
				case MA_RAMP_IN_ENERGY:
					GPU_link(mat, "ramp_rgbtobw", rgb, &fac);
					break;
				case MA_RAMP_IN_SHADER:
					fac = is;
					break;
				case MA_RAMP_IN_NOR:
					GPU_link(mat, "vec_math_dot", shi->view, shi->vn, &tmp, &fac);
					break;
				default:
					GPU_link(mat, "set_value_zero", &fac);
					break;
			}

			/* colorband + blend */
			BKE_colorband_eval_blend(mat, ma->ramp_col, fac, ma->rampfac_col, ma->rampblend_col, shi->rgb, &addcol);
		}
	}
	else
		addcol = shi->rgb;

	/* output to */
	GPU_link(mat, "shade_madd", *r_diff, rgb, addcol, r_diff);
}

static void ramp_spec_result(GPUShadeInput *shi, GPUNodeLink **spec)
{
	Material *ma = shi->mat;
	GPUMaterial *mat = shi->gpumat;

	if (!(mat->scene->gm.flag & GAME_GLSL_NO_RAMPS) &&
	    ma->ramp_spec && ma->rampin_spec == MA_RAMP_IN_RESULT)
	{
		GPUNodeLink *fac;
		GPU_link(mat, "ramp_rgbtobw", *spec, &fac);

		/* colorband + blend */
		BKE_colorband_eval_blend(mat, ma->ramp_spec, fac, ma->rampfac_spec, ma->rampblend_spec, *spec, spec);
	}
}

static void do_specular_ramp(GPUShadeInput *shi, GPUNodeLink *is, GPUNodeLink *t, GPUNodeLink **spec)
{
	Material *ma = shi->mat;
	GPUMaterial *mat = shi->gpumat;
	GPUNodeLink *fac, *tmp;

	*spec = shi->specrgb;

	/* MA_RAMP_IN_RESULT is exception */
	if (ma->ramp_spec && (ma->rampin_spec != MA_RAMP_IN_RESULT)) {

		/* input */
		switch (ma->rampin_spec) {
			case MA_RAMP_IN_ENERGY:
				fac = t;
				break;
			case MA_RAMP_IN_SHADER:
				fac = is;
				break;
			case MA_RAMP_IN_NOR:
				GPU_link(mat, "vec_math_dot", shi->view, shi->vn, &tmp, &fac);
				break;
			default:
				GPU_link(mat, "set_value_zero", &fac);
				break;
		}

		/* colorband + blend */
		BKE_colorband_eval_blend(mat, ma->ramp_spec, fac, ma->rampfac_spec, ma->rampblend_spec, *spec, spec);
	}
}

static void add_user_list(ListBase *list, void *data)
{
	LinkData *link = MEM_callocN(sizeof(LinkData), "GPULinkData");
	link->data = data;
	BLI_addtail(list, link);
}

static void shade_light_textures(GPUMaterial *mat, GPULamp *lamp, GPUNodeLink **rgb)
{
	for (int i = 0; i < MAX_MTEX; ++i) {
		MTex *mtex = lamp->la->mtex[i];

		if (mtex && mtex->tex && (mtex->tex->type & TEX_IMAGE) && mtex->tex->ima) {
			mat->dynproperty |= DYN_LAMP_PERSMAT;

			float one = 1.0f;
			GPUNodeLink *tex_rgb;

			GPU_link(mat, "shade_light_texture",
			         material_builtin(mat, GPU_VIEW_POSITION),
			         GPU_image(mtex->tex->ima, &mtex->tex->iuser, false), GPU_uniform(mtex->size),
					 GPU_select_uniform(&mtex->lodbias, GPU_DYNAMIC_TEX_LODBIAS, NULL, mat->ma),
			         GPU_dynamic_uniform((float *)lamp->dynpersmat, GPU_DYNAMIC_LAMP_DYNPERSMAT, lamp->ob),
			         &tex_rgb);
			texture_rgb_blend(mat, tex_rgb, *rgb, GPU_uniform(&one), GPU_uniform(&mtex->colfac), mtex->blendtype, rgb);
		}
	}
}

static void shade_one_light(GPUShadeInput *shi, GPUShadeResult *shr, GPULamp *lamp)
{
	Material *ma = shi->mat;
	GPUMaterial *mat = shi->gpumat;
	GPUNodeLink *lv, *dist, *is, *inp, *i;
	GPUNodeLink *outcol, *specfac, *t, *shadfac = NULL, *lcol, *col, *energy;
	float one = 1.0f;

	if ((lamp->mode & LA_ONLYSHADOW) && !(ma->mode & MA_SHADOW))
		return;

	GPUNodeLink *vn = shi->vn;
	GPUNodeLink *view = shi->view;

	GPUNodeLink *visifac = lamp_get_visibility(mat, lamp, &lv, &dist);

#if 0
	if (ma->mode & MA_TANGENT_V)
		GPU_link(mat, "shade_tangent_v", lv, GPU_attribute(CD_TANGENT, ""), &vn);
#endif

	GPU_link(mat, "lamp_visible",
		GPU_dynamic_uniform(&lamp->dynlayer, GPU_DYNAMIC_LAMP_DYNVISI, lamp->ob),
		material_builtin(mat, GPU_OBJECT_LAY),
		GPU_dynamic_uniform(lamp->dyncol, GPU_DYNAMIC_LAMP_DYNCOL, lamp->ob),
		GPU_dynamic_uniform(&lamp->dynenergy, GPU_DYNAMIC_LAMP_DYNENERGY, lamp->ob),
		&col, &energy);

	GPU_link(mat, "shade_inp", vn, lv, &inp);

	if (lamp->mode & LA_NO_DIFF) {
		GPU_link(mat, "shade_is_no_diffuse", &is);
	}
	else if (lamp->type == LA_HEMI) {
		GPU_link(mat, "shade_is_hemi", inp, &is);
	}
	else {
		if (lamp->type == LA_AREA) {
			float area[4][4] = {{0.0f}}, areasize = 0.0f;

			mat->dynproperty |= DYN_LAMP_VEC | DYN_LAMP_CO;
			GPU_link(mat, "shade_inp_area",
			         material_builtin(mat, GPU_VIEW_POSITION),
			         GPU_dynamic_uniform(lamp->dynco, GPU_DYNAMIC_LAMP_DYNCO, lamp->ob),
			         GPU_dynamic_uniform(lamp->dynvec, GPU_DYNAMIC_LAMP_DYNVEC, lamp->ob), vn,
			         GPU_uniform((float *)area),
			         GPU_uniform(&areasize),
			         GPU_uniform(&lamp->k), &inp);
		}

		is = inp; /* Lambert */

		if (!(mat->scene->gm.flag & GAME_GLSL_NO_SHADERS)) {
			if (ma->diff_shader == MA_DIFF_ORENNAYAR)
				GPU_link(mat, "shade_diffuse_oren_nayer", inp, vn, lv, view,
				         GPU_uniform(&ma->roughness), &is);
			else if (ma->diff_shader == MA_DIFF_TOON)
				GPU_link(mat, "shade_diffuse_toon", vn, lv, view,
				         GPU_uniform(&ma->param[0]), GPU_uniform(&ma->param[1]), &is);
			else if (ma->diff_shader == MA_DIFF_MINNAERT)
				GPU_link(mat, "shade_diffuse_minnaert", inp, vn, view,
				         GPU_uniform(&ma->darkness), &is);
			else if (ma->diff_shader == MA_DIFF_FRESNEL)
				GPU_link(mat, "shade_diffuse_fresnel", vn, lv, view,
				         GPU_uniform(&ma->param[0]), GPU_uniform(&ma->param[1]), &is);
		}
	}

	if (!(mat->scene->gm.flag & GAME_GLSL_NO_SHADERS))
		if (ma->shade_flag & MA_CUBIC)
			GPU_link(mat, "shade_cubic", is, &is);

	i = is;
	GPU_link(mat, "shade_visifac", i, visifac, shi->refl, &i);

	GPU_link(mat, "set_rgb", col, &lcol);
	shade_light_textures(mat, lamp, &lcol);
	GPU_link(mat, "shade_mul_value_v3", energy, lcol, &lcol);

#if 0
	if (ma->mode & MA_TANGENT_VN)
		GPU_link(mat, "shade_tangent_v_spec", GPU_attribute(CD_TANGENT, ""), &vn);
#endif

	/* this replaces if (i > 0.0) conditional until that is supported */
	/* done in shade_visifac now, GPU_link(mat, "mtex_value_clamp_positive", i, &i); */

	if ((ma->mode & MA_SHADOW) && GPU_lamp_has_shadow_buffer(lamp)) {
		if (!(mat->scene->gm.flag & GAME_GLSL_NO_SHADOWS)) {
			mat->dynproperty |= DYN_LAMP_PERSMAT;

			if (lamp->la->shadowmap_type == LA_SHADMAP_VARIANCE) {
				GPU_link(mat, "shadow_vsm",
				         material_builtin(mat, GPU_VIEW_POSITION),
				         GPU_dynamic_texture(lamp->tex, GPU_DYNAMIC_SAMPLER_2DSHADOW, lamp->ob),
				         GPU_dynamic_uniform((float *)lamp->dynpersmat, GPU_DYNAMIC_LAMP_DYNPERSMAT, lamp->ob),
				         GPU_uniform(&lamp->bias), GPU_uniform(&lamp->la->bleedbias), inp, &shadfac);
			}
			else {
				if (lamp->la->samp > 1 && lamp->la->soft >= 0.01f && lamp->la->shadow_filter != LA_SHADOW_FILTER_NONE) {
					float samp = lamp->la->samp;
					float samplesize = lamp->la->soft / lamp->la->shadow_frustum_size;
					if (lamp->la->shadow_filter == LA_SHADOW_FILTER_PCF) {
						GPU_link(mat, "shadow_pcf",
								 material_builtin(mat, GPU_VIEW_POSITION),
								 material_builtin(mat, GPU_VIEW_NORMAL),
								 GPU_dynamic_texture(lamp->depthtex, GPU_DYNAMIC_SAMPLER_2DSHADOW, lamp->ob),
								 GPU_dynamic_uniform((float *)lamp->dynpersmat, GPU_DYNAMIC_LAMP_DYNPERSMAT, lamp->ob),
								 GPU_uniform(&lamp->bias), GPU_uniform(&lamp->slopebias),
								 GPU_uniform(&samp), GPU_uniform(&samplesize), inp, &shadfac);
					}
					if (lamp->la->shadow_filter == LA_SHADOW_FILTER_PCF_JITTER) {
						GPU_link(mat, "shadow_pcf_jitter",
								 material_builtin(mat, GPU_VIEW_POSITION),
								 material_builtin(mat, GPU_VIEW_NORMAL),
								 GPU_dynamic_texture(lamp->depthtex, GPU_DYNAMIC_SAMPLER_2DSHADOW, lamp->ob),
								 GPU_dynamic_uniform((float *)lamp->dynpersmat, GPU_DYNAMIC_LAMP_DYNPERSMAT, lamp->ob),
								 GPU_uniform(&lamp->bias), GPU_uniform(&lamp->slopebias),
								 GPU_dynamic_texture(GPU_texture_global_jitter_64(), GPU_DYNAMIC_SAMPLER_2DIMAGE, NULL),
								 GPU_uniform(&samp), GPU_uniform(&samplesize), inp, &shadfac);
					}
					else if (lamp->la->shadow_filter == LA_SHADOW_FILTER_PCF_BAIL) {
						GPU_link(mat, "shadow_pcf_early_bail",
								 material_builtin(mat, GPU_VIEW_POSITION),
								 material_builtin(mat, GPU_VIEW_NORMAL),
								 GPU_dynamic_texture(lamp->depthtex, GPU_DYNAMIC_SAMPLER_2DSHADOW, lamp->ob),
								 GPU_dynamic_uniform((float *)lamp->dynpersmat, GPU_DYNAMIC_LAMP_DYNPERSMAT, lamp->ob),
								 GPU_uniform(&lamp->bias), GPU_uniform(&lamp->slopebias),
								 GPU_uniform(&samp), GPU_uniform(&samplesize), inp, &shadfac);
					}
				}
				else {
					GPU_link(mat, "shadow_simple",
				             material_builtin(mat, GPU_VIEW_POSITION),
							 material_builtin(mat, GPU_VIEW_NORMAL),
				             GPU_dynamic_texture(lamp->depthtex, GPU_DYNAMIC_SAMPLER_2DSHADOW, lamp->ob),
				             GPU_dynamic_uniform((float *)lamp->dynpersmat, GPU_DYNAMIC_LAMP_DYNPERSMAT, lamp->ob),
				             GPU_uniform(&lamp->bias), GPU_uniform(&lamp->slopebias), inp, &shadfac);
				}
			}

			if (lamp->mode & LA_ONLYSHADOW) {
				GPUNodeLink *shadrgb;
				GPU_link(mat, "shade_only_shadow", i, shadfac, energy, GPU_uniform(lamp->shadow_color), &shadrgb);

				if (!(lamp->mode & LA_NO_DIFF)) {
					GPU_link(mat, "shade_only_shadow_diffuse", shadrgb, shi->rgb,
					         shr->diff, &shr->diff);
				}

				if (!(lamp->mode & LA_NO_SPEC)) {
					GPU_link(mat, "shade_only_shadow_specular", shadrgb, shi->specrgb,
					         shr->spec, &shr->spec);
				}

				add_user_list(&mat->lamps, lamp);
				add_user_list(&lamp->materials, shi->gpumat->ma);
				return;
			}
		}
	}
	else if ((mat->scene->gm.flag & GAME_GLSL_NO_SHADOWS) && (lamp->mode & LA_ONLYSHADOW)) {
		add_user_list(&mat->lamps, lamp);
		add_user_list(&lamp->materials, shi->gpumat->ma);
		return;
	}
	else
		GPU_link(mat, "set_value", GPU_uniform(&one), &shadfac);

	if (ma->sss_flag && lamp->type != LA_SPOT) {
		GPU_link(mat, "set_sss", energy, visifac, col,
								 GPU_uniform(&ma->sss_scale), GPU_uniform((float *)&ma->sss_radius),
								 shi->rgb, i, view, lv, vn, &shr->combined);
		GPU_link(mat, "shade_add", shr->combined, shr->diff, &shr->diff);
	}		
			
	if (GPU_link_changed(shi->refl) || ma->ref != 0.0f || !(ma->constflag & MA_CONSTANT_MATERIAL)) {
		if (!(lamp->mode & LA_NO_DIFF)) {
			GPUNodeLink *rgb;
			GPU_link(mat, "shade_mul_value", i, lcol, &rgb);
			GPU_link(mat, "mtex_value_invert", shadfac, &shadfac);
			GPU_link(mat, "mix_mult",  shadfac, rgb, GPU_uniform(lamp->shadow_color), &rgb);
			GPU_link(mat, "mtex_value_invert", shadfac, &shadfac);
			add_to_diffuse(mat, ma, shi, is, rgb, &shr->diff);
		}
	}

	if (mat->scene->gm.flag & GAME_GLSL_NO_SHADERS) {
		/* pass */
	}
	else if (!(lamp->mode & LA_NO_SPEC) && !(lamp->mode & LA_ONLYSHADOW) &&
		(GPU_link_changed(shi->spec) || ma->spec != 0.0f || !(ma->constflag & MA_CONSTANT_MATERIAL)))
	{
		if (lamp->type == LA_HEMI) {
			GPU_link(mat, "shade_hemi_spec", vn, lv, view, GPU_select_uniform(&ma->spec, GPU_DYNAMIC_MAT_SPEC, NULL, ma), shi->har, visifac, &t);
			GPU_link(mat, "shade_add_spec", t, lcol, shi->specrgb, &outcol);
			GPU_link(mat, "shade_add_clamped", shr->spec, outcol, &shr->spec);
		}
		else {
			if (ma->spec_shader == MA_SPEC_PHONG) {
				GPU_link(mat, "shade_phong_spec", vn, lv, view, shi->har, &specfac);
			}
			else if (ma->spec_shader == MA_SPEC_COOKTORR) {
				GPU_link(mat, "shade_cooktorr_spec", vn, lv, view, shi->har, &specfac);
			}
			else if (ma->spec_shader == MA_SPEC_BLINN) {
				GPU_link(mat, "shade_blinn_spec", vn, lv, view,
				         GPU_uniform(&ma->refrac), shi->har, &specfac);
			}
			else if (ma->spec_shader == MA_SPEC_WARDISO) {
				GPU_link(mat, "shade_wardiso_spec", vn, lv, view,
				         GPU_uniform(&ma->rms), &specfac);
			}
			else {
				GPU_link(mat, "shade_toon_spec", vn, lv, view,
				         GPU_uniform(&ma->param[2]), GPU_uniform(&ma->param[3]), &specfac);
			}

			if (lamp->type == LA_AREA)
				GPU_link(mat, "shade_spec_area_inp", specfac, inp, &specfac);

			GPU_link(mat, "shade_spec_t", shadfac, shi->spec, visifac, specfac, &t);

			if (ma->mode & MA_RAMP_SPEC) {
				GPUNodeLink *spec;
				do_specular_ramp(shi, specfac, t, &spec);
				GPU_link(mat, "shade_add_spec", t, lcol, spec, &outcol);
				GPU_link(mat, "shade_add_clamped", shr->spec, outcol, &shr->spec);
			}
			else {
				GPU_link(mat, "shade_add_spec", t, lcol, shi->specrgb, &outcol);
				GPU_link(mat, "shade_add_clamped", shr->spec, outcol, &shr->spec);
			}
		}
	}

	add_user_list(&mat->lamps, lamp);
	add_user_list(&lamp->materials, shi->gpumat->ma);
}

static void material_lights(GPUShadeInput *shi, GPUShadeResult *shr)
{
	Base *base;
	Scene *sce_iter;

	for (SETLOOPER(shi->gpumat->scene, sce_iter, base)) {
		Object *ob = base->object;

		if (ob->type == OB_LAMP) {
			GPULamp *lamp = GPU_lamp_from_blender(shi->gpumat->scene, ob, NULL);
			if (lamp)
				shade_one_light(shi, shr, lamp);
		}

		if (ob->transflag & OB_DUPLI) {
			ListBase *lb = object_duplilist(G.main, G.main->eval_ctx, shi->gpumat->scene, ob);

			for (DupliObject *dob = lb->first; dob; dob = dob->next) {
				Object *ob_iter = dob->ob;

				if (ob_iter->type == OB_LAMP) {
					float omat[4][4];
					copy_m4_m4(omat, ob_iter->obmat);
					copy_m4_m4(ob_iter->obmat, dob->mat);

					GPULamp *lamp = GPU_lamp_from_blender(shi->gpumat->scene, ob_iter, ob);
					if (lamp)
						shade_one_light(shi, shr, lamp);

					copy_m4_m4(ob_iter->obmat, omat);
				}
			}

			free_object_duplilist(lb);
		}
	}

	/* prevent only shadow lamps from producing negative colors.*/
	GPU_link(shi->gpumat, "shade_clamp_positive", shr->spec, &shr->spec);
	GPU_link(shi->gpumat, "shade_clamp_positive", shr->diff, &shr->diff);
}

static void texture_rgb_blend(
        GPUMaterial *mat, GPUNodeLink *tex, GPUNodeLink *out, GPUNodeLink *fact, GPUNodeLink *facg,
        int blendtype, GPUNodeLink **in)
{
	switch (blendtype) {
		case MTEX_BLEND:
			GPU_link(mat, "mtex_rgb_blend", out, tex, fact, facg, in);
			break;
		case MTEX_MUL:
			GPU_link(mat, "mtex_rgb_mul", out, tex, fact, facg, in);
			break;
		case MTEX_SCREEN:
			GPU_link(mat, "mtex_rgb_screen", out, tex, fact, facg, in);
			break;
		case MTEX_OVERLAY:
			GPU_link(mat, "mtex_rgb_overlay", out, tex, fact, facg, in);
			break;
		case MTEX_SUB:
			GPU_link(mat, "mtex_rgb_sub", out, tex, fact, facg, in);
			break;
		case MTEX_ADD:
			GPU_link(mat, "mtex_rgb_add", out, tex, fact, facg, in);
			break;
		case MTEX_DIV:
			GPU_link(mat, "mtex_rgb_div", out, tex, fact, facg, in);
			break;
		case MTEX_DIFF:
			GPU_link(mat, "mtex_rgb_diff", out, tex, fact, facg, in);
			break;
		case MTEX_DARK:
			GPU_link(mat, "mtex_rgb_dark", out, tex, fact, facg, in);
			break;
		case MTEX_LIGHT:
			GPU_link(mat, "mtex_rgb_light", out, tex, fact, facg, in);
			break;
		case MTEX_BLEND_HUE:
			GPU_link(mat, "mtex_rgb_hue", out, tex, fact, facg, in);
			break;
		case MTEX_BLEND_SAT:
			GPU_link(mat, "mtex_rgb_sat", out, tex, fact, facg, in);
			break;
		case MTEX_BLEND_VAL:
			GPU_link(mat, "mtex_rgb_val", out, tex, fact, facg, in);
			break;
		case MTEX_BLEND_COLOR:
			GPU_link(mat, "mtex_rgb_color", out, tex, fact, facg, in);
			break;
		case MTEX_SOFT_LIGHT:
			GPU_link(mat, "mtex_rgb_soft", out, tex, fact, facg, in);
			break;
		case MTEX_LIN_LIGHT:
			GPU_link(mat, "mtex_rgb_linear", out, tex, fact, facg, in);
			break;
		default:
			GPU_link(mat, "set_rgb_zero", &in);
			break;
	}
}

static void texture_value_blend(
        GPUMaterial *mat, GPUNodeLink *tex, GPUNodeLink *out, GPUNodeLink *fact, GPUNodeLink *facg,
        int blendtype, GPUNodeLink **in)
{
	switch (blendtype) {
		case MTEX_BLEND:
			GPU_link(mat, "mtex_value_blend", out, tex, fact, facg, in);
			break;
		case MTEX_MUL:
			GPU_link(mat, "mtex_value_mul", out, tex, fact, facg, in);
			break;
		case MTEX_SCREEN:
			GPU_link(mat, "mtex_value_screen", out, tex, fact, facg, in);
			break;
		case MTEX_SUB:
			GPU_link(mat, "mtex_value_sub", out, tex, fact, facg, in);
			break;
		case MTEX_ADD:
			GPU_link(mat, "mtex_value_add", out, tex, fact, facg, in);
			break;
		case MTEX_DIV:
			GPU_link(mat, "mtex_value_div", out, tex, fact, facg, in);
			break;
		case MTEX_DIFF:
			GPU_link(mat, "mtex_value_diff", out, tex, fact, facg, in);
			break;
		case MTEX_DARK:
			GPU_link(mat, "mtex_value_dark", out, tex, fact, facg, in);
			break;
		case MTEX_LIGHT:
			GPU_link(mat, "mtex_value_light", out, tex, fact, facg, in);
			break;
		default:
			GPU_link(mat, "set_value_zero", &in);
			break;
	}
}

static void do_material_tex(GPUShadeInput *shi)
{
	Material *ma = shi->mat;
	GPUMaterial *mat = shi->gpumat;
	MTex *mtex;
	Tex *tex;
	GPUNodeLink *texco, *tin, *trgb, *tnor, *tcol, *stencil, *tnorfac, *tangent;
	GPUNodeLink *texco_norm, *texco_orco, *texco_object;
	GPUNodeLink *texco_global, *texco_uv = NULL;
	GPUNodeLink *newnor, *orn;
	float one = 1.0f;
	GPUNodeLink *parco = NULL;
	int rgbnor, talpha;
	bool init_done = false;
	float discard;
	int tex_nr;
	int iBumpSpacePrev = 0; /* Not necessary, quieting gcc warning. */
	GPUNodeLink *vNorg, *vNacc, *fPrevMagnitude;
	int iFirstTimeNMap = 1;
	bool found_deriv_map = false;

	GPU_link(mat, "set_value", GPU_uniform(&one), &stencil);

	GPU_link(mat, "texco_norm", material_builtin(mat, GPU_VIEW_NORMAL), &texco_norm);
	GPU_link(mat, "texco_orco", GPU_attribute(CD_ORCO, ""), &texco_orco);
	GPU_link(mat, "texco_object", material_builtin(mat, GPU_INVERSE_VIEW_MATRIX),
		material_builtin(mat, GPU_INVERSE_OBJECT_MATRIX),
		material_builtin(mat, GPU_VIEW_POSITION), &texco_object);
#if 0
	GPU_link(mat, "texco_tangent", GPU_attribute(CD_TANGENT, ""), &texco_tangent);
#endif
	GPU_link(mat, "texco_global", material_builtin(mat, GPU_INVERSE_VIEW_MATRIX),
		material_builtin(mat, GPU_VIEW_POSITION), &texco_global);

	orn = texco_norm;
	
	/* find parallax texco (parco) */
	for (tex_nr = 0; tex_nr < MAX_MTEX; tex_nr++) {
		/* separate tex switching */
		if (ma->septex & (1 << tex_nr)) continue;
		if (ma->mtex[tex_nr]) {
			mtex = ma->mtex[tex_nr];
			tex = mtex->tex;

			if (tex == NULL || !(mtex->mapto & MAP_PARALLAX)) {
				continue;
			}

			tangent = GPU_attribute(CD_TANGENT, "");
			if (!(ma->constflag & MA_CONSTANT_TEXTURE_UV) || (mtex->rot != 0.0f)) {
				GPU_link(mat, "mtex_tangent_rotate", tangent, orn,
					GPU_select_uniform(&mtex->rot, GPU_DYNAMIC_TEX_UVROTATION, NULL, ma),
					&tangent);
			}

			GPU_link(mat, "texco_uv", GPU_attribute(CD_MTFACE, mtex->uvname), &texco_uv);
			texco = texco_uv;

			if (!(ma->constflag & MA_CONSTANT_TEXTURE_UV) ||
				((mtex->size[0] != 1.0f || mtex->size[1] != 1.0f || mtex->size[2] != 1.0f) ||
				(mtex->ofs[0] == 0.0f || mtex->ofs[1] == 0.0f) ||
				(mtex->rot != 0.0f)))
			{
				GPU_link(mat, "mtex_mapping_transform", texco,
						GPU_select_uniform(&mtex->rot, GPU_DYNAMIC_TEX_UVROTATION, NULL, ma),
						GPU_select_uniform(mtex->ofs, GPU_DYNAMIC_TEX_UVOFFSET, NULL, ma),
						GPU_select_uniform(mtex->size, GPU_DYNAMIC_TEX_UVSIZE, NULL, ma),
						&texco);
			}

			discard = (mtex->parflag & MTEX_DISCARD_AT_EDGES) != 0 ? 1.0f : 0.0f;
			float comp = 3.0f; // alpha
			GPU_link(mat, "mtex_parallax", texco,
					 material_builtin(mat, GPU_VIEW_POSITION), tangent, orn,
					 GPU_image(tex->ima, &tex->iuser, false),
					 GPU_select_uniform(&mtex->parallaxsteps, GPU_DYNAMIC_TEX_PARALLAXSTEP, NULL, ma),
					 GPU_select_uniform(&mtex->parallaxbumpsc, GPU_DYNAMIC_TEX_PARALLAXBUMP, NULL, ma),
					 GPU_select_uniform(mtex->size, GPU_DYNAMIC_TEX_UVSIZE, NULL, ma),
					 GPU_uniform(&discard),
					 GPU_uniform(&comp),
					 &parco);

			// only one parallax per material.
			break;
		}
	}

	/* go over texture slots */
	for (tex_nr = 0; tex_nr < MAX_MTEX; tex_nr++) {
		/* separate tex switching */
		if (ma->septex & (1 << tex_nr)) continue;

		if (ma->mtex[tex_nr]) {
			mtex = ma->mtex[tex_nr];
			bool use_parallax = (mtex->texflag & MTEX_PARALLAX_UV) || (mtex->mapto & MAP_PARALLAX);

			tex = mtex->tex;
			if (tex == NULL) continue;

			/* which coords */
			if (mtex->texco == TEXCO_ORCO)
				texco = texco_orco;
			else if (mtex->texco == TEXCO_OBJECT)
				texco = texco_object;
			else if (mtex->texco == TEXCO_NORM)
				texco = orn;
			else if (mtex->texco == TEXCO_TANGENT)
				texco = texco_object;
			else if (mtex->texco == TEXCO_GLOB)
				texco = texco_global;
			else if (mtex->texco == TEXCO_REFL) {
				GPU_link(mat, "texco_refl", shi->vn, shi->view, &shi->ref);
				texco = shi->ref;
			}
			else if (mtex->texco == TEXCO_UV) {
				if (1) { //!(texco_uv && strcmp(mtex->uvname, lastuvname) == 0)) {
					GPU_link(mat, "texco_uv", GPU_attribute(CD_MTFACE, mtex->uvname), &texco_uv);
					/*lastuvname = mtex->uvname;*/ /*UNUSED*/
				}
				texco = texco_uv;
			}
			else
				continue;
			
			/*if parallax has modified uv*/
			if (use_parallax && parco) {
				texco = parco;
			}
			/* in case of uv, this would just undo a multiplication in texco_uv */
			if (mtex->texco != TEXCO_UV)
				GPU_link(mat, "mtex_2d_mapping", texco, &texco);

			if (!use_parallax && (!(ma->constflag & MA_CONSTANT_TEXTURE_UV) ||
				(mtex->size[0] != 1.0f || mtex->size[1] != 1.0f || mtex->size[2] != 1.0f) ||
				(mtex->ofs[0] == 0.0f || mtex->ofs[1] == 0.0f) ||
				(mtex->rot != 0.0f)))
			{
				GPU_link(mat, "mtex_mapping_transform", texco,
						 GPU_select_uniform(&mtex->rot, GPU_DYNAMIC_TEX_UVROTATION, NULL, ma),
						 GPU_select_uniform(mtex->ofs, GPU_DYNAMIC_TEX_UVOFFSET, NULL, ma),
						 GPU_select_uniform(mtex->size, GPU_DYNAMIC_TEX_UVSIZE, NULL, ma),
						 &texco);
			}

			talpha = 0;

			if (tex && tex->ima &&
			    ((tex->type == TEX_IMAGE) ||
			     ((tex->type == TEX_ENVMAP) && (mtex->texco == TEXCO_REFL))))
			{
				if (tex->type == TEX_IMAGE) {
					GPU_link(mat, "mtex_image", texco, GPU_image(tex->ima, &tex->iuser, false),
							 GPU_select_uniform(&mtex->lodbias, GPU_DYNAMIC_TEX_LODBIAS, NULL, ma), &tin, &trgb);
				}
				else if (tex->type == TEX_ENVMAP) {
					if (tex->env->type == ENV_PLANE) {
						GPU_link(mat, "mtex_image_refl",
							material_builtin(mat, GPU_VIEW_POSITION),
							material_builtin(mat, GPU_CAMERA_TEXCO_FACTORS),
							GPU_image(tex->ima, &tex->iuser, false),
							GPU_select_uniform(&mtex->lodbias, GPU_DYNAMIC_TEX_LODBIAS, NULL, ma),
							material_builtin(mat, GPU_OBJECT_MATRIX),
							material_builtin(mat, GPU_VIEW_MATRIX),
							shi->view, shi->vn, &tin, &trgb);
					}
					else if (tex->env->type == ENV_CUBE) {
						GPU_link(mat, "mtex_cube_map_refl_refr",
							 GPU_cube_map(tex->ima, &tex->iuser, false), shi->view, shi->vn,
							 GPU_select_uniform(&mtex->lodbias, GPU_DYNAMIC_TEX_LODBIAS, NULL, ma),
							 material_builtin(mat, GPU_INVERSE_VIEW_MATRIX),
							 GPU_select_uniform(&mtex->ior, GPU_DYNAMIC_TEX_IOR, NULL, ma),
							 GPU_select_uniform(&mtex->refrratio, GPU_DYNAMIC_TEX_REFRRATIO, NULL, ma),
							 &tin, &trgb);
					}
				}
				rgbnor = TEX_RGB;

				talpha = ((tex->imaflag & TEX_USEALPHA) && tex->ima && (tex->ima->flag & IMA_IGNORE_ALPHA) == 0);
			}
			else {
				continue;
			}

			/* texture output */
			if ((rgbnor & TEX_RGB) && (mtex->texflag & MTEX_RGBTOINT)) {
				GPU_link(mat, "mtex_rgbtoint", trgb, &tin);
				rgbnor -= TEX_RGB;
			}

			if (mtex->texflag & MTEX_NEGATIVE) {
				if (rgbnor & TEX_RGB)
					GPU_link(mat, "mtex_rgb_invert", trgb, &trgb);
				else
					GPU_link(mat, "mtex_value_invert", tin, &tin);
			}

			if (mtex->texflag & MTEX_STENCIL) {
				if (rgbnor & TEX_RGB)
					GPU_link(mat, "mtex_rgb_stencil", stencil, trgb, &stencil, &trgb);
				else
					GPU_link(mat, "mtex_value_stencil", stencil, tin, &stencil, &tin);
			}

			/* mapping */
			if (mtex->mapto & (MAP_COL | MAP_COLSPEC | MAP_COLMIR)) {
				/* stencil maps on the texture control slider, not texture intensity value */
				if ((rgbnor & TEX_RGB) == 0) {
					GPU_link(mat, "set_rgb", GPU_uniform(&mtex->r), &tcol);
				}
				else {
					GPU_link(mat, "set_rgba", trgb, &tcol);

					if (mtex->mapto & MAP_ALPHA)
						GPU_link(mat, "set_value", stencil, &tin);
					else if (talpha)
						GPU_link(mat, "mtex_alpha_from_col", trgb, &tin);
					else
						GPU_link(mat, "set_value_one", &tin);
				}

				if (tex_do_color_management(mat, mtex, tex)) {
					GPU_link(mat, "srgb_to_linearrgb", tcol, &tcol);
				}

				if (mtex->mapto & MAP_COL) {
					GPUNodeLink *colfac;

					if (mtex->colfac == 1.0f && (ma->constflag & MA_CONSTANT_TEXTURE)) colfac = stencil;
					else GPU_link(mat, "math_multiply", GPU_select_uniform(&mtex->colfac, GPU_DYNAMIC_TEX_COLFAC, NULL, ma), stencil, &colfac);

					texture_rgb_blend(mat, tcol, shi->rgb, tin, colfac, mtex->blendtype, &shi->rgb);
				}

				if (!(mat->scene->gm.flag & GAME_GLSL_NO_EXTRA_TEX) && (mtex->mapto & MAP_COLSPEC)) {
					GPUNodeLink *colspecfac;

					if (mtex->colspecfac == 1.0f && (ma->constflag & MA_CONSTANT_TEXTURE)) colspecfac = stencil;
					else GPU_link(mat, "math_multiply", GPU_select_uniform(&mtex->colspecfac, GPU_DYNAMIC_TEX_SPECFAC, NULL, ma), stencil, &colspecfac);

					texture_rgb_blend(mat, tcol, shi->specrgb, tin, colspecfac, mtex->blendtype, &shi->specrgb);
				}

				if (mtex->mapto & MAP_COLMIR) {
					GPUNodeLink *colmirfac;

					if (mtex->mirrfac == 1.0f && (ma->constflag & MA_CONSTANT_TEXTURE)) colmirfac = stencil;
					else GPU_link(mat, "math_multiply", GPU_select_uniform(&mtex->mirrfac, GPU_DYNAMIC_TEX_MIRROR, NULL, ma), stencil, &colmirfac);

					/* exception for envmap only */
					if (tex->type == TEX_ENVMAP && mtex->blendtype == MTEX_BLEND) {
						GPU_link(mat, "mtex_mirror", tcol, shi->refcol, tin, colmirfac, &shi->refcol);
					}
					else
						texture_rgb_blend(mat, tcol, shi->mir, tin, colmirfac, mtex->blendtype, &shi->mir);
				}
			}

			if (!(mat->scene->gm.flag & GAME_GLSL_NO_EXTRA_TEX) && (mtex->mapto & MAP_NORM)) {
				if (tex->type == TEX_IMAGE) {
					found_deriv_map = tex->imaflag & TEX_DERIVATIVEMAP;

					if (tex->imaflag & TEX_NORMALMAP) {
						/* normalmap image */
						GPU_link(mat, "mtex_normal", texco, GPU_image(tex->ima, &tex->iuser, true),
								 GPU_select_uniform(&mtex->lodbias, GPU_DYNAMIC_TEX_LODBIAS, NULL, ma), &tnor);

						if (mtex->norfac < 0.0f)
							GPU_link(mat, "mtex_negate_texnormal", tnor, &tnor);

						if (mtex->normapspace == MTEX_NSPACE_TANGENT) {
							tangent = GPU_attribute(CD_TANGENT, "");
							if (!(ma->constflag & MA_CONSTANT_TEXTURE_UV) || (mtex->rot != 0.0f)) {
								GPU_link(mat, "mtex_tangent_rotate", tangent, orn,
										GPU_select_uniform(&mtex->rot, GPU_DYNAMIC_TEX_UVROTATION, NULL, ma),
										&tangent);
							}

							if (iFirstTimeNMap != 0) {
								// use unnormalized normal (this is how we bake it - closer to gamedev)
								GPUNodeLink *vNegNorm;
								GPU_link(mat, "vec_math_negate",
								         material_builtin(mat, GPU_VIEW_NORMAL), &vNegNorm);
								GPU_link(mat, "mtex_nspace_tangent",
								         tangent, vNegNorm, tnor, &newnor);
								iFirstTimeNMap = 0;
							}
							else { /* otherwise use accumulated perturbations */
								GPU_link(mat, "mtex_nspace_tangent",
								         tangent, shi->vn, tnor, &newnor);
							}
						}
						else if (mtex->normapspace == MTEX_NSPACE_OBJECT) {
							/* transform normal by object then view matrix */
							GPU_link(mat, "mtex_nspace_object", tnor, &newnor);
						}
						else if (mtex->normapspace == MTEX_NSPACE_WORLD) {
							/* transform normal by view matrix */
							GPU_link(mat, "mtex_nspace_world", material_builtin(mat, GPU_VIEW_MATRIX), tnor, &newnor);
						}
						else {
							/* no transform, normal in camera space */
							newnor = tnor;
						}

						float norfac = min_ff(fabsf(mtex->norfac), 1.0f);

						if (norfac == 1.0f && !GPU_link_changed(stencil) && (ma->constflag & MA_CONSTANT_TEXTURE)) {
							shi->vn = newnor;
						}
						else {
							tnorfac = GPU_select_uniform(&mtex->norfac, GPU_DYNAMIC_TEX_NORMAL, NULL, ma);

							if (GPU_link_changed(stencil))
								GPU_link(mat, "math_multiply", tnorfac, stencil, &tnorfac);

							GPU_link(mat, "mtex_blend_normal", tnorfac, shi->vn, newnor, &shi->vn);
						}

					}
					else if (found_deriv_map ||
					         (mtex->texflag & (MTEX_3TAP_BUMP | MTEX_5TAP_BUMP | MTEX_BICUBIC_BUMP)))
					{
						/* ntap bumpmap image */
						int iBumpSpace;
						float ima_x, ima_y;

						float imag_tspace_dimension_x = 1024.0f; /* only used for texture space variant */
						float aspect = 1.0f;

						GPUNodeLink *vR1, *vR2;
						GPUNodeLink *dBs, *dBt, *fDet;

						float hScale = 0.1f; /* compatibility adjustment factor for all bumpspace types */
						if (mtex->texflag & MTEX_BUMP_TEXTURESPACE)
							hScale = 13.0f; /* factor for scaling texspace bumps */
						else if (found_deriv_map)
							hScale = 1.0f;

						/* resolve texture resolution */
						if ((mtex->texflag & MTEX_BUMP_TEXTURESPACE) || found_deriv_map) {
							ImBuf *ibuf = BKE_image_acquire_ibuf(tex->ima, &tex->iuser, NULL);
							ima_x = 512.0f; ima_y = 512.0f; /* prevent calling textureSize, glsl 1.3 only */
							if (ibuf) {
								ima_x = ibuf->x;
								ima_y = ibuf->y;
								aspect = (float)ima_y / ima_x;
							}
							BKE_image_release_ibuf(tex->ima, ibuf, NULL);
						}

						/* The negate on norfac is done because the
						 * normal in the renderer points inward which corresponds
						 * to inverting the bump map. Should this ever change
						 * this negate must be removed. */
						float norfac = -hScale * mtex->norfac;
						if (found_deriv_map) {
							float fVirtDim = sqrtf(fabsf(ima_x * mtex->size[0] * ima_y * mtex->size[1]));
							norfac /= MAX2(fVirtDim, FLT_EPSILON);
						}

						tnorfac = GPU_uniform(&norfac);

						if (found_deriv_map)
							GPU_link(mat, "math_multiply", tnorfac, material_builtin(mat, GPU_AUTO_BUMPSCALE), &tnorfac);

						if (GPU_link_changed(stencil))
							GPU_link(mat, "math_multiply", tnorfac, stencil, &tnorfac);

						if (!init_done) {
							/* copy shi->vn to vNorg and vNacc, set magnitude to 1 */
							GPU_link(mat, "mtex_bump_normals_init", shi->vn, &vNorg, &vNacc, &fPrevMagnitude);
							iBumpSpacePrev = 0;
							init_done = true;
						}

						// find current bump space
						if (mtex->texflag & MTEX_BUMP_OBJECTSPACE)
							iBumpSpace = 1;
						else if (mtex->texflag & MTEX_BUMP_TEXTURESPACE)
							iBumpSpace = 2;
						else
							iBumpSpace = 4; /* ViewSpace */

						/* re-initialize if bump space changed */
						if (iBumpSpacePrev != iBumpSpace) {
							GPUNodeLink *surf_pos = material_builtin(mat, GPU_VIEW_POSITION);

							if (mtex->texflag & MTEX_BUMP_OBJECTSPACE)
								GPU_link(mat, "mtex_bump_init_objspace",
								         surf_pos, vNorg,
								         material_builtin(mat, GPU_VIEW_MATRIX),
								         material_builtin(mat, GPU_INVERSE_VIEW_MATRIX),
								         material_builtin(mat, GPU_OBJECT_MATRIX),
								         material_builtin(mat, GPU_INVERSE_OBJECT_MATRIX),
								         fPrevMagnitude, vNacc,
								         &fPrevMagnitude, &vNacc,
								         &vR1, &vR2, &fDet);

							else if (mtex->texflag & MTEX_BUMP_TEXTURESPACE)
								GPU_link(mat, "mtex_bump_init_texturespace",
								         surf_pos, vNorg,
								         fPrevMagnitude, vNacc,
								         &fPrevMagnitude, &vNacc,
								         &vR1, &vR2, &fDet);

							else
								GPU_link(mat, "mtex_bump_init_viewspace",
								         surf_pos, vNorg,
								         fPrevMagnitude, vNacc,
								         &fPrevMagnitude, &vNacc,
								         &vR1, &vR2, &fDet);

							iBumpSpacePrev = iBumpSpace;
						}


						if (found_deriv_map) {
							GPU_link(mat, "mtex_bump_deriv",
							         texco, GPU_image(tex->ima, &tex->iuser, true),
							         GPU_uniform(&ima_x), GPU_uniform(&ima_y), tnorfac,
									 GPU_select_uniform(&mtex->lodbias, GPU_DYNAMIC_TEX_LODBIAS, NULL, ma),
							         &dBs, &dBt);
						}
						else if (mtex->texflag & MTEX_3TAP_BUMP)
							GPU_link(mat, "mtex_bump_tap3",
							         texco, GPU_image(tex->ima, &tex->iuser, true), tnorfac,
									 GPU_select_uniform(&mtex->lodbias, GPU_DYNAMIC_TEX_LODBIAS, NULL, ma),
							         &dBs, &dBt);
						else if (mtex->texflag & MTEX_5TAP_BUMP)
							GPU_link(mat, "mtex_bump_tap5",
							         texco, GPU_image(tex->ima, &tex->iuser, true), tnorfac,
									 GPU_select_uniform(&mtex->lodbias, GPU_DYNAMIC_TEX_LODBIAS, NULL, ma),
							         &dBs, &dBt);
						else if (mtex->texflag & MTEX_BICUBIC_BUMP) {
							if (GPU_bicubic_bump_support()) {
								GPU_link(mat, "mtex_bump_bicubic",
								         texco, GPU_image(tex->ima, &tex->iuser, true), tnorfac,
										 GPU_select_uniform(&mtex->lodbias, GPU_DYNAMIC_TEX_LODBIAS, NULL, ma),
								         &dBs, &dBt);
							}
							else {
								GPU_link(mat, "mtex_bump_tap5",
								         texco, GPU_image(tex->ima, &tex->iuser, true), tnorfac,
										 GPU_select_uniform(&mtex->lodbias, GPU_DYNAMIC_TEX_LODBIAS, NULL, ma),
								         &dBs, &dBt);
							}
						}


						if (mtex->texflag & MTEX_BUMP_TEXTURESPACE) {
							float imag_tspace_dimension_y = aspect * imag_tspace_dimension_x;
							GPU_link(mat, "mtex_bump_apply_texspace",
							         fDet, dBs, dBt, vR1, vR2,
							         GPU_image(tex->ima, &tex->iuser, true), texco,
							         GPU_uniform(&imag_tspace_dimension_x),
							         GPU_uniform(&imag_tspace_dimension_y), vNacc,
							         &vNacc, &shi->vn);
						}
						else
							GPU_link(mat, "mtex_bump_apply",
							         fDet, dBs, dBt, vR1, vR2, vNacc,
							         &vNacc, &shi->vn);

					}
				}

				GPU_link(mat, "vec_math_negate", shi->vn, &orn);
			}

			if ((mtex->mapto & MAP_VARS)) {
				if (rgbnor & TEX_RGB) {
					if (talpha)
						GPU_link(mat, "mtex_alpha_from_col", trgb, &tin);
					else
						GPU_link(mat, "mtex_rgbtoint", trgb, &tin);
				}

				if (!(mat->scene->gm.flag & GAME_GLSL_NO_EXTRA_TEX) && mtex->mapto & MAP_REF) {
					GPUNodeLink *difffac;

					if (mtex->difffac == 1.0f && (ma->constflag & MA_CONSTANT_TEXTURE)) difffac = stencil;
					else GPU_link(mat, "math_multiply", GPU_select_uniform(&mtex->difffac, GPU_DYNAMIC_TEX_COLINTENS, NULL, ma), stencil, &difffac);

					texture_value_blend(
					        mat, GPU_uniform(&mtex->def_var), shi->refl, tin, difffac,
					        mtex->blendtype, &shi->refl);
					GPU_link(mat, "mtex_value_clamp_positive", shi->refl, &shi->refl);
				}
				if (!(mat->scene->gm.flag & GAME_GLSL_NO_EXTRA_TEX) && mtex->mapto & MAP_SPEC) {
					GPUNodeLink *specfac;

					if (mtex->specfac == 1.0f && (ma->constflag & MA_CONSTANT_TEXTURE)) specfac = stencil;
					else GPU_link(mat, "math_multiply", GPU_select_uniform(&mtex->specfac, GPU_DYNAMIC_TEX_SPECINTENS, NULL, ma), stencil, &specfac);

					texture_value_blend(
					        mat, GPU_uniform(&mtex->def_var), shi->spec, tin, specfac,
					        mtex->blendtype, &shi->spec);
					GPU_link(mat, "mtex_value_clamp_positive", shi->spec, &shi->spec);
				}
				if (!(mat->scene->gm.flag & GAME_GLSL_NO_EXTRA_TEX) && mtex->mapto & MAP_EMIT) {
					GPUNodeLink *emitfac;

					if (mtex->emitfac == 1.0f && (ma->constflag & MA_CONSTANT_TEXTURE)) emitfac = stencil;
					else GPU_link(mat, "math_multiply", GPU_select_uniform(&mtex->emitfac, GPU_DYNAMIC_TEX_EMIT, NULL, ma), stencil, &emitfac);

					texture_value_blend(
					        mat, GPU_uniform(&mtex->def_var), shi->emit, tin, emitfac,
					        mtex->blendtype, &shi->emit);
					GPU_link(mat, "mtex_value_clamp_positive", shi->emit, &shi->emit);
				}
				if (!(mat->scene->gm.flag & GAME_GLSL_NO_EXTRA_TEX) && mtex->mapto & MAP_HAR) {
					GPUNodeLink *hardfac;

					if (mtex->hardfac == 1.0f && (ma->constflag & MA_CONSTANT_TEXTURE)) hardfac = stencil;
					else GPU_link(mat, "math_multiply", GPU_select_uniform(&mtex->hardfac, GPU_DYNAMIC_TEX_HARDNESS, NULL, ma), stencil, &hardfac);

					GPU_link(mat, "mtex_har_divide", shi->har, &shi->har);
					texture_value_blend(
					        mat, GPU_uniform(&mtex->def_var), shi->har, tin, hardfac,
					        mtex->blendtype, &shi->har);
					GPU_link(mat, "mtex_har_multiply_clamp", shi->har, &shi->har);
				}
				if (mtex->mapto & MAP_ALPHA) {
					GPUNodeLink *alphafac;

					if (mtex->alphafac == 1.0f && (ma->constflag & MA_CONSTANT_TEXTURE)) alphafac = stencil;
					else GPU_link(mat, "math_multiply", GPU_select_uniform(&mtex->alphafac, GPU_DYNAMIC_TEX_ALPHA, NULL, ma), stencil, &alphafac);

					texture_value_blend(
					        mat, GPU_uniform(&mtex->def_var), shi->alpha, tin, alphafac,
					        mtex->blendtype, &shi->alpha);
					GPU_link(mat, "mtex_value_clamp", shi->alpha, &shi->alpha);
				}
				if (!(mat->scene->gm.flag & GAME_GLSL_NO_EXTRA_TEX) && mtex->mapto & MAP_AMB) {
					GPUNodeLink *ambfac;

					if (mtex->ambfac == 1.0f) ambfac = stencil;
					else GPU_link(mat, "math_multiply", GPU_uniform(&mtex->ambfac), stencil, &ambfac);

					texture_value_blend(
					        mat, GPU_uniform(&mtex->def_var), shi->amb, tin, ambfac,
					        mtex->blendtype, &shi->amb);
					GPU_link(mat, "mtex_value_clamp", shi->amb, &shi->amb);
				}
			}
		}
	}
}

void GPU_shadeinput_set(GPUMaterial *mat, Material *ma, GPUShadeInput *shi)
{
	float one = 1.0f;

	memset(shi, 0, sizeof(*shi));

	shi->gpumat = mat;
	shi->mat = ma;

	GPU_link(mat, "set_rgb", GPU_select_uniform(&ma->r, GPU_DYNAMIC_MAT_DIFFRGB, ma, ma), &shi->rgb);
	GPU_link(mat, "set_rgb", GPU_select_uniform(&ma->specr, GPU_DYNAMIC_MAT_SPECRGB, ma, ma), &shi->specrgb);
	GPU_link(mat, "set_rgb", GPU_select_uniform(&ma->mirr, GPU_DYNAMIC_MAT_MIR, ma, ma), &shi->mir);
	GPU_link(mat, "set_rgba_zero", &shi->refcol);
	GPU_link(mat, "shade_norm", material_builtin(mat, GPU_VIEW_NORMAL), &shi->vn);

	if (mat->alpha)
		GPU_link(mat, "set_value", GPU_select_uniform(&ma->alpha, GPU_DYNAMIC_MAT_ALPHA, ma, ma), &shi->alpha);
	else
		GPU_link(mat, "set_value", GPU_uniform(&one), &shi->alpha);

	GPU_link(mat, "set_value", GPU_select_uniform(&ma->ref, GPU_DYNAMIC_MAT_REF, ma, ma), &shi->refl);
	GPU_link(mat, "set_value", GPU_select_uniform(&ma->spec, GPU_DYNAMIC_MAT_SPEC, ma, ma), &shi->spec);
	GPU_link(mat, "set_value", GPU_select_uniform(&ma->emit, GPU_DYNAMIC_MAT_EMIT, ma, ma), &shi->emit);
	GPU_link(mat, "set_value", GPU_select_uniform(&mat->har, GPU_DYNAMIC_MAT_HARD, ma, ma), &shi->har);
	GPU_link(mat, "set_value", GPU_select_uniform(&ma->amb, GPU_DYNAMIC_MAT_AMB, ma, ma), &shi->amb);
	GPU_link(mat, "set_value", GPU_select_uniform(&ma->spectra, GPU_DYNAMIC_MAT_SPECTRA, ma, ma), &shi->spectra);
	GPU_link(mat, "shade_view", material_builtin(mat, GPU_VIEW_POSITION), &shi->view);
	GPU_link(mat, "vcol_attribute", GPU_attribute(CD_MCOL, ""), &shi->vcol);
	if (GPU_material_do_color_management(mat) && !(ma->sss_flag))
		GPU_link(mat, "srgb_to_linearrgb", shi->vcol, &shi->vcol);
	GPU_link(mat, "texco_refl", shi->vn, shi->view, &shi->ref);
}

void GPU_mist_update_enable(short enable)
{
	GPUWorld.mistenabled = (float)enable;
}

void GPU_mist_update_values(int type, float start, float dist, float inten, const float color[3])
{
	GPUWorld.mistype = (float)type;
	GPUWorld.miststart = start;
	GPUWorld.mistdistance = dist;
	GPUWorld.mistintensity = inten;
	copy_v3_v3(GPUWorld.mistcol, color);
	GPUWorld.mistcol[3] = 1.0f;
}

void GPU_horizon_update_color(const float color[3])
{
	copy_v3_v3(GPUWorld.horicol, color);
}

void GPU_ambient_update_color(const float color[3])
{
	copy_v3_v3(GPUWorld.ambcol, color);
	GPUWorld.ambcol[3] = 1.0f;
}

void GPU_zenith_update_color(const float color[3])
{
	copy_v3_v3(GPUWorld.zencol, color);
}

void GPU_update_exposure_range(float exp, float range)
{
	GPUWorld.linfac = 1.0f + powf((2.0f * exp + 0.5f), -10.0f);
	GPUWorld.logfac = log((GPUWorld.linfac - 1.0f) / GPUWorld.linfac) / range;
}

void GPU_update_envlight_energy(float energy)
{
	GPUWorld.envlightenergy = energy;
}

void GPU_shaderesult_set(GPUShadeInput *shi, GPUShadeResult *shr)
{
	GPUMaterial *mat = shi->gpumat;
	GPUNodeLink *emit, *mistfac;
	Material *ma = shi->mat;
	World *world = mat->scene->world;

	mat->dynproperty |= DYN_LAMP_CO;
	memset(shr, 0, sizeof(*shr));

	if (ma->mode & MA_VERTEXCOLP)
		shi->rgb = shi->vcol;

	do_material_tex(shi);

	if ((mat->scene->gm.flag & GAME_GLSL_NO_LIGHTS) || (ma->mode & MA_SHLESS)) {
		GPU_link(mat, "set_rgb", shi->rgb, &shr->diff);
		GPU_link(mat, "set_rgb_zero", &shr->spec);
		GPU_link(mat, "set_value", shi->alpha, &shr->alpha);
		shr->combined = shr->diff;
	}
	else {
		if (GPU_link_changed(shi->emit) || ma->emit != 0.0f || !(ma->constflag & MA_CONSTANT_MATERIAL)) {
			if ((ma->mode & (MA_VERTEXCOL | MA_VERTEXCOLP)) == MA_VERTEXCOL) {
				GPU_link(mat, "shade_add", shi->emit, shi->vcol, &emit);
				GPU_link(mat, "shade_mul", emit, shi->rgb, &shr->diff);
			}
			else
				GPU_link(mat, "shade_mul_value", shi->emit, shi->rgb, &shr->diff);
		}
		else
			GPU_link(mat, "set_rgb_zero", &shr->diff);

		GPU_link(mat, "set_rgb_zero", &shr->spec);

		material_lights(shi, shr);

		shr->combined = shr->diff;

		GPU_link(mat, "set_value", shi->alpha, &shr->alpha);

		if (world) {
			/* exposure correction */
			if (world->exp != 0.0f || world->range != 1.0f || !(ma->constflag & MA_CONSTANT_WORLD)) {
				GPU_link(mat, "shade_exposure_correct", shr->combined,
					GPU_select_uniform(&GPUWorld.linfac, GPU_DYNAMIC_WORLD_LINFAC, NULL, ma),
					GPU_select_uniform(&GPUWorld.logfac, GPU_DYNAMIC_WORLD_LOGFAC, NULL, ma),
					&shr->combined);
				GPU_link(mat, "shade_exposure_correct", shr->spec,
					GPU_select_uniform(&GPUWorld.linfac, GPU_DYNAMIC_WORLD_LINFAC, NULL, ma),
					GPU_select_uniform(&GPUWorld.logfac, GPU_DYNAMIC_WORLD_LOGFAC, NULL, ma),
					&shr->spec);
			}

			/* environment lighting */
			if (!(mat->scene->gm.flag & GAME_GLSL_NO_ENV_LIGHTING) &&
			    (world->mode & WO_ENV_LIGHT) &&
			    (mat->scene->r.mode & R_SHADOW) &&
			    !BKE_scene_use_new_shading_nodes(mat->scene))
			{
				if (((world->ao_env_energy != 0.0f) && (GPU_link_changed(shi->amb) || ma->amb != 0.0f) &&
				    (GPU_link_changed(shi->refl) || ma->ref != 0.0f)) || !(ma->constflag & MA_CONSTANT_WORLD))
				{
					if (world->aocolor == WO_AOSKYCOL) {
						if (!(is_zero_v3(&world->horr) & is_zero_v3(&world->zenr)) || !(ma->constflag & MA_CONSTANT_WORLD)) {
							GPUNodeLink *fcol, *f;
							GPU_link(mat, "math_multiply", shi->amb, shi->refl, &f);
							GPU_link(mat, "math_multiply", f, GPU_select_uniform(&GPUWorld.envlightenergy, GPU_DYNAMIC_ENVLIGHT_ENERGY, NULL, ma), &f);
							GPU_link(mat, "shade_mul_value", f, shi->rgb, &fcol);
							GPU_link(mat, "env_apply", shr->combined,
							         GPU_select_uniform(GPUWorld.horicol, GPU_DYNAMIC_HORIZON_COLOR, NULL, ma),
							         GPU_select_uniform(GPUWorld.zencol, GPU_DYNAMIC_ZENITH_COLOR, NULL, ma), fcol,
									 material_builtin(mat, GPU_VIEW_MATRIX), shi->vn, &shr->combined);
						}
					}
					else if (world->aocolor == WO_AOSKYTEX) {
						if (world->mtex[0] && world->mtex[0]->tex && world->mtex[0]->tex->ima) {
							GPUNodeLink *fcol, *f;
							Tex* tex = world->mtex[0]->tex;
							GPU_link(mat, "math_multiply", shi->amb, shi->refl, &f);
							GPU_link(mat, "math_multiply", f, GPU_select_uniform(&GPUWorld.envlightenergy, GPU_DYNAMIC_ENVLIGHT_ENERGY, NULL, ma), &f);
							GPU_link(mat, "shade_mul_value", f, shi->rgb, &fcol);
							GPU_link(mat, "env_apply_tex", shr->combined, fcol,
									 GPU_cube_map(tex->ima, &tex->iuser, false),
									 material_builtin(mat, GPU_VIEW_NORMAL),
									 material_builtin(mat, GPU_INVERSE_VIEW_MATRIX),
									 &shr->combined);
						}
					}
					else {
						GPUNodeLink *f;
						GPU_link(mat, "math_multiply", shi->amb, shi->refl, &f);
						GPU_link(mat, "math_multiply", f, GPU_select_uniform(&GPUWorld.envlightenergy, GPU_DYNAMIC_ENVLIGHT_ENERGY, NULL, ma), &f);
						GPU_link(mat, "shade_maddf", shr->combined, f, shi->rgb, &shr->combined);
					}
				}
			}

			/* ambient color */
			if (GPU_link_changed(shi->amb) || ma->amb != 0.0f || !(ma->constflag & MA_CONSTANT_MATERIAL)) {
				GPU_link(mat, "shade_maddf", shr->combined, GPU_select_uniform(&ma->amb, GPU_DYNAMIC_MAT_AMB, NULL, ma),
				         GPU_select_uniform(GPUWorld.ambcol, GPU_DYNAMIC_AMBIENT_COLOR, NULL, ma),
				         &shr->combined);
			}
		}

		if (ma->mode & MA_TRANSP && (ma->mode & (MA_ZTRANSP | MA_RAYTRANSP))) {
			if (GPU_link_changed(shi->spectra) || ma->spectra != 0.0f || !(ma->constflag & MA_CONSTANT_MATERIAL)) {
				GPU_link(mat, "alpha_spec_correction", shr->spec, shi->spectra,
				         shi->alpha, &shr->alpha);
			}
		}

		if (ma->mode & MA_RAMP_COL) ramp_diffuse_result(shi, &shr->combined);
		if (ma->mode & MA_RAMP_SPEC) ramp_spec_result(shi, &shr->spec);

		if (GPU_link_changed(shi->refcol))
			GPU_link(mat, "shade_add_mirror", shi->mir, shi->refcol, shr->combined, &shr->combined);

		if (GPU_link_changed(shi->spec) || ma->spec != 0.0f || !(ma->constflag & MA_CONSTANT_MATERIAL))
			GPU_link(mat, "shade_add", shr->combined, shr->spec, &shr->combined);
	}

	if (ma->mode & MA_TRANSP && ma->mode2 & MA_DEPTH_TRANSP) {
		GPU_link(mat, "shade_alpha_depth",
				 material_builtin(mat, GPU_VIEW_POSITION),
				 GPU_dynamic_texture_ptr(GPU_texture_global_depth_ptr(), GPU_DYNAMIC_SAMPLER_2DBUFFER, ma),
				 shr->alpha, GPU_uniform(&ma->depthtranspfactor), &shr->alpha);
	}

	GPU_link(mat, "mtex_alpha_to_col", shr->combined, shr->alpha, &shr->combined);

	if (ma->shade_flag & MA_OBCOLOR) {
		GPU_link(mat, "shade_obcolor", shr->combined, material_builtin(mat, GPU_OBCOLOR), &shr->combined);
	}

	if (!(ma->mode & MA_NOMIST)) {
		GPU_link(mat, "shade_mist_factor", material_builtin(mat, GPU_VIEW_POSITION),
		         GPU_dynamic_uniform(&GPUWorld.mistenabled, GPU_DYNAMIC_MIST_ENABLE, NULL),
		         GPU_select_uniform(&GPUWorld.miststart, GPU_DYNAMIC_MIST_START, NULL, ma),
		         GPU_select_uniform(&GPUWorld.mistdistance, GPU_DYNAMIC_MIST_DISTANCE, NULL, ma),
		         GPU_select_uniform(&GPUWorld.mistype, GPU_DYNAMIC_MIST_TYPE, NULL, ma),
		         GPU_select_uniform(&GPUWorld.mistintensity, GPU_DYNAMIC_MIST_INTENSITY, NULL, ma), &mistfac);

		GPU_link(mat, "mix_blend", mistfac, shr->combined,
		         GPU_select_uniform(GPUWorld.mistcol, GPU_DYNAMIC_MIST_COLOR, NULL, ma), &shr->combined);
	}

	if (!mat->alpha) {
		if (world && (GPU_link_changed(shr->alpha) || ma->alpha != 1.0f || !(ma->constflag & MA_CONSTANT_WORLD)))
			GPU_link(mat, "shade_world_mix", GPU_select_uniform(GPUWorld.horicol, GPU_DYNAMIC_HORIZON_COLOR, NULL, ma),
			         shr->combined, &shr->combined);

		GPU_link(mat, "shade_alpha_opaque", shr->combined, &shr->combined);
	}

	if (ma->shade_flag & MA_OBCOLOR) {
		mat->obcolalpha = 1;
		GPU_link(mat, "shade_alpha_obcolor", shr->combined, material_builtin(mat, GPU_OBCOLOR), &shr->combined);
	}
}

static GPUNodeLink *GPU_blender_material(GPUMaterial *mat, Material *ma)
{
	GPUShadeInput shi;
	GPUShadeResult shr;

	GPU_shadeinput_set(mat, ma, &shi);
	GPU_shaderesult_set(&shi, &shr);

	return shr.combined;
}

static GPUNodeLink *gpu_material_diffuse_bsdf(GPUMaterial *mat, Material *ma)
{
	static float roughness = 0.0f;
	GPUNodeLink *outlink;

	GPU_link(mat, "node_bsdf_diffuse",
	         GPU_uniform(&ma->r), GPU_uniform(&roughness), material_builtin(mat, GPU_VIEW_NORMAL), &outlink);

	return outlink;
}

static GPUNodeLink *gpu_material_preview_matcap(GPUMaterial *mat, Material *ma)
{
	GPUNodeLink *outlink;

	/* some explanations here:
	 * matcap normal holds the normal remapped to the 0.0 - 1.0 range. To take advantage of flat shading, we abuse
	 * the built in secondary color of opengl. Color is just the regular color, which should include mask value too.
	 * This also needs flat shading so we use the primary opengl color built-in */
	GPU_link(mat, "material_preview_matcap", GPU_uniform(&ma->r), GPU_image_preview(ma->preview),
	         GPU_opengl_builtin(GPU_MATCAP_NORMAL), GPU_opengl_builtin(GPU_COLOR), &outlink);

	return outlink;
}

/* new solid draw mode with glsl matcaps */
GPUMaterial *GPU_material_matcap(Scene *scene, Material *ma, GPUMaterialFlag flags)
{
	GPUMaterial *mat;
	GPUNodeLink *outlink;
	LinkData *link;

	for (link = ma->gpumaterial.first; link; link = link->next) {
		GPUMaterial *current_material = (GPUMaterial *)link->data;
		if (current_material->scene == scene &&
		    current_material->flags == flags)
		{
			return current_material;
		}
	}

	/* allocate material */
	mat = GPU_material_construct_begin(ma);
	mat->scene = scene;
	mat->type = GPU_MATERIAL_TYPE_MESH;
	mat->flags = flags;

	if (ma->preview && ma->preview->rect[0]) {
		outlink = gpu_material_preview_matcap(mat, ma);
	}
	else {
		outlink = gpu_material_diffuse_bsdf(mat, ma);
	}

	GPU_material_output_link(mat, outlink);

	gpu_material_construct_end(mat, "matcap_pass");

	/* note that even if building the shader fails in some way, we still keep
	 * it to avoid trying to compile again and again, and simple do not use
	 * the actual shader on drawing */

	link = MEM_callocN(sizeof(LinkData), "GPUMaterialLink");
	link->data = mat;
	BLI_addtail(&ma->gpumaterial, link);

	return mat;
}

static void do_world_tex(GPUShadeInput *shi, struct World *wo, GPUNodeLink **hor, GPUNodeLink **zen, GPUNodeLink **blend)
{
	GPUMaterial *mat = shi->gpumat;
	GPUNodeLink *texco, *tin, *trgb, *stencil, *tcol, *zenfac;
	MTex *mtex;
	Tex *tex;
	float ofs[3], zero = 0.0f;
	int tex_nr, rgbnor;

	GPU_link(mat, "set_value_one", &stencil);
	/* go over texture slots */
	for (tex_nr = 0; tex_nr < MAX_MTEX; tex_nr++) {
		if (wo->mtex[tex_nr]) {
			mtex = wo->mtex[tex_nr];
			tex = mtex->tex;
			if (tex == NULL || !tex->ima || (tex->type != TEX_IMAGE && tex->type != TEX_ENVMAP))
				continue;
			/* which coords */
			if (mtex->texco == TEXCO_VIEW || mtex->texco == TEXCO_GLOB) {
				if (tex->type == TEX_IMAGE)
					texco = material_builtin(mat, GPU_VIEW_POSITION);
				else if (tex->type == TEX_ENVMAP)
					GPU_link(mat, "background_transform_to_world", material_builtin(mat, GPU_VIEW_POSITION), &texco);
			}
			else if (mtex->texco == TEXCO_EQUIRECTMAP || mtex->texco == TEXCO_ANGMAP) {
				if ((tex->type == TEX_IMAGE && wo->skytype & WO_SKYREAL) || tex->type == TEX_ENVMAP)
					GPU_link(mat, "background_transform_to_world", material_builtin(mat, GPU_VIEW_POSITION), &texco);
				else
					texco = material_builtin(mat, GPU_VIEW_POSITION);
			}
			else
				continue;
			GPU_link(mat, "texco_norm", texco, &texco);
			if (tex->type == TEX_IMAGE && !(wo->skytype & WO_SKYREAL)) {
				GPU_link(mat, "mtex_2d_mapping", texco, &texco);
			}
			if (mtex->size[0] != 1.0f || mtex->size[1] != 1.0f || mtex->size[2] != 1.0f) {
				float size[3] = { mtex->size[0], mtex->size[1], mtex->size[2] };
				if (tex->type == TEX_ENVMAP) {
					size[1] = mtex->size[2];
					size[2] = mtex->size[1];
				}
				GPU_link(mat, "mtex_mapping_size", texco, GPU_uniform(size), &texco);
			}
			ofs[0] = mtex->ofs[0] + 0.5f - 0.5f * mtex->size[0];
			if (tex->type == TEX_ENVMAP) {
				ofs[1] = -mtex->ofs[2] + 0.5f - 0.5f * mtex->size[2];
				ofs[2] = mtex->ofs[1] + 0.5f - 0.5f * mtex->size[1];
			}
			else {
				ofs[1] = mtex->ofs[1] + 0.5f - 0.5f * mtex->size[1];
				ofs[2] = 0.0;
			}
			if (ofs[0] != 0.0f || ofs[1] != 0.0f || ofs[2] != 0.0f)
				GPU_link(mat, "mtex_mapping_ofs", texco, GPU_uniform(ofs), &texco);
			if (mtex->texco == TEXCO_EQUIRECTMAP) {
				GPU_link(mat, "node_tex_environment_equirectangular", texco, GPU_image(tex->ima, &tex->iuser, false), GPU_uniform(&mtex->lodbias), &trgb);
			}
			else if (mtex->texco == TEXCO_ANGMAP) {
				GPU_link(mat, "node_tex_environment_mirror_ball", texco, GPU_image(tex->ima, &tex->iuser, false), GPU_uniform(&mtex->lodbias), &trgb);
			}
			else {
				if (tex->type == TEX_ENVMAP)
					GPU_link(mat, "mtex_cube_map", texco, GPU_cube_map(tex->ima, &tex->iuser, false), GPU_uniform(&mtex->lodbias), &tin, &trgb);
				else if (tex->type == TEX_IMAGE)
					GPU_link(mat, "mtex_image", texco, GPU_image(tex->ima, &tex->iuser, false), GPU_uniform(&mtex->lodbias), &tin, &trgb);
			}
			rgbnor = TEX_RGB;
			if (tex_do_color_management(mat, mtex, tex)) {
				GPU_link(mat, "srgb_to_linearrgb", trgb, &trgb);
			}
			/* texture output */
			if ((rgbnor & TEX_RGB) && (mtex->texflag & MTEX_RGBTOINT)) {
				GPU_link(mat, "mtex_rgbtoint", trgb, &tin);
				rgbnor -= TEX_RGB;
			}
			if (mtex->texflag & MTEX_NEGATIVE) {
				if (rgbnor & TEX_RGB)
					GPU_link(mat, "mtex_rgb_invert", trgb, &trgb);
				else
					GPU_link(mat, "mtex_value_invert", tin, &tin);
			}
			if (mtex->texflag & MTEX_STENCIL) {
				if (rgbnor & TEX_RGB)
					GPU_link(mat, "mtex_rgb_stencil", stencil, trgb, &stencil, &trgb);
				else
					GPU_link(mat, "mtex_value_stencil", stencil, tin, &stencil, &tin);
			}
			else {
				if (rgbnor & TEX_RGB)
					GPU_link(mat, "mtex_alpha_multiply_value", trgb, stencil, &trgb);
				else
					GPU_link(mat, "math_multiply", stencil, tin, &tin);
			}
			/* color mapping */
			if (mtex->mapto & (WOMAP_HORIZ + WOMAP_ZENUP + WOMAP_ZENDOWN)) {
				if ((rgbnor & TEX_RGB) == 0)
					GPU_link(mat, "set_rgb", GPU_uniform(&mtex->r), &trgb);
				else
					GPU_link(mat, "mtex_alpha_from_col", trgb, &tin);
				GPU_link(mat, "set_rgb", trgb, &tcol);
				if (mtex->mapto & WOMAP_HORIZ) {
					texture_rgb_blend(mat, tcol, *hor, tin, GPU_uniform(&mtex->colfac), mtex->blendtype, hor);
				}
				if (mtex->mapto & (WOMAP_ZENUP + WOMAP_ZENDOWN)) {
					GPU_link(mat, "set_value_zero", &zenfac);
					if (wo->skytype & WO_SKYREAL) {
						if (mtex->mapto & WOMAP_ZENUP) {
							if (mtex->mapto & WOMAP_ZENDOWN) {
								GPU_link(mat, "world_zen_mapping", shi->view, GPU_uniform(&mtex->zenupfac),
								         GPU_uniform(&mtex->zendownfac), &zenfac);
							}
							else {
								GPU_link(mat, "world_zen_mapping", shi->view, GPU_uniform(&mtex->zenupfac),
								         GPU_uniform(&zero), &zenfac);
							}
						}
						else if (mtex->mapto & WOMAP_ZENDOWN) {
							GPU_link(mat, "world_zen_mapping", shi->view, GPU_uniform(&zero),
							         GPU_uniform(&mtex->zendownfac), &zenfac);
						}
					}
					else {
						if (mtex->mapto & WOMAP_ZENUP)
							GPU_link(mat, "set_value", GPU_uniform(&mtex->zenupfac), &zenfac);
						else if (mtex->mapto & WOMAP_ZENDOWN)
							GPU_link(mat, "set_value", GPU_uniform(&mtex->zendownfac), &zenfac);
					}
					texture_rgb_blend(mat, tcol, *zen, tin, zenfac, mtex->blendtype, zen);
				}
			}
			if (mtex->mapto & WOMAP_BLEND && wo->skytype & WO_SKYBLEND) {
				if (rgbnor & TEX_RGB)
					GPU_link(mat, "mtex_rgbtoint", trgb, &tin);
				texture_value_blend(mat, GPU_uniform(&mtex->def_var), *blend, tin, GPU_uniform(&mtex->blendfac), mtex->blendtype, blend);
			}
		}
	}
}

static void gpu_material_old_world(struct GPUMaterial *mat, struct World *wo)
{
	GPUShadeInput shi;
	GPUShadeResult shr;
	GPUNodeLink *hor, *zen, *ray, *blend;

	shi.gpumat = mat;

	for (int i = 0; i < MAX_MTEX; i++) {
		if (wo->mtex[i] && wo->mtex[i]->tex) {
			wo->skytype |= WO_SKYTEX;
			break;
		}
	}
	if ((wo->skytype & (WO_SKYBLEND + WO_SKYTEX)) == 0) {
		GPU_link(mat, "set_rgb", GPU_dynamic_uniform(&wo->horr, GPU_DYNAMIC_HORIZON_COLOR, NULL), &shr.combined);
	}
	else {
		GPU_link(mat, "set_rgb_zero", &shi.rgb);
		GPU_link(mat, "background_transform_to_world", material_builtin(mat, GPU_VIEW_POSITION), &ray);
		if (wo->skytype & WO_SKYPAPER)
			GPU_link(mat, "world_paper_view", material_builtin(mat, GPU_VIEW_POSITION), &shi.view);
		else
			GPU_link(mat, "shade_view", ray, &shi.view);
		if (wo->skytype & WO_SKYBLEND) {
			if (wo->skytype & WO_SKYPAPER) {
				if (wo->skytype & WO_SKYREAL)
					GPU_link(mat, "world_blend_paper_real", material_builtin(mat, GPU_VIEW_POSITION), &blend);
				else
					GPU_link(mat, "world_blend_paper", material_builtin(mat, GPU_VIEW_POSITION), &blend);
			}
			else {
				if (wo->skytype & WO_SKYREAL)
					GPU_link(mat, "world_blend_real", ray, &blend);
				else
					GPU_link(mat, "world_blend", ray, &blend);
			}
		}
		else {
			GPU_link(mat, "set_value_zero", &blend);
		}
		GPU_link(mat, "set_rgb", GPU_dynamic_uniform(&wo->horr, GPU_DYNAMIC_HORIZON_COLOR, NULL), &hor);
		GPU_link(mat, "set_rgb", GPU_dynamic_uniform(&wo->zenr, GPU_DYNAMIC_ZENITH_COLOR, NULL), &zen);
		do_world_tex(&shi, wo, &hor, &zen, &blend);
		if (wo->skytype & WO_SKYBLEND)
			GPU_link(mat, "node_mix_shader", blend, hor, zen, &shi.rgb);
		else
			GPU_link(mat, "set_rgb", hor, &shi.rgb);
		GPU_link(mat, "set_rgb", shi.rgb, &shr.combined);
	}
	GPU_material_output_link(mat, shr.combined);
}

GPUMaterial *GPU_material_world(struct Scene *scene, struct World *wo, GPUMaterialFlag flags)
{
	LinkData *link;
	GPUMaterial *mat;

	for (link = wo->gpumaterial.first; link; link = link->next) {
		GPUMaterial *current_material = (GPUMaterial *)link->data;
		if (current_material->scene == scene &&
			current_material->flags == flags)
		{
			return link->data;
		}
	}

	/* allocate material */
	mat = GPU_material_construct_begin(NULL);
	mat->scene = scene;
	mat->type = GPU_MATERIAL_TYPE_WORLD;
	mat->flags = flags;

	/* create nodes */
	if (BKE_scene_use_new_shading_nodes(scene) && wo->nodetree && wo->use_nodes) {
		ntreeGPUMaterialNodes(wo->nodetree, mat, NODE_NEW_SHADING);
	}
	else {
		gpu_material_old_world(mat, wo);
	}

	if (GPU_material_do_color_management(mat) && !(mat->flags & GPU_MATERIAL_NO_COLOR_MANAGEMENT))
		if (mat->outlink)
			GPU_link(mat, "linearrgb_to_srgb", mat->outlink, &mat->outlink);

	gpu_material_construct_end(mat, wo->id.name);

	/* note that even if building the shader fails in some way, we still keep
	 * it to avoid trying to compile again and again, and simple do not use
	 * the actual shader on drawing */

	link = MEM_callocN(sizeof(LinkData), "GPUMaterialLink");
	link->data = mat;
	BLI_addtail(&wo->gpumaterial, link);

	return mat;
}


GPUMaterial *GPU_material_from_blender(Scene *scene, Material *ma, GPUMaterialFlag flags)
{
	GPUMaterial *mat;
	GPUNodeLink *outlink;
	LinkData *link;
	ListBase *gpumaterials;

	if (flags & GPU_MATERIAL_INSTANCING) {
		gpumaterials = &ma->gpumaterialinstancing;
	}
	else {
		gpumaterials = &ma->gpumaterial;
	}

	for (link = gpumaterials->first; link; link = link->next) {
		GPUMaterial *current_material = (GPUMaterial *)link->data;
		if (current_material->scene == scene &&
		    current_material->flags == flags)
		{
			return current_material;
		}
	}

	/* allocate material */
	mat = GPU_material_construct_begin(ma);
	mat->scene = scene;
	mat->type = GPU_MATERIAL_TYPE_MESH;
	mat->flags = flags;
	mat->har = ma->har;

	/* render pipeline option */
	bool new_shading_nodes = BKE_scene_use_new_shading_nodes(scene);
	if (!new_shading_nodes && (ma->mode & MA_TRANSP))
		GPU_material_enable_alpha(mat);
	else if (new_shading_nodes && ma->alpha < 1.0f)
		GPU_material_enable_alpha(mat);

	if (!(scene->gm.flag & GAME_GLSL_NO_NODES) && ma->nodetree && ma->use_nodes) {
		/* create nodes */
		if (new_shading_nodes)
			ntreeGPUMaterialNodes(ma->nodetree, mat, NODE_NEW_SHADING);
		else
			ntreeGPUMaterialNodes(ma->nodetree, mat, NODE_OLD_SHADING);
	}
	else {
		if (new_shading_nodes) {
			/* create simple diffuse material instead of nodes */
			outlink = gpu_material_diffuse_bsdf(mat, ma);
		}
		else {
			/* create blender material */
			outlink = GPU_blender_material(mat, ma);
		}

		GPU_material_output_link(mat, outlink);
	}

	if (GPU_material_do_color_management(mat) && !(ma->sss_flag) && !(mat->flags & GPU_MATERIAL_NO_COLOR_MANAGEMENT))
		if (mat->outlink)
			GPU_link(mat, "linearrgb_to_srgb", mat->outlink, &mat->outlink);

	gpu_material_construct_end(mat, ma->id.name);

	/* note that even if building the shader fails in some way, we still keep
	 * it to avoid trying to compile again and again, and simple do not use
	 * the actual shader on drawing */

	link = MEM_callocN(sizeof(LinkData), "GPUMaterialLink");
	link->data = mat;
	BLI_addtail(gpumaterials, link);

	return mat;
}

void GPU_materials_free(Main *bmain)
{
	Object *ob;
	Material *ma;
	World *wo;
	extern Material defmaterial;

	for (ma = bmain->mat.first; ma; ma = ma->id.next) {
		GPU_material_free(&ma->gpumaterial);
		GPU_material_free(&ma->gpumaterialinstancing);
	}

	for (wo = bmain->world.first; wo; wo = wo->id.next)
		GPU_material_free(&wo->gpumaterial);

	GPU_material_free(&defmaterial.gpumaterial);
	GPU_material_free(&defmaterial.gpumaterialinstancing);

	for (ob = bmain->object.first; ob; ob = ob->id.next)
		GPU_lamp_free(ob);
}

/* Lamps and shadow buffers */

static void gpu_lamp_calc_winmat(GPULamp *lamp)
{
	float temp, angle, pixsize, wsize;

	if (lamp->type == LA_SUN) {
		wsize = lamp->la->shadow_frustum_size;
		orthographic_m4(lamp->winmat, -wsize, wsize, -wsize, wsize, lamp->d, lamp->clipend);
	}
	else if (lamp->type == LA_SPOT) {
		angle = saacos(lamp->spotsi);
		temp = 0.5f * lamp->size * cosf(angle) / sinf(angle);
		pixsize = lamp->d / temp;
		wsize = pixsize * 0.5f * lamp->size;
		/* compute shadows according to X and Y scaling factors */
		perspective_m4(
		        lamp->winmat,
		        -wsize * lamp->spotvec[0], wsize * lamp->spotvec[0],
		        -wsize * lamp->spotvec[1], wsize * lamp->spotvec[1],
		        lamp->d, lamp->clipend);
	}
}

void GPU_lamp_update(GPULamp *lamp, int lay, int hide, float obmat[4][4])
{
	float mat[4][4];
	float obmat_scale[3];

	lamp->lay = lay;
	lamp->hide = hide;

	normalize_m4_m4_ex(mat, obmat, obmat_scale);

	copy_v3_v3(lamp->vec, mat[2]);
	copy_v3_v3(lamp->co, mat[3]);
	copy_m4_m4(lamp->obmat, mat);
	invert_m4_m4(lamp->imat, mat);

	if (lamp->type == LA_SPOT) {
		/* update spotlamp scale on X and Y axis */
		lamp->spotvec[0] = obmat_scale[0] / obmat_scale[2];
		lamp->spotvec[1] = obmat_scale[1] / obmat_scale[2];
	}

	if (GPU_lamp_has_shadow_buffer(lamp)) {
		/* makeshadowbuf */
		gpu_lamp_calc_winmat(lamp);
	}
}

void GPU_lamp_update_colors(GPULamp *lamp, float r, float g, float b, float energy)
{
	lamp->energy = energy;
	if (lamp->mode & LA_NEG) lamp->energy = -lamp->energy;

	lamp->col[0] = r;
	lamp->col[1] = g;
	lamp->col[2] = b;
}

void GPU_lamp_update_distance(GPULamp *lamp, float distance, float att1, float att2,
                              float coeff_const, float coeff_lin, float coeff_quad)
{
	lamp->dist = distance;
	lamp->att1 = att1;
	lamp->att2 = att2;
	lamp->coeff_const = coeff_const;
	lamp->coeff_lin = coeff_lin;
	lamp->coeff_quad = coeff_quad;
}

void GPU_lamp_update_spot(GPULamp *lamp, float spotsize, float spotblend)
{
	lamp->spotsi = cosf(spotsize * 0.5f);
	lamp->spotbl = (1.0f - lamp->spotsi) * spotblend;
}

static void gpu_lamp_from_blender(Scene *scene, Object *ob, Object *par, Lamp *la, GPULamp *lamp)
{
	lamp->scene = scene;
	lamp->ob = ob;
	lamp->par = par;
	lamp->la = la;

	/* add_render_lamp */
	lamp->mode = la->mode;
	lamp->type = la->type;

	lamp->energy = la->energy;
	if (lamp->mode & LA_NEG) lamp->energy = -lamp->energy;

	lamp->col[0] = la->r;
	lamp->col[1] = la->g;
	lamp->col[2] = la->b;

	GPU_lamp_update(lamp, ob->lay, (ob->restrictflag & OB_RESTRICT_RENDER), ob->obmat);

	lamp->spotsi = la->spotsize;
	if (lamp->mode & LA_HALO)
		if (lamp->spotsi > DEG2RADF(170.0f))
			lamp->spotsi = DEG2RADF(170.0f);
	lamp->spotsi = cosf(lamp->spotsi * 0.5f);
	lamp->spotbl = (1.0f - lamp->spotsi) * la->spotblend;
	lamp->k = la->k;

	lamp->dist = la->dist;
	lamp->falloff_type = la->falloff_type;
	lamp->att1 = la->att1;
	lamp->att2 = la->att2;
	lamp->coeff_const = la->coeff_const;
	lamp->coeff_lin = la->coeff_lin;
	lamp->coeff_quad = la->coeff_quad;
	lamp->curfalloff = la->curfalloff;
	lamp->cutoff = la->cutoff;
	lamp->radius = la->radius;

	/* initshadowbuf */
	lamp->bias = 0.02f * la->bias;
	lamp->slopebias = la->slopebias;
	lamp->size = la->bufsize;
	lamp->d = la->clipsta;
	lamp->clipend = la->clipend;

	/* arbitrary correction for the fact we do no soft transition */
	lamp->bias *= 0.25f;
}

static void gpu_lamp_shadow_free(GPULamp *lamp)
{
	if (lamp->tex) {
		GPU_texture_free(lamp->tex);
		lamp->tex = NULL;
	}
	if (lamp->depthtex) {
		GPU_texture_free(lamp->depthtex);
		lamp->depthtex = NULL;
	}
	if (lamp->fb) {
		GPU_framebuffer_free(lamp->fb);
		lamp->fb = NULL;
	}
	if (lamp->blurtex) {
		GPU_texture_free(lamp->blurtex);
		lamp->blurtex = NULL;
	}
	if (lamp->blurfb) {
		GPU_framebuffer_free(lamp->blurfb);
		lamp->blurfb = NULL;
	}
}

GPULamp *GPU_lamp_from_blender(Scene *scene, Object *ob, Object *par)
{
	Lamp *la;
	GPULamp *lamp;
	LinkData *link;

	for (link = ob->gpulamp.first; link; link = link->next) {
		lamp = (GPULamp *)link->data;

		if (lamp->par == par && lamp->scene == scene)
			return link->data;
	}

	lamp = MEM_callocN(sizeof(GPULamp), "GPULamp");

	link = MEM_callocN(sizeof(LinkData), "GPULampLink");
	link->data = lamp;
	BLI_addtail(&ob->gpulamp, link);

	la = ob->data;
	gpu_lamp_from_blender(scene, ob, par, la, lamp);

	if ((la->type == LA_SPOT && (la->mode & (LA_SHAD_BUF | LA_SHAD_RAY))) ||
	    (la->type == LA_SUN && (la->mode & LA_SHAD_RAY)))
	{
		/* opengl */
		lamp->fb = GPU_framebuffer_create();
		if (!lamp->fb) {
			gpu_lamp_shadow_free(lamp);
			return lamp;
		}

		if (lamp->la->shadowmap_type == LA_SHADMAP_VARIANCE) {
			/* Shadow depth map */
			lamp->depthtex = GPU_texture_create_depth(lamp->size, lamp->size, true, NULL);
			if (!lamp->depthtex) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}

			if (!GPU_framebuffer_texture_attach(lamp->fb, lamp->depthtex, 0, NULL)) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}

			/* Shadow color map */
			lamp->tex = GPU_texture_create_vsm_shadow_map(lamp->size, NULL);
			if (!lamp->tex) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}

			if (!GPU_framebuffer_texture_attach(lamp->fb, lamp->tex, 0, NULL)) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}

			if (!GPU_framebuffer_check_valid(lamp->fb, NULL)) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}

			/* FBO and texture for blurring */
			lamp->blurfb = GPU_framebuffer_create();
			if (!lamp->blurfb) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}

			lamp->blurtex = GPU_texture_create_vsm_shadow_map(lamp->size * 0.5, NULL);
			if (!lamp->blurtex) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}

			if (!GPU_framebuffer_texture_attach(lamp->blurfb, lamp->blurtex, 0, NULL)) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}

			/* we need to properly bind to test for completeness */
			GPU_texture_bind_as_framebuffer(lamp->blurtex);

			if (!GPU_framebuffer_check_valid(lamp->blurfb, NULL)) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}

			GPU_framebuffer_texture_unbind(lamp->blurfb, lamp->blurtex);
		}
		else {
			lamp->depthtex = GPU_texture_create_depth(lamp->size, lamp->size, true, NULL);
			if (!lamp->depthtex) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}

			if (!GPU_framebuffer_texture_attach(lamp->fb, lamp->depthtex, 0, NULL)) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}

			if (!GPU_framebuffer_check_valid(lamp->fb, NULL)) {
				gpu_lamp_shadow_free(lamp);
				return lamp;
			}
		}

		GPU_framebuffer_restore();

		lamp->shadow_color[0] = la->shdwr;
		lamp->shadow_color[1] = la->shdwg;
		lamp->shadow_color[2] = la->shdwb;
	}
	else {
		lamp->shadow_color[0] = 1.0;
		lamp->shadow_color[1] = 1.0;
		lamp->shadow_color[2] = 1.0;
	}

	return lamp;
}

void GPU_lamp_free(Object *ob)
{
	GPULamp *lamp;
	LinkData *link;
	LinkData *nlink;
	Material *ma;

	for (link = ob->gpulamp.first; link; link = link->next) {
		lamp = link->data;

		while (lamp->materials.first) {
			nlink = lamp->materials.first;
			ma = nlink->data;
			BLI_freelinkN(&lamp->materials, nlink);

			if (ma->gpumaterial.first)
				GPU_material_free(&ma->gpumaterial);
			if (ma->gpumaterialinstancing.first)
				GPU_material_free(&ma->gpumaterialinstancing);
		}

		gpu_lamp_shadow_free(lamp);

		MEM_freeN(lamp);
	}

	BLI_freelistN(&ob->gpulamp);
}

bool GPU_lamp_has_shadow_buffer(GPULamp *lamp)
{
	return (!(lamp->scene->gm.flag & GAME_GLSL_NO_SHADOWS) &&
	        !(lamp->scene->gm.flag & GAME_GLSL_NO_LIGHTS) &&
	        lamp->depthtex && lamp->fb);
}

void GPU_lamp_update_buffer_mats(GPULamp *lamp)
{
	float rangemat[4][4], persmat[4][4];

	/* initshadowbuf */
	invert_m4_m4(lamp->viewmat, lamp->obmat);
	normalize_v3(lamp->viewmat[0]);
	normalize_v3(lamp->viewmat[1]);
	normalize_v3(lamp->viewmat[2]);

	/* makeshadowbuf */
	mul_m4_m4m4(persmat, lamp->winmat, lamp->viewmat);

	/* opengl depth buffer is range 0.0..1.0 instead of -1.0..1.0 in blender */
	unit_m4(rangemat);
	rangemat[0][0] = 0.5f;
	rangemat[1][1] = 0.5f;
	rangemat[2][2] = 0.5f;
	rangemat[3][0] = 0.5f;
	rangemat[3][1] = 0.5f;
	rangemat[3][2] = 0.5f;

	mul_m4_m4m4(lamp->persmat, rangemat, persmat);
}

void GPU_lamp_shadow_buffer_bind(GPULamp *lamp, float viewmat[4][4], int *winsize, float winmat[4][4])
{
	GPU_lamp_update_buffer_mats(lamp);

	/* opengl */
	glDisable(GL_SCISSOR_TEST);
	if (lamp->la->shadowmap_type == LA_SHADMAP_VARIANCE) {
		GPU_texture_bind_as_framebuffer(lamp->tex);
	}
	else {
		GPU_texture_bind_as_framebuffer(lamp->depthtex);
	}

	/* set matrices */
	copy_m4_m4(viewmat, lamp->viewmat);
	copy_m4_m4(winmat, lamp->winmat);
	*winsize = lamp->size;
}

void GPU_lamp_shadow_buffer_unbind(GPULamp *lamp)
{
	if (lamp->la->shadowmap_type == LA_SHADMAP_VARIANCE) {
		GPU_shader_unbind();
		GPU_framebuffer_blur(lamp->fb, lamp->tex, lamp->blurfb, lamp->blurtex, lamp->la->bufsharp);
	}

	GPU_framebuffer_texture_unbind(lamp->fb, lamp->tex);
	GPU_framebuffer_restore();
	glEnable(GL_SCISSOR_TEST);
}

int GPU_lamp_shadow_buffer_type(GPULamp *lamp)
{
	return lamp->la->shadowmap_type;
}

int GPU_lamp_shadow_bind_code(GPULamp *lamp)
{
	if (lamp->tex) {
		return GPU_texture_opengl_bindcode(lamp->tex);
	}
	else if (lamp->depthtex) {
		return GPU_texture_opengl_bindcode(lamp->depthtex);
	}
	return -1;
}

const float *GPU_lamp_dynpersmat(GPULamp *lamp)
{
	return &lamp->dynpersmat[0][0];
}

const float *GPU_lamp_get_viewmat(GPULamp *lamp)
{
	return &lamp->viewmat[0][0];
}

const float *GPU_lamp_get_winmat(GPULamp *lamp)
{
	return &lamp->winmat[0][0];
}

int GPU_lamp_shadow_layer(GPULamp *lamp)
{
	if (lamp->fb && lamp->depthtex && (lamp->mode & (LA_LAYER | LA_LAYER_SHADOW)))
		return lamp->lay;
	else
		return -1;
}

GPUNodeLink *GPU_lamp_get_data(
        GPUMaterial *mat, GPULamp *lamp,
        GPUNodeLink **r_col, GPUNodeLink **r_lv, GPUNodeLink **r_dist, GPUNodeLink **r_shadow, GPUNodeLink **r_energy)
{
	GPUNodeLink *visifac;
	GPUNodeLink *shadowfac;

	GPU_link(mat, "lamp_visible",
		GPU_dynamic_uniform(&lamp->dynlayer, GPU_DYNAMIC_LAMP_DYNVISI, lamp->ob),
		material_builtin(mat, GPU_OBJECT_LAY),
		GPU_dynamic_uniform(lamp->dyncol, GPU_DYNAMIC_LAMP_DYNCOL, lamp->ob),
		GPU_dynamic_uniform(&lamp->dynenergy, GPU_DYNAMIC_LAMP_DYNENERGY, lamp->ob),
		r_col, r_energy);

	visifac = lamp_get_visibility(mat, lamp, r_lv, r_dist);

	shade_light_textures(mat, lamp, r_col);

	if (GPU_lamp_has_shadow_buffer(lamp)) {
		GPUNodeLink *vn, *inp;

		GPU_link(mat, "shade_norm", material_builtin(mat, GPU_VIEW_NORMAL), &vn);
		GPU_link(mat, "shade_inp", vn, *r_lv, &inp);
		mat->dynproperty |= DYN_LAMP_PERSMAT;

		if (lamp->la->shadowmap_type == LA_SHADMAP_VARIANCE) {
			GPU_link(mat, "shadow_vsm",
			         material_builtin(mat, GPU_VIEW_POSITION),
			         GPU_dynamic_texture(lamp->tex, GPU_DYNAMIC_SAMPLER_2DSHADOW, lamp->ob),
			         GPU_dynamic_uniform((float *)lamp->dynpersmat, GPU_DYNAMIC_LAMP_DYNPERSMAT, lamp->ob),
			         GPU_uniform(&lamp->bias), GPU_uniform(&lamp->la->bleedbias), inp, &shadowfac);
		}
		else {
			if (lamp->la->samp > 1 && lamp->la->soft >= 0.01f && lamp->la->shadow_filter != LA_SHADOW_FILTER_NONE) {
				float samp = lamp->la->samp;
				float samplesize = lamp->la->soft / lamp->la->shadow_frustum_size;
				if (lamp->la->shadow_filter == LA_SHADOW_FILTER_PCF) {
					GPU_link(mat, "shadow_pcf",
							 material_builtin(mat, GPU_VIEW_POSITION),
							 material_builtin(mat, GPU_VIEW_NORMAL),
							 GPU_dynamic_texture(lamp->depthtex, GPU_DYNAMIC_SAMPLER_2DSHADOW, lamp->ob),
							 GPU_dynamic_uniform((float *)lamp->dynpersmat, GPU_DYNAMIC_LAMP_DYNPERSMAT, lamp->ob),
							 GPU_uniform(&lamp->bias), GPU_uniform(&lamp->slopebias),
							 GPU_uniform(&samp), GPU_uniform(&samplesize), inp, &shadowfac);
				}
				if (lamp->la->shadow_filter == LA_SHADOW_FILTER_PCF_JITTER) {
					GPU_link(mat, "shadow_pcf_jitter",
							 material_builtin(mat, GPU_VIEW_POSITION),
							 material_builtin(mat, GPU_VIEW_NORMAL),
							 GPU_dynamic_texture(lamp->depthtex, GPU_DYNAMIC_SAMPLER_2DSHADOW, lamp->ob),
							 GPU_dynamic_uniform((float *)lamp->dynpersmat, GPU_DYNAMIC_LAMP_DYNPERSMAT, lamp->ob),
							 GPU_uniform(&lamp->bias), GPU_uniform(&lamp->slopebias),
							 GPU_dynamic_texture(GPU_texture_global_jitter_64(), GPU_DYNAMIC_SAMPLER_2DIMAGE, NULL),
							 GPU_uniform(&samp), GPU_uniform(&samplesize), inp, &shadowfac);
				}
				else if (lamp->la->shadow_filter == LA_SHADOW_FILTER_PCF_BAIL) {
					GPU_link(mat, "shadow_pcf_early_bail",
							 material_builtin(mat, GPU_VIEW_POSITION),
							 material_builtin(mat, GPU_VIEW_NORMAL),
							 GPU_dynamic_texture(lamp->depthtex, GPU_DYNAMIC_SAMPLER_2DSHADOW, lamp->ob),
							 GPU_dynamic_uniform((float *)lamp->dynpersmat, GPU_DYNAMIC_LAMP_DYNPERSMAT, lamp->ob),
							 GPU_uniform(&lamp->bias), GPU_uniform(&lamp->slopebias),
							 GPU_uniform(&samp), GPU_uniform(&samplesize), inp, &shadowfac);
				}
			}
			else {
				GPU_link(mat, "shadow_simple",
			             material_builtin(mat, GPU_VIEW_POSITION),
						 material_builtin(mat, GPU_VIEW_NORMAL),
			             GPU_dynamic_texture(lamp->depthtex, GPU_DYNAMIC_SAMPLER_2DSHADOW, lamp->ob),
			             GPU_dynamic_uniform((float *)lamp->dynpersmat, GPU_DYNAMIC_LAMP_DYNPERSMAT, lamp->ob),
			             GPU_uniform(&lamp->bias), GPU_uniform(&lamp->slopebias), inp, &shadowfac);
			}
		}

		GPU_link(mat, "shadows_only", inp, shadowfac, GPU_uniform(lamp->shadow_color), r_shadow);
	}
	else {
		GPU_link(mat, "set_rgb_one", r_shadow);
	}

	/* ensure shadow buffer and lamp textures will be updated */
	add_user_list(&mat->lamps, lamp);
	add_user_list(&lamp->materials, mat->ma);

	return visifac;
}

/* export the GLSL shader */

GPUShaderExport *GPU_shader_export(struct Scene *scene, struct Material *ma)
{
	static struct {
		GPUBuiltin gputype;
		GPUDynamicType dynamictype;
		GPUDataType datatype;
	} builtins[] = {
		{ GPU_VIEW_MATRIX, GPU_DYNAMIC_OBJECT_VIEWMAT, GPU_DATA_16F },
		{ GPU_INVERSE_VIEW_MATRIX, GPU_DYNAMIC_OBJECT_VIEWIMAT, GPU_DATA_16F },
		{ GPU_OBJECT_MATRIX, GPU_DYNAMIC_OBJECT_MAT, GPU_DATA_16F },
		{ GPU_INVERSE_OBJECT_MATRIX, GPU_DYNAMIC_OBJECT_IMAT, GPU_DATA_16F },
		{ GPU_LOC_TO_VIEW_MATRIX, GPU_DYNAMIC_OBJECT_LOCTOVIEWMAT, GPU_DATA_16F },
		{ GPU_INVERSE_LOC_TO_VIEW_MATRIX, GPU_DYNAMIC_OBJECT_LOCTOVIEWIMAT, GPU_DATA_16F },
		{ GPU_OBCOLOR, GPU_DYNAMIC_OBJECT_COLOR, GPU_DATA_4F },
		{ GPU_AUTO_BUMPSCALE, GPU_DYNAMIC_OBJECT_AUTOBUMPSCALE, GPU_DATA_1F },
		{ GPU_TIME, GPU_DYNAMIC_TIME, GPU_DATA_1F },
		{ 0 }
	};

	GPUShaderExport *shader = NULL;
	GPUInput *input;
	int liblen, fraglen;

	/* TODO(sergey): How to determine whether we need OSD or not here? */
	GPUMaterial *mat = GPU_material_from_blender(scene, ma, 0);
	GPUPass *pass = (mat) ? mat->pass : NULL;

	if (pass && pass->fragmentcode && pass->vertexcode) {
		shader = MEM_callocN(sizeof(GPUShaderExport), "GPUShaderExport");

		for (input = pass->inputs.first; input; input = input->next) {
			GPUInputUniform *uniform = MEM_callocN(sizeof(GPUInputUniform), "GPUInputUniform");

			if (input->ima) {
				/* image sampler uniform */
				uniform->type = GPU_DYNAMIC_SAMPLER_2DIMAGE;
				uniform->datatype = GPU_DATA_1I;
				uniform->image = input->ima;
				uniform->texnumber = input->texid;
				BLI_strncpy(uniform->varname, input->shadername, sizeof(uniform->varname));
			}
			else if (input->tex) {
				/* generated buffer */
				uniform->texnumber = input->texid;
				uniform->datatype = GPU_DATA_1I;
				BLI_strncpy(uniform->varname, input->shadername, sizeof(uniform->varname));

				switch (input->textype) {
					case GPU_SHADOW2D:
						uniform->type = GPU_DYNAMIC_SAMPLER_2DSHADOW;
						uniform->lamp = input->dynamicdata;
						break;
					case GPU_TEX2D:
						if (GPU_texture_opengl_bindcode(input->tex)) {
							uniform->type = GPU_DYNAMIC_SAMPLER_2DBUFFER;
							glBindTexture(GL_TEXTURE_2D, GPU_texture_opengl_bindcode(input->tex));
							uniform->texsize = GPU_texture_width(input->tex) * GPU_texture_height(input->tex);
							uniform->texpixels = MEM_mallocN(uniform->texsize * 4, "RGBApixels");
							glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, uniform->texpixels);
							glBindTexture(GL_TEXTURE_2D, 0);
						}
						break;

					case GPU_NONE:
					case GPU_TEXCUBE:
					case GPU_FLOAT:
					case GPU_VEC2:
					case GPU_VEC3:
					case GPU_VEC4:
					case GPU_MAT3:
					case GPU_MAT4:
					case GPU_INT:
					case GPU_ATTRIB:
						break;
				}
			}
			else {
				uniform->type = input->dynamictype;
				BLI_strncpy(uniform->varname, input->shadername, sizeof(uniform->varname));
				switch (input->type) {
					case GPU_FLOAT:
						uniform->datatype = GPU_DATA_1F;
						break;
					case GPU_VEC2:
						uniform->datatype = GPU_DATA_2F;
						break;
					case GPU_VEC3:
						uniform->datatype = GPU_DATA_3F;
						break;
					case GPU_VEC4:
						uniform->datatype = GPU_DATA_4F;
						break;
					case GPU_MAT3:
						uniform->datatype = GPU_DATA_9F;
						break;
					case GPU_MAT4:
						uniform->datatype = GPU_DATA_16F;
						break;

					case GPU_NONE:
					case GPU_INT:
					case GPU_TEX2D:
					case GPU_TEXCUBE:
					case GPU_SHADOW2D:
					case GPU_ATTRIB:
						break;
				}

				if (GPU_DYNAMIC_GROUP_FROM_TYPE(uniform->type) == GPU_DYNAMIC_GROUP_LAMP)
					uniform->lamp = input->dynamicdata;

				if (GPU_DYNAMIC_GROUP_FROM_TYPE(uniform->type) == GPU_DYNAMIC_GROUP_MAT)
					uniform->material = input->dynamicdata;
			}

			if (uniform->type != GPU_DYNAMIC_NONE)
				BLI_addtail(&shader->uniforms, uniform);
			else
				MEM_freeN(uniform);
		}

		/* process builtin uniform */
		for (int i = 0; builtins[i].gputype; i++) {
			if (mat->builtins & builtins[i].gputype) {
				GPUInputUniform *uniform = MEM_callocN(sizeof(GPUInputUniform), "GPUInputUniform");
				uniform->type = builtins[i].dynamictype;
				uniform->datatype = builtins[i].datatype;
				BLI_strncpy(uniform->varname, GPU_builtin_name(builtins[i].gputype), sizeof(uniform->varname));
				BLI_addtail(&shader->uniforms, uniform);
			}
		}

		/* now link fragment shader with library shader */
		/* TBD: remove the function that are not used in the main function */
		liblen = (pass->libcode) ? strlen(pass->libcode) : 0;
		fraglen = strlen(pass->fragmentcode);
		shader->fragment = (char *)MEM_mallocN(liblen + fraglen + 1, "GPUFragShader");
		if (pass->libcode)
			memcpy(shader->fragment, pass->libcode, liblen);
		memcpy(&shader->fragment[liblen], pass->fragmentcode, fraglen);
		shader->fragment[liblen + fraglen] = 0;

		// export the attribute
		for (int i = 0; i < mat->attribs.totlayer; i++) {
			GPUInputAttribute *attribute = MEM_callocN(sizeof(GPUInputAttribute), "GPUInputAttribute");
			attribute->type = mat->attribs.layer[i].type;
			attribute->number = mat->attribs.layer[i].glindex;
			BLI_snprintf(attribute->varname, sizeof(attribute->varname), "att%d", mat->attribs.layer[i].attribid);

			switch (attribute->type) {
				case CD_TANGENT:
					attribute->datatype = GPU_DATA_4F;
					break;
				case CD_MTFACE:
					attribute->datatype = GPU_DATA_2F;
					attribute->name = mat->attribs.layer[i].name;
					break;
				case CD_MCOL:
					attribute->datatype = GPU_DATA_4UB;
					attribute->name = mat->attribs.layer[i].name;
					break;
				case CD_ORCO:
					attribute->datatype = GPU_DATA_3F;
					break;
			}

			if (attribute->datatype != GPU_DATA_NONE)
				BLI_addtail(&shader->attributes, attribute);
			else
				MEM_freeN(attribute);
		}

		/* export the vertex shader */
		shader->vertex = BLI_strdup(pass->vertexcode);
	}

	return shader;
}

void GPU_free_shader_export(GPUShaderExport *shader)
{
	if (shader == NULL)
		return;

	for (GPUInputUniform *uniform = shader->uniforms.first; uniform; uniform = uniform->next)
		if (uniform->texpixels)
			MEM_freeN(uniform->texpixels);

	BLI_freelistN(&shader->uniforms);
	BLI_freelistN(&shader->attributes);

	if (shader->vertex)
		MEM_freeN(shader->vertex);
	if (shader->fragment)
		MEM_freeN(shader->fragment);

	MEM_freeN(shader);
}

#ifdef WITH_OPENSUBDIV
void GPU_material_update_fvar_offset(GPUMaterial *gpu_material,
                                     DerivedMesh *dm)
{
	GPUPass *pass = gpu_material->pass;
	GPUShader *shader = (pass != NULL ? pass->shader : NULL);
	ListBase *inputs = (pass != NULL ? &pass->inputs : NULL);
	GPUInput *input;

	if (shader == NULL) {
		return;
	}

	GPU_shader_bind(shader);

	for (input = inputs->first;
	     input != NULL;
	     input = input->next)
	{
		if (input->source == GPU_SOURCE_ATTRIB &&
		    input->attribtype == CD_MTFACE)
		{
			char name[64];
			/* TODO(sergey): This will work for until names are
			 * consistent, we'll need to solve this somehow in the future.
			 */
			int layer_index;
			int location;

			if (input->attribname[0] != '\0') {
				layer_index = CustomData_get_named_layer(&dm->loopData,
				                                         CD_MLOOPUV,
				                                         input->attribname);
			}
			else {
				layer_index = CustomData_get_active_layer(&dm->loopData,
				                                          CD_MLOOPUV);
			}

			BLI_snprintf(name, sizeof(name),
			             "fvar%d_offset",
			             input->attribid);
			location = GPU_shader_get_uniform(shader, name);
			GPU_shader_uniform_int(shader, location, layer_index);
		}
	}

	GPU_shader_unbind();
}
#endif
