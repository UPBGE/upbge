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

/** \file gameengine/VideoTexture/ImageRender.cpp
 *  \ingroup bgevideotex
 */

// implementation

#include "EXP_PyObjectPlus.h"
#include <structmember.h>
#include <float.h>
#include <math.h>

#include "GPU_framebuffer.h"
#include "GPU_texture.h"

#include "GPU_glew.h"

#include "KX_Globals.h"
#include "KX_Mesh.h"
#include "DNA_scene_types.h"
#include "RAS_OffScreen.h"
#include "RAS_CameraData.h"
#include "RAS_MaterialBucket.h"
#include "RAS_DisplayArray.h"
#include "RAS_ISync.h"
#include "BLI_math.h"

#include "ImageRender.h"
#include "ImageBase.h"
#include "Exception.h"
#include "Texture.h"

ExceptionID SceneInvalid, CameraInvalid, ObserverInvalid, OffScreenInvalid;
ExceptionID MirrorInvalid, MirrorSizeInvalid, MirrorNormalInvalid, MirrorHorizontal, MirrorTooSmall;
ExpDesc SceneInvalidDesc(SceneInvalid, "Scene object is invalid");
ExpDesc CameraInvalidDesc(CameraInvalid, "Camera object is invalid");
ExpDesc ObserverInvalidDesc(ObserverInvalid, "Observer object is invalid");
ExpDesc OffScreenInvalidDesc(OffScreenInvalid, "Offscreen object is invalid");
ExpDesc MirrorInvalidDesc(MirrorInvalid, "Mirror object is invalid");
ExpDesc MirrorSizeInvalidDesc(MirrorSizeInvalid, "Mirror has no vertex or no size");
ExpDesc MirrorNormalInvalidDesc(MirrorNormalInvalid, "Cannot determine mirror plane");
ExpDesc MirrorHorizontalDesc(MirrorHorizontal, "Mirror is horizontal in local space");
ExpDesc MirrorTooSmallDesc(MirrorTooSmall, "Mirror is too small");

// constructor
ImageRender::ImageRender(KX_Scene *scene, KX_Camera *camera, unsigned int width, unsigned int height, unsigned short samples, int hdr) :
	ImageViewport(width, height),
	m_render(true),
	m_updateShadowBuffer(false),
	m_done(false),
	m_scene(scene),
	m_camera(camera),
	m_owncamera(false),
	m_samples(samples),
	m_finalOffScreen(nullptr),
	m_sync(nullptr),
	m_observer(nullptr),
	m_mirror(nullptr),
	m_clip(100.f),
	m_mirrorHalfWidth(0.f),
	m_mirrorHalfHeight(0.f)
{
	// initialize background color to scene background color as default
	setHorizonFromScene(m_scene);
	setZenithFromScene(m_scene);
	// retrieve rendering objects
	m_engine = KX_GetActiveEngine();
	m_rasterizer = m_engine->GetRasterizer();
	m_canvas = m_engine->GetCanvas();

	GPUHDRType type;
	if (hdr == RAS_Rasterizer::RAS_HDR_HALF_FLOAT) {
		type = GPU_HDR_HALF_FLOAT;
		m_internalFormat = GL_RGBA16F_ARB;
	}
	else if (hdr == RAS_Rasterizer::RAS_HDR_FULL_FLOAT) {
		type = GPU_HDR_FULL_FLOAT;
		m_internalFormat = GL_RGBA32F_ARB;
	}
	else {
		type = GPU_HDR_NONE;
		m_internalFormat = GL_RGBA8;
	}

	m_offScreen.reset(new RAS_OffScreen(m_width, m_height, m_samples, type, GPU_OFFSCREEN_RENDERBUFFER_DEPTH, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_CUSTOM));
	if (m_samples > 0) {
		m_blitOffScreen.reset(new RAS_OffScreen(m_width, m_height, 0, type, GPU_OFFSCREEN_RENDERBUFFER_DEPTH, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_CUSTOM));
		m_finalOffScreen = m_blitOffScreen.get();
	}
	else {
		m_finalOffScreen = m_offScreen.get();
	}
}

// destructor
ImageRender::~ImageRender(void)
{
	if (m_owncamera) {
		delete m_camera;
	}

#ifdef WITH_GAMEENGINE_GPU_SYNC
	if (m_sync) {
		delete m_sync;
	}
#endif
}

int ImageRender::GetColorBindCode() const
{
	return m_finalOffScreen->GetColorBindCode();
}

// get update shadow buffer
bool ImageRender::getUpdateShadowBuffer()
{
	return m_updateShadowBuffer;
}

// set update shadow buffer
void ImageRender::setUpdateShadowBuffer(bool refresh)
{
	m_updateShadowBuffer = refresh;
}

// get horizon color
float ImageRender::getHorizon(int idx)
{
	return (idx < 0 || idx > 3) ? 0.0f : m_horizon[idx];
}

// set horizon color
void ImageRender::setHorizon(float red, float green, float blue, float alpha)
{
	m_horizon[0] = (red < 0.0f) ? 0.0f : (red > 1.0f) ? 1.0f : red;
	m_horizon[1] = (green < 0.0f) ? 0.0f : (green > 1.0f) ? 1.0f : green;
	m_horizon[2] = (blue < 0.0f) ? 0.0f : (blue > 1.0f) ? 1.0f : blue;
	m_horizon[3] = (alpha < 0.0f) ? 0.0f : (alpha > 1.0f) ? 1.0f : alpha;
}

// get zenith color
float ImageRender::getZenith(int idx)
{
	return (idx < 0 || idx > 3) ? 0.0f : m_zenith[idx];
}

