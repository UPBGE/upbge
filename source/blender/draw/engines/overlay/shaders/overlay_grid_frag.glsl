/**
 * Infinite grid:
 * Draw antialiazed grid and axes of different sizes with smooth blending between Level of details.
 * We draw multiple triangles to avoid float precision issues due to perspective interpolation.
 **/

#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_lib.glsl)

/**
 * We want to know how much a pixel is covered by a line.
 * We replace the square pixel with acircle of the same area and try to find the intersection area.
 * The area we search is the circular segment. https://en.wikipedia.org/wiki/Circular_segment
 * The formula for the area uses inverse trig function and is quite complexe. Instead,
 * we approximate it by using the smoothstep function and a 1.05 factor to the disc radius.
 */
#define M_1_SQRTPI 0.5641895835477563 /* 1/sqrt(pi) */
#define DISC_RADIUS (M_1_SQRTPI * 1.05)
#define GRID_LINE_SMOOTH_START (0.5 - DISC_RADIUS)
#define GRID_LINE_SMOOTH_END (0.5 + DISC_RADIUS)
#define GRID_LINE_STEP(dist) smoothstep(GRID_LINE_SMOOTH_START, GRID_LINE_SMOOTH_END, dist)

float get_grid(vec2 co, vec2 fwidthCos, float grid_scale)
{
  float half_size = grid_scale / 2.0;
  /* Triangular wave pattern, amplitude is [0, half_size]. */
  vec2 grid_domain = abs(mod(co + half_size, vec2(grid_scale)) - half_size);
  /* Modulate by the absolute rate of change of the coordinates
   * (make line have the same width under perspective). */
  grid_domain /= fwidthCos;
  /* Collapse waves. */
  float line_dist = min(grid_domain.x, grid_domain.y);
  return 1.0 - GRID_LINE_STEP(line_dist - grid_buf.line_size);
}

vec3 get_axes(vec3 co, vec3 fwidthCos, float line_size)
{
  vec3 axes_domain = abs(co);
  /* Modulate by the absolute rate of change of the coordinates
   * (make line have the same width under perspective). */
  axes_domain /= fwidthCos;
  return 1.0 - GRID_LINE_STEP(axes_domain - (line_size + grid_buf.line_size));
}

#define linearstep(p0, p1, v) (clamp(((v) - (p0)) / abs((p1) - (p0)), 0.0, 1.0))

