/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file gameengine/Ketsji/KX_CullingGateOcclusion.cpp
 *  \ingroup ketsji
 *
 * GPU occlusion for the CPU culling gate.
 *
 * After the active camera pass:
 *   1. A max Hi-Z pyramid of the pass depth is built (farthest depth per
 *      texel, standard depth: 0 near, 1 far). Level 0 is half resolution and
 *      level sizes round up, so every depth pixel is covered by every level.
 *   2. Every gate box is projected and its nearest depth compared with the
 *      farthest depth of the 2x2 texels of the level where its screen
 *      rectangle spans at most two texels.
 *   3. The visibility words are copied to the host with the non-blocking
 *      GPU_storagebuf_read_if_ready(): the CPU reads the copy of an earlier
 *      frame, never waits for the current one.
 *
 * The results are consumed at least two frames after the depth was
 * rendered, so they are only trusted when the camera has not turned or
 * moved more than the boxes were expanded for, and an object needs two
 * consecutive occluded results to be culled.
 */

#include "KX_CullingGate.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "GPU_compute.hh"
#include "GPU_context.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_texture.hh"
#include "gpu/intern/gpu_shader_create_info.hh"

#include "KX_Camera.h"

using namespace blender;
using namespace blender::gpu::shader;

/* Pyramid levels kept: level L texels span 2^(L+1) depth pixels. A box wider
 * than the last level is too large to be worth testing. */
static constexpr int OCC_MAX_LEVELS = 8;
static constexpr int OCC_GROUP_2D = 8;
static constexpr int OCC_GROUP_1D = 64;
/* Results older than this many frames are ignored. */
static constexpr uint32_t OCC_MAX_AGE = 4;
/* Camera rotation since the tested depth above which results are ignored:
 * cos(10 degrees). */
static constexpr float OCC_MIN_FWD_DOT = 0.98480775f;
/* Boxes are tested inflated: relative and absolute world margin, plus the
 * distance the camera would cover in the latency at its last speed. */
static constexpr float OCC_EXPAND = 1.05f;
static constexpr float OCC_MARGIN = 0.05f;
static constexpr float OCC_LATENCY_FRAMES = 3.0f;
static constexpr float OCC_MAX_SPEED_MARGIN = 2.0f;

/* Rectangle of level m in the atlas: level 0 on the left, the next levels
 * stacked on its right. */
static const char *OCC_GLSL_LEVEL_RECT = R"GLSL(
ivec4 level_rect(int m)
{
  ivec2 size = hiz0_size;
  if (m == 0) {
    return ivec4(0, 0, size);
  }
  ivec2 offset = ivec2(hiz0_size.x, 0);
  for (int k = 1; k <= m; k++) {
    size = max((size + 1) / 2, ivec2(1));
    if (k < m) {
      offset.y += size.y;
    }
  }
  return ivec4(offset, size);
}
)GLSL";

static const char *OCC_GLSL_HIZ = R"GLSL(
void main()
{
  ivec2 px = ivec2(gl_GlobalInvocationID.xy);
  ivec4 dst = level_rect(level);
  if (any(greaterThanEqual(px, dst.zw))) {
    return;
  }
  /* Clamped 2x2 footprint: with rounded up sizes the clamp folds the odd
   * last row / column of the source in, the reduction stays conservative. */
  ivec2 s = px * 2;
  float d;
  if (level == 0) {
    ivec2 smax = depth_size - 1;
    d = max(max(texelFetch(depth_tx, min(s, smax), 0).r,
                texelFetch(depth_tx, min(s + ivec2(1, 0), smax), 0).r),
            max(texelFetch(depth_tx, min(s + ivec2(0, 1), smax), 0).r,
                texelFetch(depth_tx, min(s + ivec2(1, 1), smax), 0).r));
  }
  else {
    ivec4 src = level_rect(level - 1);
    ivec2 smax = src.zw - 1;
    d = max(max(imageLoad(hiz_img, src.xy + min(s, smax)).r,
                imageLoad(hiz_img, src.xy + min(s + ivec2(1, 0), smax)).r),
            max(imageLoad(hiz_img, src.xy + min(s + ivec2(0, 1), smax)).r,
                imageLoad(hiz_img, src.xy + min(s + ivec2(1, 1), smax)).r));
  }
  imageStore(hiz_img, dst.xy + px, vec4(d));
}
)GLSL";

