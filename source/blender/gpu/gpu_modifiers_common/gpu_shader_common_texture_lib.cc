/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Common texture sampling and processing functions for GPU compute shaders.
 * Includes ColorBand, imagewrap, boxsample, and texture mapping utilities.
 */

#include <string>

#include "gpu_shader_common_texture_lib.hh"

namespace blender {
namespace gpu {

/* Typedefs for ColorBand / CBData used by shaders. Kept separate so callers
 * can set as typedef header in ShaderCreateInfo (avoids duplicating in compute source). */
const std::string get_texture_typedefs_glsl()
{
  return R"GLSL(
struct CBData {
  vec4 rgba;         /* r, g, b, a packed in vec4 */
  vec4 pos_cur_pad;  /* pos, cur (as float), pad[2] */
};

struct ColorBand {
  ivec4 tot_cur_ipotype_hue;  /* tot, cur, ipotype, ipotype_hue */
  ivec4 color_mode_pad;       /* color_mode, pad[3] */
  CBData data[32];
};

struct TextureParams {
  vec4 tex_crop;           /* cropxmin, cropymin, cropxmax, cropymax */
  ivec4 tex_repeat_xmir;   /* repeat.x, repeat.y, xmir(0/1), ymir(0/1) */
  ivec4 tex_properties;    /* is_byte(0/1), is_float(0/1), channels, type */
  vec4 tex_bricont;        /* bright, contrast, saturation, pad */
  vec4 tex_size_ofs_rot;   /* size.x, size.y, ofs.x, ofs.y (rot in .z or separate) */
  ivec4 tex_mapping_misc;  /* mapping, mapping_use_input_positions(0/1), mtex_mapto, stype */
  ivec4 tex_flags;         /* flag, extend, checkerdist(as int*1000), pad */
  ivec4 tex_noise;         /* noisebasis, noisebasis2, noisedepth, noisetype */
  vec4 tex_noisesize_turbul; /* noisesize, turbul, pad, pad */
  ivec4 tex_filtersize_frame_colorband_pad; /* filtersize*1000 (as int), frame, use_colorband(0/1), pad */
  vec4 tex_rgbfac;         /* rfac, gfac, bfac, pad */
  vec4 tex_distamount;     /* distamount, ns_outscale, pad, pad */
  vec4 tex_mg_params0;     /* mg_H, mg_lacunarity, mg_octaves, mg_offset */
  vec4 tex_mg_params1;     /* mg_gain, (reserved), pad, pad */
  vec4 tex_voronoi;        /* vn_w1, vn_w2, vn_w3, vn_w4 */
  vec4 tex_voronoi_misc;   /* vn_mexp, vn_distm, vn_coltype, pad */
  ivec4 tex_imaflag_runtime_flags; /* imaflag, use_talpha, calcalpha, negalpha */
  mat4 u_object_to_world_mat; /* object->world */
  mat4 u_mapref_imat;         /* inverse map reference for object mapping */
};

)GLSL";
}

/* Texture parameter UBO shared by texture/image helpers.
 * Packed for std140 layout. Shaders can read via `tex_params`.
 * This allows modifiers to upload a single UBO instead of many push constants. */
const std::string get_texture_params_glsl()
{
  return R"GLSL(
/* Convenience macros that map legacy push-constant names used throughout the
 * texture helpers to fields inside the std140 `tex_params` UBO. This allows
 * existing shader code to keep referencing `u_tex_*` identifiers while the
 * real data is packed into a single UBO on the CPU side.
 *
 * Note: some values (rfac/gfac/bfac, u_use_talpha, u_tex_calcalpha, etc.) are
 * still provided as push-constants in the current C++ shader setup. We only
 * map fields that were migrated into `TextureParams` above.
 */
#define u_tex_crop tex_params.tex_crop
#define u_tex_repeat vec2(tex_params.tex_repeat_xmir.x, tex_params.tex_repeat_xmir.y)
#define u_tex_xmir (tex_params.tex_repeat_xmir.z != 0)
#define u_tex_ymir (tex_params.tex_repeat_xmir.w != 0)
#define u_tex_is_byte (tex_params.tex_properties.x != 0)
#define u_tex_is_float (tex_params.tex_properties.y != 0)
#define u_tex_channels int(tex_params.tex_properties.z)
#define u_tex_type int(tex_params.tex_properties.w)
#define u_tex_bricont tex_params.tex_bricont
#define u_tex_bright tex_params.tex_bricont.x
#define u_tex_contrast tex_params.tex_bricont.y
#define u_tex_saturation tex_params.tex_bricont.z
#define u_tex_rfac tex_params.tex_rgbfac.x
#define u_tex_gfac tex_params.tex_rgbfac.y
#define u_tex_bfac tex_params.tex_rgbfac.z
#define u_tex_distamount tex_params.tex_distamount.x
/* ns_outscale exposed next to distamount (CPU writes it into tex_distamount[1]) */
#define u_tex_ns_outscale tex_params.tex_distamount.y
#define u_tex_noisesize tex_params.tex_noisesize_turbul.x
#define u_tex_turbul tex_params.tex_noisesize_turbul.y
#define u_tex_size_param vec3(tex_params.tex_size_ofs_rot.x, tex_params.tex_size_ofs_rot.y, 0.0)
#define u_tex_ofs vec3(tex_params.tex_size_ofs_rot.z, tex_params.tex_size_ofs_rot.w, 0.0)
#define u_tex_mapping int(tex_params.tex_mapping_misc.x)
#define u_mapping_use_input_positions (tex_params.tex_mapping_misc.y != 0)
#define u_mtex_mapto int(tex_params.tex_mapping_misc.z)
#define u_tex_stype int(tex_params.tex_mapping_misc.w)
#define u_tex_flag int(tex_params.tex_flags.x)
#define u_tex_extend int(tex_params.tex_flags.y)
#define u_tex_checkerdist (float(tex_params.tex_flags.z) / 1000.0)
#define u_tex_noisebasis int(tex_params.tex_noise.x)
#define u_tex_noisebasis2 int(tex_params.tex_noise.y)
#define u_tex_noisedepth int(tex_params.tex_noise.z)
#define u_tex_noisetype int(tex_params.tex_noise.w)
#define u_tex_filtersize (float(tex_params.tex_filtersize_frame_colorband_pad.x) / 1000.0)
#define u_tex_frame int(tex_params.tex_filtersize_frame_colorband_pad.y)
#define u_object_to_world_mat tex_params.u_object_to_world_mat
#define u_mapref_imat tex_params.u_mapref_imat
#define u_tex_no_clamp ((tex_params.tex_flags.x & TEX_NO_CLAMP) != 0)

/* Runtime flags migrated into tex_imaflag_runtime_flags */
#define u_use_talpha (tex_params.tex_imaflag_runtime_flags.y != 0)
#define u_tex_calcalpha (tex_params.tex_imaflag_runtime_flags.z != 0)
#define u_tex_negalpha (tex_params.tex_imaflag_runtime_flags.w != 0)
#define u_use_colorband (tex_params.tex_filtersize_frame_colorband_pad.z != 0)

/* Musgrave params */
#define u_tex_mg_H (tex_params.tex_mg_params0.x)
#define u_tex_mg_lacunarity (tex_params.tex_mg_params0.y)
#define u_tex_mg_octaves (tex_params.tex_mg_params0.z)
#define u_tex_mg_offset (tex_params.tex_mg_params0.w)
#define u_tex_mg_gain (tex_params.tex_mg_params1.x)

/* Voronoi params */
#define u_tex_vn_w1 tex_params.tex_voronoi.x
#define u_tex_vn_w2 tex_params.tex_voronoi.y
#define u_tex_vn_w3 tex_params.tex_voronoi.z
#define u_tex_vn_w4 tex_params.tex_voronoi.w
#define u_tex_vn_mexp tex_params.tex_voronoi_misc.x
#define u_tex_vn_distm int(tex_params.tex_voronoi_misc.y)
#define u_tex_vn_coltype int(tex_params.tex_voronoi_misc.z)

/* Auxiliary flags/macros */
#define U_BIT_IMAROT (1 << 4)
#define U_BIT_CHECKER_ODD (1 << 3)
#define U_BIT_CHECKER_EVEN (1 << 4)
#define U_BIT_INTERPOL (1 << 0)
#define U_BIT_FLIPBLEND (1 << 1)

#define u_tex_flip_axis ((tex_params.tex_imaflag_runtime_flags.x & U_BIT_IMAROT) != 0)
#define u_tex_checker_odd ((tex_params.tex_flags.x & U_BIT_CHECKER_ODD) == 0)
#define u_tex_checker_even ((tex_params.tex_flags.x & U_BIT_CHECKER_EVEN) == 0)
#define u_tex_interpol ((tex_params.tex_imaflag_runtime_flags.x & U_BIT_INTERPOL) != 0)
#define u_tex_flipblend ((tex_params.tex_flags.x & U_BIT_FLIPBLEND) != 0)
#define u_tex_rot (tex_params.tex_size_ofs_rot.z)
)GLSL";
}

/* -------------------------------------------------------------------- */
/** \name ColorBand Defines and Helper Functions
 * \{ */

/* Placeholder defines for texture subtypes (blend stype, wood/marble waveforms, etc.)
 * These provide a single place to keep TEX_* name definitions for GPU shaders.
 * Values should match those in DNA_texture_types.h / CPU implementation. */
static const std::string get_texture_defines_glsl()
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

/* Musgrave subtype values (used by musgrave textures / mg_* helpers) */
#define TEX_MFRACTAL 0
#define TEX_RIDGEDMF 1
#define TEX_HYBRIDMF 2
#define TEX_FBM 3
#define TEX_HTERRAIN 4

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
static const std::string get_texture_structs_glsl()
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
static const std::string get_colorband_helpers_glsl()
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

static const std::string get_texnoise_glsl()
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

  uint seed = uint(thread_id) + uint(u_tex_frame) * 1664525u;
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

static const std::string get_distnoise_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* GPU port of CPU `mg_distNoiseTex()` / TEX_DISTNOISE.
 * Note: CPU scales `texvec` by 1/noisesize before calling the musgrave noise.
 */
int distnoise_tex(vec3 texvec, inout TexResult_tex texres, float noisesize, float distortion, int noisebasis, int noisebasis2)
{
  /* Avoid div-by-zero; mimic CPU behavior (noisesize==0 -> leave coords unscaled). */
  if (noisesize != 0.0) {
    texvec *= (1.0 / noisesize);
  }

  /* CPU returns signed noise from BLI_noise_mg_variable_lacunarity then BRICONT.
   * BRICONT expects an intensity-like value; keep behavior consistent with CPU code path.
   */
  texres.tin = bli_noise_mg_variable_lacunarity(texvec.x, texvec.y, texvec.z, distortion, noisebasis, noisebasis2);
  texres.tin = apply_bricont_fn(texres.tin);
  return TEX_INT;
}

/* Musgrave wrapper that reads parameters from the TextureParams UBO and
 * matches CPU `mg_*` branches (scaled by u_tex_ns_outscale). */
int musgrave_tex_ubo(vec3 texvec, inout TexResult_tex texres)
{
  float H = u_tex_mg_H;
  float lac = u_tex_mg_lacunarity;
  float oct = u_tex_mg_octaves;
  float off = u_tex_mg_offset;
  float gain = u_tex_mg_gain;
  float ns = u_tex_ns_outscale;
  int basis = u_tex_noisebasis;

  int st = u_tex_stype;

  if (st == TEX_MFRACTAL) {
    texres.tin = ns * bli_noise_mg_multi_fractal(texvec.x, texvec.y, texvec.z, H, lac, oct, basis);
  }
  else if (st == TEX_FBM) {
    texres.tin = ns * bli_noise_mg_fbm(texvec.x, texvec.y, texvec.z, H, lac, oct, basis);
  }
  else if (st == TEX_RIDGEDMF) {
    texres.tin = ns * bli_noise_mg_ridged_multi_fractal(texvec.x, texvec.y, texvec.z, H, lac, oct, off, gain, basis);
  }
  else if (st == TEX_HYBRIDMF) {
    texres.tin = ns * bli_noise_mg_hybrid_multi_fractal(texvec.x, texvec.y, texvec.z, H, lac, oct, off, gain, basis);
  }
  else if (st == TEX_HTERRAIN) {
    texres.tin = ns * bli_noise_mg_hetero_terrain(texvec.x, texvec.y, texvec.z, H, lac, oct, off, basis);
  }
  else {
    /* Fallback -> use fbm */
    texres.tin = ns * bli_noise_mg_fbm(texvec.x, texvec.y, texvec.z, H, lac, oct, basis);
  }

  /* Match CPU: apply BRICONT after musgrave intensity */
  texres.tin = apply_bricont_fn(texres.tin);
  return TEX_INT;
}

#endif // HAS_TEXTURE
  )GLSL";
}

static const std::string get_bricont_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* BRICONT helpers (matches CPU BRICONT/BRICONTRGB macros in texture_common.h) */

float apply_bricont_fn(float tin)
{
  float t = (tin - 0.5) * u_tex_contrast + u_tex_bright - 0.5;
  if (!u_tex_no_clamp) {
    t = clamp(t, 0.0, 1.0);
  }
  return t;
}

