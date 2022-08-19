/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. */

/** \file
 * \ingroup imbcineon
 */

#include "logImageCore.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "IMB_filetype.h"
#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "IMB_colormanagement.h"
#include "IMB_colormanagement_intern.h"

#include "BKE_global.h"

#include "MEM_guardedalloc.h"

static struct ImBuf *imb_load_dpx_cineon(const unsigned char *mem,
                                         size_t size,
                                         int use_cineon,
                                         int flags,
                                         char colorspace[IM_MAX_SPACE])
{
  ImBuf *ibuf;
  LogImageFile *image;
  int width, height, depth;

  colorspace_set_default_role(colorspace, IM_MAX_SPACE, COLOR_ROLE_DEFAULT_FLOAT);

  logImageSetVerbose((G.debug & G_DEBUG) ? 1 : 0);

  image = logImageOpenFromMemory(mem, size);

  if (image == NULL) {
    printf("DPX/Cineon: error opening image.\n");
    return NULL;
  }

  logImageGetSize(image, &width, &height, &depth);

  ibuf = IMB_allocImBuf(width, height, 32, IB_rectfloat | flags);
  if (ibuf == NULL) {
    logImageClose(image);
    return NULL;
  }

  if (!(flags & IB_test)) {
    if (logImageGetDataRGBA(image, ibuf->rect_float, 1) != 0) {
      logImageClose(image);
      IMB_freeImBuf(ibuf);
      return NULL;
    }
    IMB_flipy(ibuf);
  }

  logImageClose(image);
  ibuf->ftype = use_cineon ? IMB_FTYPE_CINEON : IMB_FTYPE_DPX;

  if (flags & IB_alphamode_detect) {
    ibuf->flags |= IB_alphamode_premul;
  }

  return ibuf;
}

static int imb_save_dpx_cineon(ImBuf *ibuf, const char *filepath, int use_cineon, int flags)
{
  LogImageFile *logImage;
  float *fbuf;
  float *fbuf_ptr;
  unsigned char *rect_ptr;
  int x, y, depth, bitspersample, rvalue;

  if (flags & IB_mem) {
    printf("DPX/Cineon: saving in memory is not supported.\n");
    return 0;
  }

  logImageSetVerbose((G.debug & G_DEBUG) ? 1 : 0);

  depth = (ibuf->planes + 7) >> 3;
  if (depth > 4 || depth < 3) {
    printf("DPX/Cineon: unsupported depth: %d for file: '%s'\n", depth, filepath);
    return 0;
  }

  if (ibuf->foptions.flag & CINEON_10BIT) {
    bitspersample = 10;
  }
  else if (ibuf->foptions.flag & CINEON_12BIT) {
    bitspersample = 12;
  }
  else if (ibuf->foptions.flag & CINEON_16BIT) {
    bitspersample = 16;
  }
  else {
    bitspersample = 8;
  }

  logImage = logImageCreate(filepath,
                            use_cineon,
                            ibuf->x,
                            ibuf->y,
                            bitspersample,
                            (depth == 4),
                            (ibuf->foptions.flag & CINEON_LOG),
                            -1,
                            -1,
                            -1,
                            "Blender");

  if (logImage == NULL) {
    printf("DPX/Cineon: error creating file.\n");
    return 0;
  }

  if (ibuf->rect_float != NULL && bitspersample != 8) {
    /* Don't use the float buffer to save 8 BPP picture to prevent color banding
     * (there's no dithering algorithm behind the #logImageSetDataRGBA function). */

    fbuf = (float *)MEM_mallocN(sizeof(float[4]) * ibuf->x * ibuf->y,
                                "fbuf in imb_save_dpx_cineon");

    for (y = 0; y < ibuf->y; y++) {
      float *dst_ptr = fbuf + 4 * ((ibuf->y - y - 1) * ibuf->x);
      float *src_ptr = ibuf->rect_float + 4 * (y * ibuf->x);

      memcpy(dst_ptr, src_ptr, 4 * ibuf->x * sizeof(float));
    }

    rvalue = (logImageSetDataRGBA(logImage, fbuf, 1) == 0);

    MEM_freeN(fbuf);
  }
  else {
    if (ibuf->rect == NULL) {
      IMB_rect_from_float(ibuf);
    }

    fbuf = (float *)MEM_mallocN(sizeof(float[4]) * ibuf->x * ibuf->y,
                                "fbuf in imb_save_dpx_cineon");
    if (fbuf == NULL) {
      printf("DPX/Cineon: error allocating memory.\n");
      logImageClose(logImage);
      return 0;
    }
    for (y = 0; y < ibuf->y; y++) {
      for (x = 0; x < ibuf->x; x++) {
        fbuf_ptr = fbuf + 4 * ((ibuf->y - y - 1) * ibuf->x + x);
        rect_ptr = (unsigned char *)ibuf->rect + 4 * (y * ibuf->x + x);
        fbuf_ptr[0] = (float)rect_ptr[0] / 255.0f;
        fbuf_ptr[1] = (float)rect_ptr[1] / 255.0f;
        fbuf_ptr[2] = (float)rect_ptr[2] / 255.0f;
        fbuf_ptr[3] = (depth == 4) ? ((float)rect_ptr[3] / 255.0f) : 1.0f;
      }
    }
    rvalue = (logImageSetDataRGBA(logImage, fbuf, 0) == 0);
    MEM_freeN(fbuf);
  }

  logImageClose(logImage);
  return rvalue;
}

bool imb_save_cineon(struct ImBuf *buf, const char *filepath, int flags)
{
  return imb_save_dpx_cineon(buf, filepath, 1, flags);
}

bool imb_is_a_cineon(const unsigned char *buf, size_t size)
{
  return logImageIsCineon(buf, size);
}

ImBuf *imb_load_cineon(const unsigned char *mem,
                       size_t size,
                       int flags,
                       char colorspace[IM_MAX_SPACE])
{
  if (!imb_is_a_cineon(mem, size)) {
    return NULL;
  }
  return imb_load_dpx_cineon(mem, size, 1, flags, colorspace);
}

bool imb_save_dpx(struct ImBuf *buf, const char *filepath, int flags)
{
  return imb_save_dpx_cineon(buf, filepath, 0, flags);
}

bool imb_is_a_dpx(const unsigned char *buf, size_t size)
{
  return logImageIsDpx(buf, size);
}

ImBuf *imb_load_dpx(const unsigned char *mem,
                    size_t size,
                    int flags,
                    char colorspace[IM_MAX_SPACE])
{
  if (!imb_is_a_dpx(mem, size)) {
    return NULL;
  }
  return imb_load_dpx_cineon(mem, size, 0, flags, colorspace);
}
