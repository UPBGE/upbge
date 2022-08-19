/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2016 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

#include "BLI_linklist.h"

/* Required for proxy to liboverrides conversion code. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_collection_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "DEG_depsgraph.h"

#include "BKE_collection.h"
#include "BKE_idtype.h"
#include "BKE_lib_id.h"
#include "BKE_lib_override.h"
#include "BKE_main.h"

#include "BLO_readfile.h"

static CLG_LogRef LOG = {"bke.liboverride_proxy_conversion"};

bool BKE_lib_override_library_proxy_convert(Main *bmain,
                                            Scene *scene,
                                            ViewLayer *view_layer,
                                            Object *ob_proxy)
{
  /* `proxy_group`, if defined, is the empty instantiating the collection from which the proxy is
   * coming. */
  Object *ob_proxy_group = ob_proxy->proxy_group;
  const bool is_override_instancing_object = (ob_proxy_group != NULL) &&
                                             (ob_proxy_group->instance_collection != NULL);
  ID *id_root = is_override_instancing_object ? &ob_proxy_group->instance_collection->id :
                                                &ob_proxy->proxy->id;
  ID *id_instance_hint = is_override_instancing_object ? &ob_proxy_group->id : &ob_proxy->id;

  /* In some cases the instance collection of a proxy object may be local (see e.g. T83875). Not
   * sure this is a valid state, but for now just abort the overriding process. */
  if (!ID_IS_OVERRIDABLE_LIBRARY_HIERARCHY(id_root)) {
    if (ob_proxy->proxy != NULL) {
      ob_proxy->proxy->proxy_from = NULL;
    }
    id_us_min((ID *)ob_proxy->proxy);
    ob_proxy->proxy = ob_proxy->proxy_group = NULL;
    return false;
  }

  /* We manually convert the proxy object into a library override, further override handling will
   * then be handled by `BKE_lib_override_library_create()` just as for a regular override
   * creation.
   */
  ob_proxy->proxy->id.tag |= LIB_TAG_DOIT;
  ob_proxy->proxy->id.newid = &ob_proxy->id;
  BKE_lib_override_library_init(&ob_proxy->id, &ob_proxy->proxy->id);
  ob_proxy->id.override_library->flag &= ~IDOVERRIDE_LIBRARY_FLAG_SYSTEM_DEFINED;

  ob_proxy->proxy->proxy_from = NULL;
  ob_proxy->proxy = ob_proxy->proxy_group = NULL;

  DEG_id_tag_update(&ob_proxy->id, ID_RECALC_COPY_ON_WRITE);

  /* In case of proxy conversion, remap all local ID usages to linked IDs to their newly created
   * overrides. Also do that for the IDs from the same lib as the proxy in case it is linked.
   * While this might not be 100% the desired behavior, it is likely to be the case most of the
   * time. Ref: T91711. */
  ID *id_iter;
  FOREACH_MAIN_ID_BEGIN (bmain, id_iter) {
    if (!ID_IS_LINKED(id_iter) || id_iter->lib == ob_proxy->id.lib) {
      id_iter->tag |= LIB_TAG_DOIT;
    }
  }
  FOREACH_MAIN_ID_END;

  return BKE_lib_override_library_create(
      bmain, scene, view_layer, ob_proxy->id.lib, id_root, id_root, id_instance_hint, NULL, false);
}

static void lib_override_library_proxy_convert_do(Main *bmain,
                                                  Scene *scene,
                                                  Object *ob_proxy,
                                                  BlendFileReadReport *reports)
{
  Object *ob_proxy_group = ob_proxy->proxy_group;
  const bool is_override_instancing_object = ob_proxy_group != NULL;

  const bool success = BKE_lib_override_library_proxy_convert(bmain, scene, NULL, ob_proxy);

  if (success) {
    CLOG_INFO(&LOG,
              4,
              "Proxy object '%s' successfully converted to library overrides",
              ob_proxy->id.name);
    /* Remove the instance empty from this scene, the items now have an overridden collection
     * instead. */
    if (is_override_instancing_object) {
      BKE_scene_collections_object_remove(bmain, scene, ob_proxy_group, true);
    }
    reports->count.proxies_to_lib_overrides_success++;
  }
}

void BKE_lib_override_library_main_proxy_convert(Main *bmain, BlendFileReadReport *reports)
{
  LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
    LinkNodePair proxy_objects = {NULL};

    FOREACH_SCENE_OBJECT_BEGIN (scene, object) {
      if (object->proxy_group != NULL) {
        BLI_linklist_append(&proxy_objects, object);
      }
    }
    FOREACH_SCENE_OBJECT_END;

    FOREACH_SCENE_OBJECT_BEGIN (scene, object) {
      if (object->proxy != NULL && object->proxy_group == NULL) {
        BLI_linklist_append(&proxy_objects, object);
      }
    }
    FOREACH_SCENE_OBJECT_END;

    for (LinkNode *proxy_object_iter = proxy_objects.list; proxy_object_iter != NULL;
         proxy_object_iter = proxy_object_iter->next) {
      Object *proxy_object = proxy_object_iter->link;
      lib_override_library_proxy_convert_do(bmain, scene, proxy_object, reports);
    }

    BLI_linklist_free(proxy_objects.list, NULL);
  }

  LISTBASE_FOREACH (Object *, object, &bmain->objects) {
    if (object->proxy_group != NULL || object->proxy != NULL) {
      if (ID_IS_LINKED(object)) {
        CLOG_WARN(&LOG,
                  "Linked proxy object '%s' from '%s' failed to be converted to library override",
                  object->id.name + 2,
                  object->id.lib->filepath);
      }
      else {
        CLOG_WARN(&LOG,
                  "Proxy object '%s' failed to be converted to library override",
                  object->id.name + 2);
      }
      reports->count.proxies_to_lib_overrides_failures++;
      if (object->proxy != NULL) {
        object->proxy->proxy_from = NULL;
      }
      id_us_min((ID *)object->proxy);
      object->proxy = object->proxy_group = NULL;
    }
  }
}
