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
#include "RAS_IRasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_Rect.h"

#include "RAS_OpenGLFilters/RAS_VertexShader2DFilter.h"

#include "EXP_Value.h"

#include "glew-mx.h"

static char predefinedUniformsName[RAS_2DFilter::MAX_PREDEFINED_UNIFORM_TYPE][40] = {
	"bgl_RenderedTexture", // RENDERED_TEXTURE_UNIFORM
	"bgl_LuminanceTexture", // LUMINANCE_TEXTURE_UNIFORM
	"bgl_DepthTexture", // DEPTH_TEXTURE_UNIFORM
	"bgl_RenderedTextureWidth", // RENDERED_TEXTURE_WIDTH_UNIFORM
	"bgl_RenderedTextureHeight", // RENDERED_TEXTURE_HEIGHT_UNIFORM
	"bgl_TextureCoordinateOffset" // TEXTURE_COORDINATE_OFFSETS_UNIFORM
};

RAS_2DFilter::RAS_2DFilter(RAS_2DFilterData& data, RAS_2DFilterManager *manager)
	:m_manager(manager),
	m_properties(data.propertyNames),
	m_gameObject(data.gameObject)
{
	for(unsigned int i = 0; i < TEXTURE_OFFSETS_SIZE; i++) {
		m_textureOffsets[i] = 0;
	}

	for (unsigned int i = 0; i < MAX_PREDEFINED_UNIFORM_TYPE; ++i) {
		m_predefinedUniforms[i] = -1;
	}

	for (unsigned int i = 0; i < MAX_RENDERED_TEXTURE_TYPE; ++i) {
		m_renderedTextures[i] = 0;
	}

	vertProg = VertexShader;
	char *tmp = (char *)malloc(sizeof(char) * data.shaderText.Length() + 1);
	strcpy(tmp, data.shaderText.ReadPtr());
	fragProg = tmp;

	mUse = true;
}

void RAS_2DFilter::ReleaseTextures()
{
	for (unsigned int i = 0; i < MAX_RENDERED_TEXTURE_TYPE; ++i) {
		unsigned int textureId = m_renderedTextures[i];
		if (textureId) {
			glDeleteTextures(1, &textureId);
		}
	}
}

RAS_2DFilter::~RAS_2DFilter()
{
	ReleaseTextures();
}

void RAS_2DFilter::Initialize()
{
	/* The shader must be initialized at the first frame when the canvas is set.
	 * to solve this we initialize filter at the frist render frame. */
	if (!mOk && !mError) {
		LinkProgram();
		ParseShaderProgram();
		InitializeTextures();
		ComputeTextureOffsets();
	}
}

void RAS_2DFilter::Start()
{
	Initialize();

	if (Ok()) {
		SetProg(true);
		BindUniforms();
		ApplyShader();
		DrawOverlayPlane();
	}
}

void RAS_2DFilter::End()
{
	if(Ok()) {
		SetProg(false);
	}
}

void RAS_2DFilter::ParseShaderProgram()
{
	for (unsigned int i = 0; i < MAX_PREDEFINED_UNIFORM_TYPE; ++i) {
		m_predefinedUniforms[i] = GetUniformLocation(predefinedUniformsName[i], false);
	}

	if (m_gameObject) {
		std::vector<STR_String> foundProperties;
		for (std::vector<STR_String>::iterator it = m_properties.begin(), end = m_properties.end(); it != end; ++it) {
			STR_String prop = *it;
			unsigned int loc = GetUniformLocation(prop, false);
			if (loc != -1) {
				m_propertiesLoc.push_back(loc);
				foundProperties.push_back(prop);
			}
		}
		m_properties = foundProperties;
	}
}

