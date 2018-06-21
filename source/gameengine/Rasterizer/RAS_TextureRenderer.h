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
* Contributor(s): Ulysse Martin, Tristan Porteries, Martins Upitis.
*
* ***** END GPL LICENSE BLOCK *****
*/

/** \file RAS_TextureRenderer.h
*  \ingroup bgerast
*/

#ifndef __RAS_TEXTURE_RENDERER_H__
#define __RAS_TEXTURE_RENDERER_H__

#include "RAS_Rasterizer.h"
#include <vector>

struct GPUFrameBuffer;
struct GPURenderBuffer;
struct GPUTexture;
struct Image;

/** \brief This class is used to render something into a material texture (RAS_Texture).
 * The render is made by faces added in the sub classes of RAS_TextureRenderer.
 */
class RAS_TextureRenderer
{
public:
	enum LayerUsage
	{
		LAYER_SHARED = 0,
		LAYER_UNIQUE = 1
	};

protected:
	class Face
	{
	private:
		/// Planar FBO and RBO
		GPUFrameBuffer *m_fbo;
		GPURenderBuffer *m_rb;
		int m_target;

	public:
		Face(int target);
		~Face();

		/// Bind the face frame buffer object.
		void Bind() const;
		/// Recreate and attach frame buffer object and render buffer to the texture.
		void AttachTexture(GPUTexture *tex);
		/// Free and detach frame buffer object and render buffer to the texture.
		void DetachTexture(GPUTexture *tex);
	};

	/** \brief Layer are used to make the texture rendering unique per viewport
	 * in case of rendering depending on the camera view.
	 * They use their own created texture attached to the faces FBO.
	 */
	class Layer
	{
	private:
		std::vector<Face> m_faces;
		GPUTexture *m_gpuTex;
		unsigned int m_bindCode;

	public:
		Layer() = default;
		Layer(const std::vector<int>& attachmentTargets, int textureTarget, Image *ima,
				bool mipmap, bool linear);
		~Layer();

		Layer(const Layer& other) = delete;
		Layer(Layer&& other);

		Layer& operator=(const Layer& other) = delete;
		Layer& operator=(Layer&& other);

		unsigned short GetNumFaces() const;
		unsigned int GetBindCode() const;

		void BindFace(unsigned short index) const;
		void Bind() const;
		void Unbind(bool mipmap) const;
	};

	/// Use mipmapping ?
	bool m_useMipmap;
	/// Use linear filtering ?
	bool m_useLinear;
	/// Share one layer for all the viewports ?
	LayerUsage m_layerUsage;
	/// Layers used for each viewport, only one if m_shareLayer is true.
	std::vector<Layer> m_layers;

public:
	/**
	 * \param mipmap Use texture mipmapping.
	 * \param linear Use linear texture filtering.
	 * \param layerUsage Use only one shared layer for all the viewports or unique.
	 */
	RAS_TextureRenderer(bool mipmap, bool linear, LayerUsage layerUsage);
	virtual ~RAS_TextureRenderer();

	unsigned short GetNumFaces(unsigned short layer) const;
	/// Get a layer texture bind code.
	unsigned int GetBindCode(unsigned short index);

	/// Setup frame buffer for rendering.
	void BeginRender(RAS_Rasterizer *rasty, unsigned short layer);
	/// Reset the frame buffer.
	void EndRender(RAS_Rasterizer *rasty, unsigned short layer);

	/// Setup rasterizer for a face render.
	virtual void BeginRenderFace(RAS_Rasterizer *rasty, unsigned short layer, unsigned short face);
	/// Unset rasterizer setup for the last face.
	virtual void EndRenderFace(RAS_Rasterizer *rasty, unsigned short layer, unsigned short face);

	/** Ensure to have the all layers for the number of viewport, returns the
	 * usage of the layers, shared (only one) or unique (as much as viewport).
	 */
	virtual LayerUsage EnsureLayers(int viewportCount) = 0;

	void ReloadTexture();
};

#endif  // __RAS_TEXTURE_RENDERER_H__
