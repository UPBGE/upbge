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

#include "infos/eevee_game_infos.hh"

/* Adjust according to SMAA_STAGE for C++ compilation. */
VERTEX_SHADER_CREATE_INFO(eevee_game_smaa_weight)

#include "gpu_shader_smaa_lib.glsl"

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

  vec4 offset[3];
#if SMAA_STAGE == 0
  SMAAEdgeDetectionVS(uvs, offset);
#elif SMAA_STAGE == 1
  SMAABlendingWeightCalculationVS(uvs, pixcoord, offset);
#elif SMAA_STAGE == 2
  SMAANeighborhoodBlendingVS(uvs, offset[0]);
#endif
  /* Varyings cannot be arrays. */
  offset0 = offset[0];
  offset1 = offset[1];
  offset2 = offset[2];
}
