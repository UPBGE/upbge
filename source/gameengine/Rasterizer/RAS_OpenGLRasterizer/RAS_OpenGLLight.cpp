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

#include <stdio.h>


#include "RAS_OpenGLLight.h"
#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"

#include "KX_Camera.h"
#include "KX_Light.h"
#include "KX_Scene.h"

#include "DNA_lamp_types.h"
#include "DNA_scene_types.h"

#include "GPU_lamp.h"
#include "GPU_material.h"

#include "BLI_math.h"

#include "KX_Globals.h"
#include "KX_Scene.h"

extern "C" {
#  include "eevee_private.h"
#  include "DRW_render.h"
}

RAS_OpenGLLight::RAS_OpenGLLight(RAS_Rasterizer *ras)
	:m_rasterizer(ras)
{
}

RAS_OpenGLLight::~RAS_OpenGLLight()
{
	/*GPULamp *lamp;
	KX_LightObject *kxlight = (KX_LightObject *)m_light;
	Lamp *la = (Lamp *)kxlight->GetBlenderObject()->data;

	if ((lamp = GetGPULamp())) {
		float obmat[4][4] = {{0}};
		GPU_lamp_update(lamp, 0, 0, obmat);
		GPU_lamp_update_distance(lamp, la->dist, la->att1, la->att2, la->coeff_const, la->coeff_lin, la->coeff_quad);
		GPU_lamp_update_spot(lamp, la->spotsize, la->spotblend);
	}*/
}