// set zenith color
void ImageRender::setZenith(float red, float green, float blue, float alpha)
{
	m_zenith[0] = (red < 0.0f) ? 0.0f : (red > 1.0f) ? 1.0f : red;
	m_zenith[1] = (green < 0.0f) ? 0.0f : (green > 1.0f) ? 1.0f : green;
	m_zenith[2] = (blue < 0.0f) ? 0.0f : (blue > 1.0f) ? 1.0f : blue;
	m_zenith[3] = (alpha < 0.0f) ? 0.0f : (alpha > 1.0f) ? 1.0f : alpha;
}

// set horizon color from scene
void ImageRender::setHorizonFromScene(KX_Scene *scene)
{
	if (scene) {
		m_horizon = scene->GetWorldInfo()->m_horizoncolor;
	}
	else {
		m_horizon = mt::vec4(0.0f, 0.0f, 1.0f, 1.0f);
	}
}

// set zenith color from scene
void ImageRender::setZenithFromScene(KX_Scene *scene)
{
	if (scene) {
		m_zenith = scene->GetWorldInfo()->m_zenithcolor;
	}
	else {
		m_zenith = mt::vec4(0.0f, 0.0f, 1.0f, 1.0f);
	}
}

// capture image from viewport
void ImageRender::calcViewport(unsigned int texId, double ts, bool mipmap, unsigned int format)
{
	// render the scene from the camera
	if (!m_done) {
		if (!Render()) {
			return;
		}
	}

	m_finalOffScreen->Bind();

	// wait until all render operations are completed
	WaitSync();
	// get image from viewport (or FBO)
	ImageViewport::calcViewport(texId, ts, mipmap, format);

	RAS_OffScreen::RestoreScreen();
}

