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

/** \file gameengine/Rasterizer/RAS_DebugDraw.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_DebugDraw.h"
#include "RAS_OpenGLDebugDraw.h"

RAS_DebugDraw::Shape::Shape(const MT_Vector4& color)
	:m_color(color)
{
}

RAS_DebugDraw::Line::Line(const MT_Vector3& from, const MT_Vector3& to, const MT_Vector4& color)
	:Shape(color),
	m_from(from),
	m_to(to)
{
}

RAS_DebugDraw::Circle::Circle(const MT_Vector3& center, const MT_Vector3& normal, float radius, int sector, const MT_Vector4& color)
	:Shape(color),
	m_center(center),
	m_normal(normal),
	m_radius(radius),
	m_sector(sector)
{
}

RAS_DebugDraw::Aabb::Aabb(const MT_Vector3& pos, const MT_Matrix3x3& rot, const MT_Vector3& min, const MT_Vector3& max, const MT_Vector4& color)
	:Shape(color),
	m_pos(pos),
	m_rot(rot),
	m_min(min),
	m_max(max)
{
}

RAS_DebugDraw::Box::Box(const std::array<MT_Vector3, 8>& vertices, const MT_Vector4& color)
	:Shape(color),
	m_vertices(vertices)
{
}

RAS_DebugDraw::SolidBox::SolidBox(const MT_Vector4& insideColor, const MT_Vector4& outsideColor, const std::array<MT_Vector3, 8>& vertices, const MT_Vector4& color)
	:Box(vertices, color),
	m_insideColor(insideColor),
	m_outsideColor(outsideColor)
{
}

RAS_DebugDraw::Text2D::Text2D(const std::string& text, const MT_Vector2& pos, const MT_Vector4& color)
	:Shape(color),
	m_text(text),
	m_pos(pos)
{
}

RAS_DebugDraw::Box2D::Box2D(const MT_Vector2& pos, const MT_Vector2& size, const MT_Vector4& color)
	:Shape(color),
	m_pos(pos),
	m_size(size)
{
}

RAS_DebugDraw::RAS_DebugDraw() = default;
RAS_DebugDraw::~RAS_DebugDraw() = default;

void RAS_DebugDraw::DrawLine(const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4 &color)
{
	m_lines.emplace_back(from, to, color);
}

void RAS_DebugDraw::DrawCircle(const MT_Vector3 &center, const MT_Scalar radius,
		const MT_Vector4 &color, const MT_Vector3 &normal, int nsector)
{
	m_circles.emplace_back(center, normal, radius, nsector, color);
}

void RAS_DebugDraw::DrawAabb(const MT_Vector3& pos, const MT_Matrix3x3& rot,
		const MT_Vector3& min, const MT_Vector3& max, const MT_Vector4& color)
{
	m_aabbs.emplace_back(pos, rot, min, max, color);
}

void RAS_DebugDraw::DrawBox(const std::array<MT_Vector3, 8>& vertices, const MT_Vector4& color)
{
	m_boxes.emplace_back(vertices, color);
}

void RAS_DebugDraw::DrawSolidBox(const std::array<MT_Vector3, 8>& vertices, const MT_Vector4& insideColor,
		const MT_Vector4& outsideColor, const MT_Vector4& lineColor)
{
	m_solidBoxes.emplace_back(insideColor, outsideColor, vertices, lineColor);
}

void RAS_DebugDraw::DrawCameraFrustum(const MT_Matrix4x4& projmat, const MT_Matrix4x4& viewmat)
{
	std::array<MT_Vector3, 8> box;

	box[0][0] = box[1][0] = box[4][0] = box[5][0] = -1.0f;
	box[2][0] = box[3][0] = box[6][0] = box[7][0] = 1.0f;
	box[0][1] = box[3][1] = box[4][1] = box[7][1] = -1.0f;
	box[1][1] = box[2][1] = box[5][1] = box[6][1] = 1.0f;
	box[0][2] = box[1][2] = box[2][2] = box[3][2] = -1.0f;
	box[4][2] = box[5][2] = box[6][2] = box[7][2] = 1.0f;

	const MT_Matrix4x4 mv = (projmat * viewmat).inverse();

	for (MT_Vector3& p3 : box) {
		const MT_Vector4 p4 = mv * MT_Vector4(p3.x(), p3.y(), p3.z(), 1.0f);
		p3 = MT_Vector3(p4.x() / p4.w(), p4.y() / p4.w(), p4.z() / p4.w());
	}

	DrawSolidBox(box, MT_Vector4(0.4f, 0.4f, 0.4f, 0.4f), MT_Vector4(0.0f, 0.0f, 0.0f, 0.4f),
		MT_Vector4(0.8f, 0.5f, 0.0f, 1.0f));
}

/*void RAS_DebugDraw::DisableForText()
{
	SetAlphaBlend(GPU_BLEND_ALPHA);
	SetLines(false); // needed for texture fonts otherwise they render as wireframe

	Enable(RAS_CULL_FACE);

	ProcessLighting(false, MT_Transform::Identity());

	m_impl->DisableForText();
}*/

void RAS_DebugDraw::RenderBox2D(const MT_Vector2& pos, const MT_Vector2& size, const MT_Vector4& color)
{
	m_boxes2D.emplace_back(pos, size, color);
}

void RAS_DebugDraw::RenderText2D(const std::string& text, const MT_Vector2& size, const MT_Vector4& color)
{
	m_texts2D.emplace_back(text, size, color);
}

void RAS_DebugDraw::Flush(RAS_IRasterizer *rasty, RAS_ICanvas *canvas)
{
	if ((m_lines.size() + m_circles.size() + m_aabbs.size() + m_boxes.size() + m_solidBoxes.size() + m_texts2D.size() + m_boxes2D.size()) == 0) {
		return;
	}

	m_impl->Flush(rasty, canvas, this);

	m_lines.clear();
	m_circles.clear();
	m_aabbs.clear();
	m_boxes.clear();
	m_solidBoxes.clear();
	m_texts2D.clear();
	m_boxes2D.clear();
}
