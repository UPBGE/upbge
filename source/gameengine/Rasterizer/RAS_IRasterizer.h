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

/** \file RAS_IRasterizer.h
 *  \ingroup bgerast
 */

#ifndef __RAS_IRASTERIZER_H__
#define __RAS_IRASTERIZER_H__

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#include <string>

#include "MT_CmMatrix4x4.h"
#include "MT_Matrix4x4.h"

#ifdef WITH_CXX_GUARDEDALLOC
#include "MEM_guardedalloc.h"
#endif

#include <map>
#include <vector>

class RAS_ICanvas;
class RAS_IPolyMaterial;
class RAS_MeshSlot;
class RAS_DisplayArrayBucket;
class RAS_ILightObject;
class SCA_IScene;
class RAS_IOffScreen;
class RAS_ISync;
class KX_Camera;
class KX_Scene;

/**
 * 3D rendering device context interface. 
 */
class RAS_IRasterizer
{
public:
	enum RAS_TEXT_RENDER_MODE {
		RAS_TEXT_RENDER_NODEF = 0,
		RAS_TEXT_NORMAL,
		RAS_TEXT_PADDED,
		RAS_TEXT_MAX,
	};

	RAS_IRasterizer() {}
	virtual ~RAS_IRasterizer() {}

	/**
	 * Drawing types
	 */
	enum DrawType {
		RAS_WIREFRAME = 0,
		RAS_SOLID,
		RAS_TEXTURED,
		RAS_CUBEMAP,
		RAS_SHADOW,
		RAS_DRAW_MAX,
	};

	/**
	 * Valid SetDepthMask parameters
	 */
	enum DepthMask {
		RAS_DEPTHMASK_ENABLED = 1,
		RAS_DEPTHMASK_DISABLED,
	};

	/**
	 */
	enum {
		RAS_BACKCULL = 16, // GEMAT_BACKCULL
	};

	/**
	 * Stereo mode types
	 */
	enum StereoMode {
		RAS_STEREO_NOSTEREO = 1,
		// WARNING: Not yet supported.
		RAS_STEREO_QUADBUFFERED,
		RAS_STEREO_ABOVEBELOW,
		RAS_STEREO_INTERLACED,
		RAS_STEREO_ANAGLYPH,
		RAS_STEREO_SIDEBYSIDE,
		RAS_STEREO_VINTERLACE,
		RAS_STEREO_3DTVTOPBOTTOM,

		RAS_STEREO_MAXSTEREO
	};

	/**
	 * Texture gen modes.
	 */
	enum TexCoGen {
		RAS_TEXCO_GEN,      /* < GPU will generate texture coordinates */
		RAS_TEXCO_ORCO,     /* < Vertex coordinates (object space) */
		RAS_TEXCO_GLOB,     /* < Vertex coordinates (world space) */
		RAS_TEXCO_UV,       /* < UV coordinates */
		RAS_TEXCO_OBJECT,   /* < Use another object's position as coordinates */
		RAS_TEXCO_LAVECTOR, /* < Light vector as coordinates */
		RAS_TEXCO_VIEW,     /* < View vector as coordinates */
		RAS_TEXCO_STICKY,   /* < Sticky coordinates */
		RAS_TEXCO_WINDOW,   /* < Window coordinates */
		RAS_TEXCO_NORM,     /* < Normal coordinates */
		RAS_TEXTANGENT,     /* < */
		RAS_TEXCO_VCOL,     /* < Vertex Color */
		RAS_TEXCO_DISABLE,  /* < Disable this texture unit (cached) */
	};

	typedef std::vector<TexCoGen> TexCoGenList;
	typedef std::map<unsigned short, unsigned short> AttribLayerList;

	/**
	 * Render pass identifiers for stereo.
	 */
	enum StereoEye {
		RAS_STEREO_LEFTEYE = 0,
		RAS_STEREO_RIGHTEYE,
	};

	/**
	 * Mipmap options
	 */
	enum MipmapOption {
		RAS_MIPMAP_NONE,
		RAS_MIPMAP_NEAREST,
		RAS_MIPMAP_LINEAR,

		RAS_MIPMAP_MAX,  /* Should always be last */
	};

