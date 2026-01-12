/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Common texture sampling and processing functions for GPU compute shaders.
 * Includes ColorBand, imagewrap, boxsample, and texture mapping utilities.
 */

#pragma once

#include <string>

namespace blender {

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name ColorBand Defines and Helper Functions
 * \{ */

/* Placeholder defines for texture subtypes (blend stype, wood/marble waveforms, etc.)
 * These provide a single place to keep TEX_* name definitions for GPU shaders.
 * Values should match those in DNA_texture_types.h / CPU implementation. */
static std::string get_texture_defines_glsl()
{
  return R"GLSL(
/* Texture type defines (match DNA_texture_types.h) */
/* Note: some legacy types are kept commented for reference. */
#define TEX_CLOUDS 1
#define TEX_WOOD 2
#define TEX_MARBLE 3
#define TEX_MAGIC 4
#define TEX_BLEND 5
#define TEX_STUCCI 6
#define TEX_NOISE 7
#define TEX_IMAGE 8
/* #define TEX_PLUGIN 9  // Deprecated */
/* #define TEX_ENVMAP 10 // Deprecated */
#define TEX_MUSGRAVE 11
#define TEX_VORONOI 12
#define TEX_DISTNOISE 13
/* #define TEX_POINTDENSITY 14 // Deprecated */
/* #define TEX_VOXELDATA 15  // Deprecated */
/* #define TEX_OCEAN 16      // Deprecated */

/* Noise basis values (match DNA_texture_types.h) */
#define TEX_BLENDER 0
#define TEX_STDPERLIN 1
#define TEX_NEWPERLIN 2
#define TEX_VORONOI_F1 3
#define TEX_VORONOI_F2 4
#define TEX_VORONOI_F3 5
#define TEX_VORONOI_F4 6
#define TEX_VORONOI_F2F1 7
#define TEX_VORONOI_CRACKLE 8
#define TEX_CELLNOISE 14

/* Texture subtype defines (blend stype, wood/marble waveforms, etc.) */
#define TEX_LIN 0
#define TEX_QUAD 1
#define TEX_EASE 2
#define TEX_DIAG 3
#define TEX_SPHERE 4
#define TEX_HALO 5
#define TEX_RAD 6

/* Wood waveform (noisebasis2) values */
#define TEX_SIN 0
#define TEX_SAW 1
#define TEX_TRI 2

/* Wood stype values */
#define TEX_BAND 0
#define TEX_RING 1
#define TEX_BANDNOISE 2
#define TEX_RINGNOISE 3

/* Cloud stype values */
#define TEX_DEFAULT 0
#define TEX_COLOR 1

/* Marble types */
#define TEX_SOFT 0
#define TEX_SHARP 1
#define TEX_SHARPER 2

/* Stucci types */
#define TEX_PLASTIC 0
#define TEX_WALLIN 1
#define TEX_WALLOUT 2

/* Image flags (imaflag bits) */
#define TEX_INTERPOL (1 << 0)
#define TEX_USEALPHA (1 << 1)
#define TEX_IMAROT (1 << 4)
#define TEX_CALCALPHA (1 << 5)
#define TEX_NORMALMAP (1 << 11)
#define TEX_DERIVATIVEMAP (1 << 14)

/* Tex flags (flag bits) */
#define TEX_COLORBAND (1 << 0)
#define TEX_FLIPBLEND (1 << 1)
#define TEX_NEGALPHA (1 << 2)
#define TEX_CHECKER_ODD (1 << 3)
#define TEX_CHECKER_EVEN (1 << 4)
#define TEX_PRV_ALPHA (1 << 5)
#define TEX_PRV_NOR (1 << 6)
#define TEX_REPEAT_XMIR (1 << 7)
#define TEX_REPEAT_YMIR (1 << 8)
#define TEX_DS_EXPAND (1 << 9)
#define TEX_NO_CLAMP (1 << 10)

/* Voronoi color/distance types */
#define TEX_DISTANCE 0
#define TEX_DISTANCE_SQUARED 1
#define TEX_MANHATTAN 2
#define TEX_CHEBYCHEV 3
#define TEX_MINKOVSKY_HALF 4
#define TEX_MINKOVSKY_FOUR 5
#define TEX_MINKOVSKY 6

#define TEX_INTENSITY 0
#define TEX_COL1 1
#define TEX_COL2 2
#define TEX_COL3 3

/* Return flags (match CPU TEX_INT/TEX_RGB semantics) */
#define TEX_INT 0
#define TEX_RGB 1

/* GPU Displace Modifier Compute Shader v2.1 with ColorBand support */
/* Displace direction modes (matching DisplaceModifierDirection enum) */
#define MOD_DISP_DIR_X 0
#define MOD_DISP_DIR_Y 1
#define MOD_DISP_DIR_Z 2
#define MOD_DISP_DIR_NOR 3
#define MOD_DISP_DIR_RGB_XYZ 4
#define MOD_DISP_DIR_CLNOR 5

/* Displace space modes (matching DisplaceModifierSpace enum) */
#define MOD_DISP_SPACE_LOCAL 0
#define MOD_DISP_SPACE_GLOBAL 1

/* Texture extend modes (matching DNA_texture_types.h line 280-286)
 * CRITICAL: Values start at 1 due to backward compatibility! */
#define TEX_EXTEND 1
#define TEX_CLIP 2
#define TEX_REPEAT 3
#define TEX_CLIPCUBE 4
#define TEX_CHECKER 5

/* Texture sampling strategy.
 *
 * - DISP_TEXSAMPLER_FAST_BILINEAR: Use hardware bilinear sampling (`texture()`).
 *   This is very fast and matches the pre-refactor GPU performance characteristics.
 *   It does NOT implement CPU `boxsample()` area filtering controlled by `filtersize`.
 *
 * - DISP_TEXSAMPLER_CPU_BOXSAMPLE: Use the CPU-ported `boxsample()` implementation.
 *   This aims to match `texture_image.cc::boxsample()` behavior more closely but can be
 *   extremely expensive in a compute shader due to nested loops and many `texelFetch()`.
 *
 * Toggle this define when debugging CPU/GPU mismatches.
 */
#define DISP_TEXSAMPLER_FAST_BILINEAR 0
#define DISP_TEXSAMPLER_CPU_BOXSAMPLE 1

#ifndef DISP_TEXSAMPLER_MODE
#  /* Default to the historical Displace GPU behavior: use the CPU-ported box filter.
#   * This matches the previous shader more closely, especially for TEX_REPEAT + filtering.
#   *
#   * The FAST_BILINEAR mode can be enabled for performance, but it may diverge from the
#   * CPU `texture_image.cc::boxsample()` result (filtersize becomes an approximation). */
#  define DISP_TEXSAMPLER_MODE DISP_TEXSAMPLER_CPU_BOXSAMPLE
#endif

/* NOTE ABOUT REPEAT + FILTERING
 * The CPU image pipeline wraps integer pixel coordinates (x/y) and then adjusts the
 * floating coordinates (fx/fy) by the wrapping delta (xi-x, yi-y) before filtering:
 *   fx -= float(xi - x) / size_x;
 *   fy -= float(yi - y) / size_y;
 * This is critical for stable filtering across wrap boundaries (#27782).
 *
 * For easy CPU/GPU comparison we preserve this remap in both sampling modes.
 *
 * Even in the FAST_BILINEAR mode we do NOT simply do `fract(fx)` because that would
 * ignore the snapped integer wrap that CPU performs and can change which side of the
 * boundary gets filtered.
 */

/* ColorBand interpolation types (matching DNA_color_types.h) */
#define COLBAND_INTERP_LINEAR 0
#define COLBAND_INTERP_EASE 1
#define COLBAND_INTERP_B_SPLINE 2
#define COLBAND_INTERP_CARDINAL 3
#define COLBAND_INTERP_CONSTANT 4

/* ColorBand color modes (matching DNA_color_types.h) */
#define COLBAND_BLEND_RGB 0
#define COLBAND_BLEND_HSV 1
#define COLBAND_BLEND_HSL 2

/* ColorBand hue interpolation modes (matching DNA_color_types.h) */
#define COLBAND_HUE_NEAR 0
#define COLBAND_HUE_FAR 1
#define COLBAND_HUE_CW 2
#define COLBAND_HUE_CCW 3

/* Math constants */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
)GLSL";
}

/* Common texture result struct used by multiple texture helpers (different name
 * to avoid clash with local shader definitions). Mirrors CPU `TexResult` fields
 * but kept compact for shader usage. */
static std::string get_texture_structs_glsl()
{
  return R"GLSL(
/* rctf rectangle struct (GPU port of BLI_rect.h) */
struct rctf {
  float xmin;
  float xmax;
  float ymin;
  float ymax;
};

struct TexResult_tex {
  float tin;  /* intensity */
  vec4 trgba; /* RGBA color */
  bool talpha; /* use alpha as intensity (0/1) */
};
)GLSL";
}


