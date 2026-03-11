/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/Texture.cpp
 *  \ingroup bgevideotex
 */

// implementation

#include "Texture.h"

#include "BKE_image.hh"
#include "DEG_depsgraph_query.hh"
#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_texture.hh"
#include "GPU_viewport.hh"
#include "gpu_shader_create_info.hh"
#ifdef WITH_PYTHON
#include "../python/gpu/gpu_py_texture.hh"
#endif

#include "ImageRender.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "RAS_IPolygonMaterial.h"
#ifdef WITH_FFMPEG
#include "VideoFFmpeg.h"
#endif

using namespace blender;

#ifdef WITH_FFMPEG
extern PyTypeObject VideoFFmpegType;
extern PyTypeObject ImageFFmpegType;
#endif
extern PyTypeObject ImageMixType;
extern PyTypeObject ImageViewportType;

/* Lazy-initialized YUV shaders — freed in FreeAllTextures(). */
static blender::gpu::Shader *g_yuv_shaders[3] = {nullptr, nullptr, nullptr};

static std::vector<Texture *> textures;

/* Lazy-initialized shader — one instance shared across all Texture objects.
 * Freed in FreeAllTextures() when the scene shuts down. */
static blender::gpu::Shader *g_yuv_to_rgb_shader = nullptr;

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
      m_ssboY(nullptr),
      m_ssboUV(nullptr),
      m_ssboV(nullptr),
      m_ssboWidth(0),
      m_ssboHeight(0),
      m_ssboYStride(0),
      m_ssboUVStride(0),
      m_ssboFmt(-1),
      m_mipmap(false),
      m_scaledImBuf(nullptr),
      m_lastClock(0.0),
      m_source(nullptr),
      m_isImageRender(false)
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
  /* Free the shared YUV->RGB compute shaders once all textures for this
   * scene have been released. Safe here because Close() no longer touches them. */
  for (int i = 0; i < 3; i++) {
    if (g_yuv_shaders[i]) {
      GPU_shader_free(g_yuv_shaders[i]);
      g_yuv_shaders[i] = nullptr;
    }
  }
}

void Texture::Close()
{
  if (m_orgSaved) {
    m_orgSaved = false;
  }
  if (m_origGpuTex) {
    m_imgTexture->runtime->gputexture[TEXTARGET_2D][0] = m_origGpuTex;
    m_origGpuTex = nullptr;
  }
  if (m_imgBuf) {
    blender::IMB_freeImBuf(m_imgBuf);
    m_imgBuf = nullptr;
  }
  if (m_modifiedGPUTexture) {
    GPU_texture_free(m_modifiedGPUTexture);
    m_modifiedGPUTexture = nullptr;
  }
  if (m_ssboY) {
    GPU_storagebuf_free(m_ssboY);
    m_ssboY = nullptr;
  }
  if (m_ssboUV) {
    GPU_storagebuf_free(m_ssboUV);
    m_ssboUV = nullptr;
  }
  if (m_ssboV) {
    GPU_storagebuf_free(m_ssboV);
    m_ssboV = nullptr;
  }
  m_ssboWidth = 0;
  m_ssboHeight = 0;
  m_ssboYStride = 0;
  m_ssboUVStride = 0;
  m_ssboFmt = -1;
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
  // Cache whether source is ImageRender to avoid dynamic_cast in the hot path every frame.
  m_isImageRender = (dynamic_cast<ImageRender *>(source->m_image) != nullptr);
}

/* -------------------------------------------------------------------- */
/** \name YUV → RGB compute shaders
 *
 * Three variants covering the most common pixel formats output by FFmpeg
 * hardware and software decoders:
 *
 *  NV12     — 8-bit semi-planar 4:2:0  (H.264/HEVC 8-bit, D3D11VA/VAAPI)
 *  P010     — 10-bit semi-planar 4:2:0 (HEVC HDR, D3D11VA)
 *  YUV420P  — 8-bit planar 4:2:0       (software decoder fallback)
 *
 * All use BT.601 limited-range, output RGBA8 via imageStore.
 * \{ */