	/**
	 * Override shaders
	 */
	enum OverrideShaderType {
		RAS_OVERRIDE_SHADER_NONE,
		RAS_OVERRIDE_SHADER_BASIC,
		RAS_OVERRIDE_SHADER_BASIC_INSTANCING,
		RAS_OVERRIDE_SHADER_SHADOW_VARIANCE,
		RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING,
	};

	enum ShadowType {
		RAS_SHADOW_NONE,
		RAS_SHADOW_SIMPLE,
		RAS_SHADOW_VARIANCE,
	};

	enum EnableBit {
		RAS_DEPTH_TEST = 0,
		RAS_ALPHA_TEST,
		RAS_SCISSOR_TEST,
		RAS_TEXTURE_2D,
		RAS_TEXTURE_CUBE_MAP,
		RAS_BLEND,
		RAS_COLOR_MATERIAL,
		RAS_CULL_FACE,
		RAS_FOG,
		RAS_LIGHTING,
		RAS_MULTISAMPLE,
		RAS_POLYGON_STIPPLE,
		RAS_POLYGON_OFFSET_FILL,
		RAS_POLYGON_OFFSET_LINE
	};

	enum DepthFunc {
		RAS_NEVER = 0,
		RAS_LEQUAL,
		RAS_LESS,
		RAS_ALWAYS,
		RAS_GEQUAL,
		RAS_GREATER,
		RAS_NOTEQUAL,
		RAS_EQUAL
	};

	enum BlendFunc {
		RAS_ZERO = 0,
		RAS_ONE,
		RAS_SRC_COLOR,
		RAS_ONE_MINUS_SRC_COLOR,
		RAS_DST_COLOR,
		RAS_ONE_MINUS_DST_COLOR,
		RAS_SRC_ALPHA,
		RAS_ONE_MINUS_SRC_ALPHA,
		RAS_DST_ALPHA,
		RAS_ONE_MINUS_DST_ALPHA,
		RAS_SRC_ALPHA_SATURATE
	};

	enum MatrixMode {
		RAS_PROJECTION = 0,
		RAS_MODELVIEW,
		RAS_TEXTURE,
		RAS_MATRIX_MODE_MAX
	};

	enum ClearBit {
		RAS_COLOR_BUFFER_BIT = 0x2,
		RAS_DEPTH_BUFFER_BIT = 0x4,
		RAS_STENCIL_BUFFER_BIT = 0x8
	};

	enum OffScreen {
		RAS_OFFSCREEN_COLOR = 0,
		RAS_OFFSCREEN_DEPTH = 1,

		RAS_OFFSCREEN_RENDER = 0,
		RAS_OFFSCREEN_FILTER0,
		RAS_OFFSCREEN_FILTER1,
		RAS_OFFSCREEN_EYE_LEFT0,
		RAS_OFFSCREEN_EYE_RIGHT0,
		RAS_OFFSCREEN_EYE_LEFT1,
		RAS_OFFSCREEN_EYE_RIGHT1,
		RAS_OFFSCREEN_FINAL,
		RAS_OFFSCREEN_BLIT_DEPTH,
		RAS_OFFSCREEN_MAX,
	};

	enum HdrType {
		RAS_HDR_NONE = 0,
		RAS_HDR_HALF_FLOAT,
		RAS_HDR_FULL_FLOAT
	};

	/** Return the output frame buffer normally used for the input frame buffer
	 * index in case of filters render.
	 * \param index The input frame buffer, can be a non-filter frame buffer.
	 * \return The output filter frame buffer.
	 */
	static unsigned short NextFilterOffScreen(unsigned short index);


	/** Return the output frame buffer normally used for the input frame buffer
	 * index in case of per eye stereo render.
	 * \param index The input eye frame buffer, can NOT be a non-eye frame buffer.
	 * \return The output eye frame buffer.
	 */
	static unsigned short NextEyeOffScreen(unsigned short index);

	/** Return the output frame buffer normally used for the input frame buffer
	 * index in case of simple render.
	 * \param index The input render frame buffer, can NOT be a non-render frame buffer.
	 * \return The output render frame buffer.
	 */
	static unsigned short NextRenderOffScreen(unsigned short index);

	/**
	 * Enable capability
	 * \param bit Enable bit
	 */
	virtual void Enable(EnableBit bit) = 0;

	/**
	 * Disable capability
	 * \param bit Enable bit
	 */
	virtual void Disable(EnableBit bit) = 0;

