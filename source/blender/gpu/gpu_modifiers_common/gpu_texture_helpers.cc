/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_texture_helpers.hh"

#include "IMB_imbuf.hh"
#include "GPU_texture.hh"
#include "MEM_guardedalloc.h"
#include "BKE_colorband.hh"

namespace blender {
namespace gpu {

void displace_upload_ibuf_to_texture(gpu::Texture *tex, ImBuf *ibuf, const char * /*colorspace_name*/)
{
  if (!tex || !ibuf) {
    return;
  }

  const int width = ibuf->x;
  const int height = ibuf->y;

  float *upload_data = (float *)MEM_malloc_arrayN<float>(width * height * 4, "displace_tex_upload");
  if (ibuf->float_buffer.data) {
    for (int i = 0; i < width * height; i++) {
      if (ibuf->channels == 4) {
        upload_data[4 * i + 0] = ibuf->float_buffer.data[4 * i + 0];
        upload_data[4 * i + 1] = ibuf->float_buffer.data[4 * i + 1];
        upload_data[4 * i + 2] = ibuf->float_buffer.data[4 * i + 2];
        upload_data[4 * i + 3] = ibuf->float_buffer.data[4 * i + 3];
      }
      else if (ibuf->channels == 3) {
        upload_data[4 * i + 0] = ibuf->float_buffer.data[3 * i + 0];
        upload_data[4 * i + 1] = ibuf->float_buffer.data[3 * i + 1];
        upload_data[4 * i + 2] = ibuf->float_buffer.data[3 * i + 2];
        upload_data[4 * i + 3] = 1.0f;
      }
      else {
        float val = ibuf->float_buffer.data[i];
        upload_data[4 * i + 0] = val;
        upload_data[4 * i + 1] = val;
        upload_data[4 * i + 2] = val;
        upload_data[4 * i + 3] = 1.0f;
      }
    }
  }
  else if (ibuf->byte_buffer.data) {
    for (int i = 0; i < width * height; i++) {
      const unsigned char *rect = &ibuf->byte_buffer.data[4 * i];
      upload_data[4 * i + 0] = float(rect[0]) * (1.0f / 255.0f);
      upload_data[4 * i + 1] = float(rect[1]) * (1.0f / 255.0f);
      upload_data[4 * i + 2] = float(rect[2]) * (1.0f / 255.0f);
      upload_data[4 * i + 3] = float(rect[3]) * (1.0f / 255.0f);
    }
  }
  else {
    MEM_freeN(upload_data);
    return;
  }

  GPU_texture_update(tex, GPU_DATA_FLOAT, upload_data);
  MEM_freeN(upload_data);
}

/* Helper: fill a GPUTextureParams struct from a Tex + modifier info. */
void fill_texture_params_from_tex(GPUTextureParams &gpu_tex_params,
                                  Tex *tex,
                                  const ModifierData *md,
                                  Object *deformed_eval,
                                  int scene_frame,
                                  bool tex_is_byte,
                                  bool tex_is_float,
                                  int tex_channels,
                                  bool has_tex_coords)
{
  memset(&gpu_tex_params, 0, sizeof(gpu_tex_params));

  if (!tex) {
    return;
  }

  gpu_tex_params.tex_crop[0] = tex->cropxmin;
  gpu_tex_params.tex_crop[1] = tex->cropymin;
  gpu_tex_params.tex_crop[2] = tex->cropxmax;
  gpu_tex_params.tex_crop[3] = tex->cropymax;

  /* repeat.x, repeat.y, xmir, ymir */
  gpu_tex_params.tex_repeat_and_mirror[0] = int32_t(tex->xrepeat);
  gpu_tex_params.tex_repeat_and_mirror[1] = int32_t(tex->yrepeat);
  gpu_tex_params.tex_repeat_and_mirror[2] = int32_t((tex->flag & TEX_REPEAT_XMIR) ? 1 : 0);
  gpu_tex_params.tex_repeat_and_mirror[3] = int32_t((tex->flag & TEX_REPEAT_YMIR) ? 1 : 0);

  /* format properties: is_byte, is_float, channels, type */
  gpu_tex_params.tex_format_properties[0] = tex_is_byte ? 1 : 0;
  gpu_tex_params.tex_format_properties[1] = tex_is_float ? 1 : 0;
  gpu_tex_params.tex_format_properties[2] = tex_channels;
  gpu_tex_params.tex_format_properties[3] = tex->type;

  gpu_tex_params.tex_bricont[0] = tex->bright;
  gpu_tex_params.tex_bricont[1] = tex->contrast;
  gpu_tex_params.tex_bricont[2] = tex->saturation;

  gpu_tex_params.tex_rgbfac[0] = tex->rfac;
  gpu_tex_params.tex_rgbfac[1] = tex->gfac;
  gpu_tex_params.tex_rgbfac[2] = tex->bfac;

  int tex_mapping = MOD_DISP_MAP_LOCAL;
  if (md && md->type == eModifierType_Displace) {
    const DisplaceModifierData *dmd = reinterpret_cast<const DisplaceModifierData *>(md);
    tex_mapping = int(dmd->texmapping);
    if (tex_mapping == MOD_DISP_MAP_OBJECT && dmd->map_object == nullptr) {
      tex_mapping = MOD_DISP_MAP_LOCAL;
    }
  }

  bool mapping_use_input_positions = !has_tex_coords;
  gpu_tex_params.tex_mapping_info[0] = tex_mapping;
  gpu_tex_params.tex_mapping_info[1] = mapping_use_input_positions ? 1 : 0;

  int mtex_mapto = 0;
  gpu_tex_params.tex_mapping_info[2] = mtex_mapto;
  gpu_tex_params.tex_mapping_info[3] = tex->stype;

  gpu_tex_params.tex_flags[0] = tex->flag;
  gpu_tex_params.tex_flags[1] = tex->extend;
  gpu_tex_params.tex_flags[2] = int(tex->checkerdist * 1000.0f);

  /* Expose TEX_FLIPBLEND as a dedicated field for shaders to avoid bit-twiddling in GLSL.
   * Stored in tex_flipblend[0] to match std140 layout (mapped to ivec4 in GLSL). */
  gpu_tex_params.tex_flipblend[0] = (tex->flag & TEX_FLIPBLEND) ? 1 : 0;
  gpu_tex_params.tex_flipblend[1] = 0;
  gpu_tex_params.tex_flipblend[2] = 0;
  gpu_tex_params.tex_flipblend[3] = 0;

  gpu_tex_params.tex_imaflag_runtime_flags[0] = tex->imaflag;
  {
    Image *ima_local = tex->ima;
    bool use_talpha_local = false;
    if ((tex->imaflag & TEX_USEALPHA) && ima_local && (ima_local->alpha_mode != IMA_ALPHA_IGNORE)) {
      if ((tex->imaflag & TEX_CALCALPHA) == 0) {
        use_talpha_local = true;
      }
    }
    gpu_tex_params.tex_imaflag_runtime_flags[1] = use_talpha_local ? 1 : 0;
    gpu_tex_params.tex_imaflag_runtime_flags[2] = ((tex->imaflag & TEX_CALCALPHA) != 0) ? 1 : 0;
    gpu_tex_params.tex_imaflag_runtime_flags[3] = ((tex->flag & TEX_NEGALPHA) != 0) ? 1 : 0;
  }

  gpu_tex_params.tex_noise[0] = tex->noisebasis;
  gpu_tex_params.tex_noise[1] = tex->noisebasis2;
  gpu_tex_params.tex_noise[2] = tex->noisedepth;
  gpu_tex_params.tex_noise[3] = tex->noisetype;

  gpu_tex_params.tex_noisesize_turbul[0] = tex->noisesize;
  gpu_tex_params.tex_noisesize_turbul[1] = tex->turbul;

  gpu_tex_params.tex_filtersize_frame_colorband_pad[0] = int(tex->filtersize * 1000.0f);
  gpu_tex_params.tex_filtersize_frame_colorband_pad[1] = scene_frame;
  gpu_tex_params.tex_filtersize_frame_colorband_pad[2] = ((tex->flag & TEX_COLORBAND) != 0) ? 1 : 0;

  gpu_tex_params.tex_distamount[0] = tex->dist_amount;
  gpu_tex_params.tex_distamount[1] = tex->ns_outscale;

  gpu_tex_params.tex_mg_params[0] = tex->mg_H;
  gpu_tex_params.tex_mg_params[1] = tex->mg_lacunarity;
  gpu_tex_params.tex_mg_params[2] = tex->mg_octaves;
  gpu_tex_params.tex_mg_params[3] = tex->mg_offset;
  gpu_tex_params.tex_mg_params[4] = tex->mg_gain;

  gpu_tex_params.tex_voronoi[0] = tex->vn_w1;
  gpu_tex_params.tex_voronoi[1] = tex->vn_w2;
  gpu_tex_params.tex_voronoi[2] = tex->vn_w3;
  gpu_tex_params.tex_voronoi[3] = tex->vn_w4;
  gpu_tex_params.tex_voronoi_misc[0] = tex->vn_mexp;
  gpu_tex_params.tex_voronoi_misc[1] = float(tex->vn_distm);
  gpu_tex_params.tex_voronoi_misc[2] = float(tex->vn_coltype);

  if (deformed_eval) {
    memcpy(gpu_tex_params.u_object_to_world_mat,
           deformed_eval->object_to_world().ptr(),
           sizeof(gpu_tex_params.u_object_to_world_mat));
  }

  float mapref_imat[4][4];
  unit_m4(mapref_imat);

  if (md) {
    switch (md->type) {
      case eModifierType_Displace: {
        const DisplaceModifierData *dmd = reinterpret_cast<const DisplaceModifierData *>(md);
        if (dmd->texmapping == MOD_DISP_MAP_OBJECT && dmd->map_object != nullptr) {
          Object *map_object = dmd->map_object;
          if (dmd->map_bone[0] != '\0') {
            bPoseChannel *pchan = BKE_pose_channel_find_name(map_object->pose, dmd->map_bone);
            if (pchan) {
              float mat_bone_world[4][4];
              mul_m4_m4m4(mat_bone_world, map_object->object_to_world().ptr(), pchan->pose_mat);
              invert_m4_m4(mapref_imat, mat_bone_world);
            }
            else {
              invert_m4_m4(mapref_imat, map_object->object_to_world().ptr());
            }
          }
          else {
            invert_m4_m4(mapref_imat, map_object->object_to_world().ptr());
          }
        }
        break;
      }
      case eModifierType_Wave: {
        const WaveModifierData *wmd = reinterpret_cast<const WaveModifierData *>(md);
        if (wmd->texmapping == MOD_DISP_MAP_OBJECT && wmd->map_object != nullptr) {
          Object *map_object = wmd->map_object;
          if (wmd->map_bone[0] != '\0') {
            bPoseChannel *pchan = BKE_pose_channel_find_name(map_object->pose, wmd->map_bone);
            if (pchan) {
              float mat_bone_world[4][4];
              mul_m4_m4m4(mat_bone_world, map_object->object_to_world().ptr(), pchan->pose_mat);
              invert_m4_m4(mapref_imat, mat_bone_world);
            }
            else {
              invert_m4_m4(mapref_imat, map_object->object_to_world().ptr());
            }
          }
          else {
            invert_m4_m4(mapref_imat, map_object->object_to_world().ptr());
          }
        }
        break;
      }
      default:
        break;
    }
  }

  memcpy(gpu_tex_params.u_mapref_imat, mapref_imat, sizeof(gpu_tex_params.u_mapref_imat));
}

/* Fill a GPUColorBand from a CPU ColorBand. Returns false if `src` is null or empty. */
bool fill_gpu_colorband_from_colorband(GPUColorBand &dst, const ColorBand *src)
{
  if (src == nullptr || src->tot <= 0) {
    return false;
  }

  memset(&dst, 0, sizeof(dst));

  dst.tot_cur_ipotype_hue[0] = src->tot;
  dst.tot_cur_ipotype_hue[1] = src->cur;
  dst.tot_cur_ipotype_hue[2] = src->ipotype;
  dst.tot_cur_ipotype_hue[3] = src->ipotype_hue;

  dst.color_mode_pad[0] = src->color_mode;

  const int max_elems = 32;
  int copy_n = (src->tot < max_elems) ? src->tot : max_elems;
  for (int i = 0; i < copy_n; ++i) {
    const CBData &s = src->data[i];
    dst.data[i].rgba[0] = s.r;
    dst.data[i].rgba[1] = s.g;
    dst.data[i].rgba[2] = s.b;
    dst.data[i].rgba[3] = s.a;
    dst.data[i].pos_cur_pad[0] = s.pos;
    dst.data[i].pos_cur_pad[1] = float(s.cur);
    dst.data[i].pos_cur_pad[2] = 0.0f;
    dst.data[i].pos_cur_pad[3] = 0.0f;
  }

  for (int i = copy_n; i < max_elems; ++i) {
    dst.data[i].rgba[0] = dst.data[i].rgba[1] = dst.data[i].rgba[2] = dst.data[i].rgba[3] = 0.0f;
    dst.data[i].pos_cur_pad[0] = dst.data[i].pos_cur_pad[1] = dst.data[i].pos_cur_pad[2] =
        dst.data[i].pos_cur_pad[3] = 0.0f;
  }

  return true;
}

} // namespace gpu
} // namespace blender
