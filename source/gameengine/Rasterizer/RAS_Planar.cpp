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
* Contributor(s): Ulysse Martin, Tristan Porteries, Martins Upitis.
*
* ***** END GPL LICENSE BLOCK *****
*/

/** \file RAS_Planar.cpp
*  \ingroup bgerast
*/

#include "RAS_Planar.h"
#include "RAS_Texture.h"
#include "RAS_IRasterizer.h"

#include "GPU_texture.h"
#include "GPU_framebuffer.h"
#include "GPU_draw.h"

#include "BKE_image.h"
#include "KX_GameObject.h"
#include "RAS_MeshObject.h"
#include "RAS_Polygon.h"

#include "DNA_texture_types.h"

#include "glew-mx.h"

#include "BLI_math.h"

RAS_Planar::RAS_Planar(KX_GameObject *mirror, RAS_IPolyMaterial *mat)
	:m_gpuTex(NULL)
{
	m_fbo = NULL;
	m_rb = NULL;

	std::vector<RAS_ITexVert *> mirrorVerts;
	std::vector<RAS_ITexVert *>::iterator it;

	float mirrorArea = 0.0f;
	float mirrorNormal[3] = { 0.0f, 0.0f, 0.0f };
	float mirrorUp[3];
	float dist, vec[3], axis[3];
	float zaxis[3] = { 0.0f, 0.0f, 1.0f };
	float yaxis[3] = { 0.0f, 1.0f, 0.0f };
	float mirrorMat[3][3];
	float left, right, top, bottom, back;
	
	// locate the vertex assigned to mat and do following calculation in mesh coordinates
	for (int meshIndex = 0; meshIndex < mirror->GetMeshCount(); meshIndex++)
	{
		RAS_MeshObject *mesh = mirror->GetMesh(meshIndex);
		int numPolygons = mesh->NumPolygons();
		for (int polygonIndex = 0; polygonIndex < numPolygons; polygonIndex++)
		{
			RAS_Polygon *polygon = mesh->GetPolygon(polygonIndex);
			if (polygon->GetMaterial()->GetPolyMaterial() == mat)
			{
				RAS_ITexVert *v1, *v2, *v3, *v4;
				float normal[3];
				float area;
				// this polygon is part of the mirror
				v1 = polygon->GetVertex(0);
				v2 = polygon->GetVertex(1);
				v3 = polygon->GetVertex(2);
				mirrorVerts.push_back(v1);
				mirrorVerts.push_back(v2);
				mirrorVerts.push_back(v3);
				if (polygon->VertexCount() == 4) {
					v4 = polygon->GetVertex(3);
					mirrorVerts.push_back(v4);
					area = normal_quad_v3(normal, (float *)v1->getXYZ(), (float *)v2->getXYZ(), (float *)v3->getXYZ(), (float *)v4->getXYZ());
				}
				else {
					area = normal_tri_v3(normal, (float *)v1->getXYZ(), (float *)v2->getXYZ(), (float *)v3->getXYZ());
				}
				area = fabs(area);
				mirrorArea += area;
				mul_v3_fl(normal, area);
				add_v3_v3v3(mirrorNormal, mirrorNormal, normal);
			}
		}
	}
	if (mirrorVerts.size() == 0 || mirrorArea < FLT_EPSILON)
	{
		// no vertex or zero size mirror
		std::cout << "invalid mirror size" << std::endl;
	}
	// compute average normal of mirror faces
	mul_v3_fl(mirrorNormal, 1.0f / mirrorArea);
	if (normalize_v3(mirrorNormal) == 0.0f)
	{
		// no normal
		std::cout << "no normal found" << std::endl;
	}
	// the mirror plane has an equation of the type ax+by+cz = d where (a,b,c) is the normal vector
	// if the mirror is more vertical then horizontal, the Z axis is the up direction.
	// otherwise the Y axis is the up direction.
	// If the mirror is not perfectly vertical(horizontal), the Z(Y) axis projection on the mirror
	// plan by the normal will be the up direction.
	if (fabsf(mirrorNormal[2]) > fabsf(mirrorNormal[1]) &&
		fabsf(mirrorNormal[2]) > fabsf(mirrorNormal[0]))
	{
		// the mirror is more horizontal than vertical
		copy_v3_v3(axis, yaxis);
	}
	else
	{
		// the mirror is more vertical than horizontal
		copy_v3_v3(axis, zaxis);
	}
	dist = dot_v3v3(mirrorNormal, axis);
	if (fabsf(dist) < FLT_EPSILON)
	{
		// the mirror is already fully aligned with up axis
		copy_v3_v3(mirrorUp, axis);
	}
	else
	{
		// projection of axis to mirror plane through normal
		copy_v3_v3(vec, mirrorNormal);
		mul_v3_fl(vec, dist);
		sub_v3_v3v3(mirrorUp, axis, vec);
		if (normalize_v3(mirrorUp) == 0.0f)
		{
			// should not happen
			std::cout << "mirror horizontal" << std::endl;
		}
	}
	// compute rotation matrix between local coord and mirror coord
	// to match camera orientation, we select mirror z = -normal, y = up, x = y x z
	negate_v3_v3(mirrorMat[2], mirrorNormal);
	copy_v3_v3(mirrorMat[1], mirrorUp);
	cross_v3_v3v3(mirrorMat[0], mirrorMat[1], mirrorMat[2]);
	// transpose to make it a orientation matrix from local space to mirror space
	transpose_m3(mirrorMat);
	// transform all vertex to plane coordinates and determine mirror position
	left = FLT_MAX;
	right = -FLT_MAX;
	bottom = FLT_MAX;
	top = -FLT_MAX;
	back = -FLT_MAX; // most backward vertex (=highest Z coord in mirror space)
	for (it = mirrorVerts.begin(); it != mirrorVerts.end(); it++)
	{
		copy_v3_v3(vec, (float *)(*it)->getXYZ());
		mul_m3_v3(mirrorMat, vec);
		if (vec[0] < left)
			left = vec[0];
		if (vec[0] > right)
			right = vec[0];
		if (vec[1] < bottom)
			bottom = vec[1];
		if (vec[1] > top)
			top = vec[1];
		if (vec[2] > back)
			back = vec[2];
	}

	// mirror position in mirror coord
	vec[0] = (left + right) * 0.5f;
	vec[1] = (top + bottom) * 0.5f;
	vec[2] = back;
	// convert it in local space: transpose again the matrix to get back to mirror to local transform
	transpose_m3(mirrorMat);
	mul_m3_v3(mirrorMat, vec);
	// mirror position in local space
	m_mirrorPos.setValue(vec[0], vec[1], vec[2]);
	// mirror normal vector (pointed towards the back of the mirror) in local space
	m_mirrorZ.setValue(-mirrorNormal[0], -mirrorNormal[1], -mirrorNormal[2]);
}

