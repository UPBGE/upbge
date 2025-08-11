/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 *
 * The SGI Image File Format.
 * https://en.wikipedia.org/wiki/Silicon_Graphics_Image
 *
 * \note this format uses big-endian values.
 */

#include <algorithm>
#include <cstring>

#include "BLI_fileops.h"
#include "BLI_utildefines.h"

#include "MEM_guardedalloc.h"

#include "CLG_log.h"

#include "IMB_colormanagement.hh"
#include "IMB_filetype.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

static CLG_LogRef LOG = {"image.jpeg"};

/**
 * The SGI IRIS magic number.
 * The value is `[0x01 0xda]` when read as a big-endian ushort.
 */
#define IRIS_MAGIC 0732

/**
 * The SGI IRIS header.
 *
 * Not directly read and written, although this maps neatly to the on-disk value.
 */
struct IRIS_Header {
  ushort imagic; /* Stuff saved on disk. */
  ushort type;
  ushort dim;
  ushort xsize;
  ushort ysize;
  ushort zsize;
  uint min;
  uint max;
  uchar _pad1[4];
  char name[80];
  uint colormap;
  uchar _pad2[404];
};

#define HEADER_SIZE 512

BLI_STATIC_ASSERT(sizeof(IRIS_Header) == HEADER_SIZE, "Invalid header size");

#define RINTLUM (79)
#define GINTLUM (156)
#define BINTLUM (21)

#define ILUM(r, g, b) (int(RINTLUM * (r) + GINTLUM * (g) + BINTLUM * (b)) >> 8)

#define OFFSET_R 0 /* this is byte order dependent */
#define OFFSET_G 1
#define OFFSET_B 2
// #define OFFSET_A    3

#define CHANOFFSET(z) (3 - (z)) /* this is byte order dependent */

// #define TYPEMASK        0xff00
#define BPPMASK 0x00ff
// #define ITYPE_VERBATIM      0x0000 /* UNUSED */
#define ITYPE_RLE 0x0100
#define ISRLE(type) (((type) & 0xff00) == ITYPE_RLE)
// #define ISVERBATIM(type)    (((type) & 0xff00) == ITYPE_VERBATIM)
#define BPP(type) ((type) & BPPMASK)
#define RLE(bpp) (ITYPE_RLE | (bpp))
// #define VERBATIM(bpp)       (ITYPE_VERBATIM | (bpp)) /* UNUSED */
// #define IBUFSIZE(pixels)    ((pixels + (pixels >> 6)) << 2) /* UNUSED */
// #define RLE_NOP         0x00

/* local struct for mem access */
struct MFileOffset {
  const uchar *_file_data;
  uint _file_offset;
};

#define MFILE_DATA(inf) ((void)0, ((inf)->_file_data + (inf)->_file_offset))
#define MFILE_STEP(inf, step) \
  { \
    (inf)->_file_offset += step; \
  } \
  ((void)0)
#define MFILE_SEEK(inf, pos) \
  { \
    (inf)->_file_offset = pos; \
  } \
  ((void)0)

/* error flags */
#define DIRTY_FLAG_EOF (1 << 0)
#define DIRTY_FLAG_ENCODING (1 << 1)

/* Functions. */
static void readheader(MFileOffset *inf, IRIS_Header *image);
static int writeheader(FILE *outf, const IRIS_Header *image);

static ushort getshort(MFileOffset *inf);
static uint getlong(MFileOffset *mofs);
static void putshort(FILE *outf, ushort val);
static int putlong(FILE *outf, uint val);
static int writetab(FILE *outf, const uint *tab, int len);
static void readtab(MFileOffset *inf, uint *tab, int len);

static int expandrow(
    uchar *optr, const uchar *optr_end, const uchar *iptr, const uchar *iptr_end, int z);
static int expandrow2(
    float *optr, const float *optr_end, const uchar *iptr, const uchar *iptr_end, int z);
static void interleaverow(uchar *lptr, const uchar *cptr, int z, int n);
static void interleaverow2(float *lptr, const uchar *cptr, int z, int n);
static int compressrow(const uchar *lbuf, uchar *rlebuf, int z, int row_len);
static void lumrow(const uchar *rgbptr, uchar *lumptr, int n);

