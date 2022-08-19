/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include "MEM_guardedalloc.h"

#include "BLI_fileops.h"
#include "BLI_math.h"

#include "IMB_filetype.h"
#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "IMB_colormanagement.h"
#include "IMB_colormanagement_intern.h"

#include "openjpeg.h"

#define JP2_FILEHEADER_SIZE 12

static const char JP2_HEAD[] = {
    0x0, 0x0, 0x0, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A};
static const char J2K_HEAD[] = {0xFF, 0x4F, 0xFF, 0x51, 0x00};

/* We only need this because of how the presets are set */
/* this typedef is copied from 'openjpeg-1.5.0/applications/codec/image_to_j2k.c' */
typedef struct img_folder {
  /** The directory path of the folder containing input images. */
  char *imgdirpath;
  /** Output format. */
  char *out_format;
  /** Enable option. */
  char set_imgdir;
  /** Enable Cod Format for output. */
  char set_out_format;
  /** User specified rate stored in case of cinema option. */
  float *rates;
} img_fol_t;

static bool check_jp2(const unsigned char *mem, const size_t size) /* J2K_CFMT */
{
  if (size < sizeof(JP2_HEAD)) {
    return false;
  }
  return memcmp(JP2_HEAD, mem, sizeof(JP2_HEAD)) ? 0 : 1;
}

static bool check_j2k(const unsigned char *mem, const size_t size) /* J2K_CFMT */
{
  if (size < sizeof(J2K_HEAD)) {
    return false;
  }
  return memcmp(J2K_HEAD, mem, sizeof(J2K_HEAD)) ? 0 : 1;
}

static OPJ_CODEC_FORMAT format_from_header(const unsigned char mem[JP2_FILEHEADER_SIZE],
                                           const size_t size)
{
  if (check_jp2(mem, size)) {
    return OPJ_CODEC_JP2;
  }
  if (check_j2k(mem, size)) {
    return OPJ_CODEC_J2K;
  }

  return OPJ_CODEC_UNKNOWN;
}

bool imb_is_a_jp2(const unsigned char *buf, size_t size)
{
  return (check_jp2(buf, size) || check_j2k(buf, size));
}

/**
 * sample error callback expecting a FILE* client object
 */
static void error_callback(const char *msg, void *client_data)
{
  FILE *stream = (FILE *)client_data;
  fprintf(stream, "[ERROR] %s", msg);
}
/**
 * sample warning callback expecting a FILE* client object
 */
static void warning_callback(const char *msg, void *client_data)
{
  FILE *stream = (FILE *)client_data;
  fprintf(stream, "[WARNING] %s", msg);
}

#ifdef DEBUG
/**
 * sample debug callback expecting no client object
 */
static void info_callback(const char *msg, void *client_data)
{
  FILE *stream = (FILE *)client_data;
  fprintf(stream, "[INFO] %s", msg);
}
#endif

