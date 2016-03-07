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

#include "EXP_Value.h"

#include "glew-mx.h"
#include <iostream>

const char *RAS_2DFilter::UNIFORM_NAME_RENDERED_TEXTURE = "bgl_RenderedTexture";
const char *RAS_2DFilter::UNIFORM_NAME_LUMINANCE_TEXTURE = "bgl_LuminanceTexture";
const char *RAS_2DFilter::UNIFORM_NAME_DEPTH_TEXTURE = "bgl_DepthTexture";
const char *RAS_2DFilter::UNIFORM_NAME_RENDERED_TEXTURE_WIDTH = "bgl_RenderedTextureWidth";
const char *RAS_2DFilter::UNIFORM_NAME_RENDERED_TEXTURE_HEIGHT = "bgl_RenderedTextureHeight";
const char *RAS_2DFilter::UNIFORM_NAME_TEXTURE_COORDINATE_OFFSETS = "bgl_TextureCoordinateOffset";

RAS_2DFilter::RAS_2DFilter(RAS_2DFilterData& data, RAS_2DFilterManager *manager)
	:m_manager(manager),
	m_shaderProgramUid(0),
	m_fragmentShaderUid(0),
	m_renderedTextureUniformLocation(-1),
	m_luminanceTextureUniformLocation(-1),
	m_depthTextureUniformLocation(-1),
	m_renderedTextureWidthUniformLocation(-1),
	m_renderedTextureHeightUniformLocation(-1),
	m_textureOffsetsUniformLocation(-1),
	m_renderedTextureUid(0),
	m_luminanceTextureUid(0),
	m_depthTextureUid(0),
	m_properties(data.propertyNames),
	m_gameObject(data.gameObject),
	m_passIndex(data.filterPassIndex),
	m_enabled(true),
	m_error(false),
	m_initialized(false)
{
	for(unsigned int i = 0; i < TEXTURE_OFFSETS_SIZE; i++) {
		m_textureOffsets[i] = 0;
	}
	m_fragmentShaderSourceCode = data.shaderText;
}

void RAS_2DFilter::ReleaseTextures()
{
	if(m_renderedTextureUid != -1) {
		glDeleteTextures(1, &m_renderedTextureUid);
	}
	if(m_luminanceTextureUid != -1) {
		glDeleteTextures(1, &m_luminanceTextureUid);
	}
	if(m_depthTextureUid != -1) {
		glDeleteTextures(1, &m_depthTextureUid);
	}
}

void RAS_2DFilter::DeleteShader()
{
	if (m_fragmentShaderUid) {
		glDeleteObjectARB(m_fragmentShaderUid);
	}
	if (m_shaderProgramUid) {
		glDeleteObjectARB(m_shaderProgramUid);
	}
}

RAS_2DFilter::~RAS_2DFilter()
{
	DeleteShader();
	ReleaseTextures();
}

void RAS_2DFilter::SetEnabled(bool enabled)
{
	m_enabled = enabled;
}

void RAS_2DFilter::Initialize()
{
	/* The shader must be initialized at the first frame when the canvas is set.
	 * to solve this we initialize filter at the frist render frame. */
	if (!m_initialized) {
		InitializeShader();
		InitializeTextures();
		ComputeTextureOffsets();
		m_initialized = true;
	}
}

int RAS_2DFilter::GetPassIndex()
{
	return m_passIndex;
}

void RAS_2DFilter::Start()
{
	Initialize();

	if (m_enabled && !m_error) {
		BindShaderProgram();
		BindUniforms();
		DrawOverlayPlane();
	}
}

void RAS_2DFilter::End()
{
	if(m_enabled && !m_error) {
		UnbindShaderProgram();
	}
}

void RAS_2DFilter::ParseShaderProgram()
{
	m_renderedTextureUniformLocation = glGetUniformLocationARB(m_shaderProgramUid, UNIFORM_NAME_RENDERED_TEXTURE);
	m_luminanceTextureUniformLocation = glGetUniformLocationARB(m_shaderProgramUid, UNIFORM_NAME_LUMINANCE_TEXTURE);
	m_depthTextureUniformLocation = glGetUniformLocationARB(m_shaderProgramUid, UNIFORM_NAME_DEPTH_TEXTURE);
	m_renderedTextureWidthUniformLocation = glGetUniformLocationARB(m_shaderProgramUid, UNIFORM_NAME_RENDERED_TEXTURE_WIDTH);
	m_renderedTextureHeightUniformLocation = glGetUniformLocationARB(m_shaderProgramUid, UNIFORM_NAME_RENDERED_TEXTURE_HEIGHT);
	m_textureOffsetsUniformLocation = glGetUniformLocationARB(m_shaderProgramUid, UNIFORM_NAME_TEXTURE_COORDINATE_OFFSETS);

	if (m_gameObject) {
		std::vector<STR_String> foundProperties;
		for (std::vector<STR_String>::iterator it = m_properties.begin(), end = m_properties.end(); it != end; ++it) {
			STR_String prop = *it;
			unsigned int loc = glGetUniformLocationARB(m_shaderProgramUid, prop);
			if (loc != -1) {
				m_propertiesLoc.push_back(loc);
				foundProperties.push_back(prop);
			}
		}
		m_properties = foundProperties;
	}
}