/* -------------------------------------------------------------------- */
/** \name Internal Image API
 * \{ */

/**
 * Change the ordering of the color bytes pointed to by rect from
 * RGBA to ABGR. size * 4 color bytes are reordered.
 *
 * Only this one is used liberally here, and in imbuf.
 */
static void imbuf_rgba_to_abgr(ImBuf *ibuf)
{
  size_t size;
  uchar rt, *cp = ibuf->byte_buffer.data;

  if (ibuf->byte_buffer.data) {
    size = IMB_get_pixel_count(ibuf);

    while (size-- > 0) {
      rt = cp[0];
      cp[0] = cp[3];
      cp[3] = rt;
      rt = cp[1];
      cp[1] = cp[2];
      cp[2] = rt;
      cp += 4;
    }
  }
}

/** \} */

/*
 * byte order independent read/write of shorts and ints.
 */

static ushort getshort(MFileOffset *inf)
{
  const uchar *buf;

  buf = MFILE_DATA(inf);
  MFILE_STEP(inf, 2);

  return (ushort(buf[0]) << 8) + (ushort(buf[1]) << 0);
}

static uint getlong(MFileOffset *mofs)
{
  const uchar *buf;

  buf = MFILE_DATA(mofs);
  MFILE_STEP(mofs, 4);

  return (uint(buf[0]) << 24) + (uint(buf[1]) << 16) + (uint(buf[2]) << 8) + (uint(buf[3]) << 0);
}

static void putshort(FILE *outf, ushort val)
{
  uchar buf[2];

  buf[0] = (val >> 8);
  buf[1] = (val >> 0);
  fwrite(buf, 2, 1, outf);
}

static int putlong(FILE *outf, uint val)
{
  uchar buf[4];

  buf[0] = (val >> 24);
  buf[1] = (val >> 16);
  buf[2] = (val >> 8);
  buf[3] = (val >> 0);
  return fwrite(buf, 4, 1, outf);
}

static void readheader(MFileOffset *inf, IRIS_Header *image)
{
  memset(image, 0, sizeof(IRIS_Header));
  image->imagic = getshort(inf);
  image->type = getshort(inf);
  image->dim = getshort(inf);
  image->xsize = getshort(inf);
  image->ysize = getshort(inf);
  image->zsize = getshort(inf);
}

static int writeheader(FILE *outf, const IRIS_Header *image)
{
  IRIS_Header t = {0};

  fwrite(&t, sizeof(IRIS_Header), 1, outf);
  fseek(outf, 0, SEEK_SET);
  putshort(outf, image->imagic);
  putshort(outf, image->type);
  putshort(outf, image->dim);
  putshort(outf, image->xsize);
  putshort(outf, image->ysize);
  putshort(outf, image->zsize);
  putlong(outf, image->min);
  putlong(outf, image->max);
  putlong(outf, 0);
  return fwrite("no name", 8, 1, outf);
}

static int writetab(FILE *outf, const uint *tab, int len)
{
  int r = 0;

  while (len) {
    r = putlong(outf, *tab++);
    len -= 4;
  }
  return r;
}

static void readtab(MFileOffset *inf, uint *tab, int len)
{
  while (len) {
    *tab++ = getlong(inf);
    len -= 4;
  }
}

/* From misc_util: flip the bytes from x. */
#define GS(x) (((uchar *)(x))[0] << 8 | ((uchar *)(x))[1])

bool imb_is_a_iris(const uchar *mem, size_t size)
{
  if (size < 2) {
    return false;
  }
  return GS(mem) == IRIS_MAGIC;
}

