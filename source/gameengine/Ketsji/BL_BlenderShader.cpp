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
#include "RAS_MeshObject.h"
#include "RAS_MeshUser.h"
#include "RAS_Rasterizer.h"
#include "RAS_IPolygonMaterial.h"

#include "KX_Scene.h"

#include <cstring>

BL_BlenderShader::BL_BlenderShader(KX_Scene *scene, struct Material *ma,
		int lightlayer, CM_UpdateServer<RAS_IPolyMaterial> *materialUpdateServer)
	:m_blenderScene(scene->GetBlenderScene()),
	m_mat(ma),
	m_lightLayer(lightlayer),
	m_alphaBlend(GPU_BLEND_SOLID),
	m_gpuMat(nullptr),
	m_materialUpdateServer(materialUpdateServer)
{
	ReloadMaterial();
}

BL_BlenderShader::~BL_BlenderShader()
{
}

const RAS_AttributeArray::AttribList BL_BlenderShader::GetAttribs(const RAS_MeshObject::LayersInfo& layersInfo) const
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
				for (const RAS_MeshObject::Layer& layer : layersInfo.uvLayers) {
					if (layer.name == attribname) {
						attribs.push_back({glindex, RAS_AttributeArray::RAS_ATTRIB_UV, false, layer.index});
						break;
					}
				}
			}
			else {
				for (const RAS_MeshObject::Layer& layer : layersInfo.colorLayers) {
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

	m_gpuMat = (m_mat) ? GPU_material_from_blender(m_blenderScene, m_mat, false, UseInstancing()) : nullptr;

	m_materialUpdateServer->NotifyUpdate(RAS_IPolyMaterial::SHADER_MODIFIED);
}

void BL_BlenderShader::SetProg(bool enable, double time, RAS_Rasterizer *rasty)
{
	if (Ok()) {
		if (enable) {
			BLI_assert(rasty != nullptr); // XXX Kinda hacky, but SetProg() should always have the rasterizer if enable is true

			GPU_material_bind(m_gpuMat, m_lightLayer, m_blenderScene->lay, time, 1,
							  rasty->GetViewMatrix().Data(), rasty->GetViewInvMatrix().Data(), nullptr, false);
		}
		else {
			GPU_material_unbind(m_gpuMat);
		}
	}
}

void BL_BlenderShader::Update(RAS_MeshSlot *ms, RAS_Rasterizer *rasty)
{
	if (!m_gpuMat || !GPU_material_bound(m_gpuMat)) {
		return;
	}

	const float *obcol = ms->m_meshUser->GetColor().Data();

	GPU_material_bind_uniforms(m_gpuMat, (float(*)[4])ms->m_meshUser->GetMatrix(), rasty->GetViewMatrix().Data(),
							   obcol, 1.0f, nullptr, nullptr);

	m_alphaBlend = GPU_material_alpha_blend(m_gpuMat, obcol);
}

bool BL_BlenderShader::UseInstancing() const
{
	return (GPU_instanced_drawing_support() && (m_mat->shade_flag & MA_INSTANCING));
}

void BL_BlenderShader::ActivateInstancing(void *matrixoffset, void *positionoffset, void *coloroffset, unsigned int stride)
{
	if (Ok()) {
		GPU_material_bind_instancing_attrib(m_gpuMat, matrixoffset, positionoffset, coloroffset, stride);
	}
}

int BL_BlenderShader::GetAlphaBlend()
{
	return m_alphaBlend;
}
