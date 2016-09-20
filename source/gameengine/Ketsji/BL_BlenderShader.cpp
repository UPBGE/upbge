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
#include "GPU_extensions.h"

#include "BL_BlenderShader.h"

#include "RAS_BucketManager.h"
#include "RAS_MeshObject.h"
#include "RAS_MeshUser.h"
#include "RAS_IRasterizer.h"

#include "KX_Scene.h"

BL_BlenderShader::BL_BlenderShader(KX_Scene *scene, struct Material *ma, int lightlayer)
	:m_mat(ma),
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

const RAS_IRasterizer::AttribLayerList BL_BlenderShader::GetAttribLayers(const STR_String uvsname[RAS_Texture::MaxUnits]) const
{
	RAS_IRasterizer::AttribLayerList uvLayers;
	GPUVertexAttribs attribs;
	GPU_material_vertex_attributes(m_GPUMat, &attribs);

	for (unsigned int i = 0; i < attribs.totlayer; ++i) {
		if (attribs.layer[i].type == CD_MTFACE) {
			const char *attribname = attribs.layer[i].name;
			if (strlen(attribname) == 0) {
				// The attribut use the default UV = the first one.
				uvLayers[attribs.layer[i].glindex] = 0;
				continue;
			}
			for (unsigned int j = 0; j < RAS_Texture::MaxUnits; ++j) {
				if (strcmp(uvsname[j], attribname) == 0) {
					uvLayers[attribs.layer[i].glindex] = j;
					break;
				}
			}
		}
	}

	return uvLayers;
}

void BL_BlenderShader::ReloadMaterial()
{
	m_GPUMat = (m_mat) ? GPU_material_from_blender(m_blenderScene, m_mat, false, UseInstancing()) : NULL;
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

int BL_BlenderShader::GetAttribNum() const
{
	GPUVertexAttribs attribs;
	int i, enabled = 0;

	if (!Ok())
		return enabled;

	GPU_material_vertex_attributes(m_GPUMat, &attribs);

	for (i = 0; i < attribs.totlayer; i++)
		if (attribs.layer[i].glindex + 1 > enabled)
			enabled = attribs.layer[i].glindex + 1;

	return enabled;
}

void BL_BlenderShader::ParseAttribs()
{
	if (!Ok()) {
		return;
	}

	GPUVertexAttribs attribs;
	GPU_material_vertex_attributes(m_GPUMat, &attribs);
	unsigned short attrib_num = GetAttribNum();

	m_attribs.resize(attrib_num, RAS_IRasterizer::RAS_TEXCO_DISABLE);

	for (unsigned short i = 0; i < attribs.totlayer; ++i) {
		if (attribs.layer[i].type == CD_MTFACE) {
			m_attribs[attribs.layer[i].glindex] = RAS_IRasterizer::RAS_TEXCO_UV;
		}
		else if (attribs.layer[i].type == CD_TANGENT) {
			m_attribs[attribs.layer[i].glindex] = RAS_IRasterizer::RAS_TEXTANGENT;
		}
		else if (attribs.layer[i].type == CD_ORCO) {
			m_attribs[attribs.layer[i].glindex] = RAS_IRasterizer::RAS_TEXCO_ORCO;
		}
		else if (attribs.layer[i].type == CD_NORMAL) {
			m_attribs[attribs.layer[i].glindex] = RAS_IRasterizer::RAS_TEXCO_NORM;
		}
		else if (attribs.layer[i].type == CD_MCOL) {
			m_attribs[attribs.layer[i].glindex] = RAS_IRasterizer::RAS_TEXCO_VCOL;
		}
	}
}

void BL_BlenderShader::SetAttribs(RAS_IRasterizer *ras)
{
	if (!Ok()) {
		return;
	}

	ras->ClearTexCoords();
	ras->SetAttribs(m_attribs);
}

void BL_BlenderShader::Update(RAS_MeshSlot *ms, RAS_IRasterizer *rasty)
{
	float obmat[4][4], viewmat[4][4], obcol[4];
	GPUMaterial *gpumat;

	gpumat = m_GPUMat;

	if (!gpumat || !GPU_material_bound(gpumat))
		return;

	MT_Matrix4x4 model;
	model.setValue(ms->m_meshUser->GetMatrix());

	// note: getValue gives back column major as needed by OpenGL
	model.getValue((float *)obmat);

	ms->m_meshUser->GetColor().getValue((float *)obcol);

	rasty->GetViewMatrix().getValue((float *)viewmat);
	float auto_bump_scale = ms->m_pDerivedMesh != 0 ? ms->m_pDerivedMesh->auto_bump_scale : 1.0f;
	GPU_material_bind_uniforms(gpumat, obmat, viewmat, obcol, auto_bump_scale, NULL);

	m_alphaBlend = GPU_material_alpha_blend(gpumat, obcol);
}

bool BL_BlenderShader::UseInstancing() const
{
	return (GPU_instanced_drawing_support() && (m_mat->shade_flag & MA_INSTANCING));
}

void BL_BlenderShader::ActivateInstancing(void *matrixoffset, void *positionoffset, void *coloroffset, unsigned int stride)
{
	if (Ok()) {
		GPU_material_bind_instancing_attrib(m_GPUMat, matrixoffset, positionoffset, coloroffset, stride);
	}
}

void BL_BlenderShader::DesactivateInstancing()
{
	if (Ok()) {
		GPU_material_unbind_instancing_attrib(m_GPUMat);
	}
}

int BL_BlenderShader::GetAlphaBlend()
{
	return m_alphaBlend;
}