ImBuf *imb_loadiris(const uchar *mem, size_t size, int flags, ImFileColorSpace & /*r_colorspace*/)
{
  uint *base, *lptr = nullptr;
  float *fbase, *fptr = nullptr;
  const uchar *rledat;
  const uchar *mem_end = mem + size;
  MFileOffset _inf_data = {mem, 0}, *inf = &_inf_data;
  IRIS_Header image;
  int bpp, rle, cur, badorder;
  ImBuf *ibuf = nullptr;
  uchar dirty_flag = 0;

  if (!imb_is_a_iris(mem, size)) {
    return nullptr;
  }

  /* Could be part of the magic check above,
   * by convention this check only requests the size needed to read it's magic though. */
  if (size < HEADER_SIZE) {
    return nullptr;
  }

  readheader(inf, &image);
  /* The call to `imb_is_a_iris` ensures this. */
  BLI_assert(image.imagic == IRIS_MAGIC);

  rle = ISRLE(image.type);
  bpp = BPP(image.type);
  if (!ELEM(bpp, 1, 2)) {
    CLOG_ERROR(&LOG, "Image must have 1 or 2 byte per pix chan");
    return nullptr;
  }
  if (uint(image.zsize) > 8) {
    CLOG_ERROR(&LOG, "Channels over 8 not supported");
    return nullptr;
  }
  if (image.xsize == 0 || image.ysize == 0 || image.zsize == 0) {
    CLOG_ERROR(&LOG, "Zero size image found");
    return nullptr;
  }

  const int xsize = image.xsize;
  const int ysize = image.ysize;

  const int zsize_file = image.zsize;
  const int zsize_read = min_ii(image.zsize, 4);

  if (flags & IB_test) {
    ibuf = IMB_allocImBuf(image.xsize, image.ysize, 8 * image.zsize, 0);
    if (ibuf) {
      ibuf->ftype = IMB_FTYPE_IRIS;
    }
    return ibuf;
  }

  if (rle) {
    size_t tablen = size_t(ysize) * size_t(zsize_file) * sizeof(int);
    MFILE_SEEK(inf, HEADER_SIZE);

    uint *starttab = MEM_malloc_arrayN<uint>(tablen, "iris starttab");
    uint *lengthtab = MEM_malloc_arrayN<uint>(tablen, "iris endtab");

#define MFILE_CAPACITY_AT_PTR_OK_OR_FAIL(p) \
  if (UNLIKELY((p) > mem_end)) { \
    dirty_flag |= DIRTY_FLAG_EOF; \
    goto fail_rle; \
  } \
  ((void)0)

    MFILE_CAPACITY_AT_PTR_OK_OR_FAIL(MFILE_DATA(inf) + (2 * tablen));

    readtab(inf, starttab, tablen);
    readtab(inf, lengthtab, tablen);

    /* check data order */
    cur = 0;
    badorder = 0;
    for (size_t y = 0; y < ysize; y++) {
      for (size_t z = 0; z < zsize_file; z++) {
        if (starttab[y + z * ysize] < cur) {
          badorder = 1;
          break;
        }
        cur = starttab[y + z * ysize];
      }
      if (badorder) {
        break;
      }
    }

    if (bpp == 1) {

      ibuf = IMB_allocImBuf(xsize, ysize, 8 * zsize_read, IB_byte_data);
      if (!ibuf) {
        goto fail_rle;
      }
      ibuf->planes = std::min<int>(ibuf->planes, 32);
      base = (uint *)ibuf->byte_buffer.data;

      if (badorder) {
        for (size_t z = 0; z < zsize_read; z++) {
          lptr = base;
          for (size_t y = 0; y < ysize; y++) {
            MFILE_SEEK(inf, starttab[y + z * ysize]);
            rledat = MFILE_DATA(inf);
            MFILE_STEP(inf, lengthtab[y + z * ysize]);
            const uchar *rledat_next = MFILE_DATA(inf);
            uint *lptr_next = lptr + xsize;
            MFILE_CAPACITY_AT_PTR_OK_OR_FAIL(rledat_next);
            dirty_flag |= expandrow((uchar *)lptr, (uchar *)lptr_next, rledat, rledat_next, 3 - z);
            lptr = lptr_next;
          }
        }
      }
      else {
        lptr = base;
        for (size_t y = 0; y < ysize; y++) {

          uint *lptr_next = lptr + xsize;

          for (size_t z = 0; z < zsize_read; z++) {
            MFILE_SEEK(inf, starttab[y + z * ysize]);
            rledat = MFILE_DATA(inf);
            MFILE_STEP(inf, lengthtab[y + z * ysize]);
            const uchar *rledat_next = MFILE_DATA(inf);
            MFILE_CAPACITY_AT_PTR_OK_OR_FAIL(rledat_next);
            if (z < 4) {
              dirty_flag |= expandrow(
                  (uchar *)lptr, (uchar *)lptr_next, rledat, rledat_next, 3 - z);
            }
            else {
              break;
            }
          }
          lptr = lptr_next;
        }
      }
    }
    else { /* bpp == 2 */

      ibuf = IMB_allocImBuf(xsize, ysize, 32, (flags & IB_byte_data) | IB_float_data);
      if (!ibuf) {
        goto fail_rle;
      }

      fbase = ibuf->float_buffer.data;

      if (badorder) {
        for (size_t z = 0; z < zsize_read; z++) {
          fptr = fbase;
          for (size_t y = 0; y < ysize; y++) {
            MFILE_SEEK(inf, starttab[y + z * ysize]);
            rledat = MFILE_DATA(inf);
            MFILE_STEP(inf, lengthtab[y + z * ysize]);
            const uchar *rledat_next = MFILE_DATA(inf);
            MFILE_CAPACITY_AT_PTR_OK_OR_FAIL(rledat_next);
            float *fptr_next = fptr + (xsize * 4);
            dirty_flag |= expandrow2(fptr, fptr_next, rledat, rledat_next, 3 - z);
            fptr = fptr_next;
          }
        }
      }
      else {
        fptr = fbase;
        float *fptr_next = fptr + (xsize * 4);

        for (size_t y = 0; y < ysize; y++) {

          for (size_t z = 0; z < zsize_read; z++) {
            MFILE_SEEK(inf, starttab[y + z * ysize]);
            rledat = MFILE_DATA(inf);
            MFILE_STEP(inf, lengthtab[y + z * ysize]);
            const uchar *rledat_next = MFILE_DATA(inf);
            MFILE_CAPACITY_AT_PTR_OK_OR_FAIL(rledat_next);
            dirty_flag |= expandrow2(fptr, fptr_next, rledat, rledat_next, 3 - z);
          }
          fptr = fptr_next;
        }
      }
    }
#undef MFILE_CAPACITY_AT_PTR_OK_OR_FAIL
  fail_rle:
    MEM_freeN(starttab);
    MEM_freeN(lengthtab);

    if (!ibuf) {
      return nullptr;
    }
  }
  else {

#define MFILE_CAPACITY_AT_PTR_OK_OR_FAIL(p) \
  if (UNLIKELY((p) > mem_end)) { \
    dirty_flag |= DIRTY_FLAG_EOF; \
    goto fail_uncompressed; \
  } \
  ((void)0)

    if (bpp == 1) {

      ibuf = IMB_allocImBuf(xsize, ysize, 8 * zsize_read, IB_byte_data);
      if (!ibuf) {
        goto fail_uncompressed;
      }
      ibuf->planes = std::min<int>(ibuf->planes, 32);

      base = (uint *)ibuf->byte_buffer.data;

      MFILE_SEEK(inf, HEADER_SIZE);
      rledat = MFILE_DATA(inf);

      for (size_t z = 0; z < zsize_read; z++) {

        if (z < 4) {
          lptr = base;
        }
        else {
          break;
        }

        for (size_t y = 0; y < ysize; y++) {
          const uchar *rledat_next = rledat + xsize;
          const int z_ofs = 3 - z;
          MFILE_CAPACITY_AT_PTR_OK_OR_FAIL(rledat_next + z_ofs);
          interleaverow((uchar *)lptr, rledat, z_ofs, xsize);
          rledat = rledat_next;
          lptr += xsize;
        }
      }
    }
    else { /* bpp == 2 */

      ibuf = IMB_allocImBuf(xsize, ysize, 32, (flags & IB_byte_data) | IB_float_data);
      if (!ibuf) {
        goto fail_uncompressed;
      }

      fbase = ibuf->float_buffer.data;

      MFILE_SEEK(inf, HEADER_SIZE);
      rledat = MFILE_DATA(inf);

      for (size_t z = 0; z < zsize_read; z++) {

        fptr = fbase;

        for (size_t y = 0; y < ysize; y++) {
          const uchar *rledat_next = rledat + xsize * 2;
          const int z_ofs = 3 - z;
          MFILE_CAPACITY_AT_PTR_OK_OR_FAIL(rledat_next + z_ofs);
          interleaverow2(fptr, rledat, z_ofs, xsize);
          rledat = rledat_next;
          fptr += xsize * 4;
        }
      }
    }
#undef MFILE_CAPACITY_AT_PTR_OK_OR_FAIL
  fail_uncompressed:
    if (!ibuf) {
      return nullptr;
    }
  }

  if (bpp == 1) {
    uchar *rect;

    if (image.zsize == 1) {
      rect = ibuf->byte_buffer.data;
      for (size_t x = size_t(ibuf->x) * size_t(ibuf->y); x > 0; x--) {
        rect[0] = 255;
        rect[1] = rect[2] = rect[3];
        rect += 4;
      }
    }
    else if (image.zsize == 2) {
      /* Gray-scale with alpha. */
      rect = ibuf->byte_buffer.data;
      for (size_t x = size_t(ibuf->x) * size_t(ibuf->y); x > 0; x--) {
        rect[0] = rect[2];
        rect[1] = rect[2] = rect[3];
        rect += 4;
      }
    }
    else if (image.zsize == 3) {
      /* add alpha */
      rect = ibuf->byte_buffer.data;
      for (size_t x = size_t(ibuf->x) * size_t(ibuf->y); x > 0; x--) {
        rect[0] = 255;
        rect += 4;
      }
    }
  }
  else { /* bpp == 2 */

    if (image.zsize == 1) {
      fbase = ibuf->float_buffer.data;
      for (size_t x = size_t(ibuf->x) * size_t(ibuf->y); x > 0; x--) {
        fbase[0] = 1;
        fbase[1] = fbase[2] = fbase[3];
        fbase += 4;
      }
    }
    else if (image.zsize == 2) {
      /* Gray-scale with alpha. */
      fbase = ibuf->float_buffer.data;
      for (size_t x = size_t(ibuf->x) * size_t(ibuf->y); x > 0; x--) {
        fbase[0] = fbase[2];
        fbase[1] = fbase[2] = fbase[3];
        fbase += 4;
      }
    }
    else if (image.zsize == 3) {
      /* add alpha */
      fbase = ibuf->float_buffer.data;
      for (size_t x = size_t(ibuf->x) * size_t(ibuf->y); x > 0; x--) {
        fbase[0] = 1;
        fbase += 4;
      }
    }

    if (flags & IB_byte_data) {
      IMB_byte_from_float(ibuf);
    }
  }

  if (dirty_flag) {
    CLOG_ERROR(&LOG, "Corrupt file content (%d)", dirty_flag);
  }
  ibuf->ftype = IMB_FTYPE_IRIS;

  if (ibuf->byte_buffer.data) {
    imbuf_rgba_to_abgr(ibuf);
  }

  return ibuf;
}