static const char *OCC_GLSL_TEST = R"GLSL(
bool box_visible(vec3 c, vec3 e)
{
  if (any(greaterThan(e, vec3(1e20)))) {
    return true;
  }
  vec2 smin = vec2(1e30);
  vec2 smax = vec2(-1e30);
  float zmin = 1e30;
  for (int k = 0; k < 8; k++) {
    vec3 sgn = vec3(((k & 1) != 0) ? 1.0 : -1.0,
                    ((k & 2) != 0) ? 1.0 : -1.0,
                    ((k & 4) != 0) ? 1.0 : -1.0);
    vec4 h = persmat * vec4(c + e * sgn, 1.0);
    if (h.w <= 1e-5) {
      /* Reaches the camera plane: nothing in front can be proven. */
      return true;
    }
    vec3 ndc = h.xyz / h.w;
    smin = min(smin, ndc.xy);
    smax = max(smax, ndc.xy);
    zmin = min(zmin, ndc.z);
  }
  if (any(lessThan(smax, vec2(-1.0))) || any(greaterThan(smin, vec2(1.0)))) {
    /* Off screen: frustum culling is the CPU business. */
    return true;
  }
  float box_depth = zmin * 0.5 + 0.5;
  if (box_depth <= 0.0) {
    return true;
  }
  /* Depth pixel rectangle, one extra pixel for the TAA jitter. */
  vec2 pmin = clamp((smin * 0.5 + 0.5) * depth_size - 1.0, vec2(0.0), depth_size);
  vec2 pmax = clamp((smax * 0.5 + 0.5) * depth_size + 1.0, vec2(0.0), depth_size);
  vec2 span = max(pmax - pmin, vec2(1.0));
  /* Level whose texels (2^(m+1) pixels) are at least the span: the
   * rectangle then touches at most 2x2 texels. */
  int m = max(0, int(ceil(log2(max(span.x, span.y)))) - 1);
  if (m >= level_count) {
    return true;
  }
  float texel = exp2(float(m + 1));
  ivec4 r = level_rect(m);
  ivec2 t0 = clamp(ivec2(floor(pmin / texel)), ivec2(0), r.zw - 1);
  ivec2 t1 = clamp(ivec2(floor(pmax / texel)), ivec2(0), r.zw - 1);
  float occluder = max(max(imageLoad(hiz_img, r.xy + t0).r,
                           imageLoad(hiz_img, r.xy + ivec2(t1.x, t0.y)).r),
                       max(imageLoad(hiz_img, r.xy + ivec2(t0.x, t1.y)).r,
                           imageLoad(hiz_img, r.xy + t1).r));
  return box_depth <= occluder + 1e-6;
}

void main()
{
  int i = int(gl_GlobalInvocationID.x);
  if (i >= count) {
    return;
  }
  vec3 c = vec3(bounds[i], bounds[i + stride], bounds[i + 2 * stride]);
  vec3 e = vec3(bounds[i + 3 * stride], bounds[i + 4 * stride], bounds[i + 5 * stride]);
  visibility[i] = box_visible(c, e * expand + vec3(margin)) ? 1u : 0u;
}
)GLSL";

static gpu::Shader *create_hiz_shader()
{
  ShaderCreateInfo info("pyGPU_Shader");
  info.local_group_size(OCC_GROUP_2D, OCC_GROUP_2D, 1);
  info.sampler(0, ImageType::Depth2D, "depth_tx");
  info.image(0,
             gpu::TextureFormat::SFLOAT_32,
             Qualifier::read_write,
             ImageReadWriteType::Float2D,
             "hiz_img");
  info.push_constant(gpu::shader::Type::int_t, "level");
  info.push_constant(gpu::shader::Type::int2_t, "hiz0_size");
  info.push_constant(gpu::shader::Type::int2_t, "depth_size");
  info.compute_source_generated = std::string(OCC_GLSL_LEVEL_RECT) + OCC_GLSL_HIZ;
  return GPU_shader_create_from_info_python(reinterpret_cast<GPUShaderCreateInfo *>(&info),
                                            false);
}

