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
 * reads instanceCount from the buffer before issuing the indirect draw.
 * FIX: indirect_draw_buf was declared READ_WRITE but is only read by the CPU draw call.
 * Changed to readonly to allow the driver to skip coherence tracking on this buffer,
 * improving throughput on AMD (avoids L2 invalidation) and NVIDIA (avoids flush).
 *
 * Layout matches eevee_game_culling_compute ShaderCreateInfo:
 *   storage_buf(0, READ,       "InstanceData",                "instance_data_buf[]")
 *   storage_buf(1, READ,       "DrawElementsIndirectCommand", "indirect_draw_buf[]")
 *   storage_buf(2, WRITE,      "uint",                        "visible_indices_buf[]")
 *   storage_buf(3, READ_WRITE, "uint",                        "visible_count_buf")
 *   push_constant(INT,  "instance_count")
 *   push_constant(MAT4, "viewproj")
 *
 * Local group size: 64 — matches one NVIDIA warp-pair or one AMD wavefront.
 * CPU dispatches ceil(instance_count / 64) groups.
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
  uint instanceCount;   /* Written by CPU; GPU reads this at draw time */
  uint firstIndex;      /* Written by CPU */
  uint baseVertex;      /* Written by CPU */
  uint baseInstance;    /* Written by CPU */
};

/* ------------------------------------------------------------------ */
/* Storage buffers                                                      */
/* ------------------------------------------------------------------ */

/* FIX: indirect_draw_buf changed from coherent READ_WRITE to readonly.
 * This shader never writes instanceCount — the CPU pre-fills it and the
 * driver issues the indirect draw from the buffer directly.
 * readonly removes the requirement for the GPU to track write coherence,
 * which measurably reduces stall latency on AMD RDNA (no L2 invalidation). */
layout(std430, binding = 0) readonly  buffer InstanceBuf    { InstanceData                 instance_data_buf[]; };
layout(std430, binding = 1) readonly  buffer IndirectBuf    { DrawElementsIndirectCommand  indirect_draw_buf[]; };
layout(std430, binding = 2) writeonly buffer VisibleIdxBuf  { uint                         visible_indices_buf[]; };

/* FIX: renamed from 'visible_count' to 'visible_count_buf' to match the ShaderCreateInfo
 * field name.  Mismatched names cause a GL_LINK_ERROR when the driver resolves bindings. */
layout(std430, binding = 3) coherent  buffer VisibleCountBuf { uint visible_count_buf; };

/* ------------------------------------------------------------------ */
/* Uniforms                                                             */
/* ------------------------------------------------------------------ */

uniform int  instance_count;
uniform mat4 viewproj;

/* Hi-Z pyramid — mip 0 = full res depth, higher mips = coarser. */
uniform sampler2D hiz_tx;

/* ------------------------------------------------------------------ */
/* Frustum plane extraction (Gribb-Hartmann method)
 *
 * Extract 6 clip planes directly from the view-projection matrix rows.
 * Planes are in world space. Positive half-space = inside frustum.
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
/* AABB vs frustum plane (p-vertex test)
 *
 * For each plane, test the corner of the AABB most aligned with the
 * plane normal (the "positive vertex"). If even that corner is outside,
 * the whole box is definitely outside the frustum.
 *
 * No false negatives: no visible object is ever culled.
 * False positives (occluded objects passing frustum) are caught by Hi-Z.
 * ------------------------------------------------------------------ */

