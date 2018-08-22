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

/** \file gameengine/Converter/BL_MaterialShader.cpp
 *  \ingroup ketsji
 */

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_extensions.h"

#include "BL_MaterialShader.h"

#include "RAS_BucketManager.h"
#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "RAS_InstancingBuffer.h"
#include "RAS_Rasterizer.h"
#include "RAS_IMaterial.h"

#include "KX_Scene.h"
#include "BL_Material.h"

#include <cstring>

BL_MaterialShader::BL_MaterialShader(KX_Scene *scene, BL_Material *material, Material *ma, int alphaBlend)
	:m_blenderScene(scene->GetBlenderScene()),
	m_mat(ma),
	m_alphaBlend(alphaBlend),
	m_gpuMat(nullptr),
	m_material(material)
{
	ReloadMaterial();
}

BL_MaterialShader::~BL_MaterialShader()
{
}

bool BL_MaterialShader::Ok() const
{
	return (m_gpuMat != nullptr);
}

void BL_MaterialShader::ReloadMaterial()
{
	// Force regenerating shader by deleting it.
	if (m_gpuMat) {
		GPU_material_free(&m_mat->gpumaterial);
		GPU_material_free(&m_mat->gpumaterialinstancing);
	}

	GPUMaterialFlag flags = GPU_MATERIAL_NO_COLOR_MANAGEMENT;
	if (GPU_instanced_drawing_support() && (m_mat->shade_flag & MA_INSTANCING)) {
		m_geomMode = GEOM_INSTANCING;
		flags = (GPUMaterialFlag)(flags | GPU_MATERIAL_INSTANCING);
	}

	m_gpuMat = GPU_material_from_blender(m_blenderScene, m_mat, flags);

	m_material->NotifyUpdate(RAS_IMaterial::SHADER_MODIFIED);
}

void BL_MaterialShader::Activate(RAS_Rasterizer *rasty)
{
	GPU_material_bind(m_gpuMat, m_blenderScene->lay, rasty->GetTime(), 1,
					  rasty->GetViewMatrix().Data(), rasty->GetViewInvMatrix().Data(), nullptr, false);
}

void BL_MaterialShader::Deactivate(RAS_Rasterizer *rasty)
{
	GPU_material_unbind(m_gpuMat);
}

void BL_MaterialShader::Prepare(RAS_Rasterizer *rasty)
{
	GPU_material_update_lamps(m_gpuMat, rasty->GetViewMatrix().Data(), rasty->GetViewInvMatrix().Data());
}

void BL_MaterialShader::ActivateInstancing(RAS_Rasterizer *rasty, RAS_InstancingBuffer *buffer)
{
	/* Because the geometry instancing uses setting for all instances we use the original alpha blend.
	 * This requierd that the user use "alpha blend" mode if he will mutate object color alpha.
	 */
	rasty->SetAlphaBlend(m_alphaBlend);

	GPU_material_bind_instancing_attrib(m_gpuMat, (void *)buffer->GetMatrixOffset(), (void *)buffer->GetPositionOffset(),
			(void *)buffer->GetColorOffset(), (void *)buffer->GetLayerOffset());
}

void BL_MaterialShader::ActivateMeshUser(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty, const mt::mat3x4& camtrans)
{
	const float (&obcol)[4] = meshUser->GetColor().Data();

	GPU_material_bind_uniforms(m_gpuMat, meshUser->GetMatrix().Data(), rasty->GetViewMatrix().Data(),
			obcol, meshUser->GetLayer(), 1.0f, nullptr, nullptr);

	const int alphaBlend = GPU_material_alpha_blend(m_gpuMat, obcol);
	/* we do blend modes here, because they can change per object
	 * with the same material due to obcolor/obalpha */
	if (m_alphaBlend != GEMAT_SOLID && ELEM(alphaBlend, GEMAT_SOLID, GEMAT_ALPHA, GEMAT_ALPHA_SORT)) {
		rasty->SetAlphaBlend(m_alphaBlend);
	}
	else {
		rasty->SetAlphaBlend(alphaBlend);
	}
}

const RAS_AttributeArray::AttribList BL_MaterialShader::GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const
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

RAS_InstancingBuffer::Attrib BL_MaterialShader::GetInstancingAttribs() const
{
	GPUBuiltin builtins = GPU_get_material_builtins(m_gpuMat);

	RAS_InstancingBuffer::Attrib attrib = RAS_InstancingBuffer::DEFAULT_ATTRIBS;
	if (builtins & GPU_INSTANCING_COLOR) {
		attrib = (RAS_InstancingBuffer::Attrib)(attrib | RAS_InstancingBuffer::COLOR_ATTRIB);
	}
	if (builtins & GPU_INSTANCING_LAYER) {
		attrib = (RAS_InstancingBuffer::Attrib)(attrib | RAS_InstancingBuffer::LAYER_ATTRIB);
	}

	return attrib;
}
