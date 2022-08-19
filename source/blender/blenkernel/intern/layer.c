/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include <string.h>

#include "CLG_log.h"

#include "BLI_listbase.h"
#include "BLI_mempool.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.h"
#include "BLI_threads.h"
#include "BLT_translation.h"

#include "BKE_animsys.h"
#include "BKE_collection.h"
#include "BKE_freestyle.h"
#include "BKE_idprop.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_node.h"
#include "BKE_object.h"

#include "DNA_ID.h"
#include "DNA_collection_types.h"
#include "DNA_layer_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"
#include "DNA_world_types.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_debug.h"
#include "DEG_depsgraph_query.h"

#include "DRW_engine.h"

#include "RE_engine.h"

#include "MEM_guardedalloc.h"

#include "BLO_read_write.h"

static CLG_LogRef LOG = {"bke.layercollection"};

/* Set of flags which are dependent on a collection settings. */
static const short g_base_collection_flags = (BASE_VISIBLE_DEPSGRAPH | BASE_VISIBLE_VIEWLAYER |
                                              BASE_SELECTABLE | BASE_ENABLED_VIEWPORT |
                                              BASE_ENABLED_RENDER | BASE_HOLDOUT |
                                              BASE_INDIRECT_ONLY);

/* prototype */
static void object_bases_iterator_next(BLI_Iterator *iter, const int flag);

/* -------------------------------------------------------------------- */
/** \name Layer Collections and Bases
 * \{ */

static LayerCollection *layer_collection_add(ListBase *lb_parent, Collection *collection)
{
  LayerCollection *lc = MEM_callocN(sizeof(LayerCollection), "Collection Base");
  lc->collection = collection;
  lc->local_collections_bits = ~(0);
  BLI_addtail(lb_parent, lc);

  return lc;
}

static void layer_collection_free(ViewLayer *view_layer, LayerCollection *lc)
{
  if (lc == view_layer->active_collection) {
    view_layer->active_collection = NULL;
  }

  LISTBASE_FOREACH (LayerCollection *, nlc, &lc->layer_collections) {
    layer_collection_free(view_layer, nlc);
  }

  BLI_freelistN(&lc->layer_collections);
}

static Base *object_base_new(Object *ob)
{
  Base *base = MEM_callocN(sizeof(Base), "Object Base");
  base->object = ob;
  base->local_view_bits = ~(0);
  if (ob->base_flag & BASE_SELECTED) {
    base->flag |= BASE_SELECTED;
  }
  return base;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name View Layer
 * \{ */

/* RenderLayer */

ViewLayer *BKE_view_layer_default_view(const Scene *scene)
{
  LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
    if (!(view_layer->flag & VIEW_LAYER_RENDER)) {
      return view_layer;
    }
  }

  BLI_assert(scene->view_layers.first);
  return scene->view_layers.first;
}

ViewLayer *BKE_view_layer_default_render(const Scene *scene)
{
  LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
    if (view_layer->flag & VIEW_LAYER_RENDER) {
      return view_layer;
    }
  }

  BLI_assert(scene->view_layers.first);
  return scene->view_layers.first;
}

ViewLayer *BKE_view_layer_find(const Scene *scene, const char *layer_name)
{
  LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
    if (STREQ(view_layer->name, layer_name)) {
      return view_layer;
    }
  }

  return NULL;
}

ViewLayer *BKE_view_layer_context_active_PLACEHOLDER(const Scene *scene)
{
  BLI_assert(scene->view_layers.first);
  return scene->view_layers.first;
}

static ViewLayer *view_layer_add(const char *name)
{
  if (!name) {
    name = DATA_("ViewLayer");
  }

  ViewLayer *view_layer = MEM_callocN(sizeof(ViewLayer), "View Layer");
  view_layer->flag = VIEW_LAYER_RENDER | VIEW_LAYER_FREESTYLE;

  BLI_strncpy_utf8(view_layer->name, name, sizeof(view_layer->name));

  /* Pure rendering pipeline settings. */
  view_layer->layflag = SCE_LAY_FLAG_DEFAULT;
  view_layer->passflag = SCE_PASS_COMBINED;
  view_layer->pass_alpha_threshold = 0.5f;
  view_layer->cryptomatte_levels = 6;
  view_layer->cryptomatte_flag = VIEW_LAYER_CRYPTOMATTE_ACCURATE;
  BKE_freestyle_config_init(&view_layer->freestyle_config);

  return view_layer;
}

static void layer_collection_exclude_all(LayerCollection *layer_collection)
{
  LayerCollection *sub_collection = layer_collection->layer_collections.first;
  for (; sub_collection != NULL; sub_collection = sub_collection->next) {
    sub_collection->flag |= LAYER_COLLECTION_EXCLUDE;
    layer_collection_exclude_all(sub_collection);
  }
}

ViewLayer *BKE_view_layer_add(Scene *scene,
                              const char *name,
                              ViewLayer *view_layer_source,
                              const int type)
{
  ViewLayer *view_layer_new;

  if (view_layer_source) {
    name = view_layer_source->name;
  }

  switch (type) {
    default:
    case VIEWLAYER_ADD_NEW: {
      view_layer_new = view_layer_add(name);
      BLI_addtail(&scene->view_layers, view_layer_new);
      BKE_layer_collection_sync(scene, view_layer_new);
      break;
    }
    case VIEWLAYER_ADD_COPY: {
      /* Allocate and copy view layer data */
      view_layer_new = MEM_callocN(sizeof(ViewLayer), "View Layer");
      *view_layer_new = *view_layer_source;
      BKE_view_layer_copy_data(scene, scene, view_layer_new, view_layer_source, 0);
      BLI_addtail(&scene->view_layers, view_layer_new);

      BLI_strncpy_utf8(view_layer_new->name, name, sizeof(view_layer_new->name));
      break;
    }
    case VIEWLAYER_ADD_EMPTY: {
      view_layer_new = view_layer_add(name);
      BLI_addtail(&scene->view_layers, view_layer_new);

      /* Initialize layer-collections. */
      BKE_layer_collection_sync(scene, view_layer_new);
      layer_collection_exclude_all(view_layer_new->layer_collections.first);

      /* Update collections after changing visibility */
      BKE_layer_collection_sync(scene, view_layer_new);
      break;
    }
  }

  /* unique name */
  BLI_uniquename(&scene->view_layers,
                 view_layer_new,
                 DATA_("ViewLayer"),
                 '_',
                 offsetof(ViewLayer, name),
                 sizeof(view_layer_new->name));

  return view_layer_new;
}

void BKE_view_layer_free(ViewLayer *view_layer)
{
  BKE_view_layer_free_ex(view_layer, true);
}

void BKE_view_layer_free_ex(ViewLayer *view_layer, const bool do_id_user)
{
  view_layer->basact = NULL;

  BLI_freelistN(&view_layer->object_bases);

  if (view_layer->object_bases_hash) {
    BLI_ghash_free(view_layer->object_bases_hash, NULL, NULL);
  }

  LISTBASE_FOREACH (LayerCollection *, lc, &view_layer->layer_collections) {
    layer_collection_free(view_layer, lc);
  }
  BLI_freelistN(&view_layer->layer_collections);

  LISTBASE_FOREACH (ViewLayerEngineData *, sled, &view_layer->drawdata) {
    if (sled->storage) {
      if (sled->free) {
        sled->free(sled->storage);
      }
      MEM_freeN(sled->storage);
    }
  }
  BLI_freelistN(&view_layer->drawdata);
  BLI_freelistN(&view_layer->aovs);
  view_layer->active_aov = NULL;
  BLI_freelistN(&view_layer->lightgroups);
  view_layer->active_lightgroup = NULL;

  MEM_SAFE_FREE(view_layer->stats);

  BKE_freestyle_config_free(&view_layer->freestyle_config, do_id_user);

  if (view_layer->id_properties) {
    IDP_FreeProperty_ex(view_layer->id_properties, do_id_user);
  }

  MEM_SAFE_FREE(view_layer->object_bases_array);

  MEM_freeN(view_layer);
}

void BKE_view_layer_selected_objects_tag(ViewLayer *view_layer, const int tag)
{
  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    if ((base->flag & BASE_SELECTED) != 0) {
      base->object->flag |= tag;
    }
    else {
      base->object->flag &= ~tag;
    }
  }
}

static bool find_scene_collection_in_scene_collections(ListBase *lb, const LayerCollection *lc)
{
  LISTBASE_FOREACH (LayerCollection *, lcn, lb) {
    if (lcn == lc) {
      return true;
    }
    if (find_scene_collection_in_scene_collections(&lcn->layer_collections, lc)) {
      return true;
    }
  }
  return false;
}

Object *BKE_view_layer_camera_find(ViewLayer *view_layer)
{
  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    if (base->object->type == OB_CAMERA) {
      return base->object;
    }
  }

  return NULL;
}

ViewLayer *BKE_view_layer_find_from_collection(const Scene *scene, LayerCollection *lc)
{
  LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
    if (find_scene_collection_in_scene_collections(&view_layer->layer_collections, lc)) {
      return view_layer;
    }
  }

  return NULL;
}

/* Base */

static void view_layer_bases_hash_create(ViewLayer *view_layer, const bool do_base_duplicates_fix)
{
  static ThreadMutex hash_lock = BLI_MUTEX_INITIALIZER;

  if (view_layer->object_bases_hash == NULL) {
    BLI_mutex_lock(&hash_lock);

    if (view_layer->object_bases_hash == NULL) {
      GHash *hash = BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, __func__);

      LISTBASE_FOREACH_MUTABLE (Base *, base, &view_layer->object_bases) {
        if (base->object) {
          void **val_pp;
          if (!BLI_ghash_ensure_p(hash, base->object, &val_pp)) {
            *val_pp = base;
          }
          /* The same object has several bases.
           *
           * In normal cases this is a serious bug, but this is a common situation when remapping
           * an object into another one already present in the same View Layer. While ideally we
           * would process this case separately, for performances reasons it makes more sense to
           * tackle it here. */
          else if (do_base_duplicates_fix) {
            if (view_layer->basact == base) {
              view_layer->basact = NULL;
            }
            BLI_freelinkN(&view_layer->object_bases, base);
          }
          else {
            CLOG_FATAL(&LOG,
                       "Object '%s' has more than one entry in view layer's object bases listbase",
                       base->object->id.name + 2);
          }
        }
      }

      /* Assign pointer only after hash is complete. */
      view_layer->object_bases_hash = hash;
    }

    BLI_mutex_unlock(&hash_lock);
  }
}

Base *BKE_view_layer_base_find(ViewLayer *view_layer, Object *ob)
{
  if (!view_layer->object_bases_hash) {
    view_layer_bases_hash_create(view_layer, false);
  }

  return BLI_ghash_lookup(view_layer->object_bases_hash, ob);
}