static gpu::Shader *create_test_shader()
{
  ShaderCreateInfo info("pyGPU_Shader");
  info.local_group_size(OCC_GROUP_1D, 1, 1);
  info.image(0,
             gpu::TextureFormat::SFLOAT_32,
             Qualifier::read,
             ImageReadWriteType::Float2D,
             "hiz_img");
  info.storage_buf(0, Qualifier::read, "float", "bounds[]");
  info.storage_buf(1, Qualifier::write, "uint", "visibility[]");
  info.push_constant(gpu::shader::Type::float4x4_t, "persmat");
  info.push_constant(gpu::shader::Type::int_t, "count");
  info.push_constant(gpu::shader::Type::int_t, "stride");
  info.push_constant(gpu::shader::Type::int_t, "level_count");
  info.push_constant(gpu::shader::Type::int2_t, "hiz0_size");
  info.push_constant(gpu::shader::Type::float2_t, "depth_size");
  info.push_constant(gpu::shader::Type::float_t, "expand");
  info.push_constant(gpu::shader::Type::float_t, "margin");
  info.compute_source_generated = std::string(OCC_GLSL_LEVEL_RECT) + OCC_GLSL_TEST;
  return GPU_shader_create_from_info_python(reinterpret_cast<GPUShaderCreateInfo *>(&info),
                                            false);
}

void KX_CullingGate::FreeGPUResources()
{
  if (m_occHizShader) {
    GPU_shader_free(m_occHizShader);
    m_occHizShader = nullptr;
  }
  if (m_occTestShader) {
    GPU_shader_free(m_occTestShader);
    m_occTestShader = nullptr;
  }
  if (m_occBoundsBuf) {
    GPU_storagebuf_free(m_occBoundsBuf);
    m_occBoundsBuf = nullptr;
  }
  if (m_occResultBuf) {
    GPU_storagebuf_free(m_occResultBuf);
    m_occResultBuf = nullptr;
  }
  if (m_occHizTex) {
    GPU_texture_free(m_occHizTex);
    m_occHizTex = nullptr;
  }
  m_occCapacity = 0;
  m_occDepthSize[0] = m_occDepthSize[1] = 0;
  m_occInflight.valid = false;
  m_occLatest.valid = false;
  m_occMoveLog.clear();
}

static void camera_view(KX_Camera *cam, float r_pos[3], float r_fwd[3], float r_proj[16])
{
  const MT_Vector3 pos = cam->NodeGetWorldPosition();
  /* Cameras look down their local -Z axis. */
  const MT_Vector3 z = cam->NodeGetWorldOrientation().getColumn(2);
  for (int k = 0; k < 3; k++) {
    r_pos[k] = pos[k];
    r_fwd[k] = -z[k];
  }
  const float len = sqrtf(r_fwd[0] * r_fwd[0] + r_fwd[1] * r_fwd[1] + r_fwd[2] * r_fwd[2]);
  if (len > 0.0f) {
    for (int k = 0; k < 3; k++) {
      r_fwd[k] /= len;
    }
  }
  cam->GetProjectionMatrix().getValue(r_proj);
}

bool KX_CullingGate::OcclusionUsable(KX_Camera *cam) const
{
  const OcclusionView &v = m_occLatest;
  if (!v.valid || !cam || m_frame - v.frame > OCC_MAX_AGE) {
    return false;
  }
  float pos[3], fwd[3], proj[16];
  camera_view(cam, pos, fwd, proj);
  if (fwd[0] * v.fwd[0] + fwd[1] * v.fwd[1] + fwd[2] * v.fwd[2] < OCC_MIN_FWD_DOT) {
    return false;
  }
  const float dx = pos[0] - v.pos[0], dy = pos[1] - v.pos[1], dz = pos[2] - v.pos[2];
  /* The boxes were inflated for the expected camera travel: farther than
   * that (plus slack) and the test no longer bounds what is disoccluded. */
  const float allowed = v.margin + 0.25f;
  if (dx * dx + dy * dy + dz * dz > allowed * allowed) {
    return false;
  }
  for (int k = 0; k < 16; k++) {
    if (fabsf(proj[k] - v.proj[k]) > 1e-4f * (1.0f + fabsf(v.proj[k]))) {
      /* Zoom or aspect change. */
      return false;
    }
  }
  return true;
}

