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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Mitchell Stokes
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "GPU_glew.h"
#include "GPU_shader.h"

#include <stdio.h>

#include "RAS_OpenGLLight.h"
#include "RAS_Rasterizer.h"
#include "RAS_SceneLayerData.h"

#include "KX_Light.h"
#include "KX_Camera.h"

#include "BLI_math.h"

extern "C" {
#  include "DRW_render.h"
}

RAS_OpenGLLight::RAS_OpenGLLight()
{
}

RAS_OpenGLLight::~RAS_OpenGLLight()
{
}

/***********************EEVEE SHADOWS SYSTEM************************/

/* Update buffer with lamp data */
static void eevee_light_setup(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led)
{
	/* TODO only update if data changes */
	EEVEE_LightData *evld = (EEVEE_LightData *)led->storage;
	EEVEE_Light *evli = linfo->light_data + evld->light_id;
	Object *ob = kxlight->GetBlenderObject();
	Lamp *la = (Lamp *)ob->data;
	float mat[4][4], scale[3], power, obmat[4][4];

	kxlight->NodeGetWorldTransform().getValue(&obmat[0][0]);

	/* Position */
	copy_v3_v3(evli->position, obmat[3]);

	/* Color */
	copy_v3_v3(evli->color, &la->r);

	/* Influence Radius */
	evli->dist = la->dist;

	/* Vectors */
	normalize_m4_m4_ex(mat, obmat, scale);
	copy_v3_v3(evli->forwardvec, mat[2]);
	normalize_v3(evli->forwardvec);
	negate_v3(evli->forwardvec);

	copy_v3_v3(evli->rightvec, mat[0]);
	normalize_v3(evli->rightvec);

	copy_v3_v3(evli->upvec, mat[1]);
	normalize_v3(evli->upvec);

	/* Spot size & blend */
	if (la->type == LA_SPOT) {
		evli->sizex = scale[0] / scale[2];
		evli->sizey = scale[1] / scale[2];
		evli->spotsize = cosf(la->spotsize * 0.5f);
		evli->spotblend = (1.0f - evli->spotsize) * la->spotblend;
		evli->radius = max_ff(0.001f, la->area_size);
	}
	else if (la->type == LA_AREA) {
		evli->sizex = max_ff(0.0001f, la->area_size * scale[0] * 0.5f);
		if (la->area_shape == LA_AREA_RECT) {
			evli->sizey = max_ff(0.0001f, la->area_sizey * scale[1] * 0.5f);
		}
		else {
			evli->sizey = max_ff(0.0001f, la->area_size * scale[1] * 0.5f);
		}
	}
	else {
		evli->radius = max_ff(0.001f, la->area_size);
	}

	/* Make illumination power constant */
	if (la->type == LA_AREA) {
		power = 1.0f / (evli->sizex * evli->sizey * 4.0f * M_PI) /* 1/(w*h*Pi) */
			* 80.0f; /* XXX : Empirical, Fit cycles power */
	}
	else if (la->type == LA_SPOT || la->type == LA_LOCAL) {
		power = 1.0f / (4.0f * evli->radius * evli->radius * M_PI * M_PI) /* 1/(4*r²*Pi²) */
			* M_PI * M_PI * M_PI * 10.0; /* XXX : Empirical, Fit cycles power */

		/* for point lights (a.k.a radius == 0.0) */
		// power = M_PI * M_PI * 0.78; /* XXX : Empirical, Fit cycles power */
	}
	else {
		power = 1.0f;
	}
	mul_v3_fl(evli->color, power * la->energy);

	/* Lamp Type */
	evli->lamptype = (float)la->type;

	/* No shadow by default */
	evli->shadowid = -1.0f;
}

