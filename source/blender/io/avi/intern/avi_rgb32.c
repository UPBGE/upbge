/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup avi
 *
 * This is external code. Converts between rgb32 and avi.
 */

#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "IMB_imbuf.h"

#include "AVI_avi.h"
#include "avi_rgb32.h"

void *avi_converter_from_rgb32(AviMovie *movie, int stream, unsigned char *buffer, size_t *size)
{
  unsigned char *buf;

  (void)stream; /* unused */

  *size = (size_t)movie->header->Height * (size_t)movie->header->Width * 3;
  buf = imb_alloc_pixels(
      movie->header->Height, movie->header->Width, 3, sizeof(unsigned char), "fromrgb32buf");
  if (!buf) {
    return NULL;
  }

  size_t rowstridea = movie->header->Width * 3;
  size_t rowstrideb = movie->header->Width * 4;

  for (size_t y = 0; y < movie->header->Height; y++) {
    for (size_t x = 0; x < movie->header->Width; x++) {
      buf[y * rowstridea + x * 3 + 0] = buffer[y * rowstrideb + x * 4 + 3];
      buf[y * rowstridea + x * 3 + 1] = buffer[y * rowstrideb + x * 4 + 2];
      buf[y * rowstridea + x * 3 + 2] = buffer[y * rowstrideb + x * 4 + 1];
    }
  }

  MEM_freeN(buffer);

  return buf;
}

void *avi_converter_to_rgb32(AviMovie *movie, int stream, unsigned char *buffer, size_t *size)
{
  unsigned char *buf;
  unsigned char *to, *from;

  (void)stream; /* unused */

  *size = (size_t)movie->header->Height * (size_t)movie->header->Width * 4;
  buf = imb_alloc_pixels(
      movie->header->Height, movie->header->Width, 4, sizeof(unsigned char), "torgb32buf");
  if (!buf) {
    return NULL;
  }

  memset(buf, 255, *size);

  to = buf;
  from = buffer;
  size_t i = (size_t)movie->header->Height * (size_t)movie->header->Width;

  while (i--) {
    memcpy(to, from, 3);
    to += 4;
    from += 3;
  }

  MEM_freeN(buffer);

  return buf;
}
