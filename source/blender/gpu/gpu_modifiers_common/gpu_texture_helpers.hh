/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstring>

#include "BKE_action.hh"

#include "BLI_math_matrix.h"

#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_texture_types.h"

namespace blender {
struct Image;
struct ImBuf;
struct ModifierData;
struct Object;
struct Tex;
struct ColorBand; /* forward-declare to avoid heavy includes in header */
}  // namespace blender

namespace blender {
namespace gpu {

class Texture;

/* C++ std140-compatible structs for ColorBand UBO data. Placed here so
 * CPU-side code can build and upload UBOs matching the GLSL typedefs. */
struct GPUCBData {
  float rgba[4];
  float pos_cur_pad[4];
};

struct GPUColorBand {
  int32_t tot_cur_ipotype_hue[4];
  int32_t color_mode_pad[4];
  GPUCBData data[32];
};

/* C++ std140-compatible struct for TextureParams UBO. Fields chosen to match
 * the GLSL `TextureParams` typedef. Use primitive arrays to ensure packing. */
struct GPUTextureParams {
  float tex_crop[4];
  /* repeat.x, repeat.y, xmir(0/1), ymir(0/1) */
  int32_t tex_repeat_and_mirror[4];
  /* is_byte(0/1), is_float(0/1), channels, type */
  int32_t tex_format_properties[4];
  float tex_bricont[4];
  /* mapping, mapping_use_input_positions(0/1), mtex_mapto, stype */
  int32_t tex_mapping_info[4];
  int32_t tex_flags[4];
  /* TEX_FLIPBLEND exposed as a 4-int slot so the CPU-side struct matches GLSL std140
   * layout (mapped to an `ivec4 tex_flipblend` in the UBO). Only `.x` is used. */
  int32_t tex_flipblend[4];
  int32_t tex_noise[4];
  float tex_noisesize_turbul[4];                 /* noisesize, turbul, pad, pad */
  int32_t tex_filtersize_frame_colorband_pad[4]; /* filtersize*1000 (as int), frame,
                                                    use_colorband(0/1), pad */
  float tex_rgbfac[4];
  float tex_distamount[4]; /* distamount, pad... */
  float tex_mg_params[8]; /* mg_H, mg_lacunarity, mg_octaves, mg_offset, mg_gain, ns_outscale, pad,
                             pad */
  float tex_voronoi[4];   /* vn_w1, vn_w2, vn_w3, vn_w4 */
  float tex_voronoi_misc[4];            /* vn_mexp, vn_distm, vn_coltype, pad */
  int32_t tex_imaflag_runtime_flags[4]; /* imaflag, use_talpha, calcalpha, negalpha */
  float u_object_to_world_mat[16];
  float u_mapref_imat[16];
};

/* Check if we need manual colorspace handling for this image.
 * Returns true if the image uses a non-"Non-Color" colorspace. */
bool displace_needs_manual_colorspace(Image *ima);

/* Upload ImBuf data to GPU texture WITHOUT colorspace conversion.
 * For displacement we want raw values (matching CPU behavior).
 */
void displace_upload_ibuf_to_texture(gpu::Texture *tex, ImBuf *ibuf, const char *colorspace_name = nullptr);

void fill_texture_params_from_tex(GPUTextureParams &gpu_tex_params,
                                  Tex *tex,
                                  const ModifierData *md,
                                  Object *deformed_eval,
                                  int scene_frame,
                                  bool tex_is_byte,
                                  bool tex_is_float,
                                  int tex_channels,
                                  bool has_tex_coords);

/* Fill a GPUColorBand from a CPU ColorBand. Returns false if `src` is null or empty. */
bool fill_gpu_colorband_from_colorband(GPUColorBand &dst, const ColorBand *src);

} // namespace gpu
} // namespace blender