	/**
	 * Set the value for Depth Buffer comparisons
	 * \param func Depth comparison function
	 */
	virtual void SetDepthFunc(DepthFunc func) = 0;

	/** 
	 * Set the blending equation.
	 * \param src The src value.
	 * \param dst The destination value.
	 */
	virtual void SetBlendFunc(BlendFunc src, BlendFunc dst) = 0;

	/**
	 * Takes a screenshot
	 */
	virtual unsigned int *MakeScreenshot(int x, int y, int width, int height) = 0;

	/**
	 * SetDepthMask enables or disables writing a fragment's depth value
	 * to the Z buffer.
	 */
	virtual void SetDepthMask(DepthMask depthmask) = 0;

	/**
	 * Init initializes the renderer.
	 */
	virtual void Init() = 0;

	/**
	 * Exit cleans up the renderer.
	 */
	virtual void Exit() = 0;

	/**
	 * Draw screen overlay plane with basic uv coordinates.
	 */
	virtual void DrawOverlayPlane() = 0;

	/**
	 * BeginFrame is called at the start of each frame.
	 */
	virtual void BeginFrame(double time) = 0;

	/**
	 * Clears a specified set of buffers
	 * \param clearbit What buffers to clear (separated by bitwise OR)
	 */
	virtual void Clear(int clearbit) = 0;

	/**
	 * Set background color
	 */
	virtual void SetClearColor(float r, float g, float b, float a=1.0f) = 0;

	/**
	 * Set background depth
	 */
	virtual void SetClearDepth(float d) = 0;

	/**
	 * Set background color mask.
	 */
	virtual void SetColorMask(bool r, bool g, bool b, bool a) = 0;
	/**
	 * EndFrame is called at the end of each frame.
	 */
	virtual void EndFrame() = 0;

	/// Update dimensions of all off screens.
	virtual void UpdateOffScreens(RAS_ICanvas *canvas) = 0;
	/// Bind the off screen at the given index.
	virtual void BindOffScreen(unsigned short index) = 0;

	/** Draw off screen without set viewport.
	 * Used to copy the frame buffer object to another.
	 * \param srcindex The input off screen index.
	 * \param dstindex The output off screen index.
	 */
	virtual void DrawOffScreen(unsigned short srcindex, unsigned short dstindex) = 0;

	/** Draw off screen at the given index to screen.
	 * \param canvas The canvas containing the screen viewport.
	 * \param index The off screen index to read from.
	 */
	virtual void DrawOffScreen(RAS_ICanvas *canvas, unsigned short index) = 0;

	/** Draw each stereo off screen to screen.
	 * \param canvas The canvas containing the screen viewport.
	 * \param lefteyeindex The left off screen index.
	 * \param righteyeindex The right off screen index.
	 */
	virtual void DrawStereoOffScreen(RAS_ICanvas *canvas, unsigned short lefteyeindex, unsigned short righteyeindex) = 0;

	/** Bind the off screen texture at the given index and slot.
	 * \param index The off screen index.
	 * \param slot The texture slot to bind the texture.
	 * \param type The texture type: RAS_OFFSCREEN_COLOR or RAS_OFFSCREEN_DEPTH.
	 */
	virtual void BindOffScreenTexture(unsigned short index, unsigned short slot, OffScreen type) = 0;

	/** Unbind the off screen texture at the given index and slot.
	 * \param index The off screen index.
	 * \param type The texture type: RAS_OFFSCREEN_COLOR or RAS_OFFSCREEN_DEPTH.
	 */
	virtual void UnbindOffScreenTexture(unsigned short index, OffScreen type) = 0;

	virtual void MipmapOffScreenTexture(unsigned short index, OffScreen type) = 0;
	virtual void UnmipmapOffScreenTexture(unsigned short index, OffScreen type) = 0;

	/// Return current off screen index.
	virtual short GetCurrentOffScreenIndex() const = 0;
	/// Return the off screenn samples numbers at the given index.
	virtual int GetOffScreenSamples(unsigned short index) = 0;

	/**
	 * SetRenderArea sets the render area from the 2d canvas.
	 * Returns true if only of subset of the canvas is used.
	 */
	virtual void SetRenderArea(RAS_ICanvas *canvas) = 0;

