/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <cstring>

#include "DNA_defaults.h"
#include "DNA_scene_types.h"

#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "IMB_colormanagement.h"
#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "BKE_colortools.h"
#include "BKE_image_format.h"

/* Init/Copy/Free */

void BKE_image_format_init(ImageFormatData *imf, const bool render)
{
  *imf = *DNA_struct_default_get(ImageFormatData);

  BKE_color_managed_display_settings_init(&imf->display_settings);

  if (render) {
    BKE_color_managed_view_settings_init_render(
        &imf->view_settings, &imf->display_settings, "Filmic");
  }
  else {
    BKE_color_managed_view_settings_init_default(&imf->view_settings, &imf->display_settings);
  }

  BKE_color_managed_colorspace_settings_init(&imf->linear_colorspace_settings);
}

void BKE_image_format_copy(ImageFormatData *imf_dst, const ImageFormatData *imf_src)
{
  memcpy(imf_dst, imf_src, sizeof(*imf_dst));
  BKE_color_managed_display_settings_copy(&imf_dst->display_settings, &imf_src->display_settings);
  BKE_color_managed_view_settings_copy(&imf_dst->view_settings, &imf_src->view_settings);
  BKE_color_managed_colorspace_settings_copy(&imf_dst->linear_colorspace_settings,
                                             &imf_src->linear_colorspace_settings);
}

void BKE_image_format_free(ImageFormatData *imf)
{
  BKE_color_managed_view_settings_free(&imf->view_settings);
}

void BKE_image_format_blend_read_data(BlendDataReader *reader, ImageFormatData *imf)
{
  BKE_color_managed_view_settings_blend_read_data(reader, &imf->view_settings);
}

void BKE_image_format_blend_write(BlendWriter *writer, ImageFormatData *imf)
{
  BKE_color_managed_view_settings_blend_write(writer, &imf->view_settings);
}

/* File Types */

int BKE_imtype_to_ftype(const char imtype, ImbFormatOptions *r_options)
{
  memset(r_options, 0, sizeof(*r_options));

  if (imtype == R_IMF_IMTYPE_TARGA) {
    return IMB_FTYPE_TGA;
  }
  if (imtype == R_IMF_IMTYPE_RAWTGA) {
    r_options->flag = RAWTGA;
    return IMB_FTYPE_TGA;
  }
  if (imtype == R_IMF_IMTYPE_IRIS) {
    return IMB_FTYPE_IMAGIC;
  }
#ifdef WITH_HDR
  if (imtype == R_IMF_IMTYPE_RADHDR) {
    return IMB_FTYPE_RADHDR;
  }
#endif
  if (imtype == R_IMF_IMTYPE_PNG) {
    r_options->quality = 15;
    return IMB_FTYPE_PNG;
  }
#ifdef WITH_DDS
  if (imtype == R_IMF_IMTYPE_DDS) {
    return IMB_FTYPE_DDS;
  }
#endif
  if (imtype == R_IMF_IMTYPE_BMP) {
    return IMB_FTYPE_BMP;
  }
#ifdef WITH_TIFF
  if (imtype == R_IMF_IMTYPE_TIFF) {
    return IMB_FTYPE_TIF;
  }
#endif
  if (ELEM(imtype, R_IMF_IMTYPE_OPENEXR, R_IMF_IMTYPE_MULTILAYER)) {
    return IMB_FTYPE_OPENEXR;
  }
#ifdef WITH_CINEON
  if (imtype == R_IMF_IMTYPE_CINEON) {
    return IMB_FTYPE_CINEON;
  }
  if (imtype == R_IMF_IMTYPE_DPX) {
    return IMB_FTYPE_DPX;
  }
#endif
#ifdef WITH_OPENJPEG
  if (imtype == R_IMF_IMTYPE_JP2) {
    r_options->flag |= JP2_JP2;
    r_options->quality = 90;
    return IMB_FTYPE_JP2;
  }
#endif
#ifdef WITH_WEBP
  if (imtype == R_IMF_IMTYPE_WEBP) {
    r_options->quality = 90;
    return IMB_FTYPE_WEBP;
  }
#endif

  r_options->quality = 90;
  return IMB_FTYPE_JPG;
}