vec3 apply_bricont_rgb(vec3 col)
{
  /* Step 1: Apply brightness/contrast with RGB factors (line 26-30 CPU) */
  col.r = u_tex_rfac * ((col.r - 0.5) * u_tex_contrast + u_tex_bright - 0.5);
  col.g = u_tex_gfac * ((col.g - 0.5) * u_tex_contrast + u_tex_bright - 0.5);
  col.b = u_tex_bfac * ((col.b - 0.5) * u_tex_contrast + u_tex_bright - 0.5);

  /* Step 2: Clamp if TEX_NO_CLAMP is not set (line 31-40 CPU) */
  if (!u_tex_no_clamp) {
    col = max(col, vec3(0.0));
  }

  /* Step 3: Apply saturation via HSV (line 41-56 CPU BRICONTRGB) */
  if (u_tex_saturation != 1.0) {
    vec3 hsv = rgb_to_hsv(col);
    hsv.y *= u_tex_saturation; /* multiply saturation channel */
    col = hsv_to_rgb(hsv);

    /* Clamp again if saturation > 1.0 */
    if (u_tex_saturation > 1.0 && !u_tex_no_clamp) {
      col = max(col, vec3(0.0));
    }
  }

  return col;
}

#endif // HAS_TEXTURE

)GLSL";
}

static const std::string get_stucci_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* Simplified stucci implementation (returns intensity) */
int stucci_tex(vec3 texvec, inout TexResult_tex texres, int stype, float noisesize, float turbul, int noisebasis)
{
  /* CPU uses BLI_noise_generic_noise for stucci; use same here for parity. */
  float b2 = bli_noise_generic_noise(u_tex_noisesize, texvec.x, texvec.y, texvec.z, (u_tex_noisetype != 0), noisebasis);
  float ofs = turbul / 200.0;
  if (stype != 0) {
    ofs *= (b2 * b2);
  }
  texres.tin = bli_noise_generic_noise(u_tex_noisesize, texvec.x, texvec.y, texvec.z + ofs, (u_tex_noisetype != 0), noisebasis);
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

static const std::string get_voronoi_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* Voronoi texture approximation using helpers */
int voronoi_tex(vec3 texvec, inout TexResult_tex texres, float vn_w1, float vn_w2, float vn_w3, float vn_w4, float ns_outscale, int vn_coltype, int vn_distm, float vn_mexp)
{
  float da[4];
  float pa[12];

  /* Legacy Voronoi (texture system) exact port. */
  bli_noise_voronoi_legacy(texvec.x, texvec.y, texvec.z, da, pa, vn_mexp, vn_distm);

  /* Weighted distance (matches Blender texture Voronoi behavior). */
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
    vec3 ca0 = gpu_BLI_noise_cell_v3_legacy(pa[0], pa[1], pa[2]);
    vec3 ca1 = gpu_BLI_noise_cell_v3_legacy(pa[3], pa[4], pa[5]);
    vec3 ca2 = gpu_BLI_noise_cell_v3_legacy(pa[6], pa[7], pa[8]);
    vec3 ca3 = gpu_BLI_noise_cell_v3_legacy(pa[9], pa[10], pa[11]);
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

/* Convenience wrapper that reads Voronoi params from the TextureParams UBO.
 * Use this from multitex so shaders get CPU-populated vn_* values. */
int voronoi_tex_ubo(vec3 texvec, inout TexResult_tex texres)
{
  return voronoi_tex(texvec,
                     texres,
                     u_tex_vn_w1,
                     u_tex_vn_w2,
                     u_tex_vn_w3,
                     u_tex_vn_w4,
                     u_tex_ns_outscale,
                     u_tex_vn_coltype,
                     u_tex_vn_distm,
                     u_tex_vn_mexp);
}

#endif // HAS_TEXTURE

)GLSL";
}

static const std::string get_marble_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* GPU port of CPU `marble_int()` + `marble()` from `texture_procedural.cc`.
 * NOTE: CPU behavior:
 * - `wf` (wave form) is `tex->noisebasis2` (0..2), clamped to TEX_SIN..TEX_TRI.
 * - `mt` (marble type) is `tex->stype` (TEX_SOFT/TEX_SHARP/TEX_SHARPER).
 * - turbulence uses `tex->noisebasis` + `tex->noisedepth` + `tex->noisetype`.
 */
float marble_int_glsl(int wf, int mt, float x, float y, float z, float noisesize, float turbul, int noisedepth, bool hard, int noisebasis)
{
  /* Clamp waveform like CPU. */
  if (wf < TEX_SIN || wf > TEX_TRI) {
    wf = TEX_SIN;
  }

  float n = 5.0 * (x + y + z);
  float mi = n + turbul * bli_noise_generic_turbulence(noisesize, x, y, z, noisedepth, hard, noisebasis);

  /* waveform[wf](mi) */
  if (wf == TEX_SIN) {
    mi = tex_sin_glsl(mi);
  }
  else if (wf == TEX_SAW) {
    mi = tex_saw_glsl(mi);
  }
  else {
    /* TEX_TRI */
    mi = tex_tri_glsl(mi);
  }

  /* marble type shaping */
  if (mt == TEX_SHARP) {
    mi = sqrt(max(mi, 0.0));
  }
  else if (mt == TEX_SHARPER) {
    mi = sqrt(sqrt(max(mi, 0.0)));
  }

  return mi;
}

int marble_tex(vec3 texvec, inout TexResult_tex texres, int wf, int mt, float noisesize, float turbul)
{
  texres.tin = marble_int_glsl(wf,
                               mt,
                               texvec.x,
                               texvec.y,
                               texvec.z,
                               noisesize,
                               turbul,
                               u_tex_noisedepth,
                               (u_tex_noisetype != 0),
                               u_tex_noisebasis);
  texres.tin = apply_bricont_fn(texres.tin);
  return TEX_INT;
}

#endif // HAS_TEXTURE

)GLSL";
}

static const std::string get_magic_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* GPU port of CPU `magic()` texture from `texture_procedural.cc`.
 * Matches CPU control flow and uses `tex_noisedepth` + `turbul/5` scaling. */