	// Stereo Functions
	/**
	 * SetStereoMode will set the stereo mode
	 */
	virtual void SetStereoMode(const StereoMode stereomode) = 0;

	/**
	 * Stereo can be used to query if the rasterizer is in stereo mode.
	 * \return true if stereo mode is enabled.
	 */
	virtual bool Stereo() = 0;
	virtual StereoMode GetStereoMode() = 0;

	/**
	 * Sets which eye buffer subsequent primitives will be rendered to.
	 */
	virtual void SetEye(const StereoEye eye) = 0;
	virtual StereoEye GetEye() = 0;

	/**
	 * Sets the distance between eyes for stereo mode.
	 */
	virtual void SetEyeSeparation(const float eyeseparation) = 0;
	virtual float GetEyeSeparation() = 0;

	/**
	 * Sets the focal length for stereo mode.
	 */
	virtual void SetFocalLength(const float focallength) = 0;
	virtual float GetFocalLength() = 0;

	/**
	 * Create a sync object
	 * For use with offscreen render
	 */
	virtual RAS_ISync *CreateSync(int type) = 0;

	/**
	 * SwapBuffers swaps the back buffer with the front buffer.
	 */
	virtual void SwapBuffers(RAS_ICanvas *canvas) = 0;
	
	// Drawing Functions
	/// Set all pre render attributs for given display array bucket.
	virtual void BindPrimitives(RAS_DisplayArrayBucket *arrayBucket) = 0;
	/// UnSet all pre render attributs for given display array bucket.
	virtual void UnbindPrimitives(RAS_DisplayArrayBucket *arrayBucket) = 0;
	/**
	 * IndexPrimitives: Renders primitives from mesh slot.
	 */
	virtual void IndexPrimitives(RAS_MeshSlot *ms) = 0;

	/**
	 * Renders all primitives from mesh slots contained in this display array
	 * bucket with the geometry instancing way.
	 */
	virtual void IndexPrimitivesInstancing(RAS_DisplayArrayBucket *arrayBucket) = 0;

	/**
	 * Renders all primitives from mesh slots contained in this display array
	 * bucket with a single batched display array.
	 */
	virtual void IndexPrimitivesBatching(RAS_DisplayArrayBucket *arrayBucket, const std::vector<void *>& indices, const std::vector<int>& counts) = 0;

	/// Render text mesh slot using BLF functions.
	virtual void IndexPrimitivesText(RAS_MeshSlot *ms) = 0;
 
	virtual void SetProjectionMatrix(MT_CmMatrix4x4 &mat) = 0;

	/* This one should become our final version, methinks. */
	/**
	 * Set the projection matrix for the rasterizer. This projects
	 * from camera coordinates to window coordinates.
	 * \param mat The projection matrix.
	 */
	virtual void SetProjectionMatrix(const MT_Matrix4x4 &mat) = 0;

	/**
	 * Sets the modelview matrix.
	 */
	virtual void SetViewMatrix(const MT_Matrix4x4 &mat, const MT_Matrix3x3 &ori,
	                           const MT_Vector3 &pos, const MT_Vector3 &scale, bool perspective) = 0;

	/**
	 * Get/Set viewport area
	 */
	virtual void SetViewport(int x, int y, int width, int height) = 0;
	virtual void GetViewport(int *rect) = 0;

	/**
	 * Set scissor mask
	 */
	virtual void SetScissor(int x, int y, int width, int height) = 0;

	/**
	 */
	virtual const MT_Vector3& GetCameraPosition() = 0;
	virtual bool GetCameraOrtho() = 0;

	/**
	 * Fog
	 */
	virtual void SetFog(short type, float start, float dist, float intensity, float color[3]) = 0;
	virtual void DisplayFog() = 0;
	virtual void EnableFog(bool enable) = 0;
	
	/**
	 * \param drawingmode = RAS_WIREFRAME, RAS_SOLID, RAS_SHADOW or RAS_TEXTURED.
	 */
	virtual void SetDrawingMode(DrawType drawingmode) = 0;

	/**
	 * \return the current drawing mode: RAS_WIREFRAME, RAS_SOLID RAS_SHADOW or RAS_TEXTURED.
	 */
	virtual DrawType GetDrawingMode() = 0;