char BKE_ftype_to_imtype(const int ftype, const ImbFormatOptions *options)
{
  if (ftype == IMB_FTYPE_NONE) {
    return R_IMF_IMTYPE_TARGA;
  }
  if (ftype == IMB_FTYPE_IMAGIC) {
    return R_IMF_IMTYPE_IRIS;
  }
#ifdef WITH_HDR
  if (ftype == IMB_FTYPE_RADHDR) {
    return R_IMF_IMTYPE_RADHDR;
  }
#endif
  if (ftype == IMB_FTYPE_PNG) {
    return R_IMF_IMTYPE_PNG;
  }
#ifdef WITH_DDS
  if (ftype == IMB_FTYPE_DDS) {
    return R_IMF_IMTYPE_DDS;
  }
#endif
  if (ftype == IMB_FTYPE_BMP) {
    return R_IMF_IMTYPE_BMP;
  }
#ifdef WITH_TIFF
  if (ftype == IMB_FTYPE_TIF) {
    return R_IMF_IMTYPE_TIFF;
  }
#endif
  if (ftype == IMB_FTYPE_OPENEXR) {
    return R_IMF_IMTYPE_OPENEXR;
  }
#ifdef WITH_CINEON
  if (ftype == IMB_FTYPE_CINEON) {
    return R_IMF_IMTYPE_CINEON;
  }
  if (ftype == IMB_FTYPE_DPX) {
    return R_IMF_IMTYPE_DPX;
  }
#endif
  if (ftype == IMB_FTYPE_TGA) {
    if (options && (options->flag & RAWTGA)) {
      return R_IMF_IMTYPE_RAWTGA;
    }

    return R_IMF_IMTYPE_TARGA;
  }
#ifdef WITH_OPENJPEG
  if (ftype == IMB_FTYPE_JP2) {
    return R_IMF_IMTYPE_JP2;
  }
#endif
#ifdef WITH_WEBP
  if (ftype == IMB_FTYPE_WEBP) {
    return R_IMF_IMTYPE_WEBP;
  }
#endif

  return R_IMF_IMTYPE_JPEG90;
}

bool BKE_imtype_is_movie(const char imtype)
{
  switch (imtype) {
    case R_IMF_IMTYPE_AVIRAW:
    case R_IMF_IMTYPE_AVIJPEG:
    case R_IMF_IMTYPE_FFMPEG:
    case R_IMF_IMTYPE_H264:
    case R_IMF_IMTYPE_THEORA:
    case R_IMF_IMTYPE_XVID:
      return true;
  }
  return false;
}

bool BKE_imtype_supports_zbuf(const char imtype)
{
  switch (imtype) {
    case R_IMF_IMTYPE_IRIZ:
    case R_IMF_IMTYPE_OPENEXR: /* but not R_IMF_IMTYPE_MULTILAYER */
      return true;
  }
  return false;
}

bool BKE_imtype_supports_compress(const char imtype)
{
  switch (imtype) {
    case R_IMF_IMTYPE_PNG:
      return true;
  }
  return false;
}

bool BKE_imtype_supports_quality(const char imtype)
{
  switch (imtype) {
    case R_IMF_IMTYPE_JPEG90:
    case R_IMF_IMTYPE_JP2:
    case R_IMF_IMTYPE_AVIJPEG:
    case R_IMF_IMTYPE_WEBP:
      return true;
  }
  return false;
}

bool BKE_imtype_requires_linear_float(const char imtype)
{
  switch (imtype) {
    case R_IMF_IMTYPE_CINEON:
    case R_IMF_IMTYPE_DPX:
    case R_IMF_IMTYPE_RADHDR:
    case R_IMF_IMTYPE_OPENEXR:
    case R_IMF_IMTYPE_MULTILAYER:
      return true;
  }
  return false;
}

char BKE_imtype_valid_channels(const char imtype, bool write_file)
{
  char chan_flag = IMA_CHAN_FLAG_RGB; /* Assume all support RGB. */

  /* Alpha. */
  switch (imtype) {
    case R_IMF_IMTYPE_BMP:
      if (write_file) {
        break;
      }
      ATTR_FALLTHROUGH;
    case R_IMF_IMTYPE_TARGA:
    case R_IMF_IMTYPE_RAWTGA:
    case R_IMF_IMTYPE_IRIS:
    case R_IMF_IMTYPE_PNG:
    case R_IMF_IMTYPE_TIFF:
    case R_IMF_IMTYPE_OPENEXR:
    case R_IMF_IMTYPE_MULTILAYER:
    case R_IMF_IMTYPE_DDS:
    case R_IMF_IMTYPE_JP2:
    case R_IMF_IMTYPE_DPX:
    case R_IMF_IMTYPE_WEBP:
      chan_flag |= IMA_CHAN_FLAG_ALPHA;
      break;
  }

  /* BW. */
  switch (imtype) {
    case R_IMF_IMTYPE_BMP:
    case R_IMF_IMTYPE_PNG:
    case R_IMF_IMTYPE_JPEG90:
    case R_IMF_IMTYPE_TARGA:
    case R_IMF_IMTYPE_RAWTGA:
    case R_IMF_IMTYPE_TIFF:
    case R_IMF_IMTYPE_IRIS:
      chan_flag |= IMA_CHAN_FLAG_BW;
      break;
  }

  return chan_flag;
}