void RAS_2DFilter::InitializeShader()
{
	GLint compilationStatus;
	m_fragmentShaderUid = glCreateShaderObjectARB(GL_FRAGMENT_SHADER);
	const GLcharARB* shaderSourceCodeList[1];
	GLint shaderSourceCodeLengthList[1];
	const GLsizei shaderListSize = 1;
	shaderSourceCodeList[0] = (GLchar*)(m_fragmentShaderSourceCode.Ptr());
	shaderSourceCodeLengthList[0] = (GLint)(m_fragmentShaderSourceCode.Length());
	
	glShaderSourceARB(m_fragmentShaderUid, shaderListSize, shaderSourceCodeList, shaderSourceCodeLengthList);
	glCompileShaderARB(m_fragmentShaderUid);
	glGetObjectParameterivARB(m_fragmentShaderUid, GL_COMPILE_STATUS, &compilationStatus);
	if (!compilationStatus) {
		m_manager->PrintShaderError(m_fragmentShaderUid, "compile", m_fragmentShaderSourceCode.Ptr(), m_passIndex);
		DeleteShader();
	}
	else {
		m_shaderProgramUid = glCreateProgramObjectARB();
		glAttachObjectARB(m_shaderProgramUid, m_fragmentShaderUid);
		glLinkProgramARB(m_shaderProgramUid);
		glGetObjectParameterivARB(m_shaderProgramUid, GL_LINK_STATUS, &compilationStatus);
		if (!compilationStatus) {
			m_manager->PrintShaderError(m_fragmentShaderUid, "link", m_fragmentShaderSourceCode.Ptr(), m_passIndex);
			DeleteShader();
		}
		else {
			glValidateProgramARB(m_shaderProgramUid);
			glGetObjectParameterivARB(m_shaderProgramUid, GL_VALIDATE_STATUS, &compilationStatus);
			if (!compilationStatus) {
				m_manager->PrintShaderError(m_fragmentShaderUid, "validate", m_fragmentShaderSourceCode.Ptr(), m_passIndex);
				DeleteShader();
			}
			else {
				ParseShaderProgram();
			}
		}
	}
	if (compilationStatus != GL_TRUE) {
		m_error = true;
	}
}
void RAS_2DFilter::InitializeTextures()
{
	RAS_ICanvas *canvas = m_manager->GetCanvas();
	const unsigned int texturewidth = canvas->GetWidth() + 1;
	const unsigned int textureheight = canvas->GetHeight() + 1;

	if (m_renderedTextureUniformLocation >= 0) {
		glGenTextures(1, &m_renderedTextureUid);
		glBindTexture(GL_TEXTURE_2D, m_renderedTextureUid);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texturewidth, textureheight, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	}
	if (m_depthTextureUniformLocation >= 0) {
		glGenTextures(1, &m_depthTextureUid);
		glBindTexture(GL_TEXTURE_2D, m_depthTextureUid);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32, texturewidth, textureheight, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE,NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	}
	if (m_luminanceTextureUniformLocation >= 0) {
		glGenTextures(1, &m_luminanceTextureUid);
		glBindTexture(GL_TEXTURE_2D, m_luminanceTextureUid);
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

void RAS_2DFilter::BindShaderProgram()
{
	glUseProgramObjectARB(m_shaderProgramUid);
}

void RAS_2DFilter::UnbindShaderProgram()
{
	glUseProgramObjectARB(0);
}

void RAS_2DFilter::BindUniforms()
{
	RAS_ICanvas *canvas = m_manager->GetCanvas();
	const unsigned int texturewidth = canvas->GetWidth() + 1;
	const unsigned int textureheight = canvas->GetHeight() + 1;
	const unsigned int textureleft = canvas->GetViewPort()[0];
	const unsigned int texturebottom = canvas->GetViewPort()[1];

	if (m_renderedTextureUniformLocation >= 0) {
		// Create and bind rendered texture.
		glActiveTextureARB(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_renderedTextureUid);
		glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, textureleft, texturebottom, (GLuint)texturewidth, (GLuint)textureheight, 0);
		glUniform1iARB(m_renderedTextureUniformLocation, 0);
	}
	if (m_depthTextureUniformLocation >= 0) {
		// Create and bind depth texture.
		glActiveTextureARB(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, m_depthTextureUid);
		glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, textureleft, texturebottom, (GLuint)texturewidth, (GLuint)textureheight, 0);
		glUniform1iARB(m_depthTextureUniformLocation, 1);
	}
	if (m_luminanceTextureUniformLocation >= 0) {
		// Create and bind luminance texture.
		glActiveTextureARB(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, m_luminanceTextureUid);
		glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE16, textureleft, texturebottom, (GLuint)texturewidth, (GLuint)textureheight, 0);
		glUniform1iARB(m_luminanceTextureUniformLocation, 2);
	}
	if (m_renderedTextureWidthUniformLocation >= 0) {
		// Bind rendered texture width.
		glUniform1fARB(m_renderedTextureWidthUniformLocation, (float)texturewidth);
	}
	if (m_renderedTextureHeightUniformLocation >= 0) {
		// Bind rendered texture height.
		glUniform1fARB(m_renderedTextureHeightUniformLocation, (float)textureheight);
	}
	if (m_textureOffsetsUniformLocation >= 0) {
		// Bind texture offsets.
		glUniform2fvARB(m_textureOffsetsUniformLocation, 9, m_textureOffsets);
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
				glUniform1iARB(uniformLoc, property->GetNumber());
				break;
			case VALUE_FLOAT_TYPE:
				glUniform1fARB(uniformLoc, property->GetNumber());
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
	glBindTexture(GL_TEXTURE_2D, m_renderedTextureUid);

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