	/// \param shadowmode = RAS_SHADOW_SIMPLE, RAS_SHADOW_VARIANCE.
	virtual void SetShadowMode(ShadowType shadowmode) = 0;

	/// \return the current drawing mode: RAS_SHADOW_SIMPLE, RAS_SHADOW_VARIANCE.
	virtual ShadowType GetShadowMode() = 0;

	/**
	 * Sets face culling
	 */
	virtual void SetCullFace(bool enable) = 0;

	/**
	 * Sets wireframe mode.
	 */
	virtual void SetLines(bool enable) = 0;

	/**
	 */
	virtual double GetTime() = 0;

	/**
	 * Generates a projection matrix from the specified frustum.
	 * \param left the left clipping plane
	 * \param right the right clipping plane
	 * \param bottom the bottom clipping plane
	 * \param top the top clipping plane
	 * \param frustnear the near clipping plane
	 * \param frustfar the far clipping plane
	 * \return a 4x4 matrix representing the projection transform.
	 */
	virtual MT_Matrix4x4 GetFrustumMatrix(
	        float left, float right, float bottom, float top,
	        float frustnear, float frustfar,
	        float focallength = 0.0f, bool perspective = true) = 0;

	/**
	 * Generates a orthographic projection matrix from the specified frustum.
	 * \param left the left clipping plane
	 * \param right the right clipping plane
	 * \param bottom the bottom clipping plane
	 * \param top the top clipping plane
	 * \param frustnear the near clipping plane
	 * \param frustfar the far clipping plane
	 * \return a 4x4 matrix representing the projection transform.
	 */
	virtual MT_Matrix4x4 GetOrthoMatrix(
	        float left, float right, float bottom, float top,
	        float frustnear, float frustfar) = 0;

	/**
	 * Sets the specular color component of the lighting equation.
	 */
	virtual void SetSpecularity(float specX, float specY, float specZ, float specval) = 0;
	
	/**
	 * Sets the specular exponent component of the lighting equation.
	 */
	virtual void SetShinyness(float shiny) = 0;

	/**
	 * Sets the diffuse color component of the lighting equation.
	 */
	virtual void SetDiffuse(float difX,float difY, float difZ, float diffuse) = 0;

	/**
	 * Sets the emissive color component of the lighting equation.
	 */ 
	virtual void SetEmissive(float eX, float eY, float eZ, float e) = 0;
	
	virtual void SetAmbientColor(float color[3]) = 0;
	virtual void SetAmbient(float factor) = 0;

	/**
	 * Sets a polygon offset.  z depth will be: z1 = mult*z0 + add
	 */
	virtual void	SetPolygonOffset(float mult, float add) = 0;
	
	virtual void DrawDebugLine(SCA_IScene *scene, const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4& color) = 0;
	virtual void DrawDebugCircle(SCA_IScene *scene, const MT_Vector3 &center, const MT_Scalar radius,
								 const MT_Vector4 &color, const MT_Vector3 &normal, int nsector) = 0;
	/** Draw a box depends on minimal and maximal corner.
	 * \param scene The scene owner of this call.
	 * \param pos The box's position.
	 * \param rot The box's orientation.
	 * \param min The box's minimal corner.
	 * \param max The box's maximal corner.
	 * \param color The box's color.
	 */
	virtual void DrawDebugAabb(SCA_IScene *scene, const MT_Vector3& pos, const MT_Matrix3x3& rot,
							  const MT_Vector3& min, const MT_Vector3& max, const MT_Vector4& color) = 0;
	virtual void FlushDebugShapes(SCA_IScene *scene) = 0;

	/// Clear the material texture coordinates list used by storages.
	virtual void ClearTexCoords() = 0;
	/// Clear the material attributes list used by storages.
	virtual void ClearAttribs() = 0;
	/// Clear the material attribut layers list used with material attributes by storages.
	virtual void ClearAttribLayers() = 0;
	/// Set the material texture coordinates list used by storages.
	virtual void SetTexCoords(const TexCoGenList& texcos) = 0;
	/// Set the material attributes list used by storages.
	virtual void SetAttribs(const TexCoGenList& attribs) = 0;
	/// Set the material attribut layers used with material attributes by storages.
	virtual void SetAttribLayers(const RAS_IRasterizer::AttribLayerList& layers) = 0;

