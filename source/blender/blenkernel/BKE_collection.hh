/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_ghash.h"
#include "BLI_iterator.h"
#include "BLI_sys_types.h"

#include "DNA_listBase.h"
#include "DNA_userdef_enums.h"

/* Structs */

struct BLI_Iterator;
struct Base;
struct BlendDataReader;
struct BlendWriter;
struct Collection;
struct ID;
struct CollectionChild;
struct CollectionExport;
struct GHash;
struct Main;
struct Object;
struct Scene;
struct ViewLayer;

/** #CollectionRuntime.tag */
enum {
  /**
   * That code (#BKE_main_collections_parent_relations_rebuild and the like)
   * is called from very low-level places, like e.g ID remapping...
   * Using a generic tag like #ID_TAG_DOIT for this is just impossible, we need our very own.
   */
  COLLECTION_TAG_RELATION_REBUILD = (1 << 0),
  /**
   * Mark the `gobject` list and/or its `runtime.gobject_hash` mapping as dirty, i.e. that their
   * data is not reliable and should be cleaned-up or updated.
   *
   * This should typically only be set by ID remapping code.
   */
  COLLECTION_TAG_COLLECTION_OBJECT_DIRTY = (1 << 1),
};

namespace blender::bke {

struct CollectionRuntime {
  /**
   * Cache of objects in this collection and all its children.
   * This is created on demand when e.g. some physics simulation needs it,
   * we don't want to have it for every collections due to memory usage reasons.
   */
  ListBase object_cache = {};

  /** Need this for line art sub-collection selections. */
  ListBase object_cache_instanced = {};

  /** List of collections that are a parent of this data-block. */
  ListBase parents = {};

  /** An optional map for faster lookups on #Collection.gobject */
  GHash *gobject_hash = nullptr;

  uint8_t tag = 0;
};

}  // namespace blender::bke

struct CollectionParent {
  struct CollectionParent *next, *prev;
  struct Collection *collection;
};

/* Collections */

/**
 * Add a collection to a collection ListBase and synchronize all render layers
 * The ListBase is NULL when the collection is to be added to the master collection
 */
Collection *BKE_collection_add(Main *bmain,
                               Collection *collection_parent,
                               const char *name_custom);
/**
 * Add \a collection_dst to all scene collections that reference object \a ob_src is in.
 * Used to replace an instance object with a collection (library override operator).
 *
 * Logic is very similar to #BKE_collection_object_add_from().
 */
void BKE_collection_add_from_object(Main *bmain,
                                    Scene *scene,
                                    const Object *ob_src,
                                    Collection *collection_dst);
/**
 * Add \a collection_dst to all scene collections that reference collection \a collection_src is
 * in.
 *
 * Logic is very similar to #BKE_collection_object_add_from().
 */
void BKE_collection_add_from_collection(Main *bmain,
                                        Scene *scene,
                                        Collection *collection_src,
                                        Collection *collection_dst);
/**
 * Free (or release) any data used by this collection (does not free the collection itself).
 */
void BKE_collection_free_data(Collection *collection);

/**
 * Add a new collection exporter to the collection.
 */
CollectionExport *BKE_collection_exporter_add(Collection *collection, char *idname, char *label);

/**
 * Remove a collection exporter from the collection.
 */
void BKE_collection_exporter_remove(Collection *collection, CollectionExport *data);

/**
 * Move a collection exporter from one position to another.
 */
bool BKE_collection_exporter_move(Collection *collection, const int from, const int to);

/**
 * Assigns a unique name to the collection exporter.
 */
void BKE_collection_exporter_name_set(const ListBase *exporters,
                                      CollectionExport *data,
                                      const char *newname);

/**
 * Free all data owned by the collection exporter.
 */
void BKE_collection_exporter_free_data(CollectionExport *data);

/**
 * Remove a collection, optionally removing its child objects or moving
 * them to parent collections.
 */
bool BKE_collection_delete(Main *bmain, Collection *collection, bool hierarchy);

/**
 * Make a deep copy (aka duplicate) of the given collection and all of its children, recursively.
 *
 * \param dupflag: Controls which sub-data are also duplicated
 * (see #eDupli_ID_Flags in DNA_userdef_types.h).
 * \param duplicate_options: Additional context information about current duplicate call (e.g. if
 * it's part of a higher-level duplication or not, etc.). (see #eLibIDDuplicateFlags in
 * BKE_lib_id.hh).
 *
 * \warning By default, this functions will clear all \a bmain #ID.idnew pointers
 * (#BKE_main_id_newptr_and_tag_clear), and take care of post-duplication updates like remapping to
 * new IDs (#BKE_libblock_relink_to_newid) and rebuilding of the collection hierarchy information
 * (#BKE_main_collection_sync).
 * If \a #LIB_ID_DUPLICATE_IS_SUBPROCESS duplicate option is passed on (typically when duplication
 * is called recursively from another parent duplication operation), the caller is responsible to
 * handle all of these operations.
 *
 * \note Caller MUST handle updates of the depsgraph (#DAG_relations_tag_update).
 */