char BKE_imtype_valid_depths(const char imtype)
{
  switch (imtype) {
    case R_IMF_IMTYPE_RADHDR:
      return R_IMF_CHAN_DEPTH_32;
    case R_IMF_IMTYPE_TIFF:
      return R_IMF_CHAN_DEPTH_8 | R_IMF_CHAN_DEPTH_16;
    case R_IMF_IMTYPE_OPENEXR:
      return R_IMF_CHAN_DEPTH_16 | R_IMF_CHAN_DEPTH_32;
    case R_IMF_IMTYPE_MULTILAYER:
      return R_IMF_CHAN_DEPTH_16 | R_IMF_CHAN_DEPTH_32;
    /* NOTE: CINEON uses an unusual 10bits-LOG per channel. */
    case R_IMF_IMTYPE_DPX:
      return R_IMF_CHAN_DEPTH_8 | R_IMF_CHAN_DEPTH_10 | R_IMF_CHAN_DEPTH_12 | R_IMF_CHAN_DEPTH_16;
    case R_IMF_IMTYPE_CINEON:
      return R_IMF_CHAN_DEPTH_10;
    case R_IMF_IMTYPE_JP2:
      return R_IMF_CHAN_DEPTH_8 | R_IMF_CHAN_DEPTH_12 | R_IMF_CHAN_DEPTH_16;
    case R_IMF_IMTYPE_PNG:
      return R_IMF_CHAN_DEPTH_8 | R_IMF_CHAN_DEPTH_16;
    /* Most formats are 8bit only. */
    default:
      return R_IMF_CHAN_DEPTH_8;
  }
}

char BKE_imtype_from_arg(const char *imtype_arg)
{
  if (STREQ(imtype_arg, "TGA")) {
    return R_IMF_IMTYPE_TARGA;
  }
  if (STREQ(imtype_arg, "IRIS")) {
    return R_IMF_IMTYPE_IRIS;
  }
#ifdef WITH_DDS
  if (STREQ(imtype_arg, "DDS")) {
    return R_IMF_IMTYPE_DDS;
  }
#endif
  if (STREQ(imtype_arg, "JPEG")) {
    return R_IMF_IMTYPE_JPEG90;
  }
  if (STREQ(imtype_arg, "IRIZ")) {
    return R_IMF_IMTYPE_IRIZ;
  }
  if (STREQ(imtype_arg, "RAWTGA")) {
    return R_IMF_IMTYPE_RAWTGA;
  }
  if (STREQ(imtype_arg, "AVIRAW")) {
    return R_IMF_IMTYPE_AVIRAW;
  }
  if (STREQ(imtype_arg, "AVIJPEG")) {
    return R_IMF_IMTYPE_AVIJPEG;
  }
  if (STREQ(imtype_arg, "PNG")) {
    return R_IMF_IMTYPE_PNG;
  }
  if (STREQ(imtype_arg, "BMP")) {
    return R_IMF_IMTYPE_BMP;
  }
#ifdef WITH_HDR
  if (STREQ(imtype_arg, "HDR")) {
    return R_IMF_IMTYPE_RADHDR;
  }
#endif
#ifdef WITH_TIFF
  if (STREQ(imtype_arg, "TIFF")) {
    return R_IMF_IMTYPE_TIFF;
  }
#endif
#ifdef WITH_OPENEXR
  if (STREQ(imtype_arg, "OPEN_EXR")) {
    return R_IMF_IMTYPE_OPENEXR;
  }
  if (STREQ(imtype_arg, "OPEN_EXR_MULTILAYER")) {
    return R_IMF_IMTYPE_MULTILAYER;
  }
  if (STREQ(imtype_arg, "EXR")) {
    return R_IMF_IMTYPE_OPENEXR;
  }
  if (STREQ(imtype_arg, "MULTILAYER")) {
    return R_IMF_IMTYPE_MULTILAYER;
  }
#endif
  if (STREQ(imtype_arg, "FFMPEG")) {
    return R_IMF_IMTYPE_FFMPEG;
  }
#ifdef WITH_CINEON
  if (STREQ(imtype_arg, "CINEON")) {
    return R_IMF_IMTYPE_CINEON;
  }
  if (STREQ(imtype_arg, "DPX")) {
    return R_IMF_IMTYPE_DPX;
  }
#endif
#ifdef WITH_OPENJPEG
  if (STREQ(imtype_arg, "JP2")) {
    return R_IMF_IMTYPE_JP2;
  }
#endif
#ifdef WITH_WEBP
  if (STREQ(imtype_arg, "WEBP")) {
    return R_IMF_IMTYPE_WEBP;
  }
#endif

  return R_IMF_IMTYPE_INVALID;
}

