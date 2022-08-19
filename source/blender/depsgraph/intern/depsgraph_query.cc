/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2013 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup depsgraph
 *
 * Implementation of Querying API
 */

#include "MEM_guardedalloc.h"

#include <cstring> /* XXX: memcpy */

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "BKE_action.h" /* XXX: BKE_pose_channel_find_name */
#include "BKE_customdata.h"
#include "BKE_idtype.h"
#include "BKE_main.h"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "RNA_access.h"
#include "RNA_path.h"
#include "RNA_prototypes.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "intern/depsgraph.h"
#include "intern/eval/deg_eval_copy_on_write.h"
#include "intern/node/deg_node_id.h"

namespace blender::deg {

static const ID *get_original_id(const ID *id)
{
  if (id == nullptr) {
    return nullptr;
  }
  if (id->orig_id == nullptr) {
    return id;
  }
  BLI_assert((id->tag & LIB_TAG_COPIED_ON_WRITE) != 0);
  return (ID *)id->orig_id;
}

static ID *get_original_id(ID *id)
{
  const ID *const_id = id;
  return const_cast<ID *>(get_original_id(const_id));
}

static const ID *get_evaluated_id(const Depsgraph *deg_graph, const ID *id)
{
  if (id == nullptr) {
    return nullptr;
  }
  /* TODO(sergey): This is a duplicate of Depsgraph::get_cow_id(),
   * but here we never do assert, since we don't know nature of the
   * incoming ID data-block. */
  const IDNode *id_node = deg_graph->find_id_node(id);
  if (id_node == nullptr) {
    return id;
  }
  return id_node->id_cow;
}

static ID *get_evaluated_id(const Depsgraph *deg_graph, ID *id)
{
  const ID *const_id = id;
  return const_cast<ID *>(get_evaluated_id(deg_graph, const_id));
}

}  // namespace blender::deg

namespace deg = blender::deg;

struct Scene *DEG_get_input_scene(const Depsgraph *graph)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);
  return deg_graph->scene;
}

struct ViewLayer *DEG_get_input_view_layer(const Depsgraph *graph)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);
  return deg_graph->view_layer;
}

struct Main *DEG_get_bmain(const Depsgraph *graph)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);
  return deg_graph->bmain;
}

eEvaluationMode DEG_get_mode(const Depsgraph *graph)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);
  return deg_graph->mode;
}

float DEG_get_ctime(const Depsgraph *graph)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);
  return deg_graph->ctime;
}

bool DEG_id_type_updated(const Depsgraph *graph, short id_type)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);
  return deg_graph->id_type_updated[BKE_idtype_idcode_to_index(id_type)] != 0;
}

bool DEG_id_type_any_updated(const Depsgraph *graph)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);

  /* Loop over all ID types. */
  for (char id_type_index : deg_graph->id_type_updated) {
    if (id_type_index) {
      return true;
    }
  }

  return false;
}

bool DEG_id_type_any_exists(const Depsgraph *depsgraph, short id_type)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(depsgraph);
  return deg_graph->id_type_exist[BKE_idtype_idcode_to_index(id_type)] != 0;
}

uint32_t DEG_get_eval_flags_for_id(const Depsgraph *graph, const ID *id)
{
  if (graph == nullptr) {
    /* Happens when converting objects to mesh from a python script
     * after modifying scene graph.
     *
     * Currently harmless because it's only called for temporary
     * objects which are out of the DAG anyway. */
    return 0;
  }

  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);
  const deg::IDNode *id_node = deg_graph->find_id_node(deg::get_original_id(id));
  if (id_node == nullptr) {
    /* TODO(sergey): Does it mean we need to check set scene? */
    return 0;
  }

  return id_node->eval_flags;
}

void DEG_get_customdata_mask_for_object(const Depsgraph *graph,
                                        Object *ob,
                                        CustomData_MeshMasks *r_mask)
{
  if (graph == nullptr) {
    /* Happens when converting objects to mesh from a python script
     * after modifying scene graph.
     *
     * Currently harmless because it's only called for temporary
     * objects which are out of the DAG anyway. */
    return;
  }

  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);
  const deg::IDNode *id_node = deg_graph->find_id_node(DEG_get_original_id(&ob->id));
  if (id_node == nullptr) {
    /* TODO(sergey): Does it mean we need to check set scene? */
    return;
  }

  r_mask->vmask |= id_node->customdata_masks.vert_mask;
  r_mask->emask |= id_node->customdata_masks.edge_mask;
  r_mask->fmask |= id_node->customdata_masks.face_mask;
  r_mask->lmask |= id_node->customdata_masks.loop_mask;
  r_mask->pmask |= id_node->customdata_masks.poly_mask;
}

Scene *DEG_get_evaluated_scene(const Depsgraph *graph)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);
  Scene *scene_cow = deg_graph->scene_cow;
  /* TODO(sergey): Shall we expand data-block here? Or is it OK to assume
   * that caller is OK with just a pointer in case scene is not updated yet? */
  BLI_assert(scene_cow != nullptr && deg::deg_copy_on_write_is_expanded(&scene_cow->id));
  return scene_cow;
}

