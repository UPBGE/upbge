/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/Texture.cpp
 *  \ingroup bgevideotex
 */

// implementation

#include "Texture.h"

#include "BKE_image.hh"
#include "DEG_depsgraph_query.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_viewport.hh"
#ifdef WITH_PYTHON
#include "../python/gpu/gpu_py_texture.hh"
#endif

#include "ImageRender.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "RAS_IPolygonMaterial.h"

using namespace blender;

#ifdef WITH_FFMPEG
extern PyTypeObject VideoFFmpegType;
extern PyTypeObject ImageFFmpegType;
#endif
extern PyTypeObject ImageMixType;
extern PyTypeObject ImageViewportType;

static std::vector<Texture *> textures;

// macro for exception handling and logging
#define CATCH_EXCP \
  catch (Exception & exp) \
  { \
    exp.report(); \
    return nullptr; \
  }

PyObject *Texture_close(Texture *self);

Texture::Texture():
      m_orgTex(0),
      m_orgImg(nullptr),
      m_orgSaved(false),
      m_imgBuf(nullptr),
      m_imgTexture(nullptr),
      m_matTexture(nullptr),
      m_scene(nullptr),
      m_gameobj(nullptr),
      m_origGpuTex(nullptr),
      m_modifiedGPUTexture(nullptr),
      m_py_color(nullptr),
      m_mipmap(false),
      m_scaledImBuf(nullptr),
      m_lastClock(0.0),
      m_source(nullptr)
{
  textures.push_back(this);
}

Texture::~Texture()
{
  // release renderer
  Py_XDECREF(m_source);
  // close texture
  Close();
  // release scaled image buffer
  blender::IMB_freeImBuf(m_scaledImBuf);
}

void Texture::DestructFromPython()
{
  std::vector<Texture *>::iterator it = std::find(textures.begin(), textures.end(), this);
  if (it != textures.end()) {
    textures.erase(it);
  }

  EXP_PyObjectPlus::DestructFromPython();
}

std::string Texture::GetName()
{
  return "Texture";
}

void Texture::FreeAllTextures(KX_Scene *scene)
{
  for (std::vector<Texture *>::iterator it = textures.begin(); it != textures.end();) {
    Texture *texture = *it;
    if (texture->m_scene != scene) {
      ++it;
      continue;
    }

    it = textures.erase(it);
    texture->Release();
  }
}

void Texture::Close()
{
  if (m_orgSaved) {
    m_orgSaved = false;
  }
  if (m_origGpuTex) {
    m_imgTexture->runtime->gputexture[TEXTARGET_2D][0] = m_origGpuTex;
  }
  if (m_imgBuf) {
    blender::IMB_freeImBuf(m_imgBuf);
    m_imgBuf = nullptr;
  }
  if (m_modifiedGPUTexture) {
    GPU_texture_free(m_modifiedGPUTexture);
    m_modifiedGPUTexture = nullptr;
  }
  if (m_py_color) {
    Py_XDECREF(m_py_color);
    m_py_color = nullptr;
  }
}

void Texture::SetSource(PyImage *source)
{
  BLI_assert(source != nullptr);
  Py_XDECREF(m_source);
  Py_INCREF(source);
  m_source = source;
}