void main()
{
  vec3 P = local_pos * grid_buf.size.xyz;
  vec3 dFdxPos = dFdx(P);
  vec3 dFdyPos = dFdy(P);
  vec3 fwidthPos = abs(dFdxPos) + abs(dFdyPos);
  P += cameraPos * plane_axes;

  float dist, fade;
  bool is_persp = drw_view.winmat[3][3] == 0.0;
  if (is_persp) {
    vec3 V = cameraPos - P;
    dist = length(V);
    V /= dist;

    float angle;
    if (flag_test(grid_flag, PLANE_XZ)) {
      angle = V.y;
    }
    else if (flag_test(grid_flag, PLANE_YZ)) {
      angle = V.x;
    }
    else {
      angle = V.z;
    }

    angle = 1.0 - abs(angle);
    angle *= angle;
    fade = 1.0 - angle * angle;
    fade *= 1.0 - smoothstep(0.0, grid_buf.distance, dist - grid_buf.distance);
  }
  else {
    dist = gl_FragCoord.z * 2.0 - 1.0;
    /* Avoid fading in +Z direction in camera view (see T70193). */
    dist = flag_test(grid_flag, GRID_CAMERA) ? clamp(dist, 0.0, 1.0) : abs(dist);
    fade = 1.0 - smoothstep(0.0, 0.5, dist - 0.5);
    dist = 1.0; /* Avoid branch after. */

    if (flag_test(grid_flag, PLANE_XY)) {
      float angle = 1.0 - abs(drw_view.viewinv[2].z);
      dist = 1.0 + angle * 2.0;
      angle *= angle;
      fade *= 1.0 - angle * angle;
    }
  }

  if (flag_test(grid_flag, SHOW_GRID)) {
    /* Using `max(dot(dFdxPos, screenVecs[0]), dot(dFdyPos, screenVecs[1]))`
     * would be more accurate, but not really necessary. */
    float grid_res = dot(dFdxPos, screenVecs[0].xyz);

    /* The grid begins to appear when it comprises 4 pixels. */
    grid_res *= 4;

    /* For UV/Image editor use grid_buf.zoom_factor. */
    if (flag_test(grid_flag, PLANE_IMAGE) &&
        /* Grid begins to appear when the length of one grid unit is at least
         * (256/grid_size) pixels Value of grid_size defined in `overlay_grid.c`. */
        !flag_test(grid_flag, CUSTOM_GRID)) {
      grid_res = grid_buf.zoom_factor;
    }

    /* From biggest to smallest. */
    vec4 scale;
#define grid_step(a) grid_buf.steps[a].x
#if 0 /* Inefficient. */
    int step_id = 0;
    scale[0] = 0.0;
    scale[1] = grid_step(0);
    while (scale[1] < grid_res && step_id != STEPS_LEN - 1) {
      scale[0] = scale[1];
      scale[1] = grid_step(++step_id);
    }
    scale[2] = grid_step(min(step_id + 1, STEPS_LEN - 1));
    scale[3] = grid_step(min(step_id + 2, STEPS_LEN - 1));
#else
    /* For more efficiency, unroll the loop above. */
    if (grid_step(0) > grid_res) {
      scale = vec4(0.0, grid_step(0), grid_step(1), grid_step(2));
    }
    else if (grid_step(1) > grid_res) {
      scale = vec4(grid_step(0), grid_step(1), grid_step(2), grid_step(3));
    }
    else if (grid_step(2) > grid_res) {
      scale = vec4(grid_step(1), grid_step(2), grid_step(3), grid_step(4));
    }
    else if (grid_step(3) > grid_res) {
      scale = vec4(grid_step(2), grid_step(3), grid_step(4), grid_step(5));
    }
    else if (grid_step(4) > grid_res) {
      scale = vec4(grid_step(3), grid_step(4), grid_step(5), grid_step(6));
    }
    else if (grid_step(5) > grid_res) {
      scale = vec4(grid_step(4), grid_step(5), grid_step(6), grid_step(7));
    }
    else if (grid_step(6) > grid_res) {
      scale = vec4(grid_step(5), grid_step(6), grid_step(7), grid_step(7));
    }
    else {
      scale = vec4(grid_step(6), grid_step(7), grid_step(7), grid_step(7));
    }
#endif
#undef grid_step

    float blend = 1.0 - linearstep(scale[0], scale[1], grid_res);
    blend = blend * blend * blend;

    vec2 grid_pos, grid_fwidth;
    if (flag_test(grid_flag, PLANE_XZ)) {
      grid_pos = P.xz;
      grid_fwidth = fwidthPos.xz;
    }
    else if (flag_test(grid_flag, PLANE_YZ)) {
      grid_pos = P.yz;
      grid_fwidth = fwidthPos.yz;
    }
    else {
      grid_pos = P.xy;
      grid_fwidth = fwidthPos.xy;
    }

    float gridA = get_grid(grid_pos, grid_fwidth, scale[1]);
    float gridB = get_grid(grid_pos, grid_fwidth, scale[2]);
    float gridC = get_grid(grid_pos, grid_fwidth, scale[3]);

    out_color = colorGrid;
    out_color.a *= gridA * blend;
    out_color = mix(out_color, mix(colorGrid, colorGridEmphasis, blend), gridB);
    out_color = mix(out_color, colorGridEmphasis, gridC);
  }
  else {
    out_color = vec4(colorGrid.rgb, 0.0);
  }

  if (flag_test(grid_flag, (SHOW_AXIS_X | SHOW_AXIS_Y | SHOW_AXIS_Z))) {
    /* Setup axes 'domains' */
    vec3 axes_dist, axes_fwidth;

    if (flag_test(grid_flag, SHOW_AXIS_X)) {
      axes_dist.x = dot(P.yz, plane_axes.yz);
      axes_fwidth.x = dot(fwidthPos.yz, plane_axes.yz);
    }
    if (flag_test(grid_flag, SHOW_AXIS_Y)) {
      axes_dist.y = dot(P.xz, plane_axes.xz);
      axes_fwidth.y = dot(fwidthPos.xz, plane_axes.xz);
    }
    if (flag_test(grid_flag, SHOW_AXIS_Z)) {
      axes_dist.z = dot(P.xy, plane_axes.xy);
      axes_fwidth.z = dot(fwidthPos.xy, plane_axes.xy);
    }

    /* Computing all axes at once using vec3 */
    vec3 axes = get_axes(axes_dist, axes_fwidth, 0.1);

    if (flag_test(grid_flag, SHOW_AXIS_X)) {
      out_color.a = max(out_color.a, axes.x);
      out_color.rgb = (axes.x < 1e-8) ? out_color.rgb : colorGridAxisX.rgb;
    }
    if (flag_test(grid_flag, SHOW_AXIS_Y)) {
      out_color.a = max(out_color.a, axes.y);
      out_color.rgb = (axes.y < 1e-8) ? out_color.rgb : colorGridAxisY.rgb;
    }
    if (flag_test(grid_flag, SHOW_AXIS_Z)) {
      out_color.a = max(out_color.a, axes.z);
      out_color.rgb = (axes.z < 1e-8) ? out_color.rgb : colorGridAxisZ.rgb;
    }
  }

  float scene_depth = texelFetch(depth_tx, ivec2(gl_FragCoord.xy), 0).r;
  if (flag_test(grid_flag, GRID_BACK)) {
    fade *= (scene_depth == 1.0) ? 1.0 : 0.0;
  }
  else {
    /* Add a small bias so the grid will always be below of a mesh with the same depth. */
    float grid_depth = gl_FragCoord.z + 4.8e-7;
    /* Manual, non hard, depth test:
     * Progressively fade the grid below occluders
     * (avoids popping visuals due to depth buffer precision) */
    /* Harder settings tend to flicker more,
     * but have less "see through" appearance. */
    float bias = max(fwidth(gl_FragCoord.z), 2.4e-7);
    fade *= linearstep(grid_depth, grid_depth + bias, scene_depth);
  }

  out_color.a *= fade;
}