void RAS_2DFilter::InitializeTextures()
{
	RAS_ICanvas *canvas = m_manager->GetCanvas();
	const unsigned int texturewidth = canvas->GetWidth() + 1;
	const unsigned int textureheight = canvas->GetHeight() + 1;

	if (m_predefinedUniforms[RENDERED_TEXTURE_UNIFORM] != -1) {
		glGenTextures(1, &m_renderedTextures[RENDERED_TEXTURE]);
		glBindTexture(GL_TEXTURE_2D, m_renderedTextures[RENDERED_TEXTURE]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texturewidth, textureheight, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	}
	if (m_predefinedUniforms[DEPTH_TEXTURE_UNIFORM] != -1) {
		glGenTextures(1, &m_renderedTextures[DEPTH_TEXTURE]);
		glBindTexture(GL_TEXTURE_2D, m_renderedTextures[DEPTH_TEXTURE]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32, texturewidth, textureheight, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE,NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	}
	if (m_predefinedUniforms[LUMINANCE_TEXTURE_UNIFORM] != -1) {
		glGenTextures(1, &m_renderedTextures[LUMINANCE_TEXTURE]);
		glBindTexture(GL_TEXTURE_2D, m_renderedTextures[LUMINANCE_TEXTURE]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE16, texturewidth, textureheight, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	}
}

/* Fill the textureOffsets array with values used by the shaders to get texture samples
of nearby fragments. Or vertices or whatever.*/
void RAS_2DFilter::ComputeTextureOffsets()
{
	RAS_ICanvas *canvas = m_manager->GetCanvas();
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

void RAS_2DFilter::BindUniforms()
{
	RAS_ICanvas *canvas = m_manager->GetCanvas();
	const unsigned int texturewidth = canvas->GetWidth() + 1;
	const unsigned int textureheight = canvas->GetHeight() + 1;
	const unsigned int textureleft = canvas->GetViewPort()[0];
	const unsigned int texturebottom = canvas->GetViewPort()[1];

	if (m_predefinedUniforms[RENDERED_TEXTURE_UNIFORM] != -1) {
		// Create and bind rendered texture.
		glActiveTextureARB(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_renderedTextures[RENDERED_TEXTURE]);
		glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, textureleft, texturebottom, (GLuint)texturewidth, (GLuint)textureheight, 0);
		SetUniform(m_predefinedUniforms[RENDERED_TEXTURE_UNIFORM], 0);
	}
	if (m_predefinedUniforms[DEPTH_TEXTURE_UNIFORM] != -1) {
		// Create and bind depth texture.
		glActiveTextureARB(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, m_renderedTextures[DEPTH_TEXTURE]);
		glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, textureleft, texturebottom, (GLuint)texturewidth, (GLuint)textureheight, 0);
		SetUniform(m_predefinedUniforms[DEPTH_TEXTURE_UNIFORM], 1);
	}
	if (m_predefinedUniforms[LUMINANCE_TEXTURE_UNIFORM] != -1) {
		// Create and bind luminance texture.
		glActiveTextureARB(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, m_renderedTextures[LUMINANCE_TEXTURE]);
		glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE16, textureleft, texturebottom, (GLuint)texturewidth, (GLuint)textureheight, 0);
		SetUniform(m_predefinedUniforms[LUMINANCE_TEXTURE_UNIFORM], 2);
	}
	if (m_predefinedUniforms[RENDERED_TEXTURE_WIDTH_UNIFORM] != -1) {
		// Bind rendered texture width.
		SetUniform(m_predefinedUniforms[RENDERED_TEXTURE_WIDTH_UNIFORM], (float)texturewidth);
	}
	if (m_predefinedUniforms[RENDERED_TEXTURE_HEIGHT_UNIFORM] != -1) {
		// Bind rendered texture height.
		SetUniform(m_predefinedUniforms[RENDERED_TEXTURE_HEIGHT_UNIFORM], (float)textureheight);
	}
	if (m_predefinedUniforms[TEXTURE_COORDINATE_OFFSETS_UNIFORM] != -1) {
		// Bind texture offsets.
		SetUniformfv(m_predefinedUniforms[TEXTURE_COORDINATE_OFFSETS_UNIFORM], RAS_Uniform::UNI_FLOAT2, m_textureOffsets,
					 sizeof(float) * TEXTURE_OFFSETS_SIZE, TEXTURE_OFFSETS_SIZE / 2);
	}

	for (unsigned int i = 0, size = m_properties.size(); i < size; ++i) {
		const STR_String& prop = m_properties[i];
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

void RAS_2DFilter::DrawOverlayPlane()
{
	RAS_ICanvas *canvas = m_manager->GetCanvas();
	RAS_Rect scissor_rect = canvas->GetDisplayArea();
	glScissor(scissor_rect.GetLeft() + canvas->GetViewPort()[0], 
			  scissor_rect.GetBottom() + canvas->GetViewPort()[1],
			  scissor_rect.GetWidth() + 1,
			  scissor_rect.GetHeight() + 1);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);

	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	glPushMatrix();
	glLoadIdentity();
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();

	glActiveTextureARB(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_renderedTextures[RENDERED_TEXTURE]);

	glBegin(GL_QUADS);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

	glTexCoord2f(1.0f, 1.0f);
	glMultiTexCoord2fARB(GL_TEXTURE3_ARB, 1.0f, 1.0f);
	glVertex2f(1.0f, 1.0f);

	glTexCoord2f(0.0f, 1.0f);
	glMultiTexCoord2fARB(GL_TEXTURE3_ARB, -1.0f, 1.0f);
	glVertex2f(-1.0f, 1.0f);

	glTexCoord2f(0.0f, 0.0f);
	glMultiTexCoord2fARB(GL_TEXTURE3_ARB, -1.0f, -1.0f);
	glVertex2f(-1.0f, -1.0f);

	glTexCoord2f(1.0f, 0.0f);
	glMultiTexCoord2fARB(GL_TEXTURE3_ARB, 1.0f, -1.0f);
	glVertex2f(1.0f, -1.0f);
	glEnd();

	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();

	glEnable(GL_DEPTH_TEST);
}