int magic_tex(vec3 texvec, inout TexResult_tex texres, float turbul)
{
  float x, y, z;
  float turb = turbul / 5.0;
  int n = u_tex_noisedepth;

  x = sin((texvec.x + texvec.y + texvec.z) * 5.0);
  y = cos((-texvec.x + texvec.y - texvec.z) * 5.0);
  z = -cos((-texvec.x - texvec.y + texvec.z) * 5.0);

  if (n > 0) {
    x *= turb;
    y *= turb;
    z *= turb;
    y = -cos(x - y + z);
    y *= turb;

    if (n > 1) {
      x = cos(x - y - z);
      x *= turb;
      if (n > 2) {
        z = sin(-x - y - z);
        z *= turb;
        if (n > 3) {
          x = -cos(-x + y - z);
          x *= turb;
          if (n > 4) {
            y = -sin(-x + y + z);
            y *= turb;
            if (n > 5) {
              y = -cos(-x + y + z);
              y *= turb;
              if (n > 6) {
                x = cos(x + y + z);
                x *= turb;
                if (n > 7) {
                  z = sin(x + y - z);
                  z *= turb;
                  if (n > 8) {
                    x = -cos(-x - y + z);
                    x *= turb;
                    if (n > 9) {
                      y = -sin(x - y + z);
                      y *= turb;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  if (turb != 0.0) {
    turb *= 2.0;
    x /= turb;
    y /= turb;
    z /= turb;
  }

  texres.trgba.r = 0.5 - x;
  texres.trgba.g = 0.5 - y;
  texres.trgba.b = 0.5 - z;
  texres.trgba.a = 1.0;

  texres.tin = (1.0 / 3.0) * (texres.trgba.r + texres.trgba.g + texres.trgba.b);

  /* CPU uses BRICONTRGB (not BRICONT) for magic. */
  texres.trgba.rgb = apply_bricont_rgb(texres.trgba.rgb);

  return TEX_RGB;
}

#endif // HAS_TEXTURE

)GLSL";
}


static const std::string get_wood_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* GPU port of CPU `wood()` texture and helper wood_int()
 * Implements exact CPU control flow for BAND, RING, BANDNOISE, RINGNOISE:
 * - BAND: waveform((x+y+z)*10)
 * - RING: waveform(sqrt(x*x+y*y+z*z)*20)
 * - BANDNOISE: wi = turbul * BLI_noise_generic_noise(...); waveform((x+y+z)*10 + wi)
 * - RINGNOISE: wi = turbul * BLI_noise_generic_noise(...); waveform(sqrt(...) * 20 + wi)
 */
float wood_int_glsl(int noisebasis, int noisebasis2, int stype, float x, float y, float z, float noisesize, float turbul)
{
  float wi = 0.0;
  int wf = noisebasis2;

  /* Clamp waveform like CPU */
  if (wf < TEX_SIN || wf > TEX_TRI) {
    wf = TEX_SIN;
  }

  if (stype == TEX_BAND) {
    float coord = (x + y + z) * 10.0;
    if (wf == TEX_SIN) wi = tex_sin_glsl(coord);
    else if (wf == TEX_SAW) wi = tex_saw_glsl(coord);
    else wi = tex_tri_glsl(coord);
  }
  else if (stype == TEX_RING) {
    float coord = sqrt(x * x + y * y + z * z) * 20.0;
    if (wf == TEX_SIN) wi = tex_sin_glsl(coord);
    else if (wf == TEX_SAW) wi = tex_saw_glsl(coord);
    else wi = tex_tri_glsl(coord);
  }
  else if (stype == TEX_BANDNOISE) {
    float n = bli_noise_generic_noise(noisesize, x, y, z, (u_tex_noisetype != 0), noisebasis);
    float coord = (x + y + z) * 10.0 + turbul * n;
    if (wf == TEX_SIN) wi = tex_sin_glsl(coord);
    else if (wf == TEX_SAW) wi = tex_saw_glsl(coord);
    else wi = tex_tri_glsl(coord);
  }
  else if (stype == TEX_RINGNOISE) {
    float n = bli_noise_generic_noise(noisesize, x, y, z, (u_tex_noisetype != 0), noisebasis);
    float coord = sqrt(x * x + y * y + z * z) * 20.0 + turbul * n;
    if (wf == TEX_SIN) wi = tex_sin_glsl(coord);
    else if (wf == TEX_SAW) wi = tex_saw_glsl(coord);
    else wi = tex_tri_glsl(coord);
  }

  return wi;
}

int wood_tex(vec3 texvec, inout TexResult_tex texres, int noisebasis, int noisebasis2, int stype, float noisesize, float turbul)
{
  float wi = wood_int_glsl(noisebasis, noisebasis2, stype, texvec.x, texvec.y, texvec.z, noisesize, turbul);
  texres.tin = wi;
  texres.tin = apply_bricont_fn(texres.tin);
  return TEX_INT;
}

#endif // HAS_TEXTURE

)GLSL";
}


static const std::string get_clouds_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* GPU port of CPU `clouds()` texture from texture_procedural.cc (line 1128-1158)
 * EXACT port matching CPU behavior with correct texvec coordinates. */
int clouds_tex(vec3 texvec, inout TexResult_tex texres, float noisesize, float turbul, int noisedepth, int noisebasis, int stype)
{
  int rv = TEX_INT;

  /* Main noise evaluation using texvec (not texres.trgba!) */
  float n_base = bli_noise_generic_turbulence(noisesize,
                                              texvec.x,
                                              texvec.y,
                                              texvec.z,
                                              noisedepth,
                                              (u_tex_noisetype != 0),
                                              noisebasis);
  texres.tin = n_base;

  if (stype == TEX_COLOR) {
    /* RGB mode: permute texvec coordinates for each channel (matches CPU line 1147-1156) */
    texres.trgba.r = texres.tin;
    texres.trgba.g = bli_noise_generic_turbulence(noisesize,
                                                texvec.y,
                                                texvec.x,
                                                texvec.z,
                                                noisedepth,
                                                (u_tex_noisetype != 0),
                                                noisebasis);
    texres.trgba.b = bli_noise_generic_turbulence(noisesize,
                                                texvec.y,
                                                texvec.z,
                                                texvec.x,
                                                noisedepth,
                                                (u_tex_noisetype != 0),
                                                noisebasis);

    /* BRICONTRGB: apply brightness/contrast to RGB */
    texres.trgba.rgb = apply_bricont_rgb(texres.trgba.rgb);
    texres.trgba.a = 1.0;
    return (rv | TEX_RGB);
  }

  /* BRICONT: apply brightness/contrast to intensity */
  texres.tin = apply_bricont_fn(texres.tin);

  return rv;
}

#endif // HAS_TEXTURE

)GLSL";
}

static const std::string get_blend_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* GPU port of CPU `blend()` texture (gradient/blend)
 * Expects texvec in [-1,1] range (matching CPU convention). */
int blend_tex(vec3 texvec, inout TexResult_tex texres, int stype, bool flipblend)
{
  float x, y, t;

  if (flipblend) {
    x = texvec.y;
    y = texvec.x;
  }
  else {
    x = texvec.x;
    y = texvec.y;
  }

  if (stype == TEX_LIN) { /* Linear. */
    texres.tin = (1.0 + x) / 2.0;
  }
  else if (stype == TEX_QUAD) { /* Quadratic. */
    texres.tin = (1.0 + x) / 2.0;
    if (texres.tin < 0.0) {
      texres.tin = 0.0;
    }
    else {
      texres.tin *= texres.tin;
    }
  }
  else if (stype == TEX_EASE) { /* Ease. */
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
  else if (stype == TEX_DIAG) { /* Diagonal. */
    texres.tin = (2.0 + x + y) / 4.0;
  }
  else if (stype == TEX_RAD) { /* Radial. */
    texres.tin = (atan(texvec.y, texvec.x) / (2.0 * M_PI) + 0.5);
  }
  else { /* sphere TEX_SPHERE */
    texres.tin = 1.0 - sqrt(texvec.x * texvec.x + texvec.y * texvec.y + texvec.z * texvec.z);
    texres.tin = max(texres.tin, 0.0);
    if (stype == TEX_HALO) {
      texres.tin *= texres.tin; /* Halo. */
    }
  }

  /* BRICONT macro: apply brightness/contrast and optional clamping (matches CPU macro)
   * Uses shader-side parameters: u_tex_contrast, u_tex_bright, tex_no_clamp */
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
static const std::string get_color_conversion_glsl()
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

static const std::string get_waveform_helpers_glsl()
{
  return R"GLSL(
/* Waveform helpers shared by wood/marble to mirror CPU tex_sin/tex_saw/tex_tri */
float tex_sin_glsl(float a)
{
  return 0.5 + 0.5 * sin(a);
}

float tex_saw_glsl(float a)
{
  const float b = 2.0 * M_PI;
  /* Match CPU behavior: CPU uses `int(a / b)` which truncates toward zero.
   * Using `floor` changes results for negative `a`. Replicate truncation. */
  int ni = int(a / b);
  float r = a - float(ni) * b;
  if (r < 0.0) r += b;
  return r / b;
}

float tex_tri_glsl(float a)
{
  const float b = 2.0 * M_PI;
  const float rmax = 1.0;
  return rmax - 2.0 * abs(floor((a * (1.0 / b)) + 0.5) - (a * (1.0 / b)));
}
)GLSL";
}

static const std::string get_noise_helpers_part1_glsl()
{
  return R"GLSL(
/* Blender noise static data (from noise_c.cc line 103-106) */
const uint hash[512] = uint[512](
  0xA2u,0xA0u,0x19u,0x3Bu,0xF8u,0xEBu,0xAAu,0xEEu,0xF3u,0x1Cu,0x67u,0x28u,0x1Du,0xEDu,0x0u,0xDEu,
  0x95u,0x2Eu,0xDCu,0x3Fu,0x3Au,0x82u,0x35u,0x4Du,0x6Cu,0xBAu,0x36u,0xD0u,0xF6u,0xCu,0x79u,0x32u,
  0xD1u,0x59u,0xF4u,0x8u,0x8Bu,0x63u,0x89u,0x2Fu,0xB8u,0xB4u,0x97u,0x83u,0xF2u,0x8Fu,0x18u,0xC7u,
  0x51u,0x14u,0x65u,0x87u,0x48u,0x20u,0x42u,0xA8u,0x80u,0xB5u,0x40u,0x13u,0xB2u,0x22u,0x7Eu,0x57u,
  0xBCu,0x7Fu,0x6Bu,0x9Du,0x86u,0x4Cu,0xC8u,0xDBu,0x7Cu,0xD5u,0x25u,0x4Eu,0x5Au,0x55u,0x74u,0x50u,
  0xCDu,0xB3u,0x7Au,0xBBu,0xC3u,0xCBu,0xB6u,0xE2u,0xE4u,0xECu,0xFDu,0x98u,0xBu,0x96u,0xD3u,0x9Eu,
  0x5Cu,0xA1u,0x64u,0xF1u,0x81u,0x61u,0xE1u,0xC4u,0x24u,0x72u,0x49u,0x8Cu,0x90u,0x4Bu,0x84u,0x34u,
  0x38u,0xABu,0x78u,0xCAu,0x1Fu,0x1u,0xD7u,0x93u,0x11u,0xC1u,0x58u,0xA9u,0x31u,0xF9u,0x44u,0x6Du,
  0xBFu,0x33u,0x9Cu,0x5Fu,0x9u,0x94u,0xA3u,0x85u,0x6u,0xC6u,0x9Au,0x1Eu,0x7Bu,0x46u,0x15u,0x30u,
  0x27u,0x2Bu,0x1Bu,0x71u,0x3Cu,0x5Bu,0xD6u,0x6Fu,0x62u,0xACu,0x4Fu,0xC2u,0xC0u,0xEu,0xB1u,0x23u,
  0xA7u,0xDFu,0x47u,0xB0u,0x77u,0x69u,0x5u,0xE9u,0xE6u,0xE7u,0x76u,0x73u,0xFu,0xFEu,0x6Eu,0x9Bu,
  0x56u,0xEFu,0x12u,0xA5u,0x37u,0xFCu,0xAEu,0xD9u,0x3u,0x8Eu,0xDDu,0x10u,0xB9u,0xCEu,0xC9u,0x8Du,
  0xDAu,0x2Au,0xBDu,0x68u,0x17u,0x9Fu,0xBEu,0xD4u,0xAu,0xCCu,0xD2u,0xE8u,0x43u,0x3Du,0x70u,0xB7u,
  0x2u,0x7Du,0x99u,0xD8u,0xDu,0x60u,0x8Au,0x4u,0x2Cu,0x3Eu,0x92u,0xE5u,0xAFu,0x53u,0x7u,0xE0u,
  0x29u,0xA6u,0xC5u,0xE3u,0xF5u,0xF7u,0x4Au,0x41u,0x26u,0x6Au,0x16u,0x5Eu,0x52u,0x2Du,0x21u,0xADu,
  0xF0u,0x91u,0xFFu,0xEAu,0x54u,0xFAu,0x66u,0x1Au,0x45u,0x39u,0xCFu,0x75u,0xA4u,0x88u,0xFBu,0x5Du,
  /* repeated for wrapping (total 512 elements) */
  0xA2u,0xA0u,0x19u,0x3Bu,0xF8u,0xEBu,0xAAu,0xEEu,0xF3u,0x1Cu,0x67u,0x28u,0x1Du,0xEDu,0x0u,0xDEu,
  0x95u,0x2Eu,0xDCu,0x3Fu,0x3Au,0x82u,0x35u,0x4Du,0x6Cu,0xBAu,0x36u,0xD0u,0xF6u,0xCu,0x79u,0x32u,
  0xD1u,0x59u,0xF4u,0x8u,0x8Bu,0x63u,0x89u,0x2Fu,0xB8u,0xB4u,0x97u,0x83u,0xF2u,0x8Fu,0x18u,0xC7u,
  0x51u,0x14u,0x65u,0x87u,0x48u,0x20u,0x42u,0xA8u,0x80u,0xB5u,0x40u,0x13u,0xB2u,0x22u,0x7Eu,0x57u,
  0xBCu,0x7Fu,0x6Bu,0x9Du,0x86u,0x4Cu,0xC8u,0xDBu,0x7Cu,0xD5u,0x25u,0x4Eu,0x5Au,0x55u,0x74u,0x50u,
  0xCDu,0xB3u,0x7Au,0xBBu,0xC3u,0xCBu,0xB6u,0xE2u,0xE4u,0xECu,0xFDu,0x98u,0xBu,0x96u,0xD3u,0x9Eu,
  0x5Cu,0xA1u,0x64u,0xF1u,0x81u,0x61u,0xE1u,0xC4u,0x24u,0x72u,0x49u,0x8Cu,0x90u,0x4Bu,0x84u,0x34u,
  0x38u,0xABu,0x78u,0xCAu,0x1Fu,0x1u,0xD7u,0x93u,0x11u,0xC1u,0x58u,0xA9u,0x31u,0xF9u,0x44u,0x6Du,
  0xBFu,0x33u,0x9Cu,0x5Fu,0x9u,0x94u,0xA3u,0x85u,0x6u,0xC6u,0x9Au,0x1Eu,0x7Bu,0x46u,0x15u,0x30u,
  0x27u,0x2Bu,0x1Bu,0x71u,0x3Cu,0x5Bu,0xD6u,0x6Fu,0x62u,0xACu,0x4Fu,0xC2u,0xC0u,0xEu,0xB1u,0x23u,
  0xA7u,0xDFu,0x47u,0xB0u,0x77u,0x69u,0x5u,0xE9u,0xE6u,0xE7u,0x76u,0x73u,0xFu,0xFEu,0x6Eu,0x9Bu,
  0x56u,0xEFu,0x12u,0xA5u,0x37u,0xFCu,0xAEu,0xD9u,0x3u,0x8Eu,0xDDu,0x10u,0xB9u,0xCEu,0xC9u,0x8Du,
  0xDAu,0x2Au,0xBDu,0x68u,0x17u,0x9Fu,0xBEu,0xD4u,0xAu,0xCCu,0xD2u,0xE8u,0x43u,0x3Du,0x70u,0xB7u,
  0x2u,0x7Du,0x99u,0xD8u,0xDu,0x60u,0x8Au,0x4u,0x2Cu,0x3Eu,0x92u,0xE5u,0xAFu,0x53u,0x7u,0xE0u,
  0x29u,0xA6u,0xC5u,0xE3u,0xF5u,0xF7u,0x4Au,0x41u,0x26u,0x6Au,0x16u,0x5Eu,0x52u,0x2Du,0x21u,0xADu,
  0xF0u,0x91u,0xFFu,0xEAu,0x54u,0xFAu,0x66u,0x1Au,0x45u,0x39u,0xCFu,0x75u,0xA4u,0x88u,0xFBu,0x5Du);

/* hashvectf gradient table for orgBlenderNoise (from noise_c.cc line 108-179) */
const float hashvectf[768] = float[768](
  0.33783,0.715698,-0.611206,-0.944031,-0.326599,-0.045624,-0.101074,-0.416443,
  -0.903503,0.799286,0.49411,-0.341949,-0.854645,0.518036,0.033936,0.42514,
  -0.437866,-0.792114,-0.358948,0.597046,0.717377,-0.985413,0.144714,0.089294,
  -0.601776,-0.33728,-0.723907,-0.449921,0.594513,0.666382,0.208313,-0.10791,
  0.972076,0.575317,0.060425,0.815643,0.293365,-0.875702,-0.383453,0.293762,
  0.465759,0.834686,-0.846008,-0.233398,-0.47934,-0.115814,0.143036,-0.98291,
  0.204681,-0.949036,-0.239532,0.946716,-0.263947,0.184326,-0.235596,0.573822,
  0.784332,0.203705,-0.372253,-0.905487,0.756989,-0.651031,0.055298,0.497803,
  0.814697,-0.297363,-0.16214,0.063995,-0.98468,-0.329254,0.834381,0.441925,
  0.703827,-0.527039,-0.476227,0.956421,0.266113,0.119781,0.480133,0.482849,
  0.7323,-0.18631,0.961212,-0.203125,-0.748474,-0.656921,-0.090393,-0.085052,
  -0.165253,0.982544,-0.76947,0.628174,-0.115234,0.383148,0.537659,0.751068,
  0.616486,-0.668488,-0.415924,-0.259979,-0.630005,0.73175,0.570953,-0.087952,
  0.816223,-0.458008,0.023254,0.888611,-0.196167,0.976563,-0.088287,-0.263885,
  -0.69812,-0.665527,0.437134,-0.892273,-0.112793,-0.621674,-0.230438,0.748566,
  0.232422,0.900574,-0.367249,0.22229,-0.796143,0.562744,-0.665497,-0.73764,
  0.11377,0.670135,0.704803,0.232605,0.895599,0.429749,-0.114655,-0.11557,
  -0.474243,0.872742,0.621826,0.604004,-0.498444,-0.832214,0.012756,0.55426,
  -0.702484,0.705994,-0.089661,-0.692017,0.649292,0.315399,-0.175995,-0.977997,
  0.111877,0.096954,-0.04953,0.994019,0.635284,-0.606689,-0.477783,-0.261261,
  -0.607422,-0.750153,0.983276,0.165436,0.075958,-0.29837,0.404083,-0.864655,
  -0.638672,0.507721,0.578156,0.388214,0.412079,0.824249,0.556183,-0.208832,
  0.804352,0.778442,0.562012,0.27951,-0.616577,0.781921,-0.091522,0.196289,
  0.051056,0.979187,-0.121216,0.207153,-0.970734,-0.173401,-0.384735,0.906555,
  0.161499,-0.723236,-0.671387,0.178497,-0.006226,-0.983887,-0.126038,0.15799,
  0.97934,0.830475,-0.024811,0.556458,-0.510132,-0.76944,0.384247,0.81424,
  0.200104,-0.544891,-0.112549,-0.393311,-0.912445,0.56189,0.152222,-0.813049,
  0.198914,-0.254517,-0.946381,-0.41217,0.690979,-0.593811,-0.407257,0.324524,
  0.853668,-0.690186,0.366119,-0.624115,-0.428345,0.844147,-0.322296,-0.21228,
  -0.297546,-0.930756,-0.273071,0.516113,0.811798,0.928314,0.371643,0.007233,
  0.785828,-0.479218,-0.390778,-0.704895,0.058929,0.706818,0.173248,0.203583,
  0.963562,0.422211,-0.904297,-0.062469,-0.363312,-0.182465,0.913605,0.254028,
  -0.552307,-0.793945,-0.28891,-0.765747,-0.574554,0.058319,0.291382,0.954803,
  0.946136,-0.303925,0.111267,-0.078156,0.443695,-0.892731,0.182098,0.89389,
  0.409515,-0.680298,-0.213318,0.701141,0.062469,0.848389,-0.525635,-0.72879,
  -0.641846,0.238342,-0.88089,0.427673,0.202637,-0.532501,-0.21405,0.818878,
  0.948975,-0.305084,0.07962,0.925446,0.374664,0.055817,0.820923,0.565491,
  0.079102,0.25882,0.099792,-0.960724,-0.294617,0.910522,0.289978,0.137115,
  0.320038,-0.937408,-0.908386,0.345276,-0.235718,-0.936218,0.138763,0.322754,
  0.366577,0.925934,-0.090637,0.309296,-0.686829,-0.657684,0.66983,0.024445,
  0.742065,-0.917999,-0.059113,-0.392059,0.365509,0.462158,-0.807922,0.083374,
  0.996399,-0.014801,0.593842,0.253143,-0.763672,0.974976,-0.165466,0.148285,
  0.918976,0.137299,0.369537,0.294952,0.694977,0.655731,0.943085,0.152618,
  -0.295319,0.58783,-0.598236,0.544495,0.203796,0.678223,0.705994,-0.478821,
  -0.661011,0.577667,0.719055,-0.1698,-0.673828,-0.132172,-0.965332,0.225006,
  -0.981873,-0.14502,0.121979,0.763458,0.579742,0.284546,-0.893188,0.079681,
  0.442474,-0.795776,-0.523804,0.303802,0.734955,0.67804,-0.007446,0.15506,
  0.986267,-0.056183,0.258026,0.571503,-0.778931,-0.681549,-0.702087,-0.206116,
  -0.96286,-0.177185,0.203613,-0.470978,-0.515106,0.716095,-0.740326,0.57135,
  0.354095,-0.56012,-0.824982,-0.074982,-0.507874,0.753204,0.417969,-0.503113,
  0.038147,0.863342,0.594025,0.673553,-0.439758,-0.119873,-0.005524,-0.992737,
  0.098267,-0.213776,0.971893,-0.615631,0.643951,0.454163,0.896851,-0.441071,
  0.032166,-0.555023,0.750763,-0.358093,0.398773,0.304688,0.864929,-0.722961,
  0.303589,0.620544,-0.63559,-0.621948,-0.457306,-0.293243,0.072327,0.953278,
  -0.491638,0.661041,-0.566772,-0.304199,-0.572083,-0.761688,0.908081,-0.398956,
  0.127014,-0.523621,-0.549683,-0.650848,-0.932922,-0.19986,0.299408,0.099426,
  0.140869,0.984985,-0.020325,-0.999756,-0.002319,0.952667,0.280853,-0.11615,
  -0.971893,0.082581,0.220337,0.65921,0.705292,-0.260651,0.733063,-0.175537,
  0.657043,-0.555206,0.429504,-0.712189,0.400421,-0.89859,0.179352,0.750885,
  -0.19696,0.630341,0.785675,-0.569336,0.241821,-0.058899,-0.464111,0.883789,
  0.129608,-0.94519,0.299622,-0.357819,0.907654,0.219238,-0.842133,-0.439117,
  -0.312927,-0.313477,0.84433,0.434479,-0.241211,0.053253,0.968994,0.063873,
  0.823273,0.563965,0.476288,0.862152,-0.172516,0.620941,-0.298126,0.724915,
  0.25238,-0.749359,-0.612122,-0.577545,0.386566,0.718994,-0.406342,-0.737976,
  0.538696,0.04718,0.556305,0.82959,-0.802856,0.587463,0.101166,-0.707733,
  -0.705963,0.026428,0.374908,0.68457,0.625092,0.472137,0.208405,-0.856506,
  -0.703064,-0.581085,-0.409821,-0.417206,-0.736328,0.532623,-0.447876,-0.20285,
  -0.870728,0.086945,-0.990417,0.107086,0.183685,0.018341,-0.982788,0.560638,
  -0.428864,0.708282,0.296722,-0.952576,-0.0672,0.135773,0.990265,0.030243,
  -0.068787,0.654724,0.752686,0.762604,-0.551758,0.337585,-0.819611,-0.407684,
  0.402466,-0.727844,-0.55072,-0.408539,-0.855774,-0.480011,0.19281,0.693176,
  -0.079285,0.716339,0.226013,0.650116,-0.725433,0.246704,0.953369,-0.173553,
  -0.970398,-0.239227,-0.03244,0.136383,-0.394318,0.908752,0.813232,0.558167,
  0.164368,0.40451,0.549042,-0.731323,-0.380249,-0.566711,0.730865,0.022156,
  0.932739,0.359741,0.00824,0.996552,-0.082306,0.956635,-0.065338,-0.283722,
  -0.743561,0.008209,0.668579,-0.859589,-0.509674,0.035767,-0.852234,0.363678,
  -0.375977,-0.201965,-0.970795,-0.12915,0.313477,0.947327,0.06546,-0.254028,
  -0.528259,0.81015,0.628052,0.601105,0.49411,-0.494385,0.868378,0.037933,
  0.275635,-0.086426,0.957336,-0.197937,0.468903,-0.860748,0.895599,0.399384,
  0.195801,0.560791,0.825012,-0.069214,0.304199,-0.849487,0.43103,0.096375,
  0.93576,0.339111,-0.051422,0.408966,-0.911072,0.330444,0.942841,-0.042389,
  -0.452362,-0.786407,0.420563,0.134308,-0.933472,-0.332489,0.80191,-0.566711,
  -0.188934,-0.987946,-0.105988,0.112518,-0.24408,0.892242,-0.379791,-0.920502,
  0.229095,-0.316376,0.7789,0.325958,0.535706,-0.912872,0.185211,-0.36377,
  -0.184784,0.565369,-0.803833,-0.018463,0.119537,0.992615,-0.259247,-0.935608,
  0.239532,-0.82373,-0.449127,-0.345947,-0.433105,0.659515,0.614349,-0.822754,
  0.378845,-0.423676,0.687195,-0.674835,-0.26889,-0.246582,-0.800842,0.545715,
  -0.729187,-0.207794,0.651978,0.653534,-0.610443,-0.447388,0.492584,-0.023346,
  0.869934,0.609039,0.009094,-0.79306,0.962494,-0.271088,-0.00885,0.2659,
  -0.004913,0.963959,0.651245,0.553619,-0.518951,0.280548,-0.84314,0.458618,
  -0.175293,-0.983215,0.049805,0.035339,-0.979919,0.196045,-0.982941,0.164307,
  -0.082245,0.233734,-0.97226,-0.005005,-0.747253,-0.611328,0.260437,0.645599,
  0.592773,0.481384,0.117706,-0.949524,-0.29068,-0.535004,-0.791901,-0.294312,
  -0.627167,-0.214447,0.748718,-0.047974,-0.813477,-0.57959,-0.175537,0.477264,
  -0.860992,0.738556,-0.414246,-0.53183,0.562561,-0.704071,0.433289,-0.754944,
  0.64801,-0.100586,0.114716,0.044525,-0.992371,0.966003,0.244873,-0.082764
);

)GLSL";
}

static const std::string get_noise_helpers_part2_glsl()
{
  return R"GLSL(
/* point table for Voronoi (copied from noise_c.cc hashpntf)
 *
 * IMPORTANT: Some GLSL compilers used by Blender reject dynamic indexing into
 * a large `const float[]` (e.g. `hashpntf[idx + 1]`). To keep this compatible
 * with compute shaders, store as `vec3[256]` and index with a single integer.
 */
const vec3 hashpntf3[256] = vec3[256](
  vec3(0.536902,0.020915,0.501445),vec3(0.216316,0.517036,0.822466),vec3(0.965315,0.377313,0.678764),
  vec3(0.744545,0.097731,0.396357),vec3(0.247202,0.520897,0.613396),vec3(0.542124,0.146813,0.255489),
  vec3(0.810868,0.638641,0.980742),vec3(0.292316,0.357948,0.114382),vec3(0.861377,0.629634,0.72253),
  vec3(0.714103,0.048549,0.075668),vec3(0.56492,0.162026,0.054466),vec3(0.411738,0.156897,0.887657),
  vec3(0.599368,0.074249,0.170277),vec3(0.225799,0.393154,0.301348),vec3(0.057434,0.293849,0.442745),
  vec3(0.150002,0.398732,0.184582),vec3(0.9152,0.630984,0.97404),vec3(0.117228,0.79552,0.763238),
  vec3(0.158982,0.616211,0.250825),vec3(0.906539,0.316874,0.676205),vec3(0.23472,0.667673,0.792225),
  vec3(0.273671,0.119363,0.199131),vec3(0.856716,0.828554,0.900718),vec3(0.70596,0.635923,0.989433),
  vec3(0.027261,0.283507,0.113426),vec3(0.388115,0.900176,0.637741),vec3(0.438802,0.71549,0.043692),
  vec3(0.20264,0.378325,0.450325),vec3(0.471832,0.147803,0.906899),vec3(0.524178,0.784981,0.051483),
  vec3(0.893369,0.596895,0.275635),vec3(0.391483,0.844673,0.103061),vec3(0.257322,0.70839,0.504091),
  vec3(0.199517,0.660339,0.376071),vec3(0.03888,0.531293,0.216116),vec3(0.138672,0.907737,0.807994),
  vec3(0.659582,0.915264,0.449075),vec3(0.627128,0.480173,0.380942),vec3(0.018843,0.211808,0.569701),
  vec3(0.082294,0.689488,0.57306),vec3(0.593859,0.21608,0.373159),vec3(0.108117,0.595539,0.021768),
  vec3(0.380297,0.948125,0.377833),vec3(0.319699,0.315249,0.972805),vec3(0.79227,0.445396,0.845323),
  vec3(0.372186,0.096147,0.689405),vec3(0.423958,0.055675,0.11794),vec3(0.328456,0.605808,0.631768),
  vec3(0.37217,0.213723,0.0327),vec3(0.447257,0.440661,0.728488),vec3(0.299853,0.148599,0.649212),
  vec3(0.498381,0.049921,0.496112),vec3(0.607142,0.562595,0.990246),vec3(0.739659,0.108633,0.978156),
  vec3(0.209814,0.258436,0.876021),vec3(0.30926,0.600673,0.713597),vec3(0.576967,0.641402,0.85393),
  vec3(0.029173,0.418111,0.581593),vec3(0.008394,0.589904,0.661574),vec3(0.979326,0.275724,0.111109),
  vec3(0.440472,0.120839,0.521602),vec3(0.648308,0.284575,0.204501),vec3(0.153286,0.822444,0.300786),
  vec3(0.303906,0.364717,0.209038),vec3(0.916831,0.900245,0.600685),vec3(0.890002,0.58166,0.431154),
  vec3(0.705569,0.55125,0.417075),vec3(0.403749,0.696652,0.292652),vec3(0.911372,0.690922,0.323718),
  vec3(0.036773,0.258976,0.274265),vec3(0.225076,0.628965,0.351644),vec3(0.065158,0.08034,0.467271),
  vec3(0.130643,0.385914,0.919315),vec3(0.253821,0.966163,0.017439),vec3(0.39261,0.478792,0.978185),
  vec3(0.072691,0.982009,0.097987),vec3(0.731533,0.401233,0.10757),vec3(0.349587,0.479122,0.700598),
  vec3(0.481751,0.788429,0.706864),vec3(0.120086,0.562691,0.981797),vec3(0.001223,0.19212,0.451543),
  vec3(0.173092,0.10896,0.549594),vec3(0.587892,0.657534,0.396365),vec3(0.125153,0.66642,0.385823),
  vec3(0.890916,0.436729,0.128114),vec3(0.369598,0.759096,0.044677),vec3(0.904752,0.088052,0.621148),
  vec3(0.005047,0.452331,0.162032),vec3(0.494238,0.523349,0.741829),vec3(0.69845,0.452316,0.563487),
  vec3(0.819776,0.49216,0.00421),vec3(0.647158,0.551475,0.362995),vec3(0.177937,0.814722,0.727729),
  vec3(0.867126,0.997157,0.108149),vec3(0.085726,0.796024,0.665075),vec3(0.362462,0.323124,0.043718),
  vec3(0.042357,0.31503,0.328954),vec3(0.870845,0.683186,0.467922),vec3(0.514894,0.809971,0.631979),
  vec3(0.176571,0.36632,0.850621),vec3(0.505555,0.749551,0.75083),vec3(0.401714,0.481216,0.438393),
  vec3(0.508832,0.867971,0.654581),vec3(0.058204,0.566454,0.084124),vec3(0.548539,0.90269,0.779571),
  vec3(0.562058,0.048082,0.863109),vec3(0.07929,0.713559,0.783496),vec3(0.265266,0.672089,0.786939),
  vec3(0.143048,0.086196,0.876129),vec3(0.408708,0.229312,0.629995),vec3(0.206665,0.207308,0.710079),
  vec3(0.341704,0.264921,0.028748),vec3(0.629222,0.470173,0.726228),vec3(0.125243,0.328249,0.794187),
  vec3(0.74134,0.489895,0.189396),vec3(0.724654,0.092841,0.039809),vec3(0.860126,0.247701,0.655331),
  vec3(0.964121,0.672536,0.044522),vec3(0.690567,0.837238,0.63152),vec3(0.953734,0.352484,0.289026),
  vec3(0.034152,0.852575,0.098454),vec3(0.795529,0.452181,0.826159),vec3(0.186993,0.820725,0.440328),
  vec3(0.922137,0.704592,0.915437),vec3(0.738183,0.733461,0.193798),vec3(0.929213,0.16139,0.318547),
  vec3(0.888751,0.430968,0.740837),vec3(0.193544,0.872253,0.563074),vec3(0.274598,0.347805,0.666176),
  vec3(0.449831,0.800991,0.588727),vec3(0.052296,0.714761,0.42062),vec3(0.570325,0.05755,0.210888),
  vec3(0.407312,0.662848,0.924382),vec3(0.895958,0.775198,0.688605),vec3(0.025721,0.301913,0.791408),
  vec3(0.500602,0.831984,0.828509),vec3(0.642093,0.494174,0.52588),vec3(0.446365,0.440063,0.763114),
  vec3(0.630358,0.223943,0.333806),vec3(0.906033,0.498306,0.241278),vec3(0.42764,0.772683,0.198082),
  vec3(0.225379,0.503894,0.436599),vec3(0.016503,0.803725,0.189878),vec3(0.291095,0.499114,0.151573),
  vec3(0.079031,0.904618,0.708535),vec3(0.2739,0.067419,0.317124),vec3(0.936499,0.716511,0.543845),
  vec3(0.939909,0.826574,0.71509),vec3(0.154864,0.75015,0.845808),vec3(0.648108,0.556564,0.644757),
  vec3(0.140873,0.799167,0.632989),vec3(0.444245,0.471978,0.43591),vec3(0.359793,0.216241,0.007633),
  vec3(0.337236,0.857863,0.380247),vec3(0.092517,0.799973,0.919),vec3(0.296798,0.096989,0.854831),
  vec3(0.165369,0.568475,0.216855),vec3(0.020457,0.835511,0.538039),vec3(0.999742,0.620226,0.244053),
  vec3(0.060399,0.323007,0.294874),vec3(0.988899,0.384919,0.735655),vec3(0.773428,0.549776,0.292882),
  vec3(0.660611,0.593507,0.621118),vec3(0.175269,0.682119,0.794493),vec3(0.868197,0.63215,0.807823),
  vec3(0.509656,0.482035,0.00178),vec3(0.259126,0.358002,0.280263),vec3(0.192985,0.290367,0.208111),
  vec3(0.917633,0.114422,0.925491),vec3(0.98111,0.25557,0.974862),vec3(0.016629,0.552599,0.575741),
  vec3(0.612978,0.615965,0.803615),vec3(0.772334,0.089745,0.838812),vec3(0.634542,0.113709,0.755832),
  vec3(0.577589,0.667489,0.529834),vec3(0.32566,0.817597,0.316557),vec3(0.335093,0.737363,0.260951),
  vec3(0.737073,0.04954,0.735541),vec3(0.988891,0.299116,0.147695),vec3(0.417271,0.940811,0.52416),
  vec3(0.857968,0.176403,0.244835),vec3(0.485759,0.033353,0.280319),vec3(0.750688,0.755809,0.924208),
  vec3(0.095956,0.962504,0.275584),vec3(0.173715,0.942716,0.706721),vec3(0.078464,0.576716,0.804667),
  vec3(0.559249,0.900611,0.646904),vec3(0.432111,0.927885,0.383277),vec3(0.269973,0.114244,0.574867),
  vec3(0.150703,0.241855,0.272871),vec3(0.19995,0.079719,0.868566),vec3(0.962833,0.789122,0.320025),
  vec3(0.905554,0.234876,0.991356),vec3(0.061913,0.732911,0.78596),vec3(0.874074,0.069035,0.658632),
  vec3(0.309901,0.023676,0.791603),vec3(0.764661,0.661278,0.319583),vec3(0.82965,0.117091,0.903124),
  vec3(0.982098,0.161631,0.193576),vec3(0.670428,0.85739,0.00376),vec3(0.572578,0.222162,0.114551),
  vec3(0.420118,0.530404,0.470682),vec3(0.525527,0.764281,0.040596),vec3(0.443275,0.501124,0.816161),
  vec3(0.417467,0.332172,0.447565),vec3(0.614591,0.559246,0.805295),vec3(0.226342,0.155065,0.71463),
  vec3(0.160925,0.760001,0.453456),vec3(0.093869,0.406092,0.264801),vec3(0.72037,0.743388,0.373269),
  vec3(0.403098,0.911923,0.897249),vec3(0.147038,0.753037,0.516093),vec3(0.739257,0.175018,0.045768),
  vec3(0.735857,0.80133,0.927708),vec3(0.240977,0.59187,0.921831),vec3(0.540733,0.1491,0.423152),
  vec3(0.806876,0.397081,0.0611),vec3(0.81163,0.044899,0.460915),vec3(0.961202,0.822098,0.971524),
  vec3(0.867608,0.773604,0.226616),vec3(0.686286,0.926972,0.411613),vec3(0.267873,0.081937,0.226124),
  vec3(0.295664,0.374594,0.53324),vec3(0.237876,0.669629,0.599083),vec3(0.513081,0.878719,0.201577),
  vec3(0.721296,0.495038,0.07976),vec3(0.965959,0.23309,0.052496),vec3(0.714748,0.887844,0.308724),
  vec3(0.972885,0.723337,0.453089),vec3(0.914474,0.704063,0.823198),vec3(0.834769,0.906561,0.9196),
  vec3(0.100601,0.307564,0.901977),vec3(0.468879,0.265376,0.885188),vec3(0.683875,0.868623,0.081032),
  vec3(0.466835,0.199087,0.663437),vec3(0.812241,0.311337,0.821361),vec3(0.356628,0.898054,0.160781),
  vec3(0.222539,0.714889,0.490287),vec3(0.984915,0.951755,0.964097),vec3(0.641795,0.815472,0.852732),
  vec3(0.862074,0.051108,0.440139),vec3(0.323207,0.517171,0.562984),vec3(0.115295,0.743103,0.977914),
  vec3(0.337596,0.440694,0.535879),vec3(0.959427,0.351427,0.704361),vec3(0.010826,0.131162,0.57708),
  vec3(0.349572,0.774892,0.425796),vec3(0.072697,0.5,0.267322),vec3(0.909654,0.206176,0.223987),
  vec3(0.937698,0.323423,0.117501),vec3(0.490308,0.474372,0.689943),vec3(0.168671,0.719417,0.188928),
  vec3(0.330464,0.265273,0.446271),vec3(0.171933,0.176133,0.474616),vec3(0.140182,0.114246,0.905043),
  vec3(0.71387,0.555261,0.951333)
);
  
)GLSL";
}

static const std::string get_noise_helpers_part3_glsl()
{
  return R"GLSL(
/* cellnoise helper for GLSL side usage */
float gpu_BLI_cellNoiseU(float cx, float cy, float cz)
{
  float fx = (cx + 0.000001) * 1.00001;
  float fy = (cy + 0.000001) * 1.00001;
  float fz = (cz + 0.000001) * 1.00001;
  int xi = int(floor(fx));
  int yi = int(floor(fy));
  int zi = int(floor(fz));
  uint n = uint(xi) + uint(yi) * uint(1301) + uint(zi) * uint(314159);
  n ^= (n << 13);
  uint v = n * (n * n * uint(15731) + uint(789221)) + uint(1376312589);
  return float(v) / 4294967296.0;
}

/* Legacy Voronoi helpers (exact port of `source/blender/blenlib/intern/noise_c.cc`)
 * Used by the old texture system (DNA noise basis 3..8) and `BLI_noise_voronoi()`.
 *
 * NOTE: This is intentionally separate from the modern node Voronoi implementation.
 */

float dist_Squared_legacy(float x, float y, float z, float e)
{
  return x * x + y * y + z * z;
}

float dist_Real_legacy(float x, float y, float z, float e)
{
  return sqrt(x * x + y * y + z * z);
}

float dist_Manhattan_legacy(float x, float y, float z, float e)
{
  return abs(x) + abs(y) + abs(z);
}

float dist_Chebychev_legacy(float x, float y, float z, float e)
{
  x = abs(x);
  y = abs(y);
  z = abs(z);
  float t = (x > y) ? x : y;
  return (z > t) ? z : t;
}

float dist_MinkovskyH_legacy(float x, float y, float z, float e)
{
  float d = sqrt(abs(x)) + sqrt(abs(y)) + sqrt(abs(z));
  return d * d;
}

float dist_Minkovsky4_legacy(float x, float y, float z, float e)
{
  x *= x;
  y *= y;
  z *= z;
  return sqrt(sqrt(x * x + y * y + z * z));
}

float dist_Minkovsky_legacy(float x, float y, float z, float e)
{
  return pow(pow(abs(x), e) + pow(abs(y), e) + pow(abs(z), e), 1.0 / e);
}

/* HASHPNT(xx, yy, zz) = hashpntf + 3 * hash[ hash[ hash[(z)&255] + (y) ] + (x) ]
 * where `hash` is the 512-entry permutation table (uchar values repeated twice).
 */
vec3 gpu_hashpntf_legacy(int x, int y, int z)
{
  uint zu = uint(z) & 255u;
  uint yu = uint(y) & 255u;
  uint xu = uint(x) & 255u;
  uint h0 = hash[zu];
  uint h1 = hash[(h0 + yu) & 255u];
  uint h2 = hash[(h1 + xu) & 255u];
  return hashpntf3[int(h2)];
}

/* Exact helper matching CPU `BLI_noise_cell_v3()`.
 * Returns the raw `hashpntf` triple for a cell.
 */
vec3 gpu_BLI_noise_cell_v3_legacy(float x, float y, float z)
{
  x = (x + 0.000001) * 1.00001;
  y = (y + 0.000001) * 1.00001;
  z = (z + 0.000001) * 1.00001;
  int xi = int(floor(x));
  int yi = int(floor(y));
  int zi = int(floor(z));
  return gpu_hashpntf_legacy(xi, yi, zi);
}

/* Exact port of CPU `BLI_noise_voronoi(float x, float y, float z, float *da, float *pa, float me, int dtype)`.
 *
 * Inputs:
 * - x,y,z: point
 * - me: minkowski exponent (only used for dtype==6)
 * - dtype: distance type (0..6)
 * Outputs:
 * - da[4]: four nearest distances
 * - pa[12]: feature point positions (4 points * 3 coords). Layout matches CPU.
 */
void bli_noise_voronoi_legacy(float x, float y, float z, out float da[4], out float pa[12], float me, int dtype)
{
  da[0] = da[1] = da[2] = da[3] = 1e10;

  int xi = int(floor(x));
  int yi = int(floor(y));
  int zi = int(floor(z));

  for (int xx = xi - 1; xx <= xi + 1; xx++) {
    for (int yy = yi - 1; yy <= yi + 1; yy++) {
      for (int zz = zi - 1; zz <= zi + 1; zz++) {
        vec3 p = gpu_hashpntf_legacy(xx, yy, zz);
        float xd = x - (p.x + float(xx));
        float yd = y - (p.y + float(yy));
        float zd = z - (p.z + float(zz));

        float d;
        if (dtype == 1) {
          d = dist_Squared_legacy(xd, yd, zd, me);
        }
        else if (dtype == 2) {
          d = dist_Manhattan_legacy(xd, yd, zd, me);
        }
        else if (dtype == 3) {
          d = dist_Chebychev_legacy(xd, yd, zd, me);
        }
        else if (dtype == 4) {
          d = dist_MinkovskyH_legacy(xd, yd, zd, me);
        }
        else if (dtype == 5) {
          d = dist_Minkovsky4_legacy(xd, yd, zd, me);
        }
        else if (dtype == 6) {
          d = dist_Minkovsky_legacy(xd, yd, zd, me);
        }
        else {
          d = dist_Real_legacy(xd, yd, zd, me);
        }

        if (d < da[0]) {
          da[3] = da[2];
          da[2] = da[1];
          da[1] = da[0];
          da[0] = d;

          pa[9] = pa[6];
          pa[10] = pa[7];
          pa[11] = pa[8];
          pa[6] = pa[3];
          pa[7] = pa[4];
          pa[8] = pa[5];
          pa[3] = pa[0];
          pa[4] = pa[1];
          pa[5] = pa[2];

          pa[0] = p.x + float(xx);
          pa[1] = p.y + float(yy);
          pa[2] = p.z + float(zz);
        }
        else if (d < da[1]) {
          da[3] = da[2];
          da[2] = da[1];
          da[1] = d;

          pa[9] = pa[6];
          pa[10] = pa[7];
          pa[11] = pa[8];
          pa[6] = pa[3];
          pa[7] = pa[4];
          pa[8] = pa[5];

          pa[3] = p.x + float(xx);
          pa[4] = p.y + float(yy);
          pa[5] = p.z + float(zz);
        }
        else if (d < da[2]) {
          da[3] = da[2];
          da[2] = d;

          pa[9] = pa[6];
          pa[10] = pa[7];
          pa[11] = pa[8];

          pa[6] = p.x + float(xx);
          pa[7] = p.y + float(yy);
          pa[8] = p.z + float(zz);
        }
        else if (d < da[3]) {
          da[3] = d;
          pa[9] = p.x + float(xx);
          pa[10] = p.y + float(yy);
          pa[11] = p.z + float(zz);
        }
      }
    }
  }
}

/* Exact GLSL port of CPU `noise3_perlin(const float vec[3])` in `noise_c.cc`.
 * IMPORTANT: The CPU uses a separate permutation table (g_perlin_data_ub) which is
 * identical to the first 256 values of `hash` (and then repeated). We reuse `hash`
 * while preserving the CPU's `SETUP()` behavior (add 10000, take int&255, use fract).
 *
 * This is used for TEX_STDPERLIN (basis==1) matching.
 */
float noise3_perlin(vec3 vec)
{
  /* CPU SETUP(vec[i], b0, b1, r0, r1):
   * t = val + 10000; b0 = int(t) & 255; b1=(b0+1)&255; r0=t-floor(t); r1=r0-1;
   */
  float t;

  t = vec.x + 10000.0;
  int bx0 = int(t) & 255;
  int bx1 = (bx0 + 1) & 255;
  float rx0 = t - floor(t);
  float rx1 = rx0 - 1.0;

  t = vec.y + 10000.0;
  int by0 = int(t) & 255;
  int by1 = (by0 + 1) & 255;
  float ry0 = t - floor(t);
  float ry1 = ry0 - 1.0;

  t = vec.z + 10000.0;
  int bz0 = int(t) & 255;
  int bz1 = (bz0 + 1) & 255;
  float rz0 = t - floor(t);
  float rz1 = rz0 - 1.0;

  /* CPU uses `char *p = g_perlin_data_ub;` and indexes it directly.
   * Our `hash` is uint[512] but values are in 0..255.
   */
  int i = int(hash[bx0]);
  int j = int(hash[bx1]);

  int b00 = int(hash[i + by0]);
  int b10 = int(hash[j + by0]);
  int b01 = int(hash[i + by1]);
  int b11 = int(hash[j + by1]);

  float sx = rx0 * rx0 * (3.0 - 2.0 * rx0);
  float sy = ry0 * ry0 * (3.0 - 2.0 * ry0);
  float sz = rz0 * rz0 * (3.0 - 2.0 * rz0);

  /* CPU uses `g_perlin_data_v3` (same gradient vectors as `hashvectf`). */
  int idx;
  float u, v;

  idx = 3 * (b00 + bz0);
  u = rx0 * hashvectf[idx] + ry0 * hashvectf[idx + 1] + rz0 * hashvectf[idx + 2];
  idx = 3 * (b10 + bz0);
  v = rx1 * hashvectf[idx] + ry0 * hashvectf[idx + 1] + rz0 * hashvectf[idx + 2];
  float a = mix(u, v, sx);

  idx = 3 * (b01 + bz0);
  u = rx0 * hashvectf[idx] + ry1 * hashvectf[idx + 1] + rz0 * hashvectf[idx + 2];
  idx = 3 * (b11 + bz0);
  v = rx1 * hashvectf[idx] + ry1 * hashvectf[idx + 1] + rz0 * hashvectf[idx + 2];
  float b = mix(u, v, sx);

  float c = mix(a, b, sy);

  idx = 3 * (b00 + bz1);
  u = rx0 * hashvectf[idx] + ry0 * hashvectf[idx + 1] + rz1 * hashvectf[idx + 2];
  idx = 3 * (b10 + bz1);
  v = rx1 * hashvectf[idx] + ry0 * hashvectf[idx + 1] + rz1 * hashvectf[idx + 2];
  a = mix(u, v, sx);

  idx = 3 * (b01 + bz1);
  u = rx0 * hashvectf[idx] + ry1 * hashvectf[idx + 1] + rz1 * hashvectf[idx + 2];
  idx = 3 * (b11 + bz1);
  v = rx1 * hashvectf[idx] + ry1 * hashvectf[idx + 1] + rz1 * hashvectf[idx + 2];
  b = mix(u, v, sx);

  float d = mix(a, b, sy);

  return 1.5 * mix(c, d, sz);
}

float orgBlenderNoise(float x, float y, float z)
{
  float cn1, cn2, cn3, cn4, cn5, cn6, i_tmp;
  float ox, oy, oz, jx, jy, jz;
  float n = 0.5;
  int ix, iy, iz, b00, b01, b10, b11, b20, b21;

  float fx = floor(x);
  float fy = floor(y);
  float fz = floor(z);

  ox = x - fx;
  oy = y - fy;
  oz = z - fz;

  ix = int(fx);
  iy = int(fy);
  iz = int(fz);

  jx = ox - 1.0;
  jy = oy - 1.0;
  jz = oz - 1.0;

  cn1 = ox * ox;
  cn2 = oy * oy;
  cn3 = oz * oz;
  cn4 = jx * jx;
  cn5 = jy * jy;
  cn6 = jz * jz;

  cn1 = 1.0 - 3.0 * cn1 + 2.0 * cn1 * ox;
  cn2 = 1.0 - 3.0 * cn2 + 2.0 * cn2 * oy;
  cn3 = 1.0 - 3.0 * cn3 + 2.0 * cn3 * oz;
  cn4 = 1.0 - 3.0 * cn4 - 2.0 * cn4 * jx;
  cn5 = 1.0 - 3.0 * cn5 - 2.0 * cn5 * jy;
  cn6 = 1.0 - 3.0 * cn6 - 2.0 * cn6 * jz;

  /* Compute hash indices with proper wrapping (matches CPU uchar behavior)
   * CRITICAL: hash[] is uint[512], but CPU uses uchar[512]. We must mask to [0,255]
   * to avoid out-of-bounds access and match CPU wrap-around semantics. */
  b00 = int(hash[(hash[ix & 255] + uint(iy & 255)) & 255u]);
  b10 = int(hash[(hash[(ix + 1) & 255] + uint(iy & 255)) & 255u]);
  b01 = int(hash[(hash[ix & 255] + uint((iy + 1) & 255)) & 255u]);
  b11 = int(hash[(hash[(ix + 1) & 255] + uint((iy + 1) & 255)) & 255u]);

  b20 = iz & 255;
  b21 = (iz + 1) & 255;

  /* Sample 8 corners of the cube (matches CPU line 224-248) */
  int h_idx;
  vec3 h;

  /* corner 0 */
  i_tmp = cn1 * cn2 * cn3;
  h_idx = 3 * int(hash[(b20 + b00) & 255u]);
  h = vec3(hashvectf[h_idx], hashvectf[h_idx + 1], hashvectf[h_idx + 2]);
  n += i_tmp * (h.x * ox + h.y * oy + h.z * oz);

  /* corner 1 */
  i_tmp = cn1 * cn2 * cn6;
  h_idx = 3 * int(hash[(b21 + b00) & 255u]);
  h = vec3(hashvectf[h_idx], hashvectf[h_idx + 1], hashvectf[h_idx + 2]);
  n += i_tmp * (h.x * ox + h.y * oy + h.z * jz);

  /* corner 2 */
  i_tmp = cn1 * cn5 * cn3;
  h_idx = 3 * int(hash[(b20 + b01) & 255u]);
  h = vec3(hashvectf[h_idx], hashvectf[h_idx + 1], hashvectf[h_idx + 2]);
  n += i_tmp * (h.x * ox + h.y * jy + h.z * oz);

  /* corner 3 */
  i_tmp = cn1 * cn5 * cn6;
  h_idx = 3 * int(hash[(b21 + b01) & 255u]);
  h = vec3(hashvectf[h_idx], hashvectf[h_idx + 1], hashvectf[h_idx + 2]);
  n += i_tmp * (h.x * ox + h.y * jy + h.z * jz);

  /* corner 4 */
  i_tmp = cn4 * cn2 * cn3;
  h_idx = 3 * int(hash[(b20 + b10) & 255u]);
  h = vec3(hashvectf[h_idx], hashvectf[h_idx + 1], hashvectf[h_idx + 2]);
  n += i_tmp * (h.x * jx + h.y * oy + h.z * oz);

  /* corner 5 */
  i_tmp = cn4 * cn2 * cn6;
  h_idx = 3 * int(hash[(b21 + b10) & 255u]);
  h = vec3(hashvectf[h_idx], hashvectf[h_idx + 1], hashvectf[h_idx + 2]);
  n += i_tmp * (h.x * jx + h.y * oy + h.z * jz);

  /* corner 6 */
  i_tmp = cn4 * cn5 * cn3;
  h_idx = 3 * int(hash[(b20 + b11) & 255u]);
  h = vec3(hashvectf[h_idx], hashvectf[h_idx + 1], hashvectf[h_idx + 2]);
  n += i_tmp * (h.x * jx + h.y * jy + h.z * oz);

  /* corner 7 */
  i_tmp = cn4 * cn5 * cn6;
  h_idx = 3 * int(hash[(b21 + b11) & 255u]);
  h = vec3(hashvectf[h_idx], hashvectf[h_idx + 1], hashvectf[h_idx + 2]);
  n += i_tmp * (h.x * jx + h.y * jy + h.z * jz);

  /* Clamp result */
  if (n < 0.0) {
    n = 0.0;
  }
  else if (n > 1.0) {
    n = 1.0;
  }

  return n;
}
)GLSL";
}

static const std::string get_noise_helpers_part4_glsl()
{
  return R"GLSL(

/* Helpers for improved Perlin (newPerlinU) used by `bli_noise_generic_turbulence`. */
float npfade(float t)
{
  return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float newperlin_grad(int hash_val, float x, float y, float z)
{
  int h = hash_val & 15;
  float u = (h < 8) ? x : y;
  float v = (h < 4) ? y : ((h == 12) || (h == 14)) ? x : z;
  return (((h & 1) == 0) ? u : -u) + (((h & 2) == 0) ? v : -v);
}

float newPerlinU(float x, float y, float z)
{
  float ux = floor(x);
  float uy = floor(y);
  float uz = floor(z);
  int X = int(ux) & 255;
  int Y = int(uy) & 255;
  int Z = int(uz) & 255;
  x -= ux;
  y -= uy;
  z -= uz;

  float u = npfade(x);
  float v = npfade(y);
  float w = npfade(z);

  int A = int(hash[X]) + Y;
  int AA = int(hash[uint(A) & 255u]) + Z;
  int AB = int(hash[uint(A + 1) & 255u]) + Z;
  int B = int(hash[uint(X + 1) & 255u]) + Y;
  int BA = int(hash[uint(B) & 255u]) + Z;
  int BB = int(hash[uint(B + 1) & 255u]) + Z;

  float g1 = newperlin_grad(int(hash[uint(AA) & 255u]), x, y, z);
  float g2 = newperlin_grad(int(hash[uint(BA) & 255u]), x - 1.0, y, z);
  float a1 = mix(g1, g2, u);

  g1 = newperlin_grad(int(hash[uint(AB) & 255u]), x, y - 1.0, z);
  g2 = newperlin_grad(int(hash[uint(BB) & 255u]), x - 1.0, y - 1.0, z);
  float b1 = mix(g1, g2, u);
  float c1 = mix(a1, b1, v);

  g1 = newperlin_grad(int(hash[uint(AA + 1) & 255u]), x, y, z - 1.0);
  g2 = newperlin_grad(int(hash[uint(BA + 1) & 255u]), x - 1.0, y, z - 1.0);
  a1 = mix(g1, g2, u);

  g1 = newperlin_grad(int(hash[uint(AB + 1) & 255u]), x, y - 1.0, z - 1.0);
  g2 = newperlin_grad(int(hash[uint(BB + 1) & 255u]), x - 1.0, y - 1.0, z - 1.0);
  b1 = mix(g1, g2, u);
  float d1 = mix(a1, b1, v);

  return 0.5 + 0.5 * mix(c1, d1, w);
}

float bli_noise_generic_turbulence(float size, float x, float y, float z, int depth, bool hard, int basis)
{
  /* If default basis (0) is used, CPU adds +1 to coords before scaling. */
  bool default_basis = (basis == 0);
  if (default_basis) {
    x += 1.0;
    y += 1.0;
    z += 1.0;
  }

  /* Normalize coordinates by size (matches CPU) */
  if (size != 0.0) {
    float inv_size = 1.0 / size;
    x *= inv_size;
    y *= inv_size;
    z *= inv_size;
  }

  /* cell noise helper (CPU implementation port) */
  /* implemented as a static function above (gpu_BLI_cellNoiseU) */

  float sum = 0.0;
  float amp = 1.0;
  float fscale = 1.0;

  for (int i = 0; i <= depth; ++i) {
    float sx = fscale * x;
    float sy = fscale * y;
    float sz = fscale * z;
    float t = 0.0;

    if (basis == 1) { /* orgPerlinNoiseU */
      /* `noise3_perlin` matches CPU `noise3_perlin()` used by `orgPerlinNoiseU`. */
      t = 0.5 + 0.5 * noise3_perlin(vec3(sx, sy, sz));
    }
    else if (basis == 2) { /* newPerlinU */
      t = newPerlinU(sx, sy, sz);
    }
    else if (basis >= 3 && basis <= 8) { /* Voronoi legacy variants: 3..8 */
      float da[4];
      float pa[12];
      bli_noise_voronoi_legacy(sx, sy, sz, da, pa, 1.0, 0);
      if (basis == 3) t = da[0];
      else if (basis == 4) t = da[1];
      else if (basis == 5) t = da[2];
      else if (basis == 6) t = da[3];
      else if (basis == 7) t = da[1] - da[0];
      else {
        float v = 10.0 * (da[1] - da[0]);
        t = (v > 1.0) ? 1.0 : v;
      }
    }
    else if (basis == 14) {
      t = gpu_BLI_cellNoiseU(sx, sy, sz);
    }
    else { /* default/fallback -> orgBlenderNoise */
      t = orgBlenderNoise(sx, sy, sz);
    }

    if (hard) {
      t = abs(2.0 * t - 1.0);
    }

    sum += t * amp;
    amp *= 0.5;
    fscale *= 2.0;
  }

  sum *= float(1 << depth) / float((1 << (depth + 1)) - 1);
  return sum;
}

/* Helper: return signed basis noise for musgrave variants (port of BLI_noise_basis_signed) */
float bli_noise_basis_signed(float x, float y, float z, int basis)
{
  if (basis == 1) {
    /* orgPerlinNoise (signed) */
    return noise3_perlin(vec3(x, y, z));
  }
  if (basis == 2) {
    /* newPerlin (signed) */
    return 2.0 * newPerlinU(x, y, z) - 1.0;
  }
  if (basis >= 3 && basis <= 8) {
    /* Voronoi signed variants used by musgrave functions on CPU. */
    float da[4];
    float pa[12];
    bli_noise_voronoi_legacy(x, y, z, da, pa, 1.0, 0);
    if (basis == 3) return 2.0 * da[0] - 1.0;
    if (basis == 4) return 2.0 * da[1] - 1.0;
    if (basis == 5) return 2.0 * da[2] - 1.0;
    if (basis == 6) return 2.0 * da[3] - 1.0;
    if (basis == 7) return 2.0 * (da[1] - da[0]) - 1.0;
    /* basis == 8 crackle */
    float t = 10.0 * (da[1] - da[0]);
    return 2.0 * min(t, 1.0) - 1.0;
  }
  if (basis == 14) {
    return 2.0 * gpu_BLI_cellNoiseU(x, y, z) - 1.0;
  }

  /* TEX_BLENDER / fallback: orgBlenderNoiseS on CPU. */
  return 2.0 * orgBlenderNoise(x, y, z) - 1.0;
}

/* GLSL port of CPU `BLI_noise_generic_noise()` (unsigned noise selector).
 * Chooses per-basis function and applies noisesize scaling and optional "hard" transform.
 */
float bli_noise_generic_noise(float noisesize, float x, float y, float z, bool hard, int noisebasis)
{
  int nb = noisebasis;
  /* default basis (0) on CPU adds +1 to coords to match BLI_noise_hnoise behavior */
  if (nb == 0) {
    x += 1.0;
    y += 1.0;
    z += 1.0;
  }

  if (noisesize != 0.0) {
    float inv = 1.0 / noisesize;
    x *= inv;
    y *= inv;
    z *= inv;
  }

  float t = 0.0;
  if (nb == 1) {
    /* orgPerlinNoiseU */
    t = 0.5 + 0.5 * noise3_perlin(vec3(x, y, z));
  }
  else if (nb == 2) {
    /* newPerlinU */
    t = newPerlinU(x, y, z);
  }
  else if (nb >= 3 && nb <= 8) {
    float da[4];
    float pa[12];
    bli_noise_voronoi_legacy(x, y, z, da, pa, 1.0, 0);
    if (nb == 3) t = da[0];
    else if (nb == 4) t = da[1];
    else if (nb == 5) t = da[2];
    else if (nb == 6) t = da[3];
    else if (nb == 7) t = da[1] - da[0];
    else { /* 8: crackle */
      float v = 10.0 * (da[1] - da[0]);
      t = (v > 1.0) ? 1.0 : v;
    }
  }
  else if (nb == 14) {
    t = gpu_BLI_cellNoiseU(x, y, z);
  }
  else {
    /* default/fallback uses orgBlenderNoise */
    t = orgBlenderNoise(x, y, z);
  }

  if (hard) {
    t = abs(2.0 * t - 1.0);
  }

  return t;
}

)GLSL";
}

static const std::string get_noise_helpers_part5_glsl()
{
  return R"GLSL(

/* Musgrave: fBm (fractional Brownian motion) GLSL port of BLI_noise_mg_fbm */
float bli_noise_mg_fbm(float x, float y, float z, float H, float lacunarity, float octaves, int noisebasis)
{
  float value = 0.0;
  float pwr = 1.0;
  float pwHL = pow(lacunarity, -H);
  int oct = int(octaves);
  for (int i = 0; i < oct; i++) {
    float n = bli_noise_basis_signed(x, y, z, noisebasis);
    value += n * pwr;
    pwr *= pwHL;
    x *= lacunarity;
    y *= lacunarity;
    z *= lacunarity;
  }

  float rmd = octaves - floor(octaves);
  if (rmd != 0.0) {
    value += rmd * bli_noise_basis_signed(x, y, z, noisebasis) * pwr;
  }

  return value;
}

/* Musgrave: Hybrid Multifractal GLSL port of BLI_noise_mg_hybrid_multi_fractal */
float bli_noise_mg_hybrid_multi_fractal(float x,
                                        float y,
                                        float z,
                                        float H,
                                        float lacunarity,
                                        float octaves,
                                        float offset,
                                        float gain,
                                        int noisebasis)
{
  float result = bli_noise_basis_signed(x, y, z, noisebasis) + offset;
  float weight = gain * result;
  x *= lacunarity;
  y *= lacunarity;
  z *= lacunarity;

  float pwHL = pow(lacunarity, -H);
  float pwr = pwHL; /* starts with i=1 instead of 0 */
  int oct = int(octaves);
  for (int i = 1; (weight > 0.001) && (i < oct); i++) {
    weight = min(weight, 1.0);
    float signal = (bli_noise_basis_signed(x, y, z, noisebasis) + offset) * pwr;
    pwr *= pwHL;
    result += weight * signal;
    weight *= gain * signal;
    x *= lacunarity;
    y *= lacunarity;
    z *= lacunarity;
  }

  float rmd = octaves - floor(octaves);
  if (rmd != 0.0) {
    result += rmd * ((bli_noise_basis_signed(x, y, z, noisebasis) + offset) * pwr);
  }

  return result;
}

/* Musgrave: Variable Lacunarity GLSL port of BLI_noise_mg_variable_lacunarity */
float bli_noise_mg_variable_lacunarity(
    float x, float y, float z, float distortion, int nbas1, int nbas2)
{
  float rv0 = bli_noise_basis_signed(x + 13.5, y + 13.5, z + 13.5, nbas1) * distortion;
  float rv1 = bli_noise_basis_signed(x, y, z, nbas1) * distortion;
  float rv2 = bli_noise_basis_signed(x - 13.5, y - 13.5, z - 13.5, nbas1) * distortion;

  return bli_noise_basis_signed(x + rv0, y + rv1, z + rv2, nbas2);
}

/* Musgrave: Ridged Multifractal GLSL port of BLI_noise_mg_ridged_multi_fractal */
float bli_noise_mg_ridged_multi_fractal(float x,
                                        float y,
                                        float z,
                                        float H,
                                        float lacunarity,
                                        float octaves,
                                        float offset,
                                        float gain,
                                        int noisebasis)
{
  float signal = pow(offset - abs(bli_noise_basis_signed(x, y, z, noisebasis)), 2.0);
  float result = signal;
  float pwHL = pow(lacunarity, -H);
  float pwr = pwHL; /* starts with i=1 instead of 0 */
  int oct = int(octaves);
  for (int i = 1; i < oct; i++) {
    x *= lacunarity;
    y *= lacunarity;
    z *= lacunarity;
    float weight = signal * gain;
    weight = clamp(weight, 0.0, 1.0);
    signal = offset - abs(bli_noise_basis_signed(x, y, z, noisebasis));
    signal *= signal;
    signal *= weight;
    result += signal * pwr;
    pwr *= pwHL;
  }

  return result;
}

/* Musgrave: Multi Fractal GLSL port of BLI_noise_mg_multi_fractal */
float bli_noise_mg_multi_fractal(float x, float y, float z, float H, float lacunarity, float octaves, int noisebasis)
{
  float value = 1.0;
  float pwr = 1.0;
  float pwHL = pow(lacunarity, -H);
  int oct = int(octaves);
  for (int i = 0; i < oct; i++) {
    float n = bli_noise_basis_signed(x, y, z, noisebasis);
    value *= (pwr * n + 1.0);
    pwr *= pwHL;
    x *= lacunarity;
    y *= lacunarity;
    z *= lacunarity;
  }

  float rmd = octaves - floor(octaves);
  if (rmd != 0.0) {
    value *= (rmd * bli_noise_basis_signed(x, y, z, noisebasis) * pwr + 1.0);
  }

  return value;
}

/* Musgrave: Heterogeneous Terrain GLSL port of BLI_noise_mg_hetero_terrain */
float bli_noise_mg_hetero_terrain(float x,
                                  float y,
                                  float z,
                                  float H,
                                  float lacunarity,
                                  float octaves,
                                  float offset,
                                  int noisebasis)
{
  /* first unscaled octave of function; later octaves are scaled */
  float value = offset + bli_noise_basis_signed(x, y, z, noisebasis);
  x *= lacunarity;
  y *= lacunarity;
  z *= lacunarity;

  float pwHL = pow(lacunarity, -H);
  float pwr = pwHL; /* starts with i=1 instead of 0 */
  int oct = int(octaves);
  for (int i = 1; i < oct; i++) {
    float increment = (bli_noise_basis_signed(x, y, z, noisebasis) + offset) * pwr * value;
    value += increment;
    pwr *= pwHL;
    x *= lacunarity;
    y *= lacunarity;
    z *= lacunarity;
  }

  float rmd = octaves - floor(octaves);
  if (rmd != 0.0) {
    float increment = (bli_noise_basis_signed(x, y, z, noisebasis) + offset) * pwr * value;
    value += rmd * increment;
  }
  return value;
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
static const std::string get_boxsample_helpers_glsl()
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
static const std::string get_boxsample_core_glsl()
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
static const std::string get_texture_mapping_glsl()
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

static const std::string get_imagewrap()
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
  if (u_tex_flip_axis) {
    float temp = fx;
    fx = fy;
    fy = temp;
  }

  /* Step 2: TEX_CHECKER filtering */
  if (u_tex_extend == TEX_CHECKER) {
    int xs = int(floor(fx));
    int ys = int(floor(fy));
    int tile_parity = (xs + ys) & 1;
    
    bool show_tile = true;
    if (u_tex_checker_odd && (tile_parity == 0)) {
      show_tile = false;
    }
    if (u_tex_checker_even && (tile_parity == 1)) {
      show_tile = false;
    }
    
    if (!show_tile) {
      return retval;
    }

    fx -= float(xs);
    fy -= float(ys);
    
    if (u_tex_checkerdist < 1.0) {
      fx = (fx - 0.5) / (1.0 - u_tex_checkerdist) + 0.5;
      fy = (fy - 0.5) / (1.0 - u_tex_checkerdist) + 0.5;
    }
  }
  
  /* Step 3: Compute integer pixel coordinates */
  int x = int(floor(fx * float(tex_size.x)));
  int y = int(floor(fy * float(tex_size.y)));
  int xi = x;
  int yi = y;
  
  /* Step 4: CLIPCUBE early return */
  if (u_tex_extend == TEX_CLIPCUBE) {
    if (x < 0 || y < 0 || x >= tex_size.x || y >= tex_size.y ||
        tex_coord.z < -1.0 || tex_coord.z > 1.0) {
      return retval;
    }
  }
  /* Step 5: CLIP/CHECKER early return */
  else if (u_tex_extend == TEX_CLIP || u_tex_extend == TEX_CHECKER) {
    if (x < 0 || y < 0 || x >= tex_size.x || y >= tex_size.y) {
      return retval;
    }
  }
  /* Step 6: EXTEND or REPEAT mode: wrap/clamp coordinates */
  else {
    if (u_tex_extend == TEX_EXTEND) {
      x = (x >= tex_size.x) ? (tex_size.x - 1) : ((x < 0) ? 0 : x);
    }
    else {
      x = x % tex_size.x;
      if (x < 0) x += tex_size.x;
    }
    
    if (u_tex_extend == TEX_EXTEND) {
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
  if (u_tex_interpol) {
#if DISP_TEXSAMPLER_MODE == DISP_TEXSAMPLER_CPU_BOXSAMPLE
    /* CPU-like path: area filtered boxsample.
     * This is closer to `texture_image.cc::boxsample()` but can be very slow on GPU.
     */
    float filterx = (0.5 * u_tex_filtersize) / float(tex_size.x);
    float filtery = (0.5 * u_tex_filtersize) / float(tex_size.y);

    /* Match CPU: wrap back to float coords after integer clamp/wrap. */
    fx -= float(xi - x) / float(tex_size.x);
    fy -= float(yi - y) / float(tex_size.y);

    float min_tex_x = fx - filterx;
    float min_tex_y = fy - filtery;
    float max_tex_x = fx + filterx;
    float max_tex_y = fy + filtery;

    TexResult_tex texres_box;
    texres_box.trgba = vec4(0.0);
    texres_box.talpha = u_use_talpha;

    boxsample(displacement_texture,
              tex_size,
              min_tex_x,
              min_tex_y,
              max_tex_x,
              max_tex_y,
              texres_box,
              (u_tex_extend == TEX_REPEAT),
              (u_tex_extend == TEX_EXTEND),
              u_tex_is_byte,
              u_tex_is_float,
              u_tex_channels);

    result = texres_box.trgba;
#else
    /* Fast path: rely on hardware bilinear sampling.
     * This approximates CPU filtering but avoids the heavy nested loops of boxsample.
     */
    /* Match CPU wrap-remap (#27782) even in fast mode. */
    float u = fx - float(xi - x) / float(tex_size.x);
    float v = fy - float(yi - y) / float(tex_size.y);

    if (u_tex_extend == TEX_EXTEND) {
      u = clamp(u, 0.0, 1.0);
      v = clamp(v, 0.0, 1.0);
    }
    else if (u_tex_extend == TEX_REPEAT) {
      /* After CPU-style remap, u/v are expected to already be in wrapped space.
       * Keep them as-is to mimic CPU x/y wrapping behavior.
       */
    }

    result = shader_ibuf_get_color(texture(displacement_texture, vec2(u, v)),
                                   u_tex_is_float,
                                   u_tex_channels,
                                   u_tex_is_byte);
#endif
  }
  else {
    ivec2 px_coord = ivec2(x, y);
    px_coord = clamp(px_coord, ivec2(0), tex_size - 1);
    result = shader_ibuf_get_color(texelFetch(displacement_texture, px_coord, 0),
                                  u_tex_is_float,
                                  u_tex_channels,
                                  u_tex_is_byte);
  }
  
  /* Compute intensity */
  if (u_use_talpha) {
    out_tin = result.a;
  }
  else if (u_tex_calcalpha) {
    out_tin = max(max(result.r, result.g), result.b);
    result.a = out_tin;
  }
  else {
    out_tin = 1.0;
    result.a = 1.0;
  }

  if (u_tex_negalpha) {
    result.a = 1.0 - result.a;
  }

  /* De-pre-multiply */
  if (result.a != 1.0 && result.a > 1e-4 && !u_tex_calcalpha) {
    float inv_alpha = 1.0 / result.a;
    result.rgb *= inv_alpha;
  }

  /* BRICONTRGB (brightness/contrast/RGB factors) */
  vec3 rgb = result.rgb;
  rgb.r = u_tex_rfac * ((rgb.r - 0.5) * u_tex_contrast + u_tex_bright - 0.5);
  rgb.g = u_tex_gfac * ((rgb.g - 0.5) * u_tex_contrast + u_tex_bright - 0.5);
  rgb.b = u_tex_bfac * ((rgb.b - 0.5) * u_tex_contrast + u_tex_bright - 0.5);

  if (!u_tex_no_clamp) {
    rgb = max(rgb, vec3(0.0));
  }

  /* Apply saturation */
  if (u_tex_saturation != 1.0) {
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

    s *= u_tex_saturation;

    float nr = abs(h * 6.0 - 3.0) - 1.0;
    float ng = 2.0 - abs(h * 6.0 - 2.0);
    float nb = 2.0 - abs(h * 6.0 - 4.0);

    nr = clamp(nr, 0.0, 1.0);
    ng = clamp(ng, 0.0, 1.0);
    nb = clamp(nb, 0.0, 1.0);

    float r = ((nr - 1.0) * s + 1.0) * v;
    float g = ((ng - 1.0) * s + 1.0) * v;
    float b = ((nb - 1.0) * s + 1.0) * v;

    if (u_tex_saturation > 1.0 && !u_tex_no_clamp) {
      rgb = max(rgb, vec3(0.0));
    }
  }

  result.rgb = rgb;
  return retval;
}
#endif  // HAS_TEXTURE
)GLSL";
}

static const std::string get_multitex_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* Generic multitex dispatcher: calls procedural texture helpers based on a set of
 * uniforms describing the current `Tex` state. This is a simplified GPU-side
 * port intended for compute shaders (e.g. Displace). The caller must provide
 * the following uniforms when using this dispatcher:
 *
 * uniform int u_tex_type;
 * uniform int u_tex_stype;
 * uniform int u_tex_noisebasis;
 * uniform int u_tex_noisebasis2; //marble and ?
 * uniform float u_tex_noisesize;
 * uniform float u_tex_turbul;
 * uniform int u_tex_noisedepth;
 * uniform bool u_tex_flipblend;
 * int tex_mt -> uniform u_tex_stype; // marble mode
 * uniform sampler2D displacement_texture; // for image types (sampler name used by imagewrap)
 * uniform ivec2 u_tex_image_size (not needed - use textureSize);
 * uniform bool u_use_colorband; // whether to apply colorband
 * uniform ColorBand tex_colorband; // ColorBand UBO
 */

int multitex(vec3 texvec, inout TexResult_tex texres, int thread_id)
{
  texres.talpha = false;
  int retval = 0;
  int t = u_tex_type;

  if (t == 0) {
    texres.tin = 0.0;
    return 0;
  }

  if (t == TEX_CLOUDS) {
    retval = clouds_tex(texvec, texres, u_tex_noisesize, u_tex_turbul, u_tex_noisedepth, u_tex_noisebasis, u_tex_stype);
  }
  else if (t == TEX_WOOD) {
    retval = wood_tex(texvec, texres, u_tex_noisebasis, u_tex_noisebasis2, u_tex_stype, u_tex_noisesize, u_tex_turbul);
  }
  else if (t == TEX_MARBLE) {
    /* Marble uses `stype` as its mode in CPU code (tex->stype). Use tex_stype here. */
    retval = marble_tex(texvec, texres, u_tex_noisebasis2, u_tex_stype, u_tex_noisesize, u_tex_turbul);
  }
  else if (t == TEX_MAGIC) {
    retval = magic_tex(texvec, texres, u_tex_turbul);
  }
  else if (t == TEX_BLEND) {
    retval = blend_tex(texvec, texres, u_tex_stype, u_tex_flipblend);
  }
  else if (t == TEX_STUCCI) {
    retval = stucci_tex(texvec, texres, u_tex_stype, u_tex_noisesize, u_tex_turbul, u_tex_noisebasis);
  }
  else if (t == TEX_MUSGRAVE) {
    /* Match CPU: scale texvec by 1/noisesize before Musgrave evaluation. */
    vec3 tmp = texvec;
    if (u_tex_noisesize != 0.0) {
      tmp *= (1.0 / u_tex_noisesize);
    }
    retval = musgrave_tex_ubo(tmp, texres);
  }
  else if (t == TEX_NOISE) {
    retval = texnoise_tex(texres, thread_id, u_tex_noisedepth);
  }
  else if (t == TEX_VORONOI) {
    /* Match CPU: scale texvec by 1/noisesize before Voronoi evaluation. */
    vec3 tmp = texvec;
    if (u_tex_noisesize != 0.0) {
      tmp *= (1.0 / u_tex_noisesize);
    }
    retval = voronoi_tex_ubo(tmp, texres);
  }
  else if (t == TEX_DISTNOISE) {
    retval = distnoise_tex(texvec, texres, u_tex_noisesize, u_tex_distamount, u_tex_noisebasis, u_tex_noisebasis2);
  }
  else if (t == TEX_IMAGE) {
    /* Image sampling: apply 2D mapping (scale/rotation/repeat/mirror/crop/offset)
     * only for image textures, then call imagewrap() which handles sampling.
     * This matches CPU behavior where do_2d_mapping is applied only for images. */
    float fx = (texvec.x + 1.0) / 2.0;
    float fy = (texvec.y + 1.0) / 2.0;

    /* Apply mapping transforms (SCALE → ROTATION → REPEAT → MIRROR → CROP → OFFSET) */
    do_2d_mapping(fx, fy, u_tex_extend, u_tex_repeat, u_tex_xmir, u_tex_ymir, u_tex_crop, u_tex_size_param, u_tex_ofs, u_tex_rot);

    vec3 mapped_coord = vec3(fx, fy, texvec.z);
    ivec2 img_size = textureSize(displacement_texture, 0);
    int ir = imagewrap(mapped_coord, texres.trgba, texres.tin, img_size);
    if (ir == TEX_RGB) retval = TEX_RGB;
    else retval = TEX_INT;
  }

  /* Apply colorband if requested (requires ColorBand UBO with name `u_tex_colorband`).
   * ColorBand maps an intensity -> color, so apply it only when the texture
   * result is an intensity (i.e. TEX_RGB flag is NOT set). TEX_INT is defined
   * as 0, so testing `retval & TEX_INT` is always false. */
  if (u_use_colorband) {
    vec4 cbcol;
    if (BKE_colorband_evaluate(tex_colorband, texres.tin, cbcol)) {
      texres.talpha = true;
      texres.trgba = cbcol;
      retval |= TEX_RGB;
    }
  }

  return retval;
}
#endif // HAS_TEXTURE
)GLSL";
}

static const std::string get_multitex_image_only_glsl()
{
  return R"GLSL(
#ifdef HAS_TEXTURE

/* Minimal multitex dispatcher that only implements the IMAGE branch.
 * Used to reduce compile-time when only image sampling is required.
 */
int multitex(vec3 texvec, inout TexResult_tex texres, int thread_id)
{
  texres.talpha = false;
  int retval = 0;
  int t = u_tex_type;

  if (t == 0) {
    texres.tin = 0.0;
    return 0;
  }

  if (t == TEX_IMAGE) {
    /* Map coordinates from [-1,1] to [0,1] like CPU for image textures */
    float fx = (texvec.x + 1.0) / 2.0;
    float fy = (texvec.y + 1.0) / 2.0;

    /* Apply 2D mapping transforms (scale/rotation/repeat/mirror/crop/offset) */
    do_2d_mapping(fx, fy, u_tex_extend, u_tex_repeat, u_tex_xmir, u_tex_ymir, u_tex_crop, u_tex_size_param, u_tex_ofs, u_tex_rot);

    vec3 mapped_coord = vec3(fx, fy, texvec.z);
    ivec2 img_size = textureSize(displacement_texture, 0);
    int ir = imagewrap(mapped_coord, texres.trgba, texres.tin, img_size);
    if (ir == TEX_RGB) retval = TEX_RGB;
    else retval = TEX_INT;
  }

  /* Apply colorband if requested */
  if (u_use_colorband) {
    vec4 cbcol;
    if (BKE_colorband_evaluate(tex_colorband, texres.tin, cbcol)) {
      texres.talpha = true;
      texres.trgba = cbcol;
      retval |= TEX_RGB;
    }
  }

  return retval;
}
#endif // HAS_TEXTURE
)GLSL";
}