Collection *BKE_collection_duplicate(Main *bmain,
                                     Collection *parent,
                                     CollectionChild *child_old,
                                     Collection *collection,
                                     eDupli_ID_Flags duplicate_flags,
                                     /*eLibIDDuplicateFlags*/ uint duplicate_options);

/* Master Collection for Scene */

#define BKE_SCENE_COLLECTION_NAME "Scene Collection"
Collection *BKE_collection_master_add(Scene *scene);

/**
 * Check if the collection contains any geometry that can be rendered. Otherwise there's nothing to
 * display in the preview, so don't generate one.
 * Objects and sub-collections hidden in the render will be skipped.
 */
bool BKE_collection_contains_geometry_recursive(const Collection *collection);

/* Collection Objects */

bool BKE_collection_has_object(Collection *collection, const Object *ob);
bool BKE_collection_has_object_recursive(Collection *collection, Object *ob);
bool BKE_collection_has_object_recursive_instanced(Collection *collection, Object *ob);
/**
 * Find whether an evaluated object's original ID is contained or instanced by any object in this
 * collection. The collection is expected to be an evaluated data-block too.
 */
bool BKE_collection_has_object_recursive_instanced_orig_id(Collection *collection_eval,
                                                           Object *object_eval);
Collection *BKE_collection_object_find(Main *bmain,
                                       Scene *scene,
                                       Collection *collection,
                                       Object *ob);

CollectionChild *BKE_collection_child_find(Collection *parent, Collection *collection);

bool BKE_collection_is_empty(const Collection *collection);

/**
 * Add object to given collection, ensuring this collection is 'editable' (i.e. local and not a
 * liboverride), and finding a suitable parent one otherwise.
 */
bool BKE_collection_object_add(Main *bmain, Collection *collection, Object *ob);

/**
 * Add object to given collection, similar to #BKE_collection_object_add.
 *
 * However, it additionally ensures that the selected collection is also part of the given
 * `view_layer`, if non-NULL. Otherwise, the object is not added to any collection.
 */
bool BKE_collection_viewlayer_object_add(Main *bmain,
                                         const ViewLayer *view_layer,
                                         Collection *collection,
                                         Object *ob);

/**
 * Same as #BKE_collection_object_add, but unconditionally adds the object to the given collection.
 *
 * NOTE: required in certain cases, like do-versioning or complex ID management tasks.
 */
bool BKE_collection_object_add_notest(Main *bmain, Collection *collection, Object *ob);
/**
 * Add \a ob_dst to all scene collections that reference object \a ob_src is in.
 * Used for copying objects.
 *
 * Logic is very similar to #BKE_collection_add_from_object()
 */
void BKE_collection_object_add_from(Main *bmain, Scene *scene, Object *ob_src, Object *ob_dst);
/**
 * Remove ob from collection.
 */
bool BKE_collection_object_remove(Main *bmain, Collection *collection, Object *ob, bool free_us);
/**
 * Replace one object with another in a collection (managing user counts).
 */
bool BKE_collection_object_replace(Main *bmain,
                                   Collection *collection,
                                   Object *ob_old,
                                   Object *ob_new);

/**
 * Move object from a collection into another
 *
 * If source collection is NULL move it from all the existing collections.
 */
void BKE_collection_object_move(
    Main *bmain, Scene *scene, Collection *collection_dst, Collection *collection_src, Object *ob);

/**
 * Remove object from all collections of scene
 */
bool BKE_scene_collections_object_remove(Main *bmain, Scene *scene, Object *ob, bool free_us);

/**
 * Check all collections in \a bmain (including embedded ones in scenes) for invalid
 * CollectionObject (either with NULL object pointer, or duplicates), and remove them.
 *
 * \note In case of duplicates, the first CollectionObject in the list is kept, all others are
 * removed.
 */
void BKE_collections_object_remove_invalids(Main *bmain);

/**
 * Remove all NULL children from parent collections of changed \a collection.
 * This is used for library remapping, where these pointers have been set to NULL.
 * Otherwise this should never happen.
 *
 * \note caller must ensure #BKE_main_collection_sync_remap() is called afterwards!
 *
 * \param parent_collection: The collection owning the pointers that were remapped. May be \a NULL,
 * in which case whole \a bmain database of collections is checked.
 * \param child_collection: The collection that was remapped to another pointer. May be \a NULL,
 * in which case whole \a bmain database of collections is checked.
 */
