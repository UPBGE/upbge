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
#include "BLI_dynstr.h"
}

extern "C" {
	extern char datatoc_default_frag_glsl[];
	extern char datatoc_default_world_frag_glsl[];
	extern char datatoc_ltc_lib_glsl[];
	extern char datatoc_bsdf_lut_frag_glsl[];
	extern char datatoc_bsdf_common_lib_glsl[];
	extern char datatoc_bsdf_direct_lib_glsl[];
	extern char datatoc_bsdf_sampling_lib_glsl[];
	extern char datatoc_lit_surface_frag_glsl[];
	extern char datatoc_lit_surface_vert_glsl[];
	extern char datatoc_shadow_frag_glsl[];
	extern char datatoc_shadow_geom_glsl[];
	extern char datatoc_shadow_vert_glsl[];
	extern char datatoc_probe_filter_frag_glsl[];
	extern char datatoc_probe_sh_frag_glsl[];
	extern char datatoc_probe_geom_glsl[];
	extern char datatoc_probe_vert_glsl[];
	extern char datatoc_background_vert_glsl[];
}

BL_BlenderShader::BL_BlenderShader(KX_Scene *scene, struct Material *ma, int lightlayer)
	:m_blenderScene(scene->GetBlenderScene()),
	m_mat(ma),
	m_lightLayer(lightlayer),
	m_alphaBlend(GPU_BLEND_SOLID),
	m_gpuMat(nullptr)
{
	ReloadMaterial();
}

BL_BlenderShader::~BL_BlenderShader()
{
	if (m_gpuMat) {
		GPU_material_unbind(m_gpuMat);
	}
}

const RAS_Rasterizer::AttribLayerList BL_BlenderShader::GetAttribLayers(const RAS_MeshObject::LayersInfo& layersInfo) const
{
	RAS_Rasterizer::AttribLayerList attribLayers;
	GPUVertexAttribs attribs;
	GPU_material_vertex_attributes(m_gpuMat, &attribs);

	for (unsigned int i = 0; i < attribs.totlayer; ++i) {
		if (attribs.layer[i].type == CD_MTFACE || attribs.layer[i].type == CD_MCOL) {
			const char *attribname = attribs.layer[i].name;
			if (strlen(attribname) == 0) {
				// The color or uv layer is not specified, then use the active color or uv layer.
				if (attribs.layer[i].type == CD_MTFACE) {
					attribLayers[attribs.layer[i].glindex] = layersInfo.activeUv;
				}
				else {
					attribLayers[attribs.layer[i].glindex] = layersInfo.activeColor;
				}
				continue;
			}

			for (RAS_MeshObject::LayerList::const_iterator it = layersInfo.layers.begin(), end = layersInfo.layers.end();
			     it != end; ++it) {
				const RAS_MeshObject::Layer& layer = *it;
				bool found = false;
				if (attribs.layer[i].type == CD_MTFACE && layer.face && layer.name == attribname) {
					found = true;
				}
				else if (attribs.layer[i].type == CD_MCOL && layer.color && layer.name == attribname) {
					found = true;
				}
				if (found) {
					attribLayers[attribs.layer[i].glindex] = layer.index;
					break;
				}
			}
		}
	}

	return attribLayers;
}

bool BL_BlenderShader::Ok() const
{
	return (m_gpuMat != nullptr);
}