static void eevee_shadow_cube_setup(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led)
{
	/*****************Game engine*****************************/
	EEVEE_ShadowCubeData *sh_data = (EEVEE_ShadowCubeData *)led->storage;
	EEVEE_Light *evli = linfo->light_data + sh_data->light_id;
	EEVEE_Shadow *ubo_data = linfo->shadow_data + sh_data->shadow_id;
	EEVEE_ShadowCube *cube_data = linfo->shadow_cube_data + sh_data->cube_id;
	Object *ob = kxlight->GetBlenderObject();
	Lamp *la = (Lamp *)ob->data;

	float obmat[4][4];
	kxlight->NodeGetWorldTransform().getValue(&obmat[0][0]);
	/**************End of Game engine*************************/

	int sh_nbr = 1; /* TODO: MSM */

	for (int i = 0; i < sh_nbr; ++i) {
		/* TODO : choose MSM sample point here. */
		copy_v3_v3(cube_data->position, obmat[3]);
	}

	ubo_data->bias = 0.05f * la->bias;
	ubo_data->nearf = la->clipsta;
	ubo_data->farf = la->clipend;
	ubo_data->exp = (linfo->shadow_method == SHADOW_VSM) ? la->bleedbias : la->bleedexp;

	evli->shadowid = (float)(sh_data->shadow_id);
	ubo_data->shadow_start = (float)(sh_data->layer_id);
	ubo_data->data_start = (float)(sh_data->cube_id);
	ubo_data->multi_shadow_count = (float)(sh_nbr);

	ubo_data->contact_dist = (la->mode & LA_SHAD_CONTACT) ? la->contact_dist : 0.0f;
	ubo_data->contact_bias = 0.05f * la->contact_bias;
	ubo_data->contact_spread = la->contact_spread;
	ubo_data->contact_thickness = la->contact_thickness;
}

static void frustum_min_bounding_sphere(const float corners[8][4], float r_center[3], float *r_radius)
{
#if 0 /* Simple solution but waist too much space. */
	float minvec[3], maxvec[3];

	/* compute the bounding box */
	INIT_MINMAX(minvec, maxvec);
	for (int i = 0; i < 8; ++i)	{
		minmax_v3v3_v3(minvec, maxvec, corners[i]);
	}

	/* compute the bounding sphere of this box */
	r_radius = len_v3v3(minvec, maxvec) * 0.5f;
	add_v3_v3v3(r_center, minvec, maxvec);
	mul_v3_fl(r_center, 0.5f);
#else
	/* Make the bouding sphere always centered on the front diagonal */
	add_v3_v3v3(r_center, corners[4], corners[7]);
	mul_v3_fl(r_center, 0.5f);
	*r_radius = len_v3v3(corners[0], r_center);

	/* Search the largest distance between the sphere center
	* and the front plane corners. */
	for (int i = 0; i < 4; ++i) {
		float rad = len_v3v3(corners[4 + i], r_center);
		if (rad > *r_radius) {
			*r_radius = rad;
		}
	}
#endif
}

