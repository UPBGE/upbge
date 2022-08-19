/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup blf
 *
 * Manage search paths for font files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>

#include FT_FREETYPE_H
#include FT_GLYPH_H

#include "MEM_guardedalloc.h"

#include "DNA_vec_types.h"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "BLF_api.h"
#include "blf_internal.h"
#include "blf_internal_types.h"

static ListBase global_font_dir = {NULL, NULL};

static DirBLF *blf_dir_find(const char *path)
{
  DirBLF *p;

  p = global_font_dir.first;
  while (p) {
    if (BLI_path_cmp(p->path, path) == 0) {
      return p;
    }
    p = p->next;
  }
  return NULL;
}

void BLF_dir_add(const char *path)
{
  DirBLF *dir;

  dir = blf_dir_find(path);
  if (dir) { /* already in the list ? just return. */
    return;
  }

  dir = (DirBLF *)MEM_callocN(sizeof(DirBLF), "BLF_dir_add");
  dir->path = BLI_strdup(path);
  BLI_addhead(&global_font_dir, dir);
}

void BLF_dir_rem(const char *path)
{
  DirBLF *dir;

  dir = blf_dir_find(path);
  if (dir) {
    BLI_remlink(&global_font_dir, dir);
    MEM_freeN(dir->path);
    MEM_freeN(dir);
  }
}

char **BLF_dir_get(int *ndir)
{
  DirBLF *p;
  char **dirs;
  char *path;
  int i, count;

  count = BLI_listbase_count(&global_font_dir);
  if (!count) {
    return NULL;
  }

  dirs = (char **)MEM_callocN(sizeof(char *) * count, "BLF_dir_get");
  p = global_font_dir.first;
  i = 0;
  while (p) {
    path = BLI_strdup(p->path);
    dirs[i] = path;
    p = p->next;
  }
  *ndir = i;
  return dirs;
}

void BLF_dir_free(char **dirs, int count)
{
  for (int i = 0; i < count; i++) {
    char *path = dirs[i];
    MEM_freeN(path);
  }
  MEM_freeN(dirs);
}

char *blf_dir_search(const char *file)
{
  BLI_assert_msg(!BLI_path_is_rel(file), "Relative paths must always be expanded!");

  DirBLF *dir;
  char full_path[FILE_MAX];
  char *s = NULL;

  for (dir = global_font_dir.first; dir; dir = dir->next) {
    BLI_join_dirfile(full_path, sizeof(full_path), dir->path, file);
    if (BLI_exists(full_path)) {
      s = BLI_strdup(full_path);
      break;
    }
  }

  if (!s) {
    /* This may be an absolute path which exists. */
    if (BLI_exists(file)) {
      s = BLI_strdup(file);
    }
  }

  return s;
}

char *blf_dir_metrics_search(const char *filepath)
{
  char *mfile;
  char *s;

  mfile = BLI_strdup(filepath);
  s = strrchr(mfile, '.');
  if (s) {
    if (BLI_strnlen(s, 4) < 4) {
      MEM_freeN(mfile);
      return NULL;
    }
    s++;
    s[0] = 'a';
    s[1] = 'f';
    s[2] = 'm';

    /* First check `.afm`. */
    if (BLI_exists(mfile)) {
      return mfile;
    }

    /* And now check `.pfm`. */
    s[0] = 'p';

    if (BLI_exists(mfile)) {
      return mfile;
    }
  }
  MEM_freeN(mfile);
  return NULL;
}
