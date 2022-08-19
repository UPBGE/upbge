/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup imbuf
 */

#ifdef _WIN32
#  include <io.h>
#  include <stddef.h>
#  include <sys/types.h>
#endif

#include "BLI_fileops.h"
#include "BLI_mmap.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include <stdlib.h>

#include "IMB_allocimbuf.h"
#include "IMB_filetype.h"
#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"
#include "IMB_metadata.h"
#include "IMB_thumbs.h"
#include "imbuf.h"

#include "IMB_colormanagement.h"
#include "IMB_colormanagement_intern.h"

static void imb_handle_alpha(ImBuf *ibuf,
                             int flags,
                             char colorspace[IM_MAX_SPACE],
                             char effective_colorspace[IM_MAX_SPACE])
{
  if (colorspace) {
    if (ibuf->rect != NULL && ibuf->rect_float == NULL) {
      /* byte buffer is never internally converted to some standard space,
       * store pointer to its color space descriptor instead
       */
      ibuf->rect_colorspace = colormanage_colorspace_get_named(effective_colorspace);
    }

    BLI_strncpy(colorspace, effective_colorspace, IM_MAX_SPACE);
  }

  bool is_data = (colorspace && IMB_colormanagement_space_name_is_data(colorspace));
  int alpha_flags = (flags & IB_alphamode_detect) ? ibuf->flags : flags;

  if (is_data || (flags & IB_alphamode_channel_packed)) {
    /* Don't touch alpha. */
    ibuf->flags |= IB_alphamode_channel_packed;
  }
  else if (flags & IB_alphamode_ignore) {
    /* Make opaque. */
    IMB_rectfill_alpha(ibuf, 1.0f);
    ibuf->flags |= IB_alphamode_ignore;
  }
  else {
    if (alpha_flags & IB_alphamode_premul) {
      if (ibuf->rect) {
        IMB_unpremultiply_alpha(ibuf);
      }
      else {
        /* pass, floats are expected to be premul */
      }
    }
    else {
      if (ibuf->rect_float) {
        IMB_premultiply_alpha(ibuf);
      }
      else {
        /* pass, bytes are expected to be straight */
      }
    }
  }

  /* OCIO_TODO: in some cases it's faster to do threaded conversion,
   *            but how to distinguish such cases */
  colormanage_imbuf_make_linear(ibuf, effective_colorspace);
}

ImBuf *IMB_ibImageFromMemory(const unsigned char *mem,
                             size_t size,
                             int flags,
                             char colorspace[IM_MAX_SPACE],
                             const char *descr)
{
  ImBuf *ibuf;
  const ImFileType *type;
  char effective_colorspace[IM_MAX_SPACE] = "";

  if (mem == NULL) {
    fprintf(stderr, "%s: NULL pointer\n", __func__);
    return NULL;
  }

  if (colorspace) {
    BLI_strncpy(effective_colorspace, colorspace, sizeof(effective_colorspace));
  }

  for (type = IMB_FILE_TYPES; type < IMB_FILE_TYPES_LAST; type++) {
    if (type->load) {
      ibuf = type->load(mem, size, flags, effective_colorspace);
      if (ibuf) {
        imb_handle_alpha(ibuf, flags, colorspace, effective_colorspace);
        return ibuf;
      }
    }
  }

  if ((flags & IB_test) == 0) {
    fprintf(stderr, "%s: unknown file-format (%s)\n", __func__, descr);
  }

  return NULL;
}

static ImBuf *IMB_ibImageFromFile(const char *filepath,
                                  int flags,
                                  char colorspace[IM_MAX_SPACE],
                                  const char *descr)
{
  ImBuf *ibuf;
  const ImFileType *type;
  char effective_colorspace[IM_MAX_SPACE] = "";

  if (colorspace) {
    BLI_strncpy(effective_colorspace, colorspace, sizeof(effective_colorspace));
  }

  for (type = IMB_FILE_TYPES; type < IMB_FILE_TYPES_LAST; type++) {
    if (type->load_filepath) {
      ibuf = type->load_filepath(filepath, flags, effective_colorspace);
      if (ibuf) {
        imb_handle_alpha(ibuf, flags, colorspace, effective_colorspace);
        return ibuf;
      }
    }
  }

  if ((flags & IB_test) == 0) {
    fprintf(stderr, "%s: unknown fileformat (%s)\n", __func__, descr);
  }

  return NULL;
}

static bool imb_is_filepath_format(const char *filepath)
{
  /* return true if this is one of the formats that can't be loaded from memory */
  return BLI_path_extension_check_array(filepath, imb_ext_image_filepath_only);
}

ImBuf *IMB_loadifffile(
    int file, const char *filepath, int flags, char colorspace[IM_MAX_SPACE], const char *descr)
{
  ImBuf *ibuf;
  unsigned char *mem;
  size_t size;

  if (file == -1) {
    return NULL;
  }

  if (imb_is_filepath_format(filepath)) {
    return IMB_ibImageFromFile(filepath, flags, colorspace, descr);
  }

  size = BLI_file_descriptor_size(file);

  imb_mmap_lock();
  BLI_mmap_file *mmap_file = BLI_mmap_open(file);
  imb_mmap_unlock();
  if (mmap_file == NULL) {
    fprintf(stderr, "%s: couldn't get mapping %s\n", __func__, descr);
    return NULL;
  }

  mem = BLI_mmap_get_pointer(mmap_file);

  ibuf = IMB_ibImageFromMemory(mem, size, flags, colorspace, descr);

  imb_mmap_lock();
  BLI_mmap_free(mmap_file);
  imb_mmap_unlock();

  return ibuf;
}

