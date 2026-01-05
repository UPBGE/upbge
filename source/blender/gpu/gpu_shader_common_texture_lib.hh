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

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name ColorBand Defines and Helper Functions
 * \{ */

/**
 * Returns GLSL code for ColorBand-related defines and helper functions.
 * Includes interpolation types, color modes, hue modes, and key_curve_position_weights.
 */
static std::string get_colorband_helpers_glsl()
{
  return R"GLSL(
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Boxsample Structures and Helpers
 * \{ */

/**
 * Returns GLSL code for boxsample helper structures and functions.
 * Includes rctf, TexResult_boxsample, ibuf_get_color, clipping functions.
 */
static std::string get_boxsample_helpers_glsl()
{
  return R"GLSL(
/* GPU port of rctf structure from BLI_rect.h */
struct rctf {
  float xmin;
  float xmax;
  float ymin;
  float ymax;
};

struct TexResult_boxsample {
  vec4 trgba;
  bool talpha;
};

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
void boxsampleclip(sampler2D displacement_texture, ivec2 tex_size, rctf rf, inout TexResult_boxsample texres, bool tex_is_byte, bool tex_is_float, int tex_channels)
{
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
}

void boxsample(sampler2D displacement_texture,
              ivec2 tex_size,
              float minx,
              float miny,
              float maxx,
              float maxy,
              inout TexResult_boxsample texres,
              bool imaprepeat,
              bool imapextend,
              bool tex_is_byte,
              bool tex_is_float,
              int tex_channels)
{
  TexResult_boxsample texr;
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
  else if (imaprepeat) {
    alphaclip = clipx_rctf(rf, 0.0, float(tex_size.x));
    if (alphaclip <= 0.0) {
      texres.trgba[0] = texres.trgba[2] = texres.trgba[1] = texres.trgba[3] = 0.0;
      return;
    }
  }
  else {
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
  else if (imaprepeat) {
    alphaclip *= clipy_rctf(rf, 0.0, float(tex_size.y));
    if (alphaclip <= 0.0) {
      texres.trgba[0] = texres.trgba[2] = texres.trgba[1] = texres.trgba[3] = 0.0;
      return;
    }
  }
  else {
    alphaclip *= clipy_rctf(rf, 0.0, float(tex_size.y));
    if (alphaclip <= 0.0) {
      texres.trgba[0] = texres.trgba[2] = texres.trgba[1] = texres.trgba[3] = 0.0;
      return;
    }
  }

  /* NOTE(GPU divergence): CPU averages multiple rectangles when repeat-splitting occurs.
   * Since we do not split, we always sample a single rectangle here. */
  boxsampleclip(displacement_texture, tex_size, rf, texres, tex_is_byte, tex_is_float, tex_channels);
  
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

/** \} */

/**
 * Returns GLSL code for common texture processing functions.
 * Call this to get the full texture library.
 */
static std::string get_common_texture_lib_glsl()
{
  return get_colorband_helpers_glsl() + get_color_conversion_glsl() + 
         get_boxsample_helpers_glsl() + get_boxsample_core_glsl() +
         get_texture_mapping_glsl();
}

}  // namespace blender::gpu
