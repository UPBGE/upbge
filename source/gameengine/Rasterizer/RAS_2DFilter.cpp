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
* Contributor(s): Pierluigi Grassi, Porteries Tristan.
*
* ***** END GPL LICENSE BLOCK *****
*/

#include "RAS_2DFilter.h"
#include "RAS_2DFilterManager.h"
#include "RAS_2DFilterOffScreen.h"
#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_OffScreen.h"
#include "RAS_Rect.h"

#include "EXP_Value.h"

#include "GPU_glew.h"

extern "C" {
	extern char datatoc_RAS_VertexShader2DFilter_glsl[];
}

static std::string predefinedUniformsName[RAS_2DFilter::MAX_PREDEFINED_UNIFORM_TYPE] = {
	"bgl_RenderedTexture", // RENDERED_TEXTURE_UNIFORM
	"bgl_DepthTexture", // DEPTH_TEXTURE_UNIFORM
	"bgl_RenderedTextureWidth", // RENDERED_TEXTURE_WIDTH_UNIFORM
	"bgl_RenderedTextureHeight", // RENDERED_TEXTURE_HEIGHT_UNIFORM
	"bgl_TextureCoordinateOffset" // TEXTURE_COORDINATE_OFFSETS_UNIFORM
};

RAS_2DFilter::RAS_2DFilter(RAS_2DFilterData& data)
	:m_properties(data.propertyNames),
	m_gameObject(data.gameObject),
	m_uniformInitialized(false),
	m_mipmap(data.mipmap)
{
	for(unsigned int i = 0; i < TEXTURE_OFFSETS_SIZE; i++) {
		m_textureOffsets[i] = 0;
	}

	for (unsigned int i = 0; i < MAX_PREDEFINED_UNIFORM_TYPE; ++i) {
		m_predefinedUniforms[i] = -1;
	}

	for (unsigned short i = 0; i < 8; ++i) {
		m_textures[i] = 0;
	}

	m_progs[VERTEX_PROGRAM] = std::string(datatoc_RAS_VertexShader2DFilter_glsl);
	m_progs[FRAGMENT_PROGRAM] = data.shaderText;

	LinkProgram();
}

RAS_2DFilter::~RAS_2DFilter()
{
}

bool RAS_2DFilter::GetMipmap() const
{
	return m_mipmap;
}

void RAS_2DFilter::SetMipmap(bool mipmap)
{
	m_mipmap = mipmap;
}

RAS_2DFilterOffScreen *RAS_2DFilter::GetOffScreen() const
{
	return m_offScreen.get();
}

void RAS_2DFilter::SetOffScreen(RAS_2DFilterOffScreen *offScreen)
{
	m_offScreen.reset(offScreen);
}

void RAS_2DFilter::Initialize(RAS_ICanvas *canvas)
{
	/* The shader must be initialized at the first frame when the canvas is accesible.
	 * to solve this we initialize filter at the frist render frame. */
	if (!m_uniformInitialized) {
		ParseShaderProgram();
		ComputeTextureOffsets(canvas);
		m_uniformInitialized = true;
	}
}

RAS_OffScreen *RAS_2DFilter::Start(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *depthofs,
						 RAS_OffScreen *colorofs, RAS_OffScreen *targetofs)
{
	/* The off screen the filter rendered to. If the filter is invalid or uses a custom
	 * off screen the output off screen is the same as the input off screen. */
	RAS_OffScreen *outputofs = colorofs;
	if (!Ok()) {
		return outputofs;
	}

	/* The target off screen must be not the color input off screen, it can be the same as depth input
	 * screen because depth is unchanged. */
	BLI_assert(targetofs != colorofs);

	if (m_offScreen) {
		if (!m_offScreen->Update(canvas)) {
			return outputofs;
		}

		m_offScreen->Bind(rasty);
	}
	else {
		targetofs->Bind();
		outputofs = targetofs;
	}

	Initialize(canvas);

	SetProg(true);

	BindTextures(depthofs, colorofs);
	BindUniforms(canvas);

	Update(rasty, MT_Matrix4x4::Identity());

	ApplyShader();

	rasty->DrawOverlayPlane();

	UnbindTextures(depthofs, colorofs);

	if (m_offScreen) {
		m_offScreen->Unbind(rasty, canvas);
	}

	return outputofs;
}

void RAS_2DFilter::End()
{
	if(Ok()) {
		SetProg(false);
	}
}

bool RAS_2DFilter::LinkProgram()
{
	if (!RAS_Shader::LinkProgram(false)) {
		return false;
	}

	BindAttributes({{0, "bgl_InPosition"}, {1, "bgl_InTexCoord"}});

	m_uniformInitialized = false;

	return true;
}

