/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spbuttons
 */

#pragma once

#include "BLI_bitmap.h"

#include "DNA_listBase.h"

#include "RNA_types.hh"

struct ARegionType;
struct ID;
struct SpaceProperties;
struct Tex;
struct bContext;
struct bContextDataResult;
struct bNode;
struct bNodeSocket;
struct bNodeTree;
struct wmOperatorType;

struct SpaceProperties_Runtime {
  /** For filtering properties displayed in the space. */
  char search_string[UI_MAX_NAME_STR];
  /**
   * Bit-field (in the same order as the tabs) for whether each tab has properties
   * that match the search filter. Only valid when #search_string is set.
   */
  BLI_bitmap *tab_search_results;
};

/* context data */

struct ButsContextPath {
  PointerRNA ptr[8];
  int len;
  int flag;
  int collection_ctx;
};

struct ButsTextureUser {
  ButsTextureUser *next, *prev;

  ID *id;

  PointerRNA ptr;
  PropertyRNA *prop;

  bNodeTree *ntree;
  bNode *node;
  bNodeSocket *socket;

  const char *category;
  int icon;
  const char *name;

  int index;
};

struct ButsContextTexture {
  ListBase users;

  struct Tex *texture;

  struct ButsTextureUser *user;
  int index;
};

/* internal exports only */

/* `buttons_context.cc` */

void buttons_context_compute(const bContext *C, SpaceProperties *sbuts);
int buttons_context(const bContext *C, const char *member, bContextDataResult *result);
void buttons_context_register(ARegionType *art);
ID *buttons_context_id_path(const bContext *C);

extern "C" const char *buttons_context_dir[]; /* doc access */

/* `buttons_texture.cc` */

void buttons_texture_context_compute(const bContext *C, SpaceProperties *sbuts);

/* `buttons_ops.cc` */

void BUTTONS_OT_start_filter(wmOperatorType *ot);
void BUTTONS_OT_clear_filter(wmOperatorType *ot);
void BUTTONS_OT_toggle_pin(wmOperatorType *ot);
void BUTTONS_OT_file_browse(wmOperatorType *ot);
/**
 * Second operator, only difference from #BUTTONS_OT_file_browse is #WM_FILESEL_DIRECTORY.
 */
void BUTTONS_OT_directory_browse(wmOperatorType *ot);
void BUTTONS_OT_context_menu(wmOperatorType *ot);
