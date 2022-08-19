/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Posix compatibility functions for windows dealing with DIR
 * (opendir, readdir, closedir)
 */

#ifdef WIN32

/* Standalone for inclusion in binaries other than Blender. */
#  ifdef USE_STANDALONE
#    define MEM_mallocN(size, str) ((void)str, malloc(size))
#    define MEM_callocN(size, str) ((void)str, calloc(size, 1))
#    define MEM_freeN(ptr) free(ptr)
#  else
#    include "MEM_guardedalloc.h"
#  endif

#  define WIN32_SKIP_HKEY_PROTECTION /* Need to use `HKEY`. */
#  include "BLI_utildefines.h"
#  include "BLI_winstuff.h"
#  include "utfconv.h"

#  define PATH_SUFFIX "\\*"
#  define PATH_SUFFIX_LEN 2

/* keep local to this file */
struct __dirstream {
  HANDLE handle;
  WIN32_FIND_DATAW data;
  char path[MAX_PATH + PATH_SUFFIX_LEN];
  long dd_loc;
  long dd_size;
  char dd_buf[4096];
  void *dd_direct;

  struct dirent direntry;
};

/**
 * \note MinGW (FREE_WINDOWS) has #opendir() and #_wopendir(), and only the
 * latter accepts a path name of #wchar_t type. Rather than messing up with
 * extra #ifdef's here and there, Blender's own implementations of #opendir()
 * and related functions are used to properly support paths with non-ASCII
 * characters. (kjym3)
 */

DIR *opendir(const char *path)
{
  wchar_t *path_16 = alloc_utf16_from_8(path, 0);
  int path_len;
  DIR *newd = NULL;

  if ((GetFileAttributesW(path_16) & FILE_ATTRIBUTE_DIRECTORY) &&
      ((path_len = strlen(path)) < (sizeof(newd->path) - PATH_SUFFIX_LEN))) {
    newd = MEM_mallocN(sizeof(DIR), "opendir");
    newd->handle = INVALID_HANDLE_VALUE;
    memcpy(newd->path, path, path_len);
    memcpy(newd->path + path_len, PATH_SUFFIX, PATH_SUFFIX_LEN + 1);

    newd->direntry.d_ino = 0;
    newd->direntry.d_off = 0;
    newd->direntry.d_reclen = 0;
    newd->direntry.d_name = NULL;
  }

  free(path_16);
  return newd;
}

static char *BLI_alloc_utf_8_from_16(wchar_t *in16, size_t add)
{
  size_t bsize = count_utf_8_from_16(in16);
  char *out8 = NULL;
  if (!bsize) {
    return NULL;
  }
  out8 = (char *)MEM_mallocN(sizeof(char) * (bsize + add), "UTF-8 String");
  conv_utf_16_to_8(in16, out8, bsize);
  return out8;
}

static wchar_t *UNUSED_FUNCTION(BLI_alloc_utf16_from_8)(char *in8, size_t add)
{
  size_t bsize = count_utf_16_from_8(in8);
  wchar_t *out16 = NULL;
  if (!bsize) {
    return NULL;
  }
  out16 = (wchar_t *)MEM_mallocN(sizeof(wchar_t) * (bsize + add), "UTF-16 String");
  conv_utf_8_to_16(in8, out16, bsize);
  return out16;
}

struct dirent *readdir(DIR *dp)
{
  if (dp->direntry.d_name) {
    MEM_freeN(dp->direntry.d_name);
    dp->direntry.d_name = NULL;
  }

  if (dp->handle == INVALID_HANDLE_VALUE) {
    wchar_t *path_16 = alloc_utf16_from_8(dp->path, 0);
    dp->handle = FindFirstFileW(path_16, &(dp->data));
    free(path_16);
    if (dp->handle == INVALID_HANDLE_VALUE) {
      return NULL;
    }

    dp->direntry.d_name = BLI_alloc_utf_8_from_16(dp->data.cFileName, 0);

    return &dp->direntry;
  }
  else if (FindNextFileW(dp->handle, &(dp->data))) {
    dp->direntry.d_name = BLI_alloc_utf_8_from_16(dp->data.cFileName, 0);

    return &dp->direntry;
  }
  else {
    return NULL;
  }
}

int closedir(DIR *dp)
{
  if (dp->direntry.d_name) {
    MEM_freeN(dp->direntry.d_name);
  }
  if (dp->handle != INVALID_HANDLE_VALUE) {
    FindClose(dp->handle);
  }

  MEM_freeN(dp);

  return 0;
}

/* End of copied part */

#else

/* intentionally empty for UNIX */

#endif
