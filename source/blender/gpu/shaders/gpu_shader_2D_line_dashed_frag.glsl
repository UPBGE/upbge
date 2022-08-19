
/*
 * Fragment Shader for dashed lines, with uniform multi-color(s),
 * or any single-color, and any thickness.
 *
 * Dashed is performed in screen space.
 */

void main()
{
  float distance_along_line = distance(stipple_pos, stipple_start);
  /* Solid line case, simple. */
  if (dash_factor >= 1.0f) {
    fragColor = color;
  }
  /* Actually dashed line... */
  else {
    float normalized_distance = fract(distance_along_line / dash_width);
    if (normalized_distance <= dash_factor) {
      fragColor = color;
    }
    else if (colors_len > 0) {
      fragColor = color2;
    }
    else {
      discard;
    }
  }
}
