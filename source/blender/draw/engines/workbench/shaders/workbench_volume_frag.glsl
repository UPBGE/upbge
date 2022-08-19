
#pragma BLENDER_REQUIRE(common_math_lib.glsl)
#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(workbench_common_lib.glsl)

float phase_function_isotropic()
{
  return 1.0 / (4.0 * M_PI);
}

float line_unit_box_intersect_dist(vec3 lineorigin, vec3 linedirection)
{
  /* https://seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/
   */
  vec3 firstplane = (vec3(1.0) - lineorigin) / linedirection;
  vec3 secondplane = (vec3(-1.0) - lineorigin) / linedirection;
  vec3 furthestplane = min(firstplane, secondplane);
  return max_v3(furthestplane);
}

#define sample_trilinear(ima, co) texture(ima, co)

vec4 sample_tricubic(sampler3D ima, vec3 co)
{
  vec3 tex_size = vec3(textureSize(ima, 0).xyz);

  co *= tex_size;
  /* texel center */
  vec3 tc = floor(co - 0.5) + 0.5;
  vec3 f = co - tc;
  vec3 f2 = f * f;
  vec3 f3 = f2 * f;
  /* Bspline coefs (optimized) */
  vec3 w3 = f3 / 6.0;
  vec3 w0 = -w3 + f2 * 0.5 - f * 0.5 + 1.0 / 6.0;
  vec3 w1 = f3 * 0.5 - f2 + 2.0 / 3.0;
  vec3 w2 = 1.0 - w0 - w1 - w3;

  vec3 s0 = w0 + w1;
  vec3 s1 = w2 + w3;

  vec3 f0 = w1 / (w0 + w1);
  vec3 f1 = w3 / (w2 + w3);

  vec2 final_z;
  vec4 final_co;
  final_co.xy = tc.xy - 1.0 + f0.xy;
  final_co.zw = tc.xy + 1.0 + f1.xy;
  final_z = tc.zz + vec2(-1.0, 1.0) + vec2(f0.z, f1.z);

  final_co /= tex_size.xyxy;
  final_z /= tex_size.zz;

  vec4 color;
  color = texture(ima, vec3(final_co.xy, final_z.x)) * s0.x * s0.y * s0.z;
  color += texture(ima, vec3(final_co.zy, final_z.x)) * s1.x * s0.y * s0.z;
  color += texture(ima, vec3(final_co.xw, final_z.x)) * s0.x * s1.y * s0.z;
  color += texture(ima, vec3(final_co.zw, final_z.x)) * s1.x * s1.y * s0.z;

  color += texture(ima, vec3(final_co.xy, final_z.y)) * s0.x * s0.y * s1.z;
  color += texture(ima, vec3(final_co.zy, final_z.y)) * s1.x * s0.y * s1.z;
  color += texture(ima, vec3(final_co.xw, final_z.y)) * s0.x * s1.y * s1.z;
  color += texture(ima, vec3(final_co.zw, final_z.y)) * s1.x * s1.y * s1.z;

  return color;
}

/* Nearest-neighbor interpolation */
vec4 sample_closest(sampler3D ima, vec3 co)
{
  /* Unnormalize coordinates */
  ivec3 cell_co = ivec3(co * vec3(textureSize(ima, 0).xyz));

  return texelFetch(ima, cell_co, 0);
}

vec4 flag_to_color(uint flag)
{
  /* Color mapping for flags */
  vec4 color = vec4(0.0, 0.0, 0.0, 0.06);
  /* Cell types: 1 is Fluid, 2 is Obstacle, 4 is Empty, 8 is Inflow, 16 is Outflow */
  if (bool(flag & uint(1))) {
    color.rgb += vec3(0.0, 0.0, 0.75); /* blue */
  }
  if (bool(flag & uint(2))) {
    color.rgb += vec3(0.2, 0.2, 0.2); /* dark gray */
  }
  if (bool(flag & uint(4))) {
    color.rgb += vec3(0.25, 0.0, 0.2); /* dark purple */
  }
  if (bool(flag & uint(8))) {
    color.rgb += vec3(0.0, 0.5, 0.0); /* dark green */
  }
  if (bool(flag & uint(16))) {
    color.rgb += vec3(0.9, 0.3, 0.0); /* orange */
  }
  if (is_zero(color.rgb)) {
    color.rgb += vec3(0.5, 0.0, 0.0); /* medium red */
  }
  return color;
}