	virtual const MT_Matrix4x4 &GetViewMatrix() const = 0;
	virtual const MT_Matrix4x4 &GetViewInvMatrix() const = 0;

	virtual void EnableMotionBlur(float motionblurvalue) = 0;
	virtual void DisableMotionBlur() = 0;
	
	virtual float GetMotionBlurValue() = 0;
	virtual int GetMotionBlurState() = 0;
	virtual void SetMotionBlurState(int newstate) = 0;

	virtual void SetAlphaBlend(int alphablend) = 0;
	virtual void SetFrontFace(bool ccw) = 0;

	virtual void SetAnisotropicFiltering(short level) = 0;
	virtual short GetAnisotropicFiltering() = 0;

	virtual void SetMipmapping(MipmapOption val) = 0;
	virtual MipmapOption GetMipmapping() = 0;

	virtual void SetOverrideShader(OverrideShaderType type) = 0;
	virtual OverrideShaderType GetOverrideShader() = 0;
	virtual void ActivateOverrideShaderInstancing(void *matrixoffset, void *positionoffset, unsigned int stride) = 0;
	virtual void DesactivateOverrideShaderInstancing() = 0;

	/**
	 * Render Tools
	 */
	virtual void GetTransform(float *origmat, int objectdrawmode, float mat[16]) = 0;

	/**
	 * Renders 2D boxes.
	 * \param xco			Position on the screen (origin in lower left corner).
	 * \param yco			Position on the screen (origin in lower left corner).
	 * \param width			Width of the canvas to draw to.
	 * \param height		Height of the canvas to draw to.
	 * \param percentage	Percentage of bar.
	 */
	virtual void RenderBox2D(int xco, int yco, int width, int height, float percentage) = 0;

	/**
	 * Renders 3D text string using BFL.
	 * \param fontid	The id of the font.
	 * \param text		The string to render.
	 * \param size		The size of the text.
	 * \param dpi		The resolution of the text.
	 * \param color		The color of the object.
	 * \param mat		The Matrix of the text object.
	 * \param aspect	A scaling factor to compensate for the size.
	 */
	virtual void RenderText3D(
	        int fontid, const std::string& text, int size, int dpi,
	        const float color[4], const float mat[16], float aspect) = 0;

	/**
	 * Renders 2D text string.
	 * \param mode      The type of text
	 * \param text		The string to render.
	 * \param xco		Position on the screen (origin in lower left corner).
	 * \param yco		Position on the screen (origin in lower left corner).
	 * \param width		Width of the canvas to draw to.
	 * \param height	Height of the canvas to draw to.
	 */
	virtual void RenderText2D(
	        RAS_TEXT_RENDER_MODE mode, const std::string& text,
	        int xco, int yco, int width, int height) = 0;

	virtual void ProcessLighting(bool uselights, const MT_Transform &trans) = 0;

	virtual void PushMatrix() = 0;
	virtual void PopMatrix() = 0;
	virtual void MultMatrix(const float mat[16]) = 0;
	virtual void SetMatrixMode(MatrixMode mode) = 0;
	virtual void LoadMatrix(const float mat[16]) = 0;
	virtual void LoadIdentity() = 0;

	virtual RAS_ILightObject *CreateLight() = 0;

	virtual void AddLight(RAS_ILightObject *lightobject) = 0;

	virtual void RemoveLight(RAS_ILightObject *lightobject) = 0;

	/** Set the current off screen depth to the global depth texture used by materials.
	 * In case of mutlisample off screen a blit to RAS_OFFSCREEN_BLIT_DEPTH is procceed.
	 */
	virtual void UpdateGlobalDepthTexture() = 0;
	/// Set the global depth texture to an empty texture.
	virtual void ResetGlobalDepthTexture() = 0;

	virtual void MotionBlur() = 0;

	virtual void SetClientObject(void *obj) = 0;

	virtual void SetAuxilaryClientInfo(void *inf) = 0;

	/**
	 * Prints information about what the hardware supports.
	 */
	virtual void PrintHardwareInfo() = 0;

	virtual void DrawCameraFrustum(KX_Camera *cam, KX_Scene *scene) = 0;
	virtual void DrawDebugTransparentBoxes(MT_Vector3 box[8]) = 0;

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_IRasterizer")
#endif
};

#endif  /* __RAS_IRASTERIZER_H__ */