/* Static utility functions for loading image data. */

static void interleaverow(uchar *lptr, const uchar *cptr, int z, int n)
{
  lptr += z;
  while (n--) {
    *lptr = *cptr++;
    lptr += 4;
  }
}

static void interleaverow2(float *lptr, const uchar *cptr, int z, int n)
{
  lptr += z;
  while (n--) {
    *lptr = ((cptr[0] << 8) | (cptr[1] << 0)) / float(0xFFFF);
    cptr += 2;
    lptr += 4;
  }
}

static int expandrow2(
    float *optr, const float *optr_end, const uchar *iptr, const uchar *iptr_end, int z)
{
  ushort pixel, count;
  float pixel_f;

#define EXPAND_CAPACITY_AT_INPUT_OK_OR_FAIL(iptr_next) \
  if (UNLIKELY(iptr_next > iptr_end)) { \
    goto fail; \
  } \
  ((void)0)

#define EXPAND_CAPACITY_AT_OUTPUT_OK_OR_FAIL(optr_next) \
  if (UNLIKELY(optr_next > optr_end)) { \
    goto fail; \
  } \
  ((void)0)

  optr += z;
  optr_end += z;
  while (true) {
    const uchar *iptr_next = iptr + 2;
    EXPAND_CAPACITY_AT_INPUT_OK_OR_FAIL(iptr_next);
    pixel = (iptr[0] << 8) | (iptr[1] << 0);
    iptr = iptr_next;

    if (!(count = (pixel & 0x7f))) {
      return false;
    }
    const float *optr_next = optr + count;
    EXPAND_CAPACITY_AT_OUTPUT_OK_OR_FAIL(optr_next);
    if (pixel & 0x80) {
      iptr_next = iptr + (count * 2);
      EXPAND_CAPACITY_AT_INPUT_OK_OR_FAIL(iptr_next);
      while (count >= 8) {
        optr[0 * 4] = ((iptr[0] << 8) | (iptr[1] << 0)) / float(0xFFFF);
        optr[1 * 4] = ((iptr[2] << 8) | (iptr[3] << 0)) / float(0xFFFF);
        optr[2 * 4] = ((iptr[4] << 8) | (iptr[5] << 0)) / float(0xFFFF);
        optr[3 * 4] = ((iptr[6] << 8) | (iptr[7] << 0)) / float(0xFFFF);
        optr[4 * 4] = ((iptr[8] << 8) | (iptr[9] << 0)) / float(0xFFFF);
        optr[5 * 4] = ((iptr[10] << 8) | (iptr[11] << 0)) / float(0xFFFF);
        optr[6 * 4] = ((iptr[12] << 8) | (iptr[13] << 0)) / float(0xFFFF);
        optr[7 * 4] = ((iptr[14] << 8) | (iptr[15] << 0)) / float(0xFFFF);
        optr += 8 * 4;
        iptr += 8 * 2;
        count -= 8;
      }
      while (count--) {
        *optr = ((iptr[0] << 8) | (iptr[1] << 0)) / float(0xFFFF);
        iptr += 2;
        optr += 4;
      }
      BLI_assert(iptr == iptr_next);
    }
    else {
      iptr_next = iptr + 2;
      EXPAND_CAPACITY_AT_INPUT_OK_OR_FAIL(iptr_next);
      pixel_f = ((iptr[0] << 8) | (iptr[1] << 0)) / float(0xFFFF);
      iptr = iptr_next;

      while (count >= 8) {
        optr[0 * 4] = pixel_f;
        optr[1 * 4] = pixel_f;
        optr[2 * 4] = pixel_f;
        optr[3 * 4] = pixel_f;
        optr[4 * 4] = pixel_f;
        optr[5 * 4] = pixel_f;
        optr[6 * 4] = pixel_f;
        optr[7 * 4] = pixel_f;
        optr += 8 * 4;
        count -= 8;
      }
      while (count--) {
        *optr = pixel_f;
        optr += 4;
      }
      BLI_assert(iptr == iptr_next);
    }
    BLI_assert(optr == optr_next);
  }
  return false;

#undef EXPAND_CAPACITY_AT_INPUT_OK_OR_FAIL
#undef EXPAND_CAPACITY_AT_OUTPUT_OK_OR_FAIL
fail:
  return DIRTY_FLAG_ENCODING;
}