// load texture
void Texture::loadTexture(unsigned int *texture,
                          short *size,
                          bool mipmap,
                          blender::gpu::TextureFormat format)
{
  // Check if the source is an ImageRender (offscreen 3D render)
  ImageRender *imr = dynamic_cast<ImageRender *>(m_source ? m_source->m_image : nullptr);
  if (imr && !m_origGpuTex) {
    // For ImageRender, directly use the GPU texture from the active framebuffer
    KX_Camera *cam = imr->GetCamera();
    if (cam && m_imgTexture && m_imgTexture->runtime->gputexture[TEXTARGET_2D][0]) {
      blender::GPUViewport *viewport = cam->GetGPUViewport();
      // Get the color texture from the viewport's framebuffer
      blender::gpu::Texture *gpuTex = GPU_viewport_color_texture(viewport, 0);
      // Assign the GPU texture to the Blender image slot
      m_origGpuTex = m_imgTexture->runtime->gputexture[TEXTARGET_2D][0];
      m_imgTexture->runtime->gputexture[TEXTARGET_2D][0] = gpuTex;
      m_py_color = BPyGPUTexture_CreatePyObject(m_imgTexture->runtime->gputexture[TEXTARGET_2D][0],
                                                false);
      Py_INCREF(m_py_color);
    }
    // No need to upload a CPU buffer, return early
    return;
  }

  // For video/image sources: upload the CPU buffer to a GPU texture
  if (m_imgTexture && m_imgTexture->runtime->gputexture[TEXTARGET_2D][0]) {
    if (m_modifiedGPUTexture && (size[0] != GPU_texture_width(m_modifiedGPUTexture) ||
                                 size[1] != GPU_texture_height(m_modifiedGPUTexture)))
    {
      GPU_texture_free(m_modifiedGPUTexture);
      m_modifiedGPUTexture = nullptr;
    }
    if (!m_modifiedGPUTexture) {
      // Create the GPU texture if not already done
      m_modifiedGPUTexture = GPU_texture_create_2d("videotexture",
                                                   size[0],
                                                   size[1],
                                                   1,
                                                   blender::gpu::TextureFormat::UNORM_8_8_8_8,
                                                   GPU_TEXTURE_USAGE_SHADER_READ |
                                                       GPU_TEXTURE_USAGE_ATTACHMENT,
                                                   nullptr);
    }

    // Upload the RGBA8 buffer to the GPU texture
    GPU_texture_update(m_modifiedGPUTexture, GPU_DATA_UBYTE, texture);

    // Optionally update mipmaps
    if (mipmap) {
      GPU_texture_update_mipmap_chain(m_modifiedGPUTexture);
    }
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
    if (!m_origGpuTex) {
      m_origGpuTex = m_imgTexture->runtime->gputexture[TEXTARGET_2D][0];
    }
    // Integrate the new GPU texture into the Blender pipeline
    m_imgTexture->runtime->gputexture[TEXTARGET_2D][0] = m_modifiedGPUTexture;
    if (!m_py_color) {
      m_py_color = BPyGPUTexture_CreatePyObject(m_imgTexture->runtime->gputexture[TEXTARGET_2D][0],
                                                false);
      Py_INCREF(m_py_color);
    }
  }
}

// get pointer to material
RAS_IPolyMaterial *getMaterial(KX_GameObject *gameObj, short matID)
{
  // get pointer to texture image
  if (gameObj->GetMeshCount() > 0) {
    // get material from mesh
    RAS_MeshObject *mesh = gameObj->GetMesh(0);
    RAS_MeshMaterial *meshMat = mesh->GetMeshMaterial(matID);
    if (meshMat != nullptr && meshMat->GetBucket() != nullptr)
      // return pointer to polygon or blender material
      return meshMat->GetBucket()->GetPolyMaterial();
  }

  // otherwise material was not found
  return nullptr;
}

// get material blender::ID
short getMaterialID(PyObject *obj, const char *name)
{
  // search for material
  for (short matID = 0;; ++matID) {
    // get material
    KX_GameObject *gameObj;
    if (!ConvertPythonToGameObject(
            KX_GetActiveScene()->GetLogicManager(), obj, &gameObj, false, "")) {
      break;
    }

    RAS_IPolyMaterial *mat = getMaterial(gameObj, matID);
    // if material is not available, report that no material was found
    if (mat == nullptr)
      break;
    // name is a material name if it starts with MA and a UV texture name if it starts with IM
    if (name[0] == 'I' && name[1] == 'M') {
      // if texture name matches
      if (mat->GetTextureName() == name)
        return matID;
    }
    else {
      // if material name matches
      if (mat->GetName() == name)
        return matID;
    }
  }
  // material was not found
  return -1;
}

// Texture object allocation
static PyObject *Texture_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  // allocate object
  Texture *self = new Texture();

  // return allocated object
  return self->NewProxy(true);
}

ExceptionID MaterialNotAvail;
ExpDesc MaterialNotAvailDesc(MaterialNotAvail, "Texture material is not available");

ExceptionID TextureNotAvail;
ExpDesc TextureNotAvailDesc(TextureNotAvail, "Texture is not available");

