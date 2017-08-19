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

extern "C" {
#  include "BLF_api.h"
}

void RAS_OpenGLDebugDraw::Flush(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_DebugDraw *debugDraw)
{
	rasty->SetFrontFace(true);
	rasty->SetAlphaBlend(GPU_BLEND_ALPHA);
	rasty->DisableLights();

	// draw lines
	glBegin(GL_LINES);
	for (const RAS_DebugDraw::Line& line : debugDraw->m_lines) {
		glColor4fv(line.m_color);
		glVertex3fv(line.m_from);
		glVertex3fv(line.m_to);
	}
	glEnd();

	glEnableClientState(GL_VERTEX_ARRAY);
	// Draw aabbs
	for (const RAS_DebugDraw::Aabb& aabb : debugDraw->m_aabbs) {
		glColor4fv(aabb.m_color);

		const float *rot = aabb.m_rot;
		const float *pos = aabb.m_pos;
		float mat[16] = {
			rot[0], rot[1], rot[2], 0.0f,
			rot[3], rot[4], rot[5], 0.0f,
			rot[6], rot[7], rot[8], 0.0f,
			pos[0], pos[1], pos[2], 1.0f
		};
		rasty->PushMatrix();
		rasty->MultMatrix(mat);

		const float *min = aabb.m_min;
		const float *max = aabb.m_max;

		float vertexes[24] = {
			min[0], min[1], min[2],
			max[0], min[1], min[2],
			max[0], max[1], min[2],
			min[0], max[1], min[2],
			min[0], min[1], max[2],
			max[0], min[1], max[2],
			max[0], max[1], max[2],
			min[0], max[1], max[2]
		};

		static unsigned short indexes[24] = {
			0, 1, 1, 2,
			2, 3, 3, 0,
			4, 5, 5, 6,
			6, 7, 7, 4,
			0, 4, 1, 5,
			2, 6, 3, 7
		};

		glVertexPointer(3, GL_FLOAT, 0, vertexes);
		glDrawRangeElements(GL_LINES, 0, 7, 24, GL_UNSIGNED_SHORT, indexes);

		rasty->PopMatrix();
	}

	// Draw boxes.
	static const GLubyte wireIndices[24] = {0, 1, 1, 2, 2, 3, 3, 0, 0, 4, 4, 5, 5, 6, 6, 7, 7, 4, 1, 5, 2, 6, 3, 7};
	for (const RAS_DebugDraw::Box& box : debugDraw->m_boxes) {
		glVertexPointer(3, GL_FLOAT, sizeof(float) * 3, box.m_vertices.data());
		glColor4fv(box.m_color);
		glDrawRangeElements(GL_LINES, 0, 7, 24, GL_UNSIGNED_BYTE, wireIndices);
	}

	static const GLubyte solidIndices[24] = {0, 1, 2, 3, 7, 6, 5, 4, 4, 5, 1, 0, 3, 2, 6, 7, 3, 7, 4, 0, 1, 5, 6, 2};
	for (const RAS_DebugDraw::SolidBox& box : debugDraw->m_solidBoxes) {
		glVertexPointer(3, GL_FLOAT, sizeof(float) * 3, box.m_vertices.data());
		glColor4fv(box.m_color);
		glDrawRangeElements(GL_LINES, 0, 7, 24, GL_UNSIGNED_BYTE, wireIndices);

		rasty->SetFrontFace(false);
		glColor4fv(box.m_insideColor);
		glDrawRangeElements(GL_QUADS, 0, 7, 24, GL_UNSIGNED_BYTE, solidIndices);

		rasty->SetFrontFace(true);
		glColor4fv(box.m_outsideColor);
		glDrawRangeElements(GL_QUADS, 0, 7, 24, GL_UNSIGNED_BYTE, solidIndices);
	}
	glDisableClientState(GL_VERTEX_ARRAY);

	// draw circles
	for (const RAS_DebugDraw::Circle& circle : debugDraw->m_circles) {
		glBegin(GL_LINE_LOOP);
		glColor4fv(circle.m_color);

		static const mt::vec3 worldUp = mt::zero3;
		const mt::vec3 norm(circle.m_normal);
		mt::mat3 tr;
		if (mt::FuzzyZero(norm) || norm == worldUp) {
			tr = mt::mat3::Identity();
		}
		else {
			const mt::vec3 xaxis = mt::cross(norm, worldUp);
			const mt::vec3 yaxis = mt::cross(xaxis, norm);
			tr = mt::mat3(xaxis.x, xaxis.y, xaxis.z,
						yaxis.x, yaxis.y, yaxis.z,
						norm.x, norm.y, norm.z);
		}
		const float rad = circle.m_radius;
		const int n = circle.m_sector;
		for (int j = 0; j < n; ++j) {
			const float theta = j * (M_PI * 2) / n;
			mt::vec3 pos(cosf(theta) * rad, sinf(theta) * rad, 0.0f);
			pos = pos * tr;
			pos += mt::vec3(circle.m_center);
			glVertex3fv(pos.Data());
		}
		glEnd();
	}

	rasty->Disable(RAS_Rasterizer::RAS_DEPTH_TEST);
	rasty->DisableForText();

	rasty->PushMatrix();
	rasty->LoadIdentity();

	rasty->SetMatrixMode(RAS_Rasterizer::RAS_PROJECTION);
	rasty->PushMatrix();
	rasty->LoadIdentity();

	const unsigned int width = canvas->GetWidth();
	const unsigned int height = canvas->GetHeight();
	glOrtho(0, width, 0, height, -100, 100);

	glBegin(GL_QUADS);
	for (const RAS_DebugDraw::Box2D& box2d : debugDraw->m_boxes2D) {
		const float xco = box2d.m_pos[0];
		const float yco = height - box2d.m_pos[1];
		const float xsize = box2d.m_size[0];
		const float ysize = box2d.m_size[1];

		glColor4fv(box2d.m_color);
		glVertex2f(xco + 1 + xsize, yco + ysize);
		glVertex2f(xco, yco + ysize);
		glVertex2f(xco, yco);
		glVertex2f(xco + 1 + xsize, yco);
	}
	glEnd();

	BLF_size(blf_mono_font, 11, 72);

	BLF_enable(blf_mono_font, BLF_SHADOW);
	static float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	BLF_shadow(blf_mono_font, 1, black);
	BLF_shadow_offset(blf_mono_font, 1, 1);

	for (const RAS_DebugDraw::Text2D& text2d : debugDraw->m_texts2D) {
		const std::string& text = text2d.m_text;
		const float xco = text2d.m_pos[0];
		const float yco = height - text2d.m_pos[1];

		glColor4fv(text2d.m_color);
		BLF_position(blf_mono_font, xco, yco, 0.0f);
		BLF_draw(blf_mono_font, text.c_str(), text.size());
	}
	BLF_disable(blf_mono_font, BLF_SHADOW);

	rasty->PopMatrix();
	rasty->SetMatrixMode(RAS_Rasterizer::RAS_MODELVIEW);

	rasty->PopMatrix();
}