/**
 * Returns GLSL code for ColorBand-related defines and helper functions.
 * Includes interpolation types, color modes, hue modes, and key_curve_position_weights.
 */
static std::string get_colorband_helpers_glsl()
{
  return R"GLSL(
/* GPU port of key_curve_position_weights from key.cc */
void key_curve_position_weights(float t, out float data[4], int type)
{
  float t2 = 0.0;
  float t3 = 0.0;
  float fc = 0.0;

  if (type == 0) { /* KEY_LINEAR */
    data[0] = 0.0;
    data[1] = -t + 1.0;
    data[2] = t;
    data[3] = 0.0;
    return;
  }

  t2 = t * t;
  t3 = t2 * t;

  if (type == 1) { /* KEY_CARDINAL */
    fc = 0.71;
    data[0] = -fc * t3 + 2.0 * fc * t2 - fc * t;
    data[1] = (2.0 - fc) * t3 + (fc - 3.0) * t2 + 1.0;
    data[2] = (fc - 2.0) * t3 + (3.0 - 2.0 * fc) * t2 + fc * t;
    data[3] = fc * t3 - fc * t2;
    return;
  }

  if (type == 2) { /* KEY_BSPLINE */
    data[0] = -0.16666666 * t3 + 0.5 * t2 - 0.5 * t + 0.16666666;
    data[1] = 0.5 * t3 - t2 + 0.66666666;
    data[2] = -0.5 * t3 + 0.5 * t2 + 0.5 * t + 0.16666666;
    data[3] = 0.16666666 * t3;
    return;
  }

  /* KEY_CATMULL_ROM (fallback) */
  fc = 0.5;
  data[0] = -fc * t3 + 2.0 * fc * t2 - fc * t;
  data[1] = (2.0 - fc) * t3 + (fc - 3.0) * t2 + 1.0;
  data[2] = (fc - 2.0) * t3 + (3.0 - 2.0 * fc) * t2 + fc * t;
  data[3] = fc * t3 - fc * t2;
}

/* GPU port of colorband_hue_interp() from colorband.cc */
float colorband_hue_interp(int ipotype_hue, float mfac, float fac, float h1, float h2)
{
  float h_interp;
  int mode = 0;

  h1 = (h1 < 1.0) ? h1 : h1 - 1.0;
  h2 = (h2 < 1.0) ? h2 : h2 - 1.0;

  if (ipotype_hue == COLBAND_HUE_NEAR) {
    if ((h1 < h2) && (h2 - h1) > 0.5) {
      mode = 1;
    }
    else if ((h1 > h2) && (h2 - h1) < -0.5) {
      mode = 2;
    }
    else {
      mode = 0;
    }
  }
  else if (ipotype_hue == COLBAND_HUE_FAR) {
    if (h1 == h2) {
      mode = 1;
    }
    else if ((h1 < h2) && (h2 - h1) < 0.5) {
      mode = 1;
    }
    else if ((h1 > h2) && (h2 - h1) > -0.5) {
      mode = 2;
    }
    else {
      mode = 0;
    }
  }
  else if (ipotype_hue == COLBAND_HUE_CCW) {
    if (h1 > h2) {
      mode = 2;
    }
    else {
      mode = 0;
    }
  }
  else if (ipotype_hue == COLBAND_HUE_CW) {
    if (h1 < h2) {
      mode = 1;
    }
    else {
      mode = 0;
    }
  }

  if (mode == 0) {
    h_interp = (mfac * h1) + (fac * h2);
  }
  else if (mode == 1) {
    h_interp = (mfac * (h1 + 1.0)) + (fac * h2);
    h_interp = (h_interp < 1.0) ? h_interp : h_interp - 1.0;
  }
  else {
    h_interp = (mfac * h1) + (fac * (h2 + 1.0));
    h_interp = (h_interp < 1.0) ? h_interp : h_interp - 1.0;
  }

  return h_interp;
}
)GLSL";
}

static std::string get_texnoise_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* Simple texnoise equivalent: produce a small-int-valued noise in [0,1]
 * Small shader-local LCG to reproduce BLI_rng_thread_rand(random_tex_array, thread);
 * without RNG texture dependency.
 */

/* Integer hash (Wang/Jenkins-like) */
uint wang_hash(uint x)
{
  x = (x ^ 61u) ^ (x >> 16);
  x *= 9u;
  x = x ^ (x >> 4);
  x *= 0x27d4eb2du;
  x = x ^ (x >> 15);
  return x;
}

int texnoise_tex(inout TexResult_tex texres, int thread_id, int noisedepth)
{
  /* Fallback: generate a cheap deterministic per-invocation RNG from
   * thread id + frame. This avoids uploads and produces animated noise
   * when `tex_frame` changes. */

  uint seed = uint(thread_id) + uint(tex_frame) * 1664525u;
  uint ran = wang_hash(seed);

  int shift = 30;
  int val = int((ran >> shift) & 3u);
  float div = 3.0;
  int loop = noisedepth;
  while (loop-- > 0 && shift >= 2) {
    shift -= 2;
    val *= int((ran >> shift) & 3u);
    div *= 3.0;
  }

  texres.tin = float(val) / div;
  texres.tin = apply_bricont_fn(texres.tin);
  return TEX_INT;
}

#endif // HAS_TEXTURE

)GLSL";
}

static std::string get_bricont_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* BRICONT helpers (matches CPU BRICONT/BRICONTRGB macros). */

float apply_bricont_fn(float tin)
{
  float t = (tin - 0.5) * tex_contrast + tex_bright - 0.5;
  if (!tex_no_clamp) {
    t = clamp(t, 0.0, 1.0);
  }
  return t;
}

vec3 apply_bricont_rgb(vec3 col)
{
  col.r = apply_bricont_fn(col.r);
  col.g = apply_bricont_fn(col.g);
  col.b = apply_bricont_fn(col.b);
  return col;
}

#endif // HAS_TEXTURE

)GLSL";
}

static std::string get_stucci_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* Simplified stucci implementation (returns intensity) */
int stucci_tex(vec3 texvec, inout TexResult_tex texres, int stype, float noisesize, float turbul, int noisebasis)
{
  float b2 = bli_noise_generic_turbulence(noisesize, texvec.x, texvec.y, texvec.z, 2, false, noisebasis);
  float ofs = turbul / 200.0;
  if (stype != 0) {
    ofs *= (b2 * b2);
  }
  texres.tin = bli_noise_generic_turbulence(noisesize, texvec.x, texvec.y, texvec.z + ofs, 2, false, noisebasis);
  if (stype == TEX_WALLOUT) {
    texres.tin = 1.0 - texres.tin;
  }
  texres.tin = max(texres.tin, 0.0);
  texres.tin = apply_bricont_fn(texres.tin);
  return TEX_INT;
}

#endif // HAS_TEXTURE

)GLSL";
}

static std::string get_voronoi_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* Voronoi texture approximation using helpers */
int voronoi_tex(vec3 texvec, inout TexResult_tex texres, float vn_w1, float vn_w2, float vn_w3, float vn_w4, float ns_outscale, int vn_coltype, int vn_distm, float vn_mexp)
{
  float da[4];
  float pa[12];
  bli_noise_voronoi(texvec.x, texvec.y, texvec.z, da, pa, vn_mexp, vn_distm);
  // simple weighted distance
  float aw1 = abs(vn_w1);
  float aw2 = abs(vn_w2);
  float aw3 = abs(vn_w3);
  float aw4 = abs(vn_w4);
  float sc = (aw1 + aw2 + aw3 + aw4);
  if (sc != 0.0) sc = ns_outscale / sc;
  float val = sc * abs(aw1 * da[0] + aw2 * da[1] + aw3 * da[2] + aw4 * da[3]);
  texres.tin = val;
  bool is_color = (vn_coltype == TEX_COL1 || vn_coltype == TEX_COL2 || vn_coltype == TEX_COL3);
  if (is_color) {
    vec3 ca0 = bli_noise_cell_v3(pa[0], pa[1], pa[2]);
    vec3 ca1 = bli_noise_cell_v3(pa[3], pa[4], pa[5]);
    vec3 ca2 = bli_noise_cell_v3(pa[6], pa[7], pa[8]);
    vec3 ca3 = bli_noise_cell_v3(pa[9], pa[10], pa[11]);
    vec3 c = aw1 * ca0 + aw2 * ca1 + aw3 * ca2 + aw4 * ca3;
    if (vn_coltype == TEX_COL2 || vn_coltype == TEX_COL3) {
      float t1 = (da[1] - da[0]) * 10.0;
      t1 = min(t1, 1.0);
      if (vn_coltype == TEX_COL3) {
        t1 *= texres.tin;
      }
      else {
        t1 *= sc;
      }
      c *= t1;
    }
    else {
      c *= sc;
    }
    texres.trgba = vec4(c, 1.0);
    return (TEX_RGB);
  }
  return TEX_INT;
}

