/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * SMAA Vertex Shader — Common to all 3 SMAA stages.
 *
 * Generates a fullscreen triangle using the gl_VertexID trick (no VBO needed):
 *   vert 0: (-1, -1)
 *   vert 1: ( 3, -1)
 *   vert 2: (-1,  3)
 * Together they cover the entire NDC clip space with a single oversized triangle.
 *
 * Per-stage SMAA library calls set up the required UV offsets in `offset[3]`
 * that the fragment shader passes into SMAAEdgeDetection, SMAABlendingWeight,
 * and SMAANeighborhoodBlending respectively.
 */

#define SMAA_GLSL_4
#define SMAA_PRESET_HIGH

uniform vec4 smaa_rt_metrics;  /* vec4(1/W, 1/H, W, H) */
#define SMAA_RT_METRICS smaa_rt_metrics

#include "gpu_shader_smaa_lib.glsl"

out vec2 uvs;
out vec2 pixcoord;
out vec4 offset[3];

void main()
{
  /* Fullscreen triangle from vertex ID — no vertex buffer required.
   * Vertex positions:
   *   id=0: ndc=(-1,-1), uv=(0,0)
   *   id=1: ndc=( 3,-1), uv=(2,0)  ← off-screen but clips correctly
   *   id=2: ndc=(-1, 3), uv=(0,2)  ← off-screen but clips correctly
   * The triangle covers the full [-1,1]² NDC square. */
  int v = gl_VertexID % 3;
  float x = float((v & 1) << 2) - 1.0;
  float y = float((v & 2) << 1) - 1.0;
  gl_Position = vec4(x, y, 1.0, 1.0);

  /* UV [0,1] derived from NDC [−1,1]. */
  uvs = (gl_Position.xy + 1.0) * 0.5;

#if SMAA_STAGE == 0
  SMAAEdgeDetectionVS(uvs, offset);
#elif SMAA_STAGE == 1
  SMAABlendingWeightCalculationVS(uvs, pixcoord, offset);
#elif SMAA_STAGE == 2
  SMAANeighborhoodBlendingVS(uvs, offset[0]);
#endif
}