void RAS_2DFilter::ParseShaderProgram()
{
	// Parse shader to found used uniforms.
	for (unsigned int i = 0; i < MAX_PREDEFINED_UNIFORM_TYPE; ++i) {
		m_predefinedUniforms[i] = GetUniformLocation(predefinedUniformsName[i], false);
	}

	if (m_gameObject) {
		std::vector<std::string> foundProperties;
		for (std::vector<std::string>::iterator it = m_properties.begin(), end = m_properties.end(); it != end; ++it) {
			std::string prop = *it;
			unsigned int loc = GetUniformLocation(prop, false);
			if (loc != -1) {
				m_propertiesLoc.push_back(loc);
				foundProperties.push_back(prop);
			}
		}
		m_properties = foundProperties;
	}
}

/* Fill the textureOffsets array with values used by the shaders to get texture samples
of nearby fragments. Or vertices or whatever.*/
void RAS_2DFilter::ComputeTextureOffsets(RAS_ICanvas *canvas)
{
	const GLfloat texturewidth = (GLfloat)canvas->GetWidth() + 1;
	const GLfloat textureheight = (GLfloat)canvas->GetHeight() + 1;
	const GLfloat xInc = 1.0f / texturewidth;
	const GLfloat yInc = 1.0f / textureheight;

	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			m_textureOffsets[(((i * 3) + j) * 2) + 0] = (-1.0f * xInc) + ((GLfloat)i * xInc);
			m_textureOffsets[(((i * 3) + j) * 2) + 1] = (-1.0f * yInc) + ((GLfloat)j * yInc);
		}
	}
}

void RAS_2DFilter::BindTextures(RAS_OffScreen *depthofs, RAS_OffScreen *colorofs)
{
	if (m_predefinedUniforms[RENDERED_TEXTURE_UNIFORM] != -1) {
		colorofs->BindColorTexture(8);
		if (m_mipmap) {
			colorofs->MipmapTexture();
		}
	}
	if (m_predefinedUniforms[DEPTH_TEXTURE_UNIFORM] != -1) {
		depthofs->BindDepthTexture(9);
	}

	// Bind custom textures.
	for (unsigned short i = 0; i < 8; ++i) {
		if (m_textures[i]) {
			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(GL_TEXTURE_2D, m_textures[i]);
		}
	}
}

void RAS_2DFilter::UnbindTextures(RAS_OffScreen *depthofs, RAS_OffScreen *colorofs)
{
	if (m_predefinedUniforms[RENDERED_TEXTURE_UNIFORM] != -1) {
		colorofs->UnbindColorTexture();
		if (m_mipmap) {
			colorofs->UnmipmapTexture();
		}
	}
	if (m_predefinedUniforms[DEPTH_TEXTURE_UNIFORM] != -1) {
		depthofs->UnbindDepthTexture();
	}

	// Bind custom textures.
	for (unsigned short i = 0; i < 8; ++i) {
		if (m_textures[i]) {
			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
	}

	glActiveTexture(GL_TEXTURE0);
}

void RAS_2DFilter::BindUniforms(RAS_ICanvas *canvas)
{
	if (m_predefinedUniforms[RENDERED_TEXTURE_UNIFORM] != -1) {
		SetUniform(m_predefinedUniforms[RENDERED_TEXTURE_UNIFORM], 8);
	}
	if (m_predefinedUniforms[DEPTH_TEXTURE_UNIFORM] != -1) {
		SetUniform(m_predefinedUniforms[DEPTH_TEXTURE_UNIFORM], 9);
	}
	if (m_predefinedUniforms[RENDERED_TEXTURE_WIDTH_UNIFORM] != -1) {
		// Bind rendered texture width.
		const unsigned int texturewidth = canvas->GetWidth() + 1;
		SetUniform(m_predefinedUniforms[RENDERED_TEXTURE_WIDTH_UNIFORM], (float)texturewidth);
	}
	if (m_predefinedUniforms[RENDERED_TEXTURE_HEIGHT_UNIFORM] != -1) {
		// Bind rendered texture height.
		const unsigned int textureheight = canvas->GetHeight() + 1;
		SetUniform(m_predefinedUniforms[RENDERED_TEXTURE_HEIGHT_UNIFORM], (float)textureheight);
	}
	if (m_predefinedUniforms[TEXTURE_COORDINATE_OFFSETS_UNIFORM] != -1) {
		// Bind texture offsets.
		SetUniformfv(m_predefinedUniforms[TEXTURE_COORDINATE_OFFSETS_UNIFORM], RAS_Uniform::UNI_FLOAT2, m_textureOffsets,
					 sizeof(float) * TEXTURE_OFFSETS_SIZE, TEXTURE_OFFSETS_SIZE / 2);
	}

	for (unsigned int i = 0, size = m_properties.size(); i < size; ++i) {
		const std::string& prop = m_properties[i];
		unsigned int uniformLoc = m_propertiesLoc[i];

		CValue *property = m_gameObject->GetProperty(prop);

		if (!property) {
			continue;
		}

		switch (property->GetValueType()) {
			case VALUE_INT_TYPE:
				SetUniform(uniformLoc, (int)property->GetNumber());
				break;
			case VALUE_FLOAT_TYPE:
				SetUniform(uniformLoc, (float)property->GetNumber());
				break;
			default:
				break;
		}
	}
}