// Texture object initialization
static int Texture_init(PyObject *self, PyObject *args, PyObject *kwds)
{
  if (!EXP_PROXY_PYREF(self)) {
    return -1;
  }

  Texture *tex = (Texture *)EXP_PROXY_REF(self);

  // parameters - game object with video texture
  PyObject *obj = nullptr;
  // material blender::ID
  short matID = 0;
  // texture blender::ID
  short texID = 0;
  // texture object with shared texture blender::ID
  Texture *texObj = nullptr;

  static const char *kwlist[] = {"gameObj", "materialID", "textureID", "textureObj", nullptr};

  // get parameters
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "O|hhO!",
                                   const_cast<char **>(kwlist),
                                   &obj,
                                   &matID,
                                   &texID,
                                   &Texture::Type,
                                   &texObj))
  {
    return -1;
  }

  KX_GameObject *gameObj = nullptr;
  if (ConvertPythonToGameObject(
          KX_GetActiveScene()->GetLogicManager(), obj, &gameObj, false, "")) {
    // process polygon material or blender material
    try {
      tex->m_gameobj = gameObj;
      tex->m_scene = gameObj->GetScene();
      // get pointer to texture image
      RAS_IPolyMaterial *mat = getMaterial(gameObj, matID);
      KX_LightObject *lamp = nullptr;
      if (gameObj->GetGameObjectType() == SCA_IObject::OBJ_LIGHT) {
        lamp = (KX_LightObject *)gameObj;
      }

      if (mat != nullptr) {
        // get blender material texture
        tex->m_matTexture = mat->GetTexture(texID);
        if (!tex->m_matTexture) {
          THRWEXCP(TextureNotAvail, S_OK);
        }
        tex->m_imgTexture = tex->m_matTexture->GetImage();
        tex->m_useMatTexture = true;
      }
      else if (lamp != nullptr) {
        // tex->m_imgTexture = lamp->GetLightData()->GetTextureImage(texID);
        // tex->m_useMatTexture = false;
      }

      // check if texture is available, if not, initialization failed
      if (tex->m_imgTexture == nullptr && tex->m_matTexture == nullptr)
        // throw exception if initialization failed
        THRWEXCP(MaterialNotAvail, S_OK);

      // if texture object is provided
      if (texObj != nullptr) {
        tex->m_mipmap = texObj->m_mipmap;
        if (texObj->m_source != nullptr)
          tex->SetSource(texObj->m_source);
      }
    }
    catch (Exception &exp) {
      exp.report();
      return -1;
    }
  }
  // initialization succeded
  return 0;
}

// close added texture
EXP_PYMETHODDEF_DOC(Texture, close, "Close dynamic texture and restore original")
{
  // restore texture
  Close();
  Py_RETURN_NONE;
}

// refresh texture
EXP_PYMETHODDEF_DOC(Texture, refresh, "Refresh texture from source")
{
  // get parameter - refresh source
  PyObject *param;
  double ts = -1.0;

  if (!PyArg_ParseTuple(args, "O|d:refresh", &param, &ts) || !PyBool_Check(param)) {
    // report error
    PyErr_SetString(PyExc_TypeError, "The value must be a bool");
    return nullptr;
  }
  // some trick here: we are in the business of loading a texture,
  // no use to do it if we are still in the same rendering frame.
  // We find this out by looking at the engine current clock time
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  if (engine->GetClockTime() != m_lastClock) {
    m_lastClock = engine->GetClockTime();
    // set source refresh
    bool refreshSource = (param == Py_True);
    // try to proces texture from source
    try {
      // if source is available
      if (m_source != nullptr) {
        // check texture code
        if (!m_orgSaved) {
          m_orgSaved = true;
          if (m_useMatTexture) {
            m_orgImg = m_matTexture->GetImage();
            if (m_imgTexture) {
              m_orgImg = m_imgTexture;
            }
          }
          else {
            // Swapping will work only if the GPU has already loaded the image.
            // If not, it will delete and overwrite our texture on next render.
            // To avoid that, we acquire the image buffer now.
            // WARNING: GPU has a ImageUser to pass, we don't. Using nullptr
            // works on image file, not necessarily on other type of image.
            m_imgBuf = BKE_image_acquire_ibuf(m_imgTexture, nullptr, nullptr);
            m_orgImg = m_imgTexture;
          }
        }

        // get texture
        unsigned int *texture = m_source->m_image->getImage(0, ts);
        // if texture is available
        if (texture != nullptr) {
          // get texture size
          short *orgSize = m_source->m_image->getSize();
          // calc scaled sizes
          short size[2];
          if (0) {
            size[0] = orgSize[0];
            size[1] = orgSize[1];
          }
          else {
            size[0] = ImageBase::calcSize(orgSize[0]);
            size[1] = ImageBase::calcSize(orgSize[1]);
          }
          // scale texture if needed
          if (size[0] != orgSize[0] || size[1] != orgSize[1]) {
            blender::IMB_freeImBuf(m_scaledImBuf);
            m_scaledImBuf = blender::IMB_allocFromBuffer((uint8_t *)texture, nullptr, orgSize[0], orgSize[1], 4);
            blender::IMB_scale(m_scaledImBuf, size[0], size[1], IMBScaleFilter::Box, false);
            // use scaled image instead original
            texture = (unsigned int *)m_scaledImBuf->byte_buffer.data;
          }
          // load texture for rendering
          loadTexture(texture,
              size,
              m_mipmap,
              m_source->m_image->GetInternalFormat());
        }
        // refresh texture source, if required
        if (refreshSource) {
          m_source->m_image->refresh();
        }
      }

      /* Add a depsgraph notifier to trigger
       * an update on next draw loop (depsgraph_last_update_ != DEG_get_update_count(depsgraph))
       * for some VideoTexture types (types which have a
       * "refresh" method), because the depsgraph has not been warned yet. */
      bool needs_notifier = m_source && (
#ifdef WITH_FFMPEG
                            PyObject_TypeCheck(&m_source->ob_base, &VideoFFmpegType) ||
                                PyObject_TypeCheck(&m_source->ob_base, &ImageFFmpegType) ||
#endif  // WITH_FFMPEG
                                PyObject_TypeCheck(&m_source->ob_base, &ImageMixType) ||
                                PyObject_TypeCheck(&m_source->ob_base, &ImageViewportType));
      if (needs_notifier) {
        /* This update notifier will be flushed next time
         * BKE_scene_graph_update_tagged will be called */
        DEG_id_tag_update(&m_gameobj->GetBlenderObject()->id, ID_RECALC_TRANSFORM);
      }
    }
    CATCH_EXCP;
  }
  Py_RETURN_NONE;
}