static int expandrow(
    uchar *optr, const uchar *optr_end, const uchar *iptr, const uchar *iptr_end, int z)
{
  uchar pixel, count;

#define EXPAND_CAPACITY_AT_INPUT_OK_OR_FAIL(iptr_next) \
  if (UNLIKELY(iptr_next > iptr_end)) { \
    goto fail; \
  } \
  ((void)0)

#define EXPAND_CAPACITY_AT_OUTPUT_OK_OR_FAIL(optr_next) \
  if (UNLIKELY(optr_next > optr_end)) { \
    goto fail; \
  } \
  ((void)0)

  optr += z;
  optr_end += z;
  while (true) {
    const uchar *iptr_next = iptr + 1;
    EXPAND_CAPACITY_AT_INPUT_OK_OR_FAIL(iptr_next);
    pixel = *iptr;
    iptr = iptr_next;
    if (!(count = (pixel & 0x7f))) {
      return false;
    }
    const uchar *optr_next = optr + (int(count) * 4);
    EXPAND_CAPACITY_AT_OUTPUT_OK_OR_FAIL(optr_next);

    if (pixel & 0x80) {
      iptr_next = iptr + count;
      EXPAND_CAPACITY_AT_INPUT_OK_OR_FAIL(iptr_next);
      while (count >= 8) {
        optr[0 * 4] = iptr[0];
        optr[1 * 4] = iptr[1];
        optr[2 * 4] = iptr[2];
        optr[3 * 4] = iptr[3];
        optr[4 * 4] = iptr[4];
        optr[5 * 4] = iptr[5];
        optr[6 * 4] = iptr[6];
        optr[7 * 4] = iptr[7];
        optr += 8 * 4;
        iptr += 8;
        count -= 8;
      }
      while (count--) {
        *optr = *iptr++;
        optr += 4;
      }
      BLI_assert(iptr == iptr_next);
    }
    else {
      iptr_next = iptr + 1;
      EXPAND_CAPACITY_AT_INPUT_OK_OR_FAIL(iptr_next);
      pixel = *iptr++;
      while (count >= 8) {
        optr[0 * 4] = pixel;
        optr[1 * 4] = pixel;
        optr[2 * 4] = pixel;
        optr[3 * 4] = pixel;
        optr[4 * 4] = pixel;
        optr[5 * 4] = pixel;
        optr[6 * 4] = pixel;
        optr[7 * 4] = pixel;
        optr += 8 * 4;
        count -= 8;
      }
      while (count--) {
        *optr = pixel;
        optr += 4;
      }
      BLI_assert(iptr == iptr_next);
    }
    BLI_assert(optr == optr_next);
  }

  return false;

#undef EXPAND_CAPACITY_AT_INPUT_OK_OR_FAIL
#undef EXPAND_CAPACITY_AT_OUTPUT_OK_OR_FAIL
fail:
  return DIRTY_FLAG_ENCODING;
}