static const std::string get_common_texture_bke_get_value_glsl()
{
  return R"GLSL(
/* Returns intensity in [0..1]. When compiled without HAS_TEXTURE returns 1.0
 * so shaders that multiply by the sampled intensity keep the value unchanged. */
float BKE_texture_get_value(inout TexResult_tex texres, vec3 texture_coords_v, vec4 input_pos_v, int idx)
{
#ifdef HAS_TEXTURE
  /* mapping selection (macros from get_texture_params_glsl() map these to tex_params) */
  vec3 tex_coord = texture_coords_v;

  if (u_mapping_use_input_positions) {
    vec3 in_pos = input_pos_v.xyz;
    if (u_tex_mapping == 0) { /* MOD_DISP_MAP_LOCAL */
      tex_coord = in_pos;
    } else if (u_tex_mapping == 1) { /* MOD_DISP_MAP_GLOBAL */
      vec4 w = u_object_to_world_mat * vec4(in_pos, 1.0);
      tex_coord = w.xyz;
    } else if (u_tex_mapping == 2) { /* MOD_DISP_MAP_OBJECT */
      vec4 w = u_object_to_world_mat * vec4(in_pos, 1.0);
      vec4 o = u_mapref_imat * w;
      tex_coord = o.xyz;
    } else {
      tex_coord = texture_coords_v;
    }
  }

  /* Initialize provided texres and sample using the shared multitex path. */
  texres.trgba = vec4(0.0);
  texres.talpha = u_use_talpha; /* mapped to TextureParams UBO by macros */
  texres.tin = 0.0;

  vec3 mapped_coord = tex_coord;
  int retval = multitex(mapped_coord, texres, idx);

  vec3 rgb = texres.trgba.rgb;
  if ((retval & TEX_RGB) != 0) {
    texres.tin = (rgb.r + rgb.g + rgb.b) / 3.0;
  } else {
    texres.trgba.rgb = vec3(texres.tin);
  }

  return texres.tin;
#else
  return 1.0; /* no texture: neutral multiplier */
#endif
}
)GLSL";
}