bool ImageRender::Render()
{
	RAS_FrameFrustum frustum;

	if (!m_render ||
	    m_rasterizer->GetDrawingMode() != RAS_Rasterizer::RAS_TEXTURED ||   // no need for texture
	    m_camera->GetViewport() ||        // camera must be inactive
	    m_camera == m_scene->GetActiveCamera()) {
		// no need to compute texture in non texture rendering
		return false;
	}

	if (m_updateShadowBuffer) {
		m_engine->RenderShadowBuffers(m_scene);
	}

	if (m_mirror) {
		// mirror mode, compute camera frustum, position and orientation
		// convert mirror position and normal in world space
		const mt::mat3 & mirrorObjWorldOri = m_mirror->GetNode()->GetWorldOrientation();
		const mt::vec3 & mirrorObjWorldPos = m_mirror->GetNode()->GetWorldPosition();
		const mt::vec3 & mirrorObjWorldScale = m_mirror->GetNode()->GetWorldScaling();
		mt::vec3 mirrorWorldPos =
			mirrorObjWorldPos + mirrorObjWorldScale * (mirrorObjWorldOri * m_mirrorPos);
		mt::vec3 mirrorWorldZ = mirrorObjWorldOri * m_mirrorZ;
		// get observer world position
		const mt::vec3 & observerWorldPos = m_observer->GetNode()->GetWorldPosition();
		// get plane D term = mirrorPos . normal
		float mirrorPlaneDTerm = mt::dot(mirrorWorldPos, mirrorWorldZ);
		// compute distance of observer to mirror = D - observerPos . normal
		float observerDistance = mirrorPlaneDTerm - mt::dot(observerWorldPos, mirrorWorldZ);
		// if distance < 0.01 => observer is on wrong side of mirror, don't render
		if (observerDistance < 0.01) {
			return false;
		}
		// set camera world position = observerPos + normal * 2 * distance
		mt::vec3 cameraWorldPos = observerWorldPos + (2.0f * observerDistance) * mirrorWorldZ;
		m_camera->GetNode()->SetLocalPosition(cameraWorldPos);
		// set camera orientation: z=normal, y=mirror_up in world space, x= y x z
		mt::vec3 mirrorWorldY = mirrorObjWorldOri * m_mirrorY;
		mt::vec3 mirrorWorldX = mirrorObjWorldOri * m_mirrorX;
		mt::mat3 cameraWorldOri(
			mirrorWorldX[0], mirrorWorldY[0], mirrorWorldZ[0],
			mirrorWorldX[1], mirrorWorldY[1], mirrorWorldZ[1],
			mirrorWorldX[2], mirrorWorldY[2], mirrorWorldZ[2]);
		m_camera->GetNode()->SetLocalOrientation(cameraWorldOri);
		m_camera->GetNode()->UpdateWorldData();
		// compute camera frustum:
		//   get position of mirror relative to camera: offset = mirrorPos-cameraPos
		mt::vec3 mirrorOffset = mirrorWorldPos - cameraWorldPos;
		//   convert to camera orientation
		mirrorOffset = mirrorOffset * cameraWorldOri;
		//   scale mirror size to world scale:
		//     get closest local axis for mirror Y and X axis and scale height and width by local axis scale
		float x, y;
		x = fabs(m_mirrorY[0]);
		y = fabs(m_mirrorY[1]);
		float height = (x > y) ?
		               ((x > fabs(m_mirrorY[2])) ? mirrorObjWorldScale[0] : mirrorObjWorldScale[2]) :
		               ((y > fabs(m_mirrorY[2])) ? mirrorObjWorldScale[1] : mirrorObjWorldScale[2]);
		x = fabs(m_mirrorX[0]);
		y = fabs(m_mirrorX[1]);
		float width = (x > y) ?
		              ((x > fabs(m_mirrorX[2])) ? mirrorObjWorldScale[0] : mirrorObjWorldScale[2]) :
		              ((y > fabs(m_mirrorX[2])) ? mirrorObjWorldScale[1] : mirrorObjWorldScale[2]);
		width *= m_mirrorHalfWidth;
		height *= m_mirrorHalfHeight;
		//   left = offsetx-width
		//   right = offsetx+width
		//   top = offsety+height
		//   bottom = offsety-height
		//   near = -offsetz
		//   far = near+100
		frustum.x1 = mirrorOffset[0] - width;
		frustum.x2 = mirrorOffset[0] + width;
		frustum.y1 = mirrorOffset[1] - height;
		frustum.y2 = mirrorOffset[1] + height;
		frustum.camnear = -mirrorOffset[2];
		frustum.camfar = -mirrorOffset[2] + m_clip;
	}

	// The screen area that ImageViewport will copy is also the rendering zone
	// bind the fbo and set the viewport to full size
	m_offScreen->Bind();

	m_rasterizer->BeginFrame(m_engine->GetFrameTime());

	m_rasterizer->SetViewport(m_position[0], m_position[1], m_position[0] + m_capSize[0], m_position[1] + m_capSize[1]);
	m_rasterizer->SetScissor(m_position[0], m_position[1], m_position[0] + m_capSize[0], m_position[1] + m_capSize[1]);

	m_rasterizer->Clear(RAS_Rasterizer::RAS_DEPTH_BUFFER_BIT);

	m_scene->GetWorldInfo()->UpdateWorldSettings(m_rasterizer);
	m_rasterizer->SetAuxilaryClientInfo(m_scene);

	if (m_mirror) {
		// frustum was computed above
		// get frustum matrix and set projection matrix
		const mt::mat4 projmat = m_rasterizer->GetFrustumMatrix(frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar);

		m_camera->SetProjectionMatrix(projmat);
	}
	else if (!m_camera->hasValidProjectionMatrix()) {
		float lens = m_camera->GetLens();
		float sensor_x = m_camera->GetSensorWidth();
		float sensor_y = m_camera->GetSensorHeight();
		float shift_x = m_camera->GetShiftHorizontal();
		float shift_y = m_camera->GetShiftVertical();
		bool orthographic = !m_camera->GetCameraData()->m_perspective;
		float nearfrust = m_camera->GetCameraNear();
		float farfrust = m_camera->GetCameraFar();
		float aspect_ratio = 1.0f;
		Scene *blenderScene = m_scene->GetBlenderScene();
		mt::mat4 projmat;

		// compute the aspect ratio from frame blender scene settings so that render to texture
		// works the same in Blender and in Blender player
		if (blenderScene->r.ysch != 0) {
			aspect_ratio = float(blenderScene->r.xsch * blenderScene->r.xasp) / float(blenderScene->r.ysch * blenderScene->r.yasp);
		}

		if (orthographic) {

			RAS_FramingManager::ComputeDefaultOrtho(
				nearfrust,
				farfrust,
				m_camera->GetScale(),
				aspect_ratio,
				m_camera->GetSensorFit(),
				shift_x,
				shift_y,
				frustum
				);

			projmat = m_rasterizer->GetOrthoMatrix(
				frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar);
		}
		else {
			RAS_FramingManager::ComputeDefaultFrustum(
				nearfrust,
				farfrust,
				lens,
				sensor_x,
				sensor_y,
				RAS_SENSORFIT_AUTO,
				shift_x,
				shift_y,
				aspect_ratio,
				frustum);

			projmat = m_rasterizer->GetFrustumMatrix(frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar);
		}
		m_camera->SetProjectionMatrix(projmat);
	}

	m_rasterizer->SetProjectionMatrix(m_camera->GetProjectionMatrix());

	mt::mat3x4 camtrans(m_camera->GetWorldToCamera());
	mt::mat4 viewmat = mt::mat4::FromAffineTransform(camtrans);

	m_rasterizer->SetViewMatrix(viewmat, m_camera->NodeGetWorldScaling());
	m_camera->SetModelviewMatrix(viewmat);

	// Render Background
	if (m_scene->GetWorldInfo()) {
		const mt::vec4 hor = m_scene->GetWorldInfo()->m_horizoncolor;
		const mt::vec4 zen = m_scene->GetWorldInfo()->m_zenithcolor;
		m_scene->GetWorldInfo()->setHorizonColor(m_horizon);
		m_scene->GetWorldInfo()->setZenithColor(m_zenith);
		m_scene->GetWorldInfo()->UpdateBackGround(m_rasterizer);
		m_scene->GetWorldInfo()->RenderBackground(m_rasterizer);
		m_scene->GetWorldInfo()->setHorizonColor(hor);
		m_scene->GetWorldInfo()->setZenithColor(zen);
	}

	const std::vector<KX_GameObject *> objects = m_scene->CalculateVisibleMeshes(m_camera, 0);

	m_engine->UpdateAnimations(m_scene);

	m_scene->RenderBuckets(objects, RAS_Rasterizer::RAS_TEXTURED, camtrans, m_rasterizer, m_offScreen.get());

	m_canvas->EndFrame();

	// In case multisample is active, blit the FBO
	if (m_samples > 0) {
		m_offScreen->Blit(m_blitOffScreen.get(), true, true);
	}

#ifdef WITH_GAMEENGINE_GPU_SYNC
	// end of all render operations, let's create a sync object just in case
	if (m_sync) {
		// a sync from a previous render, should not happen
		delete m_sync;
		m_sync = nullptr;
	}
	m_sync = m_rasterizer->CreateSync(RAS_ISync::RAS_SYNC_TYPE_FENCE);
#endif

	// remember that we have done render
	m_done = true;
	// the image is not available at this stage
	m_avail = false;
	return true;
}