void KX_CullingGate::ConsumeOcclusion(const uint32_t *results)
{
  /* Bring the results from the layout of their submission to the current
   * one by replaying the swap-removes logged since. */
  std::vector<uint8_t> vis(results, results + m_occInflight.count);
  for (const std::pair<int, int> &move : m_occMoveLog) {
    const int slot = move.first, last = move.second;
    if (slot < int(vis.size())) {
      vis[slot] = (last < int(vis.size())) ? vis[last] : 1;
    }
    vis.resize(std::min(vis.size(), size_t(std::max(last, 0))));
  }

  /* Objects registered after the submission have no result: their streak
   * stays as is (0 for a new object). */
  const size_t n = std::min(vis.size(), m_obOccStreak.size());
  for (size_t i = 0; i < n; i++) {
    m_obOccStreak[i] = vis[i] ? 0 : uint8_t(std::min(int(m_obOccStreak[i]) + 1, 255));
  }
  m_occLatest = m_occInflight;
}

void KX_CullingGate::SubmitOcclusion(KX_Camera *cam, gpu::Texture *depth_tx)
{
  if (m_occSupported < 0) {
    /* The Metal backend has no non-blocking readback: it would stall. */
    m_occSupported = (GPU_backend_get_type() != GPU_BACKEND_METAL) ? 1 : 0;
  }
  const int n = int(m_obOwner.size());
  if (m_occSupported == 0 || !cam || !depth_tx || n == 0) {
    return;
  }

  if (!m_occHizShader) {
    m_occHizShader = create_hiz_shader();
    m_occTestShader = create_test_shader();
    if (!m_occHizShader || !m_occTestShader) {
      m_occSupported = 0;
      FreeGPUResources();
      return;
    }
  }

  if (n > m_occCapacity) {
    /* New buffers: whatever was in flight on the old ones is lost. */
    if (m_occBoundsBuf) {
      GPU_storagebuf_free(m_occBoundsBuf);
      GPU_storagebuf_free(m_occResultBuf);
    }
    int capacity = 1024;
    while (capacity < n) {
      capacity *= 2;
    }
    m_occCapacity = capacity;
    m_occBoundsBuf = GPU_storagebuf_create_ex(
        sizeof(float) * 6 * capacity, nullptr, GPU_USAGE_DYNAMIC, "KX_CullingGate.bounds");
    m_occResultBuf = GPU_storagebuf_create_ex(
        sizeof(uint32_t) * capacity, nullptr, GPU_USAGE_DYNAMIC, "KX_CullingGate.visibility");
    m_occStaging.assign(size_t(6) * capacity, 0.0f);
    m_occReadback.assign(size_t(capacity), 1u);
    m_occInflight.valid = false;
    m_occMoveLog.clear();
  }

  const int dw = GPU_texture_width(depth_tx);
  const int dh = GPU_texture_height(depth_tx);
  if (dw != m_occDepthSize[0] || dh != m_occDepthSize[1] || !m_occHizTex) {
    if (m_occHizTex) {
      GPU_texture_free(m_occHizTex);
    }
    m_occDepthSize[0] = dw;
    m_occDepthSize[1] = dh;
    m_occHizSize[0] = std::max(1, (dw + 1) / 2);
    m_occHizSize[1] = std::max(1, (dh + 1) / 2);
    /* Levels 1+ are stacked right of level 0. Rounded up halves can sum
     * above level 0 height (5 -> 3 + 2 + 1). */
    m_occLevels = 1;
    int stack_h = 0;
    for (int w = m_occHizSize[0], h = m_occHizSize[1];
         (w > 1 || h > 1) && m_occLevels < OCC_MAX_LEVELS;
         m_occLevels++)
    {
      w = std::max(1, (w + 1) / 2);
      h = std::max(1, (h + 1) / 2);
      stack_h += h;
    }
    const int atlas_w = m_occHizSize[0] + std::max(1, (m_occHizSize[0] + 1) / 2);
    const int atlas_h = std::max(m_occHizSize[1], stack_h);
    m_occHizTex = GPU_texture_create_2d("KX_CullingGate.hiz",
                                        atlas_w,
                                        atlas_h,
                                        1,
                                        gpu::TextureFormat::SFLOAT_32,
                                        GPU_TEXTURE_USAGE_SHADER_READ |
                                            GPU_TEXTURE_USAGE_SHADER_WRITE,
                                        nullptr);
    if (!m_occHizTex) {
      return;
    }
  }

  /* Boxes of this frame, SoA with the capacity as stride. */
  const size_t stride = size_t(m_occCapacity);
  const std::vector<float> *soa[6] = {&m_cx, &m_cy, &m_cz, &m_ex, &m_ey, &m_ez};
  for (int k = 0; k < 6; k++) {
    memcpy(m_occStaging.data() + k * stride, soa[k]->data(), sizeof(float) * n);
  }
  GPU_storagebuf_update(m_occBoundsBuf, m_occStaging.data());

  OcclusionView view;
  camera_view(cam, view.pos, view.fwd, view.proj);
  const float dx = view.pos[0] - m_occLastCamPos[0];
  const float dy = view.pos[1] - m_occLastCamPos[1];
  const float dz = view.pos[2] - m_occLastCamPos[2];
  const float speed = sqrtf(dx * dx + dy * dy + dz * dz);
  for (int k = 0; k < 3; k++) {
    m_occLastCamPos[k] = view.pos[k];
  }
  view.margin = OCC_MARGIN + std::min(OCC_MAX_SPEED_MARGIN, OCC_LATENCY_FRAMES * speed);
  view.frame = m_frame;
  view.count = n;
  view.valid = true;

  /* Hi-Z pyramid. */
  GPU_shader_bind(m_occHizShader);
  GPU_texture_bind(depth_tx, 0);
  GPU_texture_image_bind(m_occHizTex, 0);
  GPU_shader_uniform_2iv(m_occHizShader, "hiz0_size", m_occHizSize);
  GPU_shader_uniform_2iv(m_occHizShader, "depth_size", m_occDepthSize);
  int w = m_occHizSize[0], h = m_occHizSize[1];
  for (int level = 0; level < m_occLevels; level++) {
    GPU_shader_uniform_1i(m_occHizShader, "level", level);
    GPU_compute_dispatch(m_occHizShader,
                         uint((w + OCC_GROUP_2D - 1) / OCC_GROUP_2D),
                         uint((h + OCC_GROUP_2D - 1) / OCC_GROUP_2D),
                         1);
    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
    w = std::max(1, (w + 1) / 2);
    h = std::max(1, (h + 1) / 2);
  }
  GPU_texture_unbind(depth_tx);
  GPU_texture_image_unbind(m_occHizTex);

  /* Box test. */
  float persmat[4][4];
  (cam->GetProjectionMatrix() * cam->GetModelviewMatrix()).getValue(&persmat[0][0]);
  const float depth_size[2] = {float(m_occDepthSize[0]), float(m_occDepthSize[1])};
  GPU_shader_bind(m_occTestShader);
  GPU_texture_image_bind(m_occHizTex, 0);
  GPU_storagebuf_bind(m_occBoundsBuf, 0);
  GPU_storagebuf_bind(m_occResultBuf, 1);
  GPU_shader_uniform_mat4(m_occTestShader, "persmat", persmat);
  GPU_shader_uniform_1i(m_occTestShader, "count", n);
  GPU_shader_uniform_1i(m_occTestShader, "stride", m_occCapacity);
  GPU_shader_uniform_1i(m_occTestShader, "level_count", m_occLevels);
  GPU_shader_uniform_2iv(m_occTestShader, "hiz0_size", m_occHizSize);
  GPU_shader_uniform_2fv(m_occTestShader, "depth_size", depth_size);
  GPU_shader_uniform_1f(m_occTestShader, "expand", OCC_EXPAND);
  GPU_shader_uniform_1f(m_occTestShader, "margin", view.margin);
  GPU_compute_dispatch(m_occTestShader, uint((n + OCC_GROUP_1D - 1) / OCC_GROUP_1D), 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_BUFFER_UPDATE);
  GPU_storagebuf_unbind(m_occBoundsBuf);
  GPU_storagebuf_unbind(m_occResultBuf);
  GPU_texture_image_unbind(m_occHizTex);
  GPU_shader_unbind();

  /* Returns the copy submitted by an earlier call when the GPU is done with
   * it, and submits a copy of what was just written. On OpenGL a copy still
   * in flight makes it return false without submitting a new one. */
  const bool got = GPU_storagebuf_read_if_ready(m_occResultBuf, m_occReadback.data());
  if (got && m_occInflight.valid) {
    ConsumeOcclusion(m_occReadback.data());
  }
  if (got || !m_occInflight.valid) {
    m_occInflight = view;
    m_occMoveLog.clear();
  }
}
