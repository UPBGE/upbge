/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <string.h>

#include "BLI_array.h"

#include "BKE_collection.h"
#include "BKE_customdata.h"
#include "BKE_editmesh.h"
#include "BKE_layer.h"

#include "DNA_ID.h"
#include "DNA_layer_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

/* -------------------------------------------------------------------- */
/** \name Selected Object Array
 * \{ */

Object **BKE_view_layer_array_selected_objects_params(
    struct ViewLayer *view_layer,
    const struct View3D *v3d,
    uint *r_len,
    const struct ObjectsInViewLayerParams *params)
{
  if (params->no_dup_data) {
    FOREACH_SELECTED_OBJECT_BEGIN (view_layer, v3d, ob_iter) {
      ID *id = ob_iter->data;
      if (id) {
        id->tag |= LIB_TAG_DOIT;
      }
    }
    FOREACH_SELECTED_OBJECT_END;
  }

  Object **object_array = NULL;
  BLI_array_declare(object_array);

  FOREACH_SELECTED_OBJECT_BEGIN (view_layer, v3d, ob_iter) {
    if (params->filter_fn) {
      if (!params->filter_fn(ob_iter, params->filter_userdata)) {
        continue;
      }
    }

    if (params->no_dup_data) {
      ID *id = ob_iter->data;
      if (id) {
        if (id->tag & LIB_TAG_DOIT) {
          id->tag &= ~LIB_TAG_DOIT;
        }
        else {
          continue;
        }
      }
    }

    BLI_array_append(object_array, ob_iter);
  }
  FOREACH_SELECTED_OBJECT_END;

  if (object_array != NULL) {
    BLI_array_trim(object_array);
  }
  else {
    /* We always need a valid allocation (prevent crash on free). */
    object_array = MEM_mallocN(0, __func__);
  }
  *r_len = BLI_array_len(object_array);
  return object_array;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Objects in Mode Array
 * \{ */

Base **BKE_view_layer_array_from_bases_in_mode_params(ViewLayer *view_layer,
                                                      const View3D *v3d,
                                                      uint *r_len,
                                                      const struct ObjectsInModeParams *params)
{
  if (params->no_dup_data) {
    FOREACH_BASE_IN_MODE_BEGIN (view_layer, v3d, -1, params->object_mode, base_iter) {
      ID *id = base_iter->object->data;
      if (id) {
        id->tag |= LIB_TAG_DOIT;
      }
    }
    FOREACH_BASE_IN_MODE_END;
  }

  Base **base_array = NULL;
  BLI_array_declare(base_array);

  FOREACH_BASE_IN_MODE_BEGIN (view_layer, v3d, -1, params->object_mode, base_iter) {
    if (params->filter_fn) {
      if (!params->filter_fn(base_iter->object, params->filter_userdata)) {
        continue;
      }
    }
    if (params->no_dup_data) {
      ID *id = base_iter->object->data;
      if (id) {
        if (id->tag & LIB_TAG_DOIT) {
          id->tag &= ~LIB_TAG_DOIT;
        }
        else {
          continue;
        }
      }
    }
    BLI_array_append(base_array, base_iter);
  }
  FOREACH_BASE_IN_MODE_END;

  /* We always need a valid allocation (prevent crash on free). */
  if (base_array != NULL) {
    BLI_array_trim(base_array);
  }
  else {
    base_array = MEM_mallocN(0, __func__);
  }
  *r_len = BLI_array_len(base_array);
  return base_array;
}

Object **BKE_view_layer_array_from_objects_in_mode_params(ViewLayer *view_layer,
                                                          const View3D *v3d,
                                                          uint *r_len,
                                                          const struct ObjectsInModeParams *params)
{
  Base **base_array = BKE_view_layer_array_from_bases_in_mode_params(
      view_layer, v3d, r_len, params);
  if (base_array != NULL) {
    for (uint i = 0; i < *r_len; i++) {
      ((Object **)base_array)[i] = base_array[i]->object;
    }
  }
  return (Object **)base_array;
}

struct Object **BKE_view_layer_array_from_objects_in_edit_mode(ViewLayer *view_layer,
                                                               const View3D *v3d,
                                                               uint *r_len)
{
  struct ObjectsInModeParams params = {0};
  params.object_mode = OB_MODE_EDIT;
  return BKE_view_layer_array_from_objects_in_mode_params(view_layer, v3d, r_len, &params);
}

struct Base **BKE_view_layer_array_from_bases_in_edit_mode(ViewLayer *view_layer,
                                                           const View3D *v3d,
                                                           uint *r_len)
{
  struct ObjectsInModeParams params = {0};
  params.object_mode = OB_MODE_EDIT;
  return BKE_view_layer_array_from_bases_in_mode_params(view_layer, v3d, r_len, &params);
}

struct Object **BKE_view_layer_array_from_objects_in_edit_mode_unique_data(ViewLayer *view_layer,
                                                                           const View3D *v3d,
                                                                           uint *r_len)
{
  struct ObjectsInModeParams params = {0};
  params.object_mode = OB_MODE_EDIT;
  params.no_dup_data = true;
  return BKE_view_layer_array_from_objects_in_mode_params(view_layer, v3d, r_len, &params);
}

struct Base **BKE_view_layer_array_from_bases_in_edit_mode_unique_data(ViewLayer *view_layer,
                                                                       const View3D *v3d,
                                                                       uint *r_len)
{
  struct ObjectsInModeParams params = {0};
  params.object_mode = OB_MODE_EDIT;
  params.no_dup_data = true;
  return BKE_view_layer_array_from_bases_in_mode_params(view_layer, v3d, r_len, &params);
}

struct Object **BKE_view_layer_array_from_objects_in_edit_mode_unique_data_with_uvs(
    ViewLayer *view_layer, const View3D *v3d, uint *r_len)
{
  struct ObjectsInModeParams params = {0};
  params.object_mode = OB_MODE_EDIT;
  params.no_dup_data = true;
  params.filter_fn = BKE_view_layer_filter_edit_mesh_has_uvs;
  return BKE_view_layer_array_from_objects_in_mode_params(view_layer, v3d, r_len, &params);
}

struct Object **BKE_view_layer_array_from_objects_in_mode_unique_data(ViewLayer *view_layer,
                                                                      const View3D *v3d,
                                                                      uint *r_len,
                                                                      const eObjectMode mode)
{
  struct ObjectsInModeParams params = {0};
  params.object_mode = mode;
  params.no_dup_data = true;
  return BKE_view_layer_array_from_objects_in_mode_params(view_layer, v3d, r_len, &params);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Filter Functions
 * \{ */

bool BKE_view_layer_filter_edit_mesh_has_uvs(const Object *ob, void *UNUSED(user_data))
{
  if (ob->type == OB_MESH) {
    const Mesh *me = ob->data;
    const BMEditMesh *em = me->edit_mesh;
    if (em != NULL) {
      if (CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV) != -1) {
        return true;
      }
    }
  }
  return false;
}

bool BKE_view_layer_filter_edit_mesh_has_edges(const Object *ob, void *UNUSED(user_data))
{
  if (ob->type == OB_MESH) {
    const Mesh *me = ob->data;
    const BMEditMesh *em = me->edit_mesh;
    if (em != NULL) {
      if (em->bm->totedge != 0) {
        return true;
      }
    }
  }
  return false;
}

Object *BKE_view_layer_non_active_selected_object(struct ViewLayer *view_layer,
                                                  const struct View3D *v3d)
{
  Object *ob_active = OBACT(view_layer);
  Object *ob_result = NULL;
  FOREACH_SELECTED_OBJECT_BEGIN (view_layer, v3d, ob_iter) {
    if (ob_iter == ob_active) {
      continue;
    }

    if (ob_result == NULL) {
      ob_result = ob_iter;
    }
    else {
      ob_result = NULL;
      break;
    }
  }
  FOREACH_SELECTED_OBJECT_END;
  return ob_result;
}

/** \} */