// get gputexture
PyObject *Texture::pyattr_get_gputexture(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  Texture *self = static_cast<Texture *>(self_v);
  blender::gpu::Texture *gputex = self->m_imgTexture->runtime->gputexture[TEXTARGET_2D][0];
  if (gputex) {
    return BPyGPUTexture_CreatePyObject(gputex, true);
  }
  Py_RETURN_NONE;
}

// get mipmap value
PyObject *Texture::pyattr_get_mipmap(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  Texture *self = (Texture *)self_v;

  // return true if flag is set, otherwise false
  if (self->m_mipmap)
    Py_RETURN_TRUE;
  else
    Py_RETURN_FALSE;
}

// set mipmap value
int Texture::pyattr_set_mipmap(EXP_PyObjectPlus *self_v,
                               const EXP_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value)
{
  Texture *self = (Texture *)self_v;

  // check parameter, report failure
  if (value == nullptr || !PyBool_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "The value must be a bool");
    return -1;
  }
  // set mipmap
  self->m_mipmap = value == Py_True;
  // success
  return 0;
}

// get source object
PyObject *Texture::pyattr_get_source(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  Texture *self = (Texture *)self_v;

  // if source exists
  if (self->m_source != nullptr) {
    Py_INCREF(self->m_source);
    return reinterpret_cast<PyObject *>(self->m_source);
  }
  // otherwise return None
  Py_RETURN_NONE;
}

// set source object
int Texture::pyattr_set_source(EXP_PyObjectPlus *self_v,
                               const EXP_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value)
{
  Texture *self = (Texture *)self_v;
  // check new value
  if (value == nullptr || !pyImageTypes.in(Py_TYPE(value))) {
    // report value error
    PyErr_SetString(PyExc_TypeError, "Invalid type of value");
    return -1;
  }
  PyImage *pyimg = reinterpret_cast<PyImage *>(value);
  self->SetSource(pyimg);
  ImageRender *imgRender = dynamic_cast<ImageRender *>(pyimg->m_image);
  if (imgRender) {
    imgRender->SetTexture(self);
  }
  // return success
  return 0;
}

// class Texture methods
PyMethodDef Texture::Methods[] = {
    EXP_PYMETHODTABLE(Texture, close),
    EXP_PYMETHODTABLE(Texture, refresh),
    {nullptr, nullptr}  // Sentinel
};

// class Texture attributes
PyAttributeDef Texture::Attributes[] = {
    EXP_PYATTRIBUTE_RW_FUNCTION("mipmap", Texture, pyattr_get_mipmap, pyattr_set_mipmap),
    EXP_PYATTRIBUTE_RW_FUNCTION("source", Texture, pyattr_get_source, pyattr_set_source),
    EXP_PYATTRIBUTE_RO_FUNCTION("gpuTexture", Texture, pyattr_get_gputexture),
    EXP_PYATTRIBUTE_NULL};

// class Texture declaration
PyTypeObject Texture::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.Texture",
                              sizeof(EXP_PyObjectPlus_Proxy),
                              0,
                              py_base_dealloc,
                              0,
                              0,
                              0,
                              0,
                              py_base_repr,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              &imageBufferProcs,
                              Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              Methods,
                              0,
                              0,
                              &EXP_PyObjectPlus::Type,
                              0,
                              0,
                              0,
                              0,
                              (initproc)Texture_init,
                              0,
                              Texture_new};