RAS_Planar::~RAS_Planar()
{
	DetachTexture();

	/* This call has for side effect to ask regeneration of all textures
	* depending of this image.
	*/
	for (std::vector<RAS_Texture *>::iterator it = m_textureUsers.begin(), end = m_textureUsers.end(); it != end; ++it) {
		RAS_Texture *texture = *it;
		// Invalidate the planar in each material texture users.
		texture->SetPlanar(NULL);
		BKE_image_free_buffers(texture->GetImage());
	}
}

void RAS_Planar::AttachTexture()
{
	// Increment reference to make sure the gpu texture will not be freed by someone else.
	GPU_texture_ref(m_gpuTex);
	
	m_fbo = GPU_framebuffer_create();
	m_rb = GPU_renderbuffer_create(GPU_texture_width(m_gpuTex), GPU_texture_height(m_gpuTex),
		0, GPU_HDR_NONE, GPU_RENDERBUFFER_DEPTH, NULL);

	GPU_framebuffer_texture_attach_target(m_fbo, m_gpuTex, GL_TEXTURE_2D, 0, NULL);
	GPU_framebuffer_renderbuffer_attach(m_fbo, m_rb, 0, NULL);
}

void RAS_Planar::DetachTexture()
{
	if (!m_gpuTex) {
		return;
	}
	
	if (m_fbo) {
		GPU_framebuffer_texture_detach_target(m_gpuTex, GL_TEXTURE_2D);
	}
	if (m_rb) {
		GPU_framebuffer_renderbuffer_detach(m_rb);
	}

	if (m_fbo) {
		GPU_framebuffer_free(m_fbo);
		m_fbo = NULL;
	}
	if (m_rb) {
		GPU_renderbuffer_free(m_rb);
		m_rb = NULL;
	}
	

	//GPU_texture_free(m_gpuTex); //////// WARNING: Don't uncomment this for planars.
}

