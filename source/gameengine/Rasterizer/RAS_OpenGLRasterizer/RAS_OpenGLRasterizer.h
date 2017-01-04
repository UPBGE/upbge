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

#include "RAS_IRasterizer.h"
#include "RAS_MaterialBucket.h"
#include "RAS_IPolygonMaterial.h"

#include "BLI_utildefines.h"

class RAS_StorageVBO;
class RAS_ICanvas;
class RAS_OpenGLLight;
struct GPUOffScreen;
struct GPUTexture;
struct GPUShader;

/**
 * 3D rendering device context.
 */
class RAS_OpenGLRasterizer : public RAS_IRasterizer
{
public:
	struct StorageAttribs
	{
		TexCoGenList attribs;
		TexCoGenList texcos;
		AttribLayerList layers;
	};

private:
	class ScreenPlane
	{
	private:
		unsigned int m_vbo;
		unsigned int m_ibo;

	public:
		ScreenPlane();
		virtual ~ScreenPlane();

		void Render();
	};

	/// Internal manager of off screens.
	class OffScreens
	{
	private:
		/// All the off screens used.
		GPUOffScreen *m_offScreens[RAS_OFFSCREEN_MAX];
		/// The current off screen index.
		short m_currentIndex;

		/// The last width.
		unsigned int m_width;
		/// The last height.
		unsigned int m_height;
		/// The number of wanted/supported samples.
		int m_samples;
		/// The HDR quality.
		short m_hdr;

		/// Return or create off screen for the given index.
		GPUOffScreen *GetOffScreen(unsigned short index);

	public:
		OffScreens();
		~OffScreens();

		void Update(RAS_ICanvas *canvas);
		void Bind(unsigned short index);
		void RestoreScreen();
		/// NOTE: This function has the side effect to leave the destination off screen bound.
		void Blit(unsigned short srcindex, unsigned short dstindex, bool color, bool depth);
		void BindTexture(unsigned short index, unsigned short slot, OffScreen type);
		void UnbindTexture(unsigned short index, OffScreen type);
		void MipmapTexture(unsigned short index, OffScreen type);
		void UnmipmapTexture(unsigned short index, OffScreen type);
		short GetCurrentIndex() const;
		int GetSamples(unsigned short index);
		GPUTexture *GetDepthTexture(unsigned short index);
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

	struct SceneDebugShape
	{
		std::vector<DebugLine> m_lines;
		std::vector<DebugCircle> m_circles;
		std::vector<DebugAabb> m_aabbs;
	};

	// All info used to compute the ray cast transform matrix.
	struct RayCastTranform
	{
		/// The object scale.
		MT_Vector3 scale;
		/// The original object matrix.
		float *origmat;
		/// The output matrix.
		float *mat;
	};

	/// \section Interfaces used for frame buffer shaders.

	struct OverrideShaderDrawFrameBufferInterface
	{
		int colorTexLoc;
	};

	struct OverrideShaderStereoStippleInterface
	{
		int leftEyeTexLoc;
		int rightEyeTexLoc;
		int stippleIdLoc;
	};

	struct OverrideShaderStereoAnaglyph
	{
		int leftEyeTexLoc;
		int rightEyeTexLoc;
	};

	/* fogging vars */
	bool m_fogenabled;

	float m_ambr;
	float m_ambg;
	float m_ambb;
	double m_time;
	MT_Matrix4x4 m_viewmatrix;
	MT_Matrix4x4 m_viewinvmatrix;
	MT_Vector3 m_campos;
	bool m_camortho;
	bool m_camnegscale;

	StereoMode m_stereomode;
	StereoEye m_curreye;
	float m_eyeseparation;
	float m_focallength;
	bool m_setfocallength;
	int m_noOfScanlines;

	/* motion blur */
	int m_motionblur;
	float m_motionblurvalue;

	/* Render tools */
	void *m_clientobject;
	void *m_auxilaryClientInfo;
	std::vector<RAS_OpenGLLight *> m_lights;
	int m_lastlightlayer;
	bool m_lastlighting;
	void *m_lastauxinfo;
	unsigned int m_numgllights;

	/// Class used to render a screen plane.
	ScreenPlane m_screenPlane;

	// We store each debug shape by scene.
	std::map<SCA_IScene *, SceneDebugShape> m_debugShapes;

	/// Class used to manage off screens.
	OffScreens m_offScreens;

protected:
	DrawType m_drawingmode;
	ShadowType m_shadowMode;

	StorageAttribs m_storageAttribs;

	/* int m_last_alphablend; */
	bool m_last_frontface;

	OverrideShaderType m_overrideShader;

	RAS_StorageVBO *m_storage;

	/// Initialize custom shader interface containing uniform location.
	void InitOverrideShadersInterface();

