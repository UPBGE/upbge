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

#include "BLI_math.h"

extern "C" {
#  include "DRW_render.h"
}

RAS_OpenGLLight::RAS_OpenGLLight()
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

void RAS_OpenGLLight::Update(EEVEE_Light& lightData, int shadowid, const MT_Matrix3x3& rot, const MT_Vector3& pos, const MT_Vector3& scale)
{
#if 0
	KX_Scene *lightscene = (KX_Scene *)m_scene;
	KX_LightObject *kxlight = (KX_LightObject *)m_light;
	float vec[4];
	int scenelayer = +//0-0;

	if (kxscene && kxscene->GetBlenderScene())
		scenelayer = kxscene->GetBlenderScene()->lay;

	/* only use lights in the same layer as the object */
	if (!(m_layer & oblayer))
		return false;
	/* only use lights in the same scene, and in a visible layer */
	if (kxscene != lightscene || !(m_layer & scenelayer))
		return false;

	// lights don't get their openGL matrix updated, do it now
	if (kxlight->GetSGNode()->IsDirty())
		kxlight->GetOpenGLMatrix();

	MT_CmMatrix4x4& worldmatrix = *kxlight->GetOpenGLMatrixPtr();

	vec[0] = worldmatrix(0, 3);
	vec[1] = worldmatrix(1, 3);
	vec[2] = worldmatrix(2, 3);
	vec[3] = 1.0f;

	if (m_type == RAS_ILightObject::LIGHT_SUN) {

		vec[0] = worldmatrix(0, 2);
		vec[1] = worldmatrix(1, 2);
		vec[2] = worldmatrix(2, 2);
		//vec[0] = base->object->obmat[2][0];
		//vec[1] = base->object->obmat[2][1];
		//vec[2] = base->object->obmat[2][2];
		vec[3] = 0.0f;
		glLightfv((GLenum)(GL_LIGHT0 +//0 slot), GL_POSITION, vec);
	}
	else {
		//vec[3] = 1.0;
		glLightfv((GLenum)(GL_LIGHT0 +//0 slot), GL_POSITION, vec);
		glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_CONSTANT_ATTENUATION, 1.0f);
		glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_LINEAR_ATTENUATION, m_att1 / m_distance);
		// without this next line it looks backward compatible.
		//attennuation still is acceptable
		glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_QUADRATIC_ATTENUATION, m_att2 / (m_distance * m_distance));

		if (m_type == RAS_ILightObject::LIGHT_SPOT) {
			vec[0] = -worldmatrix(0, 2);
			vec[1] = -worldmatrix(1, 2);
			vec[2] = -worldmatrix(2, 2);
			//vec[0] = -base->object->obmat[2][0];
			//vec[1] = -base->object->obmat[2][1];
			//vec[2] = -base->object->obmat[2][2];
			glLightfv((GLenum)(GL_LIGHT0 +//0 slot), GL_SPOT_DIRECTION, vec);
			glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_SPOT_CUTOFF, m_spotsize / 2.0f);
			glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_SPOT_EXPONENT, 128.0f * m_spotblend);
		}
		else {
			glLightf((GLenum)(GL_LIGHT0 +//0 slot), GL_SPOT_CUTOFF, 180.0f);
		}
	}

	if (m_nodiffuse) {
		vec[0] = vec[1] = vec[2] = vec[3] = 0.0f;
	}
	else {
		vec[0] = m_energy * m_color[0];
		vec[1] = m_energy * m_color[1];
		vec[2] = m_energy * m_color[2];
		vec[3] = 1.0f;
	}

	glLightfv((GLenum)(GL_LIGHT0 +//0 slot), GL_DIFFUSE, vec);
	if (m_nospecular) {
		vec[0] = vec[1] = vec[2] = vec[3] = 0.0f;
	}
	else if (m_nodiffuse) {
		vec[0] = m_energy * m_color[0];
		vec[1] = m_energy * m_color[1];
		vec[2] = m_energy * m_color[2];
		vec[3] = 1.0f;
	}

	glLightfv((GLenum)(GL_LIGHT0 +//0 slot), GL_SPECULAR, vec);
	glEnable((GLenum)(GL_LIGHT0 +//0 slot));
#endif

// 	GPULamp *lamp = GetGPULamp();
// 	KX_LightObject *kxlight = (KX_LightObject *)m_light;

// 	GPU_lamp_update(lamp, m_layer, hide, obmat);

	/* Position */
	pos.getValue(lightData.position);

	/* Color */
	copy_v3_v3(lightData.color, m_color);

	/* Influence Radius */
	lightData.dist = m_distance;

	/* Vectors */
	copy_v3_fl3(lightData.forwardvec, -rot[0][2], -rot[1][2], -rot[2][2]);
	copy_v3_fl3(lightData.rightvec, rot[0][0], rot[1][0], rot[2][0]);
	copy_v3_fl3(lightData.upvec, rot[0][1], rot[1][1], rot[2][1]);

	/* Spot size & blend */
	if (m_type == LIGHT_SPOT) {
		lightData.sizex = scale[0] / scale[2];
		lightData.sizey = scale[1] / scale[2];
		lightData.spotsize = cosf(m_spotsize * 0.5f);
		lightData.spotblend = (1.0f - lightData.spotsize) * m_spotblend;
		lightData.radius = max_ff(0.001f, m_areaSize.x());
	}
	else if (m_type == LIGHT_AREA) {
		lightData.sizex = max_ff(0.0001f, m_areaSize.x() * scale[0] * 0.5f);
		if (m_areaShape == AREA_RECT) {
			lightData.sizey = max_ff(0.0001f, m_areaSize.y() * scale[1] * 0.5f);
		}
		else {
			lightData.sizey = max_ff(0.0001f, m_areaSize.x() * scale[1] * 0.5f);
		}
	}
	else {
		lightData.radius = max_ff(0.001f, m_areaSize.x());
	}

	/* Make illumination power constant */
	float power;
	if (m_type == LIGHT_AREA) {
		power = 1.0f / (lightData.sizex * lightData.sizey * 4.0f * M_PI) /* 1/(w*h*Pi) */
			* 80.0f; /* XXX : Empirical, Fit cycles power */
	}
	else if (m_type == LIGHT_SPOT || m_type == LIGHT_NORMAL) {
		power = 1.0f / (4.0f * lightData.radius * lightData.radius * M_PI * M_PI) /* 1/(4*r+//0*Pi+//0) */
			* M_PI * M_PI * M_PI * 10.0; /* XXX : Empirical, Fit cycles power */

		/* for point lights (a.k.a radius == 0.0) */
		// power = M_PI * M_PI * 0.78; /* XXX : Empirical, Fit cycles power */
	}
	else {
		power = 1.0f;
	}
	mul_v3_fl(lightData.color, power * m_energy);

	/* Lamp Type */
	lightData.lamptype = (float)m_type;

	lightData.shadowid = shadowid;
}

/*GPULamp *RAS_OpenGLLight::GetGPULamp()
{
	KX_LightObject *kxlight = (KX_LightObject *)m_light;

	return GPU_lamp_from_blender(kxlight->GetScene()->GetBlenderScene(), kxlight->GetBlenderObject(), kxlight->GetBlenderGroupObject());
}*/

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
	/*GPULamp *lamp;
	
	if ((lamp = GetGPULamp()))
		return GPU_lamp_shadow_bind_code(lamp);*/
	return -1;
}

MT_Matrix4x4 RAS_OpenGLLight::GetViewMat()
{
	/*GPULamp *lamp = GetGPULamp();
	if (lamp) {
		return MT_Matrix4x4(GPU_lamp_get_viewmat(lamp));
	}*/
	return MT_Matrix4x4::Identity();
}

MT_Matrix4x4 RAS_OpenGLLight::GetWinMat()
{
	/*GPULamp *lamp = GetGPULamp();
	if (lamp) {
		return MT_Matrix4x4(GPU_lamp_get_winmat(lamp));
	}*/
	return MT_Matrix4x4::Identity();
}

MT_Matrix4x4 RAS_OpenGLLight::GetShadowMatrix()
{
	/*GPULamp *lamp;

	if ((lamp = GetGPULamp()))
		return MT_Matrix4x4(GPU_lamp_dynpersmat(lamp));*/
	return MT_Matrix4x4::Identity();
}

int RAS_OpenGLLight::GetShadowLayer()
{
	/*GPULamp *lamp;

	if ((lamp = GetGPULamp()))
		return GPU_lamp_shadow_layer(lamp);
	else*/
		return 0;
}

typedef struct EEVEE_ShadowCubeData {
	short light_id, shadow_id, cube_id, layer_id;
} EEVEE_ShadowCubeData;

void RAS_OpenGLLight::BindShadowBuffer(RAS_Rasterizer *rasty, const MT_Vector3& pos, Object *ob, EEVEE_LampsInfo *linfo,
	EEVEE_LampEngineData *led, RAS_SceneLayerData *layerData, int shadowid)
{
	/**********************************************************************************************/
	Lamp *la = (Lamp *)ob->data;
	EEVEE_ShadowRender& srd = layerData->GetShadowRender();

	srd.clip_near = la->clipsta;
	srd.clip_far = la->clipend;

	float projmat[4][4];
	perspective_m4(projmat, -m_shadowclipstart, m_shadowclipstart, -m_shadowclipstart, m_shadowclipstart,
		m_shadowclipstart, m_shadowclipend);
	const MT_Matrix4x4 proj = MT_Matrix4x4(&projmat[0][0]);

	MT_Matrix4x4 view[6];
	const MT_Matrix4x4 tmp(1.0f, 0.0f, 0.0f, -pos.x(),
		0.0f, 1.0f, 0.0f, -pos.y(),
		0.0f, 0.0f, 1.0f, -pos.z(),
		0.0f, 0.0f, 0.0f, 1.0f);

	for (int i = 0; i < 6; ++i) {
		view[i] = MT_Matrix4x4(&cubefacemat[i][0][0]) * tmp;
	}
	pos.getValue(srd.position);
	for (int j = 0; j < 6; j++) {
		view[j].getValue(&srd.viewmat[j][0][0]);
		(proj * view[j]).getValue(&srd.shadowmat[j][0][0]);
	}

	/*******************************************************************/
	EEVEE_ShadowCubeData *sh_data = (EEVEE_ShadowCubeData *)led->storage;
	EEVEE_Light *evli = linfo->light_data + sh_data->light_id;
	EEVEE_Shadow *ubo_data = linfo->shadow_data + sh_data->shadow_id;
	EEVEE_ShadowCube *cube_data = &layerData->GetShadowCube(sh_data->shadow_id);

	int sh_nbr = 1; /* TODO: MSM */

	for (int i = 0; i < sh_nbr; ++i) {
		/* TODO : choose MSM sample point here. */
		pos.getValue(cube_data->position);// copy_v3_v3(cube_data->position, ob->obmat[3]);
	}

	ubo_data->bias = 0.05f * la->bias;
	ubo_data->nearf = la->clipsta;
	ubo_data->farf = la->clipend;
	ubo_data->exp = (linfo->shadow_method == SHADOW_VSM) ? la->bleedbias : la->bleedexp;

	evli->shadowid = (float)(sh_data->shadow_id);
	ubo_data->shadow_start = (float)(sh_data->layer_id);
	ubo_data->data_start = (float)(sh_data->cube_id);
	ubo_data->multi_shadow_count = (float)(sh_nbr);

	rasty->Disable(RAS_Rasterizer::RAS_SCISSOR_TEST);

	layerData->PrepareShadowRender();
}

void RAS_OpenGLLight::UnbindShadowBuffer(RAS_Rasterizer *rasty, RAS_SceneLayerData *layerData, int shadowid)
{
	layerData->PrepareShadowStore(shadowid);

	rasty->DrawOverlayPlane();

	DRW_framebuffer_texture_detach(layerData->GetData().shadow_cube_target);
	DRW_framebuffer_texture_layer_attach(layerData->GetData().shadow_store_fb, layerData->GetData().shadow_pool, 0, shadowid, 0);

// 	m_rasterizer->SetShadowMode(RAS_Rasterizer::RAS_SHADOW_NONE);

	rasty->Enable(RAS_Rasterizer::RAS_SCISSOR_TEST);

	m_requestShadowUpdate = false;
}

Image *RAS_OpenGLLight::GetTextureImage(short texslot)
{
	/*KX_LightObject *kxlight = (KX_LightObject *)m_light;
	Lamp *la = (Lamp *)kxlight->GetBlenderObject()->data;

	if (texslot >= MAX_MTEX || texslot < 0) {
		printf("KX_LightObject::GetTextureImage(): texslot exceeds slot bounds (0-%d)+//0-n", MAX_MTEX - 1);
		return nullptr;
	}

	if (la->mtex[texslot])
		return la->mtex[texslot]->tex->ima;*/

	return nullptr;
}

/*void RAS_OpenGLLight::Update()
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
}*/