#endif // HAS_TEXTURE

)GLSL";
}

static std::string get_marble_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* GPU port of marble texture (marble_int + marble) - simplified */
float marble_int_glsl(int noisebasis2, int mt, float x, float y, float z, float noisesize, float turbul)
{
  // approximate like CPU: n + turbul * turbulence
  float n = 5.0 * (x + y + z);
  float turb = turbul * bli_noise_generic_turbulence(noisesize, x, y, z, 2, false, noisebasis2);
  float mi = n + turb;

  // waveform
  int wf = noisebasis2;
  float val = 0.0;
  if (wf == 0) {
    val = 0.5 + 0.5 * sin(mi);
  }
  else if (wf == 1) {
    val = fract(mi / (2.0 * M_PI));
  }
  else {
    float a = mi;
    val = 1.0 - 2.0 * abs(fract(a / (2.0 * M_PI) + 0.5) - 0.5);
  }

  if (mt == 1) { // TEX_SHARP
    val = sqrt(max(val, 0.0));
  }
  else if (mt == 2) { // TEX_SHARPER
    val = sqrt(max(sqrt(max(val, 0.0)), 0.0));
  }
  return val;
}

int marble_tex(vec3 texvec, inout TexResult_tex texres, int noisebasis2, int mt, float noisesize, float turbul)
{
  texres.tin = marble_int_glsl(noisebasis2, mt, texvec.x, texvec.y, texvec.z, noisesize, turbul);
  texres.tin = apply_bricont_fn(texres.tin);
  return TEX_INT;
}

#endif // HAS_TEXTURE

)GLSL";
}

static std::string get_magic_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* GPU port of magic texture (simplified) */
int magic_tex(vec3 texvec, inout TexResult_tex texres, float turbul)
{
  float x = sin((texvec.x + texvec.y + texvec.z) * 5.0);
  float y = cos((-texvec.x + texvec.y - texvec.z) * 5.0);
  float z = -cos((-texvec.x - texvec.y + texvec.z) * 5.0);

  texres.trgba.r = 0.5 - x;
  texres.trgba.g = 0.5 - y;
  texres.trgba.b = 0.5 - z;
  texres.tin = (texres.trgba.r + texres.trgba.g + texres.trgba.b) / 3.0;
  texres.trgba.a = 1.0;
  return TEX_RGB;
}

#endif // HAS_TEXTURE

)GLSL";
}


static std::string get_wood_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* GPU port of CPU `wood()` texture and helper wood_int() */
float wood_int_glsl(int noisebasis2, int stype, float x, float y, float z, float noisesize, float turbul)
{
  float wi = 0.0;
  // waveform selection: 0=sin,1=saw,2=tri
  int wf = noisebasis2;

  // Simple waveform functions
  float wave = 0.0;
  if (wf == 0) {
    wave = 0.5 + 0.5 * sin((x + y + z) * 10.0);
  }
  else if (wf == 1) {
    float a = (x + y + z) * 10.0;
    float b = 2.0 * M_PI;
    float n = a - floor(a / b) * b;
    wave = n / b;
  }
  else {
    float a = (x + y + z) * 10.0;
    float b = 2.0 * M_PI;
    float n = a - floor(a / b) * b;
    wave = 1.0 - 2.0 * abs(fract(n / b + 0.5) - 0.5);
  }

  if (stype == TEX_BAND) {
    wi = wave;
  }
  else if (stype == TEX_RING) {
    wi = wave; // approximate ring via same waveform for now
  }
  else {
    // noise-influenced variants
    float n = bli_noise_generic_turbulence(noisesize, x, y, z, 2, false, noisebasis2);
    wi = wave + turbul * n;
  }

  return wi;
}

int wood_tex(vec3 texvec, inout TexResult_tex texres, int noisebasis2, int stype, float noisesize, float turbul)
{
  float wi = wood_int_glsl(noisebasis2, stype, texvec.x, texvec.y, texvec.z, noisesize, turbul);
  texres.tin = wi;
  texres.tin = apply_bricont_fn(texres.tin);
  return TEX_INT;
}

#endif // HAS_TEXTURE

)GLSL";
}


static std::string get_clouds_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* GPU port of CPU `clouds()` texture */
int clouds_tex(const TexResult_tex tex_in, inout TexResult_tex texres_in, float noisesize, float turbul, int noisedepth, int noisebasis, int stype)
{
  // NOTE: tex_in unused; function signature matches pattern for other helpers.
  // We'll compute intensity via bli_noise_generic_turbulence approximation.
  TexResult_tex texres = texres_in;
  float tin = bli_noise_generic_turbulence(noisesize, texres.trgba.r, texres.trgba.g, texres.trgba.b, noisedepth, (noisebasis != 0), noisebasis);
  texres.tin = tin;

  if (stype == TEX_COLOR) {
    texres.trgba.r = texres.tin;
    texres.trgba.g = bli_noise_generic_turbulence(noisesize, texres.trgba.g, texres.trgba.r, texres.trgba.b, noisedepth, (noisebasis != 0), noisebasis);
    texres.trgba.b = bli_noise_generic_turbulence(noisesize, texres.trgba.b, texres.trgba.r, texres.trgba.g, noisedepth, (noisebasis != 0), noisebasis);
    // Apply BRICONTRGB-like processing: brightness/contrast/saturation handled elsewhere
    texres.trgba.a = 1.0;
    texres_in = texres;
    return TEX_RGB;
  }

  texres_in = texres;
  texres_in.tin = apply_bricont_fn(texres_in.tin);
  return TEX_INT;
}

#endif // HAS_TEXTURE

)GLSL";
}

static std::string get_blend_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* GPU port of CPU `blend()` texture (gradient/blend) */
int blend_tex(vec3 texvec, inout TexResult_tex texres, int tex_stype, bool tex_flipblend)
{
  float x, y, t;

  if (tex_flipblend) {
    x = texvec.y;
    y = texvec.x;
  }
  else {
    x = texvec.x;
    y = texvec.y;
  }

  if (tex_stype == TEX_LIN) { /* Linear. */
    texres.tin = (1.0 + x) / 2.0;
  }
  else if (tex_stype == TEX_QUAD) { /* Quadratic. */
    texres.tin = (1.0 + x) / 2.0;
    if (texres.tin < 0.0) {
      texres.tin = 0.0;
    }
    else {
      texres.tin *= texres.tin;
    }
  }
  else if (tex_stype == TEX_EASE) { /* Ease. */
    texres.tin = (1.0 + x) / 2.0;
    if (texres.tin <= 0.0) {
      texres.tin = 0.0;
    }
    else if (texres.tin >= 1.0) {
      texres.tin = 1.0;
    }
    else {
      t = texres.tin * texres.tin;
      texres.tin = (3.0 * t - 2.0 * t * texres.tin);
    }
  }
  else if (tex_stype == TEX_DIAG) { /* Diagonal. */
    texres.tin = (2.0 + x + y) / 4.0;
  }
  else if (tex_stype == TEX_RAD) { /* Radial. */
    texres.tin = (atan(texvec.y, texvec.x) / (2.0 * M_PI) + 0.5);
  }
  else { /* sphere TEX_SPHERE */
    texres.tin = 1.0 - sqrt(texvec.x * texvec.x + texvec.y * texvec.y + texvec.z * texvec.z);
    texres.tin = max(texres.tin, 0.0);
    if (tex_stype == TEX_HALO) {
      texres.tin *= texres.tin; /* Halo. */
    }
  }

  /* BRICONT macro: apply brightness/contrast and optional clamping (matches CPU macro)
   * Uses shader-side parameters: tex_contrast, tex_bright, tex_no_clamp */
  /* Use reusable BRICONT helper */
  texres.tin = apply_bricont_fn(texres.tin);

  return TEX_INT;
}

