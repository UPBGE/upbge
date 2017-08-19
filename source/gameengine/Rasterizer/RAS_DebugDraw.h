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

/** \file RAS_DebugDraw.h
 *  \ingroup bgerast
 */

#ifndef __RAS_DEBUG_DRAW_H__
#define __RAS_DEBUG_DRAW_H__

#include "mathfu.h"

#include <string>
#include <vector>
#include <array>
#include <memory>

class RAS_Rasterizer;
class RAS_ICanvas;
class RAS_OpenGLDebugDraw;

class RAS_DebugDraw
{
	friend RAS_OpenGLDebugDraw;

private:
	struct Shape
	{
		Shape(const mt::vec4& color);
		float m_color[4];
	};

	struct Line : Shape
	{
		Line(const mt::vec3& from, const mt::vec3& to, const mt::vec4& color);
		float m_from[3];
		float m_to[3];
	};

	struct Circle : Shape
	{
		Circle(const mt::vec3& center, const mt::vec3& normal, float radius, int sector, const mt::vec4& color);
		float m_center[3];
		float m_normal[3];
		float m_radius;
		int m_sector;
	};

	struct Aabb : Shape
	{
		Aabb(const mt::vec3& pos, const mt::mat3& rot, const mt::vec3& min, const mt::vec3& max, const mt::vec4& color);
		float m_pos[3];
		float m_rot[9];
		float m_min[3];
		float m_max[3];
	};

	struct Box : Shape
	{
		Box(const std::array<mt::vec3, 8>& vertices, const mt::vec4& color);
		std::array<std::array<float, 3>, 8> m_vertices;
	};

	struct SolidBox : Box
	{
		SolidBox(const mt::vec4& insideColor, const mt::vec4& outsideColor, const std::array<mt::vec3, 8>& vertices, const mt::vec4& color);
		float m_insideColor[4];
		float m_outsideColor[4];
	};

	struct Text2D : Shape
	{
		Text2D(const std::string& text, const mt::vec2& pos, const mt::vec4& color);
		std::string m_text;
		float m_pos[2];
	};

	struct Box2D : Shape
	{
		Box2D(const mt::vec2& pos, const mt::vec2& size, const mt::vec4& color);
		float m_pos[2];
		float m_size[2];
	};

	std::vector<Line> m_lines;
	std::vector<Circle> m_circles;
	std::vector<Aabb> m_aabbs;
	std::vector<Box> m_boxes;
	std::vector<SolidBox> m_solidBoxes;
	std::vector<Text2D> m_texts2D;
	std::vector<Box2D> m_boxes2D;

	std::unique_ptr<RAS_OpenGLDebugDraw> m_impl;

public:
	RAS_DebugDraw();
	~RAS_DebugDraw();

	void DrawLine(const mt::vec3 &from, const mt::vec3 &to, const mt::vec4& color);
	void DrawCircle(const mt::vec3 &center, const float radius,
								 const mt::vec4 &color, const mt::vec3 &normal, int nsector);
	/** Draw a box depends on minimal and maximal corner.
	 * \param pos The box's position.
	 * \param rot The box's orientation.
	 * \param min The box's minimal corner.
	 * \param max The box's maximal corner.
	 * \param color The box's color.
	 */
	void DrawAabb(const mt::vec3& pos, const mt::mat3& rot,
							  const mt::vec3& min, const mt::vec3& max, const mt::vec4& color);
	void DrawBox(const std::array<mt::vec3, 8>& vertices, const mt::vec4& color);
	void DrawSolidBox(const std::array<mt::vec3, 8>& vertices, const mt::vec4& insideColor,
							  const mt::vec4& outsideColor, const mt::vec4& lineColor);
	/** Draw a box representing a camera frustum volume.
	 * \param projmat The camera projection matrix.
	 * \param viewmat The camera view matrix.
	 */
	void DrawCameraFrustum(const mt::mat4& projmat, const mt::mat4& viewmat);

	void RenderBox2D(const mt::vec2& pos, const mt::vec2& size, const mt::vec4& color);

	void RenderText2D(const std::string& text, const mt::vec2& pos, const mt::vec4& color);

	void Flush(RAS_Rasterizer *rasty, RAS_ICanvas *canvas);
};

#endif  // __RAS_DEBUG_DRAW_H__