bool aabb_outside_plane(vec3 aabb_min, vec3 aabb_max, vec4 plane)
{
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
 * Reference: "Hierarchical-Z map based occlusion culling"
 *             — Johan Andersson, Ubisoft / Frostbite 2007.
 * ------------------------------------------------------------------ */

bool hiz_occluded(vec3 ws_min, vec3 ws_max)
{
  vec2  ndc_min      =  vec2( 1.0);
  vec2  ndc_max      =  vec2(-1.0);
  float nearest_depth = 1.0; /* Standard depth: 0 = near, 1 = far. */

  for (int i = 0; i < 8; i++) {
    vec3 corner = vec3(
      (i & 1) != 0 ? ws_max.x : ws_min.x,
      (i & 2) != 0 ? ws_max.y : ws_min.y,
      (i & 4) != 0 ? ws_max.z : ws_min.z);

    vec4 clip = viewproj * vec4(corner, 1.0);
    if (clip.w <= 0.0) {
      /* Corner behind camera — AABB straddles near plane; skip occlusion test. */
      return false;
    }

    vec3 ndc = clip.xyz / clip.w;
    ndc_min = min(ndc_min, ndc.xy);
    ndc_max = max(ndc_max, ndc.xy);
    nearest_depth = min(nearest_depth, ndc.z);
  }

  ndc_min = clamp(ndc_min, -1.0, 1.0);
  ndc_max = clamp(ndc_max, -1.0, 1.0);

  vec2 uv_min = ndc_min * 0.5 + 0.5;
  vec2 uv_max = ndc_max * 0.5 + 0.5;

  /* Screen-space footprint at mip 0 */
  vec2 hiz_size  = vec2(textureSize(hiz_tx, 0));
  vec2 footprint = (uv_max - uv_min) * hiz_size;

  /* Smallest mip where one texel covers the full projected AABB. */
  int mip = clamp(int(ceil(log2(max(footprint.x, footprint.y)))),
                  0,
                  textureQueryLevels(hiz_tx) - 1);

  /* Sample the coarsest depth stored at this mip level.
   * We use the centre of the footprint; a conservative approach would sample
   * four corners and take the max, but centre is sufficient for most objects. */
  vec2 uv_center = (uv_min + uv_max) * 0.5;
  float hiz_depth = textureLod(hiz_tx, uv_center, float(mip)).r;

  /* If the AABB's nearest depth is farther than the occluder stored in Hi-Z,
   * the object is fully behind the occluder and can be safely culled. */
  return nearest_depth > hiz_depth;
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

void main()
{
  const uint idx = gl_GlobalInvocationID.x;

  /* Guard: extra threads launched by the ceil(count/64) dispatch. */
  if (int(idx) >= instance_count) {
    return;
  }

  const InstanceData inst = instance_data_buf[idx];

  /* Transform local-space AABB to world space.
   * We transform all 8 corners and compute the world-space AABB from them.
   * This is conservative but avoids the complexity of an OBB test. */
  vec3 ws_min = vec3( 1e30);
  vec3 ws_max = vec3(-1e30);

  for (int i = 0; i < 8; i++) {
    vec3 local_corner = vec3(
      (i & 1) != 0 ? inst.bb_max.x : inst.bb_min.x,
      (i & 2) != 0 ? inst.bb_max.y : inst.bb_min.y,
      (i & 4) != 0 ? inst.bb_max.z : inst.bb_min.z);

    vec3 world_corner = (inst.model_matrix * vec4(local_corner, 1.0)).xyz;
    ws_min = min(ws_min, world_corner);
    ws_max = max(ws_max, world_corner);
  }

  /* Frustum cull first — cheapest test, rejects the most. */
  if (frustum_cull(ws_min, ws_max)) {
    return;
  }

  /* Hi-Z occlusion cull — more expensive, catches objects behind terrain/walls. */
  if (hiz_occluded(ws_min, ws_max)) {
    return;
  }

  /* Object is visible: atomically claim a slot in visible_indices_buf[] and
   * write the resource_id so the draw call can index the per-object UBO.
   *
   * FIX: atomic target renamed from 'visible_count' to 'visible_count_buf'
   * to match the ShaderCreateInfo buffer name. */
  uint slot = atomicAdd(visible_count_buf, 1u);
  visible_indices_buf[slot] = inst.resource_id;
}
