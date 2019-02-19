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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_OpenGLRasterizer/RAS_OpenGLDebugDraw.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_OpenGLDebugDraw.h"
#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_DebugDraw.h"

#include "GPU_material.h"
#include "GPU_glew.h"
#include "GPU_matrix.h"

#include "KX_Globals.h"
#include "KX_Scene.h"
#include "KX_Camera.h"

extern "C" {
#  include "BLF_api.h"
#  include "DRW_render.h"
#  include "GPU_immediate.h"
}

RAS_OpenGLDebugDraw::RAS_OpenGLDebugDraw():
	m_genericProg(-1),
	m_vbo(-1),
	m_wireibo(-1),
	m_solidibo(-1)
{
	/* program */
	const char *v =
		"#version 330\n"
		"uniform mat4 ModelViewProjectionMatrix;\n"
		"in vec4 bgeDebugPos;\n"
		"void main()\n"
		"{\n"
		"	gl_Position = ModelViewProjectionMatrix * bgeDebugPos;\n"
		"}\n";

	const char *f =
		"#version 330\n"
		"uniform vec4 color;\n"
		"out vec4 fragColor;\n"
		"void main()\n"
		"{\n"
		"	fragColor = color;\n"
		"}\n";

	m_genericProg = glCreateProgram();

	unsigned int vertexShader = glCreateShader(GL_VERTEX_SHADER);
	unsigned int fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);

	glShaderSource(vertexShader, 1, &v, NULL);
	glShaderSource(fragmentShader, 1, &f, NULL);

	glCompileShader(vertexShader);
	glCompileShader(fragmentShader);

	glAttachShader(m_genericProg, vertexShader);
	glAttachShader(m_genericProg, fragmentShader);

	glBindAttribLocation(m_genericProg, 0, "bgeDebugPos");

	glLinkProgram(m_genericProg);

	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);

	/* check link */
	GLint stat;
	glGetProgramiv(m_genericProg, GL_LINK_STATUS, &stat);
	if (!stat) {
		GLchar log[1000];
		GLsizei len;
		glGetProgramInfoLog(m_genericProg, 1000, &len, log);
		fprintf(stderr, "Shader link error:\n%s\n", log);
	}
	/* VAO */
	glGenVertexArrays(1, &m_vao);

	/* vbos/ibos */
	glGenBuffers(1, &m_vbo);
	glGenBuffers(1, &m_wireibo);
	glGenBuffers(1, &m_solidibo);

	GLubyte wireIndices[24] = { 0, 1, 1, 2, 2, 3, 3, 0, 0, 4, 4, 5, 5, 6, 6, 7, 7, 4, 1, 5, 2, 6, 3, 7 };

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_wireibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLubyte) * 24, wireIndices, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	const GLubyte solidIndices[36] = { 0, 1, 2, 2, 3, 0, 1, 5, 6, 6, 2, 1, 7, 6, 5, 5, 4, 7, 4, 0, 3, 3, 7, 4, 4, 5, 1, 1, 0, 4, 3, 2, 6, 6, 7, 3 };

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_solidibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLubyte) * 36, solidIndices, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

RAS_OpenGLDebugDraw::~RAS_OpenGLDebugDraw()
{
	glDeleteBuffers(1, &m_vbo);
	glDeleteBuffers(1, &m_wireibo);
	glDeleteBuffers(1, &m_solidibo);
	glDeleteVertexArrays(1, &m_vao);
}

void RAS_OpenGLDebugDraw::BindVBO(float *mvp, float color[4], float *vertexes, unsigned int ibo)
{
	glUseProgram(m_genericProg);

	glUniform4f(glGetUniformLocation(m_genericProg, "color"), color[0], color[1], color[2], color[3]);
	glUniformMatrix4fv(glGetUniformLocation(m_genericProg, "ModelViewProjectionMatrix"), 1, false, mvp);

	glBindVertexArray(m_vao);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 24, vertexes, GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 3, 0);
}

