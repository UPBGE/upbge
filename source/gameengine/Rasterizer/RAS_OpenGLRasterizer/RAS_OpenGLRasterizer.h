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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_OpenGLRasterizer.h
 *  \ingroup bgerastogl
 */

#ifndef __RAS_OPENGLRASTERIZER_H__
#define __RAS_OPENGLRASTERIZER_H__

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#include "RAS_Rasterizer.h"

/**
 * 3D rendering device context.
 */
class RAS_OpenGLRasterizer
{
private:
	class ScreenPlane
	{
	private:
		unsigned int m_vbo;
		unsigned int m_ibo;
		unsigned int m_vao;

	public:
		ScreenPlane();
		~ScreenPlane();

		void Render();
	};

	/// Class used to render a screen plane.
	ScreenPlane m_screenPlane;

	RAS_Rasterizer *m_rasterizer;

public:
	RAS_OpenGLRasterizer(RAS_Rasterizer *rasterizer);
	virtual ~RAS_OpenGLRasterizer();

	unsigned short GetNumLights() const;

	void Enable(RAS_Rasterizer::EnableBit bit);
	void Disable(RAS_Rasterizer::EnableBit bit);
	void EnableLight(unsigned short count);
	void DisableLight(unsigned short count);

	void SetDepthFunc(RAS_Rasterizer::DepthFunc func);
	void SetDepthMask(RAS_Rasterizer::DepthMask depthmask);

	void SetBlendFunc(RAS_Rasterizer::BlendFunc src, RAS_Rasterizer::BlendFunc dst);

	unsigned int *MakeScreenshot(int x, int y, int width, int height);

	void Init();
	void Exit();
	void DrawOverlayPlane();
	void BeginFrame();
	void Clear(int clearbit);
	void SetClearColor(float r, float g, float b, float a=1.0f);
	void SetClearDepth(float d);
	void SetColorMask(bool r, bool g, bool b, bool a);
	void EndFrame();

	void SetViewport(int x, int y, int width, int height);
	void GetViewport(int *rect);
	void SetScissor(int x, int y, int width, int height);

	void SetFog(short type, float start, float dist, float intensity, const mt::vec3& color);

	void SetLines(bool enable);

	void SetSpecularity(float specX, float specY, float specZ, float specval);
	void SetShinyness(float shiny);
	void SetDiffuse(float difX, float difY, float difZ, float diffuse);
	void SetEmissive(float eX, float eY, float eZ, float e);

	void SetAmbient(const mt::vec3& amb, float factor);

	void SetPolygonOffset(float mult, float add);

	void EnableClipPlane(unsigned short index, const mt::vec4& plane);
	void DisableClipPlane(unsigned short index);

	void SetFrontFace(bool ccw);

	/**
	 * Render Tools
	 */
	void EnableLights();
	void DisableLights();
	void ProcessLighting(bool uselights, const mt::mat3x4 &viewmat);

	void DisableForText();
	void RenderText3D(int fontid, const std::string& text, int size, int dpi,
	                  const float color[4], const float mat[16], float aspect);

	void PushMatrix();
	void PopMatrix();
	void MultMatrix(const float mat[16]);
	void SetMatrixMode(RAS_Rasterizer::MatrixMode mode);
	void LoadMatrix(const float mat[16]);
	void LoadIdentity();

	void MotionBlur(unsigned short state, float value);

	/**
	 * Prints information about what the hardware supports.
	 */
	void PrintHardwareInfo();

  const unsigned char *GetGraphicsCardVendor();
};

#endif  /* __RAS_OPENGLRASTERIZER_H__ */