static const char *yuv_nv12_compute_src = R"GLSL(
/* NV12: compact (no padding), stride == width */
#define READ_BYTE_Y(i)   ((y_plane[(i)/4u]  >> (((i)%4u)*8u)) & 0xFFu)
#define READ_BYTE_UV(i)  ((uv_plane[(i)/4u] >> (((i)%4u)*8u)) & 0xFFu)
void main() {
  uint idx = gl_GlobalInvocationID.x;
  uint w = uint(image_width), h = uint(image_height);
  if (idx >= w * h) return;
  uint x = idx % w, y = idx / w;
  float Y = float(READ_BYTE_Y(idx)) / 255.0;
  uint uv_off = (y/2u)*(w/2u)*2u + (x & ~1u);
  float U = float(READ_BYTE_UV(uv_off))    / 255.0 - 0.5;
  float V = float(READ_BYTE_UV(uv_off+1u)) / 255.0 - 0.5;
  float Yc = (Y - 16.0/255.0) * (255.0/219.0);
  float R = clamp(Yc + 1.596*V,           0.0, 1.0);
  float G = clamp(Yc - 0.391*U - 0.813*V, 0.0, 1.0);
  float B = clamp(Yc + 2.018*U,           0.0, 1.0);
  imageStore(rgba_img, ivec2(int(x), int(h-1u-y)), vec4(R, G, B, 1.0));
}
)GLSL";

static const char *yuv_p010_compute_src = R"GLSL(
/* P010: compact, each sample is uint16 in high 10 bits */
#define READ_U16_Y(i)   ((y_plane[(i)/2u]  >> (((i)%2u)*16u)) & 0xFFFFu)
#define READ_U16_UV(i)  ((uv_plane[(i)/2u] >> (((i)%2u)*16u)) & 0xFFFFu)
void main() {
  uint idx = gl_GlobalInvocationID.x;
  uint w = uint(image_width), h = uint(image_height);
  if (idx >= w * h) return;
  uint x = idx % w, y = idx / w;
  float Y = float(READ_U16_Y(idx) >> 6u) / 1023.0;
  uint uv_off = (y/2u)*(w/2u)*2u + (x & ~1u);
  float U = float(READ_U16_UV(uv_off)   >> 6u) / 1023.0 - 0.5;
  float V = float(READ_U16_UV(uv_off+1u)>> 6u) / 1023.0 - 0.5;
  float Yc = (Y - 64.0/1023.0) * (1023.0/877.0);
  float R = clamp(Yc + 1.596*V,           0.0, 1.0);
  float G = clamp(Yc - 0.391*U - 0.813*V, 0.0, 1.0);
  float B = clamp(Yc + 2.018*U,           0.0, 1.0);
  imageStore(rgba_img, ivec2(int(x), int(h-1u-y)), vec4(R, G, B, 1.0));
}
)GLSL";

static const char *yuv_420p_compute_src = R"GLSL(
/* YUV420P: compact, U in uv_plane, V in v_plane */
#define READ_BYTE_Y(i)  ((y_plane[(i)/4u]  >> (((i)%4u)*8u)) & 0xFFu)
#define READ_BYTE_U(i)  ((uv_plane[(i)/4u] >> (((i)%4u)*8u)) & 0xFFu)
#define READ_BYTE_V(i)  ((v_plane[(i)/4u]  >> (((i)%4u)*8u)) & 0xFFu)
void main() {
  uint idx = gl_GlobalInvocationID.x;
  uint w = uint(image_width), h = uint(image_height);
  if (idx >= w * h) return;
  uint x = idx % w, y = idx / w;
  uint uv_off = (y/2u)*(w/2u) + (x/2u);
  float Y = float(READ_BYTE_Y(idx))     / 255.0;
  float U = float(READ_BYTE_U(uv_off))  / 255.0 - 0.5;
  float V = float(READ_BYTE_V(uv_off))  / 255.0 - 0.5;
  float Yc = (Y - 16.0/255.0) * (255.0/219.0);
  float R = clamp(Yc + 1.596*V,           0.0, 1.0);
  float G = clamp(Yc - 0.391*U - 0.813*V, 0.0, 1.0);
  float B = clamp(Yc + 2.018*U,           0.0, 1.0);
  imageStore(rgba_img, ivec2(int(x), int(h-1u-y)), vec4(R, G, B, 1.0));
}
)GLSL";

