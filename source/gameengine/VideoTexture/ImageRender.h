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
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright (c) 2007 The Zdeno Ash Miklas
 *
 * This source file is part of VideoTexture library
 *
 * Contributor(s):
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file ImageRender.h
 *  \ingroup bgevideotex
 */

#ifndef __IMAGERENDER_H__
#define __IMAGERENDER_H__


#include "Common.h"

#include "KX_Scene.h"
#include "KX_Camera.h"
#include "DNA_screen_types.h"
#include "RAS_ICanvas.h"
#include "RAS_Rasterizer.h"
#include "RAS_ISync.h"

#include "ImageViewport.h"

class RAS_OffScreen;

/// class for render 3d scene
class ImageRender : public ImageViewport, public mt::SimdClassAllocator
{
public:
	/// constructor
	ImageRender(KX_Scene *scene, KX_Camera *camera, unsigned int width, unsigned int height, unsigned short samples, int hrd);
	ImageRender(KX_Scene *scene, KX_GameObject *observer, KX_GameObject *mirror, RAS_IMaterial * mat, unsigned int width, unsigned int height, unsigned short samples, int hrd);

	/// destructor
	virtual ~ImageRender (void);

	/// get horizon color
	float getHorizon(int idx);
	/// set horizon color
	void setHorizon(float red, float green, float blue, float alpha);

	/// get zenith color
	float getZenith(int idx);
	/// set zenith color
	void setZenith(float red, float green, float blue, float alpha);

	/// get update shadow buffer
	bool getUpdateShadowBuffer();
	/// set update shadow buffer
	void setUpdateShadowBuffer(bool refresh);

	/// Get color off screen bind code.
	int GetColorBindCode() const;

	/// clipping distance
	float getClip (void) { return m_clip; }
	/// set whole buffer use
	void setClip (float clip) { m_clip = clip; }
	/// render status
	bool isDone() { return m_done; }
	/// render frame (public so that it is accessible from python)
	bool Render();
	/// in case fbo is used, method to unbind
	void Unbind();
	/// wait for render to complete
	void WaitSync();

protected:
	/// true if ready to render
	bool m_render;
	/// update shadow buffer?
	bool m_updateShadowBuffer;
	/// is render done already?
	bool m_done;
	/// rendered scene
	KX_Scene * m_scene;
	/// camera for render
	KX_Camera * m_camera;
	/// do we own the camera?
	bool m_owncamera;

	// Number of samples used in FBO.
	int m_samples;

	/// The rendered off screen, can be multisampled.
	std::unique_ptr<RAS_OffScreen> m_offScreen;
	/// The non multisampled off screen used when bliting, can be nullptr.
	std::unique_ptr<RAS_OffScreen> m_blitOffScreen;
	/** The pointer to the final off screen without multisamples, can
	 * be m_offScreen or m_blitOffScreen in case of mutlisamples.
	 */
	RAS_OffScreen *m_finalOffScreen;

	/// object to synchronize render even if no buffer transfer
	RAS_ISync *m_sync;
	/// for mirror operation
	KX_GameObject * m_observer;
	KX_GameObject * m_mirror;
	float m_clip;						// clipping distance
	float m_mirrorHalfWidth;            // mirror width in mirror space
	float m_mirrorHalfHeight;           // mirror height in mirror space
	mt::vec3 m_mirrorPos;              // mirror center position in local space
	mt::vec3 m_mirrorZ;               // mirror Z axis in local space
	mt::vec3 m_mirrorY;               // mirror Y axis in local space
	mt::vec3 m_mirrorX;               // mirror X axis in local space
	/// canvas
	RAS_ICanvas* m_canvas;
	/// rasterizer
	RAS_Rasterizer* m_rasterizer;
	/// engine
	KX_KetsjiEngine* m_engine;

	/// horizon color
	mt::vec4 m_horizon;

	/// zenith color
	mt::vec4 m_zenith;

	/// render 3d scene to image
	virtual void calcImage (unsigned int texId, double ts, bool mipmap, unsigned int format)
	{
		calcViewport(texId, ts, mipmap, format);
	}

	/// render 3d scene to image
	virtual void calcViewport (unsigned int texId, double ts, bool mipmap, unsigned int format);

	void setHorizonFromScene(KX_Scene *scene);
	void setZenithFromScene(KX_Scene *scene);
	void SetWorldSettings(KX_WorldInfo* wi);
};


#endif

