/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  float matte = texture_load(input_matte_tx, texel).x;

  /* Search the neighborhood around the current matte value and identify if it lies along the
   * edges of the matte. This is needs to be computed only when we need to compute the edges output
   * or tweak the levels of the matte. */
  bool is_edge = false;
#if defined(COMPUTE_EDGES)
  bool compute_edges = true;
#else
  bool compute_edges = black_level != 0.0f || white_level != 1.0f;
#endif
  if (compute_edges) {
    /* Count the number of neighbors whose matte is sufficiently similar to the current matte,
     * as controlled by the edge_tolerance factor. */
    int count = 0;
    for (int j = -edge_search_radius; j <= edge_search_radius; j++) {
      for (int i = -edge_search_radius; i <= edge_search_radius; i++) {
        float neighbor_matte = texture_load(input_matte_tx, texel + int2(i, j)).x;
        count += int(distance(matte, neighbor_matte) < edge_tolerance);
      }
    }

    /* If the number of neighbors that are sufficiently similar to the center matte is less that
     * 90% of the total number of neighbors, then that means the variance is high in that areas
     * and it is considered an edge. */
    is_edge = count < ((edge_search_radius * 2 + 1) * (edge_search_radius * 2 + 1)) * 0.9f;
  }

  float tweaked_matte = matte;

  /* Remap the matte using the black and white levels, but only for areas that are not on the edge
   * of the matte to preserve details. Also check for equality between levels to avoid zero
   * division. */
  if (!is_edge && white_level != black_level) {
    tweaked_matte = clamp((matte - black_level) / (white_level - black_level), 0.0f, 1.0f);
  }

  /* Exclude unwanted areas using the provided garbage matte, 1 means unwanted, so invert the
   * garbage matte and take the minimum. */
  float garbage_matte = texture_load(garbage_matte_tx, texel).x;
  tweaked_matte = min(tweaked_matte, 1.0f - garbage_matte);

  /* Include wanted areas that were incorrectly keyed using the provided core matte. */
  float core_matte = texture_load(core_matte_tx, texel).x;
  tweaked_matte = max(tweaked_matte, core_matte);

  imageStore(output_matte_img, texel, float4(tweaked_matte));
#if defined(COMPUTE_EDGES)
  imageStore(output_edges_img, texel, float4(is_edge ? 1.0f : 0.0f));
#endif
}