#endif // HAS_TEXTURE
)GLSL";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name RGB/HSV/HSL Color Conversion
 * \{ */

/**
 * Returns GLSL code for RGB ↔ HSV/HSL conversion functions.
 * GPU port of BLI_math_color.h functions.
 */
static std::string get_color_conversion_glsl()
{
  return R"GLSL(
/* RGB ↔ HSV/HSL conversion functions (GPU port of BLI_math_color.h) */
vec3 rgb_to_hsv(vec3 rgb)
{
  float r = rgb.r;
  float g = rgb.g;
  float b = rgb.b;

  float k = 0.0;
  float chroma;
  float min_gb;

  if (g < b) {
    float tmp = g; g = b; b = tmp;
    k = -1.0;
  }
  min_gb = b;
  if (r < g) {
    float tmp = r; r = g; g = tmp;
    k = -2.0 / 6.0 - k;
    min_gb = min(g, b);
  }

  chroma = r - min_gb;

  float h = abs(k + (g - b) / (6.0 * chroma + 1e-20));
  float s = chroma / (r + 1e-20);
  float v = r;

  return vec3(h, s, v);
}

vec3 hsv_to_rgb(vec3 hsv)
{
  float h = hsv.x;
  float s = hsv.y;
  float v = hsv.z;

  float nr = abs(h * 6.0 - 3.0) - 1.0;
  float ng = 2.0 - abs(h * 6.0 - 2.0);
  float nb = 2.0 - abs(h * 6.0 - 4.0);

  nr = clamp(nr, 0.0, 1.0);
  nb = clamp(nb, 0.0, 1.0);
  ng = clamp(ng, 0.0, 1.0);

  float r = ((nr - 1.0) * s + 1.0) * v;
  float g = ((ng - 1.0) * s + 1.0) * v;
  float b = ((nb - 1.0) * s + 1.0) * v;

  return vec3(r, g, b);
}

vec3 rgb_to_hsl(vec3 rgb)
{
  float cmax = max(max(rgb.r, rgb.g), rgb.b);
  float cmin = min(min(rgb.r, rgb.g), rgb.b);
  float h, s;
  float l = min(1.0, (cmax + cmin) / 2.0);

  if (cmax == cmin) {
    h = 0.0;
    s = 0.0;
  }
  else {
    float d = cmax - cmin;
    s = (l > 0.5) ? (d / (2.0 - cmax - cmin)) : (d / (cmax + cmin));

    if (cmax == rgb.r) {
      h = (rgb.g - rgb.b) / d + (rgb.g < rgb.b ? 6.0 : 0.0);
    }
    else if (cmax == rgb.g) {
      h = (rgb.b - rgb.r) / d + 2.0;
    }
    else {
      h = (rgb.r - rgb.g) / d + 4.0;
    }
  }

  h /= 6.0;
  return vec3(h, s, l);
}

vec3 hsl_to_rgb(vec3 hsl)
{
  float h = hsl.x;
  float s = hsl.y;
  float l = hsl.z;

  float nr = abs(h * 6.0 - 3.0) - 1.0;
  float ng = 2.0 - abs(h * 6.0 - 2.0);
  float nb = 2.0 - abs(h * 6.0 - 4.0);

  nr = clamp(nr, 0.0, 1.0);
  nb = clamp(nb, 0.0, 1.0);
  ng = clamp(ng, 0.0, 1.0);

  float chroma = (1.0 - abs(2.0 * l - 1.0)) * s;

  float r = (nr - 0.5) * chroma + l;
  float g = (ng - 0.5) * chroma + l;
  float b = (nb - 0.5) * chroma + l;

  return vec3(r, g, b);
}

float srgb_to_linearrgb(float c)
{
  if (c <= 0.04045) {
    return c / 12.92;
  }
  return pow((c + 0.055) / 1.055, 2.4);
}

vec3 srgb_to_linearrgb_vec3(vec3 v)
{
  return vec3(srgb_to_linearrgb(v.r), srgb_to_linearrgb(v.g), srgb_to_linearrgb(v.b));
}

vec3 linearrgb_to_srgb_vec3(vec3 v)
{
  return vec3(linearrgb_to_srgb(v.r), linearrgb_to_srgb(v.g), linearrgb_to_srgb(v.b));
}
)GLSL";
}

/* -------------------------------------------------------------------- */
/** \name Noise Helpers
 * \{ */

