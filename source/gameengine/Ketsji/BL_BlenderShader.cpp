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

#include "BKE_DerivedMesh.h"

#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_extensions.h"


#include "BL_BlenderShader.h"

#include "RAS_BucketManager.h"
#include "RAS_MeshObject.h"
#include "RAS_MeshUser.h"
#include "RAS_Rasterizer.h"

#include "KX_Scene.h"

extern "C" {
#  include "eevee_engine.h"
#  include "DRW_engine.h"
#  include "DRW_render.h"
}

BL_BlenderShader::BL_BlenderShader(KX_Scene *scene, struct Material *ma, int lightlayer)
	:m_blenderScene(scene->GetBlenderScene()),
	m_mat(ma),
	m_lightLayer(lightlayer),
	m_alphaBlend(GPU_BLEND_SOLID),
	m_gpuMat(nullptr),
	m_shGroup(nullptr)
{
	ReloadMaterial(scene);
}

BL_BlenderShader::~BL_BlenderShader()
{
	if (m_shGroup) {
		DRW_shgroup_free(m_shGroup);
	}
}

const RAS_AttributeArray::AttribList BL_BlenderShader::GetAttribs(const RAS_MeshObject::LayersInfo& layersInfo) const
{
	RAS_AttributeArray::AttribList attribs;

	if (!m_shGroup) {
		return attribs;
	}

	GPUShader *shader = DRW_shgroup_shader_get(m_shGroup);

	struct AttribData
	{
		std::string name;
		RAS_AttributeArray::AttribType type;
		unsigned short layer;
	};

	// Default attributes.
	std::vector<AttribData> attribData = {
		{"pos", RAS_AttributeArray::RAS_ATTRIB_POS, 0},
		{"nor", RAS_AttributeArray::RAS_ATTRIB_NORM, 0},
		{"u", RAS_AttributeArray::RAS_ATTRIB_UV, layersInfo.activeUv},
		{"c", RAS_AttributeArray::RAS_ATTRIB_COLOR, layersInfo.activeColor}
	};

	// Extra attributes for uv and color layers.
	for (const RAS_MeshObject::Layer& layer : layersInfo.layers) {
		const RAS_AttributeArray::AttribType type = (layer.face) ?
				RAS_AttributeArray::RAS_ATTRIB_UV : RAS_AttributeArray::RAS_ATTRIB_COLOR;

		const std::string prefix = (layer.face) ? "u" : "c";
		const unsigned int hash = BLI_ghashutil_strhash(layer.name.c_str());
		const std::string name = prefix + std::to_string(hash);

		attribData.push_back({name, type, layer.index});
	}

	// Try to find all the attributes and return the valid ones.
	for (const AttribData& data : attribData) {
		const short loc = GPU_shader_get_attribute(shader, data.name.c_str());
		if (loc != -1) {
			attribs.push_back({loc, data.type, data.layer});
		}
	}

	return attribs;
}

bool BL_BlenderShader::Ok() const
{
	return (m_shGroup != nullptr);
}

void BL_BlenderShader::ReloadMaterial(KX_Scene *scene)
{
	if (m_shGroup) {
		DRW_shgroup_free(m_shGroup);
	}

	if (m_mat->use_nodes && m_mat->nodetree) {
		m_gpuMat = EEVEE_material_mesh_get(m_blenderScene, m_mat, false, false, false, false);

		m_shGroup = DRW_shgroup_material_create(m_gpuMat, nullptr);
	}
	else {
		float *color_p = &m_mat->r;
		float *metal_p = &m_mat->ray_mirror;
		float *spec_p = &m_mat->spec;
		float *rough_p = &m_mat->gloss_mir;

		m_shGroup = EEVEE_default_shading_group_get_no_pass(false, false, false, false);
		DRW_shgroup_uniform_vec3(m_shGroup, "basecol", color_p, 1);
		DRW_shgroup_uniform_float(m_shGroup, "metallic", metal_p, 1);
		DRW_shgroup_uniform_float(m_shGroup, "specular", spec_p, 1);
		DRW_shgroup_uniform_float(m_shGroup, "roughness", rough_p, 1);
	}

	EEVEE_shgroup_add_standard_uniforms_game(m_shGroup, &scene->GetSceneLayerData(), EEVEE_engine_data_get());
}

void BL_BlenderShader::SetProg(bool enable, double time, RAS_Rasterizer *rasty)
{
	if (Ok()) {
		if (enable) {
			/*BLI_assert(rasty != nullptr); // XXX Kinda hacky, but SetProg() should always have the rasterizer if enable is true

			float viewmat[4][4], viewinvmat[4][4];
			const MT_Matrix4x4& view = rasty->GetViewMatrix();
			const MT_Matrix4x4& viewinv = rasty->GetViewInvMatrix();
			view.getValue((float *)viewmat);
			viewinv.getValue((float *)viewinvmat);

			GPU_material_bind(m_gpuMat, m_lightLayer, m_blenderScene->lay, time, 1, viewmat, viewinvmat, nullptr, false);*/

			DRW_bind_shader_shgroup(m_shGroup);
			/*, (DRWState)(
				DRW_STATE_WRITE_DEPTH |
				DRW_STATE_DEPTH_LESS |
				DRW_STATE_CULL_BACK |
				DRW_STATE_WRITE_COLOR));*/
		}
		else {
			//GPU_material_unbind(m_gpuMat);
		}
	}
}

void BL_BlenderShader::Update(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty)
{
	/*if (!m_gpuMat || !GPU_material_bound(m_gpuMat)) {
		return;
	}*/

// 	float *obcol = (float *)ms->m_meshUser->GetColor().getValue();

// 	rasty->GetViewMatrix().getValue((float *)viewmat);
// 	float auto_bump_scale = ms->m_pDerivedMesh != 0 ? ms->m_pDerivedMesh->auto_bump_scale : 1.0f;
// 	GPU_material_bind_uniforms(m_gpuMat, (float(*)[4])ms->m_meshUser->GetMatrix(), viewmat, obcol, auto_bump_scale, nullptr, nullptr);

// 	print_m4("obmat : ", (float(*)[4])ms->m_meshUser->GetMatrix());
	DRW_draw_geometry_prepare(m_shGroup, (float(*)[4])meshUser->GetMatrix(), nullptr, nullptr);

// 	m_alphaBlend = GPU_material_alpha_blend(m_gpuMat, obcol);
}

bool BL_BlenderShader::UseInstancing() const
{
	return m_mat->shade_flag & MA_INSTANCING;
}

void BL_BlenderShader::ActivateInstancing(void *matrixoffset, void *positionoffset, void *coloroffset, unsigned int stride)
{
	if (Ok()) {
		GPU_material_bind_instancing_attrib(m_gpuMat, matrixoffset, positionoffset, coloroffset, stride);
	}
}

void BL_BlenderShader::DesactivateInstancing()
{
	if (Ok()) {
		GPU_material_unbind_instancing_attrib(m_gpuMat);
	}
}

int BL_BlenderShader::GetAlphaBlend()
{
	return m_alphaBlend;
}
