/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/ImageRender.cpp
 *  \ingroup bgevideotex
 */

// implementation

#include "ImageRender.h"

#include "BKE_context.hh"
#include "BKE_scene.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "DEG_depsgraph_query.hh"
#include "GPU_framebuffer.hh"
#include "GPU_state.hh"
#include "GPU_viewport.hh"

#include "CM_Message.h"
#include "EXP_PythonCallBack.h"
#include "KX_Globals.h"
#include "RAS_IVertex.h"
#include "RAS_MeshObject.h"
#include "RAS_Polygon.h"
#include "Texture.h"

ExceptionID SceneInvalid, CameraInvalid, ObserverInvalid, FrameBufferInvalid;
ExceptionID MirrorInvalid, MirrorSizeInvalid, MirrorNormalInvalid, MirrorHorizontal,
    MirrorTooSmall;
ExpDesc SceneInvalidDesc(SceneInvalid, "Scene object is invalid");
ExpDesc CameraInvalidDesc(CameraInvalid, "Camera object is invalid");
ExpDesc ObserverInvalidDesc(ObserverInvalid, "Observer object is invalid");
ExpDesc FrameBufferInvalidDesc(FrameBufferInvalid, "FrameBuffer object is invalid");
ExpDesc MirrorInvalidDesc(MirrorInvalid, "Mirror object is invalid");
ExpDesc MirrorSizeInvalidDesc(MirrorSizeInvalid, "Mirror has no vertex or no size");
ExpDesc MirrorNormalInvalidDesc(MirrorNormalInvalid, "Cannot determine mirror plane");
ExpDesc MirrorHorizontalDesc(MirrorHorizontal, "Mirror is horizontal in local space");
ExpDesc MirrorTooSmallDesc(MirrorTooSmall, "Mirror is too small");

// constructor
ImageRender::ImageRender(KX_Scene *scene,
                         KX_Camera *camera,
                         unsigned int width,
                         unsigned int height,
                         unsigned short samples)
    : ImageViewport(width, height),
#ifdef WITH_PYTHON
      m_preDrawCallbacks(nullptr),
      m_postDrawCallbacks(nullptr),
#endif
      m_render(true),
      m_done(false),
      m_scene(scene),
      m_camera(camera),
      m_samples(samples),
      m_owncamera(false),
      m_observer(nullptr),
      m_mirror(nullptr),
      m_clip(100.f),
      m_mirrorHalfWidth(0.f),
      m_mirrorHalfHeight(0.f)
{
  // retrieve rendering objects
  m_engine = KX_GetActiveEngine();
  m_rasterizer = m_engine->GetRasterizer();
  m_canvas = m_engine->GetCanvas();

  m_internalFormat = GL_RGBA16F_ARB;

  m_targetfb = GPU_framebuffer_create("game_fb");

  m_scene->AddImageRenderCamera(m_camera);
}

// destructor
ImageRender::~ImageRender(void)
{
#ifdef WITH_PYTHON
  // These may be nullptr but the macro checks.
  Py_CLEAR(m_preDrawCallbacks);
  m_preDrawCallbacks = nullptr;
  Py_CLEAR(m_postDrawCallbacks);
  m_postDrawCallbacks = nullptr;
#endif
  m_scene->RemoveImageRenderCamera(m_camera);

  if (m_owncamera) {
    m_camera->Release();
  }

  GPU_framebuffer_free(m_targetfb);
  m_targetfb = nullptr;
}

int ImageRender::GetColorBindCode() const
{
  if (m_camera->GetGPUViewport()) {
    return GPU_texture_opengl_bindcode(GPU_viewport_color_texture(m_camera->GetGPUViewport(), 0));
  }
  return -1;
}