	/// Return GPUShader coresponding to the override shader enumeration.
	GPUShader *GetOverrideGPUShader(OverrideShaderType type);

public:
	double GetTime();
	RAS_OpenGLRasterizer();
	virtual ~RAS_OpenGLRasterizer();

	virtual void Enable(EnableBit bit);
	virtual void Disable(EnableBit bit);

	virtual void SetDepthFunc(DepthFunc func);
	virtual void SetDepthMask(DepthMask depthmask);

	virtual void SetBlendFunc(BlendFunc src, BlendFunc dst);

	virtual unsigned int *MakeScreenshot(int x, int y, int width, int height);

	virtual void Init();
	virtual void Exit();
	virtual void DrawOverlayPlane();
	virtual void BeginFrame(double time);
	virtual void Clear(int clearbit);
	virtual void SetClearColor(float r, float g, float b, float a=1.0f);
	virtual void SetClearDepth(float d);
	virtual void SetColorMask(bool r, bool g, bool b, bool a);
	virtual void EndFrame();

	virtual void UpdateOffScreens(RAS_ICanvas *canvas);
	virtual void BindOffScreen(unsigned short index);
	virtual void DrawOffScreen(unsigned short srcindex, unsigned short dstindex);
	virtual void DrawOffScreen(RAS_ICanvas *canvas, unsigned short index);
	virtual void DrawStereoOffScreen(RAS_ICanvas *canvas, unsigned short lefteyeindex, unsigned short righteyeindex);
	virtual void BindOffScreenTexture(unsigned short index, unsigned short slot, OffScreen type);
	virtual void UnbindOffScreenTexture(unsigned short index, OffScreen type);
	virtual void MipmapOffScreenTexture(unsigned short index, OffScreen type);
	virtual void UnmipmapOffScreenTexture(unsigned short index, OffScreen type);
	virtual short GetCurrentOffScreenIndex() const;
	virtual int GetOffScreenSamples(unsigned short index);

	virtual void SetRenderArea(RAS_ICanvas *canvas);

	virtual void SetStereoMode(const StereoMode stereomode);
	virtual RAS_IRasterizer::StereoMode GetStereoMode();
	virtual bool Stereo();
	virtual void SetEye(const StereoEye eye);
	virtual StereoEye GetEye();
	virtual void SetEyeSeparation(const float eyeseparation);
	virtual float GetEyeSeparation();
	virtual void SetFocalLength(const float focallength);
	virtual float GetFocalLength();
	virtual RAS_ISync *CreateSync(int type);
	virtual void SwapBuffers(RAS_ICanvas *canvas);

	virtual void BindPrimitives(RAS_DisplayArrayBucket *arrayBucket);
	virtual void UnbindPrimitives(RAS_DisplayArrayBucket *arrayBucket);
	virtual void IndexPrimitives(RAS_MeshSlot *ms);
	virtual void IndexPrimitivesInstancing(RAS_DisplayArrayBucket *arrayBucket);
	virtual void IndexPrimitivesBatching(RAS_DisplayArrayBucket *arrayBucket, const std::vector<void *>& indices, const std::vector<int>& counts);
	virtual void IndexPrimitivesText(RAS_MeshSlot *ms);
	virtual void DrawDerivedMesh(class RAS_MeshSlot *ms);

	virtual void SetProjectionMatrix(MT_CmMatrix4x4 &mat);
	virtual void SetProjectionMatrix(const MT_Matrix4x4 &mat);
	virtual void SetViewMatrix(const MT_Matrix4x4 &mat, const MT_Matrix3x3 &ori, const MT_Vector3 &pos, const MT_Vector3 &scale, bool perspective);

	virtual void SetViewport(int x, int y, int width, int height);
	virtual void GetViewport(int *rect);
	virtual void SetScissor(int x, int y, int width, int height);

	virtual const MT_Vector3& GetCameraPosition();
	virtual bool GetCameraOrtho();

	virtual void SetFog(short type, float start, float dist, float intensity, float color[3]);
	virtual void EnableFog(bool enable);
	virtual void DisplayFog();

	virtual void SetDrawingMode(DrawType drawingmode);
	virtual DrawType GetDrawingMode();

	virtual void SetShadowMode(ShadowType shadowmode);
	virtual ShadowType GetShadowMode();

	virtual void SetCullFace(bool enable);
	virtual void SetLines(bool enable);

	virtual MT_Matrix4x4 GetFrustumMatrix(
	    float left, float right, float bottom, float top,
	    float frustnear, float frustfar,
	    float focallength, bool perspective);
	virtual MT_Matrix4x4 GetOrthoMatrix(
	    float left, float right, float bottom, float top,
	    float frustnear, float frustfar);

	virtual void SetSpecularity(float specX, float specY, float specZ, float specval);
	virtual void SetShinyness(float shiny);
	virtual void SetDiffuse(float difX, float difY, float difZ, float diffuse);
	virtual void SetEmissive(float eX, float eY, float eZ, float e);