void RAS_OpenGLDebugDraw::UnbindVBO()
{
	glBindVertexArray(0);
	glDisableVertexAttribArray(0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	glUseProgram(0);
}

void RAS_OpenGLDebugDraw::Flush(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_DebugDraw *debugDraw)
{
	KX_Camera *cam = KX_GetActiveScene()->GetActiveCamera();
	MT_Matrix4x4 m_cameraMatrix(cam->GetProjectionMatrix() * cam->GetModelviewMatrix());

	rasty->SetFrontFace(true);

	// draw lines
	GPUVertFormat *format = immVertexFormat();
	unsigned int pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);

	immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

	for (const RAS_DebugDraw::Line& line : debugDraw->m_lines) {
		float col[4];
		line.m_color.getValue(col);
		immUniformColor4fv(col);

		immBeginAtMost(GPU_PRIM_LINES, 2);

		float frompos[3];
		line.m_from.getValue(frompos);
		immVertex3fv(pos, frompos);
		float topos[3];
		line.m_to.getValue(topos);
		immVertex3fv(pos, topos);

		immEnd();
	}
	immUnbindProgram();

	// Draw aabbs
	for (const RAS_DebugDraw::Aabb& aabb : debugDraw->m_aabbs) {

		const MT_Matrix3x3& rot = aabb.m_rot;
		const MT_Vector3& pos = aabb.m_pos;
		float mat[16] = {
			rot[0][0], rot[1][0], rot[2][0], 0.0,
			rot[0][1], rot[1][1], rot[2][1], 0.0,
			rot[0][2], rot[1][2], rot[2][2], 0.0,
			pos[0], pos[1], pos[2], 1.0
		};

		const MT_Vector3& min = aabb.m_min;
		const MT_Vector3& max = aabb.m_max;

		GLfloat vertexes[24] = {
			(float)min[0], (float)min[1], (float)min[2],
			(float)max[0], (float)min[1], (float)min[2],
			(float)max[0], (float)max[1], (float)min[2],
			(float)min[0], (float)max[1], (float)min[2],
			(float)min[0], (float)min[1], (float)max[2],
			(float)max[0], (float)min[1], (float)max[2],
			(float)max[0], (float)max[1], (float)max[2],
			(float)min[0], (float)max[1], (float)max[2]
		};

		//rasty->PushMatrix();
		//rasty->MultMatrix(mat);
		
		float c[4];
		aabb.m_color.getValue(c);
		
		float mvp[16];
		MT_Matrix4x4 obmat(mat);
		MT_Matrix4x4 m(m_cameraMatrix * obmat);
		m.getValue(mvp);

		BindVBO(mvp, c, vertexes, m_wireibo);
		glDrawElements(GL_LINES, 24, GL_UNSIGNED_BYTE, 0);
		UnbindVBO();

		//rasty->PopMatrix();
	}

	float mvp[16];
	m_cameraMatrix.getValue(mvp);
	// Draw boxes.	
	for (const RAS_DebugDraw::SolidBox& box : debugDraw->m_solidBoxes) {
		GLfloat vertexes[24];
		int k = 0;
		for (int i = 0; i < 8; i++){
			for (int j = 0; j < 3; j++) {
				vertexes[k] = box.m_vertices[i][j];
				k++;
			}
		}
		float c[4];
		box.m_color.getValue(c);		
		BindVBO(mvp, c, vertexes, m_wireibo);
		glDrawElements(GL_LINES, 24, GL_UNSIGNED_BYTE, 0);
		UnbindVBO();

		rasty->SetFrontFace(false);
		box.m_insideColor.getValue(c);
		BindVBO(mvp, c, vertexes, m_solidibo);
		glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_BYTE, 0);
		UnbindVBO();

		rasty->SetFrontFace(true);
		box.m_outsideColor.getValue(c);
		BindVBO(mvp, c, vertexes, m_solidibo);
		glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_BYTE, 0);
		UnbindVBO();
	}

	rasty->Disable(RAS_Rasterizer::RAS_DEPTH_TEST);
	rasty->DisableForText();

	const unsigned int width = canvas->GetWidth();
	const unsigned int height = canvas->GetHeight();
	GPU_matrix_ortho_set(0, width, 0, height, -100, 100);

	format = immVertexFormat();
	pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

	immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
	for (const RAS_DebugDraw::Box2D& box2d : debugDraw->m_boxes2D) {
		const float xco = box2d.m_pos.x();
		const float yco = height - box2d.m_pos.y();
		const float xsize = box2d.m_size.x();
		const float ysize = box2d.m_size.y();

		immUniformColor4fv(box2d.m_color.getValue());
		immRectf(pos, xco + 1 + xsize, yco + ysize, xco, yco);
	}
	immUnbindProgram();

	DRW_state_reset();

	BLF_size(blf_mono_font, 11, 72);

	BLF_enable(blf_mono_font, BLF_SHADOW);

	static float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	BLF_shadow(blf_mono_font, 1, black);
	BLF_shadow_offset(blf_mono_font, 1, 1);

	for (const RAS_DebugDraw::Text2D& text2d : debugDraw->m_texts2D) {
		std::string text = text2d.m_text;
		const float xco = text2d.m_pos.x();
		const float yco = height - text2d.m_pos.y();
		float col[4];
		text2d.m_color.getValue(col);

		BLF_color4fv(blf_mono_font, col);
		BLF_position(blf_mono_font, xco, yco, 0.0f);
		BLF_draw(blf_mono_font, text.c_str(), text.size());
	}
	BLF_disable(blf_mono_font, BLF_SHADOW);

	rasty->Enable(RAS_Rasterizer::RAS_DEPTH_TEST);
}
