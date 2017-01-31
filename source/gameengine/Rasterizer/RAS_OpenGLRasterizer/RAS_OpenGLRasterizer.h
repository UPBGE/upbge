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

#include "MT_CmMatrix4x4.h"
#include <vector>
#include <map>

#include "RAS_MaterialBucket.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_IRasterizer.h"

#include "BLI_utildefines.h"

class RAS_StorageVBO;
class RAS_ICanvas;
class RAS_OpenGLLight;
struct GPUShader;

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

	public:
		ScreenPlane();
		~ScreenPlane();

		void Render();
	};

	struct DebugShape
	{
		MT_Vector4 m_color;
	};

	struct DebugLine : DebugShape
	{
		MT_Vector3 m_from;
		MT_Vector3 m_to;
	};

	struct DebugCircle : DebugShape
	{
		MT_Vector3 m_center;
		MT_Vector3 m_normal;
		float m_radius;
		int m_sector;
	};

	struct DebugAabb : DebugShape
	{
		MT_Vector3 m_pos;
		MT_Matrix3x3 m_rot;
		MT_Vector3 m_min;
		MT_Vector3 m_max;
	};

	struct DebugBox : DebugShape
	{
		MT_Vector3 m_vertexes[8];
	};

	struct DebugSolidBox : DebugBox
	{
		MT_Vector4 m_insideColor;
		MT_Vector4 m_outsideColor;
		bool m_solid;
	};

	struct SceneDebugShape
	{
		std::vector<DebugLine> m_lines;
		std::vector<DebugCircle> m_circles;
		std::vector<DebugAabb> m_aabbs;
		std::vector<DebugBox> m_boxes;
		std::vector<DebugSolidBox> m_solidBoxes;
	};

	/* fogging vars */
	bool m_fogenabled;

	/// Class used to render a screen plane.
	ScreenPlane m_screenPlane;

	// We store each debug shape by scene.
	std::map<SCA_IScene *, SceneDebugShape> m_debugShapes;

	RAS_IRasterizer *m_rasterizer;

public:
	RAS_OpenGLRasterizer(RAS_IRasterizer *rasterizer);
	virtual ~RAS_OpenGLRasterizer();

	unsigned short GetNumLights() const;

	void Enable(RAS_IRasterizer::EnableBit bit);
	void Disable(RAS_IRasterizer::EnableBit bit);
	void EnableLight(unsigned short count);
	void DisableLight(unsigned short count);

	void SetDepthFunc(RAS_IRasterizer::DepthFunc func);
	void SetDepthMask(RAS_IRasterizer::DepthMask depthmask);

	void SetBlendFunc(RAS_IRasterizer::BlendFunc src, RAS_IRasterizer::BlendFunc dst);

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

	void DrawDerivedMesh(RAS_MeshSlot *ms, RAS_IRasterizer::DrawType drawingmode);

	void SetViewport(int x, int y, int width, int height);
	void GetViewport(int *rect);
	void SetScissor(int x, int y, int width, int height);

	void SetFog(short type, float start, float dist, float intensity, float color[3]);
	void EnableFog(bool enable);
	void DisplayFog();

	void SetLines(bool enable);

	void SetSpecularity(float specX, float specY, float specZ, float specval);
	void SetShinyness(float shiny);
	void SetDiffuse(float difX, float difY, float difZ, float diffuse);
	void SetEmissive(float eX, float eY, float eZ, float e);

	void SetAmbient(const MT_Vector3& amb, float factor);

	void SetPolygonOffset(float mult, float add);

	void FlushDebugShapes(SCA_IScene *scene);
	void DrawDebugLine(SCA_IScene *scene, const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4 &color);
	void DrawDebugCircle(SCA_IScene *scene, const MT_Vector3 &center, const MT_Scalar radius,
	                             const MT_Vector4 &color, const MT_Vector3 &normal, int nsector);
	void DrawDebugAabb(SCA_IScene *scene, const MT_Vector3& pos, const MT_Matrix3x3& rot,
							  const MT_Vector3& min, const MT_Vector3& max, const MT_Vector4& color);
	void DrawDebugBox(SCA_IScene *scene, MT_Vector3 vertexes[8], const MT_Vector4& color);
	void DrawDebugSolidBox(SCA_IScene *scene, MT_Vector3 vertexes[8], const MT_Vector4& insideColor,
							  const MT_Vector4& outsideColor, const MT_Vector4& lineColor);
	void DrawDebugCameraFrustum(SCA_IScene *scene, const MT_Matrix4x4& projmat, const MT_Matrix4x4& viewmat);

	void SetFrontFace(bool ccw);

	/**
	 * Render Tools
	 */
	void EnableLights();
	void DisableLights();
	void ProcessLighting(bool uselights, const MT_Transform &viewmat);

	void DisableForText();
	void RenderBox2D(int xco, int yco, int width, int height, float percentage);
	void RenderText3D(int fontid, const std::string& text, int size, int dpi,
	                  const float color[4], const float mat[16], float aspect);
	void RenderText2D(RAS_IRasterizer::RAS_TEXT_RENDER_MODE mode, const std::string& text,
	                  int xco, int yco, int width, int height);

	void PushMatrix();
	void PopMatrix();
	void MultMatrix(const float mat[16]);
	void SetMatrixMode(RAS_IRasterizer::MatrixMode mode);
	void LoadMatrix(const float mat[16]);
	void LoadIdentity();

	void MotionBlur(unsigned short state, float value);

	/**
	 * Prints information about what the hardware supports.
	 */
	void PrintHardwareInfo();

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_OpenGLRasterizer")
#endif
};

#endif  /* __RAS_OPENGLRASTERIZER_H__ */
