/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#pragma once

#include "IMB_imbuf.h"

/* -------------------------------------------------------------------- */
/** \name Generic File Type
 * \{ */

struct ImBuf;

#define IM_FTYPE_FLOAT 1

typedef struct ImFileType {
  /** Optional, called once when initializing. */
  void (*init)(void);
  /** Optional, called once when exiting. */
  void (*exit)(void);

  /**
   * Check if the data matches this file types 'magic',
   * \note that this may only read in a small part of the files header,
   * see: #IMB_ispic_type for details.
   */
  bool (*is_a)(const unsigned char *buf, size_t size);

  /** Load an image from memory. */
  struct ImBuf *(*load)(const unsigned char *mem,
                        size_t size,
                        int flags,
                        char colorspace[IM_MAX_SPACE]);
  /** Load an image from a file. */
  struct ImBuf *(*load_filepath)(const char *filepath, int flags, char colorspace[IM_MAX_SPACE]);
  /**
   * Load/Create a thumbnail image from a filepath. `max_thumb_size` is maximum size of either
   * dimension, so can return less on either or both. Should, if possible and performant, return
   * dimensions of the full-size image in r_width & r_height.
   */
  struct ImBuf *(*load_filepath_thumbnail)(const char *filepath,
                                           int flags,
                                           size_t max_thumb_size,
                                           char colorspace[IM_MAX_SPACE],
                                           size_t *r_width,
                                           size_t *r_height);
  /** Save to a file (or memory if #IB_mem is set in `flags` and the format supports it). */
  bool (*save)(struct ImBuf *ibuf, const char *filepath, int flags);
  void (*load_tile)(struct ImBuf *ibuf,
                    const unsigned char *mem,
                    size_t size,
                    int tx,
                    int ty,
                    unsigned int *rect);

  int flag;

  /** #eImbFileType */
  int filetype;

  int default_save_role;
} ImFileType;

extern const ImFileType IMB_FILE_TYPES[];
extern const ImFileType *IMB_FILE_TYPES_LAST;

const ImFileType *IMB_file_type_from_ftype(int ftype);
const ImFileType *IMB_file_type_from_ibuf(const struct ImBuf *ibuf);

void imb_filetypes_init(void);
void imb_filetypes_exit(void);

void imb_tile_cache_init(void);
void imb_tile_cache_exit(void);

void imb_loadtile(struct ImBuf *ibuf, int tx, int ty, unsigned int *rect);
/**
 * External free.
 */
void imb_tile_cache_tile_free(struct ImBuf *ibuf, int tx, int ty);

/** \} */

/* Type Specific Functions */

/* -------------------------------------------------------------------- */
/** \name Format: PNG (#IMB_FTYPE_PNG)
 * \{ */

bool imb_is_a_png(const unsigned char *mem, size_t size);
struct ImBuf *imb_loadpng(const unsigned char *mem,
                          size_t size,
                          int flags,
                          char colorspace[IM_MAX_SPACE]);
