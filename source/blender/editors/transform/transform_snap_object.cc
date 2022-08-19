/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <cstdlib>

#include "MEM_guardedalloc.h"

#include "BLI_float4x4.hh"
#include "BLI_kdopbvh.h"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math.h"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

#include "DNA_armature_types.h"
#include "DNA_curve_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "BKE_armature.h"
#include "BKE_bvhutils.h"
#include "BKE_curve.h"
#include "BKE_duplilist.h"
#include "BKE_editmesh.h"
#include "BKE_geometry_set.h"
#include "BKE_global.h"
#include "BKE_layer.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_object.h"
#include "BKE_tracking.h"

#include "DEG_depsgraph_query.h"

#include "ED_armature.h"
#include "ED_transform_snap_object_context.h"
#include "ED_view3d.h"

using blender::float3;
using blender::float4x4;
using blender::Map;

/* -------------------------------------------------------------------- */
/** \name Internal Data Types
 * \{ */

#define MAX_CLIPPLANE_LEN 3

enum eViewProj {
  VIEW_PROJ_NONE = -1,
  VIEW_PROJ_ORTHO = 0,
  VIEW_PROJ_PERSP = -1,
};

/* SnapObjectContext.cache.mesh_map */
struct SnapData_Mesh {
  /* Loose edges, loose verts. */
  BVHTree *bvhtree[2];
  bool cached[2];

  /* Looptris. */
  BVHTreeFromMesh treedata_mesh;

  const MPoly *poly;
  bool has_looptris;
  bool has_loose_edge;
  bool has_loose_vert;

  void clear()
  {
    for (int i = 0; i < ARRAY_SIZE(this->bvhtree); i++) {
      if (!this->cached[i]) {
        BLI_bvhtree_free(this->bvhtree[i]);
      }
      this->bvhtree[i] = nullptr;
    }
    free_bvhtree_from_mesh(&this->treedata_mesh);
  }

  ~SnapData_Mesh()
  {
    this->clear();
  }

#ifdef WITH_CXX_GUARDEDALLOC
  MEM_CXX_CLASS_ALLOC_FUNCS("SnapData_Mesh")
#endif
};

/* SnapObjectContext.cache.editmesh_map */
struct SnapData_EditMesh {
  /* Verts, Edges. */
  BVHTree *bvhtree[2];
  bool cached[2];

  /* Looptris. */
  BVHTreeFromEditMesh treedata_editmesh;

  struct Mesh_Runtime *mesh_runtime;
  float min[3], max[3];

  void clear()
  {
    for (int i = 0; i < ARRAY_SIZE(this->bvhtree); i++) {
      if (!this->cached[i]) {
        BLI_bvhtree_free(this->bvhtree[i]);
      }
      this->bvhtree[i] = nullptr;
    }
    free_bvhtree_from_editmesh(&this->treedata_editmesh);
  }

  ~SnapData_EditMesh()
  {
    this->clear();
  }

#ifdef WITH_CXX_GUARDEDALLOC
  MEM_CXX_CLASS_ALLOC_FUNCS("SnapData_EditMesh")
#endif
};

struct SnapObjectContext {
  Scene *scene;

  int flag;

  Map<const Object *, std::unique_ptr<SnapData_Mesh>> mesh_caches;
  Map<const BMEditMesh *, std::unique_ptr<SnapData_EditMesh>> editmesh_caches;

  /* Filter data, returns true to check this value */
  struct {
    struct {
      bool (*test_vert_fn)(BMVert *, void *user_data);
      bool (*test_edge_fn)(BMEdge *, void *user_data);
      bool (*test_face_fn)(BMFace *, void *user_data);
      void *user_data;
    } edit_mesh;
  } callbacks;

  struct {
    Depsgraph *depsgraph;
    const ARegion *region;
    const View3D *v3d;