void RAS_Planar::GetValidTexture()
{
	BLI_assert(m_textureUsers.size() > 0);

	/* The gpu texture returned by all material textures are the same.
	* We can so use the first material texture user.
	*/
	RAS_Texture *texture = m_textureUsers[0];
	texture->CheckValidTexture();
	GPUTexture *gputex = texture->GetGPUTexture();

	if (m_gpuTex == gputex) {
		// The gpu texture is the same.
		return;
	}

	DetachTexture();

	m_gpuTex = gputex;

	AttachTexture();

	Tex *tex = texture->GetTex();
	m_useMipmap = (tex->planarfiltering == TEX_MIPMAP_MIPMAP) && GPU_get_mipmap();

	if (!m_useMipmap) {
		// Disable mipmaping.
		GPU_texture_bind(m_gpuTex, 0);
		GPU_texture_filter_mode(m_gpuTex, false, (tex->planarfiltering == TEX_MIPMAP_LINEAR), false);
		GPU_texture_unbind(m_gpuTex);
	}
}

const std::vector<RAS_Texture *>& RAS_Planar::GetTextureUsers() const
{
	return m_textureUsers;
}

void RAS_Planar::AddTextureUser(RAS_Texture *texture)
{
	m_textureUsers.push_back(texture);
	texture->SetPlanar(this);
}

void RAS_Planar::BeginRender()
{
	GetValidTexture();
}

void RAS_Planar::EndRender()
{
	if (m_useMipmap) {
		GPU_texture_bind(m_gpuTex, 0);
		GPU_texture_generate_mipmap(m_gpuTex);
		GPU_texture_unbind(m_gpuTex);
	}
}

void RAS_Planar::BindFace(RAS_IRasterizer *rasty)
{
	GPU_framebuffer_bind_no_save(m_fbo, 0);

	rasty->Clear(RAS_IRasterizer::RAS_COLOR_BUFFER_BIT | RAS_IRasterizer::RAS_DEPTH_BUFFER_BIT);
}

MT_Vector3 RAS_Planar::GetMirrorPos()
{
	return m_mirrorPos;
}

MT_Vector3 RAS_Planar::GetMirrorZ()
{
	return m_mirrorZ;
}

void RAS_Planar::EnableClipPlane(MT_Vector3 &mirrorWorldZ, MT_Scalar &mirrorPlaneDTerm, int planartype)
{
	// initializing clipping planes for reflection and refraction
	static float offset = 0.1f;
	if (planartype == TEX_PLANAR_REFLECTION) {
		double plane[4] = { -mirrorWorldZ[0], -mirrorWorldZ[1], -mirrorWorldZ[2], mirrorPlaneDTerm + offset };
		glClipPlane(GL_CLIP_PLANE0, plane);
		glEnable(GL_CLIP_PLANE0);
		glFrontFace(GL_CW);
	}
	else {
		double plane[4] = { mirrorWorldZ[0], mirrorWorldZ[1], mirrorWorldZ[2], -mirrorPlaneDTerm + offset };
		glClipPlane(GL_CLIP_PLANE0, plane);
		glEnable(GL_CLIP_PLANE0);
	}
}

void RAS_Planar::DisableClipPlane(int planartype)
{
	glDisable(GL_CLIP_PLANE0);
	if (planartype == TEX_PLANAR_REFLECTION) {
		glFrontFace(GL_CCW);
	}
}