bool RAS_OpenGLLight::Update(EEVEE_Light& lightData)
{
	//KX_Scene *lightscene = (KX_Scene *)m_scene;
	//KX_LightObject *kxlight = (KX_LightObject *)m_light;
	//float vec[4];
	//int scenelayer = +//0-0;

	//if (kxscene && kxscene->GetBlenderScene())
	//	scenelayer = kxscene->GetBlenderScene()->lay;

	///* only use lights in the same layer as the object */
	//if (!(m_layer & oblayer))
	//	return false;
	///* only use lights in the same scene, and in a visible layer */
	//if (kxscene != lightscene || !(m_layer & scenelayer))
	//	return false;

	//// lights don't get their openGL matrix updated, do it now
	//if (kxlight->GetSGNode()->IsDirty())
	//	kxlight->GetOpenGLMatrix();

	//MT_CmMatrix4x4& worldmatrix = *kxlight->GetOpenGLMatrixPtr();

	//vec[0] = worldmatrix(0, 3);
	//vec[1] = worldmatrix(1, 3);
	//vec[2] = worldmatrix(2, 3);
	//vec[3] = 1.0f;

	//if (m_type == RAS_ILightObject::LIGHT_SUN) {

	//	vec[0] = worldmatrix(0, 2);
	//	vec[1] = worldmatrix(1, 2);
	//	vec[2] = worldmatrix(2, 2);
	//	//vec[0] = base->object->obmat[2][0];
	//	//vec[1] = base->object->obmat[2][1];
	//	//vec[2] = base->object->obmat[2][2];
	//	vec[3] = 0.0f;
	//	glLightfv((GLenum)(GL_LIGHT0 +//0 slot), GL_POSITION, vec);
	//}
	//else {
	//	//vec[3] = 1.0;
	//	glLightfv((GLenum)(GL_LIGHT0 +//0 slot), GL_POSITION, vec);
	//	glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_CONSTANT_ATTENUATION, 1.0f);
	//	glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_LINEAR_ATTENUATION, m_att1 / m_distance);
	//	// without this next line it looks backward compatible.
	//	//attennuation still is acceptable
	//	glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_QUADRATIC_ATTENUATION, m_att2 / (m_distance * m_distance));

	//	if (m_type == RAS_ILightObject::LIGHT_SPOT) {
	//		vec[0] = -worldmatrix(0, 2);
	//		vec[1] = -worldmatrix(1, 2);
	//		vec[2] = -worldmatrix(2, 2);
	//		//vec[0] = -base->object->obmat[2][0];
	//		//vec[1] = -base->object->obmat[2][1];
	//		//vec[2] = -base->object->obmat[2][2];
	//		glLightfv((GLenum)(GL_LIGHT0 +//0 slot), GL_SPOT_DIRECTION, vec);
	//		glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_SPOT_CUTOFF, m_spotsize / 2.0f);
	//		glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_SPOT_EXPONENT, 128.0f * m_spotblend);
	//	}
	//	else {
	//		glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_SPOT_CUTOFF, 180.0f);
	//	}
	//}

	//if (m_nodiffuse) {
	//	vec[0] = vec[1] = vec[2] = vec[3] = 0.0f;
	//}
	//else {
	//	vec[0] = m_energy * m_color[0];
	//	vec[1] = m_energy * m_color[1];
	//	vec[2] = m_energy * m_color[2];
	//	vec[3] = 1.0f;
	//}

	//glLightfv((GLenum)(GL_LIGHT0 +//0 slot), GL_DIFFUSE, vec);
	//if (m_nospecular) {
	//	vec[0] = vec[1] = vec[2] = vec[3] = 0.0f;
	//}
	//else if (m_nodiffuse) {
	//	vec[0] = m_energy * m_color[0];
	//	vec[1] = m_energy * m_color[1];
	//	vec[2] = m_energy * m_color[2];
	//	vec[3] = 1.0f;
	//}

	//glLightfv((GLenum)(GL_LIGHT0 +//0 slot), GL_SPECULAR, vec);
	//glEnable((GLenum)(GL_LIGHT0 +//0 slot));

// 	GPULamp *lamp = GetGPULamp();
	KX_LightObject *kxlight = (KX_LightObject *)m_light;

	int hide = kxlight->GetVisible() ? 0 : 1;
	if (hide) {
		return false;
	}

	Lamp *la = (Lamp *)kxlight->GetBlenderObject()->data;
	float obmat[4][4];
	const MT_Transform trans = kxlight->NodeGetWorldTransform();
	trans.getValue(&obmat[0][0]);
// 	GPU_lamp_update(lamp, m_layer, hide, obmat);

	
	float mat[4][4], scale[3], power;

	/* Position */
	copy_v3_v3(lightData.position, obmat[3]);

	/* Color */
	copy_v3_v3(lightData.color, &la->r);

	/* Influence Radius */
	lightData.dist = la->dist;

	/* Vectors */
	normalize_m4_m4_ex(mat, obmat, scale);
	copy_v3_v3(lightData.forwardvec, mat[2]);
	normalize_v3(lightData.forwardvec);
	negate_v3(lightData.forwardvec);

	copy_v3_v3(lightData.rightvec, mat[0]);
	normalize_v3(lightData.rightvec);

	copy_v3_v3(lightData.upvec, mat[1]);
	normalize_v3(lightData.upvec);

	/* Spot size & blend */
	if (la->type == LA_SPOT) {
		lightData.sizex = scale[0] / scale[2];
		lightData.sizey = scale[1] / scale[2];
		lightData.spotsize = cosf(la->spotsize * 0.5f);
		lightData.spotblend = (1.0f - lightData.spotsize) * la->spotblend;
		lightData.radius = max_ff(0.001f, la->area_size);
	}
	else if (la->type == LA_AREA) {
		lightData.sizex = max_ff(0.0001f, la->area_size * scale[0] * 0.5f);
		if (la->area_shape == LA_AREA_RECT) {
			lightData.sizey = max_ff(0.0001f, la->area_sizey * scale[1] * 0.5f);
		}
		else {
			lightData.sizey = max_ff(0.0001f, la->area_size * scale[1] * 0.5f);
		}
	}
	else {
		lightData.radius = max_ff(0.001f, la->area_size);
	}

	/* Make illumination power constant */
	if (la->type == LA_AREA) {
		power = 1.0f / (lightData.sizex * lightData.sizey * 4.0f * M_PI) /* 1/(w*h*Pi) */
			* 80.0f; /* XXX : Empirical, Fit cycles power */
	}
	else if (la->type == LA_SPOT || la->type == LA_LOCAL) {
		power = 1.0f / (4.0f * lightData.radius * lightData.radius * M_PI * M_PI) /* 1/(4*r+//0*Pi+//0) */
			* M_PI * M_PI * M_PI * 10.0; /* XXX : Empirical, Fit cycles power */

		/* for point lights (a.k.a radius == 0.0) */
		// power = M_PI * M_PI * 0.78; /* XXX : Empirical, Fit cycles power */
	}
	else {
		power = 1.0f;
	}
	mul_v3_fl(lightData.color, power * la->energy);

	/* Lamp Type */
	lightData.lamptype = (float)la->type;

	/* No shadow by default */
	lightData.shadowid = -1.0f;

	return true;
}

GPULamp *RAS_OpenGLLight::GetGPULamp()
{
	KX_LightObject *kxlight = (KX_LightObject *)m_light;

	return GPU_lamp_from_blender(kxlight->GetScene()->GetBlenderScene(), kxlight->GetBlenderObject(), kxlight->GetBlenderGroupObject());
}

bool RAS_OpenGLLight::HasShadowBuffer()
{
	/*GPULamp *lamp;

	if ((lamp = GetGPULamp()))
		return GPU_lamp_has_shadow_buffer(lamp);
	else
		return false;*/
	return true;
}


bool RAS_OpenGLLight::NeedShadowUpdate()
{
	if (!m_staticShadow)
		return true;
	return m_requestShadowUpdate;
}

int RAS_OpenGLLight::GetShadowBindCode()
{
	GPULamp *lamp;
	
	if ((lamp = GetGPULamp()))
		return GPU_lamp_shadow_bind_code(lamp);
	return -1;
}

MT_Matrix4x4 RAS_OpenGLLight::GetViewMat()
{
	GPULamp *lamp = GetGPULamp();
	if (lamp) {
		return MT_Matrix4x4(GPU_lamp_get_viewmat(lamp));
	}
	return MT_Matrix4x4::Identity();
}