static void imb_cache_filename(char *filepath, const char *name, int flags)
{
  /* read .tx instead if it exists and is not older */
  if (flags & IB_tilecache) {
    BLI_strncpy(filepath, name, IMB_FILENAME_SIZE);
    if (!BLI_path_extension_replace(filepath, IMB_FILENAME_SIZE, ".tx")) {
      return;
    }

    if (BLI_file_older(name, filepath)) {
      return;
    }
  }

  BLI_strncpy(filepath, name, IMB_FILENAME_SIZE);
}

ImBuf *IMB_loadiffname(const char *filepath, int flags, char colorspace[IM_MAX_SPACE])
{
  ImBuf *ibuf;
  int file, a;
  char filepath_tx[IMB_FILENAME_SIZE];

  BLI_assert(!BLI_path_is_rel(filepath));

  imb_cache_filename(filepath_tx, filepath, flags);

  file = BLI_open(filepath_tx, O_BINARY | O_RDONLY, 0);
  if (file == -1) {
    return NULL;
  }

  ibuf = IMB_loadifffile(file, filepath, flags, colorspace, filepath_tx);

  if (ibuf) {
    BLI_strncpy(ibuf->name, filepath, sizeof(ibuf->name));
    BLI_strncpy(ibuf->cachename, filepath_tx, sizeof(ibuf->cachename));
    for (a = 1; a < ibuf->miptot; a++) {
      BLI_strncpy(ibuf->mipmap[a - 1]->cachename, filepath_tx, sizeof(ibuf->cachename));
    }
  }

  close(file);

  return ibuf;
}

struct ImBuf *IMB_thumb_load_image(const char *filepath,
                                   size_t max_thumb_size,
                                   char colorspace[IM_MAX_SPACE])
{
  const ImFileType *type = IMB_file_type_from_ftype(IMB_ispic_type(filepath));
  if (type == NULL) {
    return NULL;
  }

  ImBuf *ibuf = NULL;
  int flags = IB_rect | IB_metadata;
  /* Size of the original image. */
  size_t width = 0;
  size_t height = 0;

  char effective_colorspace[IM_MAX_SPACE] = "";
  if (colorspace) {
    BLI_strncpy(effective_colorspace, colorspace, sizeof(effective_colorspace));
  }

  if (type->load_filepath_thumbnail) {
    ibuf = type->load_filepath_thumbnail(
        filepath, flags, max_thumb_size, colorspace, &width, &height);
  }
  else {
    /* Skip images of other types if over 100MB. */
    const size_t file_size = BLI_file_size(filepath);
    if (file_size != -1 && file_size > THUMB_SIZE_MAX) {
      return NULL;
    }
    ibuf = IMB_loadiffname(filepath, flags, colorspace);
    if (ibuf) {
      width = ibuf->x;
      height = ibuf->y;
    }
  }

  if (ibuf) {
    imb_handle_alpha(ibuf, flags, colorspace, effective_colorspace);

    if (width > 0 && height > 0) {
      /* Save dimensions of original image into the thumbnail metadata. */
      char cwidth[40];
      char cheight[40];
      SNPRINTF(cwidth, "%zu", width);
      SNPRINTF(cheight, "%zu", height);
      IMB_metadata_ensure(&ibuf->metadata);
      IMB_metadata_set_field(ibuf->metadata, "Thumb::Image::Width", cwidth);
      IMB_metadata_set_field(ibuf->metadata, "Thumb::Image::Height", cheight);
    }
  }

  return ibuf;
}

ImBuf *IMB_testiffname(const char *filepath, int flags)
{
  ImBuf *ibuf;
  int file;
  char filepath_tx[IMB_FILENAME_SIZE];
  char colorspace[IM_MAX_SPACE] = "\0";

  BLI_assert(!BLI_path_is_rel(filepath));

  imb_cache_filename(filepath_tx, filepath, flags);

  file = BLI_open(filepath_tx, O_BINARY | O_RDONLY, 0);
  if (file == -1) {
    return NULL;
  }

  ibuf = IMB_loadifffile(file, filepath, flags | IB_test | IB_multilayer, colorspace, filepath_tx);

  if (ibuf) {
    BLI_strncpy(ibuf->name, filepath, sizeof(ibuf->name));
    BLI_strncpy(ibuf->cachename, filepath_tx, sizeof(ibuf->cachename));
  }

  close(file);

  return ibuf;
}

static void imb_loadtilefile(ImBuf *ibuf, int file, int tx, int ty, unsigned int *rect)
{
  unsigned char *mem;
  size_t size;

  if (file == -1) {
    return;
  }

  size = BLI_file_descriptor_size(file);

  imb_mmap_lock();
  BLI_mmap_file *mmap_file = BLI_mmap_open(file);
  imb_mmap_unlock();
  if (mmap_file == NULL) {
    fprintf(stderr, "Couldn't get memory mapping for %s\n", ibuf->cachename);
    return;
  }

  mem = BLI_mmap_get_pointer(mmap_file);

  const ImFileType *type = IMB_file_type_from_ibuf(ibuf);
  if (type != NULL) {
    if (type->load_tile != NULL) {
      type->load_tile(ibuf, mem, size, tx, ty, rect);
    }
  }

  imb_mmap_lock();
  BLI_mmap_free(mmap_file);
  imb_mmap_unlock();
}

void imb_loadtile(ImBuf *ibuf, int tx, int ty, unsigned int *rect)
{
  int file;

  file = BLI_open(ibuf->cachename, O_BINARY | O_RDONLY, 0);
  if (file == -1) {
    return;
  }

  imb_loadtilefile(ibuf, file, tx, ty, rect);

  close(file);
}
