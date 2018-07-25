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
#include "RAS_Rasterizer.h"

RAS_DebugDraw::Shape::Shape(const mt::vec4& color)
{
	color.Pack(m_color);
}

RAS_DebugDraw::Line::Line(const mt::vec3& from, const mt::vec3& to, const mt::vec4& color)
{
	color.Pack(m_color);
	color.Pack(m_color2);
	from.Pack(m_from);
	to.Pack(m_to);
}

RAS_DebugDraw::Aabb::Aabb(const mt::vec3& pos, const mt::mat3& rot, const mt::vec3& min, const mt::vec3& max, const mt::vec4& color)
	:Shape(color)
{
	const mt::vec3 diag = (max - min) * 0.5f;
	const mt::vec3 center = (min + max) * 0.5f;

	const mt::mat3x4 trans(rot, rot *center + pos, diag);
	trans.PackFromAffineTransform(m_mat);
}

RAS_DebugDraw::Frustum::Frustum(const mt::mat4& persmat, const mt::vec4& insideColor, const mt::vec4& outsideColor,
                                const mt::vec4& wireColor)
{
	persmat.Pack(m_persMat);
	insideColor.Pack(m_insideColor);
	outsideColor.Pack(m_outsideColor);
	wireColor.Pack(m_wireColor);
}

RAS_DebugDraw::Text2d::Text2d(const std::string& text, const mt::vec2& pos, const mt::vec4& color)
	:Shape(color),
	m_text(text)
{
	pos.Pack(m_pos);
}

RAS_DebugDraw::Box2d::Box2d(const mt::vec2& pos, const mt::vec2& size, const mt::vec4& color)
	:Shape(color)
{
	pos.Pack(m_pos);

	m_size[0] = size.x + 1;
	m_size[1] = -size.y;
}

RAS_DebugDraw::RAS_DebugDraw()
{
}

RAS_DebugDraw::~RAS_DebugDraw() = default;

void RAS_DebugDraw::DrawLine(const mt::vec3 &from, const mt::vec3 &to, const mt::vec4 &color)
{
	m_lines.emplace_back(from, to, color);
}

void RAS_DebugDraw::DrawAabb(const mt::vec3& pos, const mt::mat3& rot,
                             const mt::vec3& min, const mt::vec3& max, const mt::vec4& color)
{
	m_aabbs.emplace_back(pos, rot, min, max, color);
}

void RAS_DebugDraw::DrawCameraFrustum(const mt::mat4& persmat)
{
	m_frustums.emplace_back(persmat.Inverse(), mt::vec4(0.4f, 0.4f, 0.4f, 0.4f), mt::vec4(0.0f, 0.0f, 0.0f, 0.4f),
	                        mt::vec4(0.8f, 0.5f, 0.0f, 1.0f));
}

void RAS_DebugDraw::RenderBox2d(const mt::vec2& pos, const mt::vec2& size, const mt::vec4& color)
{
	m_boxes2d.emplace_back(pos, size, color);
}

void RAS_DebugDraw::RenderText2d(const std::string& text, const mt::vec2& size, const mt::vec4& color)
{
	m_texts2d.emplace_back(text, size, color);
}

void RAS_DebugDraw::Flush(RAS_Rasterizer *rasty, RAS_ICanvas *canvas)
{
	if ((m_lines.size() + m_aabbs.size() + m_frustums.size() + m_texts2d.size() + m_boxes2d.size()) == 0) {
		return;
	}

	rasty->FlushDebug(canvas, this);

	m_lines.clear();
	m_aabbs.clear();
	m_frustums.clear();
	m_texts2d.clear();
	m_boxes2d.clear();
}