/**
 * \param filepath: The file path to write to.
 * \param lptr: an array of integers to an iris image file (each int represents one pixel).
 * \param zptr: depth-buffer (optional, may be nullptr).
 * \param xsize: with width of the pixel-array.
 * \param ysize: height of the pixel-array.
 * \param zsize: specifies what kind of image file to write out.
 * - 1: the luminance of the pixels are calculated,
 *      and a single channel black and white image is saved.
 * - 3: an RGB image file is saved.
 * - 4: an RGBA image file is saved.
 * - 8: an RGBA image and a Z-buffer (non-null `zptr`).
 */
static bool output_iris(const char *filepath,
                        const uint *lptr,
                        const int *zptr,
                        const int xsize,
                        const int ysize,
                        const int zsize)
{
  FILE *outf;
  IRIS_Header *image;
  int tablen, y, z, pos, len = 0;
  uint *starttab, *lengthtab;
  uchar *rlebuf;
  uint *lumbuf;
  int rlebuflen, goodwrite;

  goodwrite = 1;
  outf = BLI_fopen(filepath, "wb");
  if (!outf) {
    return false;
  }

  tablen = ysize * zsize * sizeof(int);

  image = MEM_mallocN<IRIS_Header>("iris image");
  starttab = MEM_malloc_arrayN<uint>(size_t(tablen), "iris starttab");
  lengthtab = MEM_malloc_arrayN<uint>(size_t(tablen), "iris lengthtab");
  rlebuflen = 1.05 * xsize + 10;
  rlebuf = MEM_malloc_arrayN<uchar>(size_t(rlebuflen), "iris rlebuf");
  lumbuf = MEM_malloc_arrayN<uint>(size_t(xsize), "iris lumbuf");

  memset(image, 0, sizeof(IRIS_Header));
  image->imagic = IRIS_MAGIC;
  image->type = RLE(1);
  if (zsize > 1) {
    image->dim = 3;
  }
  else {
    image->dim = 2;
  }
  image->xsize = xsize;
  image->ysize = ysize;
  image->zsize = zsize;
  image->min = 0;
  image->max = 255;
  goodwrite *= writeheader(outf, image);
  fseek(outf, HEADER_SIZE + (2 * tablen), SEEK_SET);
  pos = HEADER_SIZE + (2 * tablen);

  for (y = 0; y < ysize; y++) {
    for (z = 0; z < zsize; z++) {

      if (zsize == 1) {
        lumrow((const uchar *)lptr, (uchar *)lumbuf, xsize);
        len = compressrow((const uchar *)lumbuf, rlebuf, CHANOFFSET(z), xsize);
      }
      else {
        if (z < 4) {
          len = compressrow((const uchar *)lptr, rlebuf, CHANOFFSET(z), xsize);
        }
        else if (z < 8 && zptr) {
          len = compressrow((const uchar *)zptr, rlebuf, CHANOFFSET(z - 4), xsize);
        }
      }

      BLI_assert_msg(len <= rlebuflen, "The length calculated for 'rlebuflen' was too small!");

      goodwrite *= fwrite(rlebuf, len, 1, outf);
      starttab[y + z * ysize] = pos;
      lengthtab[y + z * ysize] = len;
      pos += len;
    }
    lptr += xsize;
    if (zptr) {
      zptr += xsize;
    }
  }

  fseek(outf, HEADER_SIZE, SEEK_SET);
  goodwrite *= writetab(outf, starttab, tablen);
  goodwrite *= writetab(outf, lengthtab, tablen);
  MEM_freeN(image);
  MEM_freeN(starttab);
  MEM_freeN(lengthtab);
  MEM_freeN(rlebuf);
  MEM_freeN(lumbuf);
  fclose(outf);
  if (goodwrite) {
    return true;
  }

  CLOG_ERROR(&LOG, "not enough space for image");
  return false;
}

