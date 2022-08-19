
/**
 * Sampling data accessors and random number generators.
 * Also contains some sample mapping functions.
 **/

#pragma BLENDER_REQUIRE(common_math_lib.glsl)

/* -------------------------------------------------------------------- */
/** \name Sampling data.
 *
 * Return a random values from Low Discrepancy Sequence in [0..1) range.
 * This value is uniform (constant) for the whole scene sample.
 * You might want to couple it with a noise function.
 * \{ */

#ifdef EEVEE_SAMPLING_DATA

float sampling_rng_1D_get(const eSamplingDimension dimension)
{
  return sampling_buf.dimensions[dimension];
}

vec2 sampling_rng_2D_get(const eSamplingDimension dimension)
{
  return vec2(sampling_buf.dimensions[dimension], sampling_buf.dimensions[dimension + 1u]);
}

vec3 sampling_rng_3D_get(const eSamplingDimension dimension)
{
  return vec3(sampling_buf.dimensions[dimension],
              sampling_buf.dimensions[dimension + 1u],
              sampling_buf.dimensions[dimension + 2u]);
}

#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Random Number Generators.
 * \{ */

/* Interlieved gradient noise by Jorge Jimenez
 * http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
 * Seeding found by Epic Game. */
float interlieved_gradient_noise(vec2 pixel, float seed, float offset)
{
  pixel += seed * (vec2(47, 17) * 0.695);
  return fract(offset + 52.9829189 * fract(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}

/* From: http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html */
float van_der_corput_radical_inverse(uint bits)
{
#if 0 /* Reference */
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
#else
  bits = bitfieldReverse(bits);
#endif
  /* Same as dividing by 0x100000000. */
  return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley_2d(float i, float sample_count)
{
  vec2 rand;
  rand.x = i / sample_count;
  rand.y = van_der_corput_radical_inverse(uint(i));
  return rand;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Distribution mapping.
 *
 * Functions mapping input random numbers to sampling shapes (i.e: hemisphere).
 * \{ */

/* Given 2 random number in [0..1] range, return a random unit disk sample. */
vec2 sample_disk(vec2 noise)
{
  float angle = noise.x * M_2PI;
  return vec2(cos(angle), sin(angle)) * sqrt(noise.y);
}

/* This transform a 2d random sample (in [0..1] range) to a sample located on a cylinder of the
 * same range. This is because the sampling functions expect such a random sample which is
 * normally precomputed. */
vec3 sample_cylinder(vec2 rand)
{
  float theta = rand.x;
  float phi = (rand.y - 0.5) * M_2PI;
  float cos_phi = cos(phi);
  float sin_phi = sqrt(1.0 - sqr(cos_phi)) * sign(phi);
  return vec3(theta, cos_phi, sin_phi);
}

/** \} */