void ImageRender::Unbind()
{
	GPU_framebuffer_restore();
}

void ImageRender::WaitSync()
{
#ifdef WITH_GAMEENGINE_GPU_SYNC
	if (m_sync) {
		m_sync->Wait();
		// done with it, deleted it
		delete m_sync;
		m_sync = nullptr;
	}
#endif

	// this is needed to finalize the image if the target is a texture
	m_finalOffScreen->MipmapTexture();

	// all rendered operation done and complete, invalidate render for next time
	m_done = false;
}

// cast Image pointer to ImageRender
inline ImageRender *getImageRender(PyImage *self)
{
	return static_cast<ImageRender *>(self->m_image);
}


// python methods

// object initialization
static int ImageRender_init(PyObject *pySelf, PyObject *args, PyObject *kwds)
{
	// parameters - scene object
	PyObject *scene;
	// camera object
	PyObject *camera;

	const RAS_Rect& rect = KX_GetActiveEngine()->GetCanvas()->GetWindowArea();
	int width = rect.GetWidth();
	int height = rect.GetHeight();
	int samples = 0;
	int hdr = 0;
	// parameter keywords
	static const char *kwlist[] = {"sceneObj", "cameraObj", "width", "height", "samples", "hdr", nullptr};
	// get parameters
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|iiii",
	                                 const_cast<char **>(kwlist), &scene, &camera, &width, &height, &samples, &hdr)) {
		return -1;
	}
	try
	{
		// get scene pointer
		KX_Scene *scenePtr(nullptr);
		if (!PyObject_TypeCheck(scene, &KX_Scene::Type)) {
			THRWEXCP(SceneInvalid, S_OK);
		}
		else {
			scenePtr = static_cast<KX_Scene *>EXP_PROXY_REF(scene);
		}

		// get camera pointer
		KX_Camera *cameraPtr(nullptr);
		if (!ConvertPythonToCamera(scenePtr, camera, &cameraPtr, false, "")) {
			THRWEXCP(CameraInvalid, S_OK);
		}

		// get pointer to image structure
		PyImage *self = reinterpret_cast<PyImage *>(pySelf);
		// create source object
		if (self->m_image != nullptr) {
			delete self->m_image;
		}
		self->m_image = new ImageRender(scenePtr, cameraPtr, width, height, samples, hdr);
	}
	catch (Exception & exp)
	{
		exp.report();
		return -1;
	}
	// initialization succeded
	return 0;
}

static PyObject *ImageRender_refresh(PyImage *self, PyObject *args)
{
	ImageRender *imageRender = getImageRender(self);

	if (!imageRender) {
		PyErr_SetString(PyExc_TypeError, "Incomplete ImageRender() object");
		return nullptr;
	}
	if (PyArg_ParseTuple(args, "")) {
		// refresh called with no argument.
		// For other image objects it simply invalidates the image buffer
		// For ImageRender it triggers a render+sync
		// Note that this only makes sense when doing offscreen render on texture
		if (!imageRender->isDone()) {
			if (!imageRender->Render()) {
				Py_RETURN_FALSE;
			}
			// as we are not trying to read the pixels, just unbind
			imageRender->Unbind();
		}
		// wait until all render operations are completed
		// this will also finalize the texture
		imageRender->WaitSync();
		Py_RETURN_TRUE;
	}
	else {
		// fallback on standard processing
		PyErr_Clear();
		return Image_refresh(self, args);
	}
}

// refresh image
static PyObject *ImageRender_render(PyImage *self)
{
	ImageRender *imageRender = getImageRender(self);

	if (!imageRender) {
		PyErr_SetString(PyExc_TypeError, "Incomplete ImageRender() object");
		return nullptr;
	}
	if (!imageRender->Render()) {
		Py_RETURN_FALSE;
	}
	// we are not reading the pixels now, unbind
	imageRender->Unbind();
	Py_RETURN_TRUE;
}


// get horizon color /////////////TO DO
static PyObject *getHorizon(PyImage *self, void *closure)
{
	return Py_BuildValue("[ffff]",
	                     getImageRender(self)->getHorizon(0),
	                     getImageRender(self)->getHorizon(1),
	                     getImageRender(self)->getHorizon(2),
	                     getImageRender(self)->getHorizon(3));
}

// set color
static int setHorizon(PyImage *self, PyObject *value, void *closure)
{
	// check validity of parameter
	if (value == nullptr || !PySequence_Check(value) || PySequence_Size(value) != 4
	    || (!PyFloat_Check(PySequence_Fast_GET_ITEM(value, 0)) && !PyLong_Check(PySequence_Fast_GET_ITEM(value, 0)))
	    || (!PyFloat_Check(PySequence_Fast_GET_ITEM(value, 1)) && !PyLong_Check(PySequence_Fast_GET_ITEM(value, 1)))
	    || (!PyFloat_Check(PySequence_Fast_GET_ITEM(value, 2)) && !PyLong_Check(PySequence_Fast_GET_ITEM(value, 2)))
	    || (!PyFloat_Check(PySequence_Fast_GET_ITEM(value, 3)) && !PyLong_Check(PySequence_Fast_GET_ITEM(value, 3)))) {

		PyErr_SetString(PyExc_TypeError, "The value must be a sequence of 4 floats or ints between 0.0 and 1.0");
		return -1;
	}
	// set horizon color
	getImageRender(self)->setHorizon(
		PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 0)),
		PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 1)),
		PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 2)),
		PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 3)));
	// success
	return 0;
}