/* File Paths */

static bool do_add_image_extension(char *string,
                                   const char imtype,
                                   const ImageFormatData *im_format)
{
  const char *extension = nullptr;
  const char *extension_test;
  (void)im_format; /* may be unused, depends on build options */

  if (imtype == R_IMF_IMTYPE_IRIS) {
    if (!BLI_path_extension_check(string, extension_test = ".rgb")) {
      extension = extension_test;
    }
  }
  else if (imtype == R_IMF_IMTYPE_IRIZ) {
    if (!BLI_path_extension_check(string, extension_test = ".rgb")) {
      extension = extension_test;
    }
  }
#ifdef WITH_HDR
  else if (imtype == R_IMF_IMTYPE_RADHDR) {
    if (!BLI_path_extension_check(string, extension_test = ".hdr")) {
      extension = extension_test;
    }
  }
#endif
  else if (ELEM(imtype,
                R_IMF_IMTYPE_PNG,
                R_IMF_IMTYPE_FFMPEG,
                R_IMF_IMTYPE_H264,
                R_IMF_IMTYPE_THEORA,
                R_IMF_IMTYPE_XVID)) {
    if (!BLI_path_extension_check(string, extension_test = ".png")) {
      extension = extension_test;
    }
  }
#ifdef WITH_DDS
  else if (imtype == R_IMF_IMTYPE_DDS) {
    if (!BLI_path_extension_check(string, extension_test = ".dds")) {
      extension = extension_test;
    }
  }
#endif
  else if (ELEM(imtype, R_IMF_IMTYPE_TARGA, R_IMF_IMTYPE_RAWTGA)) {
    if (!BLI_path_extension_check(string, extension_test = ".tga")) {
      extension = extension_test;
    }
  }
  else if (imtype == R_IMF_IMTYPE_BMP) {
    if (!BLI_path_extension_check(string, extension_test = ".bmp")) {
      extension = extension_test;
    }
  }
#ifdef WITH_TIFF
  else if (imtype == R_IMF_IMTYPE_TIFF) {
    if (!BLI_path_extension_check_n(string, extension_test = ".tif", ".tiff", nullptr)) {
      extension = extension_test;
    }
  }
#endif
#ifdef WITH_OPENIMAGEIO
  else if (imtype == R_IMF_IMTYPE_PSD) {
    if (!BLI_path_extension_check(string, extension_test = ".psd")) {
      extension = extension_test;
    }
  }
#endif
#ifdef WITH_OPENEXR
  else if (ELEM(imtype, R_IMF_IMTYPE_OPENEXR, R_IMF_IMTYPE_MULTILAYER)) {
    if (!BLI_path_extension_check(string, extension_test = ".exr")) {
      extension = extension_test;
    }
  }
#endif
#ifdef WITH_CINEON
  else if (imtype == R_IMF_IMTYPE_CINEON) {
    if (!BLI_path_extension_check(string, extension_test = ".cin")) {
      extension = extension_test;
    }
  }
  else if (imtype == R_IMF_IMTYPE_DPX) {
    if (!BLI_path_extension_check(string, extension_test = ".dpx")) {
      extension = extension_test;
    }
  }
#endif
#ifdef WITH_OPENJPEG
  else if (imtype == R_IMF_IMTYPE_JP2) {
    if (im_format) {
      if (im_format->jp2_codec == R_IMF_JP2_CODEC_JP2) {
        if (!BLI_path_extension_check(string, extension_test = ".jp2")) {
          extension = extension_test;
        }
      }
      else if (im_format->jp2_codec == R_IMF_JP2_CODEC_J2K) {
        if (!BLI_path_extension_check(string, extension_test = ".j2c")) {
          extension = extension_test;
        }
      }
      else {
        BLI_assert_msg(0, "Unsupported jp2 codec was specified in im_format->jp2_codec");
      }
    }
    else {
      if (!BLI_path_extension_check(string, extension_test = ".jp2")) {
        extension = extension_test;
      }
    }
  }
#endif
#ifdef WITH_WEBP
  else if (imtype == R_IMF_IMTYPE_WEBP) {
    if (!BLI_path_extension_check(string, extension_test = ".webp")) {
      extension = extension_test;
    }
  }
#endif
  else {  //   R_IMF_IMTYPE_AVIRAW, R_IMF_IMTYPE_AVIJPEG, R_IMF_IMTYPE_JPEG90 etc
    if (!(BLI_path_extension_check_n(string, extension_test = ".jpg", ".jpeg", nullptr))) {
      extension = extension_test;
    }
  }

  if (extension) {
    /* prefer this in many cases to avoid .png.tga, but in certain cases it breaks */
    /* remove any other known image extension */
    if (BLI_path_extension_check_array(string, imb_ext_image)) {
      return BLI_path_extension_replace(string, FILE_MAX, extension);
    }

    return BLI_path_extension_ensure(string, FILE_MAX, extension);
  }

  return false;
}

