
/**
 * Depth shader that can stochastically discard transparent pixel.
 */

#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_lib.glsl)
#pragma BLENDER_REQUIRE(common_hair_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_nodetree_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_surf_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_velocity_lib.glsl)

vec4 closure_to_rgba(Closure cl)
{
  vec4 out_color;
  out_color.rgb = g_emission;
  out_color.a = saturate(1.0 - avg(g_transmittance));

  /* Reset for the next closure tree. */
  closure_weights_reset();

  return out_color;
}

/* From the paper "Hashed Alpha Testing" by Chris Wyman and Morgan McGuire. */
float hash(vec2 a)
{
  return fract(1e4 * sin(17.0 * a.x + 0.1 * a.y) * (0.1 + abs(sin(13.0 * a.y + a.x))));
}

float hash3d(vec3 a)
{
  return hash(vec2(hash(a.xy), a.z));
}

float hashed_alpha_threshold(float hash_scale, float hash_offset, vec3 P)
{
  /* Find the discretized derivatives of our coordinates. */
  float max_deriv = max(length(dFdx(P)), length(dFdy(P)));
  float pix_scale = 1.0 / (hash_scale * max_deriv);
  /* Find two nearest log-discretized noise scales. */
  float pix_scale_log = log2(pix_scale);
  vec2 pix_scales;
  pix_scales.x = exp2(floor(pix_scale_log));
  pix_scales.y = exp2(ceil(pix_scale_log));
  /* Compute alpha thresholds at our two noise scales. */
  vec2 alpha;
  alpha.x = hash3d(floor(pix_scales.x * P));
  alpha.y = hash3d(floor(pix_scales.y * P));
  /* Factor to interpolate lerp with. */
  float fac = fract(log2(pix_scale));
  /* Interpolate alpha threshold from noise at two scales. */
  float x = mix(alpha.x, alpha.y, fac);
  /* Pass into CDF to compute uniformly distrib threshold. */
  float a = min(fac, 1.0 - fac);
  float one_a = 1.0 - a;
  float denom = 1.0 / (2 * a * one_a);
  float one_x = (1 - x);
  vec3 cases = vec3((x * x) * denom, (x - 0.5 * a) / one_a, 1.0 - (one_x * one_x * denom));
  /* Find our final, uniformly distributed alpha threshold. */
  float threshold = (x < one_a) ? ((x < a) ? cases.x : cases.y) : cases.z;
  /* Jitter the threshold for TAA accumulation. */
  threshold = fract(threshold + hash_offset);
  /* Avoids threshold == 0. */
  threshold = clamp(threshold, 1.0e-6, 1.0);
  return threshold;
}

void main()
{
#ifdef MAT_TRANSPARENT
  init_globals();

  nodetree_surface();

  // float noise_offset = sampling_rng_1D_get(SAMPLING_TRANSPARENCY);
  float noise_offset = 0.5;
  float random_threshold = hashed_alpha_threshold(1.0, noise_offset, g_data.P);

  float transparency = avg(g_transmittance);
  if (transparency > random_threshold) {
    discard;
  }
#endif

#ifdef MAT_VELOCITY
  out_velocity = velocity_surface(interp.P + motion.prev, interp.P, interp.P + motion.next);
  out_velocity = velocity_pack(out_velocity);
#endif
}