// capture image from viewport
void ImageRender::calcViewport(unsigned int texId, double ts, unsigned int format)
{
  // render the scene from the camera
  if (!m_done) {
    if (!Render()) {
      return;
    }
  }
  m_done = false;

  const RAS_Rect *viewport = &m_canvas->GetViewportArea();
  GPU_viewport(
      viewport->GetLeft(), viewport->GetBottom(), viewport->GetWidth(), viewport->GetHeight());
  GPU_scissor_test(true);
  GPU_scissor(
      viewport->GetLeft(), viewport->GetBottom(), viewport->GetWidth(), viewport->GetHeight());
  GPU_apply_state();

  GPUAttachment config[] = {
      GPU_ATTACHMENT_TEXTURE(GPU_viewport_depth_texture(m_camera->GetGPUViewport())),
      GPU_ATTACHMENT_TEXTURE(GPU_viewport_color_texture(m_camera->GetGPUViewport(), 0))};

  GPU_framebuffer_config_array(m_targetfb, config, sizeof(config) / sizeof(GPUAttachment));

  GPU_framebuffer_bind(m_targetfb);

  // get image from viewport (or FBO)
  ImageViewport::calcViewport(texId, ts, format);

  GPU_framebuffer_restore();
}

bool ImageRender::Render()
{
  RAS_FrameFrustum frustum;

  if (!m_render) {
    // no need to compute texture in non texture rendering
    return false;
  }
  if (m_camera->GetViewport() ||  // camera must be inactive
      m_camera == m_scene->GetActiveCamera() || m_camera == m_scene->GetOverlayCamera())
  {
    CM_Warning("ImageRender: You are trying to use a non valid camera named  "
               << m_camera->GetName());
    return false;
  }

  /* Viewport render mode doesn't support ImageRender then exit here
   * if we are trying to use not supported features. */
  if (KX_GetActiveEngine()->UseViewportRender()) {
    CM_Warning("Viewport Render mode doesn't support ImageRender");
    return false;
  }

  if (m_mirror) {
    // mirror mode, compute camera frustum, position and orientation
    // convert mirror position and normal in world space
    const MT_Matrix3x3 &mirrorObjWorldOri = m_mirror->GetSGNode()->GetWorldOrientation();
    const MT_Vector3 &mirrorObjWorldPos = m_mirror->GetSGNode()->GetWorldPosition();
    const MT_Vector3 &mirrorObjWorldScale = m_mirror->GetSGNode()->GetWorldScaling();
    MT_Vector3 mirrorWorldPos = mirrorObjWorldPos +
                                mirrorObjWorldScale * (mirrorObjWorldOri * m_mirrorPos);
    MT_Vector3 mirrorWorldZ = mirrorObjWorldOri * m_mirrorZ;
    // get observer world position
    const MT_Vector3 &observerWorldPos = m_observer->GetSGNode()->GetWorldPosition();
    // get plane D term = mirrorPos . normal
    MT_Scalar mirrorPlaneDTerm = mirrorWorldPos.dot(mirrorWorldZ);
    // compute distance of observer to mirror = D - observerPos . normal
    MT_Scalar observerDistance = mirrorPlaneDTerm - observerWorldPos.dot(mirrorWorldZ);
    // if distance < 0.01 => observer is on wrong side of mirror, don't render
    if (observerDistance < 0.01)
      return false;
    // set camera world position = observerPos + normal * 2 * distance
    MT_Vector3 cameraWorldPos = observerWorldPos +
                                (MT_Scalar(2.0) * observerDistance) * mirrorWorldZ;
    m_camera->GetSGNode()->SetLocalPosition(cameraWorldPos);
    // set camera orientation: z=normal, y=mirror_up in world space, x= y x z
    MT_Vector3 mirrorWorldY = mirrorObjWorldOri * m_mirrorY;
    MT_Vector3 mirrorWorldX = mirrorObjWorldOri * m_mirrorX;
    MT_Matrix3x3 cameraWorldOri(mirrorWorldX[0],
                                mirrorWorldY[0],
                                mirrorWorldZ[0],
                                mirrorWorldX[1],
                                mirrorWorldY[1],
                                mirrorWorldZ[1],
                                mirrorWorldX[2],
                                mirrorWorldY[2],
                                mirrorWorldZ[2]);
    m_camera->GetSGNode()->SetLocalOrientation(cameraWorldOri);
    m_camera->GetSGNode()->UpdateWorldData(0.0);
    // compute camera frustum:
    //   get position of mirror relative to camera: offset = mirrorPos-cameraPos
    MT_Vector3 mirrorOffset = mirrorWorldPos - cameraWorldPos;
    //   convert to camera orientation
    mirrorOffset = mirrorOffset * cameraWorldOri;
    //   scale mirror size to world scale:
    //     get closest local axis for mirror Y and X axis and scale height and width by local axis
    //     scale
    MT_Scalar x, y;
    x = fabs(m_mirrorY[0]);
    y = fabs(m_mirrorY[1]);
    float height = (x > y) ? ((x > fabs(m_mirrorY[2])) ? mirrorObjWorldScale[0] :
                                                         mirrorObjWorldScale[2]) :
                             ((y > fabs(m_mirrorY[2])) ? mirrorObjWorldScale[1] :
                                                         mirrorObjWorldScale[2]);
    x = fabs(m_mirrorX[0]);
    y = fabs(m_mirrorX[1]);
    float width = (x > y) ?
                      ((x > fabs(m_mirrorX[2])) ? mirrorObjWorldScale[0] :
                                                  mirrorObjWorldScale[2]) :
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
  // Store settings to be restored later
  const RAS_Rasterizer::StereoMode stereomode = m_rasterizer->GetStereoMode();

  m_rasterizer->BeginFrame(m_engine->GetFrameTime());

  int viewport[4] = {
      m_position[0], m_position[1], m_position[0] + m_capSize[0], m_position[1] + m_capSize[1]};

  GPU_viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  GPU_scissor_test(true);
  GPU_scissor(viewport[0], viewport[1], viewport[2], viewport[3]);
  GPU_apply_state();

  // GPU_clear_depth(1.0f);

  m_rasterizer->SetAuxilaryClientInfo(m_scene);

  // matrix calculation, don't apply any of the stereo mode
  m_rasterizer->SetStereoMode(RAS_Rasterizer::RAS_STEREO_NOSTEREO);

  if (m_mirror) {
    // frustum was computed above
    // get frustum matrix and set projection matrix
    MT_Matrix4x4 projmat = m_rasterizer->GetFrustumMatrix(RAS_Rasterizer::RAS_STEREO_LEFTEYE,
                                                          frustum.x1,
                                                          frustum.x2,
                                                          frustum.y1,
                                                          frustum.y2,
                                                          frustum.camnear,
                                                          frustum.camfar);

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
    MT_Matrix4x4 projmat;

    // compute the aspect ratio from frame blender scene settings so that render to texture
    // works the same in Blender and in Blender player
    if (blenderScene->r.ysch != 0)
      aspect_ratio = float(blenderScene->r.xsch * blenderScene->r.xasp) /
                     float(blenderScene->r.ysch * blenderScene->r.yasp);

    if (orthographic) {

      RAS_FramingManager::ComputeDefaultOrtho(nearfrust,
                                              farfrust,
                                              m_camera->GetScale(),
                                              aspect_ratio,
                                              m_camera->GetSensorFit(),
                                              shift_x,
                                              shift_y,
                                              frustum);

      projmat = m_rasterizer->GetOrthoMatrix(
          frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar);
    }
    else {
      RAS_FramingManager::ComputeDefaultFrustum(nearfrust,
                                                farfrust,
                                                lens,
                                                sensor_x,
                                                sensor_y,
                                                RAS_SENSORFIT_AUTO,
                                                shift_x,
                                                shift_y,
                                                aspect_ratio,
                                                frustum);

      projmat = m_rasterizer->GetFrustumMatrix(RAS_Rasterizer::RAS_STEREO_LEFTEYE,
                                               frustum.x1,
                                               frustum.x2,
                                               frustum.y1,
                                               frustum.y2,
                                               frustum.camnear,
                                               frustum.camfar);
    }
    m_camera->SetProjectionMatrix(projmat);
  }

  MT_Transform camtrans(m_camera->GetWorldToCamera());
  MT_Matrix4x4 viewmat(camtrans);
  m_camera->SetModelviewMatrix(viewmat);

  // restore the stereo mode now that the matrix is computed
  m_rasterizer->SetStereoMode(stereomode);

  if (m_rasterizer->Stereo()) {
    // stereo mode change render settings that disturb this render, cancel them all
    // we don't need to restore them as they are set before each frame render.
    glDrawBuffer(GL_BACK_LEFT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_POLYGON_STIPPLE);
  }

  m_engine->UpdateAnimations(m_scene);

  bContext *C = KX_GetActiveEngine()->GetContext();
  Main *bmain = CTX_data_main(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);

  if (!depsgraph) {
    return false;
  }

  m_scene->SetCurrentGPUViewport(m_camera->GetGPUViewport());

  if (m_scene->SomethingIsMoving()) {
    /* Add a depsgraph notifier to trigger
     * DRW_notify_view_update on next draw loop. */
    DEG_id_tag_update(&m_camera->GetBlenderObject()->id, ID_RECALC_TRANSFORM);
  }

  m_scene->TagForExtraIdsUpdate(bmain, m_camera);
  /* We need the changes to be flushed before each draw loop! */
  BKE_scene_graph_update_tagged(depsgraph, bmain);

#ifdef WITH_PYTHON
  RunPreDrawCallbacks();
#endif

  int num_passes = max_ii(1, m_samples);
  num_passes = min_ii(num_passes, m_scene->GetBlenderScene()->eevee.taa_samples);

  for (int i = 0; i < num_passes; i++) {
    GPU_framebuffer_clear_depth(GPU_framebuffer_active_get(), 1.0f);
    /* viewport and window share the same values here */
    const rcti window = {viewport[0], viewport[2], viewport[1], viewport[3]};
    m_scene->RenderAfterCameraSetupImageRender(m_camera, &window);
  }

#ifdef WITH_PYTHON
  RunPostDrawCallbacks();
  // These may be nullptr but the macro checks.
  Py_CLEAR(m_preDrawCallbacks);
  m_preDrawCallbacks = nullptr;
  Py_CLEAR(m_postDrawCallbacks);
  m_postDrawCallbacks = nullptr;
#endif

  m_canvas->EndFrame();

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

void ImageRender::RunPreDrawCallbacks()
{
  PyObject *list = m_preDrawCallbacks;
  if (!list || PyList_GET_SIZE(list) == 0) {
    return;
  }

  EXP_RunPythonCallBackList(list, nullptr, 0, 0);

  /* Ensure DRW_notify_view_update will be called next time BKE_scene_graph_update_tagged
   * will be called if we did changes related to scene_eval in ImageRender draw callbacks */
  DEG_id_tag_update(&m_camera->GetBlenderObject()->id, ID_RECALC_TRANSFORM);
}

void ImageRender::RunPostDrawCallbacks()
{
  PyObject *list = m_postDrawCallbacks;
  if (!list || PyList_GET_SIZE(list) == 0) {
    return;
  }

  EXP_RunPythonCallBackList(list, nullptr, 0, 0);

  /* Ensure DRW_notify_view_update will be called next time BKE_scene_graph_update_tagged
   * will be called if we did changes related to scene_eval in ImageRender draw callbacks */
  DEG_id_tag_update(&m_camera->GetBlenderObject()->id, ID_RECALC_TRANSFORM);
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

  RAS_ICanvas *canvas = KX_GetActiveEngine()->GetCanvas();
  int width = canvas->GetWidth();
  int height = canvas->GetHeight();
  int samples = 1;
  // parameter keywords
  static const char *kwlist[] = {"sceneObj", "cameraObj", "width", "height", "samples", nullptr};
  // get parameters
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "OO|iii",
                                   const_cast<char **>(kwlist),
                                   &scene,
                                   &camera,
                                   &width,
                                   &height,
                                   &samples))
    return -1;
  try {
    // get scene pointer
    KX_Scene *scenePtr(nullptr);
    if (!PyObject_TypeCheck(scene, &KX_Scene::Type)) {
      THRWEXCP(SceneInvalid, S_OK);
    }
    else {
      scenePtr = static_cast<KX_Scene *> EXP_PROXY_REF(scene);
    }

    // get camera pointer
    KX_Camera *cameraPtr(nullptr);
    if (!ConvertPythonToCamera(scenePtr, camera, &cameraPtr, false, "")) {
      THRWEXCP(CameraInvalid, S_OK);
    }

    // get pointer to image structure
    PyImage *self = reinterpret_cast<PyImage *>(pySelf);
    // create source object
    if (self->m_image != nullptr)
      delete self->m_image;
    self->m_image = new ImageRender(scenePtr, cameraPtr, width, height, samples);
  }
  catch (Exception &exp) {
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

static PyObject *getColorBindCode(PyImage *self, void *closure)
{
  return PyLong_FromLong(getImageRender(self)->GetColorBindCode());
}

static PyObject *getPreDrawCallbacks(PyImage *self, void *closure)
{
  ImageRender *imageRender = getImageRender(self);
  if (!imageRender->m_preDrawCallbacks) {
    imageRender->m_preDrawCallbacks = PyList_New(0);
  }

  Py_INCREF(imageRender->m_preDrawCallbacks);

  return imageRender->m_preDrawCallbacks;
}

static int setPreDrawCallbacks(PyImage *self, PyObject *value, void *closure)
{
  ImageRender *imageRender = getImageRender(self);

  if (!PyList_CheckExact(value)) {
    PyErr_SetString(PyExc_ValueError, "Expected a list");
    return PY_SET_ATTR_FAIL;
  }

  Py_XDECREF(imageRender->m_preDrawCallbacks);

  Py_INCREF(value);
  imageRender->m_preDrawCallbacks = value;

  return PY_SET_ATTR_SUCCESS;
}

static PyObject *getPostDrawCallbacks(PyImage *self, void *closure)
{
  ImageRender *imageRender = getImageRender(self);
  if (!imageRender->m_postDrawCallbacks) {
    imageRender->m_postDrawCallbacks = PyList_New(0);
  }

  Py_INCREF(imageRender->m_postDrawCallbacks);

  return imageRender->m_postDrawCallbacks;
}

static int setPostDrawCallbacks(PyImage *self, PyObject *value, void *closure)
{
  ImageRender *imageRender = getImageRender(self);

  if (!PyList_CheckExact(value)) {
    PyErr_SetString(PyExc_ValueError, "Expected a list");
    return PY_SET_ATTR_FAIL;
  }

  Py_XDECREF(imageRender->m_postDrawCallbacks);

  Py_INCREF(value);
  imageRender->m_postDrawCallbacks = value;

  return PY_SET_ATTR_SUCCESS;
}

// methods structure
static PyMethodDef imageRenderMethods[] = {  // methods from ImageBase class
    {"refresh",
     (PyCFunction)ImageRender_refresh,
     METH_VARARGS,
     "Refresh image - invalidate its current content after optionally transferring its content to "
     "a target buffer"},
    {"render",
     (PyCFunction)ImageRender_render,
     METH_NOARGS,
     "Render scene - run before refresh() to performs asynchronous render"},
    {nullptr}};
// attributes structure
static PyGetSetDef imageRenderGetSets[] = {
    // attribute from ImageViewport
    {(char *)"capsize",
     (getter)ImageViewport_getCaptureSize,
     (setter)ImageViewport_setCaptureSize,
     (char *)"size of render area",
     nullptr},
    {(char *)"alpha",
     (getter)ImageViewport_getAlpha,
     (setter)ImageViewport_setAlpha,
     (char *)"use alpha in texture",
     nullptr},
    {(char *)"whole",
     (getter)ImageViewport_getWhole,
     (setter)ImageViewport_setWhole,
     (char *)"use whole viewport to render",
     nullptr},
    // attributes from ImageBase class
    {(char *)"valid",
     (getter)Image_valid,
     nullptr,
     (char *)"bool to tell if an image is available",
     nullptr},
    {(char *)"image", (getter)Image_getImage, nullptr, (char *)"image data", nullptr},
    {(char *)"size", (getter)Image_getSize, nullptr, (char *)"image size", nullptr},
    {(char *)"scale",
     (getter)Image_getScale,
     (setter)Image_setScale,
     (char *)"fast scale of image (near neighbor)",
     nullptr},
    {(char *)"flip",
     (getter)Image_getFlip,
     (setter)Image_setFlip,
     (char *)"flip image vertically",
     nullptr},
    {(char *)"zbuff",
     (getter)Image_getZbuff,
     (setter)Image_setZbuff,
     (char *)"use depth buffer as texture",
     nullptr},
    {(char *)"depth",
     (getter)Image_getDepth,
     (setter)Image_setDepth,
     (char *)"get depth information from z-buffer using unsigned int precision",
     nullptr},
    {(char *)"filter",
     (getter)Image_getFilter,
     (setter)Image_setFilter,
     (char *)"pixel filter",
     nullptr},
    {(char *)"colorBindCode",
     (getter)getColorBindCode,
     nullptr,
     (char *)"Off-screen color texture bind code",
     nullptr},
    {(char *)"pre_draw",
     (getter)getPreDrawCallbacks,
     (setter)setPreDrawCallbacks,
     (char *)"Image Render pre-draw callbacks",
     nullptr},
    {(char *)"post_draw",
     (getter)getPostDrawCallbacks,
     (setter)setPostDrawCallbacks,
     (char *)"Image Render post-draw callbacks",
     nullptr},
    {nullptr}};

// define python type
PyTypeObject ImageRenderType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.ImageRender", /*tp_name*/
    sizeof(PyImage),                                              /*tp_basicsize*/
    0,                                                            /*tp_itemsize*/
    (destructor)Image_dealloc,                                    /*tp_dealloc*/
    0,                                                            /*tp_print*/
    0,                                                            /*tp_getattr*/
    0,                                                            /*tp_setattr*/
    0,                                                            /*tp_compare*/
    0,                                                            /*tp_repr*/
    0,                                                            /*tp_as_number*/
    0,                                                            /*tp_as_sequence*/
    0,                                                            /*tp_as_mapping*/
    0,                                                            /*tp_hash */
    0,                                                            /*tp_call*/
    0,                                                            /*tp_str*/
    0,                                                            /*tp_getattro*/
    0,                                                            /*tp_setattro*/
    &imageBufferProcs,                                            /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                           /*tp_flags*/
    "Image source from render",                                   /* tp_doc */
    0,                                                            /* tp_traverse */
    0,                                                            /* tp_clear */
    0,                                                            /* tp_richcompare */
    0,                                                            /* tp_weaklistoffset */
    0,                                                            /* tp_iter */
    0,                                                            /* tp_iternext */
    imageRenderMethods,                                           /* tp_methods */
    0,                                                            /* tp_members */
    imageRenderGetSets,                                           /* tp_getset */
    0,                                                            /* tp_base */
    0,                                                            /* tp_dict */
    0,                                                            /* tp_descr_get */
    0,                                                            /* tp_descr_set */
    0,                                                            /* tp_dictoffset */
    (initproc)ImageRender_init,                                   /* tp_init */
    0,                                                            /* tp_alloc */
    Image_allocNew,                                               /* tp_new */
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

  RAS_ICanvas *canvas = KX_GetActiveEngine()->GetCanvas();
  int width = canvas->GetWidth();
  int height = canvas->GetHeight();
  int samples = 1;

  // parameter keywords
  static const char *kwlist[] = {
      "scene", "observer", "mirror", "material", "width", "height", "samples", nullptr};
  // get parameters
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "OOO|hiii",
                                   const_cast<char **>(kwlist),
                                   &scene,
                                   &observer,
                                   &mirror,
                                   &materialID,
                                   &width,
                                   &height,
                                   &samples))
    return -1;
  try {
    // get scene pointer
    KX_Scene *scenePtr(nullptr);
    if (scene != nullptr && PyObject_TypeCheck(scene, &KX_Scene::Type))
      scenePtr = static_cast<KX_Scene *> EXP_PROXY_REF(scene);
    else
      THRWEXCP(SceneInvalid, S_OK);

    if (scenePtr == nullptr) /* in case the python proxy reference is invalid */
      THRWEXCP(SceneInvalid, S_OK);

    // get observer pointer
    KX_GameObject *observerPtr(nullptr);
    if (!ConvertPythonToGameObject(
            scenePtr->GetLogicManager(), observer, &observerPtr, false, "")) {
      THRWEXCP(ObserverInvalid, S_OK);
    }

    if (observerPtr == nullptr) /* in case the python proxy reference is invalid */
      THRWEXCP(ObserverInvalid, S_OK);

    // get mirror pointer
    KX_GameObject *mirrorPtr(nullptr);
    if (!ConvertPythonToGameObject(scenePtr->GetLogicManager(), mirror, &mirrorPtr, false, "")) {
      THRWEXCP(MirrorInvalid, S_OK);
    }

    if (mirrorPtr == nullptr) /* in case the python proxy reference is invalid */
      THRWEXCP(MirrorInvalid, S_OK);

    // locate the material in the mirror
    RAS_IPolyMaterial *material = getMaterial(mirrorPtr, materialID);
    if (material == nullptr)
      THRWEXCP(MaterialNotAvail, S_OK);

    // get pointer to image structure
    PyImage *self = reinterpret_cast<PyImage *>(pySelf);

    // create source object
    if (self->m_image != nullptr) {
      delete self->m_image;
      self->m_image = nullptr;
    }
    self->m_image = new ImageRender(
        scenePtr, observerPtr, mirrorPtr, material, width, height, samples);
  }
  catch (Exception &exp) {
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
  if (value == nullptr || !PyFloat_Check(value) || (clip = PyFloat_AsDouble(value)) < 0.01 ||
      clip > 5000.0) {
    PyErr_SetString(PyExc_TypeError, "The value must be an float between 0.01 and 5000");
    return -1;
  }
  // set background color
  getImageRender(self)->setClip(float(clip));
  // success
  return 0;
}