    float mval[2];
    float pmat[4][4];  /* perspective matrix */
    float win_size[2]; /* win x and y */
    enum eViewProj view_proj;
    float clip_plane[MAX_CLIPPLANE_LEN][4];
    short clip_plane_len;
    eSnapMode snap_to_flag;
    bool has_occlusion_plane; /* Ignore plane of occlusion in curves. */
  } runtime;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Utilities
 * \{ */

/**
 * Mesh used for snapping.
 *
 * - When the return value is null the `BKE_editmesh_from_object(ob_eval)` should be used.
 * - In rare cases there is no evaluated mesh available and a null result doesn't imply an
 *   edit-mesh, so callers need to account for a null edit-mesh too, see: T96536.
 */
static const Mesh *mesh_for_snap(Object *ob_eval, eSnapEditType edit_mode_type, bool *r_use_hide)
{
  const Mesh *me_eval = BKE_object_get_evaluated_mesh(ob_eval);
  bool use_hide = false;
  if (BKE_object_is_in_editmode(ob_eval)) {
    if (edit_mode_type == SNAP_GEOM_EDIT) {
      return nullptr;
    }

    const Mesh *editmesh_eval_final = BKE_object_get_editmesh_eval_final(ob_eval);
    const Mesh *editmesh_eval_cage = BKE_object_get_editmesh_eval_cage(ob_eval);

    if ((edit_mode_type == SNAP_GEOM_FINAL) && editmesh_eval_final) {
      if (editmesh_eval_final->runtime.wrapper_type == ME_WRAPPER_TYPE_BMESH) {
        return nullptr;
      }
      me_eval = editmesh_eval_final;
      use_hide = true;
    }
    else if ((edit_mode_type == SNAP_GEOM_CAGE) && editmesh_eval_cage) {
      if (editmesh_eval_cage->runtime.wrapper_type == ME_WRAPPER_TYPE_BMESH) {
        return nullptr;
      }
      me_eval = editmesh_eval_cage;
      use_hide = true;
    }
  }
  if (r_use_hide) {
    *r_use_hide = use_hide;
  }
  return me_eval;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Object Data
 * \{ */

/**
 * Calculate the minimum and maximum coordinates of the box that encompasses this mesh.
 */
static void snap_editmesh_minmax(SnapObjectContext *sctx,
                                 BMesh *bm,
                                 float r_min[3],
                                 float r_max[3])
{
  INIT_MINMAX(r_min, r_max);
  BMIter iter;
  BMVert *v;

  BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
    if (sctx->callbacks.edit_mesh.test_vert_fn &&
        !sctx->callbacks.edit_mesh.test_vert_fn(v, sctx->callbacks.edit_mesh.user_data)) {
      continue;
    }
    minmax_v3v3_v3(r_min, r_max, v->co);
  }
}

static SnapData_Mesh *snap_object_data_mesh_get(SnapObjectContext *sctx,
                                                Object *ob_eval,
                                                const Mesh *me_eval,
                                                bool use_hide)
{
  SnapData_Mesh *sod;
  bool init = false;

  if (std::unique_ptr<SnapData_Mesh> *sod_p = sctx->mesh_caches.lookup_ptr(ob_eval)) {
    sod = sod_p->get();
    bool is_dirty = false;
    if (sod->treedata_mesh.tree && sod->treedata_mesh.cached &&
        !bvhcache_has_tree(me_eval->runtime.bvh_cache, sod->treedata_mesh.tree)) {
      /* The tree is owned by the Mesh and may have been freed since we last used. */
      is_dirty = true;
    }
    else if (sod->bvhtree[0] && sod->cached[0] &&
             !bvhcache_has_tree(me_eval->runtime.bvh_cache, sod->bvhtree[0])) {
      /* The tree is owned by the Mesh and may have been freed since we last used. */
      is_dirty = true;
    }
    else if (sod->bvhtree[1] && sod->cached[1] &&
             !bvhcache_has_tree(me_eval->runtime.bvh_cache, sod->bvhtree[1])) {
      /* The tree is owned by the Mesh and may have been freed since we last used. */
      is_dirty = true;
    }
    else if (sod->treedata_mesh.looptri != me_eval->runtime.looptris.array) {
      is_dirty = true;
    }
    else if (sod->treedata_mesh.vert != me_eval->mvert) {
      is_dirty = true;
    }
    else if (sod->treedata_mesh.loop != me_eval->mloop) {
      is_dirty = true;
    }
    else if (sod->treedata_mesh.edge != me_eval->medge) {
      is_dirty = true;
    }
    else if (sod->poly != me_eval->mpoly) {
      is_dirty = true;
    }

    if (is_dirty) {
      sod->clear();
      init = true;
    }
  }
  else {
    if (ob_eval->type == OB_MESH) {
      /* Any existing #SnapData_EditMesh is now invalid. */
      sctx->editmesh_caches.remove(BKE_editmesh_from_object(ob_eval));
    }

    std::unique_ptr<SnapData_Mesh> sod_ptr = std::make_unique<SnapData_Mesh>();
    sod = sod_ptr.get();
    sctx->mesh_caches.add_new(ob_eval, std::move(sod_ptr));

    init = true;
  }

  if (init) {
    /* The BVHTree from looptris is always required. */
    BLI_assert(sod->treedata_mesh.tree == nullptr);
    BKE_bvhtree_from_mesh_get(&sod->treedata_mesh,
                              me_eval,
                              use_hide ? BVHTREE_FROM_LOOPTRI_NO_HIDDEN : BVHTREE_FROM_LOOPTRI,
                              4);

    BLI_assert(sod->treedata_mesh.vert == me_eval->mvert);
    BLI_assert(!me_eval->mvert || sod->treedata_mesh.vert_normals);
    BLI_assert(sod->treedata_mesh.loop == me_eval->mloop);
    BLI_assert(!me_eval->mpoly || sod->treedata_mesh.looptri);

    sod->has_looptris = sod->treedata_mesh.tree != nullptr;

    /* Required for snapping with occlusion. */
    sod->treedata_mesh.edge = me_eval->medge;
    sod->poly = me_eval->mpoly;

    /* Start assuming that it has each of these element types. */
    sod->has_loose_edge = true;
    sod->has_loose_vert = true;
  }

  return sod;
}

/* Searches for the #Mesh_Runtime associated with the object that is most likely to be updated due
 * to changes in the `edit_mesh`. */
static Mesh_Runtime *snap_object_data_editmesh_runtime_get(Object *ob_eval)
{
  Mesh *editmesh_eval_final = BKE_object_get_editmesh_eval_final(ob_eval);
  if (editmesh_eval_final) {
    return &editmesh_eval_final->runtime;
  }

  Mesh *editmesh_eval_cage = BKE_object_get_editmesh_eval_cage(ob_eval);
  if (editmesh_eval_cage) {
    return &editmesh_eval_cage->runtime;
  }

  return &((Mesh *)ob_eval->data)->runtime;
}

static SnapData_EditMesh *snap_object_data_editmesh_get(SnapObjectContext *sctx,
                                                        Object *ob_eval,
                                                        BMEditMesh *em)
{
  SnapData_EditMesh *sod;
  bool init = false;

  /* Any existing #SnapData_Mesh is now invalid. */
  sctx->mesh_caches.remove(ob_eval);

  if (std::unique_ptr<SnapData_EditMesh> *sod_p = sctx->editmesh_caches.lookup_ptr(em)) {
    sod = sod_p->get();
    bool is_dirty = false;
    /* Check if the geometry has changed. */
    if (sod->treedata_editmesh.em != em) {
      is_dirty = true;
    }
    else if (sod->mesh_runtime) {
      if (sod->mesh_runtime != snap_object_data_editmesh_runtime_get(ob_eval)) {
        if (G.moving) {
          /* Hack to avoid updating while transforming. */
          BLI_assert(!sod->treedata_editmesh.cached && !sod->cached[0] && !sod->cached[1]);
          sod->mesh_runtime = snap_object_data_editmesh_runtime_get(ob_eval);
        }
        else {
          is_dirty = true;
        }
      }
      else if (sod->treedata_editmesh.tree && sod->treedata_editmesh.cached &&
               !bvhcache_has_tree(sod->mesh_runtime->bvh_cache, sod->treedata_editmesh.tree)) {
        /* The tree is owned by the EditMesh and may have been freed since we last used! */
        is_dirty = true;
      }
      else if (sod->bvhtree[0] && sod->cached[0] &&
               !bvhcache_has_tree(sod->mesh_runtime->bvh_cache, sod->bvhtree[0])) {
        /* The tree is owned by the EditMesh and may have been freed since we last used! */
        is_dirty = true;
      }
      else if (sod->bvhtree[1] && sod->cached[1] &&
               !bvhcache_has_tree(sod->mesh_runtime->bvh_cache, sod->bvhtree[1])) {
        /* The tree is owned by the EditMesh and may have been freed since we last used! */
        is_dirty = true;
      }
    }

    if (is_dirty) {
      sod->clear();
      init = true;
    }
  }
  else {
    std::unique_ptr<SnapData_EditMesh> sod_ptr = std::make_unique<SnapData_EditMesh>();
    sod = sod_ptr.get();
    sctx->editmesh_caches.add_new(em, std::move(sod_ptr));
    init = true;
  }

  if (init) {
    sod->treedata_editmesh.em = em;
    sod->mesh_runtime = snap_object_data_editmesh_runtime_get(ob_eval);
    snap_editmesh_minmax(sctx, em->bm, sod->min, sod->max);
  }

  return sod;
}

static BVHTreeFromMesh *snap_object_data_mesh_treedata_get(SnapObjectContext *sctx,
                                                           Object *ob_eval,
                                                           const Mesh *me_eval,
                                                           bool use_hide)
{
  SnapData_Mesh *sod = snap_object_data_mesh_get(sctx, ob_eval, me_eval, use_hide);
  return &sod->treedata_mesh;
}

static BVHTreeFromEditMesh *snap_object_data_editmesh_treedata_get(SnapObjectContext *sctx,
                                                                   Object *ob_eval,
                                                                   BMEditMesh *em)
{
  SnapData_EditMesh *sod = snap_object_data_editmesh_get(sctx, ob_eval, em);

  BVHTreeFromEditMesh *treedata = &sod->treedata_editmesh;

  if (treedata->tree == nullptr) {
    /* Operators only update the editmesh looptris of the original mesh. */
    BLI_assert(sod->treedata_editmesh.em ==
               BKE_editmesh_from_object(DEG_get_original_object(ob_eval)));
    em = sod->treedata_editmesh.em;

    if (sctx->callbacks.edit_mesh.test_face_fn) {
      BMesh *bm = em->bm;
      BLI_assert(poly_to_tri_count(bm->totface, bm->totloop) == em->tottri);

      BLI_bitmap *elem_mask = BLI_BITMAP_NEW(em->tottri, __func__);
      int looptri_num_active = BM_iter_mesh_bitmap_from_filter_tessface(
          bm,
          elem_mask,
          sctx->callbacks.edit_mesh.test_face_fn,
          sctx->callbacks.edit_mesh.user_data);

      bvhtree_from_editmesh_looptri_ex(treedata, em, elem_mask, looptri_num_active, 0.0f, 4, 6);

      MEM_freeN(elem_mask);
    }
    else {
      /* Only cache if BVH-tree is created without a mask.
       * This helps keep a standardized BVH-tree in cache. */
      BKE_bvhtree_from_editmesh_get(treedata,
                                    em,
                                    4,
                                    BVHTREE_FROM_EM_LOOPTRI,
                                    &sod->mesh_runtime->bvh_cache,
                                    static_cast<ThreadMutex *>(sod->mesh_runtime->eval_mutex));
    }
  }
  if (treedata == nullptr || treedata->tree == nullptr) {
    return nullptr;
  }

  return treedata;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Iterator
 * \{ */

using IterSnapObjsCallback = void (*)(SnapObjectContext *sctx,
                                      const SnapObjectParams *params,
                                      Object *ob_eval,
                                      const float obmat[4][4],
                                      bool is_object_active,
                                      void *data);

static bool snap_object_is_snappable(const SnapObjectContext *sctx,
                                     const eSnapTargetSelect snap_target_select,
                                     const Base *base_act,
                                     const Base *base)
{
  if (!BASE_VISIBLE(sctx->runtime.v3d, base)) {
    return false;
  }

  if ((snap_target_select == SCE_SNAP_TARGET_ALL) ||
      (base->flag_legacy & BA_TRANSFORM_LOCKED_IN_PLACE)) {
    return true;
  }

  if (base->flag_legacy & BA_SNAP_FIX_DEPS_FIASCO) {
    return false;
  }

  /* Get attributes of potential target. */
  const bool is_active = (base_act == base);
  const bool is_selected = (base->flag & BASE_SELECTED) || (base->flag_legacy & BA_WAS_SEL);
  const bool is_edited = (base->object->mode == OB_MODE_EDIT);
  const bool is_selectable = (base->flag & BASE_SELECTABLE);
  /* Get attributes of state. */
  const bool is_in_object_mode = (base_act == nullptr) ||
                                 (base_act->object->mode == OB_MODE_OBJECT);

  if (is_in_object_mode) {
    /* Handle target selection options that make sense for object mode. */
    if ((snap_target_select & SCE_SNAP_TARGET_NOT_SELECTED) && is_selected) {
      /* What is selectable or not is part of the object and depends on the mode. */
      return false;
    }
  }
  else {
    /* Handle target selection options that make sense for edit/pose mode. */
    if ((snap_target_select & SCE_SNAP_TARGET_NOT_ACTIVE) && is_active) {
      return false;
    }
    if ((snap_target_select & SCE_SNAP_TARGET_NOT_EDITED) && is_edited && !is_active) {
      /* Base is edited, but not active. */
      return false;
    }
    if ((snap_target_select & SCE_SNAP_TARGET_NOT_NONEDITED) && !is_edited) {
      return false;
    }
  }

  if ((snap_target_select & SCE_SNAP_TARGET_ONLY_SELECTABLE) && !is_selectable) {
    return false;
  }

  return true;
}

/**
 * Walks through all objects in the scene to create the list of objects to snap.
 */
static void iter_snap_objects(SnapObjectContext *sctx,
                              const SnapObjectParams *params,
                              IterSnapObjsCallback sob_callback,
                              void *data)
{
  ViewLayer *view_layer = DEG_get_input_view_layer(sctx->runtime.depsgraph);
  const eSnapTargetSelect snap_target_select = params->snap_target_select;
  Base *base_act = view_layer->basact;

  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    if (!snap_object_is_snappable(sctx, snap_target_select, base_act, base)) {
      continue;
    }

    const bool is_object_active = (base == base_act);
    Object *obj_eval = DEG_get_evaluated_object(sctx->runtime.depsgraph, base->object);
    if (obj_eval->transflag & OB_DUPLI || BKE_object_has_geometry_set_instances(obj_eval)) {
      ListBase *lb = object_duplilist(sctx->runtime.depsgraph, sctx->scene, obj_eval);
      LISTBASE_FOREACH (DupliObject *, dupli_ob, lb) {
        BLI_assert(DEG_is_evaluated_object(dupli_ob->ob));
        sob_callback(sctx, params, dupli_ob->ob, dupli_ob->mat, is_object_active, data);
      }
      free_object_duplilist(lb);
    }

    sob_callback(sctx, params, obj_eval, obj_eval->obmat, is_object_active, data);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Ray Cast Funcs
 * \{ */

/* Store all ray-hits
 * Support for storing all depths, not just the first (ray-cast 'all'). */

struct RayCastAll_Data {
  void *bvhdata;

  /* internal vars for adding depths */
  BVHTree_RayCastCallback raycast_callback;

  const float (*obmat)[4];
  const float (*timat)[3];

  float len_diff;
  float local_scale;

  Object *ob_eval;
  uint ob_uuid;

  /* output data */
  ListBase *hit_list;
  bool retval;
};

static SnapObjectHitDepth *hit_depth_create(const float depth,
                                            const float co[3],
                                            const float no[3],
                                            int index,
                                            Object *ob_eval,
                                            const float obmat[4][4],
                                            uint ob_uuid)
{
  SnapObjectHitDepth *hit = MEM_new<SnapObjectHitDepth>(__func__);

  hit->depth = depth;
  copy_v3_v3(hit->co, co);
  copy_v3_v3(hit->no, no);
  hit->index = index;

  hit->ob_eval = ob_eval;
  copy_m4_m4(hit->obmat, (float(*)[4])obmat);
  hit->ob_uuid = ob_uuid;

  return hit;
}

static int hit_depth_cmp(const void *arg1, const void *arg2)
{
  const SnapObjectHitDepth *h1 = static_cast<const SnapObjectHitDepth *>(arg1);
  const SnapObjectHitDepth *h2 = static_cast<const SnapObjectHitDepth *>(arg2);
  int val = 0;

  if (h1->depth < h2->depth) {
    val = -1;
  }
  else if (h1->depth > h2->depth) {
    val = 1;
  }

  return val;
}

static void raycast_all_cb(void *userdata, int index, const BVHTreeRay *ray, BVHTreeRayHit *hit)
{
  RayCastAll_Data *data = static_cast<RayCastAll_Data *>(userdata);
  data->raycast_callback(data->bvhdata, index, ray, hit);
  if (hit->index != -1) {
    /* Get all values in world-space. */
    float location[3], normal[3];
    float depth;

    /* World-space location. */
    mul_v3_m4v3(location, (float(*)[4])data->obmat, hit->co);
    depth = (hit->dist + data->len_diff) / data->local_scale;

    /* World-space normal. */
    copy_v3_v3(normal, hit->no);
    mul_m3_v3((float(*)[3])data->timat, normal);
    normalize_v3(normal);

    SnapObjectHitDepth *hit_item = hit_depth_create(
        depth, location, normal, hit->index, data->ob_eval, data->obmat, data->ob_uuid);
    BLI_addtail(data->hit_list, hit_item);
  }
}

static bool raycast_tri_backface_culling_test(
    const float dir[3], const float v0[3], const float v1[3], const float v2[3], float no[3])
{
  cross_tri_v3(no, v0, v1, v2);
  return dot_v3v3(no, dir) < 0.0f;
}

/* Callback to ray-cast with back-face culling (#Mesh). */
static void mesh_looptri_raycast_backface_culling_cb(void *userdata,
                                                     int index,
                                                     const BVHTreeRay *ray,
                                                     BVHTreeRayHit *hit)
{
  const BVHTreeFromMesh *data = (BVHTreeFromMesh *)userdata;
  const MVert *vert = data->vert;
  const MLoopTri *lt = &data->looptri[index];
  const float *vtri_co[3] = {
      vert[data->loop[lt->tri[0]].v].co,
      vert[data->loop[lt->tri[1]].v].co,
      vert[data->loop[lt->tri[2]].v].co,
  };
  float dist = bvhtree_ray_tri_intersection(ray, hit->dist, UNPACK3(vtri_co));

  if (dist >= 0 && dist < hit->dist) {
    float no[3];
    if (raycast_tri_backface_culling_test(ray->direction, UNPACK3(vtri_co), no)) {
      hit->index = index;
      hit->dist = dist;
      madd_v3_v3v3fl(hit->co, ray->origin, ray->direction, dist);
      normalize_v3_v3(hit->no, no);
    }
  }
}

/* Callback to ray-cast with back-face culling (#EditMesh). */
static void editmesh_looptri_raycast_backface_culling_cb(void *userdata,
                                                         int index,
                                                         const BVHTreeRay *ray,
                                                         BVHTreeRayHit *hit)
{
  const BVHTreeFromEditMesh *data = (BVHTreeFromEditMesh *)userdata;
  BMEditMesh *em = data->em;
  const BMLoop **ltri = (const BMLoop **)em->looptris[index];

  const float *t0, *t1, *t2;
  t0 = ltri[0]->v->co;
  t1 = ltri[1]->v->co;
  t2 = ltri[2]->v->co;

  {
    float dist = bvhtree_ray_tri_intersection(ray, hit->dist, t0, t1, t2);

    if (dist >= 0 && dist < hit->dist) {
      float no[3];
      if (raycast_tri_backface_culling_test(ray->direction, t0, t1, t2, no)) {
        hit->index = index;
        hit->dist = dist;
        madd_v3_v3v3fl(hit->co, ray->origin, ray->direction, dist);
        normalize_v3_v3(hit->no, no);
      }
    }
  }
}

static bool raycastMesh(SnapObjectContext *sctx,
                        const SnapObjectParams *params,
                        const float ray_start[3],
                        const float ray_dir[3],
                        Object *ob_eval,
                        const Mesh *me_eval,
                        const float obmat[4][4],
                        const uint ob_index,
                        bool use_hide,
                        /* read/write args */
                        float *ray_depth,
                        /* return args */
                        float r_loc[3],
                        float r_no[3],
                        int *r_index,
                        ListBase *r_hit_list)
{
  bool retval = false;

  if (me_eval->totpoly == 0) {
    return retval;
  }

  float imat[4][4];
  float ray_start_local[3], ray_normal_local[3];
  float local_scale, local_depth, len_diff = 0.0f;

  invert_m4_m4(imat, obmat);

  copy_v3_v3(ray_start_local, ray_start);
  copy_v3_v3(ray_normal_local, ray_dir);

  mul_m4_v3(imat, ray_start_local);
  mul_mat3_m4_v3(imat, ray_normal_local);

  /* local scale in normal direction */
  local_scale = normalize_v3(ray_normal_local);
  local_depth = *ray_depth;
  if (local_depth != BVH_RAYCAST_DIST_MAX) {
    local_depth *= local_scale;
  }

  /* Test BoundBox */
  const BoundBox *bb = BKE_object_boundbox_get(ob_eval);
  if (bb) {
    /* was BKE_boundbox_ray_hit_check, see: cf6ca226fa58 */
    if (!isect_ray_aabb_v3_simple(
            ray_start_local, ray_normal_local, bb->vec[0], bb->vec[6], &len_diff, nullptr)) {
      return retval;
    }
  }
  /* We pass a temp ray_start, set from object's boundbox, to avoid precision issues with
   * very far away ray_start values (as returned in case of ortho view3d), see T50486, T38358.
   */
  if (len_diff > 400.0f) {
    len_diff -= local_scale; /* make temp start point a bit away from bbox hit point. */
    madd_v3_v3fl(ray_start_local, ray_normal_local, len_diff);
    local_depth -= len_diff;
  }
  else {
    len_diff = 0.0f;
  }

  SnapData_Mesh *sod = snap_object_data_mesh_get(sctx, ob_eval, me_eval, use_hide);

  BVHTreeFromMesh *treedata = &sod->treedata_mesh;

  if (treedata->tree == nullptr) {
    return retval;
  }

  float timat[3][3]; /* transpose inverse matrix for normals */
  transpose_m3_m4(timat, imat);

  BLI_assert(treedata->raycast_callback != nullptr);
  if (r_hit_list) {
    RayCastAll_Data data;

    data.bvhdata = treedata;
    data.raycast_callback = treedata->raycast_callback;
    data.obmat = obmat;
    data.timat = timat;
    data.len_diff = len_diff;
    data.local_scale = local_scale;
    data.ob_eval = ob_eval;
    data.ob_uuid = ob_index;
    data.hit_list = r_hit_list;
    data.retval = retval;

    BLI_bvhtree_ray_cast_all(treedata->tree,
                             ray_start_local,
                             ray_normal_local,
                             0.0f,
                             *ray_depth,
                             raycast_all_cb,
                             &data);

    retval = data.retval;
  }
  else {
    BVHTreeRayHit hit{};
    hit.index = -1;
    hit.dist = local_depth;

    if (BLI_bvhtree_ray_cast(treedata->tree,
                             ray_start_local,
                             ray_normal_local,
                             0.0f,
                             &hit,
                             params->use_backface_culling ?
                                 mesh_looptri_raycast_backface_culling_cb :
                                 treedata->raycast_callback,
                             treedata) != -1) {
      hit.dist += len_diff;
      hit.dist /= local_scale;
      if (hit.dist <= *ray_depth) {
        *ray_depth = hit.dist;
        copy_v3_v3(r_loc, hit.co);

        /* Back to world-space. */
        mul_m4_v3(obmat, r_loc);

        if (r_no) {
          copy_v3_v3(r_no, hit.no);
          mul_m3_v3(timat, r_no);
          normalize_v3(r_no);
        }

        retval = true;

        if (r_index) {
          *r_index = treedata->looptri[hit.index].poly;
        }
      }
    }
  }

  return retval;
}

static bool raycastEditMesh(SnapObjectContext *sctx,
                            const SnapObjectParams *params,
                            const float ray_start[3],
                            const float ray_dir[3],
                            Object *ob_eval,
                            BMEditMesh *em,
                            const float obmat[4][4],
                            const uint ob_index,
                            /* read/write args */
                            float *ray_depth,
                            /* return args */
                            float r_loc[3],
                            float r_no[3],
                            int *r_index,
                            ListBase *r_hit_list)
{
  bool retval = false;
  if (em->bm->totface == 0) {
    return retval;
  }

  float imat[4][4];
  float ray_start_local[3], ray_normal_local[3];
  float local_scale, local_depth, len_diff = 0.0f;

  invert_m4_m4(imat, obmat);

  copy_v3_v3(ray_start_local, ray_start);
  copy_v3_v3(ray_normal_local, ray_dir);

  mul_m4_v3(imat, ray_start_local);
  mul_mat3_m4_v3(imat, ray_normal_local);

  /* local scale in normal direction */
  local_scale = normalize_v3(ray_normal_local);
  local_depth = *ray_depth;
  if (local_depth != BVH_RAYCAST_DIST_MAX) {
    local_depth *= local_scale;
  }

  SnapData_EditMesh *sod = snap_object_data_editmesh_get(sctx, ob_eval, em);

  /* Test BoundBox */

  /* was BKE_boundbox_ray_hit_check, see: cf6ca226fa58 */
  if (!isect_ray_aabb_v3_simple(
          ray_start_local, ray_normal_local, sod->min, sod->max, &len_diff, nullptr)) {
    return retval;
  }

  /* We pass a temp ray_start, set from object's boundbox, to avoid precision issues with
   * very far away ray_start values (as returned in case of ortho view3d), see T50486, T38358.
   */
  if (len_diff > 400.0f) {
    len_diff -= local_scale; /* make temp start point a bit away from bbox hit point. */
    madd_v3_v3fl(ray_start_local, ray_normal_local, len_diff);
    local_depth -= len_diff;
  }
  else {
    len_diff = 0.0f;
  }

  BVHTreeFromEditMesh *treedata = snap_object_data_editmesh_treedata_get(sctx, ob_eval, em);
  if (treedata == nullptr) {
    return retval;
  }

  float timat[3][3]; /* transpose inverse matrix for normals */
  transpose_m3_m4(timat, imat);

  if (r_hit_list) {
    RayCastAll_Data data;

    data.bvhdata = treedata;
    data.raycast_callback = treedata->raycast_callback;
    data.obmat = obmat;
    data.timat = timat;
    data.len_diff = len_diff;
    data.local_scale = local_scale;
    data.ob_eval = ob_eval;
    data.ob_uuid = ob_index;
    data.hit_list = r_hit_list;
    data.retval = retval;

    BLI_bvhtree_ray_cast_all(treedata->tree,
                             ray_start_local,
                             ray_normal_local,
                             0.0f,
                             *ray_depth,
                             raycast_all_cb,
                             &data);

    retval = data.retval;
  }
  else {
    BVHTreeRayHit hit{};
    hit.index = -1;
    hit.dist = local_depth;

    if (BLI_bvhtree_ray_cast(treedata->tree,
                             ray_start_local,
                             ray_normal_local,
                             0.0f,
                             &hit,
                             params->use_backface_culling ?
                                 editmesh_looptri_raycast_backface_culling_cb :
                                 treedata->raycast_callback,
                             treedata) != -1) {
      hit.dist += len_diff;
      hit.dist /= local_scale;
      if (hit.dist <= *ray_depth) {
        *ray_depth = hit.dist;
        copy_v3_v3(r_loc, hit.co);

        /* Back to world-space. */
        mul_m4_v3(obmat, r_loc);

        if (r_no) {
          copy_v3_v3(r_no, hit.no);
          mul_m3_v3(timat, r_no);
          normalize_v3(r_no);
        }

        retval = true;

        if (r_index) {
          em = sod->treedata_editmesh.em;

          *r_index = BM_elem_index_get(em->looptris[hit.index][0]->f);
        }
      }
    }
  }

  return retval;
}

struct RaycastObjUserData {
  const float *ray_start;
  const float *ray_dir;
  uint ob_index;
  /* read/write args */
  float *ray_depth;
  /* return args */
  float *r_loc;
  float *r_no;
  int *r_index;
  Object **r_ob;
  float (*r_obmat)[4];
  ListBase *r_hit_list;
  bool use_occlusion_test;
  bool ret;
};

/**
 * \note Duplicate args here are documented at #snapObjectsRay
 */
static void raycast_obj_fn(SnapObjectContext *sctx,
                           const SnapObjectParams *params,
                           Object *ob_eval,
                           const float obmat[4][4],
                           bool is_object_active,
                           void *data)
{
  RaycastObjUserData *dt = static_cast<RaycastObjUserData *>(data);
  const uint ob_index = dt->ob_index++;
  bool use_occlusion_test = dt->use_occlusion_test;
  /* read/write args */
  float *ray_depth = dt->ray_depth;

  bool retval = false;
  if (use_occlusion_test) {
    if (ELEM(ob_eval->dt, OB_BOUNDBOX, OB_WIRE)) {
      /* Do not hit objects that are in wire or bounding box
       * display mode. */
      return;
    }
  }

  switch (ob_eval->type) {
    case OB_MESH: {
      const eSnapEditType edit_mode_type = params->edit_mode_type;
      bool use_hide = false;
      const Mesh *me_eval = mesh_for_snap(ob_eval, edit_mode_type, &use_hide);
      if (me_eval == nullptr) {
        BMEditMesh *em = BKE_editmesh_from_object(ob_eval);
        if (UNLIKELY(!em)) { /* See #mesh_for_snap doc-string. */
          return;
        }
        BLI_assert_msg(em == BKE_editmesh_from_object(DEG_get_original_object(ob_eval)),
                       "Make sure there is only one pointer for looptris");
        retval = raycastEditMesh(sctx,
                                 params,
                                 dt->ray_start,
                                 dt->ray_dir,
                                 ob_eval,
                                 em,
                                 obmat,
                                 ob_index,
                                 ray_depth,
                                 dt->r_loc,
                                 dt->r_no,
                                 dt->r_index,
                                 dt->r_hit_list);
        break;
      }
      retval = raycastMesh(sctx,
                           params,
                           dt->ray_start,
                           dt->ray_dir,
                           ob_eval,
                           me_eval,
                           obmat,
                           ob_index,
                           use_hide,
                           ray_depth,
                           dt->r_loc,
                           dt->r_no,
                           dt->r_index,
                           dt->r_hit_list);
      break;
    }
    case OB_CURVES_LEGACY:
    case OB_SURF:
    case OB_FONT: {
      if (!is_object_active) {
        const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
        if (mesh_eval) {
          retval = raycastMesh(sctx,
                               params,
                               dt->ray_start,
                               dt->ray_dir,
                               ob_eval,
                               mesh_eval,
                               obmat,
                               ob_index,
                               false,
                               ray_depth,
                               dt->r_loc,
                               dt->r_no,
                               dt->r_index,
                               dt->r_hit_list);
        }
      }
      break;
    }
  }

  if (retval) {
    if (dt->r_ob) {
      *dt->r_ob = ob_eval;
    }
    if (dt->r_obmat) {
      copy_m4_m4(dt->r_obmat, obmat);
    }
    dt->ret = true;
  }
}

/**
 * Main RayCast Function
 * ======================
 *
 * Walks through all objects in the scene to find the `hit` on object surface.
 *
 * \param sctx: Snap context to store data.
 *
 * Read/Write Args
 * ---------------
 *
 * \param ray_depth: maximum depth allowed for r_co,
 * elements deeper than this value will be ignored.
 *
 * Output Args
 * -----------
 *
 * \param r_loc: Hit location.
 * \param r_no: Hit normal (optional).
 * \param r_index: Hit index or -1 when no valid index is found.
 * (currently only set to the polygon index when using `snap_to == SCE_SNAP_MODE_FACE_RAYCAST`).
 * \param r_ob: Hit object.
 * \param r_obmat: Object matrix (may not be #Object.obmat with dupli-instances).
 * \param r_hit_list: List of #SnapObjectHitDepth (caller must free).
 */
static bool raycastObjects(SnapObjectContext *sctx,
                           const SnapObjectParams *params,
                           const float ray_start[3],
                           const float ray_dir[3],
                           /* read/write args */
                           /* Parameters below cannot be const, because they are assigned to a
                            * non-const variable (readability-non-const-parameter). */
                           float *ray_depth /* NOLINT */,
                           /* return args */
                           float r_loc[3] /* NOLINT */,
                           float r_no[3] /* NOLINT */,
                           int *r_index /* NOLINT */,
                           Object **r_ob,
                           float r_obmat[4][4],
                           ListBase *r_hit_list)
{
  const View3D *v3d = sctx->runtime.v3d;
  if (params->use_occlusion_test && v3d && XRAY_FLAG_ENABLED(v3d)) {
    /* General testing of occlusion geometry is disabled if the snap is not intended for the edit
     * cage. */
    if (params->edit_mode_type == SNAP_GEOM_EDIT) {
      return false;
    }
  }

  RaycastObjUserData data = {};
  data.ray_start = ray_start;
  data.ray_dir = ray_dir;
  data.ob_index = 0;
  data.ray_depth = ray_depth;
  data.r_loc = r_loc;
  data.r_no = r_no;
  data.r_index = r_index;
  data.r_ob = r_ob;
  data.r_obmat = r_obmat;
  data.r_hit_list = r_hit_list;
  data.use_occlusion_test = params->use_occlusion_test;
  data.ret = false;

  iter_snap_objects(sctx, params, raycast_obj_fn, &data);

  return data.ret;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Surface Snap Funcs
 * \{ */

struct NearestWorldObjUserData {
  const float *init_co;
  const float *curr_co;
  /* return args */
  float *r_loc;
  float *r_no;
  int *r_index;
  float r_dist_sq;
  Object **r_ob;
  float (*r_obmat)[4];
  ListBase *r_hit_list;
  bool ret;
};

static void nearest_world_tree_co(BVHTree *tree,
                                  BVHTree_NearestPointCallback nearest_cb,
                                  void *treedata,
                                  float co[3],
                                  float r_co[3],
                                  float r_no[3],
                                  int *r_index,
                                  float *r_dist_sq)
{
  BVHTreeNearest nearest = {};
  nearest.index = -1;
  copy_v3_fl(nearest.co, FLT_MAX);
  nearest.dist_sq = FLT_MAX;

  BLI_bvhtree_find_nearest(tree, co, &nearest, nearest_cb, treedata);

  if (r_co) {
    copy_v3_v3(r_co, nearest.co);
  }
  if (r_no) {
    copy_v3_v3(r_no, nearest.no);
  }
  if (r_index) {
    *r_index = nearest.index;
  }
  if (r_dist_sq) {
    float diff[3];
    sub_v3_v3v3(diff, co, nearest.co);
    *r_dist_sq = len_squared_v3(diff);
  }
}

static bool nearest_world_tree(SnapObjectContext *UNUSED(sctx),
                               const struct SnapObjectParams *params,
                               BVHTree *tree,
                               BVHTree_NearestPointCallback nearest_cb,
                               void *treedata,
                               const float (*obmat)[4],
                               const float init_co[3],
                               const float curr_co[3],
                               float *r_dist_sq,
                               float *r_loc,
                               float *r_no,
                               int *r_index)
{
  if (curr_co == nullptr || init_co == nullptr) {
    /* No location to work with, so just return. */
    return false;
  }

  float imat[4][4];
  invert_m4_m4(imat, obmat);

  float timat[3][3]; /* transpose inverse matrix for normals */
  transpose_m3_m4(timat, imat);

  /* compute offset between init co and prev co in local space */
  float init_co_local[3], curr_co_local[3];
  float delta_local[3];
  mul_v3_m4v3(init_co_local, imat, init_co);
  mul_v3_m4v3(curr_co_local, imat, curr_co);
  sub_v3_v3v3(delta_local, curr_co_local, init_co_local);

  float dist_sq;
  if (params->keep_on_same_target) {
    nearest_world_tree_co(
        tree, nearest_cb, treedata, init_co_local, nullptr, nullptr, nullptr, &dist_sq);
  }
  else {
    /* NOTE: when `params->face_nearest_steps == 1`, the return variables of function below contain
     * the answer.  We could return immediately after updating r_loc, r_no, r_index, but that would
     * also complicate the code. Foregoing slight optimization for code clarity. */
    nearest_world_tree_co(
        tree, nearest_cb, treedata, curr_co_local, nullptr, nullptr, nullptr, &dist_sq);
  }
  if (*r_dist_sq <= dist_sq) {
    return false;
  }
  *r_dist_sq = dist_sq;

  /* scale to make `snap_face_nearest_steps` steps */
  float step_scale_factor = 1.0f / max_ff(1.0f, (float)params->face_nearest_steps);
  mul_v3_fl(delta_local, step_scale_factor);

  float co_local[3];
  float no_local[3];
  int index;

  copy_v3_v3(co_local, init_co_local);

  for (int i = 0; i < params->face_nearest_steps; i++) {
    add_v3_v3(co_local, delta_local);
    nearest_world_tree_co(
        tree, nearest_cb, treedata, co_local, co_local, no_local, &index, nullptr);
  }

  mul_v3_m4v3(r_loc, obmat, co_local);

  if (r_no) {
    mul_v3_m3v3(r_no, timat, no_local);
    normalize_v3(r_no);
  }

  if (r_index) {
    *r_index = index;
  }

  return true;
}

static bool nearest_world_mesh(SnapObjectContext *sctx,
                               const struct SnapObjectParams *params,
                               Object *ob_eval,
                               const Mesh *me_eval,
                               const float (*obmat)[4],
                               bool use_hide,
                               const float init_co[3],
                               const float curr_co[3],
                               float *r_dist_sq,
                               float *r_loc,
                               float *r_no,
                               int *r_index)
{
  BVHTreeFromMesh *treedata = snap_object_data_mesh_treedata_get(sctx, ob_eval, me_eval, use_hide);
  if (treedata == nullptr || treedata->tree == nullptr) {
    return false;
  }

  return nearest_world_tree(sctx,
                            params,
                            treedata->tree,
                            treedata->nearest_callback,
                            treedata,
                            obmat,
                            init_co,
                            curr_co,
                            r_dist_sq,
                            r_loc,
                            r_no,
                            r_index);
}

static bool nearest_world_editmesh(SnapObjectContext *sctx,
                                   const struct SnapObjectParams *params,
                                   Object *ob_eval,
                                   BMEditMesh *em,
                                   const float (*obmat)[4],
                                   const float init_co[3],
                                   const float curr_co[3],
                                   float *r_dist_sq,
                                   float *r_loc,
                                   float *r_no,
                                   int *r_index)
{
  BVHTreeFromEditMesh *treedata = snap_object_data_editmesh_treedata_get(sctx, ob_eval, em);
  if (treedata == nullptr || treedata->tree == nullptr) {
    return false;
  }

  return nearest_world_tree(sctx,
                            params,
                            treedata->tree,
                            treedata->nearest_callback,
                            treedata,
                            obmat,
                            init_co,
                            curr_co,
                            r_dist_sq,
                            r_loc,
                            r_no,
                            r_index);
}
static void nearest_world_object_fn(SnapObjectContext *sctx,
                                    const struct SnapObjectParams *params,
                                    Object *ob_eval,
                                    const float obmat[4][4],
                                    bool is_object_active,
                                    void *data)
{
  struct NearestWorldObjUserData *dt = static_cast<NearestWorldObjUserData *>(data);

  bool retval = false;
  switch (ob_eval->type) {
    case OB_MESH: {
      const eSnapEditType edit_mode_type = params->edit_mode_type;
      bool use_hide = false;
      const Mesh *me_eval = mesh_for_snap(ob_eval, edit_mode_type, &use_hide);
      if (me_eval) {
        retval = nearest_world_mesh(sctx,
                                    params,
                                    ob_eval,
                                    me_eval,
                                    obmat,
                                    use_hide,
                                    dt->init_co,
                                    dt->curr_co,
                                    &dt->r_dist_sq,
                                    dt->r_loc,
                                    dt->r_no,
                                    dt->r_index);
      }
      else {
        BMEditMesh *em = BKE_editmesh_from_object(ob_eval);
        BLI_assert_msg(em == BKE_editmesh_from_object(DEG_get_original_object(ob_eval)),
                       "Make sure there is only one pointer for looptris");
        retval = nearest_world_editmesh(sctx,
                                        params,
                                        ob_eval,
                                        em,
                                        obmat,
                                        dt->init_co,
                                        dt->curr_co,
                                        &dt->r_dist_sq,
                                        dt->r_loc,
                                        dt->r_no,
                                        dt->r_index);
      }
      break;
    }
    case OB_CURVES_LEGACY:
    case OB_SURF:
    case OB_FONT:
      if (!is_object_active) {
        const Mesh *me_eval = BKE_object_get_evaluated_mesh(ob_eval);
        if (me_eval) {
          retval = nearest_world_mesh(sctx,
                                      params,
                                      ob_eval,
                                      me_eval,
                                      obmat,
                                      false,
                                      dt->init_co,
                                      dt->curr_co,
                                      &dt->r_dist_sq,
                                      dt->r_loc,
                                      dt->r_no,
                                      dt->r_index);
        }
      }
      break;
  }

  if (retval) {
    if (dt->r_ob) {
      *dt->r_ob = ob_eval;
    }
    if (dt->r_obmat) {
      copy_m4_m4(dt->r_obmat, obmat);
    }
    dt->ret = true;
  }
}

/**
 * Main Nearest World Surface Function
 * ===================================
 *
 * Walks through all objects in the scene to find the nearest location on target surface.
 *
 * \param sctx: Snap context to store data.
 * \param params: Settings for snapping.
 * \param init_co: Initial location of source point.
 * \param prev_co: Current location of source point after transformation but before snapping.
 *
 * Output Args
 * -----------
 *
 * \param r_loc: Location of nearest point on target surface.
 * \param r_no: Normal of nearest point on target surface.
 * \param r_index: Index of nearest polygon on target surface.
 * \param r_ob: Nearest target object.
 * \param r_obmat: Nearest target matrix (may not be #Object.obmat with dupli-instances).
 */
static bool nearestWorldObjects(SnapObjectContext *sctx,
                                const struct SnapObjectParams *params,
                                const float init_co[3],
                                const float curr_co[3],
                                float *r_loc /* NOLINT */,
                                float *r_no /* NOLINT */,
                                int *r_index /* NOLINT */,
                                Object **r_ob,
                                float r_obmat[4][4])
{
  NearestWorldObjUserData data = {};
  data.init_co = init_co;
  data.curr_co = curr_co;
  data.r_loc = r_loc;
  data.r_no = r_no;
  data.r_index = r_index;
  data.r_dist_sq = FLT_MAX;
  data.r_ob = r_ob;
  data.r_obmat = r_obmat;
  data.ret = false;

  iter_snap_objects(sctx, params, nearest_world_object_fn, &data);
  return data.ret;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Nearest utilities
 * \{ */

/* Test BoundBox */
static bool snap_bound_box_check_dist(const float min[3],
                                      const float max[3],
                                      const float lpmat[4][4],
                                      const float win_size[2],
                                      const float mval[2],
                                      float dist_px_sq)
{
  /* In vertex and edges you need to get the pixel distance from ray to BoundBox,
   * see: T46099, T46816 */

  DistProjectedAABBPrecalc data_precalc;
  dist_squared_to_projected_aabb_precalc(&data_precalc, lpmat, win_size, mval);

  bool dummy[3];
  float bb_dist_px_sq = dist_squared_to_projected_aabb(&data_precalc, min, max, dummy);

  if (bb_dist_px_sq > dist_px_sq) {
    return false;
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Callbacks
 * \{ */

struct Nearest2dUserData;

using Nearest2DGetVertCoCallback = void (*)(const int index,
                                            const Nearest2dUserData *data,
                                            const float **r_co);
using Nearest2DGetEdgeVertsCallback = void (*)(const int index,
                                               const Nearest2dUserData *data,
                                               int r_v_index[2]);
using Nearest2DGetTriVertsCallback = void (*)(const int index,
                                              const Nearest2dUserData *data,
                                              int r_v_index[3]);
/* Equal the previous one */
using Nearest2DGetTriEdgesCallback = void (*)(const int index,
                                              const Nearest2dUserData *data,
                                              int r_e_index[3]);
using Nearest2DCopyVertNoCallback = void (*)(const int index,
                                             const Nearest2dUserData *data,
                                             float r_no[3]);

struct Nearest2dUserData {
  Nearest2DGetVertCoCallback get_vert_co;
  Nearest2DGetEdgeVertsCallback get_edge_verts_index;
  Nearest2DGetTriVertsCallback get_tri_verts_index;
  Nearest2DGetTriEdgesCallback get_tri_edges_index;
  Nearest2DCopyVertNoCallback copy_vert_no;

  union {
    struct {
      BMesh *bm;
    };
    struct {
      const MVert *vert;
      const float (*vert_normals)[3];
      const MEdge *edge; /* only used for #BVHTreeFromMeshEdges */
      const MLoop *loop;
      const MLoopTri *looptri;
    };
  };

  bool is_persp;
  bool use_backface_culling;
};

static void cb_mvert_co_get(const int index, const Nearest2dUserData *data, const float **r_co)
{
  *r_co = data->vert[index].co;
}

static void cb_bvert_co_get(const int index, const Nearest2dUserData *data, const float **r_co)
{
  BMVert *eve = BM_vert_at_index(data->bm, index);
  *r_co = eve->co;
}

static void cb_mvert_no_copy(const int index, const Nearest2dUserData *data, float r_no[3])
{
  copy_v3_v3(r_no, data->vert_normals[index]);
}

static void cb_bvert_no_copy(const int index, const Nearest2dUserData *data, float r_no[3])
{
  BMVert *eve = BM_vert_at_index(data->bm, index);

  copy_v3_v3(r_no, eve->no);
}

static void cb_medge_verts_get(const int index, const Nearest2dUserData *data, int r_v_index[2])
{
  const MEdge *edge = &data->edge[index];

  r_v_index[0] = edge->v1;
  r_v_index[1] = edge->v2;
}

static void cb_bedge_verts_get(const int index, const Nearest2dUserData *data, int r_v_index[2])
{
  BMEdge *eed = BM_edge_at_index(data->bm, index);

  r_v_index[0] = BM_elem_index_get(eed->v1);
  r_v_index[1] = BM_elem_index_get(eed->v2);
}

static void cb_mlooptri_edges_get(const int index, const Nearest2dUserData *data, int r_v_index[3])
{
  const MEdge *medge = data->edge;
  const MLoop *mloop = data->loop;
  const MLoopTri *lt = &data->looptri[index];
  for (int j = 2, j_next = 0; j_next < 3; j = j_next++) {
    const MEdge *ed = &medge[mloop[lt->tri[j]].e];
    const uint tri_edge[2] = {mloop[lt->tri[j]].v, mloop[lt->tri[j_next]].v};
    if (ELEM(ed->v1, tri_edge[0], tri_edge[1]) && ELEM(ed->v2, tri_edge[0], tri_edge[1])) {
      // printf("real edge found\n");
      r_v_index[j] = mloop[lt->tri[j]].e;
    }
    else {
      r_v_index[j] = -1;
    }
  }
}

static void cb_mlooptri_verts_get(const int index, const Nearest2dUserData *data, int r_v_index[3])
{
  const MLoop *loop = data->loop;
  const MLoopTri *looptri = &data->looptri[index];

  r_v_index[0] = loop[looptri->tri[0]].v;
  r_v_index[1] = loop[looptri->tri[1]].v;
  r_v_index[2] = loop[looptri->tri[2]].v;
}

static bool test_projected_vert_dist(const DistProjectedAABBPrecalc *precalc,
                                     const float (*clip_plane)[4],
                                     const int clip_plane_len,
                                     const bool is_persp,
                                     const float co[3],
                                     float *dist_px_sq,
                                     float r_co[3])
{
  if (!isect_point_planes_v3_negated(clip_plane, clip_plane_len, co)) {
    return false;
  }

  float co2d[2] = {
      (dot_m4_v3_row_x(precalc->pmat, co) + precalc->pmat[3][0]),
      (dot_m4_v3_row_y(precalc->pmat, co) + precalc->pmat[3][1]),
  };

  if (is_persp) {
    float w = mul_project_m4_v3_zfac(precalc->pmat, co);
    mul_v2_fl(co2d, 1.0f / w);
  }

  const float dist_sq = len_squared_v2v2(precalc->mval, co2d);
  if (dist_sq < *dist_px_sq) {
    copy_v3_v3(r_co, co);
    *dist_px_sq = dist_sq;
    return true;
  }
  return false;
}

static bool test_projected_edge_dist(const DistProjectedAABBPrecalc *precalc,
                                     const float (*clip_plane)[4],
                                     const int clip_plane_len,
                                     const bool is_persp,
                                     const float va[3],
                                     const float vb[3],
                                     float *dist_px_sq,
                                     float r_co[3])
{
  float near_co[3], lambda;
  if (!isect_ray_line_v3(precalc->ray_origin, precalc->ray_direction, va, vb, &lambda)) {
    copy_v3_v3(near_co, va);
  }
  else {
    if (lambda <= 0.0f) {
      copy_v3_v3(near_co, va);
    }
    else if (lambda >= 1.0f) {
      copy_v3_v3(near_co, vb);
    }
    else {
      interp_v3_v3v3(near_co, va, vb, lambda);
    }
  }

  return test_projected_vert_dist(
      precalc, clip_plane, clip_plane_len, is_persp, near_co, dist_px_sq, r_co);
}

static void cb_snap_vert(void *userdata,
                         int index,
                         const DistProjectedAABBPrecalc *precalc,
                         const float (*clip_plane)[4],
                         const int clip_plane_len,
                         BVHTreeNearest *nearest)
{
  Nearest2dUserData *data = static_cast<Nearest2dUserData *>(userdata);

  const float *co;
  data->get_vert_co(index, data, &co);

  if (test_projected_vert_dist(precalc,
                               clip_plane,
                               clip_plane_len,
                               data->is_persp,
                               co,
                               &nearest->dist_sq,
                               nearest->co)) {
    data->copy_vert_no(index, data, nearest->no);
    nearest->index = index;
  }
}

static void cb_snap_edge(void *userdata,
                         int index,
                         const DistProjectedAABBPrecalc *precalc,
                         const float (*clip_plane)[4],
                         const int clip_plane_len,
                         BVHTreeNearest *nearest)
{
  Nearest2dUserData *data = static_cast<Nearest2dUserData *>(userdata);

  int vindex[2];
  data->get_edge_verts_index(index, data, vindex);

  const float *v_pair[2];
  data->get_vert_co(vindex[0], data, &v_pair[0]);
  data->get_vert_co(vindex[1], data, &v_pair[1]);

  if (test_projected_edge_dist(precalc,
                               clip_plane,
                               clip_plane_len,
                               data->is_persp,
                               v_pair[0],
                               v_pair[1],
                               &nearest->dist_sq,
                               nearest->co)) {
    sub_v3_v3v3(nearest->no, v_pair[0], v_pair[1]);
    nearest->index = index;
  }
}

static void cb_snap_edge_verts(void *userdata,
                               int index,
                               const DistProjectedAABBPrecalc *precalc,
                               const float (*clip_plane)[4],
                               const int clip_plane_len,
                               BVHTreeNearest *nearest)
{
  Nearest2dUserData *data = static_cast<Nearest2dUserData *>(userdata);

  int vindex[2];
  data->get_edge_verts_index(index, data, vindex);

  for (int i = 2; i--;) {
    if (vindex[i] == nearest->index) {
      continue;
    }
    cb_snap_vert(userdata, vindex[i], precalc, clip_plane, clip_plane_len, nearest);
  }
}

static void cb_snap_tri_edges(void *userdata,
                              int index,
                              const DistProjectedAABBPrecalc *precalc,
                              const float (*clip_plane)[4],
                              const int clip_plane_len,
                              BVHTreeNearest *nearest)
{
  Nearest2dUserData *data = static_cast<Nearest2dUserData *>(userdata);

  if (data->use_backface_culling) {
    int vindex[3];
    data->get_tri_verts_index(index, data, vindex);

    const float *t0, *t1, *t2;
    data->get_vert_co(vindex[0], data, &t0);
    data->get_vert_co(vindex[1], data, &t1);
    data->get_vert_co(vindex[2], data, &t2);
    float dummy[3];
    if (raycast_tri_backface_culling_test(precalc->ray_direction, t0, t1, t2, dummy)) {
      return;
    }
  }

  int eindex[3];
  data->get_tri_edges_index(index, data, eindex);
  for (int i = 3; i--;) {
    if (eindex[i] != -1) {
      if (eindex[i] == nearest->index) {
        continue;
      }
      cb_snap_edge(userdata, eindex[i], precalc, clip_plane, clip_plane_len, nearest);
    }
  }
}

static void cb_snap_tri_verts(void *userdata,
                              int index,
                              const DistProjectedAABBPrecalc *precalc,
                              const float (*clip_plane)[4],
                              const int clip_plane_len,
                              BVHTreeNearest *nearest)
{
  Nearest2dUserData *data = static_cast<Nearest2dUserData *>(userdata);

  int vindex[3];
  data->get_tri_verts_index(index, data, vindex);

  if (data->use_backface_culling) {
    const float *t0, *t1, *t2;
    data->get_vert_co(vindex[0], data, &t0);
    data->get_vert_co(vindex[1], data, &t1);
    data->get_vert_co(vindex[2], data, &t2);
    float dummy[3];
    if (raycast_tri_backface_culling_test(precalc->ray_direction, t0, t1, t2, dummy)) {
      return;
    }
  }

  for (int i = 3; i--;) {
    if (vindex[i] == nearest->index) {
      continue;
    }
    cb_snap_vert(userdata, vindex[i], precalc, clip_plane, clip_plane_len, nearest);
  }
}

static void nearest2d_data_init_mesh(SnapData_Mesh *sod,
                                     bool is_persp,
                                     bool use_backface_culling,
                                     Nearest2dUserData *r_nearest2d)
{
  r_nearest2d->get_vert_co = cb_mvert_co_get;
  r_nearest2d->get_edge_verts_index = cb_medge_verts_get;
  r_nearest2d->copy_vert_no = cb_mvert_no_copy;
  r_nearest2d->get_tri_verts_index = cb_mlooptri_verts_get;
  r_nearest2d->get_tri_edges_index = cb_mlooptri_edges_get;

  r_nearest2d->vert = sod->treedata_mesh.vert;
  r_nearest2d->vert_normals = sod->treedata_mesh.vert_normals;
  r_nearest2d->edge = sod->treedata_mesh.edge;
  r_nearest2d->loop = sod->treedata_mesh.loop;
  r_nearest2d->looptri = sod->treedata_mesh.looptri;

  r_nearest2d->is_persp = is_persp;
  r_nearest2d->use_backface_culling = use_backface_culling;
}

static void nearest2d_data_init_editmesh(SnapData_EditMesh *sod,
                                         bool is_persp,
                                         bool use_backface_culling,
                                         Nearest2dUserData *r_nearest2d)
{
  r_nearest2d->get_vert_co = cb_bvert_co_get;
  r_nearest2d->get_edge_verts_index = cb_bedge_verts_get;
  r_nearest2d->copy_vert_no = cb_bvert_no_copy;
  r_nearest2d->get_tri_verts_index = nullptr;
  r_nearest2d->get_tri_edges_index = nullptr;

  r_nearest2d->bm = sod->treedata_editmesh.em->bm;

  r_nearest2d->is_persp = is_persp;
  r_nearest2d->use_backface_culling = use_backface_culling;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Object Snapping API
 * \{ */

static eSnapMode snap_mesh_polygon(SnapObjectContext *sctx,
                                   const SnapObjectParams *params,
                                   Object *ob_eval,
                                   const float obmat[4][4],
                                   /* read/write args */
                                   float *dist_px,
                                   /* return args */
                                   float r_loc[3],
                                   float r_no[3],
                                   int *r_index)
{
  eSnapMode elem = SCE_SNAP_MODE_NONE;

  float lpmat[4][4];
  mul_m4_m4m4(lpmat, sctx->runtime.pmat, obmat);

  DistProjectedAABBPrecalc neasrest_precalc;
  dist_squared_to_projected_aabb_precalc(
      &neasrest_precalc, lpmat, sctx->runtime.win_size, sctx->runtime.mval);

  float tobmat[4][4], clip_planes_local[MAX_CLIPPLANE_LEN][4];
  transpose_m4_m4(tobmat, obmat);
  for (int i = sctx->runtime.clip_plane_len; i--;) {
    mul_v4_m4v4(clip_planes_local[i], tobmat, sctx->runtime.clip_plane[i]);
  }

  BVHTreeNearest nearest{};
  nearest.index = -1;
  nearest.dist_sq = square_f(*dist_px);

  Nearest2dUserData nearest2d;
  std::unique_ptr<SnapData_Mesh> *sod_mesh = sctx->mesh_caches.lookup_ptr(ob_eval);
  if (sod_mesh) {
    nearest2d_data_init_mesh(sod_mesh->get(),
                             sctx->runtime.view_proj == VIEW_PROJ_PERSP,
                             params->use_backface_culling,
                             &nearest2d);

    BVHTreeFromMesh *treedata = &sod_mesh->get()->treedata_mesh;

    const MPoly *mp = &sod_mesh->get()->poly[*r_index];
    const MLoop *ml = &treedata->loop[mp->loopstart];
    if (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_EDGE) {
      elem = SCE_SNAP_MODE_EDGE;
      BLI_assert(treedata->edge != nullptr);
      for (int i = mp->totloop; i--; ml++) {
        cb_snap_edge(&nearest2d,
                     ml->e,
                     &neasrest_precalc,
                     clip_planes_local,
                     sctx->runtime.clip_plane_len,
                     &nearest);
      }
    }
    else {
      elem = SCE_SNAP_MODE_VERTEX;
      for (int i = mp->totloop; i--; ml++) {
        cb_snap_vert(&nearest2d,
                     ml->v,
                     &neasrest_precalc,
                     clip_planes_local,
                     sctx->runtime.clip_plane_len,
                     &nearest);
      }
    }
  }
  else {
    /* The object's #BMEditMesh was used to snap instead. */
    std::unique_ptr<SnapData_EditMesh> &sod_editmesh = sctx->editmesh_caches.lookup(
        BKE_editmesh_from_object(ob_eval));
    BLI_assert(sod_editmesh.get() != nullptr);

    nearest2d_data_init_editmesh(sod_editmesh.get(),
                                 sctx->runtime.view_proj == VIEW_PROJ_PERSP,
                                 params->use_backface_culling,
                                 &nearest2d);

    BMEditMesh *em = sod_editmesh->treedata_editmesh.em;

    BM_mesh_elem_table_ensure(em->bm, BM_FACE);
    BMFace *f = BM_face_at_index(em->bm, *r_index);
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    if (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_EDGE) {
      elem = SCE_SNAP_MODE_EDGE;
      BM_mesh_elem_index_ensure(em->bm, BM_VERT | BM_EDGE);
      BM_mesh_elem_table_ensure(em->bm, BM_VERT | BM_EDGE);
      do {
        cb_snap_edge(&nearest2d,
                     BM_elem_index_get(l_iter->e),
                     &neasrest_precalc,
                     clip_planes_local,
                     sctx->runtime.clip_plane_len,
                     &nearest);
      } while ((l_iter = l_iter->next) != l_first);
    }
    else {
      elem = SCE_SNAP_MODE_VERTEX;
      BM_mesh_elem_index_ensure(em->bm, BM_VERT);
      BM_mesh_elem_table_ensure(em->bm, BM_VERT);
      do {
        cb_snap_vert(&nearest2d,
                     BM_elem_index_get(l_iter->v),
                     &neasrest_precalc,
                     clip_planes_local,
                     sctx->runtime.clip_plane_len,
                     &nearest);
      } while ((l_iter = l_iter->next) != l_first);
    }
  }

  if (nearest.index != -1) {
    *dist_px = sqrtf(nearest.dist_sq);

    copy_v3_v3(r_loc, nearest.co);
    mul_m4_v3(obmat, r_loc);

    if (r_no) {
      float imat[4][4];
      invert_m4_m4(imat, obmat);

      copy_v3_v3(r_no, nearest.no);
      mul_transposed_mat3_m4_v3(imat, r_no);
      normalize_v3(r_no);
    }

    *r_index = nearest.index;
    return elem;
  }

  return SCE_SNAP_MODE_NONE;
}

static eSnapMode snap_mesh_edge_verts_mixed(SnapObjectContext *sctx,
                                            const SnapObjectParams *params,
                                            Object *ob_eval,
                                            const float obmat[4][4],
                                            float original_dist_px,
                                            const float prev_co[3],
                                            /* read/write args */
                                            float *dist_px,
                                            /* return args */
                                            float r_loc[3],
                                            float r_no[3],
                                            int *r_index)
{
  eSnapMode elem = SCE_SNAP_MODE_EDGE;

  if (ob_eval->type != OB_MESH) {
    return elem;
  }

  Nearest2dUserData nearest2d;
  {
    std::unique_ptr<SnapData_Mesh> *sod_mesh = sctx->mesh_caches.lookup_ptr(ob_eval);
    if (sod_mesh) {
      nearest2d_data_init_mesh(sod_mesh->get(),
                               sctx->runtime.view_proj == VIEW_PROJ_PERSP,
                               params->use_backface_culling,
                               &nearest2d);
    }
    else {
      /* The object's #BMEditMesh was used to snap instead. */
      std::unique_ptr<SnapData_EditMesh> &sod_editmesh = sctx->editmesh_caches.lookup(
          BKE_editmesh_from_object(ob_eval));
      nearest2d_data_init_editmesh(sod_editmesh.get(),
                                   sctx->runtime.view_proj == VIEW_PROJ_PERSP,
                                   params->use_backface_culling,
                                   &nearest2d);
    }
  }

  int vindex[2];
  nearest2d.get_edge_verts_index(*r_index, &nearest2d, vindex);

  const float *v_pair[2];
  nearest2d.get_vert_co(vindex[0], &nearest2d, &v_pair[0]);
  nearest2d.get_vert_co(vindex[1], &nearest2d, &v_pair[1]);

  DistProjectedAABBPrecalc neasrest_precalc;
  {
    float lpmat[4][4];
    mul_m4_m4m4(lpmat, sctx->runtime.pmat, obmat);

    dist_squared_to_projected_aabb_precalc(
        &neasrest_precalc, lpmat, sctx->runtime.win_size, sctx->runtime.mval);
  }

  BVHTreeNearest nearest{};
  nearest.index = -1;
  nearest.dist_sq = square_f(original_dist_px);

  float lambda;
  if (!isect_ray_line_v3(neasrest_precalc.ray_origin,
                         neasrest_precalc.ray_direction,
                         v_pair[0],
                         v_pair[1],
                         &lambda)) {
    /* do nothing */
  }
  else {
    short snap_to_flag = sctx->runtime.snap_to_flag;
    int e_mode_len = ((snap_to_flag & SCE_SNAP_MODE_EDGE) != 0) +
                     ((snap_to_flag & SCE_SNAP_MODE_VERTEX) != 0) +
                     ((snap_to_flag & SCE_SNAP_MODE_EDGE_MIDPOINT) != 0);

    float range = 1.0f / (2 * e_mode_len - 1);
    if (snap_to_flag & SCE_SNAP_MODE_VERTEX) {
      if (lambda < (range) || (1.0f - range) < lambda) {
        int v_id = lambda < 0.5f ? 0 : 1;

        if (test_projected_vert_dist(&neasrest_precalc,
                                     nullptr,
                                     0,
                                     nearest2d.is_persp,
                                     v_pair[v_id],
                                     &nearest.dist_sq,
                                     nearest.co)) {
          nearest.index = vindex[v_id];
          elem = SCE_SNAP_MODE_VERTEX;
          if (r_no) {
            float imat[4][4];
            invert_m4_m4(imat, obmat);
            nearest2d.copy_vert_no(vindex[v_id], &nearest2d, r_no);
            mul_transposed_mat3_m4_v3(imat, r_no);
            normalize_v3(r_no);
          }
        }
      }
    }

    if (snap_to_flag & SCE_SNAP_MODE_EDGE_MIDPOINT) {
      range *= e_mode_len - 1;
      if ((range) < lambda && lambda < (1.0f - range)) {
        float vmid[3];
        mid_v3_v3v3(vmid, v_pair[0], v_pair[1]);

        if (test_projected_vert_dist(&neasrest_precalc,
                                     nullptr,
                                     0,
                                     nearest2d.is_persp,
                                     vmid,
                                     &nearest.dist_sq,
                                     nearest.co)) {
          nearest.index = *r_index;
          elem = SCE_SNAP_MODE_EDGE_MIDPOINT;
        }
      }
    }

    if (prev_co && (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_EDGE_PERPENDICULAR)) {
      float v_near[3], va_g[3], vb_g[3];

      mul_v3_m4v3(va_g, obmat, v_pair[0]);
      mul_v3_m4v3(vb_g, obmat, v_pair[1]);
      lambda = line_point_factor_v3(prev_co, va_g, vb_g);

      if (IN_RANGE(lambda, 0.0f, 1.0f)) {
        interp_v3_v3v3(v_near, va_g, vb_g, lambda);

        if (len_squared_v3v3(prev_co, v_near) > FLT_EPSILON) {
          dist_squared_to_projected_aabb_precalc(
              &neasrest_precalc, sctx->runtime.pmat, sctx->runtime.win_size, sctx->runtime.mval);

          if (test_projected_vert_dist(&neasrest_precalc,
                                       nullptr,
                                       0,
                                       nearest2d.is_persp,
                                       v_near,
                                       &nearest.dist_sq,
                                       nearest.co)) {
            nearest.index = *r_index;
            elem = SCE_SNAP_MODE_EDGE_PERPENDICULAR;
          }
        }
      }
    }
  }

  if (nearest.index != -1) {
    *dist_px = sqrtf(nearest.dist_sq);

    copy_v3_v3(r_loc, nearest.co);
    if (elem != SCE_SNAP_MODE_EDGE_PERPENDICULAR) {
      mul_m4_v3(obmat, r_loc);
    }

    *r_index = nearest.index;
  }

  return elem;
}

static eSnapMode snapArmature(SnapObjectContext *sctx,
                              const SnapObjectParams *params,
                              Object *ob_eval,
                              const float obmat[4][4],
                              bool is_object_active,
                              /* read/write args */
                              float *dist_px,
                              /* return args */
                              float r_loc[3],
                              float *UNUSED(r_no),
                              int *r_index)
{
  eSnapMode retval = SCE_SNAP_MODE_NONE;

  if (sctx->runtime.snap_to_flag == SCE_SNAP_MODE_FACE_RAYCAST) {
    /* Currently only edge and vert */
    return retval;
  }

  float lpmat[4][4], dist_px_sq = square_f(*dist_px);
  mul_m4_m4m4(lpmat, sctx->runtime.pmat, obmat);

  DistProjectedAABBPrecalc neasrest_precalc;
  dist_squared_to_projected_aabb_precalc(
      &neasrest_precalc, lpmat, sctx->runtime.win_size, sctx->runtime.mval);

  bArmature *arm = static_cast<bArmature *>(ob_eval->data);
  const bool is_editmode = arm->edbo != nullptr;

  if (is_editmode == false) {
    /* Test BoundBox */
    const BoundBox *bb = BKE_armature_boundbox_get(ob_eval);
    if (bb && !snap_bound_box_check_dist(bb->vec[0],
                                         bb->vec[6],
                                         lpmat,
                                         sctx->runtime.win_size,
                                         sctx->runtime.mval,
                                         dist_px_sq)) {
      return retval;
    }
  }

  float tobmat[4][4], clip_planes_local[MAX_CLIPPLANE_LEN][4];
  transpose_m4_m4(tobmat, obmat);
  for (int i = sctx->runtime.clip_plane_len; i--;) {
    mul_v4_m4v4(clip_planes_local[i], tobmat, sctx->runtime.clip_plane[i]);
  }

  const bool is_posemode = is_object_active && (ob_eval->mode & OB_MODE_POSE);
  const bool skip_selected = (is_editmode || is_posemode) &&
                             (params->snap_target_select & SCE_SNAP_TARGET_NOT_SELECTED);
  const bool is_persp = sctx->runtime.view_proj == VIEW_PROJ_PERSP;

  if (arm->edbo) {
    LISTBASE_FOREACH (EditBone *, eBone, arm->edbo) {
      if (eBone->layer & arm->layer) {
        if (eBone->flag & BONE_HIDDEN_A) {
          /* Skip hidden bones. */
          continue;
        }

        const bool is_selected = (eBone->flag & (BONE_ROOTSEL | BONE_TIPSEL)) != 0;
        if (is_selected && skip_selected) {
          continue;
        }
        bool has_vert_snap = false;

        if (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX) {
          has_vert_snap = test_projected_vert_dist(&neasrest_precalc,
                                                   clip_planes_local,
                                                   sctx->runtime.clip_plane_len,
                                                   is_persp,
                                                   eBone->head,
                                                   &dist_px_sq,
                                                   r_loc);
          has_vert_snap |= test_projected_vert_dist(&neasrest_precalc,
                                                    clip_planes_local,
                                                    sctx->runtime.clip_plane_len,
                                                    is_persp,
                                                    eBone->tail,

                                                    &dist_px_sq,
                                                    r_loc);

          if (has_vert_snap) {
            retval = SCE_SNAP_MODE_VERTEX;
          }
        }
        if (!has_vert_snap && sctx->runtime.snap_to_flag & SCE_SNAP_MODE_EDGE) {
          if (test_projected_edge_dist(&neasrest_precalc,
                                       clip_planes_local,
                                       sctx->runtime.clip_plane_len,
                                       is_persp,
                                       eBone->head,
                                       eBone->tail,
                                       &dist_px_sq,
                                       r_loc)) {
            retval = SCE_SNAP_MODE_EDGE;
          }
        }
      }
    }
  }
  else if (ob_eval->pose && ob_eval->pose->chanbase.first) {
    LISTBASE_FOREACH (bPoseChannel *, pchan, &ob_eval->pose->chanbase) {
      Bone *bone = pchan->bone;
      if (!bone || (bone->flag & (BONE_HIDDEN_P | BONE_HIDDEN_PG))) {
        /* Skip hidden bones. */
        continue;
      }

      const bool is_selected = (bone->flag & (BONE_SELECTED | BONE_ROOTSEL | BONE_TIPSEL)) != 0;
      if (is_selected && skip_selected) {
        continue;
      }

      bool has_vert_snap = false;
      const float *head_vec = pchan->pose_head;
      const float *tail_vec = pchan->pose_tail;

      if (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX) {
        has_vert_snap = test_projected_vert_dist(&neasrest_precalc,
                                                 clip_planes_local,
                                                 sctx->runtime.clip_plane_len,
                                                 is_persp,
                                                 head_vec,
                                                 &dist_px_sq,
                                                 r_loc);
        has_vert_snap |= test_projected_vert_dist(&neasrest_precalc,
                                                  clip_planes_local,
                                                  sctx->runtime.clip_plane_len,
                                                  is_persp,
                                                  tail_vec,
                                                  &dist_px_sq,
                                                  r_loc);

        if (has_vert_snap) {
          retval = SCE_SNAP_MODE_VERTEX;
        }
      }
      if (!has_vert_snap && sctx->runtime.snap_to_flag & SCE_SNAP_MODE_EDGE) {
        if (test_projected_edge_dist(&neasrest_precalc,
                                     clip_planes_local,
                                     sctx->runtime.clip_plane_len,
                                     is_persp,
                                     head_vec,
                                     tail_vec,
                                     &dist_px_sq,
                                     r_loc)) {
          retval = SCE_SNAP_MODE_EDGE;
        }
      }
    }
  }

  if (retval) {
    *dist_px = sqrtf(dist_px_sq);
    mul_m4_v3(obmat, r_loc);
    if (r_index) {
      /* Does not support index. */
      *r_index = -1;
    }
    return retval;
  }

  return SCE_SNAP_MODE_NONE;
}

static eSnapMode snapCurve(SnapObjectContext *sctx,
                           const SnapObjectParams *params,
                           Object *ob_eval,
                           const float obmat[4][4],
                           /* read/write args */
                           float *dist_px,
                           /* return args */
                           float r_loc[3],
                           float *UNUSED(r_no),
                           int *r_index)
{
  bool has_snap = false;

  /* only vertex snapping mode (eg control points and handles) supported for now) */
  if ((sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX) == 0) {
    return SCE_SNAP_MODE_NONE;
  }

  Curve *cu = static_cast<Curve *>(ob_eval->data);
  float dist_px_sq = square_f(*dist_px);

  float lpmat[4][4];
  mul_m4_m4m4(lpmat, sctx->runtime.pmat, obmat);

  DistProjectedAABBPrecalc neasrest_precalc;
  dist_squared_to_projected_aabb_precalc(
      &neasrest_precalc, lpmat, sctx->runtime.win_size, sctx->runtime.mval);

  const bool use_obedit = BKE_object_is_in_editmode(ob_eval);

  if (use_obedit == false) {
    /* Test BoundBox */
    BoundBox *bb = BKE_curve_boundbox_get(ob_eval);
    if (bb && !snap_bound_box_check_dist(bb->vec[0],
                                         bb->vec[6],
                                         lpmat,
                                         sctx->runtime.win_size,
                                         sctx->runtime.mval,
                                         dist_px_sq)) {
      return SCE_SNAP_MODE_NONE;
    }
  }

  float tobmat[4][4];
  transpose_m4_m4(tobmat, obmat);

  float(*clip_planes)[4] = sctx->runtime.clip_plane;
  int clip_plane_len = sctx->runtime.clip_plane_len;

  if (sctx->runtime.has_occlusion_plane) {
    /* We snap to vertices even if occluded. */
    clip_planes++;
    clip_plane_len--;
  }

  float clip_planes_local[MAX_CLIPPLANE_LEN][4];
  for (int i = clip_plane_len; i--;) {
    mul_v4_m4v4(clip_planes_local[i], tobmat, clip_planes[i]);
  }

  bool is_persp = sctx->runtime.view_proj == VIEW_PROJ_PERSP;
  bool skip_selected = params->snap_target_select & SCE_SNAP_TARGET_NOT_SELECTED;

  LISTBASE_FOREACH (Nurb *, nu, (use_obedit ? &cu->editnurb->nurbs : &cu->nurb)) {
    for (int u = 0; u < nu->pntsu; u++) {
      if (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX) {
        if (use_obedit) {
          if (nu->bezt) {
            if (nu->bezt[u].hide) {
              /* Skip hidden. */
              continue;
            }

            bool is_selected = (nu->bezt[u].f2 & SELECT) != 0;
            if (is_selected && skip_selected) {
              continue;
            }
            has_snap |= test_projected_vert_dist(&neasrest_precalc,
                                                 clip_planes_local,
                                                 clip_plane_len,
                                                 is_persp,
                                                 nu->bezt[u].vec[1],
                                                 &dist_px_sq,
                                                 r_loc);

            /* Don't snap if handle is selected (moving),
             * or if it is aligning to a moving handle. */
            bool is_selected_h1 = (nu->bezt[u].f1 & SELECT) != 0;
            bool is_selected_h2 = (nu->bezt[u].f3 & SELECT) != 0;
            bool is_autoalign_h1 = (nu->bezt[u].h1 & HD_ALIGN) != 0;
            bool is_autoalign_h2 = (nu->bezt[u].h2 & HD_ALIGN) != 0;
            if (!skip_selected || !(is_selected_h1 || (is_autoalign_h1 && is_selected_h2))) {
              has_snap |= test_projected_vert_dist(&neasrest_precalc,
                                                   clip_planes_local,
                                                   clip_plane_len,
                                                   is_persp,
                                                   nu->bezt[u].vec[0],
                                                   &dist_px_sq,
                                                   r_loc);
            }

            if (!skip_selected || !(is_selected_h2 || (is_autoalign_h2 && is_selected_h1))) {
              has_snap |= test_projected_vert_dist(&neasrest_precalc,
                                                   clip_planes_local,
                                                   clip_plane_len,
                                                   is_persp,
                                                   nu->bezt[u].vec[2],
                                                   &dist_px_sq,
                                                   r_loc);
            }
          }
          else {
            if (nu->bp[u].hide) {
              /* Skip hidden. */
              continue;
            }

            bool is_selected = (nu->bp[u].f1 & SELECT) != 0;
            if (is_selected && skip_selected) {
              continue;
            }

            has_snap |= test_projected_vert_dist(&neasrest_precalc,
                                                 clip_planes_local,
                                                 clip_plane_len,
                                                 is_persp,
                                                 nu->bp[u].vec,
                                                 &dist_px_sq,
                                                 r_loc);
          }
        }
        else {
          /* curve is not visible outside editmode if nurb length less than two */
          if (nu->pntsu > 1) {
            if (nu->bezt) {
              has_snap |= test_projected_vert_dist(&neasrest_precalc,
                                                   clip_planes_local,
                                                   clip_plane_len,
                                                   is_persp,
                                                   nu->bezt[u].vec[1],
                                                   &dist_px_sq,
                                                   r_loc);
            }
            else {
              has_snap |= test_projected_vert_dist(&neasrest_precalc,
                                                   clip_planes_local,
                                                   clip_plane_len,
                                                   is_persp,
                                                   nu->bp[u].vec,
                                                   &dist_px_sq,
                                                   r_loc);
            }
          }
        }
      }
    }
  }
  if (has_snap) {
    *dist_px = sqrtf(dist_px_sq);
    mul_m4_v3(obmat, r_loc);
    if (r_index) {
      /* Does not support index yet. */
      *r_index = -1;
    }
    return SCE_SNAP_MODE_VERTEX;
  }

  return SCE_SNAP_MODE_NONE;
}

/* may extend later (for now just snaps to empty center) */
static eSnapMode snap_object_center(const SnapObjectContext *sctx,
                                    Object *ob_eval,
                                    const float obmat[4][4],
                                    /* read/write args */
                                    float *dist_px,
                                    /* return args */
                                    float r_loc[3],
                                    float *UNUSED(r_no),
                                    int *r_index)
{
  eSnapMode retval = SCE_SNAP_MODE_NONE;

  if (ob_eval->transflag & OB_DUPLI) {
    return retval;
  }

  /* for now only vertex supported */
  if (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX) {
    DistProjectedAABBPrecalc neasrest_precalc;
    dist_squared_to_projected_aabb_precalc(
        &neasrest_precalc, sctx->runtime.pmat, sctx->runtime.win_size, sctx->runtime.mval);

    float tobmat[4][4], clip_planes_local[MAX_CLIPPLANE_LEN][4];
    transpose_m4_m4(tobmat, obmat);
    for (int i = sctx->runtime.clip_plane_len; i--;) {
      mul_v4_m4v4(clip_planes_local[i], tobmat, sctx->runtime.clip_plane[i]);
    }

    bool is_persp = sctx->runtime.view_proj == VIEW_PROJ_PERSP;
    float dist_px_sq = square_f(*dist_px);
    float co[3];
    copy_v3_v3(co, obmat[3]);
    if (test_projected_vert_dist(&neasrest_precalc,
                                 clip_planes_local,
                                 sctx->runtime.clip_plane_len,
                                 is_persp,
                                 co,
                                 &dist_px_sq,
                                 r_loc)) {
      *dist_px = sqrtf(dist_px_sq);
      retval = SCE_SNAP_MODE_VERTEX;
    }
  }

  if (retval) {
    if (r_index) {
      /* Does not support index. */
      *r_index = -1;
    }
    return retval;
  }

  return SCE_SNAP_MODE_NONE;
}

static eSnapMode snapCamera(const SnapObjectContext *sctx,
                            Object *object,
                            const float obmat[4][4],
                            /* read/write args */
                            float *dist_px,
                            /* return args */
                            float r_loc[3],
                            float *r_no,
                            int *r_index)
{
  eSnapMode retval = SCE_SNAP_MODE_NONE;

  Scene *scene = sctx->scene;

  bool is_persp = sctx->runtime.view_proj == VIEW_PROJ_PERSP;
  float dist_px_sq = square_f(*dist_px);

  float orig_camera_mat[4][4], orig_camera_imat[4][4], imat[4][4];
  MovieClip *clip = BKE_object_movieclip_get(scene, object, false);
  MovieTracking *tracking;

  if (clip == nullptr) {
    return snap_object_center(sctx, object, obmat, dist_px, r_loc, r_no, r_index);
  }
  if (object->transflag & OB_DUPLI) {
    return retval;
  }

  tracking = &clip->tracking;

  BKE_tracking_get_camera_object_matrix(object, orig_camera_mat);

  invert_m4_m4(orig_camera_imat, orig_camera_mat);
  invert_m4_m4(imat, obmat);

  if (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX) {
    DistProjectedAABBPrecalc neasrest_precalc;
    dist_squared_to_projected_aabb_precalc(
        &neasrest_precalc, sctx->runtime.pmat, sctx->runtime.win_size, sctx->runtime.mval);

    LISTBASE_FOREACH (MovieTrackingObject *, tracking_object, &tracking->objects) {
      ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, tracking_object);
      float reconstructed_camera_mat[4][4], reconstructed_camera_imat[4][4];
      const float(*vertex_obmat)[4];

      if ((tracking_object->flag & TRACKING_OBJECT_CAMERA) == 0) {
        BKE_tracking_camera_get_reconstructed_interpolate(
            tracking, tracking_object, scene->r.cfra, reconstructed_camera_mat);

        invert_m4_m4(reconstructed_camera_imat, reconstructed_camera_mat);
      }

      LISTBASE_FOREACH (MovieTrackingTrack *, track, tracksbase) {
        float bundle_pos[3];

        if ((track->flag & TRACK_HAS_BUNDLE) == 0) {
          continue;
        }

        copy_v3_v3(bundle_pos, track->bundle_pos);
        if (tracking_object->flag & TRACKING_OBJECT_CAMERA) {
          vertex_obmat = orig_camera_mat;
        }
        else {
          mul_m4_v3(reconstructed_camera_imat, bundle_pos);
          vertex_obmat = obmat;
        }

        mul_m4_v3(vertex_obmat, bundle_pos);
        if (test_projected_vert_dist(&neasrest_precalc,
                                     sctx->runtime.clip_plane,
                                     sctx->runtime.clip_plane_len,
                                     is_persp,
                                     bundle_pos,
                                     &dist_px_sq,
                                     r_loc)) {
          retval = SCE_SNAP_MODE_VERTEX;
        }
      }
    }
  }

  if (retval) {
    *dist_px = sqrtf(dist_px_sq);
    if (r_index) {
      /* Does not support index. */
      *r_index = -1;
    }
    return retval;
  }

  return SCE_SNAP_MODE_NONE;
}

static eSnapMode snapMesh(SnapObjectContext *sctx,
                          const SnapObjectParams *params,
                          Object *ob_eval,
                          const Mesh *me_eval,
                          const float obmat[4][4],
                          bool use_hide,
                          /* read/write args */
                          float *dist_px,
                          /* return args */
                          float r_loc[3],
                          float r_no[3],
                          int *r_index)
{
  BLI_assert(sctx->runtime.snap_to_flag != SCE_SNAP_MODE_FACE_RAYCAST);
  if (me_eval->totvert == 0) {
    return SCE_SNAP_MODE_NONE;
  }
  if (me_eval->totedge == 0 && !(sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX)) {
    return SCE_SNAP_MODE_NONE;
  }

  float lpmat[4][4];
  mul_m4_m4m4(lpmat, sctx->runtime.pmat, obmat);

  float dist_px_sq = square_f(*dist_px);

  /* Test BoundBox */
  const BoundBox *bb = BKE_object_boundbox_get(ob_eval);
  if (bb &&
      !snap_bound_box_check_dist(
          bb->vec[0], bb->vec[6], lpmat, sctx->runtime.win_size, sctx->runtime.mval, dist_px_sq)) {
    return SCE_SNAP_MODE_NONE;
  }

  SnapData_Mesh *sod = snap_object_data_mesh_get(sctx, ob_eval, me_eval, use_hide);

  BVHTreeFromMesh *treedata, treedata_tmp;
  treedata = &sod->treedata_mesh;

  if (sod->has_loose_edge && sod->bvhtree[0] == nullptr) {
    sod->bvhtree[0] = BKE_bvhtree_from_mesh_get(
        &treedata_tmp, me_eval, BVHTREE_FROM_LOOSEEDGES, 2);
    if (sod->bvhtree[0] == nullptr) {
      sod->has_loose_edge = false;
    }
    sod->cached[0] = treedata_tmp.cached;
  }

  if (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX) {
    if (sod->has_loose_vert && sod->bvhtree[1] == nullptr) {
      sod->bvhtree[1] = BKE_bvhtree_from_mesh_get(
          &treedata_tmp, me_eval, BVHTREE_FROM_LOOSEVERTS, 2);
      if (sod->bvhtree[1] == nullptr) {
        sod->has_loose_vert = false;
      }
      sod->cached[1] = treedata_tmp.cached;
    }
  }
  else {
    /* Not necessary, just to keep the data more consistent. */
    sod->has_loose_vert = false;
  }

  Nearest2dUserData nearest2d;
  nearest2d_data_init_mesh(
      sod, sctx->runtime.view_proj == VIEW_PROJ_PERSP, params->use_backface_culling, &nearest2d);

  BVHTreeNearest nearest{};
  nearest.index = -1;
  nearest.dist_sq = dist_px_sq;

  int last_index = nearest.index;
  eSnapMode elem = SCE_SNAP_MODE_VERTEX;

  float tobmat[4][4], clip_planes_local[MAX_CLIPPLANE_LEN][4];
  transpose_m4_m4(tobmat, obmat);
  for (int i = sctx->runtime.clip_plane_len; i--;) {
    mul_v4_m4v4(clip_planes_local[i], tobmat, sctx->runtime.clip_plane[i]);
  }

  if (sod->bvhtree[1] && (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX)) {
    /* snap to loose verts */
    BLI_bvhtree_find_nearest_projected(sod->bvhtree[1],
                                       lpmat,
                                       sctx->runtime.win_size,
                                       sctx->runtime.mval,
                                       clip_planes_local,
                                       sctx->runtime.clip_plane_len,
                                       &nearest,
                                       cb_snap_vert,
                                       &nearest2d);

    last_index = nearest.index;
  }

  if (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_EDGE) {
    if (sod->bvhtree[0]) {
      /* snap to loose edges */
      BLI_bvhtree_find_nearest_projected(sod->bvhtree[0],
                                         lpmat,
                                         sctx->runtime.win_size,
                                         sctx->runtime.mval,
                                         clip_planes_local,
                                         sctx->runtime.clip_plane_len,
                                         &nearest,
                                         cb_snap_edge,
                                         &nearest2d);
    }

    if (treedata->tree) {
      /* snap to looptris */
      BLI_bvhtree_find_nearest_projected(treedata->tree,
                                         lpmat,
                                         sctx->runtime.win_size,
                                         sctx->runtime.mval,
                                         clip_planes_local,
                                         sctx->runtime.clip_plane_len,
                                         &nearest,
                                         cb_snap_tri_edges,
                                         &nearest2d);
    }

    if (last_index != nearest.index) {
      elem = SCE_SNAP_MODE_EDGE;
    }
  }
  else {
    BLI_assert(sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX);
    if (sod->bvhtree[0]) {
      /* snap to loose edge verts */
      BLI_bvhtree_find_nearest_projected(sod->bvhtree[0],
                                         lpmat,
                                         sctx->runtime.win_size,
                                         sctx->runtime.mval,
                                         clip_planes_local,
                                         sctx->runtime.clip_plane_len,
                                         &nearest,
                                         cb_snap_edge_verts,
                                         &nearest2d);
    }

    if (treedata->tree) {
      /* snap to looptri verts */
      BLI_bvhtree_find_nearest_projected(treedata->tree,
                                         lpmat,
                                         sctx->runtime.win_size,
                                         sctx->runtime.mval,
                                         clip_planes_local,
                                         sctx->runtime.clip_plane_len,
                                         &nearest,
                                         cb_snap_tri_verts,
                                         &nearest2d);
    }
  }

  if (nearest.index != -1) {
    *dist_px = sqrtf(nearest.dist_sq);

    copy_v3_v3(r_loc, nearest.co);
    mul_m4_v3(obmat, r_loc);

    if (r_no) {
      float imat[4][4];
      invert_m4_m4(imat, obmat);

      copy_v3_v3(r_no, nearest.no);
      mul_transposed_mat3_m4_v3(imat, r_no);
      normalize_v3(r_no);
    }
    if (r_index) {
      *r_index = nearest.index;
    }

    return elem;
  }

  return SCE_SNAP_MODE_NONE;
}

static eSnapMode snapEditMesh(SnapObjectContext *sctx,
                              const SnapObjectParams *params,
                              Object *ob_eval,
                              BMEditMesh *em,
                              const float obmat[4][4],
                              /* read/write args */
                              float *dist_px,
                              /* return args */
                              float r_loc[3],
                              float r_no[3],
                              int *r_index)
{
  BLI_assert(sctx->runtime.snap_to_flag != SCE_SNAP_MODE_FACE_RAYCAST);

  if ((sctx->runtime.snap_to_flag & ~SCE_SNAP_MODE_FACE_RAYCAST) == SCE_SNAP_MODE_VERTEX) {
    if (em->bm->totvert == 0) {
      return SCE_SNAP_MODE_NONE;
    }
  }
  else {
    if (em->bm->totedge == 0) {
      return SCE_SNAP_MODE_NONE;
    }
  }

  float lpmat[4][4];
  mul_m4_m4m4(lpmat, sctx->runtime.pmat, obmat);

  float dist_px_sq = square_f(*dist_px);

  SnapData_EditMesh *sod = snap_object_data_editmesh_get(sctx, ob_eval, em);

  /* Test BoundBox */

  /* was BKE_boundbox_ray_hit_check, see: cf6ca226fa58 */
  if (!snap_bound_box_check_dist(
          sod->min, sod->max, lpmat, sctx->runtime.win_size, sctx->runtime.mval, dist_px_sq)) {
    return SCE_SNAP_MODE_NONE;
  }

  if (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX) {
    BVHTreeFromEditMesh treedata{};
    treedata.tree = sod->bvhtree[0];

    if (treedata.tree == nullptr) {
      BLI_bitmap *verts_mask = nullptr;
      int verts_num_active = -1;
      if (sctx->callbacks.edit_mesh.test_vert_fn) {
        verts_mask = BLI_BITMAP_NEW(em->bm->totvert, __func__);
        verts_num_active = BM_iter_mesh_bitmap_from_filter(
            BM_VERTS_OF_MESH,
            em->bm,
            verts_mask,
            (bool (*)(BMElem *, void *))sctx->callbacks.edit_mesh.test_vert_fn,
            sctx->callbacks.edit_mesh.user_data);

        bvhtree_from_editmesh_verts_ex(&treedata, em, verts_mask, verts_num_active, 0.0f, 2, 6);
        MEM_freeN(verts_mask);
      }
      else {
        BKE_bvhtree_from_editmesh_get(&treedata,
                                      em,
                                      2,
                                      BVHTREE_FROM_EM_VERTS,
                                      &sod->mesh_runtime->bvh_cache,
                                      (ThreadMutex *)sod->mesh_runtime->eval_mutex);
      }
      sod->bvhtree[0] = treedata.tree;
      sod->cached[0] = treedata.cached;
    }
  }

  if (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_EDGE) {
    BVHTreeFromEditMesh treedata{};
    treedata.tree = sod->bvhtree[1];

    if (treedata.tree == nullptr) {
      BLI_bitmap *edges_mask = nullptr;
      int edges_num_active = -1;
      if (sctx->callbacks.edit_mesh.test_edge_fn) {
        edges_mask = BLI_BITMAP_NEW(em->bm->totedge, __func__);
        edges_num_active = BM_iter_mesh_bitmap_from_filter(
            BM_EDGES_OF_MESH,
            em->bm,
            edges_mask,
            (bool (*)(BMElem *, void *))sctx->callbacks.edit_mesh.test_edge_fn,
            sctx->callbacks.edit_mesh.user_data);

        bvhtree_from_editmesh_edges_ex(&treedata, em, edges_mask, edges_num_active, 0.0f, 2, 6);
        MEM_freeN(edges_mask);
      }
      else {
        BKE_bvhtree_from_editmesh_get(&treedata,
                                      em,
                                      2,
                                      BVHTREE_FROM_EM_EDGES,
                                      &sod->mesh_runtime->bvh_cache,
                                      static_cast<ThreadMutex *>(sod->mesh_runtime->eval_mutex));
      }
      sod->bvhtree[1] = treedata.tree;
      sod->cached[1] = treedata.cached;
    }
  }

  Nearest2dUserData nearest2d;
  nearest2d_data_init_editmesh(
      sod, sctx->runtime.view_proj == VIEW_PROJ_PERSP, params->use_backface_culling, &nearest2d);

  BVHTreeNearest nearest{};
  nearest.index = -1;
  nearest.dist_sq = dist_px_sq;

  eSnapMode elem = SCE_SNAP_MODE_VERTEX;

  float tobmat[4][4], clip_planes_local[MAX_CLIPPLANE_LEN][4];
  transpose_m4_m4(tobmat, obmat);

  for (int i = sctx->runtime.clip_plane_len; i--;) {
    mul_v4_m4v4(clip_planes_local[i], tobmat, sctx->runtime.clip_plane[i]);
  }

  if (sod->bvhtree[0] && (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_VERTEX)) {
    BM_mesh_elem_table_ensure(em->bm, BM_VERT);
    BM_mesh_elem_index_ensure(em->bm, BM_VERT);
    BLI_bvhtree_find_nearest_projected(sod->bvhtree[0],
                                       lpmat,
                                       sctx->runtime.win_size,
                                       sctx->runtime.mval,
                                       clip_planes_local,
                                       sctx->runtime.clip_plane_len,
                                       &nearest,
                                       cb_snap_vert,
                                       &nearest2d);
  }

  if (sod->bvhtree[1] && (sctx->runtime.snap_to_flag & SCE_SNAP_MODE_EDGE)) {
    int last_index = nearest.index;
    nearest.index = -1;
    BM_mesh_elem_table_ensure(em->bm, BM_EDGE | BM_VERT);
    BM_mesh_elem_index_ensure(em->bm, BM_EDGE | BM_VERT);
    BLI_bvhtree_find_nearest_projected(sod->bvhtree[1],
                                       lpmat,
                                       sctx->runtime.win_size,
                                       sctx->runtime.mval,
                                       clip_planes_local,
                                       sctx->runtime.clip_plane_len,
                                       &nearest,
                                       cb_snap_edge,
                                       &nearest2d);

    if (nearest.index != -1) {
      elem = SCE_SNAP_MODE_EDGE;
    }
    else {
      nearest.index = last_index;
    }
  }

  if (nearest.index != -1) {
    *dist_px = sqrtf(nearest.dist_sq);

    copy_v3_v3(r_loc, nearest.co);
    mul_m4_v3(obmat, r_loc);
    if (r_no) {
      float imat[4][4];
      invert_m4_m4(imat, obmat);

      copy_v3_v3(r_no, nearest.no);
      mul_transposed_mat3_m4_v3(imat, r_no);
      normalize_v3(r_no);
    }
    if (r_index) {
      *r_index = nearest.index;
    }

    return elem;
  }

  return SCE_SNAP_MODE_NONE;
}

struct SnapObjUserData {
  /* read/write args */
  float *dist_px;
  /* return args */
  float *r_loc;
  float *r_no;
  int *r_index;
  Object **r_ob;
  float (*r_obmat)[4];
  eSnapMode ret;
};

/**
 * \note Duplicate args here are documented at #snapObjectsRay
 */
static void snap_obj_fn(SnapObjectContext *sctx,
                        const SnapObjectParams *params,
                        Object *ob_eval,
                        const float obmat[4][4],
                        bool is_object_active,
                        void *data)
{
  SnapObjUserData *dt = static_cast<SnapObjUserData *>(data);
  eSnapMode retval = SCE_SNAP_MODE_NONE;

  switch (ob_eval->type) {
    case OB_MESH: {
      const eSnapEditType edit_mode_type = params->edit_mode_type;
      bool use_hide;
      const Mesh *me_eval = mesh_for_snap(ob_eval, edit_mode_type, &use_hide);
      if (me_eval == nullptr) {
        BMEditMesh *em = BKE_editmesh_from_object(ob_eval);
        if (UNLIKELY(!em)) { /* See #mesh_for_snap doc-string. */
          return;
        }
        BLI_assert_msg(em == BKE_editmesh_from_object(DEG_get_original_object(ob_eval)),
                       "Make sure there is only one pointer for looptris");
        retval = snapEditMesh(
            sctx, params, ob_eval, em, obmat, dt->dist_px, dt->r_loc, dt->r_no, dt->r_index);
        break;
      }
      if (ob_eval->dt == OB_BOUNDBOX) {
        /* Do not snap to objects that are in bounding box display mode */
        return;
      }

      retval = snapMesh(sctx,
                        params,
                        ob_eval,
                        me_eval,
                        obmat,
                        use_hide,
                        dt->dist_px,
                        dt->r_loc,
                        dt->r_no,
                        dt->r_index);
      break;
    }
    case OB_ARMATURE:
      retval = snapArmature(sctx,
                            params,
                            ob_eval,
                            obmat,
                            is_object_active,
                            dt->dist_px,
                            dt->r_loc,
                            dt->r_no,
                            dt->r_index);
      break;
    case OB_CURVES_LEGACY:
    case OB_SURF:
      if (ob_eval->type == OB_CURVES_LEGACY || BKE_object_is_in_editmode(ob_eval)) {
        retval = snapCurve(
            sctx, params, ob_eval, obmat, dt->dist_px, dt->r_loc, dt->r_no, dt->r_index);
        if (params->edit_mode_type != SNAP_GEOM_FINAL) {
          break;
        }
      }
      ATTR_FALLTHROUGH;
    case OB_FONT: {
      const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
      if (mesh_eval) {
        retval |= snapMesh(sctx,
                           params,
                           ob_eval,
                           mesh_eval,
                           obmat,
                           false,
                           dt->dist_px,
                           dt->r_loc,
                           dt->r_no,
                           dt->r_index);
      }
      break;
    }
    case OB_EMPTY:
    case OB_GPENCIL:
    case OB_LAMP:
      retval = snap_object_center(
          sctx, ob_eval, obmat, dt->dist_px, dt->r_loc, dt->r_no, dt->r_index);
      break;
    case OB_CAMERA:
      retval = snapCamera(sctx, ob_eval, obmat, dt->dist_px, dt->r_loc, dt->r_no, dt->r_index);
      break;
  }

  if (retval) {
    if (dt->r_ob) {
      *dt->r_ob = ob_eval;
    }
    if (dt->r_obmat) {
      copy_m4_m4(dt->r_obmat, obmat);
    }
    dt->ret = retval;
  }
}

/**
 * Main Snapping Function
 * ======================
 *
 * Walks through all objects in the scene to find the closest snap element ray.
 *
 * \param sctx: Snap context to store data.
 *
 * Read/Write Args
 * ---------------
 *
 * \param dist_px: Maximum threshold distance (in pixels).
 *
 * Output Args
 * -----------
 *
 * \param r_loc: Hit location.
 * \param r_no: Hit normal (optional).
 * \param r_index: Hit index or -1 when no valid index is found.
 * (currently only set to the polygon index when using `snap_to == SCE_SNAP_MODE_FACE_RAYCAST`).
 * \param r_ob: Hit object.
 * \param r_obmat: Object matrix (may not be #Object.obmat with dupli-instances).
 */
static eSnapMode snapObjectsRay(SnapObjectContext *sctx,
                                const SnapObjectParams *params,
                                /* read/write args */
                                /* Parameters below cannot be const, because they are assigned to a
                                 * non-const variable (readability-non-const-parameter). */
                                float *dist_px /* NOLINT */,
                                /* return args */
                                float r_loc[3] /* NOLINT */,
                                float r_no[3] /* NOLINT */,
                                int *r_index /* NOLINT */,
                                Object **r_ob,
                                float r_obmat[4][4])
{
  SnapObjUserData data = {};
  data.dist_px = dist_px;
  data.r_loc = r_loc;
  data.r_no = r_no;
  data.r_ob = r_ob;
  data.r_index = r_index;
  data.r_obmat = r_obmat;
  data.ret = SCE_SNAP_MODE_NONE;

  iter_snap_objects(sctx, params, snap_obj_fn, &data);

  return data.ret;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Object Snapping API
 * \{ */

SnapObjectContext *ED_transform_snap_object_context_create(Scene *scene, int flag)
{
  SnapObjectContext *sctx = MEM_new<SnapObjectContext>(__func__);

  sctx->flag = flag;

  sctx->scene = scene;

  return sctx;
}

void ED_transform_snap_object_context_destroy(SnapObjectContext *sctx)
{
  MEM_delete(sctx);
}

void ED_transform_snap_object_context_set_editmesh_callbacks(
    SnapObjectContext *sctx,
    bool (*test_vert_fn)(BMVert *, void *user_data),
    bool (*test_edge_fn)(BMEdge *, void *user_data),
    bool (*test_face_fn)(BMFace *, void *user_data),
    void *user_data)
{
  sctx->callbacks.edit_mesh.test_vert_fn = test_vert_fn;
  sctx->callbacks.edit_mesh.test_edge_fn = test_edge_fn;
  sctx->callbacks.edit_mesh.test_face_fn = test_face_fn;

  sctx->callbacks.edit_mesh.user_data = user_data;
}

bool ED_transform_snap_object_project_ray_ex(SnapObjectContext *sctx,
                                             Depsgraph *depsgraph,
                                             const View3D *v3d,
                                             const SnapObjectParams *params,
                                             const float ray_start[3],
                                             const float ray_normal[3],
                                             float *ray_depth,
                                             float r_loc[3],
                                             float r_no[3],
                                             int *r_index,
                                             Object **r_ob,
                                             float r_obmat[4][4])
{
  sctx->runtime.depsgraph = depsgraph;
  sctx->runtime.v3d = v3d;

  return raycastObjects(sctx,
                        params,
                        ray_start,
                        ray_normal,
                        ray_depth,
                        r_loc,
                        r_no,
                        r_index,
                        r_ob,
                        r_obmat,
                        nullptr);
}

bool ED_transform_snap_object_project_ray_all(SnapObjectContext *sctx,
                                              Depsgraph *depsgraph,
                                              const View3D *v3d,
                                              const SnapObjectParams *params,
                                              const float ray_start[3],
                                              const float ray_normal[3],
                                              float ray_depth,
                                              bool sort,
                                              ListBase *r_hit_list)
{
  sctx->runtime.depsgraph = depsgraph;
  sctx->runtime.v3d = v3d;

  if (ray_depth == -1.0f) {
    ray_depth = BVH_RAYCAST_DIST_MAX;
  }

#ifdef DEBUG
  float ray_depth_prev = ray_depth;
#endif

  bool retval = raycastObjects(sctx,
                               params,
                               ray_start,
                               ray_normal,
                               &ray_depth,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr,
                               r_hit_list);

  /* meant to be readonly for 'all' hits, ensure it is */
#ifdef DEBUG
  BLI_assert(ray_depth_prev == ray_depth);
#endif

  if (sort) {
    BLI_listbase_sort(r_hit_list, hit_depth_cmp);
  }

  return retval;
}

/**
 * Convenience function for snap ray-casting.
 *
 * Given a ray, cast it into the scene (snapping to faces).
 *
 * \return Snap success
 */
static bool transform_snap_context_project_ray_impl(SnapObjectContext *sctx,
                                                    Depsgraph *depsgraph,
                                                    const View3D *v3d,
                                                    const SnapObjectParams *params,
                                                    const float ray_start[3],
                                                    const float ray_normal[3],
                                                    float *ray_depth,
                                                    float r_co[3],
                                                    float r_no[3])
{
  bool ret;

  /* try snap edge, then face if it fails */
  ret = ED_transform_snap_object_project_ray_ex(sctx,
                                                depsgraph,
                                                v3d,
                                                params,
                                                ray_start,
                                                ray_normal,
                                                ray_depth,
                                                r_co,
                                                r_no,
                                                nullptr,
                                                nullptr,
                                                nullptr);

  return ret;
}

bool ED_transform_snap_object_project_ray(SnapObjectContext *sctx,
                                          Depsgraph *depsgraph,
                                          const View3D *v3d,
                                          const SnapObjectParams *params,
                                          const float ray_origin[3],
                                          const float ray_direction[3],
                                          float *ray_depth,
                                          float r_co[3],
                                          float r_no[3])
{
  float ray_depth_fallback;
  if (ray_depth == nullptr) {
    ray_depth_fallback = BVH_RAYCAST_DIST_MAX;
    ray_depth = &ray_depth_fallback;
  }

  return transform_snap_context_project_ray_impl(
      sctx, depsgraph, v3d, params, ray_origin, ray_direction, ray_depth, r_co, r_no);
}

static eSnapMode transform_snap_context_project_view3d_mixed_impl(SnapObjectContext *sctx,
                                                                  Depsgraph *depsgraph,
                                                                  const ARegion *region,
                                                                  const View3D *v3d,
                                                                  const eSnapMode snap_to_flag,
                                                                  const SnapObjectParams *params,
                                                                  const float init_co[3],
                                                                  const float mval[2],
                                                                  const float prev_co[3],
                                                                  float *dist_px,
                                                                  float r_loc[3],
                                                                  float r_no[3],
                                                                  int *r_index,
                                                                  Object **r_ob,
                                                                  float r_obmat[4][4],
                                                                  float r_face_nor[3])
{
  sctx->runtime.depsgraph = depsgraph;
  sctx->runtime.region = region;
  sctx->runtime.v3d = v3d;

  BLI_assert((snap_to_flag & SCE_SNAP_MODE_GEOM) != 0);

  eSnapMode retval = SCE_SNAP_MODE_NONE;

  bool has_hit = false;
  Object *ob_eval = nullptr;
  float loc[3];
  /* Not all snapping callbacks set the normal,
   * initialize this since any hit copies both the `loc` and `no`. */
  float no[3] = {0.0f, 0.0f, 0.0f};
  float obmat[4][4];
  int index = -1;

  const RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);

  bool use_occlusion_test = params->use_occlusion_test && !XRAY_ENABLED(v3d);

  /* Note: if both face raycast and face nearest are enabled, first find result of nearest, then
   * override with raycast. */
  if ((snap_to_flag & SCE_SNAP_MODE_FACE_NEAREST) && !has_hit) {
    has_hit = nearestWorldObjects(
        sctx, params, init_co, prev_co, loc, no, &index, &ob_eval, obmat);

    if (has_hit) {
      retval = SCE_SNAP_MODE_FACE_NEAREST;

      copy_v3_v3(r_loc, loc);
      if (r_no) {
        copy_v3_v3(r_no, no);
      }
      if (r_ob) {
        *r_ob = ob_eval;
      }
      if (r_obmat) {
        copy_m4_m4(r_obmat, obmat);
      }
      if (r_index) {
        *r_index = index;
      }
    }
  }

  if (snap_to_flag & SCE_SNAP_MODE_FACE_RAYCAST || use_occlusion_test) {
    float ray_start[3], ray_normal[3];
    if (!ED_view3d_win_to_ray_clipped_ex(
            depsgraph, region, v3d, mval, nullptr, ray_normal, ray_start, true)) {
      return retval;
    }

    float dummy_ray_depth = BVH_RAYCAST_DIST_MAX;

    has_hit = raycastObjects(sctx,
                             params,
                             ray_start,
                             ray_normal,
                             &dummy_ray_depth,
                             loc,
                             no,
                             &index,
                             &ob_eval,
                             obmat,
                             nullptr);

    if (has_hit) {
      if (r_face_nor) {
        copy_v3_v3(r_face_nor, no);
      }

      if ((snap_to_flag & SCE_SNAP_MODE_FACE_RAYCAST)) {
        retval = SCE_SNAP_MODE_FACE_RAYCAST;

        copy_v3_v3(r_loc, loc);
        if (r_no) {
          copy_v3_v3(r_no, no);
        }
        if (r_ob) {
          *r_ob = ob_eval;
        }
        if (r_obmat) {
          copy_m4_m4(r_obmat, obmat);
        }
        if (r_index) {
          *r_index = index;
        }
      }
    }
  }

  if (snap_to_flag & (SCE_SNAP_MODE_VERTEX | SCE_SNAP_MODE_EDGE | SCE_SNAP_MODE_EDGE_MIDPOINT |
                      SCE_SNAP_MODE_EDGE_PERPENDICULAR)) {
    eSnapMode elem_test, elem = SCE_SNAP_MODE_NONE;
    float dist_px_tmp = *dist_px;

    copy_m4_m4(sctx->runtime.pmat, rv3d->persmat);
    sctx->runtime.win_size[0] = region->winx;
    sctx->runtime.win_size[1] = region->winy;
    copy_v2_v2(sctx->runtime.mval, mval);
    sctx->runtime.view_proj = rv3d->is_persp ? VIEW_PROJ_PERSP : VIEW_PROJ_ORTHO;

    /* First snap to edge instead of middle or perpendicular. */
    sctx->runtime.snap_to_flag = static_cast<eSnapMode>(
        snap_to_flag & (SCE_SNAP_MODE_VERTEX | SCE_SNAP_MODE_EDGE));
    if (snap_to_flag & (SCE_SNAP_MODE_EDGE_MIDPOINT | SCE_SNAP_MODE_EDGE_PERPENDICULAR)) {
      sctx->runtime.snap_to_flag = static_cast<eSnapMode>(sctx->runtime.snap_to_flag |
                                                          SCE_SNAP_MODE_EDGE);
    }

    planes_from_projmat(sctx->runtime.pmat,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr,
                        sctx->runtime.clip_plane[0],
                        sctx->runtime.clip_plane[1]);

    sctx->runtime.clip_plane_len = 2;
    sctx->runtime.has_occlusion_plane = false;

    /* By convention we only snap to the original elements of a curve. */
    if (has_hit && ob_eval->type != OB_CURVES_LEGACY) {
      /* Compute the new clip_pane but do not add it yet. */
      float new_clipplane[4];
      BLI_ASSERT_UNIT_V3(no);
      plane_from_point_normal_v3(new_clipplane, loc, no);
      if (dot_v3v3(sctx->runtime.clip_plane[0], new_clipplane) > 0.0f) {
        /* The plane is facing the wrong direction. */
        negate_v4(new_clipplane);
      }

      /* Small offset to simulate a kind of volume for edges and vertices. */
      new_clipplane[3] += 0.01f;

      /* Try to snap only to the polygon. */
      elem_test = snap_mesh_polygon(sctx, params, ob_eval, obmat, &dist_px_tmp, loc, no, &index);
      if (elem_test) {
        elem = elem_test;
      }

      /* Add the new clip plane to the beginning of the list. */
      for (int i = sctx->runtime.clip_plane_len; i != 0; i--) {
        copy_v4_v4(sctx->runtime.clip_plane[i], sctx->runtime.clip_plane[i - 1]);
      }
      copy_v4_v4(sctx->runtime.clip_plane[0], new_clipplane);
      sctx->runtime.clip_plane_len++;
      sctx->runtime.has_occlusion_plane = true;
    }

    elem_test = snapObjectsRay(sctx, params, &dist_px_tmp, loc, no, &index, &ob_eval, obmat);
    if (elem_test) {
      elem = elem_test;
    }

    if ((elem == SCE_SNAP_MODE_EDGE) &&
        (snap_to_flag & (SCE_SNAP_MODE_VERTEX | SCE_SNAP_MODE_EDGE_MIDPOINT |
                         SCE_SNAP_MODE_EDGE_PERPENDICULAR))) {
      sctx->runtime.snap_to_flag = snap_to_flag;
      elem = snap_mesh_edge_verts_mixed(
          sctx, params, ob_eval, obmat, *dist_px, prev_co, &dist_px_tmp, loc, no, &index);
    }

    if (elem & snap_to_flag) {
      retval = elem;

      copy_v3_v3(r_loc, loc);
      if (r_no) {
        copy_v3_v3(r_no, no);
      }
      if (r_ob) {
        *r_ob = ob_eval;
      }
      if (r_obmat) {
        copy_m4_m4(r_obmat, obmat);
      }
      if (r_index) {
        *r_index = index;
      }

      *dist_px = dist_px_tmp;
    }
  }

  return retval;
}

eSnapMode ED_transform_snap_object_project_view3d_ex(SnapObjectContext *sctx,
                                                     Depsgraph *depsgraph,
                                                     const ARegion *region,
                                                     const View3D *v3d,
                                                     const eSnapMode snap_to,
                                                     const SnapObjectParams *params,
                                                     const float init_co[3],
                                                     const float mval[2],
                                                     const float prev_co[3],
                                                     float *dist_px,
                                                     float r_loc[3],
                                                     float r_no[3],
                                                     int *r_index,
                                                     Object **r_ob,
                                                     float r_obmat[4][4],
                                                     float r_face_nor[3])
{
  return transform_snap_context_project_view3d_mixed_impl(sctx,
                                                          depsgraph,
                                                          region,
                                                          v3d,
                                                          snap_to,
                                                          params,
                                                          init_co,
                                                          mval,
                                                          prev_co,
                                                          dist_px,
                                                          r_loc,
                                                          r_no,
                                                          r_index,
                                                          r_ob,
                                                          r_obmat,
                                                          r_face_nor);
}

eSnapMode ED_transform_snap_object_project_view3d(SnapObjectContext *sctx,
                                                  Depsgraph *depsgraph,
                                                  const ARegion *region,
                                                  const View3D *v3d,
                                                  const eSnapMode snap_to,
                                                  const SnapObjectParams *params,
                                                  const float init_co[3],
                                                  const float mval[2],
                                                  const float prev_co[3],
                                                  float *dist_px,
                                                  float r_loc[3],
                                                  float r_no[3])
{
  return ED_transform_snap_object_project_view3d_ex(sctx,
                                                    depsgraph,
                                                    region,
                                                    v3d,
                                                    snap_to,
                                                    params,
                                                    init_co,
                                                    mval,
                                                    prev_co,
                                                    dist_px,
                                                    r_loc,
                                                    r_no,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr);
}

bool ED_transform_snap_object_project_all_view3d_ex(SnapObjectContext *sctx,
                                                    Depsgraph *depsgraph,
                                                    const ARegion *region,
                                                    const View3D *v3d,
                                                    const SnapObjectParams *params,
                                                    const float mval[2],
                                                    float ray_depth,
                                                    bool sort,
                                                    ListBase *r_hit_list)
{
  float ray_start[3], ray_normal[3];

  if (!ED_view3d_win_to_ray_clipped_ex(
          depsgraph, region, v3d, mval, nullptr, ray_normal, ray_start, true)) {
    return false;
  }

  return ED_transform_snap_object_project_ray_all(
      sctx, depsgraph, v3d, params, ray_start, ray_normal, ray_depth, sort, r_hit_list);
}

/** \} */