void BL_BlenderShader::ReloadMaterial()
{
	/* Create shaders library */
	char *fraglib;
	DynStr *ds_frag = BLI_dynstr_new();
	BLI_dynstr_append(ds_frag, datatoc_bsdf_common_lib_glsl);
	BLI_dynstr_append(ds_frag, datatoc_ltc_lib_glsl);
	BLI_dynstr_append(ds_frag, datatoc_bsdf_direct_lib_glsl);
	BLI_dynstr_append(ds_frag, datatoc_lit_surface_frag_glsl);
	fraglib = BLI_dynstr_get_cstring(ds_frag);
	BLI_dynstr_free(ds_frag);

	/* Get the material from eevee */
	m_gpuMat = (m_mat) ? GPU_material_from_eevee(m_blenderScene, m_mat, datatoc_lit_surface_vert_glsl, NULL, fraglib,
		"#define EEVEE_ENGINE\n"
		"#define PROBE_CAPTURE\n"
		"#define MAX_PROBE 128\n"
		"#define MAX_LIGHT 128\n"
		"#define MAX_SHADOW_CUBE 42\n"
		"#define MAX_SHADOW_MAP 64\n"
		"#define MAX_SHADOW_CASCADE 8\n"
		"#define MAX_CASCADE_NUM 4\n",
		UseInstancing()) : nullptr;
	ParseAttribs();
}

void BL_BlenderShader::SetProg(bool enable, double time, RAS_Rasterizer *rasty)
{
	if (Ok()) {
		if (enable) {
			BLI_assert(rasty != nullptr); // XXX Kinda hacky, but SetProg() should always have the rasterizer if enable is true

			float viewmat[4][4], viewinvmat[4][4];
			const MT_Matrix4x4& view = rasty->GetViewMatrix();
			const MT_Matrix4x4& viewinv = rasty->GetViewInvMatrix();
			view.getValue((float *)viewmat);
			viewinv.getValue((float *)viewinvmat);

			GPU_material_bind(m_gpuMat, m_lightLayer, m_blenderScene->lay, time, 1, viewmat, viewinvmat, nullptr, false);
		}
		else {
			GPU_material_unbind(m_gpuMat);
		}
	}
}

void BL_BlenderShader::ParseAttribs()
{
	if (!Ok()) {
		return;
	}

	GPUVertexAttribs attribs;
	GPU_material_vertex_attributes(m_gpuMat, &attribs);

	m_attribs.clear();

	for (unsigned short i = 0; i < attribs.totlayer; ++i) {
		const int type = attribs.layer[i].type;
		const int glindex = attribs.layer[i].glindex;
		if (type == CD_MTFACE) {
			m_attribs.emplace_back(glindex, RAS_Rasterizer::RAS_TEXCO_UV);
		}
		else if (type == CD_TANGENT) {
			m_attribs.emplace_back(glindex, RAS_Rasterizer::RAS_TEXTANGENT);
		}
		else if (type == CD_ORCO) {
			m_attribs.emplace_back(glindex, RAS_Rasterizer::RAS_TEXCO_ORCO);
		}
		else if (type == CD_NORMAL) {
			m_attribs.emplace_back(glindex, RAS_Rasterizer::RAS_TEXCO_NORM);
		}
		else if (type == CD_MCOL) {
			m_attribs.emplace_back(glindex, RAS_Rasterizer::RAS_TEXCO_VCOL);
		}
	}
}

void BL_BlenderShader::SetAttribs(RAS_Rasterizer *ras)
{
	if (!Ok()) {
		return;
	}

	ras->ClearTexCoords();
	ras->SetAttribs(m_attribs);
}

void BL_BlenderShader::Update(RAS_MeshSlot *ms, RAS_Rasterizer *rasty)
{
	if (!m_gpuMat || !GPU_material_bound(m_gpuMat)) {
		return;
	}

	ms->SetGpuMat(m_gpuMat);

	float viewmat[4][4];
	float *obcol = (float *)ms->m_meshUser->GetColor().getValue();

	rasty->GetViewMatrix().getValue((float *)viewmat);
	float auto_bump_scale = ms->m_pDerivedMesh != 0 ? ms->m_pDerivedMesh->auto_bump_scale : 1.0f;
	GPU_material_bind_uniforms(m_gpuMat, (float(*)[4])ms->m_meshUser->GetMatrix(), viewmat, obcol, auto_bump_scale, nullptr, nullptr);

	m_alphaBlend = GPU_material_alpha_blend(m_gpuMat, obcol);
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