/** \} */

/**
 * Returns GLSL code for common texture processing functions.
 * Call this to get the full texture library.
 */
const std::string &get_common_texture_lib_glsl()
{
  static const std::string texture_lib =
      /* Prepend texture subtype defines so shaders can reference TEX_* stype values
       * in a central place. These are placeholders that mirror the CPU-side names and
       * can be adjusted if DNA values differ. */
      get_texture_defines_glsl() + get_color_conversion_glsl() + get_bricont_glsl() +
      get_waveform_helpers_glsl() + get_noise_helpers_part1_glsl() +
      get_noise_helpers_part2_glsl() + get_noise_helpers_part3_glsl() +
      get_noise_helpers_part4_glsl() + get_noise_helpers_part5_glsl() +
      get_colorband_helpers_glsl() +
      /* core structs used by texture functions */
      get_texture_structs_glsl() +
      /* Procedural textures in same order as CPU `multitex` switch */
      get_clouds_glsl() + get_wood_glsl() + get_marble_glsl() + get_magic_glsl() +
      get_blend_glsl() + get_stucci_glsl() + get_texnoise_glsl() +
      /* Voronoi and other procedural types */
      get_voronoi_glsl() + get_distnoise_glsl() +
      /* Boxsample / image helpers and mapping last */
      get_boxsample_helpers_glsl() + get_boxsample_core_glsl() + get_texture_mapping_glsl() +
      get_imagewrap() + get_multitex_glsl() + get_common_texture_bke_get_value_glsl();
  return texture_lib;
}

const std::string &get_common_texture_image_lib_glsl()
{
  static const std::string texture_lib =
      get_texture_defines_glsl() + get_color_conversion_glsl() + get_bricont_glsl() +
      /* ColorBand helpers required by imagewrap() / multitex IMAGE branch */
      get_colorband_helpers_glsl() +
      /* core structs used by texture functions */
      get_texture_structs_glsl() +
      /* Boxsample / image helpers and mapping */
      get_boxsample_helpers_glsl() + get_boxsample_core_glsl() + get_texture_mapping_glsl() +
      get_imagewrap() +
      /* Minimal multitex handling for IMAGE only */
      get_multitex_image_only_glsl() + get_common_texture_bke_get_value_glsl();
  return texture_lib;
}

}  // namespace gpu
}  // namespace blender