MT_Matrix4x4 RAS_OpenGLLight::GetWinMat()
{
	GPULamp *lamp = GetGPULamp();
	if (lamp) {
		return MT_Matrix4x4(GPU_lamp_get_winmat(lamp));
	}
	return MT_Matrix4x4::Identity();
}

MT_Matrix4x4 RAS_OpenGLLight::GetShadowMatrix()
{
	GPULamp *lamp;

	if ((lamp = GetGPULamp()))
		return MT_Matrix4x4(GPU_lamp_dynpersmat(lamp));
	MT_Matrix4x4 mat;
	mat.setIdentity();
	return mat;
}

int RAS_OpenGLLight::GetShadowLayer()
{
	GPULamp *lamp;

	if ((lamp = GetGPULamp()))
		return GPU_lamp_shadow_layer(lamp);
	else
		return 0;
}

void RAS_OpenGLLight::BindShadowBuffer(const MT_Vector3& pos, int id, EEVEE_SceneLayerData& sldata)
{
	float projmat[4][4];
	float viewprojmat[6][4][4];

	EEVEE_LampsInfo *linfo = sldata.lamps;
// 	EEVEE_Light *evli = linfo->light_data + evsmp->light_id;
	EEVEE_ShadowCube& evsh = linfo->shadow_cube_data[id];
	KX_LightObject *kxlight = (KX_LightObject *)m_light;
	Lamp *la = (Lamp *)kxlight->GetBlenderObject()->data;

	perspective_m4(projmat, -la->clipsta, la->clipsta, -la->clipsta, la->clipsta, la->clipsta, la->clipend);

	for (int i = 0; i < 6; ++i) {
		float tmp[4][4];
		unit_m4(tmp);
		negate_v3_v3(tmp[3], pos.getValue());
		mul_m4_m4m4(tmp, cubefacemat[i], tmp);
		mul_m4_m4m4(viewprojmat[i], projmat, tmp);
	}

	evsh.bias = 0.05f * la->bias;
	evsh.nearf = la->clipsta;
	evsh.farf = la->clipend;
	evsh.exp = la->bleedexp;

// 	evli->shadowid = (float)(evsmp->shadow_id);

	EEVEE_ShadowRender *srd = &linfo->shadow_render_data;

	srd->layer = id;
	srd->exponent = la->bleedexp;
	pos.getValue(srd->position);
	for (int j = 0; j < 6; j++) {
		float tmp[4][4];

		unit_m4(tmp);
		negate_v3_v3(tmp[3], pos.getValue());
		mul_m4_m4m4(srd->viewmat[j], cubefacemat[j], tmp);

		copy_m4_m4(srd->shadowmat[j], viewprojmat[j]);
	}

	DRW_uniformbuffer_update(sldata.shadow_render_ubo, &linfo->shadow_render_data);

	DRW_framebuffer_bind(sldata.shadow_cube_target_fb);
	static float clear_color[4] = {FLT_MAX, FLT_MAX, FLT_MAX, 0.0f};
	DRW_framebuffer_clear(true, true, false, clear_color, 1.0f);
}

void RAS_OpenGLLight::UnbindShadowBuffer(EEVEE_SceneLayerData& sldata)
{
	GPULamp *lamp = GetGPULamp();
// 	GPU_lamp_shadow_buffer_unbind(lamp);

	m_rasterizer->SetShadowMode(RAS_Rasterizer::RAS_SHADOW_NONE);

	m_requestShadowUpdate = false;
}

Image *RAS_OpenGLLight::GetTextureImage(short texslot)
{
	KX_LightObject *kxlight = (KX_LightObject *)m_light;
	Lamp *la = (Lamp *)kxlight->GetBlenderObject()->data;

	if (texslot >= MAX_MTEX || texslot < 0) {
		printf("KX_LightObject::GetTextureImage(): texslot exceeds slot bounds (0-%d)+//0-n", MAX_MTEX - 1);
		return nullptr;
	}

	if (la->mtex[texslot])
		return la->mtex[texslot]->tex->ima;

	return nullptr;
}

void RAS_OpenGLLight::Update()
{
	GPULamp *lamp;
	KX_LightObject *kxlight = (KX_LightObject *)m_light;

	if ((lamp = GetGPULamp()) != nullptr && kxlight->GetSGNode()) {
		float obmat[4][4];
		const MT_Transform trans = kxlight->NodeGetWorldTransform();
		trans.getValue(&obmat[0][0]);

		int hide = kxlight->GetVisible() ? 0 : 1;
		GPU_lamp_update(lamp, m_layer, hide, obmat);
		GPU_lamp_update_colors(lamp, m_color[0], m_color[1],
		                       m_color[2], m_energy);
		GPU_lamp_update_distance(lamp, m_distance, m_att1, m_att2, m_coeff_const, m_coeff_lin, m_coeff_quad);
		GPU_lamp_update_spot(lamp, m_spotsize, m_spotblend);
	}
}

