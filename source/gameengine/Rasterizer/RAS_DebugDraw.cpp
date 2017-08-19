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

RAS_DebugDraw::Shape::Shape(const mt::vec4& color)
{
	color.Pack(m_color);
}

RAS_DebugDraw::Line::Line(const mt::vec3& from, const mt::vec3& to, const mt::vec4& color)
	:Shape(color)
{
	from.Pack(m_from);
	to.Pack(m_to);
}

RAS_DebugDraw::Circle::Circle(const mt::vec3& center, const mt::vec3& normal, float radius, int sector, const mt::vec4& color)
	:Shape(color),
	m_radius(radius),
	m_sector(sector)
{
	center.Pack(m_center);
	normal.Pack(m_normal);
}

RAS_DebugDraw::Aabb::Aabb(const mt::vec3& pos, const mt::mat3& rot, const mt::vec3& min, const mt::vec3& max, const mt::vec4& color)
	:Shape(color)
{
	pos.Pack(m_pos);
	rot.Pack(m_rot);
	min.Pack(m_min);
	max.Pack(m_max);
}

RAS_DebugDraw::Box::Box(const std::array<mt::vec3, 8>& vertices, const mt::vec4& color)
	:Shape(color)
{
	for (unsigned short i = 0; i < 8; ++i) {
		vertices[i].Pack(m_vertices[i].data());
	}
}

RAS_DebugDraw::SolidBox::SolidBox(const mt::vec4& insideColor, const mt::vec4& outsideColor, const std::array<mt::vec3, 8>& vertices, const mt::vec4& color)
	:Box(vertices, color)
{
	insideColor.Pack(m_insideColor);
	outsideColor.Pack(m_outsideColor);
}

RAS_DebugDraw::Text2D::Text2D(const std::string& text, const mt::vec2& pos, const mt::vec4& color)
	:Shape(color),
	m_text(text)
{
	pos.Pack(m_pos);
}

RAS_DebugDraw::Box2D::Box2D(const mt::vec2& pos, const mt::vec2& size, const mt::vec4& color)
	:Shape(color)
{
	pos.Pack(m_pos);
	size.Pack(m_size);
}

RAS_DebugDraw::RAS_DebugDraw() = default;
RAS_DebugDraw::~RAS_DebugDraw() = default;

void RAS_DebugDraw::DrawLine(const mt::vec3 &from, const mt::vec3 &to, const mt::vec4 &color)
{
	m_lines.emplace_back(from, to, color);
}

void RAS_DebugDraw::DrawCircle(const mt::vec3 &center, const float radius,
		const mt::vec4 &color, const mt::vec3 &normal, int nsector)
{
	m_circles.emplace_back(center, normal, radius, nsector, color);
}

void RAS_DebugDraw::DrawAabb(const mt::vec3& pos, const mt::mat3& rot,
		const mt::vec3& min, const mt::vec3& max, const mt::vec4& color)
{
	m_aabbs.emplace_back(pos, rot, min, max, color);
}

void RAS_DebugDraw::DrawBox(const std::array<mt::vec3, 8>& vertices, const mt::vec4& color)
{
	m_boxes.emplace_back(vertices, color);
}

void RAS_DebugDraw::DrawSolidBox(const std::array<mt::vec3, 8>& vertices, const mt::vec4& insideColor,
		const mt::vec4& outsideColor, const mt::vec4& lineColor)
{
	m_solidBoxes.emplace_back(insideColor, outsideColor, vertices, lineColor);
}

void RAS_DebugDraw::DrawCameraFrustum(const mt::mat4& projmat, const mt::mat4& viewmat)
{
	std::array<mt::vec3, 8> box;
	mt::FrustumBox((projmat * viewmat).Inverse(), box);

	DrawSolidBox(box, mt::vec4(0.4f, 0.4f, 0.4f, 0.4f), mt::vec4(0.0f, 0.0f, 0.0f, 0.4f),
		mt::vec4(0.8f, 0.5f, 0.0f, 1.0f));
}

void RAS_DebugDraw::RenderBox2D(const mt::vec2& pos, const mt::vec2& size, const mt::vec4& color)
{
	m_boxes2D.emplace_back(pos, size, color);
}

void RAS_DebugDraw::RenderText2D(const std::string& text, const mt::vec2& size, const mt::vec4& color)
{
	m_texts2D.emplace_back(text, size, color);
}

void RAS_DebugDraw::Flush(RAS_Rasterizer *rasty, RAS_ICanvas *canvas)
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
