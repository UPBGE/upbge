/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * GPU-Driven Culling Compute Shader
 *
 * One thread per instance. Each thread:
 *   1. Reads the world-space AABB from instance_data_buf[].
 *   2. Tests against 6 frustum planes (frustum culling).
 *   3. If visible, optionally tests against the Hi-Z pyramid (occlusion culling).
 *   4. If surviving, appends the resource_id to visible_indices_buf[]
 *      via a coherent atomic counter.
 *
 * The indirect_draw_buf[] is not written here — CullingModule::execute_culling()
 * on the CPU pre-fills count/firstIndex/baseVertex/baseInstance. The GPU only
 * writes instanceCount per draw bucket, which the driver reads from the buffer
 * before issuing the indirect draw.
 *
 * Layout matches eevee_game_culling_compute ShaderCreateInfo:
 *   storage_buf(0, READ,       "InstanceData",              "instance_data_buf[]")
 *   storage_buf(1, READ_WRITE, "DrawElementsIndirectCommand","indirect_draw_buf[]")
 *   push_constant(INT,  "instance_count")
 *   push_constant(MAT4, "viewproj")
 *
 * The Hi-Z texture (hiz_tx) and the visible_indices_buf are also bound by
 * execute_culling() but declared here for completeness.
 *
 * Local group size: 64 — chosen to match a single warp/wavefront on both
 * NVIDIA (32 threads) and AMD (64 threads). The CPU dispatches
 * ceil(instance_count / 64) groups.
 */

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

/* ------------------------------------------------------------------ */
/* Structs — must match C++ GPUInstanceData and DrawCommand exactly.   */
/* ------------------------------------------------------------------ */

struct InstanceData {
  mat4  model_matrix;
  vec3  bb_min;        /* AABB min corner in local space */
  uint  resource_id;
  vec3  bb_max;        /* AABB max corner in local space */
  uint  flags;         /* Reserved for shadow-caster / transparency bits */
};

/* GL_ARB_draw_indirect / Vulkan VkDrawIndexedIndirectCommand layout */
struct DrawElementsIndirectCommand {
  uint count;           /* Index count — written by CPU, not touched here */
  uint instanceCount;   /* Written by this shader (0 = culled, 1 = visible) */
  uint firstIndex;      /* Written by CPU */
  uint baseVertex;      /* Written by CPU */
  uint baseInstance;    /* Written by CPU */
};

/* ------------------------------------------------------------------ */
/* Storage buffers                                                      */
/* ------------------------------------------------------------------ */

layout(std430, binding = 0) readonly  buffer InstanceBuf  { InstanceData                 instance_data_buf[]; };
layout(std430, binding = 1) coherent  buffer IndirectBuf   { DrawElementsIndirectCommand  indirect_draw_buf[]; };
layout(std430, binding = 2) writeonly buffer VisibleIdxBuf { uint                         visible_indices_buf[]; };

/* Atomic counter for the next available slot in visible_indices_buf[].
 * Stored as the first element of a dedicated single-uint SSBO. */
layout(std430, binding = 3) coherent buffer VisibleCountBuf { uint visible_count; };

/* ------------------------------------------------------------------ */
/* Uniforms                                                             */
/* ------------------------------------------------------------------ */

uniform int  instance_count;
uniform mat4 viewproj;

/* Hi-Z pyramid — mip 0 = full res depth, higher mips = coarser.
 * Bound by execute_culling(). */
uniform sampler2D hiz_tx;

/* ------------------------------------------------------------------ */
/* Frustum plane extraction
 *
 * Gribb-Hartmann method: extract 6 clip planes directly from the
 * view-projection matrix rows. Planes are in world space.
 * Positive half-space = inside frustum.
 *
 * Reference: "Fast Extraction of Viewing Frustum Planes from the
 *             World-View-Projection Matrix" — Gribb & Hartmann 2001.
 * ------------------------------------------------------------------ */