void BKE_collections_child_remove_nulls(Main *bmain,
                                        Collection *parent_collection,
                                        Collection *child_collection);

/* Dependencies. */

bool BKE_collection_is_in_scene(Collection *collection);
void BKE_collections_after_lib_link(Main *bmain);
bool BKE_collection_object_cyclic_check(Main *bmain, Object *object, Collection *collection);

/* Object list cache. */

ListBase BKE_collection_object_cache_get(Collection *collection);
ListBase BKE_collection_object_cache_instanced_get(Collection *collection);
/**
 * Free the object cache of given `collection` and all of its ancestors (recursively).
 *
 * \param bmain: The Main database owning the collection. May be `nullptr`, only used if doing
 * depsgraph tagging.
 * \param id_create_flag: Flags controlling ID creation, used here to enable or
 * not depsgraph tagging of affected IDs
 * (e.g. #LIB_ID_CREATE_NO_DEG_TAG would prevent depsgraph tagging).
 */
void BKE_collection_object_cache_free(const Main *bmain,
                                      Collection *collection,
                                      const int id_create_flag);
/**
 * Free the object cache of all collections in given `bmain`, including master collections of
 * scenes.
 */
void BKE_main_collections_object_cache_free(const Main *bmain);

Base *BKE_collection_or_layer_objects(const Scene *scene,
                                      ViewLayer *view_layer,
                                      Collection *collection);

/* Editing. */

/**
 * Return Scene Collection for a given session_uid.
 */
Collection *BKE_collection_from_session_uid(Scene *scene, uint64_t session_uid);
/**
 * Return Collection for a given session_uid and its owner Scene.
 */
Collection *BKE_collection_from_session_uid(Main *bmain,
                                            uint64_t session_uid,
                                            Scene **r_scene = nullptr);

/**
 * The automatic/fallback name of a new collection.
 */
void BKE_collection_new_name_get(Collection *collection_parent,
                                 char r_name[/*MAX_ID_NAME - 2*/ 256]);
/**
 * The name to show in the interface.
 */
const char *BKE_collection_ui_name_get(Collection *collection);
/**
 * Select all the objects in this Collection (and its nested collections) for this ViewLayer.
 * Return true if any object was selected.
 */
bool BKE_collection_objects_select(const Scene *scene,
                                   ViewLayer *view_layer,
                                   Collection *collection,
                                   bool deselect);

/* Collection children */

bool BKE_collection_child_add(Main *bmain, Collection *parent, Collection *child);

bool BKE_collection_child_add_no_sync(Main *bmain, Collection *parent, Collection *child);

bool BKE_collection_child_remove(Main *bmain, Collection *parent, Collection *child);

bool BKE_collection_move(Main *bmain,
                         Collection *to_parent,
                         Collection *from_parent,
                         Collection *relative,
                         bool relative_after,
                         Collection *collection);

/**
 * Find potential cycles in collections.
 *
 * \param new_ancestor: the potential new owner of given \a collection,
 * or the collection to check if the later is NULL.
 * \param collection: the collection we want to add to \a new_ancestor,
 * may be NULL if we just want to ensure \a new_ancestor does not already have cycles.
 * \return true if a cycle is found.
 */
bool BKE_collection_cycle_find(Collection *new_ancestor, Collection *collection);
/**
 * Find and fix potential cycles in collections.
 *
 * \param collection: The collection to check for existing cycles.
 * \return true if cycles are found and fixed.
 */
bool BKE_collection_cycles_fix(Main *bmain, Collection *collection);

bool BKE_collection_has_collection(const Collection *parent, const Collection *collection);

/**
 * Return parent collection which is not linked.
 */
Collection *BKE_collection_parent_editable_find_recursive(const ViewLayer *view_layer,
                                                          Collection *collection);
/**
 * Rebuild parent relationships from child ones, for all children of given \a collection.
 *
 * \note Given collection is assumed to already have valid parents.
 */
void BKE_collection_parent_relations_rebuild(Collection *collection);
/**
 * Rebuild parent relationships from child ones, for all collections in given \a bmain.
 */
void BKE_main_collections_parent_relations_rebuild(Main *bmain);

/**
 * Perform some validation on integrity of the data of this collection.
 *
 * \return `true` if everything is OK, false if some errors are detected. */
bool BKE_collection_validate(Collection *collection);

/* .blend file I/O */

/**
 * Perform some pre-writing cleanup on the COllection data itself (_not_ in any sub-data
 * referenced by pointers). To be called before writing the Collection struct itself.
 */