// get zenith color
static PyObject *getZenith(PyImage *self, void *closure)
{
	return Py_BuildValue("[ffff]",
	                     getImageRender(self)->getZenith(0),
	                     getImageRender(self)->getZenith(1),
	                     getImageRender(self)->getZenith(2),
	                     getImageRender(self)->getZenith(3));
}

// set color
static int setZenith(PyImage *self, PyObject *value, void *closure)
{
	// check validity of parameter
	if (value == nullptr || !PySequence_Check(value) || PySequence_Size(value) != 4
	    || (!PyFloat_Check(PySequence_Fast_GET_ITEM(value, 0)) && !PyLong_Check(PySequence_Fast_GET_ITEM(value, 0)))
	    || (!PyFloat_Check(PySequence_Fast_GET_ITEM(value, 1)) && !PyLong_Check(PySequence_Fast_GET_ITEM(value, 1)))
	    || (!PyFloat_Check(PySequence_Fast_GET_ITEM(value, 2)) && !PyLong_Check(PySequence_Fast_GET_ITEM(value, 2)))
	    || (!PyFloat_Check(PySequence_Fast_GET_ITEM(value, 3)) && !PyLong_Check(PySequence_Fast_GET_ITEM(value, 3)))) {

		PyErr_SetString(PyExc_TypeError, "The value must be a sequence of 4 floats or ints between 0.0 and 1.0");
		return -1;
	}
	// set zenith color
	getImageRender(self)->setZenith(
		PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 0)),
		PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 1)),
		PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 2)),
		PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 3)));
	// success
	return 0;
}

// get update shadow buffer
static PyObject *getUpdateShadow(PyImage *self)
{
	return PyBool_FromLong(getImageRender(self)->getUpdateShadowBuffer());
}

// set update shadow buffer
static int setUpdateShadow(PyImage *self, PyObject *value)
{
	ImageRender *imageRender = getImageRender(self);
	imageRender->setUpdateShadowBuffer(PyLong_AsLong(value));
	return 0;
}

static PyObject *getColorBindCode(PyImage *self, void *closure)
{
	return PyLong_FromLong(getImageRender(self)->GetColorBindCode());
}

// methods structure
static PyMethodDef imageRenderMethods[] =
{ // methods from ImageBase class
	{"refresh", (PyCFunction)ImageRender_refresh, METH_VARARGS, "Refresh image - invalidate its current content after optionally transferring its content to a target buffer"},
	{"render", (PyCFunction)ImageRender_render, METH_NOARGS, "Render scene - run before refresh() to performs asynchronous render"},
	{nullptr}
};
// attributes structure
static PyGetSetDef imageRenderGetSets[] =
{
	{(char *)"horizon", (getter)getHorizon, (setter)setHorizon, (char *)"horizon color", nullptr},
	{(char *)"background", (getter)getHorizon, (setter)setHorizon, (char *)"horizon color", nullptr}, //DEPRECATED use horizon instead
	{(char *)"zenith", (getter)getZenith, (setter)setZenith, (char *)"zenith color", nullptr},
	// attribute from ImageViewport
	{(char *)"capsize", (getter)ImageViewport_getCaptureSize, (setter)ImageViewport_setCaptureSize, (char *)"size of render area", nullptr},
	{(char *)"alpha", (getter)ImageViewport_getAlpha, (setter)ImageViewport_setAlpha, (char *)"use alpha in texture", nullptr},
	{(char *)"whole", (getter)ImageViewport_getWhole, (setter)ImageViewport_setWhole, (char *)"use whole viewport to render", nullptr},
	// attributes from ImageBase class
	{(char *)"valid", (getter)Image_valid, nullptr, (char *)"bool to tell if an image is available", nullptr},
	{(char *)"image", (getter)Image_getImage, nullptr, (char *)"image data", nullptr},
	{(char *)"size", (getter)Image_getSize, nullptr, (char *)"image size", nullptr},
	{(char *)"scale", (getter)Image_getScale, (setter)Image_setScale, (char *)"fast scale of image (near neighbor)",  nullptr},
	{(char *)"flip", (getter)Image_getFlip, (setter)Image_setFlip, (char *)"flip image vertically", nullptr},
	{(char *)"zbuff", (getter)Image_getZbuff, (setter)Image_setZbuff, (char *)"use depth buffer as texture", nullptr},
	{(char *)"depth", (getter)Image_getDepth, (setter)Image_setDepth, (char *)"get depth information from z-buffer using unsigned int precision", nullptr},
	{(char *)"filter", (getter)Image_getFilter, (setter)Image_setFilter, (char *)"pixel filter", nullptr},
	{(char *)"updateShadow", (getter)getUpdateShadow, (setter)setUpdateShadow, (char *)"update shadow buffers", nullptr},
	{(char *)"colorBindCode", (getter)getColorBindCode, nullptr, (char *)"Off-screen color texture bind code", nullptr},
	{nullptr}
};