	virtual void SetAmbientColor(float color[3]);
	virtual void SetAmbient(float factor);

	virtual void SetPolygonOffset(float mult, float add);

	virtual void FlushDebugShapes(SCA_IScene *scene);
	virtual void DrawDebugLine(SCA_IScene *scene, const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4 &color);
	virtual void DrawDebugCircle(SCA_IScene *scene, const MT_Vector3 &center, const MT_Scalar radius,
	                             const MT_Vector4 &color, const MT_Vector3 &normal, int nsector);
	virtual void DrawDebugAabb(SCA_IScene *scene, const MT_Vector3& pos, const MT_Matrix3x3& rot,
							  const MT_Vector3& min, const MT_Vector3& max, const MT_Vector4& color);

	virtual void ClearTexCoords();
	virtual void ClearAttribs();
	virtual void ClearAttribLayers();
	virtual void SetTexCoords(const TexCoGenList& texcos);
	virtual void SetAttribs(const TexCoGenList& attribs);
	virtual void SetAttribLayers(const RAS_IRasterizer::AttribLayerList& layers);

	const MT_Matrix4x4 &GetViewMatrix() const;
	const MT_Matrix4x4 &GetViewInvMatrix() const;

	virtual void EnableMotionBlur(float motionblurvalue);
	virtual void DisableMotionBlur();
	virtual float GetMotionBlurValue()
	{
		return m_motionblurvalue;
	}
	virtual int GetMotionBlurState()
	{
		return m_motionblur;
	}
	virtual void SetMotionBlurState(int newstate)
	{
		if (newstate < 0)
			m_motionblur = 0;
		else if (newstate > 2)
			m_motionblur = 2;
		else
			m_motionblur = newstate;
	}

	virtual void SetAlphaBlend(int alphablend);
	virtual void SetFrontFace(bool ccw);

	virtual void SetAnisotropicFiltering(short level);
	virtual short GetAnisotropicFiltering();

	virtual void SetMipmapping(MipmapOption val);
	virtual MipmapOption GetMipmapping();

	virtual void SetOverrideShader(OverrideShaderType type);
	virtual OverrideShaderType GetOverrideShader();
	virtual void ActivateOverrideShaderInstancing(void *matrixoffset, void *positionoffset, unsigned int stride);
	virtual void DesactivateOverrideShaderInstancing();

	/**
	 * Render Tools
	 */
	void EnableOpenGLLights();
	void DisableOpenGLLights();
	void ProcessLighting(bool uselights, const MT_Transform &viewmat);

	void DisableForText();
	void RenderBox2D(int xco, int yco, int width, int height, float percentage);
	void RenderText3D(int fontid, const std::string& text, int size, int dpi,
	                  const float color[4], const float mat[16], float aspect);
	void RenderText2D(RAS_TEXT_RENDER_MODE mode, const std::string& text,
	                  int xco, int yco, int width, int height);

	virtual void GetTransform(float *origmat, int objectdrawmode, float mat[16]);

	virtual void PushMatrix();
	virtual void PopMatrix();
	virtual void MultMatrix(const float mat[16]);
	virtual void SetMatrixMode(RAS_IRasterizer::MatrixMode mode);
	virtual void LoadMatrix(const float mat[16]);
	virtual void LoadIdentity();

	/// \see KX_RayCast
	bool RayHit(struct KX_ClientObjectInfo *client, class KX_RayCast *result, RayCastTranform *raytransform);
	/// \see KX_RayCast
	bool NeedRayCast(struct KX_ClientObjectInfo *, void *UNUSED(data))
	{
		return true;
	}

	RAS_ILightObject *CreateLight();
	void AddLight(RAS_ILightObject *lightobject);

	void RemoveLight(RAS_ILightObject *lightobject);
	int ApplyLights(int objectlayer, const MT_Transform& viewmat);

	virtual void UpdateGlobalDepthTexture();
	virtual void ResetGlobalDepthTexture();

	void MotionBlur();

	void SetClientObject(void *obj);

	void SetAuxilaryClientInfo(void *inf);

	/**
	 * Prints information about what the hardware supports.
	 */
	virtual void PrintHardwareInfo();

	// Draw Camera Frustum functions
	virtual void DrawPerspectiveCameraFrustum(MT_Transform trans, float clipstart, float clipend,
		float ratiox, float ratioy, float oppositeclipsta, float oppositeclipend);
	virtual void DrawOrthographicCameraFrustum(MT_Transform trans, float clipstart, float clipend,
		float ratiox, float ratioy, float x);

	// Draw Transparent Debug Boxes functions
	virtual void DrawDebugTransparentBoxes(MT_Vector3 box[8]);
	void DrawDebugBoxFromBox(MT_Vector3 box[8], bool solid);


#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_OpenGLRasterizer")
#endif
};

#endif  /* __RAS_OPENGLRASTERIZER_H__ */
