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
#include "RAS_SceneLayerData.h"

#include "KX_Scene.h"

#include "CM_Message.h" // For debugging purposes

extern "C" {
#  include "eevee_private.h"
#  include "eevee_engine.h"
#  include "DRW_engine.h"
#  include "DRW_render.h"
}

BL_BlenderShader::BL_BlenderShader(KX_Scene *scene, Material *ma, int lightlayer)
	:m_blenderScene(scene->GetBlenderScene()),
	m_mat(ma),
	m_shGroup(nullptr),
	m_gpuMat(nullptr),
	m_depthShGroup(nullptr),
	m_depthGpuMat(nullptr),
	m_depthClipShGroup(nullptr),
	m_depthClipGpuMat(nullptr)
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
		{ "pos", RAS_AttributeArray::RAS_ATTRIB_POS, 0 },
		{ "nor", RAS_AttributeArray::RAS_ATTRIB_NORM, 0 },
		{ "u", RAS_AttributeArray::RAS_ATTRIB_UV, layersInfo.activeUv },
		{ "c", RAS_AttributeArray::RAS_ATTRIB_COLOR, layersInfo.activeColor }
	};

	// Extra attributes for uv and color layers.
	for (const RAS_MeshObject::Layer& layer : layersInfo.layers) {
		const RAS_AttributeArray::AttribType type = (layer.face) ?
			RAS_AttributeArray::RAS_ATTRIB_UV : RAS_AttributeArray::RAS_ATTRIB_COLOR;

		const std::string prefix = (layer.face) ? "u" : "c";
		const unsigned int hash = BLI_ghashutil_strhash(layer.name.c_str());
		const std::string name = prefix + std::to_string(hash);

		attribData.push_back({ name, type, layer.index });
	}

	// Try to find all the attributes and return the valid ones.
	for (const AttribData& data : attribData) {
		const short loc = GPU_shader_get_attribute(shader, data.name.c_str());
		if (loc != -1) {
			attribs.push_back({ loc, data.type, data.layer });
		}
	}

	return attribs;
}

void BL_BlenderShader::ReloadMaterial(KX_Scene *scene)
{
	if (m_shGroup) {
		DRW_shgroup_free(m_shGroup);
	}
	if (m_depthShGroup) {
		DRW_shgroup_free(m_depthShGroup);
	}
	EEVEE_Data *vedata = scene->GetEeveeData();
	EEVEE_StorageList *stl = vedata->stl;

	const bool use_refract = ((m_mat->blend_flag & MA_BL_SS_REFRACTION) != 0) && ((stl->effects->enabled_effects & EFFECT_REFRACT) != 0);
	static int no_ssr = -1;
	static int first_ssr = 0;
	int *ssr_id = (stl->effects->use_ssr && !use_refract) ? &first_ssr : &no_ssr;

	/* STANDARD MATERIAL */
	if (m_mat->use_nodes && m_mat->nodetree) {
		m_gpuMat = EEVEE_material_mesh_get(m_blenderScene, m_mat, IsTransparent(), (m_mat->blend_method == MA_BM_MULTIPLY), use_refract, SHADOW_ESM);

		m_shGroup = DRW_shgroup_material_create(m_gpuMat, nullptr);
	}
	else {
		float *color_p = &m_mat->r;
		float *metal_p = &m_mat->ray_mirror;
		float *spec_p = &m_mat->spec;
		float *rough_p = &m_mat->gloss_mir;

		m_shGroup = EEVEE_default_shading_group_create_no_pass(false, false, IsTransparent(), stl->effects->use_ssr, SHADOW_ESM);
		DRW_shgroup_uniform_vec3(m_shGroup, "basecol", color_p, 1);
		DRW_shgroup_uniform_float(m_shGroup, "metallic", metal_p, 1);
		DRW_shgroup_uniform_float(m_shGroup, "specular", spec_p, 1);
		DRW_shgroup_uniform_float(m_shGroup, "roughness", rough_p, 1);
	}
	/* DEPTH MATERIAL */
	if (m_mat->use_nodes && m_mat->nodetree) {
		m_depthGpuMat = EEVEE_material_mesh_depth_get(m_blenderScene, m_mat, false, false);

		m_depthShGroup = DRW_shgroup_material_create(m_depthGpuMat, nullptr);
	}
	else {
		m_depthShGroup = stl->g_data->depth_shgrp;
	}
	/* DEPTH CLIP MATERIAL */
	if (m_mat->use_nodes && m_mat->nodetree) {
		m_depthGpuMat = EEVEE_material_mesh_depth_get(m_blenderScene, m_mat, false, false);

		m_depthShGroup = DRW_shgroup_material_create(m_depthGpuMat, nullptr);
	}
	else {
		m_depthShGroup = stl->g_data->depth_shgrp_clip;
	}

	EEVEE_shgroup_add_standard_uniforms_game(m_shGroup, (EEVEE_SceneLayerData *)&scene->GetSceneLayerData()->GetData(),
		scene->GetEeveeData(), ssr_id, &m_mat->refract_depth, use_refract);
}