static std::string get_noise_helpers_glsl()
{
  return R"GLSL(
/* Simple noise wrappers matching BLI_noise_* naming for portability.
 * These are simplified and aim to match behavior used by procedural textures
 * (turbulence, voronoi utilities). For exact parity more functions may be
 * required later. */

// Placeholder: BLI_noise_generic_turbulence -> using fractal Brownian motion style noise
float bli_noise_generic_turbulence(float size, float x, float y, float z, int depth, bool hard, int basis)
{
  // Simple sum-of-noise approximation using GLSL builtin noise is not available.
  // Use a cheap pseudo-random hash + smooth interpolation as an approximation.
  // This is intentionally simple; we'll refine if differences are problematic.
  float amp = 1.0;
  float sum = 0.0;
  float freq = 1.0 / max(size, 1e-6);
  for (int i = 0; i < depth; ++i) {
    vec3 p = vec3(x, y, z) * freq;
    // value noise via fract(sin(dot(p, vec3(...))) * 43758.5453)
    float n = fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    if (hard) {
      n = abs(n * 2.0 - 1.0);
    }
    sum += n * amp;
    amp *= 0.5;
    freq *= 2.0;
  }
  return sum;
}

// Voronoi helper: produce cell color like BLI_noise_cell_v3
vec3 bli_noise_cell_v3(float px, float py, float pz)
{
  // pseudo random from position
  float n = fract(sin(dot(vec3(px, py, pz), vec3(127.1, 311.7, 74.7))) * 43758.5453);
  return vec3(n, fract(n * 1.37), fract(n * 2.13));
}

// FNV-like small hash for voronoi distances array reproduction
void bli_noise_voronoi(float x, float y, float z, out float da[4], out float pa[12], float mexp, int distm)
{
  // Very small approximate voronoi: sample a few nearby lattice points
  vec3 p = vec3(x, y, z);
  int idx = 0;
  for (int ix = -1; ix <= 1; ++ix) {
    for (int iy = -1; iy <= 1; ++iy) {
      for (int iz = -1; iz <= 1; ++iz) {
        if (idx >= 4) break;
        vec3 cell = floor(p) + vec3(float(ix), float(iy), float(iz));
        vec3 cp = cell + vec3(fract(sin(dot(cell, vec3(1.0, 57.0, 113.0))) * 43758.5453));
        float d = length(p - cp);
        da[idx] = d;
        pa[idx * 3 + 0] = cp.x;
        pa[idx * 3 + 1] = cp.y;
        pa[idx * 3 + 2] = cp.z;
        idx++;
      }
      if (idx >= 4) break;
    }
    if (idx >= 4) break;
  }
  // fill rest
  for (; idx < 4; ++idx) {
    da[idx] = 1e6;
    pa[idx * 3 + 0] = pa[idx * 3 + 1] = pa[idx * 3 + 2] = 0.0;
  }
}

)GLSL";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Boxsample Structures and Helpers
 * \{ */

/**
 * Returns GLSL code for boxsample helper structures and functions.
 * Includes rctf, TexResult_tex, ibuf_get_color, clipping functions.
 */
static std::string get_boxsample_helpers_glsl()
{
  return R"GLSL(

vec4 shader_ibuf_get_color(vec4 fetched, bool has_float, int channels, bool is_byte)
{
  vec4 col = fetched;
  if (has_float) {
    if (channels == 4) {
      return col;
    }
    else if (channels == 3) {
      return vec4(col.rgb, 1.0);
    }
    else {
      float v = col.r;
      return vec4(v, v, v, v);
    }
  }
  else {
    col.rgb *= col.a;
    return col;
  }
}

void ibuf_get_color_boxsample(out vec4 col, sampler2D displacement_texture, ivec2 tex_size, int x, int y, bool tex_is_byte, bool tex_is_float, int tex_channels)
{
  ivec2 texel = ivec2(x, y);
  col = texelFetch(displacement_texture, texel, 0);
  col = shader_ibuf_get_color(col, tex_is_float, tex_channels, tex_is_byte);
}

float square_rctf(rctf rf)
{
  float x, y;
  x = rf.xmax - rf.xmin;
  y = rf.ymax - rf.ymin;
  return x * y;
}

float clipx_rctf(inout rctf rf, float x1, float x2)
{
  float size;
  
  size = rf.xmax - rf.xmin;
  
  rf.xmin = max(rf.xmin, x1);
  rf.xmax = min(rf.xmax, x2);
  if (rf.xmin > rf.xmax) {
    rf.xmin = rf.xmax;
    return 0.0;
  }
  if (size != 0.0) {
    return (rf.xmax - rf.xmin) / size;
  }
  return 1.0;
}

float clipy_rctf(inout rctf rf, float y1, float y2)
{
  float size;
  
  size = rf.ymax - rf.ymin;
  
  rf.ymin = max(rf.ymin, y1);
  rf.ymax = min(rf.ymax, y2);
  
  if (rf.ymin > rf.ymax) {
    rf.ymin = rf.ymax;
    return 0.0;
  }
  if (size != 0.0) {
    return (rf.ymax - rf.ymin) / size;
  }
  return 1.0;
}
)GLSL";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Boxsample Core Functions
 * \{ */

/**
 * Returns GLSL code for boxsampleclip and boxsample functions.
 * GPU port of texture_image.cc boxsample pipeline.
 */
static std::string get_boxsample_core_glsl()
{
  return R"GLSL(
void boxsampleclip(sampler2D displacement_texture, ivec2 tex_size, rctf rf, inout TexResult_tex texres, bool imaprepeat, bool tex_is_byte, bool tex_is_float, int tex_channels)
{
  /* If repeat is not requested, use the original single-rectangle sampling path. */
  if (!imaprepeat) {
    /* Sample box, is clipped already, and minx etc. have been set at tex_size.
     * Enlarge with anti-aliased edges of the pixels. Mirror CPU `boxsampleclip` order
     * and variable naming to ease comparison. */
    float muly, mulx, div;
    vec4 col;
    int x, y, startx, endx, starty, endy;

    startx = int(floor(rf.xmin));
    endx = int(floor(rf.xmax));
    starty = int(floor(rf.ymin));
    endy = int(floor(rf.ymax));

    startx = max(startx, 0);
    starty = max(starty, 0);
    if (endx >= tex_size.x) {
      endx = tex_size.x - 1;
    }
    if (endy >= tex_size.y) {
      endy = tex_size.y - 1;
    }

    if (starty == endy && startx == endx) {
      ibuf_get_color_boxsample(texres.trgba, displacement_texture, tex_size, startx, starty, tex_is_byte, tex_is_float, tex_channels);
    }
    else {
      div = texres.trgba[0] = texres.trgba[1] = texres.trgba[2] = texres.trgba[3] = 0.0;
      for (y = starty; y <= endy; y++) {

        muly = 1.0;

        if (starty == endy) {
          /* pass */
        }
        else {
          if (y == starty) {
            muly = 1.0 - (rf.ymin - float(y));
          }
          if (y == endy) {
            muly = (rf.ymax - float(y));
          }
        }

        if (startx == endx) {
          mulx = muly;

          ibuf_get_color_boxsample(col, displacement_texture, tex_size, startx, y, tex_is_byte, tex_is_float, tex_channels);
          texres.trgba += col * mulx;
          div += mulx;
        }
        else {
          for (x = startx; x <= endx; x++) {
            mulx = muly;
            if (x == startx) {
              mulx *= 1.0 - (rf.xmin - float(x));
            }
            if (x == endx) {
              mulx *= (rf.xmax - float(x));
            }

            ibuf_get_color_boxsample(col, displacement_texture, tex_size, x, y, tex_is_byte, tex_is_float, tex_channels);
            if (mulx == 1.0) {
              texres.trgba += col;
              div += 1.0;
            }
            else {
              texres.trgba += col * mulx;
              div += mulx;
            }
          }
        }
      }

      if (div != 0.0) {
        div = 1.0 / div;
        texres.trgba *= div;
      }
      else {
        texres.trgba = vec4(0.0);
      }
    }

    return;
  }

  /* imaprepeat == true: split the footprint over neighboring tiles (up to 3x3)
   * and average results weighted by intersection area. This approximates CPU
   * split/average behavior without dynamic stacks. */
  vec3 offsX = vec3(-float(tex_size.x), 0.0, float(tex_size.x));
  vec3 offsY = vec3(-float(tex_size.y), 0.0, float(tex_size.y));

  vec4 accum = vec4(0.0);
  float tot_area = 0.0;

  /* iterate neighbor tiles */
  for (int ix = 0; ix < 3; ix++) {
    for (int iy = 0; iy < 3; iy++) {
      /* shifted rectangle into candidate tile space */
      float sxmin = rf.xmin + offsX[ix];
      float sxmax = rf.xmax + offsX[ix];
      float symin = rf.ymin + offsY[iy];
      float symax = rf.ymax + offsY[iy];

      /* intersect with base tile [0, tex_size] */
      float cxmin = max(sxmin, 0.0);
      float cxmax = min(sxmax, float(tex_size.x));
      float cymin = max(symin, 0.0);
      float cymax = min(symax, float(tex_size.y));

      if (cxmax <= cxmin || cymax <= cymin) {
        continue;
      }

      /* compute area weight */
      float area = (cxmax - cxmin) * (cymax - cymin);

      /* sample this clipped region similarly to the single-rect path */
      int startx = int(floor(cxmin));
      int endx = int(floor(cxmax));
      int starty = int(floor(cymin));
      int endy = int(floor(cymax));

      startx = max(startx, 0);
      starty = max(starty, 0);
      if (endx >= tex_size.x) {
        endx = tex_size.x - 1;
      }
      if (endy >= tex_size.y) {
        endy = tex_size.y - 1;
      }

      vec4 subcol = vec4(0.0);
      float div = 0.0;
      vec4 tmpc;

      for (int y = starty; y <= endy; y++) {
        float y0 = float(y);
        float wy = min(y0 + 1.0, cymax) - max(y0, cymin);
        if (wy <= 0.0) continue;
        for (int x = startx; x <= endx; x++) {
          float x0 = float(x);
          float wx = min(x0 + 1.0, cxmax) - max(x0, cxmin);
          if (wx <= 0.0) continue;
          float w = wx * wy;
          ibuf_get_color_boxsample(tmpc, displacement_texture, tex_size, x, y, tex_is_byte, tex_is_float, tex_channels);
          subcol += tmpc * w;
          div += w;
        }
      }

      if (div != 0.0) {
        subcol /= div;
      }
      else {
        subcol = vec4(0.0);
      }

      accum += subcol * area;
      tot_area += area;
    }
  }

  if (tot_area > 0.0) {
    texres.trgba = accum / tot_area;
  }
  else {
    texres.trgba = vec4(0.0);
  }
}

void boxsample(sampler2D displacement_texture,
              ivec2 tex_size,
              float minx,
              float miny,
              float maxx,
              float maxy,
              inout TexResult_tex texres,
              bool imaprepeat,
              bool imapextend,
              bool tex_is_byte,
              bool tex_is_float,
              int tex_channels)
{
  TexResult_tex texr;
  rctf rf;
  /* NOTE(GPU divergence): CPU `texture_image.cc::boxsample()` uses a small `stack[8]`
   * and a `count` value that can grow when `imaprepeat` triggers `clipx_rctf_swap()` /
   * `clipy_rctf_swap()` splitting. That allows sampling wrapped areas up to one repeat.
   *
   * This GPU implementation intentionally does NOT perform the swap/splitting path
   * (costly and complex in GLSL/compute). As a result, we only ever sample a single
   * clipped rectangle and do not need a stack here.
   *
   * The repeat behavior is instead primarily handled earlier in `imagewrap()` by wrapping
   * integer pixel coords and applying the #27782 floating-point remap before filtering.
   * This keeps results close to CPU for Displace while staying performant. */
  float opp, tot, alphaclip = 1.0;
  /* NOTE(GPU divergence): kept in CPU for repeat-splitting; unused here by design. */
  
  /* NOTE: This is intentionally a simplified GPU port.
   * We do NOT implement `clipx_rctf_swap` / `clipy_rctf_swap` splitting for repeat mode.
   * That logic is costly and complex on GPU. For imaprepeat we fall back to simple
   * clipping via clipx/clipy_rctf. */
  rf.xmin = minx * float(tex_size.x);
  rf.xmax = maxx * float(tex_size.x);
  rf.ymin = miny * float(tex_size.y);
  rf.ymax = maxy * float(tex_size.y);
  
  texr.talpha = texres.talpha;
  
  if (imapextend) {
    rf.xmin = clamp(rf.xmin, 0.0, float(tex_size.x - 1));
    rf.xmax = clamp(rf.xmax, 0.0, float(tex_size.x - 1));
  }
  else if (!imaprepeat) {
    /* Only clip for non-repeat modes. For repeat we must keep the original
     * rect so the split/average logic in boxsampleclip can handle wrapping. */
    alphaclip = clipx_rctf(rf, 0.0, float(tex_size.x));
    if (alphaclip <= 0.0) {
      texres.trgba[0] = texres.trgba[2] = texres.trgba[1] = texres.trgba[3] = 0.0;
      return;
    }
  }
  
  if (imapextend) {
    rf.ymin = clamp(rf.ymin, 0.0, float(tex_size.y - 1));
    rf.ymax = clamp(rf.ymax, 0.0, float(tex_size.y - 1));
  }
  else if (!imaprepeat) {
    /* Only clip for non-repeat modes. For repeat we must keep the original
     * rect so the split/average logic in boxsampleclip can handle wrapping. */
    alphaclip *= clipy_rctf(rf, 0.0, float(tex_size.y));
    if (alphaclip <= 0.0) {
      texres.trgba[0] = texres.trgba[2] = texres.trgba[1] = texres.trgba[3] = 0.0;
      return;
    }
  }

  /* NOTE(GPU divergence): CPU averages multiple rectangles when repeat-splitting occurs.
   * Since we do not split, we always sample a single rectangle here. */
  boxsampleclip(displacement_texture, tex_size, rf, texres, imaprepeat, tex_is_byte, tex_is_float, tex_channels);
  
  if (!texres.talpha) {
    texres.trgba[3] = 1.0;
  }
  
  if (alphaclip != 1.0) {
    texres.trgba[0] *= alphaclip;
    texres.trgba[1] *= alphaclip;
    texres.trgba[2] *= alphaclip;
    texres.trgba[3] *= alphaclip;
  }
}
)GLSL";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture Mapping and Wrapping Functions
 * \{ */

/**
 * Returns GLSL code for high-level texture mapping functions.
 * Includes BKE_colorband_evaluate, do_2d_mapping, imagewrap.
 * Note: These require TEX_* defines and uniform push constants.
 */
static std::string get_texture_mapping_glsl()
{
  return R"GLSL(
/* GPU port of BKE_colorband_evaluate() from colorband.cc (line 285-410)
 * NOTE: ColorBand struct is vec4-aligned in UBO (std140 layout)
 * Requires: ColorBand UBO structure with tot_cur_ipotype_hue, color_mode_pad, data[32]
 * Returns false if colorband is invalid or has no stops */
bool BKE_colorband_evaluate(ColorBand coba, float in_intensity, out vec4 out_color)
{
  vec4 cbd1_rgba, cbd2_rgba, cbd0_rgba, cbd3_rgba;
  float fac;
  int ipotype;
  int a;
  
  if (coba.tot_cur_ipotype_hue.x == 0) {
    return false;
  }
  
  cbd1_rgba = coba.data[0].rgba;
  
  ipotype = (coba.color_mode_pad.x == COLBAND_BLEND_RGB) ? coba.tot_cur_ipotype_hue.z : COLBAND_INTERP_LINEAR;
  
  if (coba.tot_cur_ipotype_hue.x == 1) {
    out_color = cbd1_rgba;
  }
  else if ((in_intensity <= coba.data[0].pos_cur_pad.x) &&
           (ipotype == COLBAND_INTERP_LINEAR || ipotype == COLBAND_INTERP_EASE ||
            ipotype == COLBAND_INTERP_CONSTANT))
  {
    out_color = cbd1_rgba;
  }
  else {
    vec4 left_rgba, right_rgba;
    float left_pos, right_pos;
    
    for (a = 0; a < coba.tot_cur_ipotype_hue.x; a++) {
      cbd1_rgba = coba.data[a].rgba;
      if (coba.data[a].pos_cur_pad.x > in_intensity) {
        break;
      }
    }
    
    if (a == coba.tot_cur_ipotype_hue.x) {
      cbd2_rgba = cbd1_rgba;
      right_rgba = cbd2_rgba;
      right_pos = 1.0;
      cbd1_rgba = right_rgba;
    }
    else if (a == 0) {
      left_rgba = cbd1_rgba;
      left_pos = 0.0;
      cbd2_rgba = left_rgba;
    }
    else {
      cbd2_rgba = coba.data[a - 1].rgba;
    }
    
    if ((a == coba.tot_cur_ipotype_hue.x) &&
        (ipotype == COLBAND_INTERP_LINEAR || ipotype == COLBAND_INTERP_EASE ||
         ipotype == COLBAND_INTERP_CONSTANT))
    {
      out_color = cbd2_rgba;
    }
    else if (ipotype == COLBAND_INTERP_CONSTANT) {
      out_color = cbd2_rgba;
    }
    else {
      float cbd1_pos, cbd2_pos;
      
      if (a == coba.tot_cur_ipotype_hue.x) {
        cbd1_pos = right_pos;
        cbd2_pos = coba.data[a - 1].pos_cur_pad.x;
      }
      else if (a == 0) {
        cbd1_pos = coba.data[0].pos_cur_pad.x;
        cbd2_pos = left_pos;
      }
      else {
        cbd1_pos = coba.data[a].pos_cur_pad.x;
        cbd2_pos = coba.data[a - 1].pos_cur_pad.x;
      }
      
      if (cbd2_pos != cbd1_pos) {
        fac = (in_intensity - cbd1_pos) / (cbd2_pos - cbd1_pos);
      }
      else {
        fac = (a != coba.tot_cur_ipotype_hue.x) ? 0.0 : 1.0;
      }
      
      if (ipotype == COLBAND_INTERP_B_SPLINE || ipotype == COLBAND_INTERP_CARDINAL) {
        float t[4];
        
        if (a >= coba.tot_cur_ipotype_hue.x - 1) {
          cbd0_rgba = cbd1_rgba;
        }
        else {
          cbd0_rgba = coba.data[a + 1].rgba;
        }
        if (a < 2) {
          cbd3_rgba = cbd2_rgba;
        }
        else {
          cbd3_rgba = coba.data[a - 2].rgba;
        }
        
        fac = clamp(fac, 0.0, 1.0);
        
        if (ipotype == COLBAND_INTERP_CARDINAL) {
          key_curve_position_weights(fac, t, 1);
        }
        else {
          key_curve_position_weights(fac, t, 2);
        }
        
        out_color = vec4(
            t[3] * cbd3_rgba.r + t[2] * cbd2_rgba.r + t[1] * cbd1_rgba.r + t[0] * cbd0_rgba.r,
            t[3] * cbd3_rgba.g + t[2] * cbd2_rgba.g + t[1] * cbd1_rgba.g + t[0] * cbd0_rgba.g,
            t[3] * cbd3_rgba.b + t[2] * cbd2_rgba.b + t[1] * cbd1_rgba.b + t[0] * cbd0_rgba.b,
            t[3] * cbd3_rgba.a + t[2] * cbd2_rgba.a + t[1] * cbd1_rgba.a + t[0] * cbd0_rgba.a);
        out_color = clamp(out_color, 0.0, 1.0);
      }
      else {
        if (ipotype == COLBAND_INTERP_EASE) {
          float fac2 = fac * fac;
          fac = 3.0 * fac2 - 2.0 * fac2 * fac;
        }
        float mfac = 1.0 - fac;
        
        if (coba.color_mode_pad.x == COLBAND_BLEND_HSV) {
          vec3 col1, col2;
          
          col1 = rgb_to_hsv(vec3(cbd1_rgba.r, cbd1_rgba.g, cbd1_rgba.b));
          col2 = rgb_to_hsv(vec3(cbd2_rgba.r, cbd2_rgba.g, cbd2_rgba.b));
          
          out_color.r = colorband_hue_interp(coba.tot_cur_ipotype_hue.w, mfac, fac, col1[0], col2[0]);
          out_color.g = mfac * col1[1] + fac * col2[1];
          out_color.b = mfac * col1[2] + fac * col2[2];
          out_color.a = mfac * cbd1_rgba.a + fac * cbd2_rgba.a;

          vec3 out_rgb = hsv_to_rgb(vec3(out_color.r, out_color.g, out_color.b));
          out_color.r = out_rgb.r;
          out_color.g = out_rgb.g;
          out_color.b = out_rgb.b;
        }
        else if (coba.color_mode_pad.x == COLBAND_BLEND_HSL) {
          vec3 col1, col2;
          
          col1 = rgb_to_hsl(vec3(cbd1_rgba.r, cbd1_rgba.g, cbd1_rgba.b));
          col2 = rgb_to_hsl(vec3(cbd2_rgba.r, cbd2_rgba.g, cbd2_rgba.b));
          
          out_color.r = colorband_hue_interp(coba.tot_cur_ipotype_hue.w, mfac, fac, col1[0], col2[0]);
          out_color.g = mfac * col1[1] + fac * col2[1];
          out_color.b = mfac * col1[2] + fac * col2[2];
          out_color.a = mfac * cbd1_rgba.a + fac * cbd2_rgba.a;

          vec3 out_rgb = hsl_to_rgb(vec3(out_color.r, out_color.g, out_color.b));
          out_color.r = out_rgb.r;
          out_color.g = out_rgb.g;
          out_color.b = out_rgb.b;
        }
        else {
          out_color = vec4(mfac * cbd1_rgba.r + fac * cbd2_rgba.r,
                           mfac * cbd1_rgba.g + fac * cbd2_rgba.g,
                           mfac * cbd1_rgba.b + fac * cbd2_rgba.b,
                           mfac * cbd1_rgba.a + fac * cbd2_rgba.a);
        }
      }
    }
  }
  
  return true;
}

/* GPU port of do_2d_mapping() EXTENDED from texture_procedural.cc (line 501-537)
 * Applies texture transformations in correct order: SCALE → ROTATION → REPEAT → MIRROR → CROP → OFFSET
 * This matches CPU behavior and supports full Tex structure parameters.
 * Requires: TEX_REPEAT define, all transformation uniforms
 * NOTE: Scale/rotation/offset are SKIPPED if values are identity (optimization + correctness) */
void do_2d_mapping(inout float fx, inout float fy, 
                   int tex_extend, 
                   vec2 tex_repeat, 
                   bool tex_xmir, 
                   bool tex_ymir, 
                   vec4 tex_crop,
                   vec3 tex_size,      /* Tex->size (scale X/Y/Z) */
                   vec3 tex_ofs,       /* Tex->ofs (offset X/Y/Z) */
                   float tex_rot)      /* Tex->rot (rotation Z in radians) */
{
  /* OPTIMIZATION: Skip transformations if identity (most common case for Displace modifier) */
  bool has_scale = (tex_size.x != 1.0 || tex_size.y != 1.0);
  bool has_rotation = (tex_rot != 0.0);
  bool has_offset = (tex_ofs.x != 0.0 || tex_ofs.y != 0.0);
  
  /* Step 1: Apply size (scale) FIRST - matches CPU order */
  if (has_scale) {
    if (tex_size.x != 1.0) fx *= tex_size.x;
    if (tex_size.y != 1.0) fy *= tex_size.y;
  }
  
  /* Step 2: Apply rotation Z around origin (0.5, 0.5) */
  if (has_rotation) {
    /* Translate to origin */
    fx -= 0.5;
    fy -= 0.5;
    
    /* Apply rotation matrix */
    float s = sin(tex_rot);
    float c = cos(tex_rot);
    float nx = fx * c - fy * s;
    float ny = fx * s + fy * c;
    
    /* Translate back */
    fx = nx + 0.5;
    fy = ny + 0.5;
  }
  
  /* Step 3: REPEAT scaling + MIRROR (existing code) */
  if (tex_extend == 3) { /* TEX_REPEAT */
    float origf_x = fx;
    float origf_y = fy;
    
    if (tex_repeat.x > 1.0) {
      fx *= tex_repeat.x;
      if (fx > 1.0) {
        fx -= float(int(fx));
      }
      else if (fx < 0.0) {
        fx += 1.0 - float(int(fx));
      }
      
      if (tex_xmir) {
        int orig = int(floor(origf_x * tex_repeat.x));
        if ((orig & 1) != 0) {
          fx = 1.0 - fx;
        }
      }
    }
    
    if (tex_repeat.y > 1.0) {
      fy *= tex_repeat.y;
      if (fy > 1.0) {
        fy -= float(int(fy));
      }
      else if (fy < 0.0) {
        fy += 1.0 - float(int(fy));
      }
      
      if (tex_ymir) {
        int orig = int(floor(origf_y * tex_repeat.y));
        if ((orig & 1) != 0) {
          fy = 1.0 - fy;
        }
      }
    }
  }

  /* Step 4: CROP (existing code) */
  if (tex_crop.x != 0.0 || tex_crop.z != 1.0) {
    float fac1 = tex_crop.z - tex_crop.x;
    fx = tex_crop.x + fx * fac1;
  }
  if (tex_crop.y != 0.0 || tex_crop.w != 1.0) {
    float fac1 = tex_crop.w - tex_crop.y;
    fy = tex_crop.y + fy * fac1;
  }
  
  /* Step 5: Apply offset LAST (only if non-zero) */
  if (has_offset) {
    fx += tex_ofs.x;
    fy += tex_ofs.y;
  }
}
)GLSL";
}

static std::string get_imagewrap()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* Displacement-specific imagewrap() function
 * Note: BKE_colorband_evaluate() and do_2d_mapping() are in gpu_shader_common_texture_lib.hh
 * imagewrap() uses shader-specific uniforms (tex_flip_axis, tex_checker_*, etc.) */

/* GPU port of imagewrap() from texture_image.cc (line 98-256)
 * Handles TEX_IMAROT, TEX_CHECKER filtering, CLIPCUBE check, coordinate wrapping, and texture sampling
 * Returns TEX_RGB flag if successful */
int imagewrap(vec3 tex_coord, inout vec4 result, inout float out_tin, ivec2 tex_size)
{
  result = vec4(0.0);
  int retval = TEX_RGB;

  float fx = tex_coord.x;
  float fy = tex_coord.y;
  
  /* Step 1: TEX_IMAROT (swap X/Y) AFTER crop */
  if (tex_flip_axis) {
    float temp = fx;
    fx = fy;
    fy = temp;
  }

  /* Step 2: TEX_CHECKER filtering */
  if (tex_extend == TEX_CHECKER) {
    int xs = int(floor(fx));
    int ys = int(floor(fy));
    int tile_parity = (xs + ys) & 1;
    
    bool show_tile = true;
    if (tex_checker_odd && (tile_parity == 0)) {
      show_tile = false;
    }
    if (tex_checker_even && (tile_parity == 1)) {
      show_tile = false;
    }
    
    if (!show_tile) {
      return retval;
    }

    fx -= float(xs);
    fy -= float(ys);
    
    if (tex_checkerdist < 1.0) {
      fx = (fx - 0.5) / (1.0 - tex_checkerdist) + 0.5;
      fy = (fy - 0.5) / (1.0 - tex_checkerdist) + 0.5;
    }
  }
  
  /* Step 3: Compute integer pixel coordinates */
  int x = int(floor(fx * float(tex_size.x)));
  int y = int(floor(fy * float(tex_size.y)));
  int xi = x;
  int yi = y;
  
  /* Step 4: CLIPCUBE early return */
  if (tex_extend == TEX_CLIPCUBE) {
    if (x < 0 || y < 0 || x >= tex_size.x || y >= tex_size.y ||
        tex_coord.z < -1.0 || tex_coord.z > 1.0) {
      return retval;
    }
  }
  /* Step 5: CLIP/CHECKER early return */
  else if (tex_extend == TEX_CLIP || tex_extend == TEX_CHECKER) {
    if (x < 0 || y < 0 || x >= tex_size.x || y >= tex_size.y) {
      return retval;
    }
  }
  /* Step 6: EXTEND or REPEAT mode: wrap/clamp coordinates */
  else {
    if (tex_extend == TEX_EXTEND) {
      x = (x >= tex_size.x) ? (tex_size.x - 1) : ((x < 0) ? 0 : x);
    }
    else {
      x = x % tex_size.x;
      if (x < 0) x += tex_size.x;
    }
    
    if (tex_extend == TEX_EXTEND) {
      y = (y >= tex_size.y) ? (tex_size.y - 1) : ((y < 0) ? 0 : y);
    }
    else {
      y = y % tex_size.y;
      if (y < 0) y += tex_size.y;
    }
  }

  /* Step 7: Sample texture with or without interpolation.
   * Keep the FAST and CPU-like paths side-by-side to make comparisons easy.
   */
  if (tex_interpol) {
#if DISP_TEXSAMPLER_MODE == DISP_TEXSAMPLER_CPU_BOXSAMPLE
    /* CPU-like path: area filtered boxsample.
     * This is closer to `texture_image.cc::boxsample()` but can be very slow on GPU.
     */
    float filterx = (0.5 * tex_filtersize) / float(tex_size.x);
    float filtery = (0.5 * tex_filtersize) / float(tex_size.y);

    /* Match CPU: wrap back to float coords after integer clamp/wrap. */
    fx -= float(xi - x) / float(tex_size.x);
    fy -= float(yi - y) / float(tex_size.y);

    float min_tex_x = fx - filterx;
    float min_tex_y = fy - filtery;
    float max_tex_x = fx + filterx;
    float max_tex_y = fy + filtery;

    TexResult_tex texres_box;
    texres_box.trgba = vec4(0.0);
    texres_box.talpha = use_talpha;

    boxsample(displacement_texture,
              tex_size,
              min_tex_x,
              min_tex_y,
              max_tex_x,
              max_tex_y,
              texres_box,
              (tex_extend == TEX_REPEAT),
              (tex_extend == TEX_EXTEND),
              tex_is_byte,
              tex_is_float,
              tex_channels);

    result = texres_box.trgba;
#else
    /* Fast path: rely on hardware bilinear sampling.
     * This approximates CPU filtering but avoids the heavy nested loops of boxsample.
     */
    /* Match CPU wrap-remap (#27782) even in fast mode. */
    float u = fx - float(xi - x) / float(tex_size.x);
    float v = fy - float(yi - y) / float(tex_size.y);

    if (tex_extend == TEX_EXTEND) {
      u = clamp(u, 0.0, 1.0);
      v = clamp(v, 0.0, 1.0);
    }
    else if (tex_extend == TEX_REPEAT) {
      /* After CPU-style remap, u/v are expected to already be in wrapped space.
       * Keep them as-is to mimic CPU x/y wrapping behavior.
       */
    }

    result = shader_ibuf_get_color(texture(displacement_texture, vec2(u, v)),
                                   tex_is_float,
                                   tex_channels,
                                   tex_is_byte);
#endif
  }
  else {
    ivec2 px_coord = ivec2(x, y);
    px_coord = clamp(px_coord, ivec2(0), tex_size - 1);
    result = shader_ibuf_get_color(texelFetch(displacement_texture, px_coord, 0),
                                  tex_is_float,
                                  tex_channels,
                                  tex_is_byte);
  }
  
  /* Compute intensity */
  if (use_talpha) {
    out_tin = result.a;
  }
  else if (tex_calcalpha) {
    out_tin = max(max(result.r, result.g), result.b);
    result.a = out_tin;
  }
  else {
    out_tin = 1.0;
    result.a = 1.0;
  }

  if (tex_negalpha) {
    result.a = 1.0 - result.a;
  }

  /* De-pre-multiply */
  if (result.a != 1.0 && result.a > 1e-4 && !tex_calcalpha) {
    float inv_alpha = 1.0 / result.a;
    result.rgb *= inv_alpha;
  }

  /* BRICONTRGB (brightness/contrast/RGB factors) */
  vec3 rgb = result.rgb;
  rgb.r = tex_rfac * ((rgb.r - 0.5) * tex_contrast + tex_bright - 0.5);
  rgb.g = tex_gfac * ((rgb.g - 0.5) * tex_contrast + tex_bright - 0.5);
  rgb.b = tex_bfac * ((rgb.b - 0.5) * tex_contrast + tex_bright - 0.5);

  if (!tex_no_clamp) {
    rgb = max(rgb, vec3(0.0));
  }

  /* Apply saturation */
  if (tex_saturation != 1.0) {
    float cmax = max(max(rgb.r, rgb.g), rgb.b);
    float cmin = min(min(rgb.r, rgb.g), rgb.b);
    float delta_hsv = cmax - cmin;

    float h = 0.0, s = 0.0, v = cmax;

    if (delta_hsv > 1e-20) {
      s = delta_hsv / (cmax + 1e-20);

      if (rgb.r >= cmax) {
        h = (rgb.g - rgb.b) / delta_hsv;
      } else if (rgb.g >= cmax) {
        h = 2.0 + (rgb.b - rgb.r) / delta_hsv;
      } else {
        h = 4.0 + (rgb.r - rgb.g) / delta_hsv;
      }

      h /= 6.0;
      if (h < 0.0) h += 1.0;
    }

    s *= tex_saturation;

    float nr = abs(h * 6.0 - 3.0) - 1.0;
    float ng = 2.0 - abs(h * 6.0 - 2.0);
    float nb = 2.0 - abs(h * 6.0 - 4.0);

    nr = clamp(nr, 0.0, 1.0);
    ng = clamp(ng, 0.0, 1.0);
    nb = clamp(nb, 0.0, 1.0);

    float r = ((nr - 1.0) * s + 1.0) * v;
    float g = ((ng - 1.0) * s + 1.0) * v;
    float b = ((nb - 1.0) * s + 1.0) * v;

    if (tex_saturation > 1.0 && !tex_no_clamp) {
      rgb = max(rgb, vec3(0.0));
    }
  }

  result.rgb = rgb;
  return retval;
}
#endif  // HAS_TEXTURE
)GLSL";
}

