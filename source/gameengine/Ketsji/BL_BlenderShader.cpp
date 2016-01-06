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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/BL_BlenderShader.cpp
 *  \ingroup ketsji
 */

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

// #include "BKE_global.h"
// #include "BKE_main.h"
#include "BKE_DerivedMesh.h"

#include "GPU_material.h"
#include "GPU_shader.h"

#include "BL_BlenderShader.h"
#include "BL_Material.h"

#include "RAS_BucketManager.h"
#include "RAS_MeshObject.h"
#include "RAS_IRasterizer.h"

#include "KX_Scene.h"

BL_BlenderShader::BL_BlenderShader(KX_Scene *scene, struct Material *ma, BL_Material *blmat, int lightlayer)
	:m_mat(ma),
	m_blMaterial(blmat),
	m_lightLayer(lightlayer),
	m_GPUMat(NULL)
{
	m_blenderScene = scene->GetBlenderScene();
	m_alphaBlend = GPU_BLEND_SOLID;

	ReloadMaterial();
	ParseAttribs();
}

BL_BlenderShader::~BL_BlenderShader()
{
	if (m_GPUMat)
		GPU_material_unbind(m_GPUMat);
}

void BL_BlenderShader::ParseAttribs()
{
	GPUVertexAttribs attribs;
	GPU_material_vertex_attributes(m_GPUMat, &attribs);
	int numattrib = GetAttribNum();

	for (unsigned int i = 0; i < MAXTEX; ++i) {
		m_uvLayers[i] = -1; // only to find bug.
	}

	for (unsigned int i = 0; i < attribs.totlayer; ++i) {
		if (attribs.layer[i].glindex > numattrib) {
			continue;
		}

		if (attribs.layer[i].type == CD_MTFACE) {
			const char *attribname = attribs.layer[i].name;
			if (strlen(attribname) == 0) {
				// The attribut use the default UV = the first one.
				m_uvLayers[i] = 0;
				continue;
			}
			for (unsigned int j = 0; j < MAXTEX; ++j) {
				if (strcmp(m_blMaterial->uvsName[j], attribname) == 0) {
					m_uvLayers[i] = j;
					break;
				}
			}
		}
	}
}

void BL_BlenderShader::ReloadMaterial()
{
	m_GPUMat = (m_mat) ? GPU_material_from_blender(m_blenderScene, m_mat, false) : NULL;
}

void BL_BlenderShader::SetProg(bool enable, double time, RAS_IRasterizer *rasty)
{
	if (Ok()) {
		if (enable) {
			assert(rasty != NULL); // XXX Kinda hacky, but SetProg() should always have the rasterizer if enable is true

			float viewmat[4][4], viewinvmat[4][4];
			const MT_Matrix4x4& view = rasty->GetViewMatrix();
			const MT_Matrix4x4& viewinv = rasty->GetViewInvMatrix();
			view.getValue((float *)viewmat);
			viewinv.getValue((float *)viewinvmat);

			GPU_material_bind(m_GPUMat, m_lightLayer, m_blenderScene->lay, time, 1, viewmat, viewinvmat, NULL, false);
		}
		else
			GPU_material_unbind(m_GPUMat);
	}
}

int BL_BlenderShader::GetAttribNum()
{
	GPUVertexAttribs attribs;
	int i, enabled = 0;

	if (!Ok())
		return enabled;

	GPU_material_vertex_attributes(m_GPUMat, &attribs);

	for (i = 0; i < attribs.totlayer; i++)
		if (attribs.layer[i].glindex + 1 > enabled)
			enabled = attribs.layer[i].glindex + 1;

	if (enabled > BL_MAX_ATTRIB)
		enabled = BL_MAX_ATTRIB;

	return enabled;
}

void BL_BlenderShader::SetAttribs(RAS_IRasterizer *ras)
{
	GPUVertexAttribs attribs;
	GPUMaterial *gpumat;
	int i, attrib_num;

	ras->SetAttribNum(0);

	if (!Ok())
		return;

	gpumat = m_GPUMat;
	if (ras->GetDrawingMode() == RAS_IRasterizer::KX_TEXTURED || (ras->GetDrawingMode() == RAS_IRasterizer::KX_SHADOW &&
	                                                              m_blMaterial->alphablend != GEMAT_SOLID && !ras->GetUsingOverrideShader())) {
		GPU_material_vertex_attributes(gpumat, &attribs);
		attrib_num = GetAttribNum();

		ras->SetTexCoordNum(0);
		ras->SetAttribNum(attrib_num);
		for (i = 0; i < attrib_num; i++)
			ras->SetAttrib(RAS_IRasterizer::RAS_TEXCO_DISABLE, i);

		for (i = 0; i < attribs.totlayer; i++) {
			if (attribs.layer[i].glindex > attrib_num)
				continue;

			if (attribs.layer[i].type == CD_MTFACE)
				ras->SetAttrib(RAS_IRasterizer::RAS_TEXCO_UV, attribs.layer[i].glindex, m_uvLayers[i]);
			else if (attribs.layer[i].type == CD_TANGENT)
				ras->SetAttrib(RAS_IRasterizer::RAS_TEXTANGENT, attribs.layer[i].glindex);
			else if (attribs.layer[i].type == CD_ORCO)
				ras->SetAttrib(RAS_IRasterizer::RAS_TEXCO_ORCO, attribs.layer[i].glindex);
			else if (attribs.layer[i].type == CD_NORMAL)
				ras->SetAttrib(RAS_IRasterizer::RAS_TEXCO_NORM, attribs.layer[i].glindex);
			else if (attribs.layer[i].type == CD_MCOL)
				ras->SetAttrib(RAS_IRasterizer::RAS_TEXCO_VCOL, attribs.layer[i].glindex);
			else
				ras->SetAttrib(RAS_IRasterizer::RAS_TEXCO_DISABLE, attribs.layer[i].glindex);
		}
	}
}

void BL_BlenderShader::Update(RAS_MeshSlot *ms, RAS_IRasterizer *rasty)
{
	float obmat[4][4], obcol[4];
	GPUMaterial *gpumat;

	gpumat = m_GPUMat;

	if (!gpumat || !GPU_material_bound(gpumat))
		return;

	MT_Matrix4x4 model;
	model.setValue(ms->m_OpenGLMatrix);

	// note: getValue gives back column major as needed by OpenGL
	model.getValue((float *)obmat);

	ms->m_RGBAcolor.getValue((float *)obcol);

	float auto_bump_scale = ms->m_pDerivedMesh != 0 ? ms->m_pDerivedMesh->auto_bump_scale : 1.0f;
	GPU_material_bind_uniforms(gpumat, obmat, obcol, auto_bump_scale, NULL);

	m_alphaBlend = GPU_material_alpha_blend(gpumat, obcol);
}

int BL_BlenderShader::GetAlphaBlend()
{
	return m_alphaBlend;
}

bool BL_BlenderShader::Equals(BL_BlenderShader *blshader)
{
	// to avoid unneeded state switches
	return (blshader && m_mat == blshader->m_mat && m_lightLayer == blshader->m_lightLayer);
}