GPUMaterial *BL_BlenderShader::GetGpuMaterial(RAS_Rasterizer::DrawType drawtype)
{
	switch (drawtype) {
	case RAS_Rasterizer::RAS_TEXTURED:
	{
		return m_gpuMat;
	}
	case RAS_Rasterizer::RAS_DEPTH_PASS:
	{
		return m_depthGpuMat;
	}
	case RAS_Rasterizer::RAS_DEPTH_PASS_CLIP:
	{
		return m_depthClipGpuMat;
	}
	default:
	{
		return m_gpuMat;
	}
	}
}

DRWShadingGroup *BL_BlenderShader::GetDRWShadingGroup(RAS_Rasterizer::DrawType drawtype)
{
	switch (drawtype) {
	case RAS_Rasterizer::RAS_TEXTURED:
	{
		return m_shGroup;
	}
	case RAS_Rasterizer::RAS_DEPTH_PASS:
	{
		return m_depthShGroup;
	}
	case RAS_Rasterizer::RAS_DEPTH_PASS_CLIP:
	{
		return m_depthClipShGroup;
	}
	default:
	{
		return m_shGroup;
	}
	}
}

bool BL_BlenderShader::IsValid(RAS_Rasterizer::DrawType drawtype) const
{
	switch (drawtype) {
	case RAS_Rasterizer::RAS_TEXTURED:
	{
		return m_shGroup != nullptr;
	}
	case RAS_Rasterizer::RAS_DEPTH_PASS:
	{
		return m_depthShGroup != nullptr;
	}
	case RAS_Rasterizer::RAS_DEPTH_PASS_CLIP:
	{
		return m_depthClipShGroup != nullptr;
	}
	default:
	{
		return false;
	}
	}
}

void BL_BlenderShader::PrintDebugInfos(RAS_Rasterizer::DrawType drawtype)
{
	/* Refer to RAS_Rasterizer DrawType definition for the number of each DrawType */
	char *drawTypeMsg[4] = { "m_shGroup nullptr", "m_depthShGroup nullptr", "m_depthClipShGroup nullptr", "hmmm, what happens here?" };
	int index;
	if (drawtype == RAS_Rasterizer::RAS_TEXTURED) {
		index = 0;
	}
	else if (drawtype == RAS_Rasterizer::RAS_DEPTH_PASS) {
		index = 1;
	}
	else if (drawtype == RAS_Rasterizer::RAS_DEPTH_PASS_CLIP) {
		index = 2;
	}
	else {
		index = 3;
	}
	CM_Debug("BL_BlenderShader::" << drawTypeMsg[index]);
}

bool BL_BlenderShader::IsTransparent()
{
	return ELEM(m_mat->blend_method, MA_BM_ADD, MA_BM_MULTIPLY, MA_BM_BLEND);
}

void BL_BlenderShader::SetAlphaBlendStates(RAS_Rasterizer *rasty)
{
	switch (m_mat->blend_method) {
	case MA_BM_ADD:
	{
		rasty->Enable(RAS_Rasterizer::RAS_BLEND);
		rasty->SetBlendFunc(RAS_Rasterizer::RAS_SRC_ALPHA, RAS_Rasterizer::RAS_ONE);
		break;
	}
	case MA_BM_MULTIPLY:
	{
		rasty->Enable(RAS_Rasterizer::RAS_BLEND);
		rasty->SetBlendFunc(RAS_Rasterizer::RAS_DST_COLOR, RAS_Rasterizer::RAS_ZERO);
		break;
	}
	case MA_BM_BLEND:
	{
		rasty->Enable(RAS_Rasterizer::RAS_BLEND);
		break;
	}
	default:
	{
		rasty->Disable(RAS_Rasterizer::RAS_BLEND);
		break;
	}
	}
}

void BL_BlenderShader::Activate(RAS_Rasterizer *rasty)
{
	if (!IsValid(rasty->GetDrawingMode())) {
		PrintDebugInfos(rasty->GetDrawingMode());
		return;
	}
	SetAlphaBlendStates(rasty);
	DRW_bind_shader_shgroup(GetDRWShadingGroup(rasty->GetDrawingMode()));
}

void BL_BlenderShader::Desactivate()
{
}

void BL_BlenderShader::Update(RAS_Rasterizer *UNUSED(rasty), RAS_MeshUser *meshUser)
{
	DRW_draw_geometry_prepare(m_shGroup, (float(*)[4])meshUser->GetMatrix(), nullptr, nullptr);
}