// attributes structure
static PyGetSetDef imageMirrorGetSets[] = {
    {(char *)"clip", (getter)getClip, (setter)setClip, (char *)"clipping distance", nullptr},
    // attribute from ImageViewport
    {(char *)"capsize",
     (getter)ImageViewport_getCaptureSize,
     (setter)ImageViewport_setCaptureSize,
     (char *)"size of render area",
     nullptr},
    {(char *)"alpha",
     (getter)ImageViewport_getAlpha,
     (setter)ImageViewport_setAlpha,
     (char *)"use alpha in texture",
     nullptr},
    {(char *)"whole",
     (getter)ImageViewport_getWhole,
     (setter)ImageViewport_setWhole,
     (char *)"use whole viewport to render",
     nullptr},
    // attributes from ImageBase class
    {(char *)"valid",
     (getter)Image_valid,
     nullptr,
     (char *)"bool to tell if an image is available",
     nullptr},
    {(char *)"image", (getter)Image_getImage, nullptr, (char *)"image data", nullptr},
    {(char *)"size", (getter)Image_getSize, nullptr, (char *)"image size", nullptr},
    {(char *)"scale",
     (getter)Image_getScale,
     (setter)Image_setScale,
     (char *)"fast scale of image (near neighbor)",
     nullptr},
    {(char *)"flip",
     (getter)Image_getFlip,
     (setter)Image_setFlip,
     (char *)"flip image vertically",
     nullptr},
    {(char *)"zbuff",
     (getter)Image_getZbuff,
     (setter)Image_setZbuff,
     (char *)"use depth buffer as texture",
     nullptr},
    {(char *)"depth",
     (getter)Image_getDepth,
     (setter)Image_setDepth,
     (char *)"get depth information from z-buffer using unsigned int precision",
     nullptr},
    {(char *)"filter",
     (getter)Image_getFilter,
     (setter)Image_setFilter,
     (char *)"pixel filter",
     nullptr},
    {nullptr}};