void BKE_view_layer_base_deselect_all(ViewLayer *view_layer)
{
  Base *base;

  for (base = view_layer->object_bases.first; base; base = base->next) {
    base->flag &= ~BASE_SELECTED;
  }
}

void BKE_view_layer_base_select_and_set_active(struct ViewLayer *view_layer, Base *selbase)
{
  view_layer->basact = selbase;
  if ((selbase->flag & BASE_SELECTABLE) != 0) {
    selbase->flag |= BASE_SELECTED;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Copy View Layer and Layer Collections
 * \{ */

static void layer_aov_copy_data(ViewLayer *view_layer_dst,
                                const ViewLayer *view_layer_src,
                                ListBase *aovs_dst,
                                const ListBase *aovs_src)
{
  if (aovs_src != NULL) {
    BLI_duplicatelist(aovs_dst, aovs_src);
  }

  ViewLayerAOV *aov_dst = aovs_dst->first;
  const ViewLayerAOV *aov_src = aovs_src->first;

  while (aov_dst != NULL) {
    BLI_assert(aov_src);
    if (aov_src == view_layer_src->active_aov) {
      view_layer_dst->active_aov = aov_dst;
    }

    aov_dst = aov_dst->next;
    aov_src = aov_src->next;
  }
}

static void layer_lightgroup_copy_data(ViewLayer *view_layer_dst,
                                       const ViewLayer *view_layer_src,
                                       ListBase *lightgroups_dst,
                                       const ListBase *lightgroups_src)
{
  if (lightgroups_src != NULL) {
    BLI_duplicatelist(lightgroups_dst, lightgroups_src);
  }

  ViewLayerLightgroup *lightgroup_dst = lightgroups_dst->first;
  const ViewLayerLightgroup *lightgroup_src = lightgroups_src->first;

  while (lightgroup_dst != NULL) {
    BLI_assert(lightgroup_src);
    if (lightgroup_src == view_layer_src->active_lightgroup) {
      view_layer_dst->active_lightgroup = lightgroup_dst;
    }

    lightgroup_dst = lightgroup_dst->next;
    lightgroup_src = lightgroup_src->next;
  }
}

static void layer_collections_copy_data(ViewLayer *view_layer_dst,
                                        const ViewLayer *view_layer_src,
                                        ListBase *layer_collections_dst,
                                        const ListBase *layer_collections_src)
{
  BLI_duplicatelist(layer_collections_dst, layer_collections_src);

  LayerCollection *layer_collection_dst = layer_collections_dst->first;
  const LayerCollection *layer_collection_src = layer_collections_src->first;

  while (layer_collection_dst != NULL) {
    layer_collections_copy_data(view_layer_dst,
                                view_layer_src,
                                &layer_collection_dst->layer_collections,
                                &layer_collection_src->layer_collections);

    if (layer_collection_src == view_layer_src->active_collection) {
      view_layer_dst->active_collection = layer_collection_dst;
    }

    layer_collection_dst = layer_collection_dst->next;
    layer_collection_src = layer_collection_src->next;
  }
}

void BKE_view_layer_copy_data(Scene *scene_dst,
                              const Scene *UNUSED(scene_src),
                              ViewLayer *view_layer_dst,
                              const ViewLayer *view_layer_src,
                              const int flag)
{
  if (view_layer_dst->id_properties != NULL) {
    view_layer_dst->id_properties = IDP_CopyProperty_ex(view_layer_dst->id_properties, flag);
  }
  BKE_freestyle_config_copy(
      &view_layer_dst->freestyle_config, &view_layer_src->freestyle_config, flag);

  view_layer_dst->stats = NULL;

  /* Clear temporary data. */
  BLI_listbase_clear(&view_layer_dst->drawdata);
  view_layer_dst->object_bases_array = NULL;
  view_layer_dst->object_bases_hash = NULL;

  /* Copy layer collections and object bases. */
  /* Inline 'BLI_duplicatelist' and update the active base. */
  BLI_listbase_clear(&view_layer_dst->object_bases);
  LISTBASE_FOREACH (Base *, base_src, &view_layer_src->object_bases) {
    Base *base_dst = MEM_dupallocN(base_src);
    BLI_addtail(&view_layer_dst->object_bases, base_dst);
    if (view_layer_src->basact == base_src) {
      view_layer_dst->basact = base_dst;
    }
  }

  view_layer_dst->active_collection = NULL;
  layer_collections_copy_data(view_layer_dst,
                              view_layer_src,
                              &view_layer_dst->layer_collections,
                              &view_layer_src->layer_collections);

  LayerCollection *lc_scene_dst = view_layer_dst->layer_collections.first;
  lc_scene_dst->collection = scene_dst->master_collection;

  BLI_listbase_clear(&view_layer_dst->aovs);
  layer_aov_copy_data(
      view_layer_dst, view_layer_src, &view_layer_dst->aovs, &view_layer_src->aovs);

  BLI_listbase_clear(&view_layer_dst->lightgroups);
  layer_lightgroup_copy_data(
      view_layer_dst, view_layer_src, &view_layer_dst->lightgroups, &view_layer_src->lightgroups);

  if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
    id_us_plus((ID *)view_layer_dst->mat_override);
  }
}

void BKE_view_layer_rename(Main *bmain, Scene *scene, ViewLayer *view_layer, const char *newname)
{
  char oldname[sizeof(view_layer->name)];

  BLI_strncpy(oldname, view_layer->name, sizeof(view_layer->name));

  BLI_strncpy_utf8(view_layer->name, newname, sizeof(view_layer->name));
  BLI_uniquename(&scene->view_layers,
                 view_layer,
                 DATA_("ViewLayer"),
                 '.',
                 offsetof(ViewLayer, name),
                 sizeof(view_layer->name));

  if (scene->nodetree) {
    bNode *node;
    int index = BLI_findindex(&scene->view_layers, view_layer);

    for (node = scene->nodetree->nodes.first; node; node = node->next) {
      if (node->type == CMP_NODE_R_LAYERS && node->id == NULL) {
        if (node->custom1 == index) {
          BLI_strncpy(node->name, view_layer->name, NODE_MAXSTR);
        }
      }
    }
  }

  /* Fix all the animation data and windows which may link to this. */
  BKE_animdata_fix_paths_rename_all(NULL, "view_layers", oldname, view_layer->name);

  /* WM can be missing on startup. */
  wmWindowManager *wm = bmain->wm.first;
  if (wm) {
    LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
      if (win->scene == scene && STREQ(win->view_layer_name, oldname)) {
        STRNCPY(win->view_layer_name, view_layer->name);
      }
    }
  }

  /* Dependency graph uses view layer name based lookups. */
  DEG_id_tag_update(&scene->id, 0);
}

/* LayerCollection */

/**
 * Recursively get the collection for a given index
 */
static LayerCollection *collection_from_index(ListBase *lb, const int number, int *i)
{
  LISTBASE_FOREACH (LayerCollection *, lc, lb) {
    if (*i == number) {
      return lc;
    }

    (*i)++;
  }

  LISTBASE_FOREACH (LayerCollection *, lc, lb) {
    LayerCollection *lc_nested = collection_from_index(&lc->layer_collections, number, i);
    if (lc_nested) {
      return lc_nested;
    }
  }
  return NULL;
}

/**
 * Determine if a collection is hidden, viewport visibility restricted, or excluded
 */
static bool layer_collection_hidden(ViewLayer *view_layer, LayerCollection *lc)
{
  if (lc->flag & LAYER_COLLECTION_EXCLUDE) {
    return true;
  }

  /* Check visiblilty restriction flags */
  if (lc->flag & LAYER_COLLECTION_HIDE || lc->collection->flag & COLLECTION_HIDE_VIEWPORT) {
    return true;
  }

  /* Restriction flags stay set, so we need to check parents */
  CollectionParent *parent = lc->collection->parents.first;

  if (parent) {
    lc = BKE_layer_collection_first_from_scene_collection(view_layer, parent->collection);

    return lc && layer_collection_hidden(view_layer, lc);
  }

  return false;

  return false;
}

LayerCollection *BKE_layer_collection_from_index(ViewLayer *view_layer, const int index)
{
  int i = 0;
  return collection_from_index(&view_layer->layer_collections, index, &i);
}

LayerCollection *BKE_layer_collection_get_active(ViewLayer *view_layer)
{
  return view_layer->active_collection;
}

bool BKE_layer_collection_activate(ViewLayer *view_layer, LayerCollection *lc)
{
  if (lc->flag & LAYER_COLLECTION_EXCLUDE) {
    return false;
  }

  view_layer->active_collection = lc;
  return true;
}

LayerCollection *BKE_layer_collection_activate_parent(ViewLayer *view_layer, LayerCollection *lc)
{
  CollectionParent *parent = lc->collection->parents.first;

  if (parent) {
    lc = BKE_layer_collection_first_from_scene_collection(view_layer, parent->collection);
  }
  else {
    lc = NULL;
  }

  /* Don't activate excluded or hidden collections to prevent creating objects in a hidden
   * collection from the UI */
  if (lc && layer_collection_hidden(view_layer, lc)) {
    return BKE_layer_collection_activate_parent(view_layer, lc);
  }

  if (!lc) {
    lc = view_layer->layer_collections.first;
  }

  view_layer->active_collection = lc;
  return lc;
}

/**
 * Recursively get the count of collections
 */
static int collection_count(const ListBase *lb)
{
  int i = 0;
  LISTBASE_FOREACH (const LayerCollection *, lc, lb) {
    i += collection_count(&lc->layer_collections) + 1;
  }
  return i;
}

int BKE_layer_collection_count(const ViewLayer *view_layer)
{
  return collection_count(&view_layer->layer_collections);
}

/**
 * Recursively get the index for a given collection
 */
static int index_from_collection(ListBase *lb, const LayerCollection *lc, int *i)
{
  LISTBASE_FOREACH (LayerCollection *, lcol, lb) {
    if (lcol == lc) {
      return *i;
    }

    (*i)++;
  }

  LISTBASE_FOREACH (LayerCollection *, lcol, lb) {
    int i_nested = index_from_collection(&lcol->layer_collections, lc, i);
    if (i_nested != -1) {
      return i_nested;
    }
  }
  return -1;
}

int BKE_layer_collection_findindex(ViewLayer *view_layer, const LayerCollection *lc)
{
  int i = 0;
  return index_from_collection(&view_layer->layer_collections, lc, &i);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Syncing
 *
 * The layer collection tree mirrors the scene collection tree. Whenever that
 * changes we need to synchronize them so that there is a corresponding layer
 * collection for each collection. Note that the scene collection tree can
 * contain link or override collections, and so this is also called on .blend
 * file load to ensure any new or removed collections are synced.
 *
 * The view layer also contains a list of bases for each object that exists
 * in at least one layer collection. That list is also synchronized here, and
 * stores state like selection.
 *
 * This API allows to temporarily forbid resync of LayerCollections.
 *
 * This can greatly improve performances in cases where those functions get
 * called a lot (e.g. during massive remappings of IDs).
 *
 * Usage of these should be done very carefully though. In particular, calling
 * code must ensures it resync LayerCollections before any UI/Event loop
 * handling can happen.
 *
 * \warning This is not threadsafe at all, only use from main thread.
 *
 * \note It is probably needed to use #BKE_main_collection_sync_remap instead
 *       of just #BKE_main_collection_sync after disabling LayerCollection resync,
 *       unless it is absolutely certain that no ID remapping (or any other process
 *       that may invalidate the caches) will happen while it is disabled.
 *
 * \note This is a quick and safe band-aid around the long-known issue
 *       regarding this resync process.
 *       Proper fix would be to make resync itself lazy, i.e. only happen
 *       when actually needed.
 *       See also T73411.
 * \{ */

static bool no_resync = false;

void BKE_layer_collection_resync_forbid(void)
{
  no_resync = true;
}

void BKE_layer_collection_resync_allow(void)
{
  no_resync = false;
}

typedef struct LayerCollectionResync {
  struct LayerCollectionResync *prev, *next;

  /* Temp data used to generate a queue during valid layer search. See
   * #layer_collection_resync_find. */
  struct LayerCollectionResync *queue_next;

  /* LayerCollection and Collection wrapped by this data. */
  LayerCollection *layer;
  Collection *collection;

  /* Hierarchical relationships in the old, existing ViewLayer state (except for newly created
   * layers). */
  struct LayerCollectionResync *parent_layer_resync;
  ListBase children_layer_resync;

  /* This layer still points to a valid collection. */
  bool is_usable;
  /* This layer is still valid as a parent, i.e. at least one of its original layer children is
   * usable and matches one of its current children collections. */
  bool is_valid_as_parent;
  /* This layer is still valid as a child, i.e. its original layer parent is usable and matches one
   * of its current parents collections. */
  bool is_valid_as_child;
  /* This layer is still fully valid in the new collection hierarchy, i.e. itself and all of its
   * parents fully match the current collection hierarchy.
   * OR
   * This layer has already been re-used to match the new collections hierarchy. */
  bool is_used;
} LayerCollectionResync;

static LayerCollectionResync *layer_collection_resync_create_recurse(
    LayerCollectionResync *parent_layer_resync, LayerCollection *layer, BLI_mempool *mempool)
{
  LayerCollectionResync *layer_resync = BLI_mempool_calloc(mempool);

  layer_resync->layer = layer;
  layer_resync->collection = layer->collection;
  layer_resync->parent_layer_resync = parent_layer_resync;
  if (parent_layer_resync != NULL) {
    BLI_addtail(&parent_layer_resync->children_layer_resync, layer_resync);
  }

  layer_resync->is_usable = (layer->collection != NULL);
  layer_resync->is_valid_as_child =
      layer_resync->is_usable && (parent_layer_resync == NULL ||
                                  (parent_layer_resync->is_usable &&
                                   BLI_findptr(&parent_layer_resync->layer->collection->children,
                                               layer->collection,
                                               offsetof(CollectionChild, collection)) != NULL));
  if (layer_resync->is_valid_as_child) {
    layer_resync->is_used = parent_layer_resync != NULL ? parent_layer_resync->is_used : true;
  }
  else {
    layer_resync->is_used = false;
  }

  if (BLI_listbase_is_empty(&layer->layer_collections)) {
    layer_resync->is_valid_as_parent = layer_resync->is_usable;
  }
  else {
    LISTBASE_FOREACH (LayerCollection *, child_layer, &layer->layer_collections) {
      LayerCollectionResync *child_layer_resync = layer_collection_resync_create_recurse(
          layer_resync, child_layer, mempool);
      if (layer_resync->is_usable && child_layer_resync->is_valid_as_child) {
        layer_resync->is_valid_as_parent = true;
      }
    }
  }

  CLOG_INFO(&LOG,
            4,
            "Old LayerCollection for %s is...\n\tusable: %d\n\tvalid parent: %d\n\tvalid child: "
            "%d\n\tused: %d\n",
            layer_resync->collection ? layer_resync->collection->id.name : "<NONE>",
            layer_resync->is_usable,
            layer_resync->is_valid_as_parent,
            layer_resync->is_valid_as_child,
            layer_resync->is_used);

  return layer_resync;
}

static LayerCollectionResync *layer_collection_resync_find(LayerCollectionResync *layer_resync,
                                                           Collection *child_collection)
{
  /* Given the given parent, valid layer collection, find in the old hierarchy the best possible
   * unused layer matching the given child collection.
   *
   * This uses the following heuristics:
   *  - Prefer a layer descendant of the given parent one if possible.
   *  - Prefer a layer as closely related as possible from the given parent.
   *  - Do not used layers that are not head (highest possible ancestor) of a local valid hierarchy
   *    branch, since we can assume we could then re-use its ancestor instead.
   *
   * A queue is used to ensure this order of preferences.
   */

  BLI_assert(layer_resync->collection != child_collection);
  BLI_assert(child_collection != NULL);

  LayerCollectionResync *current_layer_resync = NULL;
  LayerCollectionResync *root_layer_resync = layer_resync;

  LayerCollectionResync *queue_head = layer_resync, *queue_tail = layer_resync;
  layer_resync->queue_next = NULL;

  while (queue_head != NULL) {
    current_layer_resync = queue_head;
    queue_head = current_layer_resync->queue_next;

    if (current_layer_resync->collection == child_collection &&
        (current_layer_resync->parent_layer_resync == layer_resync ||
         (!current_layer_resync->is_used && !current_layer_resync->is_valid_as_child))) {
      /* This layer is a valid candidate, because its collection matches the seeked one, AND:
       *  - It is a direct child of the initial given parent ('unchanged hierarchy' case), OR
       *  - It is not currently used, and not part of a valid hierarchy (sub-)chain.
       */
      break;
    }

    /* Else, add all its direct children for further searching. */
    LISTBASE_FOREACH (LayerCollectionResync *,
                      child_layer_resync,
                      &current_layer_resync->children_layer_resync) {
      /* Add to tail of the queue. */
      queue_tail->queue_next = child_layer_resync;
      child_layer_resync->queue_next = NULL;
      queue_tail = child_layer_resync;
      if (queue_head == NULL) {
        queue_head = queue_tail;
      }
    }

    /* If all descendants from current layer have been processed, go one step higher and
     * process all of its other siblings. */
    if (queue_head == NULL && root_layer_resync->parent_layer_resync != NULL) {
      LISTBASE_FOREACH (LayerCollectionResync *,
                        sibling_layer_resync,
                        &root_layer_resync->parent_layer_resync->children_layer_resync) {
        if (sibling_layer_resync == root_layer_resync) {
          continue;
        }
        /* Add to tail of the queue. */
        queue_tail->queue_next = sibling_layer_resync;
        sibling_layer_resync->queue_next = NULL;
        queue_tail = sibling_layer_resync;
        if (queue_head == NULL) {
          queue_head = queue_tail;
        }
      }
      root_layer_resync = root_layer_resync->parent_layer_resync;
    }

    current_layer_resync = NULL;
  }

  return current_layer_resync;
}

static void layer_collection_resync_unused_layers_free(ViewLayer *view_layer,
                                                       LayerCollectionResync *layer_resync)
{
  LISTBASE_FOREACH (
      LayerCollectionResync *, child_layer_resync, &layer_resync->children_layer_resync) {
    layer_collection_resync_unused_layers_free(view_layer, child_layer_resync);
  }

  if (!layer_resync->is_used) {
    CLOG_INFO(&LOG,
              4,
              "Freeing unused LayerCollection for %s",
              layer_resync->collection != NULL ? layer_resync->collection->id.name :
                                                 "<Deleted Collection>");

    if (layer_resync->layer == view_layer->active_collection) {
      view_layer->active_collection = NULL;
    }

    /* We do not want to go recursive here, this is handled through the LayerCollectionResync data
     * wrapper. */
    MEM_freeN(layer_resync->layer);
    layer_resync->layer = NULL;
    layer_resync->collection = NULL;
    layer_resync->is_usable = false;
  }
}

static void layer_collection_objects_sync(ViewLayer *view_layer,
                                          LayerCollection *layer,
                                          ListBase *r_lb_new_object_bases,
                                          const short collection_restrict,
                                          const short layer_restrict,
                                          const ushort local_collections_bits)
{
  /* No need to sync objects if the collection is excluded. */
  if ((layer->flag & LAYER_COLLECTION_EXCLUDE) != 0) {
    return;
  }

  LISTBASE_FOREACH (CollectionObject *, cob, &layer->collection->gobject) {
    if (cob->ob == NULL) {
      continue;
    }

    /* Tag linked object as a weak reference so we keep the object
     * base pointer on file load and remember hidden state. */
    id_lib_indirect_weak_link(&cob->ob->id);

    void **base_p;
    Base *base;
    if (BLI_ghash_ensure_p(view_layer->object_bases_hash, cob->ob, &base_p)) {
      /* Move from old base list to new base list. Base might have already
       * been moved to the new base list and the first/last test ensure that
       * case also works. */
      base = *base_p;
      if (!ELEM(base, r_lb_new_object_bases->first, r_lb_new_object_bases->last)) {
        BLI_remlink(&view_layer->object_bases, base);
        BLI_addtail(r_lb_new_object_bases, base);
      }
    }
    else {
      /* Create new base. */
      base = object_base_new(cob->ob);
      base->local_collections_bits = local_collections_bits;
      *base_p = base;
      BLI_addtail(r_lb_new_object_bases, base);
    }

    if ((collection_restrict & COLLECTION_HIDE_VIEWPORT) == 0) {
      base->flag_from_collection |= (BASE_ENABLED_VIEWPORT | BASE_VISIBLE_DEPSGRAPH);
      if ((layer_restrict & LAYER_COLLECTION_HIDE) == 0) {
        base->flag_from_collection |= BASE_VISIBLE_VIEWLAYER;
      }
      if (((collection_restrict & COLLECTION_HIDE_SELECT) == 0)) {
        base->flag_from_collection |= BASE_SELECTABLE;
      }
    }

    if ((collection_restrict & COLLECTION_HIDE_RENDER) == 0) {
      base->flag_from_collection |= BASE_ENABLED_RENDER;
    }

    /* Holdout and indirect only */
    if ((layer->flag & LAYER_COLLECTION_HOLDOUT) || (base->object->visibility_flag & OB_HOLDOUT)) {
      base->flag_from_collection |= BASE_HOLDOUT;
    }
    if (layer->flag & LAYER_COLLECTION_INDIRECT_ONLY) {
      base->flag_from_collection |= BASE_INDIRECT_ONLY;
    }

    layer->runtime_flag |= LAYER_COLLECTION_HAS_OBJECTS;
  }
}

static void layer_collection_sync(ViewLayer *view_layer,
                                  LayerCollectionResync *layer_resync,
                                  BLI_mempool *layer_resync_mempool,
                                  ListBase *r_lb_new_object_bases,
                                  const short parent_layer_flag,
                                  const short parent_collection_restrict,
                                  const short parent_layer_restrict,
                                  const ushort parent_local_collections_bits)
{
  /* This function assumes current 'parent' layer collection is already fully (re)synced and valid
   * regarding current Collection hierarchy.
   *
   * It will process all the children collections of the collection from the given 'parent' layer,
   * re-use or create layer collections for each of them, and ensure orders also match.
   *
   * Then it will ensure that the objects owned by the given parent collection have a proper base.
   *
   * NOTE: This process is recursive.
   */

  /* Temporary storage for all valid (new or reused) children layers. */
  ListBase new_lb_layer = {NULL, NULL};

  BLI_assert(layer_resync->is_used);

  LISTBASE_FOREACH (CollectionChild *, child, &layer_resync->collection->children) {
    Collection *child_collection = child->collection;
    LayerCollectionResync *child_layer_resync = layer_collection_resync_find(layer_resync,
                                                                             child_collection);

    if (child_layer_resync != NULL) {
      BLI_assert(child_layer_resync->collection != NULL);
      BLI_assert(child_layer_resync->layer != NULL);
      BLI_assert(child_layer_resync->is_usable);

      if (child_layer_resync->is_used) {
        CLOG_INFO(&LOG,
                  4,
                  "Found same existing LayerCollection for %s as child of %s",
                  child_collection->id.name,
                  layer_resync->collection->id.name);
      }
      else {
        CLOG_INFO(&LOG,
                  4,
                  "Found a valid unused LayerCollection for %s as child of %s, re-using it",
                  child_collection->id.name,
                  layer_resync->collection->id.name);
      }

      child_layer_resync->is_used = true;

      /* NOTE: Do not move the resync wrapper to match the new layer hierarchy, so that the old
       * parenting info remains available. In case a search for a valid layer in the children of
       * the current is required again, the old parenting hierarchy is needed as reference, not the
       * new one.
       */
      BLI_remlink(&child_layer_resync->parent_layer_resync->layer->layer_collections,
                  child_layer_resync->layer);
      BLI_addtail(&new_lb_layer, child_layer_resync->layer);
    }
    else {
      CLOG_INFO(&LOG,
                4,
                "No available LayerCollection for %s as child of %s, creating a new one",
                child_collection->id.name,
                layer_resync->collection->id.name);

      LayerCollection *child_layer = layer_collection_add(&new_lb_layer, child_collection);
      child_layer->flag = parent_layer_flag;

      child_layer_resync = BLI_mempool_calloc(layer_resync_mempool);
      child_layer_resync->collection = child_collection;
      child_layer_resync->layer = child_layer;
      child_layer_resync->is_usable = true;
      child_layer_resync->is_used = true;
      child_layer_resync->is_valid_as_child = true;
      child_layer_resync->is_valid_as_parent = true;
      /* NOTE: Needs to be added to the layer_resync hierarchy so that the resync wrapper gets
       * freed at the end. */
      child_layer_resync->parent_layer_resync = layer_resync;
      BLI_addtail(&layer_resync->children_layer_resync, child_layer_resync);
    }

    LayerCollection *child_layer = child_layer_resync->layer;

    const ushort child_local_collections_bits = parent_local_collections_bits &
                                                child_layer->local_collections_bits;

    /* Tag linked collection as a weak reference so we keep the layer
     * collection pointer on file load and remember exclude state. */
    id_lib_indirect_weak_link(&child_collection->id);

    /* Collection restrict is inherited. */
    short child_collection_restrict = parent_collection_restrict;
    short child_layer_restrict = parent_layer_restrict;
    if (!(child_collection->flag & COLLECTION_IS_MASTER)) {
      child_collection_restrict |= child_collection->flag;
      child_layer_restrict |= child_layer->flag;
    }

    /* Sync child collections. */
    layer_collection_sync(view_layer,
                          child_layer_resync,
                          layer_resync_mempool,
                          r_lb_new_object_bases,
                          child_layer->flag,
                          child_collection_restrict,
                          child_layer_restrict,
                          child_local_collections_bits);

    /* Layer collection exclude is not inherited. */
    child_layer->runtime_flag = 0;
    if (child_layer->flag & LAYER_COLLECTION_EXCLUDE) {
      continue;
    }

    /* We separate restrict viewport and visible view layer because a layer collection can be
     * hidden in the view layer yet (locally) visible in a viewport (if it is not restricted). */
    if (child_collection_restrict & COLLECTION_HIDE_VIEWPORT) {
      child_layer->runtime_flag |= LAYER_COLLECTION_HIDE_VIEWPORT;
    }

    if (((child_layer->runtime_flag & LAYER_COLLECTION_HIDE_VIEWPORT) == 0) &&
        ((child_layer_restrict & LAYER_COLLECTION_HIDE) == 0)) {
      child_layer->runtime_flag |= LAYER_COLLECTION_VISIBLE_VIEW_LAYER;
    }
  }

  /* Replace layer collection list with new one. */
  layer_resync->layer->layer_collections = new_lb_layer;
  BLI_assert(BLI_listbase_count(&layer_resync->collection->children) ==
             BLI_listbase_count(&new_lb_layer));

  /* Update bases etc. for objects. */
  layer_collection_objects_sync(view_layer,
                                layer_resync->layer,
                                r_lb_new_object_bases,
                                parent_collection_restrict,
                                parent_layer_restrict,
                                parent_local_collections_bits);
}

#ifndef NDEBUG
static bool view_layer_objects_base_cache_validate(ViewLayer *view_layer, LayerCollection *layer)
{
  bool is_valid = true;

  if (layer == NULL) {
    layer = view_layer->layer_collections.first;
  }

  /* Only check for a collection's objects if its layer is not excluded. */
  if ((layer->flag & LAYER_COLLECTION_EXCLUDE) == 0) {
    LISTBASE_FOREACH (CollectionObject *, cob, &layer->collection->gobject) {
      if (cob->ob == NULL) {
        continue;
      }
      if (BLI_ghash_lookup(view_layer->object_bases_hash, cob->ob) == NULL) {
        CLOG_FATAL(
            &LOG,
            "Object '%s' from collection '%s' has no entry in view layer's object bases cache",
            cob->ob->id.name + 2,
            layer->collection->id.name + 2);
        is_valid = false;
        break;
      }
    }
  }

  if (is_valid) {
    LISTBASE_FOREACH (LayerCollection *, layer_child, &layer->layer_collections) {
      if (!view_layer_objects_base_cache_validate(view_layer, layer_child)) {
        is_valid = false;
        break;
      }
    }
  }

  return is_valid;
}
#else
static bool view_layer_objects_base_cache_validate(ViewLayer *UNUSED(view_layer),
                                                   LayerCollection *UNUSED(layer))
{
  return true;
}
#endif

void BKE_layer_collection_doversion_2_80(const Scene *scene, ViewLayer *view_layer)
{
  LayerCollection *first_layer_collection = view_layer->layer_collections.first;
  if (BLI_listbase_count_at_most(&view_layer->layer_collections, 2) > 1 ||
      first_layer_collection->collection != scene->master_collection) {
    /* In some cases (from older files) we do have a master collection, but no matching layer,
     * instead all the children of the master collection have their layer collections in the
     * viewlayer's list. This is not a valid situation, add a layer for the master collection and
     * add all existing first-level layers as children of that new master layer. */
    ListBase layer_collections = view_layer->layer_collections;
    BLI_listbase_clear(&view_layer->layer_collections);
    LayerCollection *master_layer_collection = layer_collection_add(&view_layer->layer_collections,
                                                                    scene->master_collection);
    master_layer_collection->layer_collections = layer_collections;
  }
}

void BKE_layer_collection_sync(const Scene *scene, ViewLayer *view_layer)
{
  if (no_resync) {
    return;
  }

  if (!scene->master_collection) {
    /* Happens for old files that don't have versioning applied yet. */
    return;
  }

  if (BLI_listbase_is_empty(&view_layer->layer_collections)) {
    /* In some cases (from older files, or when creating a new ViewLayer from
     * #BKE_view_layer_add), we do have a master collection, yet no matching layer. Create the
     * master one here, so that the rest of the code can work as expected. */
    layer_collection_add(&view_layer->layer_collections, scene->master_collection);
  }

#ifndef NDEBUG
  {
    BLI_assert_msg(BLI_listbase_count_at_most(&view_layer->layer_collections, 2) == 1,
                   "ViewLayer's first level of children layer collections should always have "
                   "exactly one item");

    LayerCollection *first_layer_collection = view_layer->layer_collections.first;
    BLI_assert_msg(first_layer_collection->collection == scene->master_collection,
                   "ViewLayer's first layer collection should always be the one for the scene's "
                   "master collection");
  }
#endif

  /* Free cache. */
  MEM_SAFE_FREE(view_layer->object_bases_array);

  /* Create object to base hash if it does not exist yet. */
  if (!view_layer->object_bases_hash) {
    view_layer_bases_hash_create(view_layer, false);
  }

  /* Clear visible and selectable flags to be reset. */
  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    base->flag &= ~g_base_collection_flags;
    base->flag_from_collection &= ~g_base_collection_flags;
  }

  /* Generate temporary data representing the old layers hierarchy, and how well it matches the
   * new collections hierarchy. */
  BLI_mempool *layer_resync_mempool = BLI_mempool_create(
      sizeof(LayerCollectionResync), 1024, 1024, BLI_MEMPOOL_NOP);
  LayerCollectionResync *master_layer_resync = layer_collection_resync_create_recurse(
      NULL, view_layer->layer_collections.first, layer_resync_mempool);

  /* Generate new layer connections and object bases when collections changed. */
  ListBase new_object_bases = {.first = NULL, .last = NULL};
  const short parent_exclude = 0, parent_restrict = 0, parent_layer_restrict = 0;
  layer_collection_sync(view_layer,
                        master_layer_resync,
                        layer_resync_mempool,
                        &new_object_bases,
                        parent_exclude,
                        parent_restrict,
                        parent_layer_restrict,
                        ~(0));

  layer_collection_resync_unused_layers_free(view_layer, master_layer_resync);
  BLI_mempool_destroy(layer_resync_mempool);
  master_layer_resync = NULL;

  /* Any remaining object bases are to be removed. */
  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    if (view_layer->basact == base) {
      view_layer->basact = NULL;
    }

    if (base->object) {
      /* Those asserts are commented, since they are too expensive to perform even in debug, as
       * this layer resync function currently gets called way too often. */
#if 0
      BLI_assert(BLI_findindex(&new_object_bases, base) == -1);
      BLI_assert(BLI_findptr(&new_object_bases, base->object, offsetof(Base, object)) == NULL);
#endif
      BLI_ghash_remove(view_layer->object_bases_hash, base->object, NULL, NULL);
    }
  }

  BLI_freelistN(&view_layer->object_bases);
  view_layer->object_bases = new_object_bases;

  view_layer_objects_base_cache_validate(view_layer, NULL);

  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    BKE_base_eval_flags(base);
  }

  /* Always set a valid active collection. */
  LayerCollection *active = view_layer->active_collection;
  if (active && layer_collection_hidden(view_layer, active)) {
    BKE_layer_collection_activate_parent(view_layer, active);
  }
  else if (active == NULL) {
    view_layer->active_collection = view_layer->layer_collections.first;
  }
}

