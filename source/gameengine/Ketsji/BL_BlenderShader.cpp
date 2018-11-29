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

#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_extensions.h"

#include "BL_BlenderShader.h"

#include "RAS_BucketManager.h"
#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "RAS_InstancingBuffer.h"
#include "RAS_Rasterizer.h"
#include "RAS_IMaterial.h"

#include "KX_Scene.h"

#include <cstring>

BL_BlenderShader::BL_BlenderShader(KX_Scene *scene, struct Material *ma,
		CM_UpdateServer<RAS_IMaterial> *materialUpdateServer)
	:m_blenderScene(scene->GetBlenderScene()),
	m_mat(ma),
	m_alphaBlend(GPU_BLEND_SOLID),
	m_gpuMat(nullptr),
	m_materialUpdateServer(materialUpdateServer)
{
	ReloadMaterial();
}

BL_BlenderShader::~BL_BlenderShader()
{
}

const RAS_AttributeArray::AttribList BL_BlenderShader::GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const
{
	RAS_AttributeArray::AttribList attribs;
	GPUVertexAttribs gpuAttribs;
	GPU_material_vertex_attributes(m_gpuMat, &gpuAttribs);

	for (unsigned int i = 0; i < gpuAttribs.totlayer; ++i) {
		const int type = gpuAttribs.layer[i].type;
		const unsigned short glindex = gpuAttribs.layer[i].glindex;

		if (type == CD_MTFACE || type == CD_MCOL) {
			const char *attribname = gpuAttribs.layer[i].name;
			if (strlen(attribname) == 0) {
				// The color or uv layer is not specified, then use the active color or uv layer.
				if (type == CD_MTFACE) {
					attribs.push_back({glindex, RAS_AttributeArray::RAS_ATTRIB_UV, false, layersInfo.activeUv});
				}
				else {
					attribs.push_back({glindex, RAS_AttributeArray::RAS_ATTRIB_COLOR, false, layersInfo.activeColor});
				}
				continue;
			}

			if (type == CD_MTFACE) {
				for (const RAS_Mesh::Layer& layer : layersInfo.uvLayers) {
					if (layer.name == attribname) {
						attribs.push_back({glindex, RAS_AttributeArray::RAS_ATTRIB_UV, false, layer.index});
						break;
					}
				}
			}
			else {
				for (const RAS_Mesh::Layer& layer : layersInfo.colorLayers) {
					if (layer.name == attribname) {
						attribs.push_back({glindex, RAS_AttributeArray::RAS_ATTRIB_COLOR, false, layer.index});
						break;
					}
				}
			}
		}
		else if (type == CD_TANGENT) {
			attribs.push_back({glindex, RAS_AttributeArray::RAS_ATTRIB_TANGENT, false, 0});
		}
		else if (type == CD_ORCO) {
			attribs.push_back({glindex, RAS_AttributeArray::RAS_ATTRIB_POS, false, 0});
		}
		else if (type == CD_NORMAL) {
			attribs.push_back({glindex, RAS_AttributeArray::RAS_ATTRIB_NORM, false, 0});
		}
	}

	return attribs;
}

RAS_InstancingBuffer::Attrib BL_BlenderShader::GetInstancingAttribs() const
{
	GPUBuiltin builtins = GPU_get_material_builtins(m_gpuMat);

	RAS_InstancingBuffer::Attrib attrib = RAS_InstancingBuffer::DEFAULT_ATTRIBS;
	if (builtins & GPU_INSTANCING_COLOR) {
		attrib = (RAS_InstancingBuffer::Attrib)(attrib | RAS_InstancingBuffer::COLOR_ATTRIB);
	}
	if (builtins & GPU_INSTANCING_LAYER) {
		attrib = (RAS_InstancingBuffer::Attrib)(attrib | RAS_InstancingBuffer::LAYER_ATTRIB);
	}
	if (builtins & GPU_INSTANCING_INFO) {
		attrib = (RAS_InstancingBuffer::Attrib)(attrib | RAS_InstancingBuffer::INFO_ATTRIB);
	}

	return attrib;
}

bool BL_BlenderShader::Ok() const
{
	return (m_gpuMat != nullptr);
}

void BL_BlenderShader::ReloadMaterial()
{
	// Force regenerating shader by deleting it.
	if (m_gpuMat) {
		GPU_material_free(&m_mat->gpumaterial);
		GPU_material_free(&m_mat->gpumaterialinstancing);
	}

	GPUMaterialFlag flags = (GPUMaterialFlag)
			(GPU_MATERIAL_NO_COLOR_MANAGEMENT | (UseInstancing() ? GPU_MATERIAL_INSTANCING : 0));
	m_gpuMat = (m_mat) ? GPU_material_from_blender(m_blenderScene, m_mat, flags) : nullptr;

	m_materialUpdateServer->NotifyUpdate(RAS_IMaterial::SHADER_MODIFIED);
}

void BL_BlenderShader::BindProg(RAS_Rasterizer *rasty)
{
	GPU_material_bind(m_gpuMat, m_blenderScene->lay, rasty->GetTime(), 1,
					  rasty->GetViewMatrix().Data(), rasty->GetViewInvMatrix().Data(), nullptr, false);
}

void BL_BlenderShader::UnbindProg()
{
	GPU_material_unbind(m_gpuMat);
}

void BL_BlenderShader::UpdateLights(RAS_Rasterizer *rasty)
{
	GPU_material_update_lamps(m_gpuMat, rasty->GetViewMatrix().Data(), rasty->GetViewInvMatrix().Data());
}

void BL_BlenderShader::Update(RAS_MeshUser *meshUser, short matPassIndex, RAS_Rasterizer *rasty)
{
	if (!GPU_material_bound(m_gpuMat)) {
		return;
	}

	const float (&obcol)[4] = meshUser->GetColor().Data();

	float objectInfo[3];
	if (GPU_get_material_builtins(m_gpuMat) & GPU_OBJECT_INFO) {
		objectInfo[0] = float(meshUser->GetPassIndex());
		objectInfo[1] = float(matPassIndex);
		objectInfo[2] = meshUser->GetRandom();
	}

	GPU_material_bind_uniforms(m_gpuMat, meshUser->GetMatrix().Data(), rasty->GetViewMatrix().Data(),
			obcol, meshUser->GetLayer(), 1.0f, nullptr, objectInfo);

	m_alphaBlend = GPU_material_alpha_blend(m_gpuMat, obcol);
}

bool BL_BlenderShader::UseInstancing() const
{
	return (GPU_instanced_drawing_support() && (m_mat->shade_flag & MA_INSTANCING));
}

void BL_BlenderShader::ActivateInstancing(RAS_InstancingBuffer *buffer)
{
	GPU_material_bind_instancing_attrib(m_gpuMat, (void *)buffer->GetMatrixOffset(), (void *)buffer->GetPositionOffset(),
			(void *)buffer->GetColorOffset(), (void *)buffer->GetLayerOffset(), (void *)buffer->GetInfoOffset());
}

int BL_BlenderShader::GetAlphaBlend()
{
	return m_alphaBlend;
}