// define python type
PyTypeObject ImageRenderType = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"VideoTexture.ImageRender",   /*tp_name*/
	sizeof(PyImage),          /*tp_basicsize*/
	0,                         /*tp_itemsize*/
	(destructor)Image_dealloc, /*tp_dealloc*/
	0,                         /*tp_print*/
	0,                         /*tp_getattr*/
	0,                         /*tp_setattr*/
	0,                         /*tp_compare*/
	0,                         /*tp_repr*/
	0,                         /*tp_as_number*/
	0,                         /*tp_as_sequence*/
	0,                         /*tp_as_mapping*/
	0,                         /*tp_hash */
	0,                         /*tp_call*/
	0,                         /*tp_str*/
	0,                         /*tp_getattro*/
	0,                         /*tp_setattro*/
	&imageBufferProcs,         /*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT,        /*tp_flags*/
	"Image source from render",       /* tp_doc */
	0,                     /* tp_traverse */
	0,                     /* tp_clear */
	0,                     /* tp_richcompare */
	0,                     /* tp_weaklistoffset */
	0,                     /* tp_iter */
	0,                     /* tp_iternext */
	imageRenderMethods,    /* tp_methods */
	0,                   /* tp_members */
	imageRenderGetSets,          /* tp_getset */
	0,                         /* tp_base */
	0,                         /* tp_dict */
	0,                         /* tp_descr_get */
	0,                         /* tp_descr_set */
	0,                         /* tp_dictoffset */
	(initproc)ImageRender_init,     /* tp_init */
	0,                         /* tp_alloc */
	Image_allocNew,           /* tp_new */
};

// object initialization
static int ImageMirror_init(PyObject *pySelf, PyObject *args, PyObject *kwds)
{
	// parameters - scene object
	PyObject *scene;
	// reference object for mirror
	PyObject *observer;
	// object holding the mirror
	PyObject *mirror;
	// material of the mirror
	short materialID = 0;

	const RAS_Rect& rect = KX_GetActiveEngine()->GetCanvas()->GetWindowArea();
	int width = rect.GetWidth();
	int height = rect.GetHeight();
	int samples = 0;
	int hdr = 0;

	// parameter keywords
	static const char *kwlist[] = {"scene", "observer", "mirror", "material", "width", "height", "samples", "hdr", nullptr};
	// get parameters
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO|hiiii",
	                                 const_cast<char **>(kwlist), &scene, &observer, &mirror, &materialID,
	                                 &width, &height, &samples, &hdr)) {
		return -1;
	}
	try
	{
		// get scene pointer
		KX_Scene *scenePtr(nullptr);
		if (scene != nullptr && PyObject_TypeCheck(scene, &KX_Scene::Type)) {
			scenePtr = static_cast<KX_Scene *>EXP_PROXY_REF(scene);
		}
		else {
			THRWEXCP(SceneInvalid, S_OK);
		}

		if (scenePtr == nullptr) { /* in case the python proxy reference is invalid */
			THRWEXCP(SceneInvalid, S_OK);
		}

		// get observer pointer
		KX_GameObject *observerPtr(nullptr);
		if (!ConvertPythonToGameObject(0, observer, &observerPtr, false, "")) {
			THRWEXCP(ObserverInvalid, S_OK);
		}

		if (observerPtr == nullptr) { /* in case the python proxy reference is invalid */
			THRWEXCP(ObserverInvalid, S_OK);
		}

		// get mirror pointer
		KX_GameObject *mirrorPtr(nullptr);
		if (!ConvertPythonToGameObject(0, mirror, &mirrorPtr, false, "")) {
			THRWEXCP(MirrorInvalid, S_OK);
		}

		if (mirrorPtr == nullptr) { /* in case the python proxy reference is invalid */
			THRWEXCP(MirrorInvalid, S_OK);
		}

		// locate the material in the mirror
		RAS_IMaterial *material = getMaterial(mirrorPtr, materialID);
		if (material == nullptr) {
			THRWEXCP(MaterialNotAvail, S_OK);
		}

		// get pointer to image structure
		PyImage *self = reinterpret_cast<PyImage *>(pySelf);

		// create source object
		if (self->m_image != nullptr) {
			delete self->m_image;
			self->m_image = nullptr;
		}
		self->m_image = new ImageRender(scenePtr, observerPtr, mirrorPtr, material, width, height, samples, hdr);
	}
	catch (Exception & exp)
	{
		exp.report();
		return -1;
	}
	// initialization succeeded
	return 0;
}

// get background color
static PyObject *getClip(PyImage *self, void *closure)
{
	return PyFloat_FromDouble(getImageRender(self)->getClip());
}

// set clip
static int setClip(PyImage *self, PyObject *value, void *closure)
{
	// check validity of parameter
	double clip;
	if (value == nullptr || !PyFloat_Check(value) || (clip = PyFloat_AsDouble(value)) < 0.01 || clip > 5000.0) {
		PyErr_SetString(PyExc_TypeError, "The value must be an float between 0.01 and 5000");
		return -1;
	}
	// set background color
	getImageRender(self)->setClip(float(clip));
	// success
	return 0;
}