void BKE_scene_collection_sync(const Scene *scene)
{
  if (no_resync) {
    return;
  }

  LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
    BKE_layer_collection_sync(scene, view_layer);
  }
}

void BKE_main_collection_sync(const Main *bmain)
{
  if (no_resync) {
    return;
  }

  /* TODO: if a single collection changed, figure out which
   * scenes it belongs to and only update those. */

  /* TODO: optimize for file load so only linked collections get checked? */

  for (const Scene *scene = bmain->scenes.first; scene; scene = scene->id.next) {
    BKE_scene_collection_sync(scene);
  }

  BKE_layer_collection_local_sync_all(bmain);
}

void BKE_main_collection_sync_remap(const Main *bmain)
{
  if (no_resync) {
    return;
  }

  /* On remapping of object or collection pointers free caches. */
  /* TODO: try to make this faster */

  for (Scene *scene = bmain->scenes.first; scene; scene = scene->id.next) {
    LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
      MEM_SAFE_FREE(view_layer->object_bases_array);

      if (view_layer->object_bases_hash) {
        BLI_ghash_free(view_layer->object_bases_hash, NULL, NULL);
        view_layer->object_bases_hash = NULL;
      }

      /* Directly re-create the mapping here, so that we can also deal with duplicates in
       * `view_layer->object_bases` list of bases properly. This is the only place where such
       * duplicates should be fixed, and not considered as a critical error. */
      view_layer_bases_hash_create(view_layer, true);
    }

    BKE_collection_object_cache_free(scene->master_collection);
    DEG_id_tag_update_ex((Main *)bmain, &scene->master_collection->id, ID_RECALC_COPY_ON_WRITE);
    DEG_id_tag_update_ex((Main *)bmain, &scene->id, ID_RECALC_COPY_ON_WRITE);
  }

  for (Collection *collection = bmain->collections.first; collection;
       collection = collection->id.next) {
    BKE_collection_object_cache_free(collection);
    DEG_id_tag_update_ex((Main *)bmain, &collection->id, ID_RECALC_COPY_ON_WRITE);
  }

  BKE_main_collection_sync(bmain);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Selection
 * \{ */

bool BKE_layer_collection_objects_select(ViewLayer *view_layer, LayerCollection *lc, bool deselect)
{
  if (lc->collection->flag & COLLECTION_HIDE_SELECT) {
    return false;
  }

  bool changed = false;

  if (!(lc->flag & LAYER_COLLECTION_EXCLUDE)) {
    LISTBASE_FOREACH (CollectionObject *, cob, &lc->collection->gobject) {
      Base *base = BKE_view_layer_base_find(view_layer, cob->ob);

      if (base) {
        if (deselect) {
          if (base->flag & BASE_SELECTED) {
            base->flag &= ~BASE_SELECTED;
            changed = true;
          }
        }
        else {
          if ((base->flag & BASE_SELECTABLE) && !(base->flag & BASE_SELECTED)) {
            base->flag |= BASE_SELECTED;
            changed = true;
          }
        }
      }
    }
  }

  LISTBASE_FOREACH (LayerCollection *, iter, &lc->layer_collections) {
    changed |= BKE_layer_collection_objects_select(view_layer, iter, deselect);
  }

  return changed;
}

bool BKE_layer_collection_has_selected_objects(ViewLayer *view_layer, LayerCollection *lc)
{
  if (lc->collection->flag & COLLECTION_HIDE_SELECT) {
    return false;
  }

  if (!(lc->flag & LAYER_COLLECTION_EXCLUDE)) {
    LISTBASE_FOREACH (CollectionObject *, cob, &lc->collection->gobject) {
      Base *base = BKE_view_layer_base_find(view_layer, cob->ob);

      if (base && (base->flag & BASE_SELECTED) && (base->flag & BASE_VISIBLE_DEPSGRAPH)) {
        return true;
      }
    }
  }

  LISTBASE_FOREACH (LayerCollection *, iter, &lc->layer_collections) {
    if (BKE_layer_collection_has_selected_objects(view_layer, iter)) {
      return true;
    }
  }

  return false;
}

bool BKE_layer_collection_has_layer_collection(LayerCollection *lc_parent,
                                               LayerCollection *lc_child)
{
  if (lc_parent == lc_child) {
    return true;
  }

  LISTBASE_FOREACH (LayerCollection *, lc_iter, &lc_parent->layer_collections) {
    if (BKE_layer_collection_has_layer_collection(lc_iter, lc_child)) {
      return true;
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Visibility
 * \{ */

void BKE_base_set_visible(Scene *scene, ViewLayer *view_layer, Base *base, bool extend)
{
  if (!extend) {
    /* Make only one base visible. */
    LISTBASE_FOREACH (Base *, other, &view_layer->object_bases) {
      other->flag |= BASE_HIDDEN;
    }

    base->flag &= ~BASE_HIDDEN;
  }
  else {
    /* Toggle visibility of one base. */
    base->flag ^= BASE_HIDDEN;
  }

  BKE_layer_collection_sync(scene, view_layer);
}

bool BKE_base_is_visible(const View3D *v3d, const Base *base)
{
  if ((base->flag & BASE_VISIBLE_DEPSGRAPH) == 0) {
    return false;
  }

  if (v3d == NULL) {
    return base->flag & BASE_VISIBLE_VIEWLAYER;
  }

  if ((v3d->localvd) && ((v3d->local_view_uuid & base->local_view_bits) == 0)) {
    return false;
  }

  if (((1 << (base->object->type)) & v3d->object_type_exclude_viewport) != 0) {
    return false;
  }

  if (v3d->flag & V3D_LOCAL_COLLECTIONS) {
    return (v3d->local_collections_uuid & base->local_collections_bits) != 0;
  }

  return base->flag & BASE_VISIBLE_VIEWLAYER;
}

bool BKE_object_is_visible_in_viewport(const View3D *v3d, const struct Object *ob)
{
  BLI_assert(v3d != NULL);

  if (ob->visibility_flag & OB_HIDE_VIEWPORT) {
    return false;
  }

  if ((v3d->object_type_exclude_viewport & (1 << ob->type)) != 0) {
    return false;
  }

  if (v3d->localvd && ((v3d->local_view_uuid & ob->base_local_view_bits) == 0)) {
    return false;
  }

  if ((v3d->flag & V3D_LOCAL_COLLECTIONS) &&
      ((v3d->local_collections_uuid & ob->runtime.local_collections_bits) == 0)) {
    return false;
  }

  /* If not using local collection the object may still be in a hidden collection. */
  if ((v3d->flag & V3D_LOCAL_COLLECTIONS) == 0) {
    return (ob->base_flag & BASE_VISIBLE_VIEWLAYER) != 0;
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Collection Isolation & Local View
 * \{ */

static void layer_collection_flag_set_recursive(LayerCollection *lc, const int flag)
{
  lc->flag |= flag;
  LISTBASE_FOREACH (LayerCollection *, lc_iter, &lc->layer_collections) {
    layer_collection_flag_set_recursive(lc_iter, flag);
  }
}

static void layer_collection_flag_unset_recursive(LayerCollection *lc, const int flag)
{
  lc->flag &= ~flag;
  LISTBASE_FOREACH (LayerCollection *, lc_iter, &lc->layer_collections) {
    layer_collection_flag_unset_recursive(lc_iter, flag);
  }
}

void BKE_layer_collection_isolate_global(Scene *scene,
                                         ViewLayer *view_layer,
                                         LayerCollection *lc,
                                         bool extend)
{
  LayerCollection *lc_master = view_layer->layer_collections.first;
  bool hide_it = extend && (lc->runtime_flag & LAYER_COLLECTION_VISIBLE_VIEW_LAYER);

  if (!extend) {
    /* Hide all collections. */
    LISTBASE_FOREACH (LayerCollection *, lc_iter, &lc_master->layer_collections) {
      layer_collection_flag_set_recursive(lc_iter, LAYER_COLLECTION_HIDE);
    }
  }

  /* Make all the direct parents visible. */
  if (hide_it) {
    lc->flag |= LAYER_COLLECTION_HIDE;
  }
  else {
    LayerCollection *lc_parent = lc;
    LISTBASE_FOREACH (LayerCollection *, lc_iter, &lc_master->layer_collections) {
      if (BKE_layer_collection_has_layer_collection(lc_iter, lc)) {
        lc_parent = lc_iter;
        break;
      }
    }

    while (lc_parent != lc) {
      lc_parent->flag &= ~LAYER_COLLECTION_HIDE;

      LISTBASE_FOREACH (LayerCollection *, lc_iter, &lc_parent->layer_collections) {
        if (BKE_layer_collection_has_layer_collection(lc_iter, lc)) {
          lc_parent = lc_iter;
          break;
        }
      }
    }

    /* Make all the children visible, but respect their disable state. */
    layer_collection_flag_unset_recursive(lc, LAYER_COLLECTION_HIDE);

    BKE_layer_collection_activate(view_layer, lc);
  }

  BKE_layer_collection_sync(scene, view_layer);
}

static void layer_collection_local_visibility_set_recursive(LayerCollection *layer_collection,
                                                            const int local_collections_uuid)
{
  layer_collection->local_collections_bits |= local_collections_uuid;
  LISTBASE_FOREACH (LayerCollection *, child, &layer_collection->layer_collections) {
    layer_collection_local_visibility_set_recursive(child, local_collections_uuid);
  }
}

static void layer_collection_local_visibility_unset_recursive(LayerCollection *layer_collection,
                                                              const int local_collections_uuid)
{
  layer_collection->local_collections_bits &= ~local_collections_uuid;
  LISTBASE_FOREACH (LayerCollection *, child, &layer_collection->layer_collections) {
    layer_collection_local_visibility_unset_recursive(child, local_collections_uuid);
  }
}

static void layer_collection_local_sync(ViewLayer *view_layer,
                                        LayerCollection *layer_collection,
                                        const unsigned short local_collections_uuid,
                                        bool visible)
{
  if ((layer_collection->local_collections_bits & local_collections_uuid) == 0) {
    visible = false;
  }

  if (visible) {
    LISTBASE_FOREACH (CollectionObject *, cob, &layer_collection->collection->gobject) {
      if (cob->ob == NULL) {
        continue;
      }

      Base *base = BKE_view_layer_base_find(view_layer, cob->ob);
      base->local_collections_bits |= local_collections_uuid;
    }
  }

  LISTBASE_FOREACH (LayerCollection *, child, &layer_collection->layer_collections) {
    if ((child->flag & LAYER_COLLECTION_EXCLUDE) == 0) {
      layer_collection_local_sync(view_layer, child, local_collections_uuid, visible);
    }
  }
}

void BKE_layer_collection_local_sync(ViewLayer *view_layer, const View3D *v3d)
{
  if (no_resync) {
    return;
  }

  const unsigned short local_collections_uuid = v3d->local_collections_uuid;

  /* Reset flags and set the bases visible by default. */
  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    base->local_collections_bits &= ~local_collections_uuid;
  }

  LISTBASE_FOREACH (LayerCollection *, layer_collection, &view_layer->layer_collections) {
    layer_collection_local_sync(view_layer, layer_collection, local_collections_uuid, true);
  }
}

void BKE_layer_collection_local_sync_all(const Main *bmain)
{
  if (no_resync) {
    return;
  }

  LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
    LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
      LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
        LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
          if (area->spacetype != SPACE_VIEW3D) {
            continue;
          }
          View3D *v3d = area->spacedata.first;
          if (v3d->flag & V3D_LOCAL_COLLECTIONS) {
            BKE_layer_collection_local_sync(view_layer, v3d);
          }
        }
      }
    }
  }
}

void BKE_layer_collection_isolate_local(ViewLayer *view_layer,
                                        const View3D *v3d,
                                        LayerCollection *lc,
                                        bool extend)
{
  LayerCollection *lc_master = view_layer->layer_collections.first;
  bool hide_it = extend && ((v3d->local_collections_uuid & lc->local_collections_bits) != 0);

  if (!extend) {
    /* Hide all collections. */
    LISTBASE_FOREACH (LayerCollection *, lc_iter, &lc_master->layer_collections) {
      layer_collection_local_visibility_unset_recursive(lc_iter, v3d->local_collections_uuid);
    }
  }

  /* Make all the direct parents visible. */
  if (hide_it) {
    lc->local_collections_bits &= ~(v3d->local_collections_uuid);
  }
  else {
    LayerCollection *lc_parent = lc;
    LISTBASE_FOREACH (LayerCollection *, lc_iter, &lc_master->layer_collections) {
      if (BKE_layer_collection_has_layer_collection(lc_iter, lc)) {
        lc_parent = lc_iter;
        break;
      }
    }

    while (lc_parent != lc) {
      lc_parent->local_collections_bits |= v3d->local_collections_uuid;

      LISTBASE_FOREACH (LayerCollection *, lc_iter, &lc_parent->layer_collections) {
        if (BKE_layer_collection_has_layer_collection(lc_iter, lc)) {
          lc_parent = lc_iter;
          break;
        }
      }
    }

    /* Make all the children visible. */
    layer_collection_local_visibility_set_recursive(lc, v3d->local_collections_uuid);
  }

  BKE_layer_collection_local_sync(view_layer, v3d);
}

static void layer_collection_bases_show_recursive(ViewLayer *view_layer, LayerCollection *lc)
{
  if ((lc->flag & LAYER_COLLECTION_EXCLUDE) == 0) {
    LISTBASE_FOREACH (CollectionObject *, cob, &lc->collection->gobject) {
      Base *base = BKE_view_layer_base_find(view_layer, cob->ob);
      base->flag &= ~BASE_HIDDEN;
    }
  }
  LISTBASE_FOREACH (LayerCollection *, lc_iter, &lc->layer_collections) {
    layer_collection_bases_show_recursive(view_layer, lc_iter);
  }
}

static void layer_collection_bases_hide_recursive(ViewLayer *view_layer, LayerCollection *lc)
{
  if ((lc->flag & LAYER_COLLECTION_EXCLUDE) == 0) {
    LISTBASE_FOREACH (CollectionObject *, cob, &lc->collection->gobject) {
      Base *base = BKE_view_layer_base_find(view_layer, cob->ob);
      base->flag |= BASE_HIDDEN;
    }
  }
  LISTBASE_FOREACH (LayerCollection *, lc_iter, &lc->layer_collections) {
    layer_collection_bases_hide_recursive(view_layer, lc_iter);
  }
}

void BKE_layer_collection_set_visible(ViewLayer *view_layer,
                                      LayerCollection *lc,
                                      const bool visible,
                                      const bool hierarchy)
{
  if (hierarchy) {
    if (visible) {
      layer_collection_flag_unset_recursive(lc, LAYER_COLLECTION_HIDE);
      layer_collection_bases_show_recursive(view_layer, lc);
    }
    else {
      layer_collection_flag_set_recursive(lc, LAYER_COLLECTION_HIDE);
      layer_collection_bases_hide_recursive(view_layer, lc);
    }
  }
  else {
    if (visible) {
      lc->flag &= ~LAYER_COLLECTION_HIDE;
    }
    else {
      lc->flag |= LAYER_COLLECTION_HIDE;
    }
  }
}

/**
 * Set layer collection hide/exclude/indirect flag on a layer collection.
 * recursively.
 */
static void layer_collection_flag_recursive_set(LayerCollection *lc,
                                                const int flag,
                                                const bool value,
                                                const bool restore_flag)
{
  if (flag == LAYER_COLLECTION_EXCLUDE) {
    /* For exclude flag, we remember the state the children had before
     * excluding and restoring it when enabling the parent collection again. */
    if (value) {
      if (restore_flag) {
        SET_FLAG_FROM_TEST(
            lc->flag, (lc->flag & LAYER_COLLECTION_EXCLUDE), LAYER_COLLECTION_PREVIOUSLY_EXCLUDED);
      }
      else {
        lc->flag &= ~LAYER_COLLECTION_PREVIOUSLY_EXCLUDED;
      }

      lc->flag |= flag;
    }
    else {
      if (!(lc->flag & LAYER_COLLECTION_PREVIOUSLY_EXCLUDED)) {
        lc->flag &= ~flag;
      }
    }
  }
  else {
    SET_FLAG_FROM_TEST(lc->flag, value, flag);
  }

  LISTBASE_FOREACH (LayerCollection *, nlc, &lc->layer_collections) {
    layer_collection_flag_recursive_set(nlc, flag, value, true);
  }
}

void BKE_layer_collection_set_flag(LayerCollection *lc, const int flag, const bool value)
{
  layer_collection_flag_recursive_set(lc, flag, value, false);
}

/* ---------------------------------------------------------------------- */

static LayerCollection *find_layer_collection_by_scene_collection(LayerCollection *lc,
                                                                  const Collection *collection)
{
  if (lc->collection == collection) {
    return lc;
  }

  LISTBASE_FOREACH (LayerCollection *, nlc, &lc->layer_collections) {
    LayerCollection *found = find_layer_collection_by_scene_collection(nlc, collection);
    if (found) {
      return found;
    }
  }
  return NULL;
}

LayerCollection *BKE_layer_collection_first_from_scene_collection(const ViewLayer *view_layer,
                                                                  const Collection *collection)
{
  LISTBASE_FOREACH (LayerCollection *, layer_collection, &view_layer->layer_collections) {
    LayerCollection *found = find_layer_collection_by_scene_collection(layer_collection,
                                                                       collection);
    if (found != NULL) {
      return found;
    }
  }
  return NULL;
}

bool BKE_view_layer_has_collection(const ViewLayer *view_layer, const Collection *collection)
{
  return BKE_layer_collection_first_from_scene_collection(view_layer, collection) != NULL;
}

bool BKE_scene_has_object(Scene *scene, Object *ob)
{
  LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
    Base *base = BKE_view_layer_base_find(view_layer, ob);
    if (base) {
      return true;
    }
  }
  return false;
}

/** \} */

/* Iterators */

/* -------------------------------------------------------------------- */
/** \name Private Iterator Helpers
 * \{ */

typedef struct LayerObjectBaseIteratorData {
  const View3D *v3d;
  Base *base;
} LayerObjectBaseIteratorData;

static bool object_bases_iterator_is_valid(const View3D *v3d, Base *base, const int flag)
{
  BLI_assert((v3d == NULL) || (v3d->spacetype == SPACE_VIEW3D));

  /* Any flag satisfies the condition. */
  if (flag == ~0) {
    return (base->flag != 0);
  }

  /* Flags may be more than one flag, so we can't check != 0. */
  return BASE_VISIBLE(v3d, base) && ((base->flag & flag) == flag);
}

static void object_bases_iterator_begin(BLI_Iterator *iter, void *data_in_v, const int flag)
{
  ObjectsVisibleIteratorData *data_in = data_in_v;
  ViewLayer *view_layer = data_in->view_layer;
  const View3D *v3d = data_in->v3d;
  Base *base = view_layer->object_bases.first;

  /* when there are no objects */
  if (base == NULL) {
    iter->data = NULL;
    iter->valid = false;
    return;
  }

  LayerObjectBaseIteratorData *data = MEM_callocN(sizeof(LayerObjectBaseIteratorData), __func__);
  iter->data = data;

  data->v3d = v3d;
  data->base = base;

  if (object_bases_iterator_is_valid(v3d, base, flag) == false) {
    object_bases_iterator_next(iter, flag);
  }
  else {
    iter->current = base;
  }
}

static void object_bases_iterator_next(BLI_Iterator *iter, const int flag)
{
  LayerObjectBaseIteratorData *data = iter->data;
  Base *base = data->base->next;

  while (base) {
    if (object_bases_iterator_is_valid(data->v3d, base, flag)) {
      iter->current = base;
      data->base = base;
      return;
    }
    base = base->next;
  }

  iter->valid = false;
}

static void object_bases_iterator_end(BLI_Iterator *iter)
{
  MEM_SAFE_FREE(iter->data);
}

static void objects_iterator_begin(BLI_Iterator *iter, void *data_in, const int flag)
{
  object_bases_iterator_begin(iter, data_in, flag);

  if (iter->valid) {
    iter->current = ((Base *)iter->current)->object;
  }
}

static void objects_iterator_next(BLI_Iterator *iter, const int flag)
{
  object_bases_iterator_next(iter, flag);

  if (iter->valid) {
    iter->current = ((Base *)iter->current)->object;
  }
}

static void objects_iterator_end(BLI_Iterator *iter)
{
  object_bases_iterator_end(iter);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_selected_objects_iterator
 * See: #FOREACH_SELECTED_OBJECT_BEGIN
 * \{ */

void BKE_view_layer_selected_objects_iterator_begin(BLI_Iterator *iter, void *data_in)
{
  objects_iterator_begin(iter, data_in, BASE_VISIBLE_DEPSGRAPH | BASE_SELECTED);
}

void BKE_view_layer_selected_objects_iterator_next(BLI_Iterator *iter)
{
  objects_iterator_next(iter, BASE_VISIBLE_DEPSGRAPH | BASE_SELECTED);
}

void BKE_view_layer_selected_objects_iterator_end(BLI_Iterator *iter)
{
  objects_iterator_end(iter);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_visible_objects_iterator
 * \{ */

void BKE_view_layer_visible_objects_iterator_begin(BLI_Iterator *iter, void *data_in)
{
  objects_iterator_begin(iter, data_in, 0);
}

void BKE_view_layer_visible_objects_iterator_next(BLI_Iterator *iter)
{
  objects_iterator_next(iter, 0);
}

void BKE_view_layer_visible_objects_iterator_end(BLI_Iterator *iter)
{
  objects_iterator_end(iter);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_selected_editable_objects_iterator
 * \{ */

void BKE_view_layer_selected_editable_objects_iterator_begin(BLI_Iterator *iter, void *data_in)
{
  objects_iterator_begin(iter, data_in, BASE_VISIBLE_DEPSGRAPH | BASE_SELECTED);
  if (iter->valid) {
    if (BKE_object_is_libdata((Object *)iter->current) == false) {
      /* First object is valid (selectable and not libdata) -> all good. */
      return;
    }

    /* Object is selectable but not editable -> search for another one. */
    BKE_view_layer_selected_editable_objects_iterator_next(iter);
  }
}

void BKE_view_layer_selected_editable_objects_iterator_next(BLI_Iterator *iter)
{
  /* Search while there are objects and the one we have is not editable (editable = not libdata).
   */
  do {
    objects_iterator_next(iter, BASE_VISIBLE_DEPSGRAPH | BASE_SELECTED);
  } while (iter->valid && BKE_object_is_libdata((Object *)iter->current) != false);
}

void BKE_view_layer_selected_editable_objects_iterator_end(BLI_Iterator *iter)
{
  objects_iterator_end(iter);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_selected_bases_iterator
 * \{ */

void BKE_view_layer_selected_bases_iterator_begin(BLI_Iterator *iter, void *data_in)
{
  objects_iterator_begin(iter, data_in, BASE_VISIBLE_DEPSGRAPH | BASE_SELECTED);
}

void BKE_view_layer_selected_bases_iterator_next(BLI_Iterator *iter)
{
  object_bases_iterator_next(iter, BASE_VISIBLE_DEPSGRAPH | BASE_SELECTED);
}

void BKE_view_layer_selected_bases_iterator_end(BLI_Iterator *iter)
{
  object_bases_iterator_end(iter);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_visible_bases_iterator
 * \{ */

void BKE_view_layer_visible_bases_iterator_begin(BLI_Iterator *iter, void *data_in)
{
  object_bases_iterator_begin(iter, data_in, 0);
}

void BKE_view_layer_visible_bases_iterator_next(BLI_Iterator *iter)
{
  object_bases_iterator_next(iter, 0);
}

void BKE_view_layer_visible_bases_iterator_end(BLI_Iterator *iter)
{
  object_bases_iterator_end(iter);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BKE_view_layer_bases_in_mode_iterator
 * \{ */

static bool base_is_in_mode(struct ObjectsInModeIteratorData *data, Base *base)
{
  return (base->object->type == data->object_type) &&
         (base->object->mode & data->object_mode) != 0;
}

void BKE_view_layer_bases_in_mode_iterator_begin(BLI_Iterator *iter, void *data_in)
{
  struct ObjectsInModeIteratorData *data = data_in;
  Base *base = data->base_active;

  /* In this case the result will always be empty, the caller must check for no mode. */
  BLI_assert(data->object_mode != 0);

  /* when there are no objects */
  if (base == NULL) {
    iter->valid = false;
    return;
  }
  iter->data = data_in;
  iter->current = base;

  /* default type is active object type */
  if (data->object_type < 0) {
    data->object_type = base->object->type;
  }

  if (!(base_is_in_mode(data, base) && BKE_base_is_visible(data->v3d, base))) {
    BKE_view_layer_bases_in_mode_iterator_next(iter);
  }
}

void BKE_view_layer_bases_in_mode_iterator_next(BLI_Iterator *iter)
{
  struct ObjectsInModeIteratorData *data = iter->data;
  Base *base = iter->current;

  if (base == data->base_active) {
    /* first step */
    base = data->view_layer->object_bases.first;
    if ((base == data->base_active) && BKE_base_is_visible(data->v3d, base)) {
      base = base->next;
    }
  }
  else {
    base = base->next;
  }

  while (base) {
    if ((base != data->base_active) && base_is_in_mode(data, base) &&
        BKE_base_is_visible(data->v3d, base)) {
      iter->current = base;
      return;
    }
    base = base->next;
  }
  iter->valid = false;
}

void BKE_view_layer_bases_in_mode_iterator_end(BLI_Iterator *UNUSED(iter))
{
  /* do nothing */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Evaluation
 * \{ */

void BKE_base_eval_flags(Base *base)
{
  /* Apply collection flags. */
  base->flag &= ~g_base_collection_flags;
  base->flag |= (base->flag_from_collection & g_base_collection_flags);

  /* Apply object restrictions. */
  const int object_restrict = base->object->visibility_flag;
  if (object_restrict & OB_HIDE_VIEWPORT) {
    base->flag &= ~BASE_ENABLED_VIEWPORT;
  }
  if (object_restrict & OB_HIDE_RENDER) {
    base->flag &= ~BASE_ENABLED_RENDER;
  }
  if (object_restrict & OB_HIDE_SELECT) {
    base->flag &= ~BASE_SELECTABLE;
  }

  /* Apply viewport visibility by default. The dependency graph for render
   * can change these again, but for tools we always want the viewport
   * visibility to be in sync regardless if depsgraph was evaluated. */
  if (!(base->flag & BASE_ENABLED_VIEWPORT) || (base->flag & BASE_HIDDEN)) {
    base->flag &= ~(BASE_VISIBLE_DEPSGRAPH | BASE_VISIBLE_VIEWLAYER | BASE_SELECTABLE);
  }

  /* Deselect unselectable objects. */
  if (!(base->flag & BASE_SELECTABLE)) {
    base->flag &= ~BASE_SELECTED;
  }
}

static void layer_eval_view_layer(struct Depsgraph *depsgraph,
                                  struct Scene *UNUSED(scene),
                                  ViewLayer *view_layer)
{
  DEG_debug_print_eval(depsgraph, __func__, view_layer->name, view_layer);

  /* Create array of bases, for fast index-based lookup. */
  const int num_object_bases = BLI_listbase_count(&view_layer->object_bases);
  MEM_SAFE_FREE(view_layer->object_bases_array);
  view_layer->object_bases_array = MEM_malloc_arrayN(
      num_object_bases, sizeof(Base *), "view_layer->object_bases_array");
  int base_index = 0;
  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    view_layer->object_bases_array[base_index++] = base;
  }
}

void BKE_layer_eval_view_layer_indexed(struct Depsgraph *depsgraph,
                                       struct Scene *scene,
                                       int view_layer_index)
{
  BLI_assert(view_layer_index >= 0);
  ViewLayer *view_layer = BLI_findlink(&scene->view_layers, view_layer_index);
  BLI_assert(view_layer != NULL);
  layer_eval_view_layer(depsgraph, scene, view_layer);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend File I/O
 * \{ */

static void write_layer_collections(BlendWriter *writer, ListBase *lb)
{
  LISTBASE_FOREACH (LayerCollection *, lc, lb) {
    BLO_write_struct(writer, LayerCollection, lc);

    write_layer_collections(writer, &lc->layer_collections);
  }
}

void BKE_view_layer_blend_write(BlendWriter *writer, ViewLayer *view_layer)
{
  BLO_write_struct(writer, ViewLayer, view_layer);
  BLO_write_struct_list(writer, Base, &view_layer->object_bases);

  if (view_layer->id_properties) {
    IDP_BlendWrite(writer, view_layer->id_properties);
  }

  LISTBASE_FOREACH (FreestyleModuleConfig *, fmc, &view_layer->freestyle_config.modules) {
    BLO_write_struct(writer, FreestyleModuleConfig, fmc);
  }

  LISTBASE_FOREACH (FreestyleLineSet *, fls, &view_layer->freestyle_config.linesets) {
    BLO_write_struct(writer, FreestyleLineSet, fls);
  }
  LISTBASE_FOREACH (ViewLayerAOV *, aov, &view_layer->aovs) {
    BLO_write_struct(writer, ViewLayerAOV, aov);
  }
  LISTBASE_FOREACH (ViewLayerLightgroup *, lightgroup, &view_layer->lightgroups) {
    BLO_write_struct(writer, ViewLayerLightgroup, lightgroup);
  }
  write_layer_collections(writer, &view_layer->layer_collections);
}

static void direct_link_layer_collections(BlendDataReader *reader, ListBase *lb, bool master)
{
  BLO_read_list(reader, lb);
  LISTBASE_FOREACH (LayerCollection *, lc, lb) {
#ifdef USE_COLLECTION_COMPAT_28
    BLO_read_data_address(reader, &lc->scene_collection);
#endif

    /* Master collection is not a real data-block. */
    if (master) {
      BLO_read_data_address(reader, &lc->collection);
    }

    direct_link_layer_collections(reader, &lc->layer_collections, false);
  }
}

void BKE_view_layer_blend_read_data(BlendDataReader *reader, ViewLayer *view_layer)
{
  view_layer->stats = NULL;
  BLO_read_list(reader, &view_layer->object_bases);
  BLO_read_data_address(reader, &view_layer->basact);

  direct_link_layer_collections(reader, &view_layer->layer_collections, true);
  BLO_read_data_address(reader, &view_layer->active_collection);

  BLO_read_data_address(reader, &view_layer->id_properties);
  IDP_BlendDataRead(reader, &view_layer->id_properties);

  BLO_read_list(reader, &(view_layer->freestyle_config.modules));
  BLO_read_list(reader, &(view_layer->freestyle_config.linesets));

  BLO_read_list(reader, &view_layer->aovs);
  BLO_read_data_address(reader, &view_layer->active_aov);

  BLO_read_list(reader, &view_layer->lightgroups);
  BLO_read_data_address(reader, &view_layer->active_lightgroup);

  BLI_listbase_clear(&view_layer->drawdata);
  view_layer->object_bases_array = NULL;
  view_layer->object_bases_hash = NULL;
}

static void lib_link_layer_collection(BlendLibReader *reader,
                                      Library *lib,
                                      LayerCollection *layer_collection,
                                      bool master)
{
  /* Master collection is not a real data-block. */
  if (!master) {
    BLO_read_id_address(reader, lib, &layer_collection->collection);
  }

  LISTBASE_FOREACH (
      LayerCollection *, layer_collection_nested, &layer_collection->layer_collections) {
    lib_link_layer_collection(reader, lib, layer_collection_nested, false);
  }
}

void BKE_view_layer_blend_read_lib(BlendLibReader *reader, Library *lib, ViewLayer *view_layer)
{
  LISTBASE_FOREACH (FreestyleModuleConfig *, fmc, &view_layer->freestyle_config.modules) {
    BLO_read_id_address(reader, lib, &fmc->script);
  }

  LISTBASE_FOREACH (FreestyleLineSet *, fls, &view_layer->freestyle_config.linesets) {
    BLO_read_id_address(reader, lib, &fls->linestyle);
    BLO_read_id_address(reader, lib, &fls->group);
  }

  LISTBASE_FOREACH_MUTABLE (Base *, base, &view_layer->object_bases) {
    /* we only bump the use count for the collection objects */
    BLO_read_id_address(reader, lib, &base->object);

    if (base->object == NULL) {
      /* Free in case linked object got lost. */
      BLI_freelinkN(&view_layer->object_bases, base);
      if (view_layer->basact == base) {
        view_layer->basact = NULL;
      }
    }
  }

  LISTBASE_FOREACH (LayerCollection *, layer_collection, &view_layer->layer_collections) {
    lib_link_layer_collection(reader, lib, layer_collection, true);
  }

  BLO_read_id_address(reader, lib, &view_layer->mat_override);

  IDP_BlendReadLib(reader, lib, view_layer->id_properties);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader AOV
 * \{ */

static void viewlayer_aov_make_name_unique(ViewLayer *view_layer)
{
  ViewLayerAOV *aov = view_layer->active_aov;
  if (aov == NULL) {
    return;
  }

  /* Don't allow dots, it's incompatible with OpenEXR convention to store channels
   * as "layer.pass.channel". */
  BLI_str_replace_char(aov->name, '.', '_');
  BLI_uniquename(
      &view_layer->aovs, aov, DATA_("AOV"), '_', offsetof(ViewLayerAOV, name), sizeof(aov->name));
}

static void viewlayer_aov_active_set(ViewLayer *view_layer, ViewLayerAOV *aov)
{
  if (aov != NULL) {
    BLI_assert(BLI_findindex(&view_layer->aovs, aov) != -1);
    view_layer->active_aov = aov;
  }
  else {
    view_layer->active_aov = NULL;
  }
}

struct ViewLayerAOV *BKE_view_layer_add_aov(struct ViewLayer *view_layer)
{
  ViewLayerAOV *aov;
  aov = MEM_callocN(sizeof(ViewLayerAOV), __func__);
  aov->type = AOV_TYPE_COLOR;
  BLI_strncpy(aov->name, DATA_("AOV"), sizeof(aov->name));
  BLI_addtail(&view_layer->aovs, aov);
  viewlayer_aov_active_set(view_layer, aov);
  viewlayer_aov_make_name_unique(view_layer);
  return aov;
}

void BKE_view_layer_remove_aov(ViewLayer *view_layer, ViewLayerAOV *aov)
{
  BLI_assert(BLI_findindex(&view_layer->aovs, aov) != -1);
  BLI_assert(aov != NULL);
  if (view_layer->active_aov == aov) {
    if (aov->next) {
      viewlayer_aov_active_set(view_layer, aov->next);
    }
    else {
      viewlayer_aov_active_set(view_layer, aov->prev);
    }
  }
  BLI_freelinkN(&view_layer->aovs, aov);
}

void BKE_view_layer_set_active_aov(ViewLayer *view_layer, ViewLayerAOV *aov)
{
  viewlayer_aov_active_set(view_layer, aov);
}

static void bke_view_layer_verify_aov_cb(void *userdata,
                                         Scene *UNUSED(scene),
                                         ViewLayer *UNUSED(view_layer),
                                         const char *name,
                                         int UNUSED(channels),
                                         const char *UNUSED(chanid),
                                         eNodeSocketDatatype UNUSED(type))
{
  GHash *name_count = userdata;
  void **value_p;
  void *key = BLI_strdup(name);

  if (!BLI_ghash_ensure_p(name_count, key, &value_p)) {
    *value_p = POINTER_FROM_INT(1);
  }
  else {
    int value = POINTER_AS_INT(*value_p);
    value++;
    *value_p = POINTER_FROM_INT(value);
    MEM_freeN(key);
  }
}

void BKE_view_layer_verify_aov(struct RenderEngine *engine,
                               struct Scene *scene,
                               struct ViewLayer *view_layer)
{
  viewlayer_aov_make_name_unique(view_layer);

  GHash *name_count = BLI_ghash_str_new(__func__);
  RE_engine_update_render_passes(
      engine, scene, view_layer, bke_view_layer_verify_aov_cb, name_count);
  LISTBASE_FOREACH (ViewLayerAOV *, aov, &view_layer->aovs) {
    void **value_p = BLI_ghash_lookup(name_count, aov->name);
    int count = POINTER_AS_INT(value_p);
    SET_FLAG_FROM_TEST(aov->flag, count > 1, AOV_CONFLICT);
  }
  BLI_ghash_free(name_count, MEM_freeN, NULL);
}

bool BKE_view_layer_has_valid_aov(ViewLayer *view_layer)
{
  LISTBASE_FOREACH (ViewLayerAOV *, aov, &view_layer->aovs) {
    if ((aov->flag & AOV_CONFLICT) == 0) {
      return true;
    }
  }
  return false;
}

ViewLayer *BKE_view_layer_find_with_aov(struct Scene *scene, struct ViewLayerAOV *aov)
{
  LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
    if (BLI_findindex(&view_layer->aovs, aov) != -1) {
      return view_layer;
    }
  }
  return NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Light Groups
 * \{ */

static void viewlayer_lightgroup_make_name_unique(ViewLayer *view_layer,
                                                  ViewLayerLightgroup *lightgroup)
{
  /* Don't allow dots, it's incompatible with OpenEXR convention to store channels
   * as "layer.pass.channel". */
  BLI_str_replace_char(lightgroup->name, '.', '_');
  BLI_uniquename(&view_layer->lightgroups,
                 lightgroup,
                 DATA_("Lightgroup"),
                 '_',
                 offsetof(ViewLayerLightgroup, name),
                 sizeof(lightgroup->name));
}

static void viewlayer_lightgroup_active_set(ViewLayer *view_layer, ViewLayerLightgroup *lightgroup)
{
  if (lightgroup != NULL) {
    BLI_assert(BLI_findindex(&view_layer->lightgroups, lightgroup) != -1);
    view_layer->active_lightgroup = lightgroup;
  }
  else {
    view_layer->active_lightgroup = NULL;
  }
}

struct ViewLayerLightgroup *BKE_view_layer_add_lightgroup(struct ViewLayer *view_layer,
                                                          const char *name)
{
  ViewLayerLightgroup *lightgroup;
  lightgroup = MEM_callocN(sizeof(ViewLayerLightgroup), __func__);
  if (name && name[0]) {
    BLI_strncpy(lightgroup->name, name, sizeof(lightgroup->name));
  }
  else {
    BLI_strncpy(lightgroup->name, DATA_("Lightgroup"), sizeof(lightgroup->name));
  }
  BLI_addtail(&view_layer->lightgroups, lightgroup);
  viewlayer_lightgroup_active_set(view_layer, lightgroup);
  viewlayer_lightgroup_make_name_unique(view_layer, lightgroup);
  return lightgroup;
}

void BKE_view_layer_remove_lightgroup(ViewLayer *view_layer, ViewLayerLightgroup *lightgroup)
{
  BLI_assert(BLI_findindex(&view_layer->lightgroups, lightgroup) != -1);
  BLI_assert(lightgroup != NULL);
  if (view_layer->active_lightgroup == lightgroup) {
    if (lightgroup->next) {
      viewlayer_lightgroup_active_set(view_layer, lightgroup->next);
    }
    else {
      viewlayer_lightgroup_active_set(view_layer, lightgroup->prev);
    }
  }
  BLI_freelinkN(&view_layer->lightgroups, lightgroup);
}

void BKE_view_layer_set_active_lightgroup(ViewLayer *view_layer, ViewLayerLightgroup *lightgroup)
{
  viewlayer_lightgroup_active_set(view_layer, lightgroup);
}

ViewLayer *BKE_view_layer_find_with_lightgroup(struct Scene *scene,
                                               struct ViewLayerLightgroup *lightgroup)
{
  LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
    if (BLI_findindex(&view_layer->lightgroups, lightgroup) != -1) {
      return view_layer;
    }
  }
  return NULL;
}

void BKE_view_layer_rename_lightgroup(Scene *scene,
                                      ViewLayer *view_layer,
                                      ViewLayerLightgroup *lightgroup,
                                      const char *name)
{
  char old_name[64];
  BLI_strncpy_utf8(old_name, lightgroup->name, sizeof(old_name));
  BLI_strncpy_utf8(lightgroup->name, name, sizeof(lightgroup->name));
  viewlayer_lightgroup_make_name_unique(view_layer, lightgroup);

  if (scene != NULL) {
    /* Update objects in the scene to refer to the new name instead. */
    FOREACH_SCENE_OBJECT_BEGIN (scene, ob) {
      if (!ID_IS_LINKED(ob) && ob->lightgroup != NULL) {
        LightgroupMembership *lgm = ob->lightgroup;
        if (STREQ(lgm->name, old_name)) {
          BLI_strncpy_utf8(lgm->name, lightgroup->name, sizeof(lgm->name));
        }
      }
    }
    FOREACH_SCENE_OBJECT_END;

    /* Update the scene's world to refer to the new name instead. */
    if (scene->world != NULL && !ID_IS_LINKED(scene->world) && scene->world->lightgroup != NULL) {
      LightgroupMembership *lgm = scene->world->lightgroup;
      if (STREQ(lgm->name, old_name)) {
        BLI_strncpy_utf8(lgm->name, lightgroup->name, sizeof(lgm->name));
      }
    }
  }
}

void BKE_lightgroup_membership_get(struct LightgroupMembership *lgm, char *name)
{
  if (lgm != NULL) {
    BLI_strncpy(name, lgm->name, sizeof(lgm->name));
  }
  else {
    name[0] = '\0';
  }
}

int BKE_lightgroup_membership_length(struct LightgroupMembership *lgm)
{
  if (lgm != NULL) {
    return strlen(lgm->name);
  }
  return 0;
}

void BKE_lightgroup_membership_set(struct LightgroupMembership **lgm, const char *name)
{
  if (name[0] != '\0') {
    if (*lgm == NULL) {
      *lgm = MEM_callocN(sizeof(LightgroupMembership), __func__);
    }
    BLI_strncpy((*lgm)->name, name, sizeof((*lgm)->name));
  }
  else {
    if (*lgm != NULL) {
      MEM_freeN(*lgm);
      *lgm = NULL;
    }
  }
}

/** \} */