#ifdef USE_TRICUBIC
#  define sample_volume_texture sample_tricubic
#elif defined(USE_TRILINEAR)
#  define sample_volume_texture sample_trilinear
#elif defined(USE_CLOSEST)
#  define sample_volume_texture sample_closest
#endif

void volume_properties(vec3 ls_pos, out vec3 scattering, out float extinction)
{
  vec3 co = ls_pos * 0.5 + 0.5;
#ifdef USE_COBA
  vec4 tval;
  if (showPhi) {
    /* Color mapping for level-set representation */
    float val = sample_volume_texture(densityTexture, co).r * gridScale;

    val = max(min(val * 0.2, 1.0), -1.0);

    if (val >= 0.0) {
      tval = vec4(val, 0.0, 0.5, 0.06);
    }
    else {
      tval = vec4(0.5, 1.0 + val, 0.0, 0.06);
    }
  }
  else if (showFlags) {
    /* Color mapping for flags */
    uint flag = texture(flagTexture, co).r;
    tval = flag_to_color(flag);
  }
  else if (showPressure) {
    /* Color mapping for pressure */
    float val = sample_volume_texture(densityTexture, co).r * gridScale;

    if (val > 0) {
      tval = vec4(val, val, val, 0.06);
    }
    else {
      tval = vec4(-val, 0.0, 0.0, 0.06);
    }
  }
  else {
    float val = sample_volume_texture(densityTexture, co).r * gridScale;
    tval = texture(transferTexture, val);
  }
  tval *= densityScale;
  tval.rgb = pow(tval.rgb, vec3(2.2));
  scattering = tval.rgb * 1500.0;
  extinction = max(1e-4, tval.a * 50.0);
#else
#  ifdef VOLUME_SMOKE
  float flame = sample_volume_texture(flameTexture, co).r;
  vec4 emission = texture(flameColorTexture, flame);
#  endif
  vec3 density = sample_volume_texture(densityTexture, co).rgb;
  float shadows = sample_volume_texture(shadowTexture, co).r;

  scattering = density * densityScale;
  extinction = max(1e-4, dot(scattering, vec3(0.33333)));
  scattering *= activeColor;

  /* Scale shadows in log space and clamp them to avoid completely black shadows. */
  scattering *= exp(clamp(log(shadows) * densityScale * 0.1, -2.5, 0.0)) * M_PI;

#  ifdef VOLUME_SMOKE
  /* 800 is arbitrary and here to mimic old viewport. TODO: make it a parameter. */
  scattering += emission.rgb * emission.a * 800.0;
#  endif
#endif
}

void eval_volume_step(inout vec3 Lscat, float extinction, float step_len, out float Tr)
{
  Lscat *= phase_function_isotropic();
  /* Evaluate Scattering */
  Tr = exp(-extinction * step_len);
  /* integrate along the current step segment */
  Lscat = (Lscat - Lscat * Tr) / extinction;
}

#define P(x) ((x + 0.5) * (1.0 / 16.0))
const vec4 dither_mat[4] = vec4[4](vec4(P(0.0), P(8.0), P(2.0), P(10.0)),
                                   vec4(P(12.0), P(4.0), P(14.0), P(6.0)),
                                   vec4(P(3.0), P(11.0), P(1.0), P(9.0)),
                                   vec4(P(15.0), P(7.0), P(13.0), P(5.0)));

vec4 volume_integration(vec3 ray_ori, vec3 ray_dir, float ray_inc, float ray_max, float step_len)
{
  /* Start with full transmittance and no scattered light. */
  vec3 final_scattering = vec3(0.0);
  float final_transmittance = 1.0;

  ivec2 tx = ivec2(gl_FragCoord.xy) % 4;
  float noise = fract(dither_mat[tx.x][tx.y] + noiseOfs);

  float ray_len = noise * ray_inc;
  for (int i = 0; i < samplesLen && ray_len < ray_max; i++, ray_len += ray_inc) {
    vec3 ls_pos = ray_ori + ray_dir * ray_len;

    vec3 Lscat;
    float s_extinction, Tr;
    volume_properties(ls_pos, Lscat, s_extinction);
    eval_volume_step(Lscat, s_extinction, step_len, Tr);
    /* accumulate and also take into account the transmittance from previous steps */
    final_scattering += final_transmittance * Lscat;
    final_transmittance *= Tr;
  }

  return vec4(final_scattering, final_transmittance);
}