// constructor
ImageRender::ImageRender(KX_Scene *scene,
                         KX_GameObject *observer,
                         KX_GameObject *mirror,
                         RAS_IPolyMaterial *mat,
                         unsigned int width,
                         unsigned int height,
                         unsigned short samples)
    : ImageViewport(width, height),
      m_render(false),
      m_done(false),
      m_scene(scene),
      m_samples(samples),
      m_observer(observer),
      m_mirror(mirror),
      m_clip(100.f)
{
  m_engine = KX_GetActiveEngine();
  m_rasterizer = m_engine->GetRasterizer();
  m_canvas = m_engine->GetCanvas();

  m_internalFormat = GL_RGBA16F_ARB;

  // this constructor is used for automatic planar mirror
  // create a camera, take all data by default, in any case we will recompute the frustum on each
  // frame
  RAS_CameraData camdata;
  std::vector<RAS_IVertex *> mirrorVerts;
  std::vector<RAS_IVertex *>::iterator it;
  float mirrorArea = 0.f;
  float mirrorNormal[3] = {0.f, 0.f, 0.f};
  float mirrorUp[3];
  float dist, vec[3], axis[3];
  float zaxis[3] = {0.f, 0.f, 1.f};
  float yaxis[3] = {0.f, 1.f, 0.f};
  float mirrorMat[3][3];
  float left, right, top, bottom, back;
  // make sure this camera will delete its node
  m_camera = new KX_Camera();
  m_camera->SetScene(scene);
  m_camera->SetCameraData(camdata);
  m_camera->SetName("__mirror__cam__");
  m_camera->MarkForDeletion();

  // don't add the camera to the scene object list, it doesn't need to be accessible
  m_owncamera = true;
  // locate the vertex assigned to mat and do following calculation in mesh coordinates
  for (int meshIndex = 0; meshIndex < mirror->GetMeshCount(); meshIndex++) {
    RAS_MeshObject *mesh = mirror->GetMesh(meshIndex);
    int numPolygons = mesh->NumPolygons();
    for (int polygonIndex = 0; polygonIndex < numPolygons; polygonIndex++) {
      RAS_Polygon *polygon = mesh->GetPolygon(polygonIndex);
      if (polygon->GetMaterial()->GetPolyMaterial() == mat) {
        RAS_IVertex *v1, *v2, *v3, *v4;
        float normal[3];
        float area;
        // this polygon is part of the mirror
        v1 = polygon->GetVertex(0);
        v2 = polygon->GetVertex(1);
        v3 = polygon->GetVertex(2);
        mirrorVerts.push_back(v1);
        mirrorVerts.push_back(v2);
        mirrorVerts.push_back(v3);
        if (polygon->VertexCount() == 4) {
          v4 = polygon->GetVertex(3);
          mirrorVerts.push_back(v4);
          area = normal_quad_v3(normal,
                                (float *)v1->getXYZ(),
                                (float *)v2->getXYZ(),
                                (float *)v3->getXYZ(),
                                (float *)v4->getXYZ());
        }
        else {
          area = normal_tri_v3(
              normal, (float *)v1->getXYZ(), (float *)v2->getXYZ(), (float *)v3->getXYZ());
        }
        area = fabs(area);
        mirrorArea += area;
        mul_v3_fl(normal, area);
        add_v3_v3v3(mirrorNormal, mirrorNormal, normal);
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
  back = -FLT_MAX;  // most backward vertex (=highest Z coord in mirror space)
  for (it = mirrorVerts.begin(); it != mirrorVerts.end(); it++) {
    copy_v3_v3(vec, (float *)(*it)->getXYZ());
    mul_m3_v3(mirrorMat, vec);
    if (vec[0] < left)
      left = vec[0];
    if (vec[0] > right)
      right = vec[0];
    if (vec[1] < bottom)
      bottom = vec[1];
    if (vec[1] > top)
      top = vec[1];
    if (vec[2] > back)
      back = vec[2];
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
  m_mirrorPos.setValue(vec[0], vec[1], vec[2]);
  // mirror normal vector (pointed towards the back of the mirror) in local space
  m_mirrorZ.setValue(-mirrorNormal[0], -mirrorNormal[1], -mirrorNormal[2]);
  m_mirrorY.setValue(mirrorUp[0], mirrorUp[1], mirrorUp[2]);
  m_mirrorX = m_mirrorY.cross(m_mirrorZ);
  m_render = true;
}

// define python type
PyTypeObject ImageMirrorType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.ImageMirror", /*tp_name*/
    sizeof(PyImage),                                              /*tp_basicsize*/
    0,                                                            /*tp_itemsize*/
    (destructor)Image_dealloc,                                    /*tp_dealloc*/
    0,                                                            /*tp_print*/
    0,                                                            /*tp_getattr*/
    0,                                                            /*tp_setattr*/
    0,                                                            /*tp_compare*/
    0,                                                            /*tp_repr*/
    0,                                                            /*tp_as_number*/
    0,                                                            /*tp_as_sequence*/
    0,                                                            /*tp_as_mapping*/
    0,                                                            /*tp_hash */
    0,                                                            /*tp_call*/
    0,                                                            /*tp_str*/
    0,                                                            /*tp_getattro*/
    0,                                                            /*tp_setattro*/
    &imageBufferProcs,                                            /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                           /*tp_flags*/
    "Image source from mirror",                                   /* tp_doc */
    0,                                                            /* tp_traverse */
    0,                                                            /* tp_clear */
    0,                                                            /* tp_richcompare */
    0,                                                            /* tp_weaklistoffset */
    0,                                                            /* tp_iter */
    0,                                                            /* tp_iternext */
    imageRenderMethods,                                           /* tp_methods */
    0,                                                            /* tp_members */
    imageMirrorGetSets,                                           /* tp_getset */
    0,                                                            /* tp_base */
    0,                                                            /* tp_dict */
    0,                                                            /* tp_descr_get */
    0,                                                            /* tp_descr_set */
    0,                                                            /* tp_dictoffset */
    (initproc)ImageMirror_init,                                   /* tp_init */
    0,                                                            /* tp_alloc */
    Image_allocNew,                                               /* tp_new */
};