bool imb_savepng(struct ImBuf *ibuf, const char *filepath, int flags);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Format: TARGA (#IMB_FTYPE_TGA)
 * \{ */

bool imb_is_a_targa(const unsigned char *buf, size_t size);
struct ImBuf *imb_loadtarga(const unsigned char *mem,
                            size_t size,
                            int flags,
                            char colorspace[IM_MAX_SPACE]);
bool imb_savetarga(struct ImBuf *ibuf, const char *filepath, int flags);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Format: IRIS (#IMB_FTYPE_IMAGIC)
 * \{ */

bool imb_is_a_iris(const unsigned char *mem, size_t size);
/**
 * Read in a B/W RGB or RGBA iris image file and return an image buffer.
 */
struct ImBuf *imb_loadiris(const unsigned char *mem,
                           size_t size,
                           int flags,
                           char colorspace[IM_MAX_SPACE]);
bool imb_saveiris(struct ImBuf *ibuf, const char *filepath, int flags);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Format: JP2 (#IMB_FTYPE_JP2)
 * \{ */

bool imb_is_a_jp2(const unsigned char *buf, size_t size);
struct ImBuf *imb_load_jp2(const unsigned char *mem,
                           size_t size,
                           int flags,
                           char colorspace[IM_MAX_SPACE]);
struct ImBuf *imb_load_jp2_filepath(const char *filepath,
                                    int flags,
                                    char colorspace[IM_MAX_SPACE]);
bool imb_save_jp2(struct ImBuf *ibuf, const char *filepath, int flags);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Format: JPEG (#IMB_FTYPE_JPG)
 * \{ */

bool imb_is_a_jpeg(const unsigned char *mem, size_t size);
bool imb_savejpeg(struct ImBuf *ibuf, const char *filepath, int flags);
struct ImBuf *imb_load_jpeg(const unsigned char *buffer,
                            size_t size,
                            int flags,
                            char colorspace[IM_MAX_SPACE]);
struct ImBuf *imb_thumbnail_jpeg(const char *filepath,
                                 int flags,
                                 size_t max_thumb_size,
                                 char colorspace[IM_MAX_SPACE],
                                 size_t *r_width,
                                 size_t *r_height);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Format: BMP (#IMB_FTYPE_BMP)
 * \{ */

bool imb_is_a_bmp(const unsigned char *buf, size_t size);
struct ImBuf *imb_bmp_decode(const unsigned char *mem,
                             size_t size,
                             int flags,
                             char colorspace[IM_MAX_SPACE]);
/* Found write info at http://users.ece.gatech.edu/~slabaugh/personal/c/bitmapUnix.c */
bool imb_savebmp(struct ImBuf *ibuf, const char *filepath, int flags);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Format: CINEON (#IMB_FTYPE_CINEON)
 * \{ */

bool imb_is_a_cineon(const unsigned char *buf, size_t size);
bool imb_save_cineon(struct ImBuf *buf, const char *filepath, int flags);
struct ImBuf *imb_load_cineon(const unsigned char *mem,
                              size_t size,
                              int flags,
                              char colorspace[IM_MAX_SPACE]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Format: DPX (#IMB_FTYPE_DPX)
 * \{ */

bool imb_is_a_dpx(const unsigned char *buf, size_t size);
bool imb_save_dpx(struct ImBuf *buf, const char *filepath, int flags);
struct ImBuf *imb_load_dpx(const unsigned char *mem,
                           size_t size,
                           int flags,
                           char colorspace[IM_MAX_SPACE]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Format: HDR (#IMB_FTYPE_RADHDR)
 * \{ */

bool imb_is_a_hdr(const unsigned char *buf, size_t size);
struct ImBuf *imb_loadhdr(const unsigned char *mem,
                          size_t size,
                          int flags,
                          char colorspace[IM_MAX_SPACE]);
bool imb_savehdr(struct ImBuf *ibuf, const char *filepath, int flags);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Format: TIFF (#IMB_FTYPE_TIF)
 * \{ */

void imb_inittiff(void);
bool imb_is_a_tiff(const unsigned char *buf, size_t size);
/**
 * Loads a TIFF file.
 * \param mem: Memory containing the TIFF file.
 * \param size: Size of the mem buffer.
 * \param flags: If flags has IB_test set then the file is not actually loaded,
 * but all other operations take place.
 *
 * \return A newly allocated #ImBuf structure if successful, otherwise NULL.
 */
struct ImBuf *imb_loadtiff(const unsigned char *mem,
                           size_t size,
                           int flags,
                           char colorspace[IM_MAX_SPACE]);
void imb_loadtiletiff(
    struct ImBuf *ibuf, const unsigned char *mem, size_t size, int tx, int ty, unsigned int *rect);
/**
 * Saves a TIFF file.
 *
 * #ImBuf structures with 1, 3 or 4 bytes per pixel (GRAY, RGB, RGBA respectively)
 * are accepted, and interpreted correctly. Note that the TIFF convention is to use
 * pre-multiplied alpha, which can be achieved within Blender by setting `premul` alpha handling.
 * Other alpha conventions are not strictly correct, but are permitted anyhow.
 *
 * \param ibuf: Image buffer.
 * \param filepath: Name of the TIFF file to create.
 * \param flags: Currently largely ignored.
 *
 * \return 1 if the function is successful, 0 on failure.
 */
bool imb_savetiff(struct ImBuf *ibuf, const char *filepath, int flags);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Format: TIFF (#IMB_FTYPE_WEBP)
 * \{ */

bool imb_is_a_webp(const unsigned char *buf, size_t size);
struct ImBuf *imb_loadwebp(const unsigned char *mem,
                           size_t size,
                           int flags,
                           char colorspace[IM_MAX_SPACE]);
bool imb_savewebp(struct ImBuf *ibuf, const char *name, int flags);

/** \} */