/** \} */

enum class YUVFormat { NV12 = 0, P010 = 1, YUV420P = 2 };

static blender::gpu::Shader *get_yuv_shader(YUVFormat fmt)
{
  int idx = int(fmt);
  if (g_yuv_shaders[idx])
    return g_yuv_shaders[idx];

  using namespace blender::gpu::shader;
  const char *src = (fmt == YUVFormat::NV12)  ? yuv_nv12_compute_src :
                    (fmt == YUVFormat::P010)  ? yuv_p010_compute_src :
                                               yuv_420p_compute_src;
  ShaderCreateInfo info("pyGPU_Shader");
  info.local_group_size(256, 1, 1);
  info.compute_source_generated = src;
  info.storage_buf(0, Qualifier::read, "uint", "y_plane[]");
  info.storage_buf(1, Qualifier::read, "uint", "uv_plane[]");
  if (fmt == YUVFormat::YUV420P)
    info.storage_buf(2, Qualifier::read, "uint", "v_plane[]");
  info.image(0,
             blender::gpu::TextureFormat::UNORM_8_8_8_8,
             Qualifier::write,
             ImageReadWriteType::Float2D,
             "rgba_img");
  info.push_constant(Type::int_t, "image_width");
  info.push_constant(Type::int_t, "image_height");

  g_yuv_shaders[idx] = GPU_shader_create_from_info_python((GPUShaderCreateInfo *)&info, false);
  return g_yuv_shaders[idx];
}