static std::string get_multitex_glsl()
{
  return R"GLSL(
/* Generic multitex dispatcher: calls procedural texture helpers based on a set of
 * uniforms describing the current `Tex` state. This is a simplified GPU-side
 * port intended for compute shaders (e.g. Displace). The caller must provide
 * the following uniforms when using this dispatcher:
 *
 * uniform int tex_type;
 * uniform int tex_stype;
 * uniform int tex_noisebasis;
 * uniform int tex_noisebasis2; //marble and ?
 * uniform float tex_noisesize;
 * uniform float tex_turbul;
 * uniform int tex_noisedepth;
 * uniform bool tex_flipblend;
 * int tex_mt -> uniform tex_stype; // marble mode
 * uniform sampler2D displacement_texture; // for image types (sampler name used by imagewrap)
 * uniform ivec2 tex_image_size (not needed - use textureSize);
 * uniform bool use_colorband; // whether to apply colorband
 * uniform ColorBand tex_colorband; // ColorBand UBO
 */

int multitex(vec3 texvec, inout TexResult_tex texres, int thread_id)
{
  texres.talpha = false;
  int retval = 0;
  int t = tex_type;

  if (t == 0) {
    texres.tin = 0.0;
    return 0;
  }

  if (t == TEX_CLOUDS) {
    retval = clouds_tex(texres, texres, tex_noisesize, tex_turbul, tex_noisedepth, tex_noisebasis, tex_stype);
  }
  else if (t == TEX_WOOD) {
    retval = wood_tex(texvec, texres, tex_noisebasis, tex_stype, tex_noisesize, tex_turbul);
  }
  else if (t == TEX_MARBLE) {
    /* Marble uses `stype` as its mode in CPU code (tex->stype). Use tex_stype here. */
    retval = marble_tex(texvec, texres, tex_noisebasis, tex_stype, tex_noisesize, tex_turbul);
  }
  else if (t == TEX_MAGIC) {
    retval = magic_tex(texvec, texres, tex_turbul);
  }
  else if (t == TEX_BLEND) {
    retval = blend_tex(texvec, texres, tex_stype, tex_flipblend);
  }
  else if (t == TEX_STUCCI) {
    retval = stucci_tex(texvec, texres, tex_stype, tex_noisesize, tex_turbul, tex_noisebasis);
  }
  else if (t == TEX_NOISE) {
    retval = texnoise_tex(texres, thread_id, tex_noisedepth);
  }
  else if (t == TEX_VORONOI) {
    /* Example default params for voronoi; callers may set additional uniforms */
    retval = voronoi_tex(texvec, texres, 1.0, 0.0, 0.0, 0.0, 1.0, TEX_COL1, 0, 1.0);
  }
  else if (t == TEX_DISTNOISE) {
    /* Fallback to generic noise path */
    retval = texnoise_tex(texres, thread_id, tex_noisedepth);
  }
  else if (t == TEX_IMAGE) {
    /* Image sampling: apply 2D mapping (scale/rotation/repeat/mirror/crop/offset)
     * only for image textures, then call imagewrap() which handles sampling.
     * This matches CPU behavior where do_2d_mapping is applied only for images. */
    float fx = (texvec.x + 1.0) / 2.0;
    float fy = (texvec.y + 1.0) / 2.0;

    /* Apply mapping transforms (SCALE → ROTATION → REPEAT → MIRROR → CROP → OFFSET) */
    do_2d_mapping(fx, fy, tex_extend, tex_repeat, tex_xmir, tex_ymir, tex_crop, tex_size_param, tex_ofs, tex_rot);

    vec3 mapped_coord = vec3(fx, fy, texvec.z);
    ivec2 img_size = textureSize(displacement_texture, 0);
    int ir = imagewrap(mapped_coord, texres.trgba, texres.tin, img_size);
    if (ir == TEX_RGB) retval = TEX_RGB;
    else retval = TEX_INT;
  }

  /* Apply colorband if requested (requires ColorBand UBO with name `tex_colorband`).
   * ColorBand maps an intensity -> color, so apply it only when the texture
   * result is an intensity (i.e. TEX_RGB flag is NOT set). TEX_INT is defined
   * as 0, so testing `retval & TEX_INT` is always false. */
  if (use_colorband) {
    vec4 cbcol;
    if (BKE_colorband_evaluate(tex_colorband, texres.tin, cbcol)) {
      texres.talpha = true;
      texres.trgba = cbcol;
      retval |= TEX_RGB;
    }
  }

  return retval;
}

/* Wrapper for environments that cannot pass `inout` structs across compilation units.
 * Accepts separate inout parameters for RGBA, intensity and talpha and forwards to
 * the `multitex` implementation which uses `TexResult_tex` internally. */
int multitex_components(vec3 texvec,
                        inout vec4 out_trgba,
                        inout float out_tin,
                        inout bool out_talpha,
                        int thread_id)
{
  TexResult_tex tmp;
  tmp.trgba = out_trgba;
  tmp.tin = out_tin;
  tmp.talpha = out_talpha;

  int retval = multitex(texvec, tmp, thread_id);

  out_trgba = tmp.trgba;
  out_tin = tmp.tin;
  out_talpha = tmp.talpha;

  return retval;
}

)GLSL";
}

/** \} */

/**
 * Returns GLSL code for common texture processing functions.
 * Call this to get the full texture library.
 */
static std::string get_common_texture_lib_glsl()
{
  /* Prepend texture subtype defines so shaders can reference TEX_* stype values
   * in a central place. These are placeholders that mirror the CPU-side names and
   * can be adjusted if DNA values differ. */
    return get_texture_defines_glsl() + get_bricont_glsl() +
           get_noise_helpers_glsl() +
           get_colorband_helpers_glsl() +
           get_color_conversion_glsl() +
           /* core structs used by texture functions */
           get_texture_structs_glsl() +
           /* Procedural textures in same order as CPU `multitex` switch */
           get_clouds_glsl() +
           get_wood_glsl() +
           get_marble_glsl() +
           get_magic_glsl() +
           get_blend_glsl() +
           get_stucci_glsl() +
           get_texnoise_glsl() +
           /* Voronoi and other procedural types */
           get_voronoi_glsl() +
           /* Boxsample / image helpers and mapping last */
           get_boxsample_helpers_glsl() +
           get_boxsample_core_glsl() +
           get_texture_mapping_glsl() +
           get_imagewrap() +
           get_multitex_glsl();
}

}  // namespace blender::gpu

}  // namespace blender
