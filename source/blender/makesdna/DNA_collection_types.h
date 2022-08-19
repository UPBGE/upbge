/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup DNA
 *
 * \brief Object groups, one object can be in many groups at once.
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_defs.h"
#include "DNA_listBase.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Collection;
struct Object;

typedef struct CollectionObject {
  struct CollectionObject *next, *prev;
  struct Object *ob;
} CollectionObject;

typedef struct CollectionChild {
  struct CollectionChild *next, *prev;
  struct Collection *collection;
} CollectionChild;

enum eCollectionLineArt_Usage {
  COLLECTION_LRT_INCLUDE = 0,
  COLLECTION_LRT_OCCLUSION_ONLY = (1 << 0),
  COLLECTION_LRT_EXCLUDE = (1 << 1),
  COLLECTION_LRT_INTERSECTION_ONLY = (1 << 2),
  COLLECTION_LRT_NO_INTERSECTION = (1 << 3),
};

enum eCollectionLineArt_Flags {
  COLLECTION_LRT_USE_INTERSECTION_MASK = (1 << 0),
  COLLECTION_LRT_USE_INTERSECTION_PRIORITY = (1 << 1),
};

typedef struct Collection {
  ID id;

  /** CollectionObject. */
  ListBase gobject;
  /** CollectionChild. */
  ListBase children;

  struct PreviewImage *preview;

  unsigned int layer DNA_DEPRECATED;
  float instance_offset[3];

  short flag;
  /* Runtime-only, always cleared on file load. */
  short tag;

  short lineart_usage;         /* eCollectionLineArt_Usage */
  unsigned char lineart_flags; /* eCollectionLineArt_Flags */
  unsigned char lineart_intersection_mask;
  unsigned char lineart_intersection_priority;
  char _pad[5];

  int16_t color_tag;

  /* Runtime. Cache of objects in this collection and all its
   * children. This is created on demand when e.g. some physics
   * simulation needs it, we don't want to have it for every
   * collections due to memory usage reasons. */
  ListBase object_cache;

  /* Need this for line art sub-collection selections. */
  ListBase object_cache_instanced;

  /* Runtime. List of collections that are a parent of this
   * datablock. */
  ListBase parents;

  /* Deprecated */
  struct SceneCollection *collection DNA_DEPRECATED;
  struct ViewLayer *view_layer DNA_DEPRECATED;
} Collection;

/* Collection->flag */
enum {
  COLLECTION_HIDE_VIEWPORT = (1 << 0),             /* Disable in viewports. */
  COLLECTION_HIDE_SELECT = (1 << 1),               /* Not selectable in viewport. */
  /* COLLECTION_DISABLED_DEPRECATED = (1 << 2), */ /* Not used anymore */
  COLLECTION_HIDE_RENDER = (1 << 3),               /* Disable in renders. */
  COLLECTION_HAS_OBJECT_CACHE = (1 << 4),          /* Runtime: object_cache is populated. */
  COLLECTION_IS_MASTER = (1 << 5), /* Is master collection embedded in the scene. */
  COLLECTION_HAS_OBJECT_CACHE_INSTANCED = (1 << 6), /* for object_cache_instanced. */
  COLLECTION_IS_SPAWNED = (1 << 14), /* UPBGE to alternate between old spawn method */
};

/* Collection->tag */
enum {
  /* That code (BKE_main_collections_parent_relations_rebuild and the like)
   * is called from very low-level places, like e.g ID remapping...
   * Using a generic tag like LIB_TAG_DOIT for this is just impossible, we need our very own. */
  COLLECTION_TAG_RELATION_REBUILD = (1 << 0),
};

/* Collection->color_tag. */
typedef enum CollectionColorTag {
  COLLECTION_COLOR_NONE = -1,
  COLLECTION_COLOR_01,
  COLLECTION_COLOR_02,
  COLLECTION_COLOR_03,
  COLLECTION_COLOR_04,
  COLLECTION_COLOR_05,
  COLLECTION_COLOR_06,
  COLLECTION_COLOR_07,
  COLLECTION_COLOR_08,

  COLLECTION_COLOR_TOT,
} CollectionColorTag;

#ifdef __cplusplus
}
#endif