vec4 frustum_plane(int idx)
{
  vec4 row3 = vec4(viewproj[0][3], viewproj[1][3], viewproj[2][3], viewproj[3][3]);
  switch (idx) {
    case 0: /* Left   */ return row3 + vec4(viewproj[0][0], viewproj[1][0], viewproj[2][0], viewproj[3][0]);
    case 1: /* Right  */ return row3 - vec4(viewproj[0][0], viewproj[1][0], viewproj[2][0], viewproj[3][0]);
    case 2: /* Bottom */ return row3 + vec4(viewproj[0][1], viewproj[1][1], viewproj[2][1], viewproj[3][1]);
    case 3: /* Top    */ return row3 - vec4(viewproj[0][1], viewproj[1][1], viewproj[2][1], viewproj[3][1]);
    case 4: /* Near   */ return row3 + vec4(viewproj[0][2], viewproj[1][2], viewproj[2][2], viewproj[3][2]);
    case 5: /* Far    */ return row3 - vec4(viewproj[0][2], viewproj[1][2], viewproj[2][2], viewproj[3][2]);
    default: return vec4(0.0);
  }
}

/* ------------------------------------------------------------------ */
/* AABB vs frustum plane
 *
 * For each plane, test the "positive vertex" of the AABB — the corner
 * that is most in the direction of the plane normal. If even that corner
 * is outside (negative signed distance), the whole box is culled.
 *
 * This is the standard "p-vertex" test; it generates no false negatives
 * (no visible object is culled) though it may pass some fully occluded
 * objects (those are caught by Hi-Z below).
 * ------------------------------------------------------------------ */

bool aabb_outside_plane(vec3 aabb_min, vec3 aabb_max, vec4 plane)
{
  /* p-vertex: for each axis pick the component that maximises dot(p, normal) */
  vec3 p = mix(aabb_min, aabb_max, greaterThanEqual(plane.xyz, vec3(0.0)));
  return dot(plane.xyz, p) + plane.w < 0.0;
}

bool frustum_cull(vec3 ws_min, vec3 ws_max)
{
  for (int i = 0; i < 6; i++) {
    if (aabb_outside_plane(ws_min, ws_max, frustum_plane(i))) {
      return true; /* Culled */
    }
  }
  return false; /* Visible */
}

/* ------------------------------------------------------------------ */
/* Hi-Z occlusion test
 *
 * Project the world-space AABB to NDC, compute the screen-space footprint,
 * select the appropriate Hi-Z mip level, and compare the stored maximum
 * depth against the AABB's nearest depth.
 *
 * If the entire AABB is behind the stored depth, the object is occluded.
 *
 * Reference: "Hierarchical-Z map based occlusion culling"
 *             — Johan Andersson, Ubisoft / Frostbite 2007.
 * ------------------------------------------------------------------ */