int BKE_image_path_ensure_ext_from_imformat(char *string, const ImageFormatData *im_format)
{
  return do_add_image_extension(string, im_format->imtype, im_format);
}

int BKE_image_path_ensure_ext_from_imtype(char *string, const char imtype)
{
  return do_add_image_extension(string, imtype, nullptr);
}

static void do_makepicstring(char *string,
                             const char *base,
                             const char *relbase,
                             int frame,
                             const char imtype,
                             const ImageFormatData *im_format,
                             const bool use_ext,
                             const bool use_frames,
                             const char *suffix)
{
  if (string == nullptr) {
    return;
  }
  BLI_strncpy(string, base, FILE_MAX - 10); /* weak assumption */
  BLI_path_abs(string, relbase);

  if (use_frames) {
    BLI_path_frame(string, frame, 4);
  }

  if (suffix) {
    BLI_path_suffix(string, FILE_MAX, suffix, "");
  }

  if (use_ext) {
    do_add_image_extension(string, imtype, im_format);
  }
}

void BKE_image_path_from_imformat(char *string,
                                  const char *base,
                                  const char *relbase,
                                  int frame,
                                  const ImageFormatData *im_format,
                                  const bool use_ext,
                                  const bool use_frames,
                                  const char *suffix)
{
  do_makepicstring(
      string, base, relbase, frame, im_format->imtype, im_format, use_ext, use_frames, suffix);
}

void BKE_image_path_from_imtype(char *string,
                                const char *base,
                                const char *relbase,
                                int frame,
                                const char imtype,
                                const bool use_ext,
                                const bool use_frames,
                                const char *suffix)
{
  do_makepicstring(string, base, relbase, frame, imtype, nullptr, use_ext, use_frames, suffix);
}

/* ImBuf Conversion */