static void eevee_shadow_cascade_setup(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led, KX_Scene *scene)
{
	/****************Game engine*********************/
	Object *ob = kxlight->GetBlenderObject();
	Lamp *la = (Lamp *)ob->data;

	float obmat[4][4];
	kxlight->NodeGetWorldTransform().getValue(&obmat[0][0]);

	/* Camera Matrices */
	float persmat[4][4], persinv[4][4];
	float viewprojmat[4][4], projinv[4][4];
	float view_near, view_far;
	float near_v[4] = { 0.0f, 0.0f, -1.0f, 1.0f };
	float far_v[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	bool is_persp = DRW_viewport_is_persp_get();

	KX_Camera *cam = scene->GetActiveCamera();

	MT_Matrix4x4 proj(cam->GetProjectionMatrix());
	MT_Matrix4x4 pers(proj * cam->GetModelviewMatrix());

	pers.getValue(&persmat[0][0]);

	invert_m4_m4(persinv, persmat);
	/* FIXME : Get near / far from Draw manager? */

	proj.getValue(&viewprojmat[0][0]);
	/**************End of Game engine******************/

	invert_m4_m4(projinv, viewprojmat);
	mul_m4_v4(projinv, near_v);
	mul_m4_v4(projinv, far_v);
	view_near = near_v[2];
	view_far = far_v[2]; /* TODO: Should be a shadow parameter */
	if (is_persp) {
		view_near /= near_v[3];
		view_far /= far_v[3];
	}

	/* Lamps Matrices */
	float viewmat[4][4], projmat[4][4];
	int sh_nbr = 1; /* TODO : MSM */
	int cascade_nbr = la->cascade_count;

	EEVEE_ShadowCascadeData *sh_data = (EEVEE_ShadowCascadeData *)led->storage;
	EEVEE_Light *evli = linfo->light_data + sh_data->light_id;
	EEVEE_Shadow *ubo_data = linfo->shadow_data + sh_data->shadow_id;
	EEVEE_ShadowCascade *cascade_data = linfo->shadow_cascade_data + sh_data->cascade_id;

	/* The technique consists into splitting
	* the view frustum into several sub-frustum
	* that are individually receiving one shadow map */

	float csm_start, csm_end;

	if (is_persp) {
		csm_start = view_near;
		csm_end = max_ff(view_far, -la->cascade_max_dist);
		/* Avoid artifacts */
		csm_end = min_ff(view_near, csm_end);
	}
	else {
		csm_start = -view_far;
		csm_end = view_far;
	}

	/* init near/far */
	for (int c = 0; c < MAX_CASCADE_NUM; ++c) {
		cascade_data->split_start[c] = csm_end;
		cascade_data->split_end[c] = csm_end;
	}

	/* Compute split planes */
	float splits_start_ndc[MAX_CASCADE_NUM];
	float splits_end_ndc[MAX_CASCADE_NUM];

	{
		/* Nearest plane */
		float p[4] = { 1.0f, 1.0f, csm_start, 1.0f };
		/* TODO: we don't need full m4 multiply here */
		mul_m4_v4(viewprojmat, p);
		splits_start_ndc[0] = p[2];
		if (is_persp) {
			splits_start_ndc[0] /= p[3];
		}
	}

	{
		/* Farthest plane */
		float p[4] = { 1.0f, 1.0f, csm_end, 1.0f };
		/* TODO: we don't need full m4 multiply here */
		mul_m4_v4(viewprojmat, p);
		splits_end_ndc[cascade_nbr - 1] = p[2];
		if (is_persp) {
			splits_end_ndc[cascade_nbr - 1] /= p[3];
		}
	}

	cascade_data->split_start[0] = csm_start;
	cascade_data->split_end[cascade_nbr - 1] = csm_end;

	for (int c = 1; c < cascade_nbr; ++c) {
		/* View Space */
		float linear_split = LERP(((float)(c) / (float)cascade_nbr), csm_start, csm_end);
		float exp_split = csm_start * powf(csm_end / csm_start, (float)(c) / (float)cascade_nbr);

		if (is_persp) {
			cascade_data->split_start[c] = LERP(la->cascade_exponent, linear_split, exp_split);
		}
		else {
			cascade_data->split_start[c] = linear_split;
		}
		cascade_data->split_end[c - 1] = cascade_data->split_start[c];

		/* Add some overlap for smooth transition */
		cascade_data->split_start[c] = LERP(la->cascade_fade, cascade_data->split_end[c - 1],
			(c > 1) ? cascade_data->split_end[c - 2] : cascade_data->split_start[0]);

		/* NDC Space */
		{
			float p[4] = { 1.0f, 1.0f, cascade_data->split_start[c], 1.0f };
			/* TODO: we don't need full m4 multiply here */
			mul_m4_v4(viewprojmat, p);
			splits_start_ndc[c] = p[2];

			if (is_persp) {
				splits_start_ndc[c] /= p[3];
			}
		}

		{
			float p[4] = { 1.0f, 1.0f, cascade_data->split_end[c - 1], 1.0f };
			/* TODO: we don't need full m4 multiply here */
			mul_m4_v4(viewprojmat, p);
			splits_end_ndc[c - 1] = p[2];

			if (is_persp) {
				splits_end_ndc[c - 1] /= p[3];
			}
		}
	}

	/* Set last cascade split fade distance into the first split_start. */
	float prev_split = (cascade_nbr > 1) ? cascade_data->split_end[cascade_nbr - 2] : cascade_data->split_start[0];
	cascade_data->split_start[0] = LERP(la->cascade_fade, cascade_data->split_end[cascade_nbr - 1], prev_split);

	/* For each cascade */
	for (int c = 0; c < cascade_nbr; ++c) {
		/* Given 8 frustum corners */
		float corners[8][4] = {
			/* Near Cap */
			{ -1.0f, -1.0f, splits_start_ndc[c], 1.0f },
			{ 1.0f, -1.0f, splits_start_ndc[c], 1.0f },
			{ -1.0f, 1.0f, splits_start_ndc[c], 1.0f },
			{ 1.0f, 1.0f, splits_start_ndc[c], 1.0f },
			/* Far Cap */
			{ -1.0f, -1.0f, splits_end_ndc[c], 1.0f },
			{ 1.0f, -1.0f, splits_end_ndc[c], 1.0f },
			{ -1.0f, 1.0f, splits_end_ndc[c], 1.0f },
			{ 1.0f, 1.0f, splits_end_ndc[c], 1.0f }
		};

		/* Transform them into world space */
		for (int i = 0; i < 8; ++i) {
			mul_m4_v4(persinv, corners[i]);
			mul_v3_fl(corners[i], 1.0f / corners[i][3]);
			corners[i][3] = 1.0f;
		}


		/* Project them into light space */
		invert_m4_m4(viewmat, obmat);
		normalize_v3(viewmat[0]);
		normalize_v3(viewmat[1]);
		normalize_v3(viewmat[2]);

		for (int i = 0; i < 8; ++i) {
			mul_m4_v4(viewmat, corners[i]);
		}

		float center[3];
		frustum_min_bounding_sphere(corners, center, &(sh_data->radius[c]));

		/* Snap projection center to nearest texel to cancel shimmering. */
		float shadow_origin[2], shadow_texco[2];
		mul_v2_v2fl(shadow_origin, center, linfo->shadow_size / (2.0f * sh_data->radius[c])); /* Light to texture space. */

		/* Find the nearest texel. */
		shadow_texco[0] = round(shadow_origin[0]);
		shadow_texco[1] = round(shadow_origin[1]);

		/* Compute offset. */
		sub_v2_v2(shadow_texco, shadow_origin);
		mul_v2_fl(shadow_texco, (2.0f * sh_data->radius[c]) / linfo->shadow_size); /* Texture to light space. */

		/* Apply offset. */
		add_v2_v2(center, shadow_texco);

		/* Expand the projection to cover frustum range */
		orthographic_m4(projmat,
			center[0] - sh_data->radius[c],
			center[0] + sh_data->radius[c],
			center[1] - sh_data->radius[c],
			center[1] + sh_data->radius[c],
			la->clipsta, la->clipend);

		mul_m4_m4m4(sh_data->viewprojmat[c], projmat, viewmat);
		mul_m4_m4m4(cascade_data->shadowmat[c], texcomat, sh_data->viewprojmat[c]);
	}

	ubo_data->bias = 0.05f * la->bias;
	ubo_data->nearf = la->clipsta;
	ubo_data->farf = la->clipend;
	ubo_data->exp = (linfo->shadow_method == SHADOW_VSM) ? la->bleedbias : la->bleedexp;

	evli->shadowid = (float)(sh_data->shadow_id);
	ubo_data->shadow_start = (float)(sh_data->layer_id);
	ubo_data->data_start = (float)(sh_data->cascade_id);
	ubo_data->multi_shadow_count = (float)(sh_nbr);

	ubo_data->contact_dist = (la->mode & LA_SHAD_CONTACT) ? la->contact_dist : 0.0f;
	ubo_data->contact_bias = 0.05f * la->contact_bias;
	ubo_data->contact_spread = la->contact_spread;
	ubo_data->contact_thickness = la->contact_thickness;
}

void RAS_OpenGLLight::UpdateLight(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led)
{
	eevee_light_setup(kxlight, linfo, led);
}

void RAS_OpenGLLight::UpdateShadowsCube(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led)
{
	eevee_shadow_cube_setup(kxlight, linfo, led);
}

void RAS_OpenGLLight::UpdateShadowsCascade(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led, KX_Scene *scene)
{
	eevee_shadow_cascade_setup(kxlight, linfo, led, scene);
}

RAS_OpenGLLight *RAS_OpenGLLight::Clone()
{
	return new RAS_OpenGLLight(*this);
}

bool RAS_OpenGLLight::HasShadow() const
{
	return m_hasShadow;
}

bool RAS_OpenGLLight::NeedShadowUpdate()
{
	if (m_staticShadow) {
		return m_requestShadowUpdate;
	}

	return true;
}

int RAS_OpenGLLight::GetShadowBindCode()
{
	return -1;
}

MT_Matrix4x4 RAS_OpenGLLight::GetViewMat()
{
	return MT_Matrix4x4::Identity();
}

MT_Matrix4x4 RAS_OpenGLLight::GetWinMat()
{
	return MT_Matrix4x4::Identity();
}

MT_Matrix4x4 RAS_OpenGLLight::GetShadowMatrix()
{
	return MT_Matrix4x4::Identity();
}

int RAS_OpenGLLight::GetShadowLayer()
{
	return 0;
}

Image *RAS_OpenGLLight::GetTextureImage(short texslot)
{
	return nullptr;
}