// attributes structure
static PyGetSetDef imageMirrorGetSets[] =
{
	{(char *)"clip", (getter)getClip, (setter)setClip, (char *)"clipping distance", nullptr},
	// attribute from ImageRender
	{(char *)"horizon", (getter)getHorizon, (setter)setHorizon, (char *)"horizon color", nullptr},
	{(char *)"background", (getter)getHorizon, (setter)setHorizon, (char *)"horizon color", nullptr}, //DEPRECATED use horizon/zenith instead.
	{(char *)"zenith", (getter)getZenith, (setter)setZenith, (char *)"zenith color", nullptr},
	// attribute from ImageViewport
	{(char *)"capsize", (getter)ImageViewport_getCaptureSize, (setter)ImageViewport_setCaptureSize, (char *)"size of render area", nullptr},
	{(char *)"alpha", (getter)ImageViewport_getAlpha, (setter)ImageViewport_setAlpha, (char *)"use alpha in texture", nullptr},
	{(char *)"whole", (getter)ImageViewport_getWhole, (setter)ImageViewport_setWhole, (char *)"use whole viewport to render", nullptr},
	// attributes from ImageBase class
	{(char *)"valid", (getter)Image_valid, nullptr, (char *)"bool to tell if an image is available", nullptr},
	{(char *)"image", (getter)Image_getImage, nullptr, (char *)"image data", nullptr},
	{(char *)"size", (getter)Image_getSize, nullptr, (char *)"image size", nullptr},
	{(char *)"scale", (getter)Image_getScale, (setter)Image_setScale, (char *)"fast scale of image (near neighbor)",  nullptr},
	{(char *)"flip", (getter)Image_getFlip, (setter)Image_setFlip, (char *)"flip image vertically", nullptr},
	{(char *)"zbuff", (getter)Image_getZbuff, (setter)Image_setZbuff, (char *)"use depth buffer as texture", nullptr},
	{(char *)"depth", (getter)Image_getDepth, (setter)Image_setDepth, (char *)"get depth information from z-buffer using unsigned int precision", nullptr},
	{(char *)"filter", (getter)Image_getFilter, (setter)Image_setFilter, (char *)"pixel filter", nullptr},
	{(char *)"updateShadow", (getter)getUpdateShadow, (setter)setUpdateShadow, (char *)"update shadow buffers", nullptr},
	{nullptr}
};


// constructor
ImageRender::ImageRender(KX_Scene *scene, KX_GameObject *observer, KX_GameObject *mirror, RAS_IMaterial *mat, unsigned int width, unsigned int height, unsigned short samples, int hdr) :
	ImageViewport(width, height),
	m_render(false),
	m_updateShadowBuffer(false),
	m_done(false),
	m_scene(scene),
	m_samples(samples),
	m_finalOffScreen(nullptr),
	m_sync(nullptr),
	m_observer(observer),
	m_mirror(mirror),
	m_clip(100.f)
{
	GPUHDRType type;
	if (hdr == RAS_Rasterizer::RAS_HDR_HALF_FLOAT) {
		type = GPU_HDR_HALF_FLOAT;
		m_internalFormat = GL_RGBA16F_ARB;
	}
	else if (hdr == RAS_Rasterizer::RAS_HDR_FULL_FLOAT) {
		type = GPU_HDR_FULL_FLOAT;
		m_internalFormat = GL_RGBA32F_ARB;
	}
	else {
		type = GPU_HDR_NONE;
		m_internalFormat = GL_RGBA8;
	}

	m_offScreen.reset(new RAS_OffScreen(m_width, m_height, m_samples, type, GPU_OFFSCREEN_RENDERBUFFER_DEPTH, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_CUSTOM));
	if (m_samples > 0) {
		m_blitOffScreen.reset(new RAS_OffScreen(m_width, m_height, 0, type, GPU_OFFSCREEN_RENDERBUFFER_DEPTH, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_CUSTOM));
		m_finalOffScreen = m_blitOffScreen.get();
	}
	else {
		m_finalOffScreen = m_offScreen.get();
	}

	// this constructor is used for automatic planar mirror
	// create a camera, take all data by default, in any case we will recompute the frustum on each frame
	RAS_CameraData camdata;
	std::vector<mt::vec3_packed> mirrorVerts;
	std::vector<mt::vec3_packed>::iterator it;
	float mirrorArea = 0.f;
	float mirrorNormal[3] = {0.f, 0.f, 0.f};
	float mirrorUp[3];
	float dist, vec[3], axis[3];
	float zaxis[3] = {0.f, 0.f, 1.f};
	float yaxis[3] = {0.f, 1.f, 0.f};
	float mirrorMat[3][3];
	float left, right, top, bottom, back;
	// make sure this camera will delete its node
	m_camera = new KX_Camera(scene, KX_Scene::m_callbacks, camdata, true);
	m_camera->SetName("__mirror__cam__");
	// don't add the camera to the scene object list, it doesn't need to be accessible
	m_owncamera = true;
	// retrieve rendering objects
	m_engine = KX_GetActiveEngine();
	m_rasterizer = m_engine->GetRasterizer();
	m_canvas = m_engine->GetCanvas();
	// locate the vertex assigned to mat and do following calculation in mesh coordinates
	for (KX_Mesh *mesh : mirror->GetMeshList()) {
		for (RAS_MeshMaterial *meshmat : mesh->GetMeshMaterialList()) {
			if (meshmat->GetBucket()->GetMaterial() == mat) {
				RAS_DisplayArray *array = meshmat->GetDisplayArray();
				for (unsigned int j = 0, indexCount = array->GetTriangleIndexCount(); j < indexCount; j += 3) {
					float normal[3];
					const mt::vec3_packed& v1 = array->GetPosition(array->GetTriangleIndex(j));
					const mt::vec3_packed& v2 = array->GetPosition(array->GetTriangleIndex(j + 1));
					const mt::vec3_packed& v3 = array->GetPosition(array->GetTriangleIndex(j + 2));

					mirrorVerts.push_back(v1);
					mirrorVerts.push_back(v2);
					mirrorVerts.push_back(v3);
					float area = normal_tri_v3(normal, v1.data, v2.data, v3.data);
					area = fabs(area);
					mirrorArea += area;
					mul_v3_fl(normal, area);
					add_v3_v3v3(mirrorNormal, mirrorNormal, normal);
				}
			}
		}
	}

	if (mirrorVerts.empty() || mirrorArea < FLT_EPSILON) {
		// no vertex or zero size mirror
		THRWEXCP(MirrorSizeInvalid, S_OK);
	}
	// compute average normal of mirror faces
	mul_v3_fl(mirrorNormal, 1.0f / mirrorArea);
	if (normalize_v3(mirrorNormal) == 0.f) {
		// no normal
		THRWEXCP(MirrorNormalInvalid, S_OK);
	}
	// the mirror plane has an equation of the type ax+by+cz = d where (a,b,c) is the normal vector
	// if the mirror is more vertical then horizontal, the Z axis is the up direction.
	// otherwise the Y axis is the up direction.
	// If the mirror is not perfectly vertical(horizontal), the Z(Y) axis projection on the mirror
	// plan by the normal will be the up direction.
	if (fabsf(mirrorNormal[2]) > fabsf(mirrorNormal[1]) &&
	    fabsf(mirrorNormal[2]) > fabsf(mirrorNormal[0])) {
		// the mirror is more horizontal than vertical
		copy_v3_v3(axis, yaxis);
	}
	else {
		// the mirror is more vertical than horizontal
		copy_v3_v3(axis, zaxis);
	}
	dist = dot_v3v3(mirrorNormal, axis);
	if (fabsf(dist) < FLT_EPSILON) {
		// the mirror is already fully aligned with up axis
		copy_v3_v3(mirrorUp, axis);
	}
	else {
		// projection of axis to mirror plane through normal
		copy_v3_v3(vec, mirrorNormal);
		mul_v3_fl(vec, dist);
		sub_v3_v3v3(mirrorUp, axis, vec);
		if (normalize_v3(mirrorUp) == 0.f) {
			// should not happen
			THRWEXCP(MirrorHorizontal, S_OK);
			return;
		}
	}
	// compute rotation matrix between local coord and mirror coord
	// to match camera orientation, we select mirror z = -normal, y = up, x = y x z
	negate_v3_v3(mirrorMat[2], mirrorNormal);
	copy_v3_v3(mirrorMat[1], mirrorUp);
	cross_v3_v3v3(mirrorMat[0], mirrorMat[1], mirrorMat[2]);
	// transpose to make it a orientation matrix from local space to mirror space
	transpose_m3(mirrorMat);
	// transform all vertex to plane coordinates and determine mirror position
	left = FLT_MAX;
	right = -FLT_MAX;
	bottom = FLT_MAX;
	top = -FLT_MAX;
	back = -FLT_MAX; // most backward vertex (=highest Z coord in mirror space)
	for (it = mirrorVerts.begin(); it != mirrorVerts.end(); it++)
	{
		copy_v3_v3(vec, it->data);
		mul_m3_v3(mirrorMat, vec);
		if (vec[0] < left) {
			left = vec[0];
		}
		if (vec[0] > right) {
			right = vec[0];
		}
		if (vec[1] < bottom) {
			bottom = vec[1];
		}
		if (vec[1] > top) {
			top = vec[1];
		}
		if (vec[2] > back) {
			back = vec[2];
		}
	}
	// now store this information in the object for later rendering
	m_mirrorHalfWidth = (right - left) * 0.5f;
	m_mirrorHalfHeight = (top - bottom) * 0.5f;
	if (m_mirrorHalfWidth < 0.01f || m_mirrorHalfHeight < 0.01f) {
		// mirror too small
		THRWEXCP(MirrorTooSmall, S_OK);
	}
	// mirror position in mirror coord
	vec[0] = (left + right) * 0.5f;
	vec[1] = (top + bottom) * 0.5f;
	vec[2] = back;
	// convert it in local space: transpose again the matrix to get back to mirror to local transform
	transpose_m3(mirrorMat);
	mul_m3_v3(mirrorMat, vec);
	// mirror position in local space
	m_mirrorPos = mt::vec3(vec);
	// mirror normal vector (pointed towards the back of the mirror) in local space
	m_mirrorZ = mt::vec3(-mirrorNormal[0], -mirrorNormal[1], -mirrorNormal[2]);
	m_mirrorY = mt::vec3(mirrorUp);
	m_mirrorX = mt::cross(m_mirrorY, m_mirrorZ);
	m_render = true;

	// set mirror background color to scene background color as default
	setHorizonFromScene(m_scene);
	setZenithFromScene(m_scene);
}