void BKE_image_format_to_imbuf(ImBuf *ibuf, const ImageFormatData *imf)
{
  /* Write to ImBuf in preparation for file writing. */
  char imtype = imf->imtype;
  char compress = imf->compress;
  char quality = imf->quality;

  /* initialize all from image format */
  ibuf->foptions.flag = 0;

  if (imtype == R_IMF_IMTYPE_IRIS) {
    ibuf->ftype = IMB_FTYPE_IMAGIC;
  }
#ifdef WITH_HDR
  else if (imtype == R_IMF_IMTYPE_RADHDR) {
    ibuf->ftype = IMB_FTYPE_RADHDR;
  }
#endif
  else if (ELEM(imtype,
                R_IMF_IMTYPE_PNG,
                R_IMF_IMTYPE_FFMPEG,
                R_IMF_IMTYPE_H264,
                R_IMF_IMTYPE_THEORA,
                R_IMF_IMTYPE_XVID)) {
    ibuf->ftype = IMB_FTYPE_PNG;

    if (imtype == R_IMF_IMTYPE_PNG) {
      if (imf->depth == R_IMF_CHAN_DEPTH_16) {
        ibuf->foptions.flag |= PNG_16BIT;
      }

      ibuf->foptions.quality = compress;
    }
  }
#ifdef WITH_DDS
  else if (imtype == R_IMF_IMTYPE_DDS) {
    ibuf->ftype = IMB_FTYPE_DDS;
  }
#endif
  else if (imtype == R_IMF_IMTYPE_BMP) {
    ibuf->ftype = IMB_FTYPE_BMP;
  }
#ifdef WITH_TIFF
  else if (imtype == R_IMF_IMTYPE_TIFF) {
    ibuf->ftype = IMB_FTYPE_TIF;

    if (imf->depth == R_IMF_CHAN_DEPTH_16) {
      ibuf->foptions.flag |= TIF_16BIT;
    }
    if (imf->tiff_codec == R_IMF_TIFF_CODEC_NONE) {
      ibuf->foptions.flag |= TIF_COMPRESS_NONE;
    }
    else if (imf->tiff_codec == R_IMF_TIFF_CODEC_DEFLATE) {
      ibuf->foptions.flag |= TIF_COMPRESS_DEFLATE;
    }
    else if (imf->tiff_codec == R_IMF_TIFF_CODEC_LZW) {
      ibuf->foptions.flag |= TIF_COMPRESS_LZW;
    }
    else if (imf->tiff_codec == R_IMF_TIFF_CODEC_PACKBITS) {
      ibuf->foptions.flag |= TIF_COMPRESS_PACKBITS;
    }
  }
#endif
#ifdef WITH_OPENEXR
  else if (ELEM(imtype, R_IMF_IMTYPE_OPENEXR, R_IMF_IMTYPE_MULTILAYER)) {
    ibuf->ftype = IMB_FTYPE_OPENEXR;
    if (imf->depth == R_IMF_CHAN_DEPTH_16) {
      ibuf->foptions.flag |= OPENEXR_HALF;
    }
    ibuf->foptions.flag |= (imf->exr_codec & OPENEXR_COMPRESS);

    if (!(imf->flag & R_IMF_FLAG_ZBUF)) {
      /* Signal for exr saving. */
      IMB_freezbuffloatImBuf(ibuf);
    }
  }
#endif
#ifdef WITH_CINEON
  else if (imtype == R_IMF_IMTYPE_CINEON) {
    ibuf->ftype = IMB_FTYPE_CINEON;
    if (imf->cineon_flag & R_IMF_CINEON_FLAG_LOG) {
      ibuf->foptions.flag |= CINEON_LOG;
    }
    if (imf->depth == R_IMF_CHAN_DEPTH_16) {
      ibuf->foptions.flag |= CINEON_16BIT;
    }
    else if (imf->depth == R_IMF_CHAN_DEPTH_12) {
      ibuf->foptions.flag |= CINEON_12BIT;
    }
    else if (imf->depth == R_IMF_CHAN_DEPTH_10) {
      ibuf->foptions.flag |= CINEON_10BIT;
    }
  }
  else if (imtype == R_IMF_IMTYPE_DPX) {
    ibuf->ftype = IMB_FTYPE_DPX;
    if (imf->cineon_flag & R_IMF_CINEON_FLAG_LOG) {
      ibuf->foptions.flag |= CINEON_LOG;
    }
    if (imf->depth == R_IMF_CHAN_DEPTH_16) {
      ibuf->foptions.flag |= CINEON_16BIT;
    }
    else if (imf->depth == R_IMF_CHAN_DEPTH_12) {
      ibuf->foptions.flag |= CINEON_12BIT;
    }
    else if (imf->depth == R_IMF_CHAN_DEPTH_10) {
      ibuf->foptions.flag |= CINEON_10BIT;
    }
  }
#endif
  else if (imtype == R_IMF_IMTYPE_TARGA) {
    ibuf->ftype = IMB_FTYPE_TGA;
  }
  else if (imtype == R_IMF_IMTYPE_RAWTGA) {
    ibuf->ftype = IMB_FTYPE_TGA;
    ibuf->foptions.flag = RAWTGA;
  }
#ifdef WITH_OPENJPEG
  else if (imtype == R_IMF_IMTYPE_JP2) {
    if (quality < 10) {
      quality = 90;
    }
    ibuf->ftype = IMB_FTYPE_JP2;
    ibuf->foptions.quality = quality;

    if (imf->depth == R_IMF_CHAN_DEPTH_16) {
      ibuf->foptions.flag |= JP2_16BIT;
    }
    else if (imf->depth == R_IMF_CHAN_DEPTH_12) {
      ibuf->foptions.flag |= JP2_12BIT;
    }

    if (imf->jp2_flag & R_IMF_JP2_FLAG_YCC) {
      ibuf->foptions.flag |= JP2_YCC;
    }

    if (imf->jp2_flag & R_IMF_JP2_FLAG_CINE_PRESET) {
      ibuf->foptions.flag |= JP2_CINE;
      if (imf->jp2_flag & R_IMF_JP2_FLAG_CINE_48) {
        ibuf->foptions.flag |= JP2_CINE_48FPS;
      }
    }

    if (imf->jp2_codec == R_IMF_JP2_CODEC_JP2) {
      ibuf->foptions.flag |= JP2_JP2;
    }
    else if (imf->jp2_codec == R_IMF_JP2_CODEC_J2K) {
      ibuf->foptions.flag |= JP2_J2K;
    }
    else {
      BLI_assert_msg(0, "Unsupported jp2 codec was specified in im_format->jp2_codec");
    }
  }
#endif
#ifdef WITH_WEBP
  else if (imtype == R_IMF_IMTYPE_WEBP) {
    ibuf->ftype = IMB_FTYPE_WEBP;
    ibuf->foptions.quality = quality;
  }
#endif
  else {
    /* #R_IMF_IMTYPE_JPEG90, etc. default to JPEG. */
    if (quality < 10) {
      quality = 90;
    }
    ibuf->ftype = IMB_FTYPE_JPG;
    ibuf->foptions.quality = quality;
  }
}