ViewLayer *DEG_get_evaluated_view_layer(const Depsgraph *graph)
{
  const deg::Depsgraph *deg_graph = reinterpret_cast<const deg::Depsgraph *>(graph);
  Scene *scene_cow = DEG_get_evaluated_scene(graph);
  if (scene_cow == nullptr) {
    return nullptr; /* Happens with new, not-yet-built/evaluated graphs. */
  }
  /* Do name-based lookup. */
  /* TODO(sergey): Can this be optimized? */
  ViewLayer *view_layer_orig = deg_graph->view_layer;
  ViewLayer *view_layer_cow = (ViewLayer *)BLI_findstring(
      &scene_cow->view_layers, view_layer_orig->name, offsetof(ViewLayer, name));
  BLI_assert(view_layer_cow != nullptr);
  return view_layer_cow;
}

Object *DEG_get_evaluated_object(const Depsgraph *depsgraph, Object *object)
{
  return (Object *)DEG_get_evaluated_id(depsgraph, &object->id);
}

ID *DEG_get_evaluated_id(const Depsgraph *depsgraph, ID *id)
{
  return deg::get_evaluated_id(reinterpret_cast<const deg::Depsgraph *>(depsgraph), id);
}

void DEG_get_evaluated_rna_pointer(const Depsgraph *depsgraph,
                                   PointerRNA *ptr,
                                   PointerRNA *r_ptr_eval)
{
  if ((ptr == nullptr) || (r_ptr_eval == nullptr)) {
    return;
  }
  ID *orig_id = ptr->owner_id;
  ID *cow_id = DEG_get_evaluated_id(depsgraph, orig_id);
  if (ptr->owner_id == ptr->data) {
    /* For ID pointers, it's easy... */
    r_ptr_eval->owner_id = cow_id;
    r_ptr_eval->data = (void *)cow_id;
    r_ptr_eval->type = ptr->type;
  }
  else if (ptr->type == &RNA_PoseBone) {
    /* HACK: Since bone keyframing is quite commonly used,
     * speed things up for this case by doing a special lookup
     * for bones */
    const Object *ob_eval = (Object *)cow_id;
    bPoseChannel *pchan = (bPoseChannel *)ptr->data;
    const bPoseChannel *pchan_eval = BKE_pose_channel_find_name(ob_eval->pose, pchan->name);
    r_ptr_eval->owner_id = cow_id;
    r_ptr_eval->data = (void *)pchan_eval;
    r_ptr_eval->type = ptr->type;
  }
  else {
    /* For everything else, try to get RNA Path of the BMain-pointer,
     * then use that to look up what the COW-domain one should be
     * given the COW ID pointer as the new lookup point */
    /* TODO: Find a faster alternative, or implement support for other
     * common types too above (e.g. modifiers) */
    char *path = RNA_path_from_ID_to_struct(ptr);
    if (path) {
      PointerRNA cow_id_ptr;
      RNA_id_pointer_create(cow_id, &cow_id_ptr);
      if (!RNA_path_resolve(&cow_id_ptr, path, r_ptr_eval, nullptr)) {
        /* Couldn't find COW copy of data */
        fprintf(stderr,
                "%s: Couldn't resolve RNA path ('%s') relative to COW ID (%p) for '%s'\n",
                __func__,
                path,
                (void *)cow_id,
                orig_id->name);
      }
    }
    else {
      /* Path resolution failed - XXX: Hide this behind a debug flag */
      fprintf(stderr,
              "%s: Couldn't get RNA path for %s relative to %s\n",
              __func__,
              RNA_struct_identifier(ptr->type),
              orig_id->name);
    }
  }
}

Object *DEG_get_original_object(Object *object)
{
  return (Object *)DEG_get_original_id(&object->id);
}

ID *DEG_get_original_id(ID *id)
{
  return deg::get_original_id(id);
}

bool DEG_is_original_id(const ID *id)
{
  /* Some explanation of the logic.
   *
   * What we want here is to be able to tell whether given ID is a result of dependency graph
   * evaluation or not.
   *
   * All the data-blocks which are created by copy-on-write mechanism will have will be tagged with
   * LIB_TAG_COPIED_ON_WRITE tag. Those data-blocks can not be original.
   *
   * Modifier stack evaluation might create special data-blocks which have all the modifiers
   * applied, and those will be tagged with LIB_TAG_COPIED_ON_WRITE_EVAL_RESULT. Such data-blocks
   * can not be original as well.
   *
   * Localization is usually happening from evaluated data-block, or will have some special pointer
   * magic which will make them to act as evaluated.
   *
   * NOTE: We consider ID evaluated if ANY of those flags is set. We do NOT require ALL of them. */
  if (id->tag &
      (LIB_TAG_COPIED_ON_WRITE | LIB_TAG_COPIED_ON_WRITE_EVAL_RESULT | LIB_TAG_LOCALIZED)) {
    return false;
  }
  return true;
}

bool DEG_is_original_object(const Object *object)
{
  return DEG_is_original_id(&object->id);
}

bool DEG_is_evaluated_id(const ID *id)
{
  return !DEG_is_original_id(id);
}

bool DEG_is_evaluated_object(const Object *object)
{
  return !DEG_is_original_object(object);
}

bool DEG_is_fully_evaluated(const struct Depsgraph *depsgraph)
{
  const deg::Depsgraph *deg_graph = (const deg::Depsgraph *)depsgraph;
  /* Check whether relations are up to date. */
  if (deg_graph->need_update_relations) {
    return false;
  }
  /* Check whether IDs are up to date. */
  if (!deg_graph->entry_tags.is_empty()) {
    return false;
  }
  return true;
}