void BKE_collection_blend_write_prepare_nolib(BlendWriter *writer, Collection *collection);
void BKE_collection_blend_write_nolib(BlendWriter *writer, Collection *collection);
void BKE_collection_blend_read_data(BlendDataReader *reader, Collection *collection, ID *owner_id);

/* Iteration callbacks. */

using BKE_scene_objects_Cb = void (*)(Object *ob, void *data);
using BKE_scene_collections_Cb = void (*)(Collection *ob, void *data);

/* Iteration over objects in collection. */

#define FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_BEGIN(_collection, _object, _mode) \
  { \
    int _base_flag = (_mode == DAG_EVAL_VIEWPORT) ? BASE_ENABLED_VIEWPORT : BASE_ENABLED_RENDER; \
    int _object_visibility_flag = (_mode == DAG_EVAL_VIEWPORT) ? OB_HIDE_VIEWPORT : \
                                                                 OB_HIDE_RENDER; \
    int _base_id = 0; \
    for (Base *_base = static_cast<Base *>(BKE_collection_object_cache_get(_collection).first); \
         _base; \
         _base = _base->next, _base_id++) \
    { \
      Object *_object = _base->object; \
      if ((_base->flag & _base_flag) && \
          (_object->visibility_flag & _object_visibility_flag) == 0) {

#define FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_END \
  } \
  } \
  } \
  ((void)0)

#define FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN(_collection, _object) \
  for (Base *_base = static_cast<Base *>(BKE_collection_object_cache_get(_collection).first); \
       _base; \
       _base = _base->next) \
  { \
    Object *_object = _base->object; \
    BLI_assert(_object != NULL);

#define FOREACH_COLLECTION_OBJECT_RECURSIVE_END \
  } \
  ((void)0)

/* Iteration over collections in scene. */

/**
 * Only use this in non-performance critical situations
 * (it iterates over all scene collections twice)
 */
void BKE_scene_collections_iterator_begin(BLI_Iterator *iter, void *data_in);
void BKE_scene_collections_iterator_next(BLI_Iterator *iter);
void BKE_scene_collections_iterator_end(BLI_Iterator *iter);

void BKE_scene_objects_iterator_begin(BLI_Iterator *iter, void *data_in);
void BKE_scene_objects_iterator_next(BLI_Iterator *iter);
void BKE_scene_objects_iterator_end(BLI_Iterator *iter);

/**
 * Iterate over objects in the scene based on a flag.
 *
 * \note The object->flag is tested against flag.
 */
struct SceneObjectsIteratorExData {
  Scene *scene;
  int flag;
  void *iter_data;
};

void BKE_scene_objects_iterator_begin_ex(BLI_Iterator *iter, void *data_in);
void BKE_scene_objects_iterator_next_ex(BLI_Iterator *iter);
void BKE_scene_objects_iterator_end_ex(BLI_Iterator *iter);

/**
 * Generate a new #GSet (or extend given `objects_gset` if not NULL) with all objects referenced by
 * all collections of given `scene`.
 *
 * \note This will include objects without a base currently
 * (because they would belong to excluded collections only e.g.).
 */
GSet *BKE_scene_objects_as_gset(Scene *scene, GSet *objects_gset);

#define FOREACH_SCENE_COLLECTION_BEGIN(scene, _instance) \
  ITER_BEGIN (BKE_scene_collections_iterator_begin, \
              BKE_scene_collections_iterator_next, \
              BKE_scene_collections_iterator_end, \
              scene, \
              Collection *, \
              _instance)

#define FOREACH_SCENE_COLLECTION_END ITER_END

#define FOREACH_COLLECTION_BEGIN(_bmain, _scene, Type, _instance) \
  { \
    Type _instance; \
    Collection *_instance_next; \
    bool is_scene_collection = (_scene) != NULL; \
\
    if (_scene) { \
      _instance_next = (_scene)->master_collection; \
    } \
    else { \
      _instance_next = static_cast<Collection *>((_bmain)->collections.first); \
    } \
\
    while ((_instance = _instance_next)) { \
      if (is_scene_collection) { \
        _instance_next = static_cast<Collection *>((_bmain)->collections.first); \
        is_scene_collection = false; \
      } \
      else { \
        _instance_next = static_cast<Collection *>(_instance->id.next); \
      }

#define FOREACH_COLLECTION_END \
  } \
  } \
  ((void)0)

#define FOREACH_SCENE_OBJECT_BEGIN(scene, _instance) \
  ITER_BEGIN (BKE_scene_objects_iterator_begin, \
              BKE_scene_objects_iterator_next, \
              BKE_scene_objects_iterator_end, \
              scene, \
              Object *, \
              _instance)

#define FOREACH_SCENE_OBJECT_END ITER_END