void main()
{
#ifdef VOLUME_SLICE
  /* Manual depth test. TODO: remove. */
  float depth = texelFetch(depthBuffer, ivec2(gl_FragCoord.xy), 0).r;
  if (gl_FragCoord.z >= depth) {
    /* NOTE: In the Metal API, prior to Metal 2.3, Discard is not an explicit return and can
     * produce undefined behavior. This is especially prominent with derivatives if control-flow
     * divergence is present.
     *
     * Adding a return call eliminates undefined behavior and a later out-of-bounds read causing
     * a crash on AMD platforms.
     * This behavior can also affect OpenGL on certain devices. */
    discard;
    return;
  }

  vec3 Lscat;
  float s_extinction, Tr;
  volume_properties(localPos, Lscat, s_extinction);
  eval_volume_step(Lscat, s_extinction, stepLength, Tr);

  fragColor = vec4(Lscat, Tr);
#else
  vec2 screen_uv = gl_FragCoord.xy / vec2(textureSize(depthBuffer, 0).xy);
  bool is_persp = drw_view.winmat[3][3] == 0.0;

  vec3 volume_center = ModelMatrix[3].xyz;

  float depth = texelFetch(depthBuffer, ivec2(gl_FragCoord.xy), 0).r;
  float depth_end = min(depth, gl_FragCoord.z);
  vec3 vs_ray_end = get_view_space_from_depth(screen_uv, depth_end);
  vec3 vs_ray_ori = get_view_space_from_depth(screen_uv, 0.0);
  vec3 vs_ray_dir = (is_persp) ? (vs_ray_end - vs_ray_ori) : vec3(0.0, 0.0, -1.0);
  vs_ray_dir /= abs(vs_ray_dir.z);

  vec3 ls_ray_dir = point_view_to_object(vs_ray_ori + vs_ray_dir);
  vec3 ls_ray_ori = point_view_to_object(vs_ray_ori);
  vec3 ls_ray_end = point_view_to_object(vs_ray_end);

#  ifdef VOLUME_SMOKE
  ls_ray_dir = (OrcoTexCoFactors[0].xyz + ls_ray_dir * OrcoTexCoFactors[1].xyz) * 2.0 - 1.0;
  ls_ray_ori = (OrcoTexCoFactors[0].xyz + ls_ray_ori * OrcoTexCoFactors[1].xyz) * 2.0 - 1.0;
  ls_ray_end = (OrcoTexCoFactors[0].xyz + ls_ray_end * OrcoTexCoFactors[1].xyz) * 2.0 - 1.0;
#  else
  ls_ray_dir = (volumeObjectToTexture * vec4(ls_ray_dir, 1.0)).xyz * 2.0f - 1.0;
  ls_ray_ori = (volumeObjectToTexture * vec4(ls_ray_ori, 1.0)).xyz * 2.0f - 1.0;
  ls_ray_end = (volumeObjectToTexture * vec4(ls_ray_end, 1.0)).xyz * 2.0f - 1.0;
#  endif

  ls_ray_dir -= ls_ray_ori;

  /* TODO: Align rays to volume center so that it mimics old behavior of slicing the volume. */

  float dist = line_unit_box_intersect_dist(ls_ray_ori, ls_ray_dir);
  if (dist > 0.0) {
    ls_ray_ori = ls_ray_dir * dist + ls_ray_ori;
  }

  vec3 ls_vol_isect = ls_ray_end - ls_ray_ori;
  if (dot(ls_ray_dir, ls_vol_isect) < 0.0) {
    /* Start is further away than the end.
     * That means no volume is intersected. */
    discard;
    return;
  }

  fragColor = volume_integration(ls_ray_ori,
                                 ls_ray_dir,
                                 stepLength,
                                 length(ls_vol_isect) / length(ls_ray_dir),
                                 length(vs_ray_dir) * stepLength);
#endif

  /* Convert transmitance to alpha so we can use premul blending. */
  fragColor.a = 1.0 - fragColor.a;
}
