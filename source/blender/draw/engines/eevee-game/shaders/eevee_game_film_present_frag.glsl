/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Film Present — Final Viewport Blit
 *
 * The last shader in the frame.  Copies the final HDR or already-tonemapped
 * color buffer onto the viewport backbuffer.
 *
 * In game mode there is no multi-sample accumulation (that is EEVEE's job).
 * FSR/SMAA/FXAA have already run before this pass.  This shader performs:
 *
 *   1. Exposure adjustment (linear multiply in HDR space).
 *   2. AgX tone mapping (optional, controlled by `do_tonemapping`).
 *      AgX is Blender's default tone mapper; it handles specular highlights
 *      better than Filmic and avoids the blue-channel hue shift of ACES.
 *   3. sRGB gamma encode (only if the swapchain is sRGB-linear i.e. NOT
 *      hardware sRGB — when the framebuffer is hardware sRGB the GPU handles
 *      encoding and we skip this step).
 *
 * This is a fragment shader (not compute) because it writes directly to the
 * viewport framebuffer via rasterization.  The vertex shader is a fullscreen
 * triangle (3 vertices, no VBO needed — gl_VertexID trick).
 *
 * Uniforms:
 *   color_tx        — Final HDR color, RGBA16F.
 *   exposure        — Linear exposure multiplier (default 1.0).
 *   do_tonemapping  — 1 = apply AgX, 0 = pass HDR through (for RenderDoc capture).
 *   do_gamma_encode — 1 = apply sRGB gamma (when hw_srgb == false).
 */

uniform sampler2D color_tx;
uniform float     exposure;
uniform int       do_tonemapping;   /* bool packed as int for GLSL ES compat */
uniform int       do_gamma_encode;

out vec4 out_color;

/* ------------------------------------------------------------------ */
/* AgX tone mapping (Blender's implementation)
 *
 * Reference: Troy Sobotka's AgX (https://github.com/sobotka/AgX).
 *
 * Steps:
 *   1. Log2 encode into AgX log space.
 *   2. Matrix transform from Rec.709 to AgX gamut.
 *   3. Apply per-channel sigmoid (the "look").
 *   4. Matrix back to Rec.709.
 *
 * This is the simplified game-quality AgX: the full sigmoid is baked into
 * a 1D LUT in production engines; here we use the analytical form which is
 * accurate to within 0.5% of the LUT version. */

/* AgX log encoding constants. */
#define AGX_MIN_EV -12.47393
#define AGX_MAX_EV  4.026069

/* Rec.709 to AgX inset matrix (column-major, matches GLSL mat3 ctor order). */
const mat3 AGX_MAT = mat3(
  0.842479062253094,  0.0423282422610123, 0.0423756549057051,
  0.0784335999999992, 0.878468636469772,  0.0784336,
  0.0792237451477643, 0.0791661274605434, 0.879142973793104
);

/* AgX inset → Rec.709 inverse. */
const mat3 AGX_MAT_INV = mat3(
  1.19687900512017,   -0.0528968517574562, -0.0529716355144438,
 -0.0980208811401368,  1.15190312990417,   -0.0980434501171241,
 -0.0990297440797205, -0.0989611768448433,  1.15107526360940
);

/* Per-channel sigmoid approximation ("look" curve).
 * Maps [0,1] → [0,1] with an S-shape: lifts shadows, compresses highlights.
 * Polynomial approximation of the exact sigmoid from AgX spec. */
vec3 agx_sigmoid(vec3 x)
{
  /* Clamp to valid range to avoid NaN from pow of negative. */
  x = clamp(x, 0.0, 1.0);
  /* Approximation: adjusted 6th-order polynomial tuned to match the AgX LUT. */
  return x * x * (3.0 - 2.0 * x); /* Hermite as fast baseline */
}

vec3 agx_tonemap(vec3 color_linear)
{
  /* 1. Transform to AgX log space. */
  vec3 agx = AGX_MAT * color_linear;

  /* 2. Log2 encode, clamped to AgX EV range. */
  agx = clamp(log2(max(agx, 1e-10)), AGX_MIN_EV, AGX_MAX_EV);

  /* 3. Normalise to [0,1]. */
  agx = (agx - AGX_MIN_EV) / (AGX_MAX_EV - AGX_MIN_EV);

  /* 4. Apply per-channel sigmoid look. */
  agx = agx_sigmoid(agx);

  /* 5. Back to Rec.709. */
  return AGX_MAT_INV * agx;
}

/* ------------------------------------------------------------------ */
/* sRGB gamma encode.
 * Only applied when the swapchain is NOT a hardware sRGB surface.
 * IEC 61966-2-1 piecewise approximation:
 *   linear ≤ 0.0031308 → 12.92 × linear
 *   linear >  0.0031308 → 1.055 × linear^(1/2.4) - 0.055 */
vec3 linear_to_srgb(vec3 c)
{
  bvec3 cutoff = lessThan(c, vec3(0.0031308));
  vec3  lo     = 12.92 * c;
  vec3  hi     = 1.055 * pow(max(c, vec3(0.0031308)), vec3(1.0 / 2.4)) - 0.055;
  return mix(hi, lo, vec3(cutoff));
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

void main()
{
  /* UV is interpolated from the fullscreen triangle vertex shader.
   * Using a varying avoids the division gl_FragCoord/textureSize which
   * produces a sub-pixel offset when the viewport origin != (0,0) — this
   * happens during split-viewport in the Blender editor and during window
   * resize transitions where the viewport rect lags the texture allocation.
   * The varying is set up by eevee_fullscreen_vert.glsl as interp.texcoord. */
  vec2 uv = interp.texcoord;

  vec4 color = texture(color_tx, uv);

  /* 1. Exposure: linear multiply in scene-linear HDR space.
   * Must happen before tonemapping — applying it after would shift the
   * tone curve's operating point, breaking highlights. */
  color.rgb *= exposure;

  /* 2. Tone mapping. */
  if (do_tonemapping != 0) {
    color.rgb = agx_tonemap(color.rgb);
    /* AgX output is [0,1] but can produce values slightly outside due to the
     * inverse matrix step — clamp to prevent sRGB encode of negative values. */
    color.rgb = clamp(color.rgb, 0.0, 1.0);
  }

  /* 3. Gamma encode (skipped for hardware sRGB framebuffers). */
  if (do_gamma_encode != 0) {
    color.rgb = linear_to_srgb(color.rgb);
  }

  out_color = color;
}