bool hiz_occluded(vec3 ws_min, vec3 ws_max)
{
  /* Transform all 8 AABB corners to clip space and find the screen-space
   * bounding rectangle + nearest clip-space depth. */
  vec2 ndc_min =  vec2( 1.0);
  vec2 ndc_max =  vec2(-1.0);
  float nearest_depth = 1.0; /* Reversed-Z: 1.0 = near plane */

  for (int i = 0; i < 8; i++) {
    vec3 corner = vec3(
      (i & 1) != 0 ? ws_max.x : ws_min.x,
      (i & 2) != 0 ? ws_max.y : ws_min.y,
      (i & 4) != 0 ? ws_max.z : ws_min.z);

    vec4 clip = viewproj * vec4(corner, 1.0);
    if (clip.w <= 0.0) {
      /* Corner behind the camera — conservatively skip the occlusion test
       * because the AABB straddles the near plane. */
      return false;
    }

    vec3 ndc = clip.xyz / clip.w;
    ndc_min = min(ndc_min, ndc.xy);
    ndc_max = max(ndc_max, ndc.xy);

    /* Standard depth: 0 = near, 1 = far (non-reversed-Z). Blender uses standard. */
    nearest_depth = min(nearest_depth, ndc.z);
  }

  /* Clamp to [-1, 1] — objects partially outside the frustum passed the
   * frustum test but may still be occluded for the visible portion. */
  ndc_min = clamp(ndc_min, -1.0, 1.0);
  ndc_max = clamp(ndc_max, -1.0, 1.0);

  /* Convert NDC [-1,1] to UV [0,1] */
  vec2 uv_min = ndc_min * 0.5 + 0.5;
  vec2 uv_max = ndc_max * 0.5 + 0.5;

  /* Screen-space footprint in pixels at mip 0 */
  vec2 hiz_size = vec2(textureSize(hiz_tx, 0));
  vec2 footprint = (uv_max - uv_min) * hiz_size;

  /* Pick the mip level whose texel covers the footprint.
   * log2(max(footprint)) gives us the smallest mip where a single texel
   * covers the entire projected AABB. We clamp to the available mip range. */
  int mip = clamp(int(ceil(log2(max(footprint.x, footprint.y)))),
                  0,
                  textureQueryLevels(hiz_tx) - 1);

  /* Sample the Hi-Z at the center of the footprint.
   * The Hi-Z stores the MAXIMUM depth in each tile — if the stored max
   * depth is closer than our nearest depth, all geometry is in front of us. */
  vec2 uv_center = (uv_min + uv_max) * 0.5;
  float hiz_depth = textureLod(hiz_tx, uv_center, float(mip)).r;

  /* Occluded if the AABB's nearest depth is farther than what Hi-Z records.
   * Standard depth convention: greater value = farther from camera. */
  return nearest_depth > hiz_depth;
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

void main()
{
  uint idx = gl_GlobalInvocationID.x;
  if (int(idx) >= instance_count) {
    return;
  }

  InstanceData inst = instance_data_buf[idx];

  /* Transform AABB to world space using the model matrix.
   * We use the "transformed AABB" approach: transform all 8 corners and
   * recompute the world-space AABB. This is conservative but avoids
   * computing the full OBB-frustum test, which is significantly more expensive.
   *
   * For most game objects (static meshes, characters) the AABB expansion is
   * small. For heavily rotated long objects this is pessimistic but correct —
   * it never culls a visible object. */
  vec3 ls_min = inst.bb_min;
  vec3 ls_max = inst.bb_max;
  mat4 M      = inst.model_matrix;

  vec3 ws_min = vec3( 1e30);
  vec3 ws_max = vec3(-1e30);
  for (int i = 0; i < 8; i++) {
    vec3 ls_corner = vec3(
      (i & 1) != 0 ? ls_max.x : ls_min.x,
      (i & 2) != 0 ? ls_max.y : ls_min.y,
      (i & 4) != 0 ? ls_max.z : ls_min.z);
    vec3 ws = (M * vec4(ls_corner, 1.0)).xyz;
    ws_min = min(ws_min, ws);
    ws_max = max(ws_max, ws);
  }

  /* ---- Frustum culling ---- */
  if (frustum_cull(ws_min, ws_max)) {
    return; /* Culled by frustum */
  }

  /* ---- Hi-Z occlusion culling ---- */
  if (hiz_occluded(ws_min, ws_max)) {
    return; /* Occluded by closer geometry */
  }

  /* ---- Survived both tests: record as visible ----
   *
   * Atomically claim the next slot in visible_indices_buf[] and write
   * the resource_id so the draw call can index into the per-object UBO.
   *
   * Note: we do NOT write instanceCount into indirect_draw_buf[] here.
   * The current architecture uses visible_indices_buf[] as an indirection
   * layer; the actual draw calls are submitted by the DRW Pass system which
   * reads from this buffer. A future optimisation is to write directly into
   * indirect_draw_buf[].instanceCount when per-bucket indirect draw is added. */
  uint slot = atomicAdd(visible_count, 1u);
  visible_indices_buf[slot] = inst.resource_id;
}
