
/**
 * Holefill pass: Gather background parts where foreground is present.
 *
 * Using the min&max CoC tile buffer, we select the best appropriate method to blur the scene
 *color. A fast gather path is taken if there is not many CoC variation inside the tile.
 *
 * We sample using an octaweb sampling pattern. We randomize the kernel center and each ring
 * rotation to ensure maximum coverage.
 **/

#pragma BLENDER_REQUIRE(eevee_depth_of_field_accumulator_lib.glsl)

void main()
{
  ivec2 tile_co = ivec2(gl_GlobalInvocationID.xy / DOF_TILES_SIZE);
  CocTile coc_tile = dof_coc_tile_load(in_tiles_fg_img, in_tiles_bg_img, tile_co);
  CocTilePrediction prediction = dof_coc_tile_prediction_get(coc_tile);

  float base_radius = -coc_tile.fg_min_coc;
  float min_radius = -coc_tile.fg_max_coc;
  float min_intersectable_radius = dof_tile_large_coc;
  bool can_early_out = !prediction.do_hole_fill;

  bool do_fast_gather = dof_do_fast_gather(base_radius, min_radius, is_foreground);

  /* Gather at half resolution. Divide CoC by 2. */
  base_radius *= 0.5;
  min_intersectable_radius *= 0.5;

  bool do_density_change = dof_do_density_change(base_radius, min_intersectable_radius);

  vec4 out_color = vec4(0.0);
  float out_weight = 0.0;
  vec2 unused_occlusion = vec2(0.0, 0.0);

  if (can_early_out) {
    /* Early out. */
  }
  else if (do_fast_gather) {
    dof_gather_accumulator(color_tx,
                           color_bilinear_tx,
                           coc_tx,
                           coc_tx,
                           base_radius,
                           min_intersectable_radius,
                           true,
                           false,
                           out_color,
                           out_weight,
                           unused_occlusion);
  }
  else {
    dof_gather_accumulator(color_tx,
                           color_bilinear_tx,
                           coc_tx,
                           coc_tx,
                           base_radius,
                           min_intersectable_radius,
                           false,
                           false,
                           out_color,
                           out_weight,
                           unused_occlusion);
  }

  ivec2 out_texel = ivec2(gl_GlobalInvocationID.xy);
  imageStore(out_color_img, out_texel, out_color);
  imageStore(out_weight_img, out_texel, vec4(out_weight));
}
