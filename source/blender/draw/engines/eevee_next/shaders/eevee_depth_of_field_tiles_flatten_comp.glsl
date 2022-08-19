
/**
 * Tile flatten pass: Takes the halfres CoC buffer and converts it to 8x8 tiles.
 *
 * Output min and max values for each tile and for both foreground & background.
 * Also outputs min intersectable CoC for the background, which is the minimum CoC
 * that comes from the background pixels.
 *
 * Input:
 * - Half-resolution Circle of confusion. Out of setup pass.
 * Output:
 * - Separated foreground and background CoC. 1/8th of half-res resolution. So 1/16th of full-res.
 */

#pragma BLENDER_REQUIRE(eevee_depth_of_field_lib.glsl)

/**
 * In order to use atomic operations, we have to use uints. But this means having to deal with the
 * negative number ourselves. Luckily, each ground have a nicely defined range of values we can
 * remap to positive float.
 */
shared uint fg_min_coc;
shared uint fg_max_coc;
shared uint fg_max_intersectable_coc;
shared uint bg_min_coc;
shared uint bg_max_coc;
shared uint bg_min_intersectable_coc;

const uint dof_tile_large_coc_uint = floatBitsToUint(dof_tile_large_coc);

void main()
{
  if (all(equal(gl_LocalInvocationID.xy, uvec2(0)))) {
    /* NOTE: Min/Max flipped because of inverted fg_coc sign. */
    fg_min_coc = floatBitsToUint(0.0);
    fg_max_coc = dof_tile_large_coc_uint;
    fg_max_intersectable_coc = dof_tile_large_coc_uint;
    bg_min_coc = dof_tile_large_coc_uint;
    bg_max_coc = floatBitsToUint(0.0);
    bg_min_intersectable_coc = dof_tile_large_coc_uint;
  }
  barrier();

  ivec2 sample_texel = min(ivec2(gl_GlobalInvocationID.xy), textureSize(coc_tx, 0).xy - 1);
  vec2 sample_data = texelFetch(coc_tx, sample_texel, 0).rg;

  float sample_coc = sample_data.x;
  uint fg_coc = floatBitsToUint(max(-sample_coc, 0.0));
  /* NOTE: atomicMin/Max flipped because of inverted fg_coc sign. */
  atomicMax(fg_min_coc, fg_coc);
  atomicMin(fg_max_coc, fg_coc);
  atomicMin(fg_max_intersectable_coc, (sample_coc < 0.0) ? fg_coc : dof_tile_large_coc_uint);

  uint bg_coc = floatBitsToUint(max(sample_coc, 0.0));
  atomicMin(bg_min_coc, bg_coc);
  atomicMax(bg_max_coc, bg_coc);
  atomicMin(bg_min_intersectable_coc, (sample_coc > 0.0) ? bg_coc : dof_tile_large_coc_uint);

  barrier();

  if (all(equal(gl_LocalInvocationID.xy, uvec2(0)))) {
    if (fg_max_intersectable_coc == dof_tile_large_coc_uint) {
      fg_max_intersectable_coc = floatBitsToUint(0.0);
    }

    CocTile tile;
    /* Foreground sign is flipped since we compare unsigned representation. */
    tile.fg_min_coc = -uintBitsToFloat(fg_min_coc);
    tile.fg_max_coc = -uintBitsToFloat(fg_max_coc);
    tile.fg_max_intersectable_coc = -uintBitsToFloat(fg_max_intersectable_coc);
    tile.bg_min_coc = uintBitsToFloat(bg_min_coc);
    tile.bg_max_coc = uintBitsToFloat(bg_max_coc);
    tile.bg_min_intersectable_coc = uintBitsToFloat(bg_min_intersectable_coc);

    ivec2 tile_co = ivec2(gl_WorkGroupID.xy);
    dof_coc_tile_store(out_tiles_fg_img, out_tiles_bg_img, tile_co, tile);
  }
}