#define PIXEL_LOOPER_BEGIN(_rect) \
  for (y = h - 1; y != (unsigned int)(-1); y--) { \
    for (i = y * w, i_next = (y + 1) * w; i < i_next; i++, _rect += 4) {

#define PIXEL_LOOPER_BEGIN_CHANNELS(_rect, _channels) \
  for (y = h - 1; y != (unsigned int)(-1); y--) { \
    for (i = y * w, i_next = (y + 1) * w; i < i_next; i++, _rect += _channels) {

#define PIXEL_LOOPER_END \
  } \
  } \
  (void)0

/* -------------------------------------------------------------------- */
/** \name Buffer Stream
 * \{ */

struct BufInfo {
  const unsigned char *buf;
  const unsigned char *cur;
  OPJ_OFF_T len;
};

static void opj_read_from_buffer_free(void *UNUSED(p_user_data))
{
  /* nop */
}

static OPJ_SIZE_T opj_read_from_buffer(void *p_buffer, OPJ_SIZE_T p_nb_bytes, void *p_user_data)
{
  struct BufInfo *p_file = p_user_data;
  OPJ_UINT32 l_nb_read;

  if (p_file->cur + p_nb_bytes < p_file->buf + p_file->len) {
    l_nb_read = p_nb_bytes;
  }
  else {
    l_nb_read = (OPJ_UINT32)(p_file->buf + p_file->len - p_file->cur);
  }
  memcpy(p_buffer, p_file->cur, l_nb_read);
  p_file->cur += l_nb_read;

  return l_nb_read ? l_nb_read : ((OPJ_SIZE_T)-1);
}

#if 0
static OPJ_SIZE_T opj_write_from_buffer(void *p_buffer, OPJ_SIZE_T p_nb_bytes, void *p_user_data)
{
  struct BufInfo *p_file = p_user_data;
  memcpy(p_file->cur, p_buffer, p_nb_bytes);
  p_file->cur += p_nb_bytes;
  p_file->len += p_nb_bytes;
  return p_nb_bytes;
}
#endif

static OPJ_OFF_T opj_skip_from_buffer(OPJ_OFF_T p_nb_bytes, void *p_user_data)
{
  struct BufInfo *p_file = p_user_data;
  if (p_file->cur + p_nb_bytes < p_file->buf + p_file->len) {
    p_file->cur += p_nb_bytes;
    return p_nb_bytes;
  }
  p_file->cur = p_file->buf + p_file->len;
  return (OPJ_OFF_T)-1;
}

static OPJ_BOOL opj_seek_from_buffer(OPJ_OFF_T p_nb_bytes, void *p_user_data)
{
  struct BufInfo *p_file = p_user_data;
  if (p_nb_bytes < p_file->len) {
    p_file->cur = p_file->buf + p_nb_bytes;
    return OPJ_TRUE;
  }
  p_file->cur = p_file->buf + p_file->len;
  return OPJ_FALSE;
}

/**
 * Stream wrapper for memory buffer
 * (would be nice if this was supported by the API).
 */

static opj_stream_t *opj_stream_create_from_buffer(struct BufInfo *p_file,
                                                   OPJ_UINT32 p_size,
                                                   OPJ_BOOL p_is_read_stream)
{
  opj_stream_t *l_stream = opj_stream_create(p_size, p_is_read_stream);
  if (l_stream == NULL) {
    return NULL;
  }
  opj_stream_set_user_data(l_stream, p_file, opj_read_from_buffer_free);
  opj_stream_set_user_data_length(l_stream, p_file->len);
  opj_stream_set_read_function(l_stream, opj_read_from_buffer);
#if 0 /* UNUSED */
  opj_stream_set_write_function(l_stream, opj_write_from_buffer);
#endif
  opj_stream_set_skip_function(l_stream, opj_skip_from_buffer);
  opj_stream_set_seek_function(l_stream, opj_seek_from_buffer);

  return l_stream;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name File Stream
 * \{ */

static void opj_free_from_file(void *p_user_data)
{
  FILE *f = p_user_data;
  fclose(f);
}

static OPJ_UINT64 opj_get_data_length_from_file(void *p_user_data)
{
  FILE *p_file = p_user_data;
  OPJ_OFF_T file_length = 0;

  fseek(p_file, 0, SEEK_END);
  file_length = ftell(p_file);
  fseek(p_file, 0, SEEK_SET);

  return (OPJ_UINT64)file_length;
}

static OPJ_SIZE_T opj_read_from_file(void *p_buffer, OPJ_SIZE_T p_nb_bytes, void *p_user_data)
{
  FILE *p_file = p_user_data;
  OPJ_SIZE_T l_nb_read = fread(p_buffer, 1, p_nb_bytes, p_file);
  return l_nb_read ? l_nb_read : (OPJ_SIZE_T)-1;
}

static OPJ_SIZE_T opj_write_from_file(void *p_buffer, OPJ_SIZE_T p_nb_bytes, void *p_user_data)
{
  FILE *p_file = p_user_data;
  return fwrite(p_buffer, 1, p_nb_bytes, p_file);
}

static OPJ_OFF_T opj_skip_from_file(OPJ_OFF_T p_nb_bytes, void *p_user_data)
{
  FILE *p_file = p_user_data;
  if (fseek(p_file, p_nb_bytes, SEEK_CUR)) {
    return -1;
  }
  return p_nb_bytes;
}

static OPJ_BOOL opj_seek_from_file(OPJ_OFF_T p_nb_bytes, void *p_user_data)
{
  FILE *p_file = p_user_data;
  if (fseek(p_file, p_nb_bytes, SEEK_SET)) {
    return OPJ_FALSE;
  }
  return OPJ_TRUE;
}

/**
 * Stream wrapper for memory file
 * (would be nice if this was supported by the API).
 */

static opj_stream_t *opj_stream_create_from_file(const char *filepath,
                                                 OPJ_UINT32 p_size,
                                                 OPJ_BOOL p_is_read_stream,
                                                 FILE **r_file)
{
  FILE *p_file = BLI_fopen(filepath, p_is_read_stream ? "rb" : "wb");
  if (p_file == NULL) {
    return NULL;
  }

  opj_stream_t *l_stream = opj_stream_create(p_size, p_is_read_stream);
  if (l_stream == NULL) {
    fclose(p_file);
    return NULL;
  }

  opj_stream_set_user_data(l_stream, p_file, opj_free_from_file);
  opj_stream_set_user_data_length(l_stream, opj_get_data_length_from_file(p_file));
  opj_stream_set_write_function(l_stream, opj_write_from_file);
  opj_stream_set_read_function(l_stream, opj_read_from_file);
  opj_stream_set_skip_function(l_stream, opj_skip_from_file);
  opj_stream_set_seek_function(l_stream, opj_seek_from_file);

  if (r_file) {
    *r_file = p_file;
  }
  return l_stream;
}

/** \} */

static ImBuf *imb_load_jp2_stream(opj_stream_t *stream,
                                  OPJ_CODEC_FORMAT p_format,
                                  int flags,
                                  char colorspace[IM_MAX_SPACE]);

ImBuf *imb_load_jp2(const unsigned char *mem,
                    size_t size,
                    int flags,
                    char colorspace[IM_MAX_SPACE])
{
  const OPJ_CODEC_FORMAT format = (size > JP2_FILEHEADER_SIZE) ? format_from_header(mem, size) :
                                                                 OPJ_CODEC_UNKNOWN;
  struct BufInfo buf_wrapper = {
      .buf = mem,
      .cur = mem,
      .len = size,
  };
  opj_stream_t *stream = opj_stream_create_from_buffer(
      &buf_wrapper, OPJ_J2K_STREAM_CHUNK_SIZE, true);
  ImBuf *ibuf = imb_load_jp2_stream(stream, format, flags, colorspace);
  opj_stream_destroy(stream);
  return ibuf;
}

ImBuf *imb_load_jp2_filepath(const char *filepath, int flags, char colorspace[IM_MAX_SPACE])
{
  FILE *p_file = NULL;
  unsigned char mem[JP2_FILEHEADER_SIZE];
  opj_stream_t *stream = opj_stream_create_from_file(
      filepath, OPJ_J2K_STREAM_CHUNK_SIZE, true, &p_file);
  if (stream) {
    return NULL;
  }

  if (fread(mem, sizeof(mem), 1, p_file) != sizeof(mem)) {
    opj_stream_destroy(stream);
    return NULL;
  }

  fseek(p_file, 0, SEEK_SET);

  const OPJ_CODEC_FORMAT format = format_from_header(mem, sizeof(mem));
  ImBuf *ibuf = imb_load_jp2_stream(stream, format, flags, colorspace);
  opj_stream_destroy(stream);
  return ibuf;
}

static ImBuf *imb_load_jp2_stream(opj_stream_t *stream,
                                  const OPJ_CODEC_FORMAT format,
                                  int flags,
                                  char colorspace[IM_MAX_SPACE])
{
  if (format == OPJ_CODEC_UNKNOWN) {
    return NULL;
  }

  struct ImBuf *ibuf = NULL;
  bool use_float = false; /* for precision higher than 8 use float */
  bool use_alpha = false;

  long signed_offsets[4] = {0, 0, 0, 0};
  int float_divs[4] = {1, 1, 1, 1};

  unsigned int i, i_next, w, h, planes;
  unsigned int y;
  int *r, *g, *b, *a; /* matching 'opj_image_comp.data' type */

  opj_dparameters_t parameters; /* decompression parameters */

  opj_image_t *image = NULL;
  opj_codec_t *codec = NULL; /* handle to a decompressor */

  /* both 8, 12 and 16 bit JP2Ks are default to standard byte colorspace */
  colorspace_set_default_role(colorspace, IM_MAX_SPACE, COLOR_ROLE_DEFAULT_BYTE);

  /* set decoding parameters to default values */
  opj_set_default_decoder_parameters(&parameters);

  /* JPEG 2000 compressed image data */

  /* get a decoder handle */
  codec = opj_create_decompress(format);

  /* configure the event callbacks (not required) */
  opj_set_error_handler(codec, error_callback, stderr);
  opj_set_warning_handler(codec, warning_callback, stderr);
#ifdef DEBUG /* too noisy */
  opj_set_info_handler(codec, info_callback, stderr);
#endif

  /* setup the decoder decoding parameters using the current image and user parameters */
  if (opj_setup_decoder(codec, &parameters) == false) {
    goto finally;
  }

  if (opj_read_header(stream, codec, &image) == false) {
    printf("OpenJPEG error: failed to read the header\n");
    goto finally;
  }

  /* decode the stream and fill the image structure */
  if (opj_decode(codec, stream, image) == false) {
    fprintf(stderr, "ERROR -> j2k_to_image: failed to decode image!\n");
    goto finally;
  }

  if ((image->numcomps * image->x1 * image->y1) == 0) {
    fprintf(stderr, "\nError: invalid raw image parameters\n");
    goto finally;
  }

  w = image->comps[0].w;
  h = image->comps[0].h;

  switch (image->numcomps) {
    case 1: /* Gray-scale. */
    case 3: /* Color. */
      planes = 24;
      use_alpha = false;
      break;
    default:       /* 2 or 4 - Gray-scale or Color + alpha. */
      planes = 32; /* Gray-scale + alpha. */
      use_alpha = true;
      break;
  }

  i = image->numcomps;
  if (i > 4) {
    i = 4;
  }

  while (i) {
    i--;

    if (image->comps[i].prec > 8) {
      use_float = true;
    }

    if (image->comps[i].sgnd) {
      signed_offsets[i] = 1 << (image->comps[i].prec - 1);
    }

    /* only needed for float images but doesn't hurt to calc this */
    float_divs[i] = (1 << image->comps[i].prec) - 1;
  }

  ibuf = IMB_allocImBuf(w, h, planes, use_float ? IB_rectfloat : IB_rect);

  if (ibuf == NULL) {
    goto finally;
  }

  ibuf->ftype = IMB_FTYPE_JP2;
  if (1 /* is_jp2 */) {
    ibuf->foptions.flag |= JP2_JP2;
  }
  else {
    ibuf->foptions.flag |= JP2_J2K;
  }

  if (use_float) {
    float *rect_float = ibuf->rect_float;

    if (image->numcomps < 3) {
      r = image->comps[0].data;
      a = (use_alpha) ? image->comps[1].data : NULL;

      /* Gray-scale 12bits+ */
      if (use_alpha) {
        a = image->comps[1].data;
        PIXEL_LOOPER_BEGIN (rect_float) {
          rect_float[0] = rect_float[1] = rect_float[2] = (float)(r[i] + signed_offsets[0]) /
                                                          float_divs[0];
          rect_float[3] = (a[i] + signed_offsets[1]) / float_divs[1];
        }
        PIXEL_LOOPER_END;
      }
      else {
        PIXEL_LOOPER_BEGIN (rect_float) {
          rect_float[0] = rect_float[1] = rect_float[2] = (float)(r[i] + signed_offsets[0]) /
                                                          float_divs[0];
          rect_float[3] = 1.0f;
        }
        PIXEL_LOOPER_END;
      }
    }
    else {
      r = image->comps[0].data;
      g = image->comps[1].data;
      b = image->comps[2].data;

      /* RGB or RGBA 12bits+ */
      if (use_alpha) {
        a = image->comps[3].data;
        PIXEL_LOOPER_BEGIN (rect_float) {
          rect_float[0] = (float)(r[i] + signed_offsets[0]) / float_divs[0];
          rect_float[1] = (float)(g[i] + signed_offsets[1]) / float_divs[1];
          rect_float[2] = (float)(b[i] + signed_offsets[2]) / float_divs[2];
          rect_float[3] = (float)(a[i] + signed_offsets[3]) / float_divs[3];
        }
        PIXEL_LOOPER_END;
      }
      else {
        PIXEL_LOOPER_BEGIN (rect_float) {
          rect_float[0] = (float)(r[i] + signed_offsets[0]) / float_divs[0];
          rect_float[1] = (float)(g[i] + signed_offsets[1]) / float_divs[1];
          rect_float[2] = (float)(b[i] + signed_offsets[2]) / float_divs[2];
          rect_float[3] = 1.0f;
        }
        PIXEL_LOOPER_END;
      }
    }
  }
  else {
    unsigned char *rect_uchar = (unsigned char *)ibuf->rect;

    if (image->numcomps < 3) {
      r = image->comps[0].data;
      a = (use_alpha) ? image->comps[1].data : NULL;

      /* Gray-scale. */
      if (use_alpha) {
        a = image->comps[3].data;
        PIXEL_LOOPER_BEGIN (rect_uchar) {
          rect_uchar[0] = rect_uchar[1] = rect_uchar[2] = (r[i] + signed_offsets[0]);
          rect_uchar[3] = a[i] + signed_offsets[1];
        }
        PIXEL_LOOPER_END;
      }
      else {
        PIXEL_LOOPER_BEGIN (rect_uchar) {
          rect_uchar[0] = rect_uchar[1] = rect_uchar[2] = (r[i] + signed_offsets[0]);
          rect_uchar[3] = 255;
        }
        PIXEL_LOOPER_END;
      }
    }
    else {
      r = image->comps[0].data;
      g = image->comps[1].data;
      b = image->comps[2].data;

      /* 8bit RGB or RGBA */
      if (use_alpha) {
        a = image->comps[3].data;
        PIXEL_LOOPER_BEGIN (rect_uchar) {
          rect_uchar[0] = r[i] + signed_offsets[0];
          rect_uchar[1] = g[i] + signed_offsets[1];
          rect_uchar[2] = b[i] + signed_offsets[2];
          rect_uchar[3] = a[i] + signed_offsets[3];
        }
        PIXEL_LOOPER_END;
      }
      else {
        PIXEL_LOOPER_BEGIN (rect_uchar) {
          rect_uchar[0] = r[i] + signed_offsets[0];
          rect_uchar[1] = g[i] + signed_offsets[1];
          rect_uchar[2] = b[i] + signed_offsets[2];
          rect_uchar[3] = 255;
        }
        PIXEL_LOOPER_END;
      }
    }
  }

  if (flags & IB_rect) {
    IMB_rect_from_float(ibuf);
  }

finally:

  /* free remaining structures */
  if (codec) {
    opj_destroy_codec(codec);
  }

  if (image) {
    opj_image_destroy(image);
  }

  return ibuf;
}

#if 0
static opj_image_t *rawtoimage(const char *filename,
                               opj_cparameters_t *parameters,
                               raw_cparameters_t *raw_cp)
#endif
/* prec can be 8, 12, 16 */

/* Use inline because the float passed can be a function call
 * that would end up being called many times. */
#if 0
#  define UPSAMPLE_8_TO_12(_val) ((_val << 4) | (_val & ((1 << 4) - 1)))
#  define UPSAMPLE_8_TO_16(_val) ((_val << 8) + _val)

#  define DOWNSAMPLE_FLOAT_TO_8BIT(_val) \
    (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 255 : (int)(255.0f * (_val)))
#  define DOWNSAMPLE_FLOAT_TO_12BIT(_val) \
    (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 4095 : (int)(4095.0f * (_val)))
#  define DOWNSAMPLE_FLOAT_TO_16BIT(_val) \
    (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 65535 : (int)(65535.0f * (_val)))
#else

BLI_INLINE int UPSAMPLE_8_TO_12(const unsigned char _val)
{
  return (_val << 4) | (_val & ((1 << 4) - 1));
}
BLI_INLINE int UPSAMPLE_8_TO_16(const unsigned char _val)
{
  return (_val << 8) + _val;
}

BLI_INLINE int DOWNSAMPLE_FLOAT_TO_8BIT(const float _val)
{
  return (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 255 : (int)(255.0f * (_val)));
}
BLI_INLINE int DOWNSAMPLE_FLOAT_TO_12BIT(const float _val)
{
  return (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 4095 : (int)(4095.0f * (_val)));
}
BLI_INLINE int DOWNSAMPLE_FLOAT_TO_16BIT(const float _val)
{
  return (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 65535 : (int)(65535.0f * (_val)));
}
#endif

/*
 * 2048x1080 (2K) at 24 fps or 48 fps, or 4096x2160 (4K) at 24 fps;
 * 3x12 bits per pixel, XYZ color space
 *
 * - In 2K, for Scope (2.39:1) presentation 2048x858  pixels of the image is used
 * - In 2K, for Flat  (1.85:1) presentation 1998x1080 pixels of the image is used
 */

/* ****************************** COPIED FROM image_to_j2k.c */

/* ----------------------------------------------------------------------- */
#define CINEMA_24_CS 1302083 /* Code-stream length for 24fps. */
#define CINEMA_48_CS 651041  /* Code-stream length for 48fps. */
#define COMP_24_CS 1041666   /* Maximum size per color component for 2K & 4K @ 24fps. */
#define COMP_48_CS 520833    /* Maximum size per color component for 2K @ 48fps. */

static int init_4K_poc(opj_poc_t *POC, int numres)
{
  POC[0].tile = 1;
  POC[0].resno0 = 0;
  POC[0].compno0 = 0;
  POC[0].layno1 = 1;
  POC[0].resno1 = numres - 1;
  POC[0].compno1 = 3;
  POC[0].prg1 = OPJ_CPRL;
  POC[1].tile = 1;
  POC[1].resno0 = numres - 1;
  POC[1].compno0 = 0;
  POC[1].layno1 = 1;
  POC[1].resno1 = numres;
  POC[1].compno1 = 3;
  POC[1].prg1 = OPJ_CPRL;
  return 2;
}

static void cinema_parameters(opj_cparameters_t *parameters)
{
  parameters->tile_size_on = 0; /* false */
  parameters->cp_tdx = 1;
  parameters->cp_tdy = 1;

  /* Tile part. */
  parameters->tp_flag = 'C';
  parameters->tp_on = 1;

  /* Tile and Image shall be at (0, 0). */
  parameters->cp_tx0 = 0;
  parameters->cp_ty0 = 0;
  parameters->image_offset_x0 = 0;
  parameters->image_offset_y0 = 0;

  /* Code-block size = 32 * 32. */
  parameters->cblockw_init = 32;
  parameters->cblockh_init = 32;
  parameters->csty |= 0x01;

  /* The progression order shall be CPRL. */
  parameters->prog_order = OPJ_CPRL;

  /* No ROI */
  parameters->roi_compno = -1;

  parameters->subsampling_dx = 1;
  parameters->subsampling_dy = 1;

  /* 9-7 transform */
  parameters->irreversible = 1;
}

static void cinema_setup_encoder(opj_cparameters_t *parameters,
                                 opj_image_t *image,
                                 img_fol_t *img_fol)
{
  int i;
  float temp_rate;

  switch (parameters->cp_cinema) {
    case OPJ_CINEMA2K_24:
    case OPJ_CINEMA2K_48:
      if (parameters->numresolution > 6) {
        parameters->numresolution = 6;
      }
      if (!((image->comps[0].w == 2048) || (image->comps[0].h == 1080))) {
        fprintf(stdout,
                "Image coordinates %u x %u is not 2K compliant.\nJPEG Digital Cinema Profile-3 "
                "(2K profile) compliance requires that at least one of coordinates match 2048 x "
                "1080\n",
                image->comps[0].w,
                image->comps[0].h);
        parameters->cp_rsiz = OPJ_STD_RSIZ;
      }
      else {
        parameters->cp_rsiz = OPJ_CINEMA2K;
      }
      break;

    case OPJ_CINEMA4K_24:
      if (parameters->numresolution < 1) {
        parameters->numresolution = 1;
      }
      else if (parameters->numresolution > 7) {
        parameters->numresolution = 7;
      }
      if (!((image->comps[0].w == 4096) || (image->comps[0].h == 2160))) {
        fprintf(stdout,
                "Image coordinates %u x %u is not 4K compliant.\nJPEG Digital Cinema Profile-4"
                "(4K profile) compliance requires that at least one of coordinates match 4096 x "
                "2160\n",
                image->comps[0].w,
                image->comps[0].h);
        parameters->cp_rsiz = OPJ_STD_RSIZ;
      }
      else {
        parameters->cp_rsiz = OPJ_CINEMA4K;
      }
      parameters->numpocs = init_4K_poc(parameters->POC, parameters->numresolution);
      break;
    case OPJ_OFF:
      /* do nothing */
      break;
  }

  switch (parameters->cp_cinema) {
    case OPJ_CINEMA2K_24:
    case OPJ_CINEMA4K_24:
      for (i = 0; i < parameters->tcp_numlayers; i++) {
        temp_rate = 0;
        if (img_fol->rates[i] == 0) {
          parameters->tcp_rates[0] = ((float)(image->numcomps * image->comps[0].w *
                                              image->comps[0].h * image->comps[0].prec)) /
                                     (CINEMA_24_CS * 8 * image->comps[0].dx * image->comps[0].dy);
        }
        else {
          temp_rate = ((float)(image->numcomps * image->comps[0].w * image->comps[0].h *
                               image->comps[0].prec)) /
                      (img_fol->rates[i] * 8 * image->comps[0].dx * image->comps[0].dy);
          if (temp_rate > CINEMA_24_CS) {
            parameters->tcp_rates[i] = ((float)(image->numcomps * image->comps[0].w *
                                                image->comps[0].h * image->comps[0].prec)) /
                                       (CINEMA_24_CS * 8 * image->comps[0].dx *
                                        image->comps[0].dy);
          }
          else {
            parameters->tcp_rates[i] = img_fol->rates[i];
          }
        }
      }
      parameters->max_comp_size = COMP_24_CS;
      break;

    case OPJ_CINEMA2K_48:
      for (i = 0; i < parameters->tcp_numlayers; i++) {
        temp_rate = 0;
        if (img_fol->rates[i] == 0) {
          parameters->tcp_rates[0] = ((float)(image->numcomps * image->comps[0].w *
                                              image->comps[0].h * image->comps[0].prec)) /
                                     (CINEMA_48_CS * 8 * image->comps[0].dx * image->comps[0].dy);
        }
        else {
          temp_rate = ((float)(image->numcomps * image->comps[0].w * image->comps[0].h *
                               image->comps[0].prec)) /
                      (img_fol->rates[i] * 8 * image->comps[0].dx * image->comps[0].dy);
          if (temp_rate > CINEMA_48_CS) {
            parameters->tcp_rates[0] = ((float)(image->numcomps * image->comps[0].w *
                                                image->comps[0].h * image->comps[0].prec)) /
                                       (CINEMA_48_CS * 8 * image->comps[0].dx *
                                        image->comps[0].dy);
          }
          else {
            parameters->tcp_rates[i] = img_fol->rates[i];
          }
        }
      }
      parameters->max_comp_size = COMP_48_CS;
      break;
    case OPJ_OFF:
      /* do nothing */
      break;
  }
  parameters->cp_disto_alloc = 1;
}

static float channel_colormanage_noop(float value)
{
  return value;
}

static opj_image_t *ibuftoimage(ImBuf *ibuf, opj_cparameters_t *parameters)
{
  unsigned char *rect_uchar;
  float *rect_float, from_straight[4];

  unsigned int subsampling_dx = parameters->subsampling_dx;
  unsigned int subsampling_dy = parameters->subsampling_dy;

  unsigned int i, i_next, numcomps, w, h, prec;
  unsigned int y;
  int *r, *g, *b, *a; /* matching 'opj_image_comp.data' type */
  OPJ_COLOR_SPACE color_space;
  opj_image_cmptparm_t cmptparm[4]; /* maximum of 4 components */
  opj_image_t *image = NULL;

  float (*chanel_colormanage_cb)(float);

  img_fol_t img_fol; /* only needed for cinema presets */
  memset(&img_fol, 0, sizeof(img_fol_t));

  if (ibuf->float_colorspace || (ibuf->colormanage_flag & IMB_COLORMANAGE_IS_DATA)) {
    /* float buffer was managed already, no need in color space conversion */
    chanel_colormanage_cb = channel_colormanage_noop;
  }
  else {
    /* standard linear-to-SRGB conversion if float buffer wasn't managed */
    chanel_colormanage_cb = linearrgb_to_srgb;
  }

  if (ibuf->foptions.flag & JP2_CINE) {

    if (ibuf->x == 4096 || ibuf->y == 2160) {
      parameters->cp_cinema = OPJ_CINEMA4K_24;
    }
    else {
      if (ibuf->foptions.flag & JP2_CINE_48FPS) {
        parameters->cp_cinema = OPJ_CINEMA2K_48;
      }
      else {
        parameters->cp_cinema = OPJ_CINEMA2K_24;
      }
    }
    if (parameters->cp_cinema) {
      img_fol.rates = (float *)MEM_mallocN(parameters->tcp_numlayers * sizeof(float), "jp2_rates");
      for (i = 0; i < parameters->tcp_numlayers; i++) {
        img_fol.rates[i] = parameters->tcp_rates[i];
      }
      cinema_parameters(parameters);
    }

    color_space = (ibuf->foptions.flag & JP2_YCC) ? OPJ_CLRSPC_SYCC : OPJ_CLRSPC_SRGB;
    prec = 12;
    numcomps = 3;
  }
  else {
    /* Get settings from the imbuf */
    color_space = (ibuf->foptions.flag & JP2_YCC) ? OPJ_CLRSPC_SYCC : OPJ_CLRSPC_SRGB;

    if (ibuf->foptions.flag & JP2_16BIT) {
      prec = 16;
    }
    else if (ibuf->foptions.flag & JP2_12BIT) {
      prec = 12;
    }
    else {
      prec = 8;
    }

    /* 32bit images == alpha channel. */
    /* Gray-scale not supported yet. */
    numcomps = (ibuf->planes == 32) ? 4 : 3;
  }

  w = ibuf->x;
  h = ibuf->y;

  /* initialize image components */
  memset(&cmptparm, 0, sizeof(opj_image_cmptparm_t[4]));
  for (i = 0; i < numcomps; i++) {
    cmptparm[i].prec = prec;
    cmptparm[i].bpp = prec;
    cmptparm[i].sgnd = 0;
    cmptparm[i].dx = subsampling_dx;
    cmptparm[i].dy = subsampling_dy;
    cmptparm[i].w = w;
    cmptparm[i].h = h;
  }
  /* create the image */
  image = opj_image_create(numcomps, &cmptparm[0], color_space);
  if (!image) {
    printf("Error: opj_image_create() failed\n");
    return NULL;
  }

  /* set image offset and reference grid */
  image->x0 = parameters->image_offset_x0;
  image->y0 = parameters->image_offset_y0;
  image->x1 = image->x0 + (w - 1) * subsampling_dx + 1 + image->x0;
  image->y1 = image->y0 + (h - 1) * subsampling_dy + 1 + image->y0;

  /* set image data */
  rect_uchar = (unsigned char *)ibuf->rect;
  rect_float = ibuf->rect_float;

  /* set the destination channels */
  r = image->comps[0].data;
  g = image->comps[1].data;
  b = image->comps[2].data;
  a = (numcomps == 4) ? image->comps[3].data : NULL;

  if (rect_float && rect_uchar && prec == 8) {
    /* No need to use the floating point buffer, just write the 8 bits from the char buffer */
    rect_float = NULL;
  }

  if (rect_float) {
    int channels_in_float = ibuf->channels ? ibuf->channels : 4;

    switch (prec) {
      case 8: /* Convert blenders float color channels to 8, 12 or 16bit ints */
        if (numcomps == 4) {
          if (channels_in_float == 4) {
            PIXEL_LOOPER_BEGIN (rect_float) {
              premul_to_straight_v4_v4(from_straight, rect_float);
              r[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(from_straight[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(from_straight[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(from_straight[2]));
              a[i] = DOWNSAMPLE_FLOAT_TO_8BIT(from_straight[3]);
            }
            PIXEL_LOOPER_END;
          }
          else if (channels_in_float == 3) {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 3) {
              r[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(rect_float[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(rect_float[2]));
              a[i] = 255;
            }
            PIXEL_LOOPER_END;
          }
          else {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 1) {
              r[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = b[i] = r[i];
              a[i] = 255;
            }
            PIXEL_LOOPER_END;
          }
        }
        else {
          if (channels_in_float == 4) {
            PIXEL_LOOPER_BEGIN (rect_float) {
              premul_to_straight_v4_v4(from_straight, rect_float);
              r[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(from_straight[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(from_straight[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(from_straight[2]));
            }
            PIXEL_LOOPER_END;
          }
          else if (channels_in_float == 3) {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 3) {
              r[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(rect_float[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(rect_float[2]));
            }
            PIXEL_LOOPER_END;
          }
          else {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 1) {
              r[i] = DOWNSAMPLE_FLOAT_TO_8BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = b[i] = r[i];
            }
            PIXEL_LOOPER_END;
          }
        }
        break;

      case 12:
        if (numcomps == 4) {
          if (channels_in_float == 4) {
            PIXEL_LOOPER_BEGIN (rect_float) {
              premul_to_straight_v4_v4(from_straight, rect_float);
              r[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(from_straight[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(from_straight[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(from_straight[2]));
              a[i] = DOWNSAMPLE_FLOAT_TO_12BIT(from_straight[3]);
            }
            PIXEL_LOOPER_END;
          }
          else if (channels_in_float == 3) {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 3) {
              r[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(rect_float[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(rect_float[2]));
              a[i] = 4095;
            }
            PIXEL_LOOPER_END;
          }
          else {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 1) {
              r[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = b[i] = r[i];
              a[i] = 4095;
            }
            PIXEL_LOOPER_END;
          }
        }
        else {
          if (channels_in_float == 4) {
            PIXEL_LOOPER_BEGIN (rect_float) {
              premul_to_straight_v4_v4(from_straight, rect_float);
              r[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(from_straight[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(from_straight[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(from_straight[2]));
            }
            PIXEL_LOOPER_END;
          }
          else if (channels_in_float == 3) {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 3) {
              r[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(rect_float[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(rect_float[2]));
            }
            PIXEL_LOOPER_END;
          }
          else {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 1) {
              r[i] = DOWNSAMPLE_FLOAT_TO_12BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = b[i] = r[i];
            }
            PIXEL_LOOPER_END;
          }
        }
        break;

      case 16:
        if (numcomps == 4) {
          if (channels_in_float == 4) {
            PIXEL_LOOPER_BEGIN (rect_float) {
              premul_to_straight_v4_v4(from_straight, rect_float);
              r[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(from_straight[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(from_straight[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(from_straight[2]));
              a[i] = DOWNSAMPLE_FLOAT_TO_16BIT(from_straight[3]);
            }
            PIXEL_LOOPER_END;
          }
          else if (channels_in_float == 3) {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 3) {
              r[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(rect_float[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(rect_float[2]));
              a[i] = 65535;
            }
            PIXEL_LOOPER_END;
          }
          else {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 1) {
              r[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = b[i] = r[i];
              a[i] = 65535;
            }
            PIXEL_LOOPER_END;
          }
        }
        else {
          if (channels_in_float == 4) {
            PIXEL_LOOPER_BEGIN (rect_float) {
              premul_to_straight_v4_v4(from_straight, rect_float);
              r[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(from_straight[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(from_straight[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(from_straight[2]));
            }
            PIXEL_LOOPER_END;
          }
          else if (channels_in_float == 3) {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 3) {
              r[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(rect_float[1]));
              b[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(rect_float[2]));
            }
            PIXEL_LOOPER_END;
          }
          else {
            PIXEL_LOOPER_BEGIN_CHANNELS (rect_float, 1) {
              r[i] = DOWNSAMPLE_FLOAT_TO_16BIT(chanel_colormanage_cb(rect_float[0]));
              g[i] = b[i] = r[i];
            }
            PIXEL_LOOPER_END;
          }
        }
        break;
    }
  }
  else {
    /* Just use rect. */
    switch (prec) {
      case 8:
        if (numcomps == 4) {
          PIXEL_LOOPER_BEGIN (rect_uchar) {
            r[i] = rect_uchar[0];
            g[i] = rect_uchar[1];
            b[i] = rect_uchar[2];
            a[i] = rect_uchar[3];
          }
          PIXEL_LOOPER_END;
        }
        else {
          PIXEL_LOOPER_BEGIN (rect_uchar) {
            r[i] = rect_uchar[0];
            g[i] = rect_uchar[1];
            b[i] = rect_uchar[2];
          }
          PIXEL_LOOPER_END;
        }
        break;

      case 12: /* Up Sampling, a bit pointless but best write the bit depth requested */
        if (numcomps == 4) {
          PIXEL_LOOPER_BEGIN (rect_uchar) {
            r[i] = UPSAMPLE_8_TO_12(rect_uchar[0]);
            g[i] = UPSAMPLE_8_TO_12(rect_uchar[1]);
            b[i] = UPSAMPLE_8_TO_12(rect_uchar[2]);
            a[i] = UPSAMPLE_8_TO_12(rect_uchar[3]);
          }
          PIXEL_LOOPER_END;
        }
        else {
          PIXEL_LOOPER_BEGIN (rect_uchar) {
            r[i] = UPSAMPLE_8_TO_12(rect_uchar[0]);
            g[i] = UPSAMPLE_8_TO_12(rect_uchar[1]);
            b[i] = UPSAMPLE_8_TO_12(rect_uchar[2]);
          }
          PIXEL_LOOPER_END;
        }
        break;

      case 16:
        if (numcomps == 4) {
          PIXEL_LOOPER_BEGIN (rect_uchar) {
            r[i] = UPSAMPLE_8_TO_16(rect_uchar[0]);
            g[i] = UPSAMPLE_8_TO_16(rect_uchar[1]);
            b[i] = UPSAMPLE_8_TO_16(rect_uchar[2]);
            a[i] = UPSAMPLE_8_TO_16(rect_uchar[3]);
          }
          PIXEL_LOOPER_END;
        }
        else {
          PIXEL_LOOPER_BEGIN (rect_uchar) {
            r[i] = UPSAMPLE_8_TO_16(rect_uchar[0]);
            g[i] = UPSAMPLE_8_TO_16(rect_uchar[1]);
            b[i] = UPSAMPLE_8_TO_16(rect_uchar[2]);
          }
          PIXEL_LOOPER_END;
        }
        break;
    }
  }

  /* Decide if MCT should be used */
  parameters->tcp_mct = image->numcomps == 3 ? 1 : 0;

  if (parameters->cp_cinema) {
    cinema_setup_encoder(parameters, image, &img_fol);
  }

  if (img_fol.rates) {
    MEM_freeN(img_fol.rates);
  }

  return image;
}

bool imb_save_jp2_stream(struct ImBuf *ibuf, opj_stream_t *stream, int flags);

bool imb_save_jp2(struct ImBuf *ibuf, const char *filepath, int flags)
{
  opj_stream_t *stream = opj_stream_create_from_file(
      filepath, OPJ_J2K_STREAM_CHUNK_SIZE, false, NULL);
  if (stream == NULL) {
    return 0;
  }
  const bool ok = imb_save_jp2_stream(ibuf, stream, flags);
  opj_stream_destroy(stream);
  return ok;
}

/* Found write info at http://users.ece.gatech.edu/~slabaugh/personal/c/bitmapUnix.c */
bool imb_save_jp2_stream(struct ImBuf *ibuf, opj_stream_t *stream, int UNUSED(flags))
{
  int quality = ibuf->foptions.quality;

  opj_cparameters_t parameters; /* compression parameters */
  opj_image_t *image = NULL;

  /* set encoding parameters to default values */
  opj_set_default_encoder_parameters(&parameters);

  /* compression ratio */
  /* invert range, from 10-100, 100-1
   * Where jpeg see's 1 and highest quality (lossless) and 100 is very low quality. */
  parameters.tcp_rates[0] = ((100 - quality) / 90.0f * 99.0f) + 1;

  parameters.tcp_numlayers = 1; /* only one resolution */
  parameters.cp_disto_alloc = 1;

  image = ibuftoimage(ibuf, &parameters);

  opj_codec_t *codec = NULL;
  bool ok = false;
  /* JP2 format output */
  {
    /* get a JP2 compressor handle */
    OPJ_CODEC_FORMAT format = OPJ_CODEC_JP2;
    if (ibuf->foptions.flag & JP2_J2K) {
      format = OPJ_CODEC_J2K;
    }
    else if (ibuf->foptions.flag & JP2_JP2) {
      format = OPJ_CODEC_JP2;
    }

    codec = opj_create_compress(format);

    /* configure the event callbacks (not required) */
    opj_set_error_handler(codec, error_callback, stderr);
    opj_set_warning_handler(codec, warning_callback, stderr);
#ifdef DEBUG /* too noisy */
    opj_set_info_handler(codec, info_callback, stderr);
#endif

    /* setup the encoder parameters using the current image and using user parameters */
    if (opj_setup_encoder(codec, &parameters, image) == false) {
      goto finally;
    }

    if (opj_start_compress(codec, image, stream) == false) {
      goto finally;
    }
    if (opj_encode(codec, stream) == false) {
      goto finally;
    }
    if (opj_end_compress(codec, stream) == false) {
      goto finally;
    }
  }

  ok = true;

finally:
  /* free remaining compression structures */
  if (codec) {
    opj_destroy_codec(codec);
  }

  /* free image data */
  if (image) {
    opj_image_destroy(image);
  }

  if (ok == false) {
    fprintf(stderr, "failed to encode image\n");
  }

  return ok;
}