// load texture
void Texture::loadTexture(unsigned int *texture,
                          short *size,
                          bool mipmap,
                          blender::gpu::TextureFormat format)
{
  // Check if the source is an ImageRender (offscreen 3D render)
  ImageRender *imr = m_isImageRender ? static_cast<ImageRender *>(m_source->m_image) : nullptr;

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

/* Upload NV12 planes to the GPU and run the YUV→RGB compute shader.
 * y_data  : Y plane, width*height bytes.
 * uv_data : interleaved UV plane, (width/2)*(height/2)*2 bytes.
 * The result lands directly in m_modifiedGPUTexture — no RGB CPU buffer needed. */
void Texture::loadTextureYUV(const uint8_t *y_data,
const uint8_t *uv_data,
const uint8_t *v_data,
int width,
int height,
int avPixFmt,
int y_linesize,
int uv_linesize,
int v_linesize)
{
  if (!m_imgTexture || !m_imgTexture->runtime->gputexture[TEXTARGET_2D][0])
    return;

  /* Select shader variant based on pixel format. */
  YUVFormat fmt;
  if (avPixFmt == AV_PIX_FMT_P010)
    fmt = YUVFormat::P010;
  else if (avPixFmt == AV_PIX_FMT_YUV420P)
    fmt = YUVFormat::YUV420P;
  else
    fmt = YUVFormat::NV12; /* NV12 and any other 8-bit semi-planar */

  blender::gpu::Shader *shader = get_yuv_shader(fmt);
  if (!shader)
    return;

  /* (Re)create output texture when size changes */
  if (m_modifiedGPUTexture && (width  != GPU_texture_width(m_modifiedGPUTexture) ||
                                height != GPU_texture_height(m_modifiedGPUTexture)))
  {
    GPU_texture_free(m_modifiedGPUTexture);
    m_modifiedGPUTexture = nullptr;
  }
  if (!m_modifiedGPUTexture) {
    m_modifiedGPUTexture = GPU_texture_create_2d(
        "videotexture_yuv", width, height, 1,
        blender::gpu::TextureFormat::UNORM_8_8_8_8,
        GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT,
        nullptr);
    if (!m_modifiedGPUTexture)
      return;
  }


  /* Compact the planes (remove FFmpeg row padding) into reusable CPU buffers so
   * the SSBOs are exactly width*height bytes with no padding — simplifies the shader. */
  const int uv_w        = width / 2;
  const int uv_h        = height / 2;
  const int uv_row_bytes = (fmt == YUVFormat::P010) ? uv_w * 4 :   /* 2 samples × 2 bytes */
                           (fmt == YUVFormat::NV12)  ? uv_w * 2 :  /* 2 interleaved bytes  */
                                                       uv_w;        /* YUV420P U-plane      */
  const int y_bytes  = width  * height;
  const int uv_bytes = uv_row_bytes * uv_h;
  const int v_bytes  = (fmt == YUVFormat::YUV420P) ? uv_w * uv_h : 0;

  /* Resize staging vectors only when needed (capacity never shrinks). */
  if ((int)m_yCompact.size()  < y_bytes)  m_yCompact.resize(y_bytes);
  if ((int)m_uvCompact.size() < uv_bytes) m_uvCompact.resize(uv_bytes);
  if (v_bytes > 0 && (int)m_vCompact.size() < v_bytes) m_vCompact.resize(v_bytes);

  /* Compact Y plane. */
  if (y_linesize == width) {
    std::memcpy(m_yCompact.data(), y_data, y_bytes);
  }
  else {
    for (int row = 0; row < height; row++)
      std::memcpy(m_yCompact.data() + row * width, y_data + row * y_linesize, width);
  }

  /* Compact UV / U plane. */
  if (uv_linesize == uv_row_bytes) {
    std::memcpy(m_uvCompact.data(), uv_data, uv_bytes);
  }
  else {
    for (int row = 0; row < uv_h; row++)
      std::memcpy(m_uvCompact.data() + row * uv_row_bytes,
                  uv_data + row * uv_linesize,
                  uv_row_bytes);
  }

  /* Compact V plane (YUV420P only) — data[2] is independent of data[1]. */
  if (fmt == YUVFormat::YUV420P && v_data) {
    if (v_linesize == uv_w) {
      std::memcpy(m_vCompact.data(), v_data, v_bytes);
    }
    else {
      for (int row = 0; row < uv_h; row++)
        std::memcpy(m_vCompact.data() + row * uv_w, v_data + row * v_linesize, uv_w);
    }
  }

  /* Recreate SSBOs if resolution OR YUV format changes (different format → different sizes). */
  if (m_ssboY && (width != m_ssboWidth || height != m_ssboHeight || avPixFmt != m_ssboFmt)) {
    GPU_storagebuf_free(m_ssboY);   m_ssboY  = nullptr;
    GPU_storagebuf_free(m_ssboUV);  m_ssboUV = nullptr;
    if (m_ssboV) { GPU_storagebuf_free(m_ssboV); m_ssboV = nullptr; }
  }
  if (!m_ssboY) {
    m_ssboY  = GPU_storagebuf_create_ex(y_bytes,  nullptr, GPU_USAGE_STREAM, "yuv_y_plane");
    m_ssboUV = GPU_storagebuf_create_ex(uv_bytes, nullptr, GPU_USAGE_STREAM, "yuv_uv_plane");
    if (fmt == YUVFormat::YUV420P)
      m_ssboV = GPU_storagebuf_create_ex(v_bytes, nullptr, GPU_USAGE_STREAM, "yuv_v_plane");
    m_ssboWidth    = width;
    m_ssboHeight   = height;
    m_ssboYStride  = width;
    m_ssboUVStride = uv_row_bytes;
    m_ssboFmt      = avPixFmt;
  }
  GPU_storagebuf_update(m_ssboY,  m_yCompact.data());
  GPU_storagebuf_update(m_ssboUV, m_uvCompact.data());
  if (fmt == YUVFormat::YUV420P && m_ssboV)
    GPU_storagebuf_update(m_ssboV, m_vCompact.data());

  const gpu::shader::SpecializationConstants *spec_consts = &GPU_shader_get_default_constant_state(shader);
  GPU_shader_bind(shader, spec_consts);
  GPU_storagebuf_bind(m_ssboY,  GPU_shader_get_ssbo_binding(shader, "y_plane"));
  GPU_storagebuf_bind(m_ssboUV, GPU_shader_get_ssbo_binding(shader, "uv_plane"));
  if (fmt == YUVFormat::YUV420P && m_ssboV)
    GPU_storagebuf_bind(m_ssboV, GPU_shader_get_ssbo_binding(shader, "v_plane"));
  /* Bind the output texture as a write image — imageStore writes directly into it */
  GPU_texture_image_bind(m_modifiedGPUTexture,
                         GPU_shader_get_sampler_binding(shader, "rgba_img"));
  GPU_shader_uniform_1i(shader, "image_width",  width);
  GPU_shader_uniform_1i(shader, "image_height", height);

  const int groups = (width * height + 255) / 256;
  GPU_compute_dispatch(shader, groups, 1, 1, spec_consts);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);

  GPU_texture_image_unbind(m_modifiedGPUTexture);
  GPU_shader_unbind();
  GPU_storagebuf_unbind(m_ssboY);
  GPU_storagebuf_unbind(m_ssboUV);
  if (fmt == YUVFormat::YUV420P && m_ssboV)
    GPU_storagebuf_unbind(m_ssboV);
  /* SSBOs are persistent — do not free here, freed in Close(). */

  if (!m_origGpuTex) {
    m_origGpuTex = m_imgTexture->runtime->gputexture[TEXTARGET_2D][0];
  }
  m_imgTexture->runtime->gputexture[TEXTARGET_2D][0] = m_modifiedGPUTexture;
  if (!m_py_color) {
    m_py_color = BPyGPUTexture_CreatePyObject(
        m_imgTexture->runtime->gputexture[TEXTARGET_2D][0], false);
    Py_INCREF(m_py_color);
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

        // get texture — also triggers calcImage which sets m_lastFrameIsNV12 if HW path
        unsigned int *texture = m_source->m_image->getImage(0, ts);

#ifdef WITH_FFMPEG
        /* NV12 fast path: bypass sws_scale RGB buffer and upload YUV planes directly
         * to the GPU via the compute shader. getImage() has already called calcImage()
         * which decoded the frame; m_lastFrameNV12 points to the per-slot NV12 AVFrame. */
        if (PyObject_TypeCheck(&m_source->ob_base, &VideoFFmpegType)) {
          VideoFFmpeg *vid = getFFmpeg(m_source);
          if (vid->getLastFrameIsNV12() && vid->getLastFrameNV12()) {
            const AVFrame *f = vid->getLastFrameNV12();
            /* For NV12/P010: data[0]=Y, data[1]=UV interleaved.
             * For YUV420P:   data[0]=Y, data[1]=U, data[2]=V.
             * loadTextureYUV handles all three via avPixFmt routing. */
            loadTextureYUV(f->data[0],
                           f->data[1],
                           f->data[2],
                           f->width,
                           f->height,
                           f->format,
                           f->linesize[0],
                           f->linesize[1],
                           f->linesize[2]);
            vid->clearLastFrameNV12();
            if (refreshSource)
              m_source->m_image->refresh();
            goto done_loading;
          }
        }
#endif

        // if texture is available (RGB path)
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
#ifdef WITH_FFMPEG
        done_loading:;
#endif
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
