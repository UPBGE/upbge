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

#include "KX_Light.h"
#include "KX_Camera.h"

#include "BLI_math.h"

extern "C" {
#  include "eevee_private.h"
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
			*80.0f; /* XXX : Empirical, Fit cycles power */
	}
	else if (la->type == LA_SPOT || la->type == LA_LOCAL) {
		power = 1.0f / (4.0f * evli->radius * evli->radius * M_PI * M_PI) /* 1/(4*r²*Pi²) */
			*M_PI * M_PI * M_PI * 10.0; /* XXX : Empirical, Fit cycles power */
		/* for point lights (a.k.a radius == 0.0) */
	}
	else {
		power = 1.0f;
	}
	mul_v3_fl(evli->color, power * la->energy);

	/* Lamp Type */
	evli->lamptype = (float)la->type;
}

void RAS_OpenGLLight::UpdateLight(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led)
{
	eevee_light_setup(kxlight, linfo, led);
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