void BKE_image_format_from_imbuf(ImageFormatData *im_format, const ImBuf *imbuf)
{
  /* Read from ImBuf after file read. */
  int ftype = imbuf->ftype;
  int custom_flags = imbuf->foptions.flag;
  char quality = imbuf->foptions.quality;

  BKE_image_format_init(im_format, false);

  /* file type */
  if (ftype == IMB_FTYPE_IMAGIC) {
    im_format->imtype = R_IMF_IMTYPE_IRIS;
  }
#ifdef WITH_HDR
  else if (ftype == IMB_FTYPE_RADHDR) {
    im_format->imtype = R_IMF_IMTYPE_RADHDR;
  }
#endif
  else if (ftype == IMB_FTYPE_PNG) {
    im_format->imtype = R_IMF_IMTYPE_PNG;

    if (custom_flags & PNG_16BIT) {
      im_format->depth = R_IMF_CHAN_DEPTH_16;
    }

    im_format->compress = quality;
  }

#ifdef WITH_DDS
  else if (ftype == IMB_FTYPE_DDS) {
    im_format->imtype = R_IMF_IMTYPE_DDS;
  }
#endif
  else if (ftype == IMB_FTYPE_BMP) {
    im_format->imtype = R_IMF_IMTYPE_BMP;
  }
#ifdef WITH_TIFF
  else if (ftype == IMB_FTYPE_TIF) {
    im_format->imtype = R_IMF_IMTYPE_TIFF;
    if (custom_flags & TIF_16BIT) {
      im_format->depth = R_IMF_CHAN_DEPTH_16;
    }
    if (custom_flags & TIF_COMPRESS_NONE) {
      im_format->tiff_codec = R_IMF_TIFF_CODEC_NONE;
    }
    if (custom_flags & TIF_COMPRESS_DEFLATE) {
      im_format->tiff_codec = R_IMF_TIFF_CODEC_DEFLATE;
    }
    if (custom_flags & TIF_COMPRESS_LZW) {
      im_format->tiff_codec = R_IMF_TIFF_CODEC_LZW;
    }
    if (custom_flags & TIF_COMPRESS_PACKBITS) {
      im_format->tiff_codec = R_IMF_TIFF_CODEC_PACKBITS;
    }
  }
#endif

#ifdef WITH_OPENEXR
  else if (ftype == IMB_FTYPE_OPENEXR) {
    im_format->imtype = R_IMF_IMTYPE_OPENEXR;
    if (custom_flags & OPENEXR_HALF) {
      im_format->depth = R_IMF_CHAN_DEPTH_16;
    }
    if (custom_flags & OPENEXR_COMPRESS) {
      im_format->exr_codec = R_IMF_EXR_CODEC_ZIP; /* Can't determine compression */
    }
    if (imbuf->zbuf_float) {
      im_format->flag |= R_IMF_FLAG_ZBUF;
    }
  }
#endif

#ifdef WITH_CINEON
  else if (ftype == IMB_FTYPE_CINEON) {
    im_format->imtype = R_IMF_IMTYPE_CINEON;
  }
  else if (ftype == IMB_FTYPE_DPX) {
    im_format->imtype = R_IMF_IMTYPE_DPX;
  }
#endif
  else if (ftype == IMB_FTYPE_TGA) {
    if (custom_flags & RAWTGA) {
      im_format->imtype = R_IMF_IMTYPE_RAWTGA;
    }
    else {
      im_format->imtype = R_IMF_IMTYPE_TARGA;
    }
  }
#ifdef WITH_OPENJPEG
  else if (ftype == IMB_FTYPE_JP2) {
    im_format->imtype = R_IMF_IMTYPE_JP2;
    im_format->quality = quality;

    if (custom_flags & JP2_16BIT) {
      im_format->depth = R_IMF_CHAN_DEPTH_16;
    }
    else if (custom_flags & JP2_12BIT) {
      im_format->depth = R_IMF_CHAN_DEPTH_12;
    }

    if (custom_flags & JP2_YCC) {
      im_format->jp2_flag |= R_IMF_JP2_FLAG_YCC;
    }

    if (custom_flags & JP2_CINE) {
      im_format->jp2_flag |= R_IMF_JP2_FLAG_CINE_PRESET;
      if (custom_flags & JP2_CINE_48FPS) {
        im_format->jp2_flag |= R_IMF_JP2_FLAG_CINE_48;
      }
    }

    if (custom_flags & JP2_JP2) {
      im_format->jp2_codec = R_IMF_JP2_CODEC_JP2;
    }
    else if (custom_flags & JP2_J2K) {
      im_format->jp2_codec = R_IMF_JP2_CODEC_J2K;
    }
    else {
      BLI_assert_msg(0, "Unsupported jp2 codec was specified in file type");
    }
  }
#endif
#ifdef WITH_WEBP
  else if (ftype == IMB_FTYPE_WEBP) {
    im_format->imtype = R_IMF_IMTYPE_WEBP;
    im_format->quality = quality;
  }
#endif

  else {
    im_format->imtype = R_IMF_IMTYPE_JPEG90;
    im_format->quality = quality;
  }

  /* planes */
  im_format->planes = imbuf->planes;
}