/* static utility functions for output_iris */

static void lumrow(const uchar *rgbptr, uchar *lumptr, int n)
{
  lumptr += CHANOFFSET(0);
  while (n--) {
    *lumptr = ILUM(rgbptr[OFFSET_R], rgbptr[OFFSET_G], rgbptr[OFFSET_B]);
    lumptr += 4;
    rgbptr += 4;
  }
}

static int compressrow(const uchar *lbuf, uchar *rlebuf, const int z, const int row_len)
{
  const uchar *iptr, *ibufend, *sptr;
  uchar *optr;
  short todo, cc;
  int count;

  lbuf += z;
  iptr = lbuf;
  ibufend = iptr + row_len * 4;
  optr = rlebuf;

  while (iptr < ibufend) {
    sptr = iptr;
    iptr += 8;
    while ((iptr < ibufend) && ((iptr[-8] != iptr[-4]) || (iptr[-4] != iptr[0]))) {
      iptr += 4;
    }
    iptr -= 8;
    count = (iptr - sptr) / 4;
    while (count) {
      todo = count > 126 ? 126 : count;
      count -= todo;
      *optr++ = 0x80 | todo;
      while (todo > 8) {
        optr[0] = sptr[0 * 4];
        optr[1] = sptr[1 * 4];
        optr[2] = sptr[2 * 4];
        optr[3] = sptr[3 * 4];
        optr[4] = sptr[4 * 4];
        optr[5] = sptr[5 * 4];
        optr[6] = sptr[6 * 4];
        optr[7] = sptr[7 * 4];

        optr += 8;
        sptr += 8 * 4;
        todo -= 8;
      }
      while (todo--) {
        *optr++ = *sptr;
        sptr += 4;
      }
    }
    sptr = iptr;
    cc = *iptr;
    iptr += 4;
    while ((iptr < ibufend) && (*iptr == cc)) {
      iptr += 4;
    }
    count = (iptr - sptr) / 4;
    while (count) {
      todo = count > 126 ? 126 : count;
      count -= todo;
      *optr++ = todo;
      *optr++ = cc;
    }
  }
  *optr++ = 0;
  return optr - rlebuf;
}

bool imb_saveiris(ImBuf *ibuf, const char *filepath, int /*flags*/)
{
  const uint limit = std::numeric_limits<ushort>::max();
  if (ibuf->x > limit || ibuf->y > limit) {
    CLOG_ERROR(&LOG, "Image x/y exceeds %u", limit);
    return false;
  }

  const short zsize = (ibuf->planes + 7) >> 3;

  imbuf_rgba_to_abgr(ibuf);

  const bool ok = output_iris(
      filepath, (uint *)ibuf->byte_buffer.data, nullptr, ibuf->x, ibuf->y, zsize);

  /* restore! Quite clumsy, 2 times a switch... maybe better a malloc ? */
  imbuf_rgba_to_abgr(ibuf);

  return ok;
}