// define python type
PyTypeObject ImageMirrorType = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"VideoTexture.ImageMirror",   /*tp_name*/
	sizeof(PyImage),          /*tp_basicsize*/
	0,                         /*tp_itemsize*/
	(destructor)Image_dealloc, /*tp_dealloc*/
	0,                         /*tp_print*/
	0,                         /*tp_getattr*/
	0,                         /*tp_setattr*/
	0,                         /*tp_compare*/
	0,                         /*tp_repr*/
	0,                         /*tp_as_number*/
	0,                         /*tp_as_sequence*/
	0,                         /*tp_as_mapping*/
	0,                         /*tp_hash */
	0,                         /*tp_call*/
	0,                         /*tp_str*/
	0,                         /*tp_getattro*/
	0,                         /*tp_setattro*/
	&imageBufferProcs,         /*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT,        /*tp_flags*/
	"Image source from mirror",       /* tp_doc */
	0,                     /* tp_traverse */
	0,                     /* tp_clear */
	0,                     /* tp_richcompare */
	0,                     /* tp_weaklistoffset */
	0,                     /* tp_iter */
	0,                     /* tp_iternext */
	imageRenderMethods,    /* tp_methods */
	0,                   /* tp_members */
	imageMirrorGetSets,          /* tp_getset */
	0,                         /* tp_base */
	0,                         /* tp_dict */
	0,                         /* tp_descr_get */
	0,                         /* tp_descr_set */
	0,                         /* tp_dictoffset */
	(initproc)ImageMirror_init,     /* tp_init */
	0,                         /* tp_alloc */
	Image_allocNew,           /* tp_new */
};