bool BKE_image_format_is_byte(const ImageFormatData *imf)
{
  return (imf->depth == R_IMF_CHAN_DEPTH_8) && (BKE_imtype_valid_depths(imf->imtype) & imf->depth);
}

/* Color Management */

void BKE_image_format_color_management_copy(ImageFormatData *imf, const ImageFormatData *imf_src)
{
  BKE_color_managed_view_settings_free(&imf->view_settings);

  BKE_color_managed_display_settings_copy(&imf->display_settings, &imf_src->display_settings);
  BKE_color_managed_view_settings_copy(&imf->view_settings, &imf_src->view_settings);
  BKE_color_managed_colorspace_settings_copy(&imf->linear_colorspace_settings,
                                             &imf_src->linear_colorspace_settings);
}

void BKE_image_format_color_management_copy_from_scene(ImageFormatData *imf, const Scene *scene)
{
  BKE_color_managed_view_settings_free(&imf->view_settings);

  BKE_color_managed_display_settings_copy(&imf->display_settings, &scene->display_settings);
  BKE_color_managed_view_settings_copy(&imf->view_settings, &scene->view_settings);
  STRNCPY(imf->linear_colorspace_settings.name,
          IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR));
}

/* Output */

void BKE_image_format_init_for_write(ImageFormatData *imf,
                                     const Scene *scene_src,
                                     const ImageFormatData *imf_src)
{
  *imf = (imf_src) ? *imf_src : scene_src->r.im_format;

  if (imf_src && imf_src->color_management == R_IMF_COLOR_MANAGEMENT_OVERRIDE) {
    /* Use settings specific to one node, image save operation, etc. */
    BKE_color_managed_display_settings_copy(&imf->display_settings, &imf_src->display_settings);
    BKE_color_managed_view_settings_copy(&imf->view_settings, &imf_src->view_settings);
    BKE_color_managed_colorspace_settings_copy(&imf->linear_colorspace_settings,
                                               &imf_src->linear_colorspace_settings);
  }
  else if (scene_src->r.im_format.color_management == R_IMF_COLOR_MANAGEMENT_OVERRIDE) {
    /* Use scene settings specific to render output. */
    BKE_color_managed_display_settings_copy(&imf->display_settings,
                                            &scene_src->r.im_format.display_settings);
    BKE_color_managed_view_settings_copy(&imf->view_settings,
                                         &scene_src->r.im_format.view_settings);
    BKE_color_managed_colorspace_settings_copy(&imf->linear_colorspace_settings,
                                               &scene_src->r.im_format.linear_colorspace_settings);
  }
  else {
    /* Use general scene settings also used for display. */
    BKE_color_managed_display_settings_copy(&imf->display_settings, &scene_src->display_settings);
    BKE_color_managed_view_settings_copy(&imf->view_settings, &scene_src->view_settings);
    STRNCPY(imf->linear_colorspace_settings.name,
            IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR));
  }
}
