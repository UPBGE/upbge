/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <climits>
#include <cstddef>
#include <cstdlib>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"

#include "BLI_array.hh"
#include "BLI_float4x4.hh"
#include "BLI_math.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rand.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_anim_types.h"
#include "DNA_collection_types.h"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_scene_types.h"
#include "DNA_vfont_types.h"
#include "DNA_volume_types.h"

#include "BKE_collection.h"
#include "BKE_duplilist.h"
#include "BKE_editmesh.h"
#include "BKE_editmesh_cache.h"
#include "BKE_geometry_set.h"
#include "BKE_geometry_set.hh"
#include "BKE_global.h"
#include "BKE_idprop.h"
#include "BKE_instances.hh"
#include "BKE_lattice.h"
#include "BKE_main.h"
#include "BKE_mesh.h"
#include "BKE_mesh_iterators.h"
#include "BKE_mesh_runtime.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_particle.h"
#include "BKE_scene.h"
#include "BKE_type_conversions.hh"
#include "BKE_vfont.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "BLI_hash.h"
#include "DNA_world_types.h"

#include "NOD_geometry_nodes_log.hh"
#include "RNA_access.h"
#include "RNA_path.h"
#include "RNA_prototypes.h"
#include "RNA_types.h"

using blender::Array;
using blender::float3;
using blender::float4x4;
using blender::Span;
using blender::Vector;
using blender::bke::InstanceReference;
using blender::bke::Instances;
namespace geo_log = blender::nodes::geo_eval_log;

/* -------------------------------------------------------------------- */
/** \name Internal Duplicate Context
 * \{ */

static constexpr short GEOMETRY_SET_DUPLI_GENERATOR_TYPE = 1;

struct DupliContext {
  Depsgraph *depsgraph;
  /** XXX child objects are selected from this group if set, could be nicer. */
  Collection *collection;
  /** Only to check if the object is in edit-mode. */
  Object *obedit;

  Scene *scene;
  /** Root parent object at the scene level. */
  Object *root_object;
  /** Immediate parent object in the context. */
  Object *object;
  float space_mat[4][4];
  /**
   * Index of the top-level instance that contains this context or -1 when unused.
   * This is an index into the instances component of #preview_base_geometry.
   */
  int preview_instance_index;
  /**
   * Top level geometry set that is previewed.
   */
  const GeometrySet *preview_base_geometry;

  /**
   * A stack that contains all the "parent" objects of a particular instance when recursive
   * instancing is used. This is used to prevent objects from instancing themselves accidentally.
   * Use a vector instead of a stack because we want to use the #contains method.
   */
  Vector<Object *> *instance_stack;

  /**
   * Older code relies on the "dupli generator type" for various visibility or processing
   * decisions. However, new code uses geometry instances in places that weren't using the dupli
   * system previously. To fix this, keep track of the last dupli generator type that wasn't a
   * geometry set instance.
   * */
  Vector<short> *dupli_gen_type_stack;

  int persistent_id[MAX_DUPLI_RECUR];
  int64_t instance_idx[MAX_DUPLI_RECUR];
  const GeometrySet *instance_data[MAX_DUPLI_RECUR];
  int level;

  const struct DupliGenerator *gen;

  /** Result containers. */
  ListBase *duplilist; /* Legacy doubly-linked list. */
};

struct DupliGenerator {
  short type; /* Dupli Type, see members of #OB_DUPLI. */
  void (*make_duplis)(const DupliContext *ctx);
};

static const DupliGenerator *get_dupli_generator(const DupliContext *ctx);

/**
 * Create initial context for root object.
 */
static void init_context(DupliContext *r_ctx,
                         Depsgraph *depsgraph,
                         Scene *scene,
                         Object *ob,
                         const float space_mat[4][4],
                         Vector<Object *> &instance_stack,
                         Vector<short> &dupli_gen_type_stack)
{
  r_ctx->depsgraph = depsgraph;
  r_ctx->scene = scene;
  r_ctx->collection = nullptr;

  r_ctx->root_object = ob;
  r_ctx->object = ob;
  r_ctx->obedit = OBEDIT_FROM_OBACT(ob);
  r_ctx->instance_stack = &instance_stack;
  r_ctx->dupli_gen_type_stack = &dupli_gen_type_stack;
  if (space_mat) {
    copy_m4_m4(r_ctx->space_mat, space_mat);
  }
  else {
    unit_m4(r_ctx->space_mat);
  }
  r_ctx->level = 0;

  r_ctx->gen = get_dupli_generator(r_ctx);
  if (r_ctx->gen && r_ctx->gen->type != GEOMETRY_SET_DUPLI_GENERATOR_TYPE) {
    r_ctx->dupli_gen_type_stack->append(r_ctx->gen->type);
  }

  r_ctx->duplilist = nullptr;
  r_ctx->preview_instance_index = -1;
  r_ctx->preview_base_geometry = nullptr;
}

/**
 * Create sub-context for recursive duplis.
 */
static bool copy_dupli_context(DupliContext *r_ctx,
                               const DupliContext *ctx,
                               Object *ob,
                               const float mat[4][4],
                               int index,
                               const GeometrySet *geometry = nullptr,
                               int64_t instance_index = 0)
{
  *r_ctx = *ctx;

  /* XXX annoying, previously was done by passing an ID* argument,
   * this at least is more explicit. */
  if (ctx->gen && ctx->gen->type == OB_DUPLICOLLECTION) {
    r_ctx->collection = ctx->object->instance_collection;
  }

  r_ctx->object = ob;
  r_ctx->instance_stack = ctx->instance_stack;
  if (mat) {
    mul_m4_m4m4(r_ctx->space_mat, (float(*)[4])ctx->space_mat, mat);
  }
  r_ctx->persistent_id[r_ctx->level] = index;
  r_ctx->instance_idx[r_ctx->level] = instance_index;
  r_ctx->instance_data[r_ctx->level] = geometry;
  ++r_ctx->level;

  if (r_ctx->level == MAX_DUPLI_RECUR - 1) {
    std::cerr << "Warning: Maximum instance recursion level reached.\n";
    return false;
  }

  r_ctx->gen = get_dupli_generator(r_ctx);
  if (r_ctx->gen && r_ctx->gen->type != GEOMETRY_SET_DUPLI_GENERATOR_TYPE) {
    r_ctx->dupli_gen_type_stack->append(r_ctx->gen->type);
  }
  return true;
}

/**
 * Generate a dupli instance.
 *
 * \param mat: is transform of the object relative to current context (including
 * #Object.object_to_world).
 */
static DupliObject *make_dupli(const DupliContext *ctx,
                               Object *ob,
                               const ID *object_data,
                               const float mat[4][4],
                               int index,
                               const GeometrySet *geometry = nullptr,
                               int64_t instance_index = 0)
{
  DupliObject *dob;
  int i;

  /* Add a #DupliObject instance to the result container. */
  if (ctx->duplilist) {
    dob = MEM_cnew<DupliObject>("dupli object");
    BLI_addtail(ctx->duplilist, dob);
  }
  else {
    return nullptr;
  }

  dob->ob = ob;
  dob->ob_data = const_cast<ID *>(object_data);
  mul_m4_m4m4(dob->mat, (float(*)[4])ctx->space_mat, mat);
  dob->type = ctx->gen == nullptr ? 0 : ctx->dupli_gen_type_stack->last();
  dob->preview_base_geometry = ctx->preview_base_geometry;
  dob->preview_instance_index = ctx->preview_instance_index;

  /* Set persistent id, which is an array with a persistent index for each level
   * (particle number, vertex number, ..). by comparing this we can find the same
   * dupli-object between frames, which is needed for motion blur.
   * The last level is ordered first in the array. */
  dob->persistent_id[0] = index;
  for (i = 1; i < ctx->level + 1; i++) {
    dob->persistent_id[i] = ctx->persistent_id[ctx->level - i];
  }
  /* Fill rest of values with #INT_MAX which index will never have as value. */
  for (; i < MAX_DUPLI_RECUR; i++) {
    dob->persistent_id[i] = INT_MAX;
  }

  /* Store geometry set data for attribute lookup in innermost to outermost
   * order, copying only non-null entries to save space. */
  const int max_instance = sizeof(dob->instance_data) / sizeof(void *);
  int next_instance = 0;
  if (geometry != nullptr) {
    dob->instance_idx[next_instance] = int(instance_index);
    dob->instance_data[next_instance] = geometry;
    next_instance++;
  }
  for (i = ctx->level - 1; i >= 0 && next_instance < max_instance; i--) {
    if (ctx->instance_data[i] != nullptr) {
      dob->instance_idx[next_instance] = int(ctx->instance_idx[i]);
      dob->instance_data[next_instance] = ctx->instance_data[i];
      next_instance++;
    }
  }

  /* Meta-balls never draw in duplis, they are instead merged into one by the basis
   * meta-ball outside of the group. this does mean that if that meta-ball is not in the
   * scene, they will not show up at all, limitation that should be solved once. */
  if (object_data && GS(object_data->name) == ID_MB) {
    dob->no_draw = true;
  }

  /* Random number per instance.
   * The root object in the scene, persistent ID up to the instance object, and the instance object
   * name together result in a unique random number. */
  dob->random_id = BLI_hash_string(dob->ob->id.name + 2);

  if (dob->persistent_id[0] != INT_MAX) {
    for (i = 0; i < MAX_DUPLI_RECUR; i++) {
      dob->random_id = BLI_hash_int_2d(dob->random_id, uint(dob->persistent_id[i]));
    }
  }
  else {
    dob->random_id = BLI_hash_int_2d(dob->random_id, 0);
  }

  if (ctx->root_object != ob) {
    dob->random_id ^= BLI_hash_int(BLI_hash_string(ctx->root_object->id.name + 2));
  }

  return dob;
}

static DupliObject *make_dupli(const DupliContext *ctx,
                               Object *ob,
                               const float mat[4][4],
                               int index,
                               const GeometrySet *geometry = nullptr,
                               int64_t instance_index = 0)
{
  return make_dupli(ctx, ob, static_cast<ID *>(ob->data), mat, index, geometry, instance_index);
}

/**
 * Recursive dupli-objects.
 *
 * \param space_mat: is the local dupli-space (excluding dupli #Object.object_to_world).
 */
static void make_recursive_duplis(const DupliContext *ctx,
                                  Object *ob,
                                  const float space_mat[4][4],
                                  int index,
                                  const GeometrySet *geometry = nullptr,
                                  int64_t instance_index = 0)
{
  if (ctx->instance_stack->contains(ob)) {
    /* Avoid recursive instances. */
    printf("Warning: '%s' object is trying to instance itself.\n", ob->id.name + 2);
    return;
  }
  /* Simple preventing of too deep nested collections with #MAX_DUPLI_RECUR. */
  if (ctx->level < MAX_DUPLI_RECUR) {
    DupliContext rctx;
    if (!copy_dupli_context(&rctx, ctx, ob, space_mat, index, geometry, instance_index)) {
      return;
    }
    if (rctx.gen) {
      ctx->instance_stack->append(ob);
      rctx.gen->make_duplis(&rctx);
      ctx->instance_stack->remove_last();
      if (rctx.gen->type != GEOMETRY_SET_DUPLI_GENERATOR_TYPE) {
        if (!ctx->dupli_gen_type_stack->is_empty()) {
          ctx->dupli_gen_type_stack->remove_last();
        }
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Child Duplicates (Used by Other Functions)
 * \{ */

using MakeChildDuplisFunc = void (*)(const DupliContext *ctx, void *userdata, Object *child);

static bool is_child(const Object *ob, const Object *parent)
{
  const Object *ob_parent = ob->parent;
  while (ob_parent) {
    if (ob_parent == parent) {
      return true;
    }
    ob_parent = ob_parent->parent;
  }
  return false;
}

/**
 * Create duplis from every child in scene or collection.
 */
static void make_child_duplis(const DupliContext *ctx,
                              void *userdata,
                              MakeChildDuplisFunc make_child_duplis_cb)
{
  Object *parent = ctx->object;

  if (ctx->collection) {
    eEvaluationMode mode = DEG_get_mode(ctx->depsgraph);
    FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_BEGIN (ctx->collection, ob, mode) {
      if ((ob != ctx->obedit) && is_child(ob, parent)) {
        DupliContext pctx;
        if (copy_dupli_context(&pctx, ctx, ctx->object, nullptr, _base_id)) {
          /* Meta-balls have a different dupli handling. */
          if (ob->type != OB_MBALL) {
            ob->flag |= OB_DONE; /* Doesn't render. */
          }
          make_child_duplis_cb(&pctx, userdata, ob);
          if (pctx.gen->type != GEOMETRY_SET_DUPLI_GENERATOR_TYPE) {
            if (!ctx->dupli_gen_type_stack->is_empty()) {
              ctx->dupli_gen_type_stack->remove_last();
            }
          }
        }
      }
    }
    FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_END;
  }
  else {
    /* FIXME: using a mere counter to generate a 'persistent' dupli id is very weak. One possible
     * better solution could be to use `session_uuid` of ID's instead? */
    int persistent_dupli_id = 0;
    DEGObjectIterSettings deg_iter_settings{};
    deg_iter_settings.depsgraph = ctx->depsgraph;
    /* NOTE: this set of flags ensure we only iterate over objects that have a base in either the
     * current scene, or the set (background) scene. */
    deg_iter_settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY |
                              DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET;
    DEG_OBJECT_ITER_BEGIN (&deg_iter_settings, ob) {
      if ((ob != ctx->obedit) && is_child(ob, parent)) {
        DupliContext pctx;
        if (copy_dupli_context(&pctx, ctx, ctx->object, nullptr, persistent_dupli_id)) {
          /* Meta-balls have a different dupli-handling. */
          if (ob->type != OB_MBALL) {
            ob->flag |= OB_DONE; /* Doesn't render. */
          }

          make_child_duplis_cb(&pctx, userdata, ob);
          if (pctx.gen->type != GEOMETRY_SET_DUPLI_GENERATOR_TYPE) {
            if (!ctx->dupli_gen_type_stack->is_empty()) {
              ctx->dupli_gen_type_stack->remove_last();
            }
          }
        }
      }
      persistent_dupli_id++;
    }
    DEG_OBJECT_ITER_END;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Data Access Utilities
 * \{ */

static const Mesh *mesh_data_from_duplicator_object(Object *ob,
                                                    BMEditMesh **r_em,
                                                    const float (**r_vert_coords)[3],
                                                    const float (**r_vert_normals)[3])
{
  /* Gather mesh info. */
  BMEditMesh *em = BKE_editmesh_from_object(ob);
  const Mesh *me_eval;

  *r_em = nullptr;
  *r_vert_coords = nullptr;
  if (r_vert_normals != nullptr) {
    *r_vert_normals = nullptr;
  }

  /* We do not need any render-specific handling anymore, depsgraph takes care of that. */
  /* NOTE: Do direct access to the evaluated mesh: this function is used
   * during meta balls evaluation. But even without those all the objects
   * which are needed for correct instancing are already evaluated. */
  if (em != nullptr) {
    /* Note that this will only show deformation if #eModifierMode_OnCage is enabled.
     * We could change this but it matches 2.7x behavior. */
    me_eval = BKE_object_get_editmesh_eval_cage(ob);
    if ((me_eval == nullptr) || (me_eval->runtime->wrapper_type == ME_WRAPPER_TYPE_BMESH)) {
      EditMeshData *emd = me_eval ? me_eval->runtime->edit_data : nullptr;

      /* Only assign edit-mesh in the case we can't use `me_eval`. */
      *r_em = em;
      me_eval = nullptr;

      if ((emd != nullptr) && (emd->vertexCos != nullptr)) {
        *r_vert_coords = emd->vertexCos;
        if (r_vert_normals != nullptr) {
          BKE_editmesh_cache_ensure_vert_normals(em, emd);
          *r_vert_normals = emd->vertexNos;
        }
      }
    }
  }
  else {
    me_eval = BKE_object_get_evaluated_mesh(ob);
  }
  return me_eval;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dupli-Collection Implementation (#OB_DUPLICOLLECTION)
 * \{ */

static void make_duplis_collection(const DupliContext *ctx)
{
  Object *ob = ctx->object;
  Collection *collection;
  float collection_mat[4][4];

  if (ob->instance_collection == nullptr) {
    return;
  }
  collection = ob->instance_collection;

  /* Combine collection offset and `obmat`. */
  unit_m4(collection_mat);
  sub_v3_v3(collection_mat[3], collection->instance_offset);
  mul_m4_m4m4(collection_mat, ob->object_to_world, collection_mat);
  /* Don't access 'ob->object_to_world' from now on. */

  eEvaluationMode mode = DEG_get_mode(ctx->depsgraph);
  FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_BEGIN (collection, cob, mode) {
    if (cob != ob) {
      float mat[4][4];

      /* Collection dupli-offset, should apply after everything else. */
      mul_m4_m4m4(mat, collection_mat, cob->object_to_world);

      make_dupli(ctx, cob, mat, _base_id);

      /* Recursion. */
      make_recursive_duplis(ctx, cob, collection_mat, _base_id);
    }
  }
  FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_END;
}

static const DupliGenerator gen_dupli_collection = {
    OB_DUPLICOLLECTION,    /* type */
    make_duplis_collection /* make_duplis */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dupli-Vertices Implementation (#OB_DUPLIVERTS for Geometry)
 * \{ */

/** Values shared between different mesh types. */
struct VertexDupliData_Params {
  /**
   * It's important we use this context instead of the `ctx` passed into #make_child_duplis
   * since these won't match in the case of recursion.
   */
  const DupliContext *ctx;

  bool use_rotation;
};

struct VertexDupliData_Mesh {
  VertexDupliData_Params params;

  int totvert;
  const MVert *mvert;
  const float (*vert_normals)[3];

  const float (*orco)[3];
};

struct VertexDupliData_EditMesh {
  VertexDupliData_Params params;

  BMEditMesh *em;

  /* Can be nullptr. */
  const float (*vert_coords)[3];
  const float (*vert_normals)[3];

  /**
   * \note The edit-mesh may assign #DupliObject.orco in cases when a regular mesh wouldn't.
   * For edit-meshes we only check for deformation, for regular meshes we check if #CD_ORCO exists.
   *
   * At the moment this isn't a meaningful difference since requesting #CD_ORCO causes the
   * edit-mesh to be converted into a mesh.
   */
  bool has_orco;
};

/**
 * \param no: The direction,
 * currently this is copied from a `short[3]` normal without division.
 * Can be null when \a use_rotation is false.
 */
static void get_duplivert_transform(const float co[3],
                                    const float no[3],
                                    const bool use_rotation,
                                    const short axis,
                                    const short upflag,
                                    float r_mat[4][4])
{
  float quat[4];
  const float size[3] = {1.0f, 1.0f, 1.0f};

  if (use_rotation) {
    /* Construct rotation matrix from normals. */
    float no_flip[3];
    negate_v3_v3(no_flip, no);
    vec_to_quat(quat, no_flip, axis, upflag);
  }
  else {
    unit_qt(quat);
  }

  loc_quat_size_to_mat4(r_mat, co, quat, size);
}

static DupliObject *vertex_dupli(const DupliContext *ctx,
                                 Object *inst_ob,
                                 const float child_imat[4][4],
                                 int index,
                                 const float co[3],
                                 const float no[3],
                                 const bool use_rotation)
{
  /* `obmat` is transform to vertex. */
  float obmat[4][4];
  get_duplivert_transform(co, no, use_rotation, inst_ob->trackflag, inst_ob->upflag, obmat);

  float space_mat[4][4];

  /* Make offset relative to inst_ob using relative child transform. */
  mul_mat3_m4_v3(child_imat, obmat[3]);
  /* Apply `obmat` _after_ the local vertex transform. */
  mul_m4_m4m4(obmat, inst_ob->object_to_world, obmat);

  /* Space matrix is constructed by removing `obmat` transform,
   * this yields the world-space transform for recursive duplis. */
  mul_m4_m4m4(space_mat, obmat, inst_ob->world_to_object);

  DupliObject *dob = make_dupli(ctx, inst_ob, obmat, index);

  /* Recursion. */
  make_recursive_duplis(ctx, inst_ob, space_mat, index);

  return dob;
}

static void make_child_duplis_verts_from_mesh(const DupliContext *ctx,
                                              void *userdata,
                                              Object *inst_ob)
{
  VertexDupliData_Mesh *vdd = (VertexDupliData_Mesh *)userdata;
  const bool use_rotation = vdd->params.use_rotation;

  const MVert *mvert = vdd->mvert;
  const int totvert = vdd->totvert;

  invert_m4_m4(inst_ob->world_to_object, inst_ob->object_to_world);
  /* Relative transform from parent to child space. */
  float child_imat[4][4];
  mul_m4_m4m4(child_imat, inst_ob->world_to_object, ctx->object->object_to_world);

  for (int i = 0; i < totvert; i++) {
    DupliObject *dob = vertex_dupli(
        vdd->params.ctx, inst_ob, child_imat, i, mvert[i].co, vdd->vert_normals[i], use_rotation);
    if (vdd->orco) {
      copy_v3_v3(dob->orco, vdd->orco[i]);
    }
  }
}

static void make_child_duplis_verts_from_editmesh(const DupliContext *ctx,
                                                  void *userdata,
                                                  Object *inst_ob)
{
  VertexDupliData_EditMesh *vdd = (VertexDupliData_EditMesh *)userdata;
  BMEditMesh *em = vdd->em;
  const bool use_rotation = vdd->params.use_rotation;

  invert_m4_m4(inst_ob->world_to_object, inst_ob->object_to_world);
  /* Relative transform from parent to child space. */
  float child_imat[4][4];
  mul_m4_m4m4(child_imat, inst_ob->world_to_object, ctx->object->object_to_world);

  BMVert *v;
  BMIter iter;
  int i;

  const float(*vert_coords)[3] = vdd->vert_coords;
  const float(*vert_normals)[3] = vdd->vert_normals;

  BM_ITER_MESH_INDEX (v, &iter, em->bm, BM_VERTS_OF_MESH, i) {
    const float *co, *no;
    if (vert_coords != nullptr) {
      co = vert_coords[i];
      no = vert_normals ? vert_normals[i] : nullptr;
    }
    else {
      co = v->co;
      no = v->no;
    }

    DupliObject *dob = vertex_dupli(vdd->params.ctx, inst_ob, child_imat, i, co, no, use_rotation);
    if (vdd->has_orco) {
      copy_v3_v3(dob->orco, v->co);
    }
  }
}

static void make_duplis_verts(const DupliContext *ctx)
{
  Object *parent = ctx->object;
  const bool use_rotation = parent->transflag & OB_DUPLIROT;

  /* Gather mesh info. */
  BMEditMesh *em = nullptr;
  const float(*vert_coords)[3] = nullptr;
  const float(*vert_normals)[3] = nullptr;
  const Mesh *me_eval = mesh_data_from_duplicator_object(
      parent, &em, &vert_coords, use_rotation ? &vert_normals : nullptr);
  if (em == nullptr && me_eval == nullptr) {
    return;
  }

  VertexDupliData_Params vdd_params{ctx, use_rotation};

  if (em != nullptr) {
    VertexDupliData_EditMesh vdd{};
    vdd.params = vdd_params;
    vdd.em = em;
    vdd.vert_coords = vert_coords;
    vdd.vert_normals = vert_normals;
    vdd.has_orco = (vert_coords != nullptr);

    make_child_duplis(ctx, &vdd, make_child_duplis_verts_from_editmesh);
  }
  else {
    VertexDupliData_Mesh vdd{};
    vdd.params = vdd_params;
    vdd.totvert = me_eval->totvert;
    vdd.mvert = me_eval->verts().data();
    vdd.vert_normals = BKE_mesh_vertex_normals_ensure(me_eval);
    vdd.orco = (const float(*)[3])CustomData_get_layer(&me_eval->vdata, CD_ORCO);

    make_child_duplis(ctx, &vdd, make_child_duplis_verts_from_mesh);
  }
}

static const DupliGenerator gen_dupli_verts = {
    OB_DUPLIVERTS,    /* type */
    make_duplis_verts /* make_duplis */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dupli-Vertices Implementation (#OB_DUPLIVERTS for 3D Text)
 * \{ */

static Object *find_family_object(
    Main *bmain, const char *family, size_t family_len, uint ch, GHash *family_gh)
{
  void *ch_key = POINTER_FROM_UINT(ch);

  Object **ob_pt;
  if ((ob_pt = (Object **)BLI_ghash_lookup_p(family_gh, ch_key))) {
    return *ob_pt;
  }

  char ch_utf8[BLI_UTF8_MAX + 1];
  size_t ch_utf8_len;

  ch_utf8_len = BLI_str_utf8_from_unicode(ch, ch_utf8, sizeof(ch_utf8) - 1);
  ch_utf8[ch_utf8_len] = '\0';
  ch_utf8_len += 1; /* Compare with null terminator. */

  LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
    if (STREQLEN(ob->id.name + 2 + family_len, ch_utf8, ch_utf8_len)) {
      if (STREQLEN(ob->id.name + 2, family, family_len)) {
        /* Inserted value can be nullptr, just to save searches in future. */
        BLI_ghash_insert(family_gh, ch_key, ob);
        return ob;
      }
    }
  }

  return nullptr;
}

static void make_duplis_font(const DupliContext *ctx)
{
  Object *par = ctx->object;
  GHash *family_gh;
  Object *ob;
  Curve *cu;
  struct CharTrans *ct, *chartransdata = nullptr;
  float vec[3], obmat[4][4], pmat[4][4], fsize, xof, yof;
  int text_len, a;
  size_t family_len;
  const char32_t *text = nullptr;
  bool text_free = false;

  /* Font dupli-verts not supported inside collections. */
  if (ctx->collection) {
    return;
  }

  copy_m4_m4(pmat, par->object_to_world);

  /* In `par` the family name is stored, use this to find the other objects. */

  BKE_vfont_to_curve_ex(
      par, (Curve *)par->data, FO_DUPLI, nullptr, &text, &text_len, &text_free, &chartransdata);

  if (text == nullptr || chartransdata == nullptr) {
    return;
  }

  cu = (Curve *)par->data;
  fsize = cu->fsize;
  xof = cu->xof;
  yof = cu->yof;

  ct = chartransdata;

  /* Cache result. */
  family_len = strlen(cu->family);
  family_gh = BLI_ghash_int_new_ex(__func__, 256);

  /* Safety check even if it might fail badly when called for original object. */
  const bool is_eval_curve = DEG_is_evaluated_id(&cu->id);

  /* Advance matching BLI_str_utf8_as_utf32. */
  for (a = 0; a < text_len; a++, ct++) {

    /* XXX That G.main is *really* ugly, but not sure what to do here.
     * Definitively don't think it would be safe to put back `Main *bmain` pointer
     * in #DupliContext as done in 2.7x? */
    ob = find_family_object(G.main, cu->family, family_len, uint(text[a]), family_gh);

    if (is_eval_curve) {
      /* Workaround for the above hack. */
      ob = DEG_get_evaluated_object(ctx->depsgraph, ob);
    }

    if (ob) {
      vec[0] = fsize * (ct->xof - xof);
      vec[1] = fsize * (ct->yof - yof);
      vec[2] = 0.0;

      mul_m4_v3(pmat, vec);

      copy_m4_m4(obmat, par->object_to_world);

      if (UNLIKELY(ct->rot != 0.0f)) {
        float rmat[4][4];

        zero_v3(obmat[3]);
        axis_angle_to_mat4_single(rmat, 'Z', -ct->rot);
        mul_m4_m4m4(obmat, obmat, rmat);
      }

      copy_v3_v3(obmat[3], vec);

      make_dupli(ctx, ob, obmat, a);
    }
  }

  if (text_free) {
    MEM_freeN((void *)text);
  }

  BLI_ghash_free(family_gh, nullptr, nullptr);

  MEM_freeN(chartransdata);
}

static const DupliGenerator gen_dupli_verts_font = {
    OB_DUPLIVERTS,   /* type */
    make_duplis_font /* make_duplis */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Instances Geometry Component Implementation
 * \{ */

static void make_duplis_geometry_set_impl(const DupliContext *ctx,
                                          const GeometrySet &geometry_set,
                                          const float parent_transform[4][4],
                                          bool geometry_set_is_instance,
                                          bool use_new_curves_type)
{
  int component_index = 0;
  if (ctx->object->type != OB_MESH || geometry_set_is_instance) {
    if (const Mesh *mesh = geometry_set.get_mesh_for_read()) {
      make_dupli(ctx, ctx->object, &mesh->id, parent_transform, component_index++);
    }
  }
  if (ctx->object->type != OB_VOLUME || geometry_set_is_instance) {
    if (const Volume *volume = geometry_set.get_volume_for_read()) {
      make_dupli(ctx, ctx->object, &volume->id, parent_transform, component_index++);
    }
  }
  if (!ELEM(ctx->object->type, OB_CURVES_LEGACY, OB_FONT, OB_CURVES) || geometry_set_is_instance) {
    if (const CurveComponent *component = geometry_set.get_component_for_read<CurveComponent>()) {
      if (use_new_curves_type) {
        if (const Curves *curves = component->get_for_read()) {
          make_dupli(ctx, ctx->object, &curves->id, parent_transform, component_index++);
        }
      }
      else {
        if (const Curve *curve = component->get_curve_for_render()) {
          make_dupli(ctx, ctx->object, &curve->id, parent_transform, component_index++);
        }
      }
    }
  }
  if (ctx->object->type != OB_POINTCLOUD || geometry_set_is_instance) {
    if (const PointCloud *pointcloud = geometry_set.get_pointcloud_for_read()) {
      make_dupli(ctx, ctx->object, &pointcloud->id, parent_transform, component_index++);
    }
  }
  const bool creates_duplis_for_components = component_index >= 1;

  const Instances *instances = geometry_set.get_instances_for_read();
  if (instances == nullptr) {
    return;
  }

  const DupliContext *instances_ctx = ctx;
  /* Create a sub-context if some duplis were created above. This is to avoid dupli id collisions
   * between the instances component below and the other components above. */
  DupliContext new_instances_ctx;
  if (creates_duplis_for_components) {
    if (!copy_dupli_context(&new_instances_ctx, ctx, ctx->object, nullptr, component_index)) {
      return;
    }
    instances_ctx = &new_instances_ctx;
  }

  Span<float4x4> instance_offset_matrices = instances->transforms();
  Span<int> reference_handles = instances->reference_handles();
  Span<int> almost_unique_ids = instances->almost_unique_ids();
  Span<InstanceReference> references = instances->references();

  for (int64_t i : instance_offset_matrices.index_range()) {
    const InstanceReference &reference = references[reference_handles[i]];
    const int id = almost_unique_ids[i];

    const DupliContext *ctx_for_instance = instances_ctx;
    /* Set the #preview_instance_index when necessary. */
    DupliContext tmp_ctx_for_instance;
    if (instances_ctx->preview_base_geometry == &geometry_set) {
      tmp_ctx_for_instance = *instances_ctx;
      tmp_ctx_for_instance.preview_instance_index = i;
      ctx_for_instance = &tmp_ctx_for_instance;
    }

    switch (reference.type()) {
      case InstanceReference::Type::Object: {
        Object &object = reference.object();
        float matrix[4][4];
        mul_m4_m4m4(matrix, parent_transform, instance_offset_matrices[i].values);
        make_dupli(ctx_for_instance, &object, matrix, id, &geometry_set, i);

        float space_matrix[4][4];
        mul_m4_m4m4(space_matrix, instance_offset_matrices[i].values, object.world_to_object);
        mul_m4_m4_pre(space_matrix, parent_transform);
        make_recursive_duplis(ctx_for_instance, &object, space_matrix, id, &geometry_set, i);
        break;
      }
      case InstanceReference::Type::Collection: {
        Collection &collection = reference.collection();
        float collection_matrix[4][4];
        unit_m4(collection_matrix);
        sub_v3_v3(collection_matrix[3], collection.instance_offset);
        mul_m4_m4_pre(collection_matrix, instance_offset_matrices[i].values);
        mul_m4_m4_pre(collection_matrix, parent_transform);

        DupliContext sub_ctx;
        if (!copy_dupli_context(&sub_ctx,
                                ctx_for_instance,
                                ctx_for_instance->object,
                                nullptr,
                                id,
                                &geometry_set,
                                i)) {
          break;
        }

        eEvaluationMode mode = DEG_get_mode(ctx_for_instance->depsgraph);
        int object_id = 0;
        FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_BEGIN (&collection, object, mode) {
          if (object == ctx_for_instance->object) {
            continue;
          }

          float instance_matrix[4][4];
          mul_m4_m4m4(instance_matrix, collection_matrix, object->object_to_world);

          make_dupli(&sub_ctx, object, instance_matrix, object_id++);
          make_recursive_duplis(&sub_ctx, object, collection_matrix, object_id++);
        }
        FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_END;
        break;
      }
      case InstanceReference::Type::GeometrySet: {
        float new_transform[4][4];
        mul_m4_m4m4(new_transform, parent_transform, instance_offset_matrices[i].values);

        DupliContext sub_ctx;
        if (copy_dupli_context(&sub_ctx,
                               ctx_for_instance,
                               ctx_for_instance->object,
                               nullptr,
                               id,
                               &geometry_set,
                               i)) {
          make_duplis_geometry_set_impl(
              &sub_ctx, reference.geometry_set(), new_transform, true, false);
        }
        break;
      }
      case InstanceReference::Type::None: {
        break;
      }
    }
  }
}

static void make_duplis_geometry_set(const DupliContext *ctx)
{
  const GeometrySet *geometry_set = ctx->object->runtime.geometry_set_eval;
  make_duplis_geometry_set_impl(ctx, *geometry_set, ctx->object->object_to_world, false, false);
}

static const DupliGenerator gen_dupli_geometry_set = {
    GEOMETRY_SET_DUPLI_GENERATOR_TYPE,
    make_duplis_geometry_set,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dupli-Faces Implementation (#OB_DUPLIFACES)
 * \{ */

/** Values shared between different mesh types. */
struct FaceDupliData_Params {
  /**
   * It's important we use this context instead of the `ctx` passed into #make_child_duplis
   * since these won't match in the case of recursion.
   */
  const DupliContext *ctx;

  bool use_scale;
};

struct FaceDupliData_Mesh {
  FaceDupliData_Params params;

  int totface;
  const MPoly *mpoly;
  const MLoop *mloop;
  const MVert *mvert;
  const float (*orco)[3];
  const MLoopUV *mloopuv;
};

struct FaceDupliData_EditMesh {
  FaceDupliData_Params params;

  BMEditMesh *em;

  bool has_orco, has_uvs;
  int cd_loop_uv_offset;
  /* Can be nullptr. */
  const float (*vert_coords)[3];
};

static void get_dupliface_transform_from_coords(Span<float3> coords,
                                                const bool use_scale,
                                                const float scale_fac,
                                                float r_mat[4][4])
{
  using namespace blender::math;

  /* Location. */
  float3 location(0);
  for (const float3 &coord : coords) {
    location += coord;
  }
  location *= 1.0f / float(coords.size());

  /* Rotation. */
  float quat[4];

  float3 f_no = normalize(cross_poly(coords));
  tri_to_quat_ex(quat, coords[0], coords[1], coords[2], f_no);

  /* Scale. */
  float scale;
  if (use_scale) {
    const float area = area_poly_v3((const float(*)[3])coords.data(), uint(coords.size()));
    scale = sqrtf(area) * scale_fac;
  }
  else {
    scale = 1.0f;
  }

  loc_quat_size_to_mat4(r_mat, location, quat, float3(scale));
}

static DupliObject *face_dupli(const DupliContext *ctx,
                               Object *inst_ob,
                               const float child_imat[4][4],
                               const int index,
                               const bool use_scale,
                               const float scale_fac,
                               Span<float3> coords)
{
  float obmat[4][4];
  float space_mat[4][4];

  /* `obmat` is transform to face. */
  get_dupliface_transform_from_coords(coords, use_scale, scale_fac, obmat);

  /* Make offset relative to inst_ob using relative child transform. */
  mul_mat3_m4_v3(child_imat, obmat[3]);

  /* XXX ugly hack to ensure same behavior as in master.
   * This should not be needed, #Object.parentinv is not consistent outside of parenting. */
  {
    float imat[3][3];
    copy_m3_m4(imat, inst_ob->parentinv);
    mul_m4_m3m4(obmat, imat, obmat);
  }

  /* Apply `obmat` _after_ the local face transform. */
  mul_m4_m4m4(obmat, inst_ob->object_to_world, obmat);

  /* Space matrix is constructed by removing `obmat` transform,
   * this yields the world-space transform for recursive duplis. */
  mul_m4_m4m4(space_mat, obmat, inst_ob->world_to_object);

  DupliObject *dob = make_dupli(ctx, inst_ob, obmat, index);

  /* Recursion. */
  make_recursive_duplis(ctx, inst_ob, space_mat, index);

  return dob;
}

static DupliObject *face_dupli_from_mesh(const DupliContext *ctx,
                                         Object *inst_ob,
                                         const float child_imat[4][4],
                                         const int index,
                                         const bool use_scale,
                                         const float scale_fac,

                                         /* Mesh variables. */
                                         const MPoly *mpoly,
                                         const MLoop *mloopstart,
                                         const MVert *mvert)
{
  const int coords_len = mpoly->totloop;
  Array<float3, 64> coords(coords_len);

  const MLoop *ml = mloopstart;
  for (int i = 0; i < coords_len; i++, ml++) {
    coords[i] = float3(mvert[ml->v].co);
  }

  return face_dupli(ctx, inst_ob, child_imat, index, use_scale, scale_fac, coords);
}

static DupliObject *face_dupli_from_editmesh(const DupliContext *ctx,
                                             Object *inst_ob,
                                             const float child_imat[4][4],
                                             const int index,
                                             const bool use_scale,
                                             const float scale_fac,

                                             /* Mesh variables. */
                                             BMFace *f,
                                             const float (*vert_coords)[3])
{
  const int coords_len = f->len;
  Array<float3, 64> coords(coords_len);

  BMLoop *l_first, *l_iter;
  int i = 0;
  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  if (vert_coords != nullptr) {
    do {
      copy_v3_v3(coords[i++], vert_coords[BM_elem_index_get(l_iter->v)]);
    } while ((l_iter = l_iter->next) != l_first);
  }
  else {
    do {
      copy_v3_v3(coords[i++], l_iter->v->co);
    } while ((l_iter = l_iter->next) != l_first);
  }

  return face_dupli(ctx, inst_ob, child_imat, index, use_scale, scale_fac, coords);
}

static void make_child_duplis_faces_from_mesh(const DupliContext *ctx,
                                              void *userdata,
                                              Object *inst_ob)
{
  FaceDupliData_Mesh *fdd = (FaceDupliData_Mesh *)userdata;
  const MPoly *mpoly = fdd->mpoly, *mp;
  const MLoop *mloop = fdd->mloop;
  const MVert *mvert = fdd->mvert;
  const float(*orco)[3] = fdd->orco;
  const MLoopUV *mloopuv = fdd->mloopuv;
  const int totface = fdd->totface;
  const bool use_scale = fdd->params.use_scale;
  int a;

  float child_imat[4][4];

  invert_m4_m4(inst_ob->world_to_object, inst_ob->object_to_world);
  /* Relative transform from parent to child space. */
  mul_m4_m4m4(child_imat, inst_ob->world_to_object, ctx->object->object_to_world);
  const float scale_fac = ctx->object->instance_faces_scale;

  for (a = 0, mp = mpoly; a < totface; a++, mp++) {
    const MLoop *loopstart = mloop + mp->loopstart;
    DupliObject *dob = face_dupli_from_mesh(
        fdd->params.ctx, inst_ob, child_imat, a, use_scale, scale_fac, mp, loopstart, mvert);

    const float w = 1.0f / float(mp->totloop);
    if (orco) {
      for (int j = 0; j < mp->totloop; j++) {
        madd_v3_v3fl(dob->orco, orco[loopstart[j].v], w);
      }
    }
    if (mloopuv) {
      for (int j = 0; j < mp->totloop; j++) {
        madd_v2_v2fl(dob->uv, mloopuv[mp->loopstart + j].uv, w);
      }
    }
  }
}

static void make_child_duplis_faces_from_editmesh(const DupliContext *ctx,
                                                  void *userdata,
                                                  Object *inst_ob)
{
  FaceDupliData_EditMesh *fdd = (FaceDupliData_EditMesh *)userdata;
  BMEditMesh *em = fdd->em;
  float child_imat[4][4];
  int a;
  BMFace *f;
  BMIter iter;
  const bool use_scale = fdd->params.use_scale;

  const float(*vert_coords)[3] = fdd->vert_coords;

  BLI_assert((vert_coords == nullptr) || (em->bm->elem_index_dirty & BM_VERT) == 0);

  invert_m4_m4(inst_ob->world_to_object, inst_ob->object_to_world);
  /* Relative transform from parent to child space. */
  mul_m4_m4m4(child_imat, inst_ob->world_to_object, ctx->object->object_to_world);
  const float scale_fac = ctx->object->instance_faces_scale;

  BM_ITER_MESH_INDEX (f, &iter, em->bm, BM_FACES_OF_MESH, a) {
    DupliObject *dob = face_dupli_from_editmesh(
        fdd->params.ctx, inst_ob, child_imat, a, use_scale, scale_fac, f, vert_coords);

    if (fdd->has_orco) {
      const float w = 1.0f / float(f->len);
      BMLoop *l_first, *l_iter;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        madd_v3_v3fl(dob->orco, l_iter->v->co, w);
      } while ((l_iter = l_iter->next) != l_first);
    }
    if (fdd->has_uvs) {
      BM_face_uv_calc_center_median(f, fdd->cd_loop_uv_offset, dob->uv);
    }
  }
}

static void make_duplis_faces(const DupliContext *ctx)
{
  Object *parent = ctx->object;

  /* Gather mesh info. */
  BMEditMesh *em = nullptr;
  const float(*vert_coords)[3] = nullptr;
  const Mesh *me_eval = mesh_data_from_duplicator_object(parent, &em, &vert_coords, nullptr);
  if (em == nullptr && me_eval == nullptr) {
    return;
  }

  FaceDupliData_Params fdd_params = {ctx, (parent->transflag & OB_DUPLIFACES_SCALE) != 0};

  if (em != nullptr) {
    const int uv_idx = CustomData_get_render_layer(&em->bm->ldata, CD_MLOOPUV);
    FaceDupliData_EditMesh fdd{};
    fdd.params = fdd_params;
    fdd.em = em;
    fdd.vert_coords = vert_coords;
    fdd.has_orco = (vert_coords != nullptr);
    fdd.has_uvs = (uv_idx != -1);
    fdd.cd_loop_uv_offset = (uv_idx != -1) ?
                                CustomData_get_n_offset(&em->bm->ldata, CD_MLOOPUV, uv_idx) :
                                -1;
    make_child_duplis(ctx, &fdd, make_child_duplis_faces_from_editmesh);
  }
  else {
    const int uv_idx = CustomData_get_render_layer(&me_eval->ldata, CD_MLOOPUV);
    FaceDupliData_Mesh fdd{};
    fdd.params = fdd_params;
    fdd.totface = me_eval->totpoly;
    fdd.mpoly = me_eval->polys().data();
    fdd.mloop = me_eval->loops().data();
    fdd.mvert = me_eval->verts().data();
    fdd.mloopuv = (uv_idx != -1) ? (const MLoopUV *)CustomData_get_layer_n(
                                       &me_eval->ldata, CD_MLOOPUV, uv_idx) :
                                   nullptr;
    fdd.orco = (const float(*)[3])CustomData_get_layer(&me_eval->vdata, CD_ORCO);

    make_child_duplis(ctx, &fdd, make_child_duplis_faces_from_mesh);
  }
}

static const DupliGenerator gen_dupli_faces = {
    OB_DUPLIFACES,    /* type */
    make_duplis_faces /* make_duplis */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dupli-Particles Implementation (#OB_DUPLIPARTS)
 * \{ */

static void make_duplis_particle_system(const DupliContext *ctx, ParticleSystem *psys)
{
  Scene *scene = ctx->scene;
  Object *par = ctx->object;
  eEvaluationMode mode = DEG_get_mode(ctx->depsgraph);
  bool for_render = mode == DAG_EVAL_RENDER;

  Object *ob = nullptr, **oblist = nullptr;
  DupliObject *dob;
  ParticleSettings *part;
  ParticleData *pa;
  ChildParticle *cpa = nullptr;
  ParticleKey state;
  ParticleCacheKey *cache;
  float ctime, scale = 1.0f;
  float tmat[4][4], mat[4][4], pamat[4][4], size = 0.0;
  int a, b, hair = 0;
  int totpart, totchild;

  int no_draw_flag = PARS_UNEXIST;

  if (psys == nullptr) {
    return;
  }

  part = psys->part;

  if (part == nullptr) {
    return;
  }

  if (!psys_check_enabled(par, psys, for_render)) {
    return;
  }

  if (!for_render) {
    no_draw_flag |= PARS_NO_DISP;
  }

  /* NOTE: in old animation system, used parent object's time-offset. */
  ctime = DEG_get_ctime(ctx->depsgraph);

  totpart = psys->totpart;
  totchild = psys->totchild;

  if ((for_render || part->draw_as == PART_DRAW_REND) &&
      ELEM(part->ren_as, PART_DRAW_OB, PART_DRAW_GR)) {
    ParticleSimulationData sim = {nullptr};
    sim.depsgraph = ctx->depsgraph;
    sim.scene = scene;
    sim.ob = par;
    sim.psys = psys;
    sim.psmd = psys_get_modifier(par, psys);
    /* Make sure emitter `world_to_object` is in global coordinates instead of render view
     * coordinates. */
    invert_m4_m4(par->world_to_object, par->object_to_world);

    /* First check for loops (particle system object used as dupli-object). */
    if (part->ren_as == PART_DRAW_OB) {
      if (ELEM(part->instance_object, nullptr, par)) {
        return;
      }
    }
    else { /* #PART_DRAW_GR. */
      if (part->instance_collection == nullptr) {
        return;
      }

      const ListBase dup_collection_objects = BKE_collection_object_cache_get(
          part->instance_collection);
      if (BLI_listbase_is_empty(&dup_collection_objects)) {
        return;
      }

      if (BLI_findptr(&dup_collection_objects, par, offsetof(Base, object))) {
        return;
      }
    }

    /* If we have a hair particle system, use the path cache. */
    if (part->type == PART_HAIR) {
      if (psys->flag & PSYS_HAIR_DONE) {
        hair = (totchild == 0 || psys->childcache) && psys->pathcache;
      }
      if (!hair) {
        return;
      }

      /* We use cache, update `totchild` according to cached data. */
      totchild = psys->totchildcache;
      totpart = psys->totcached;
    }

    RNG *rng = BLI_rng_new_srandom(31415926u + uint(psys->seed));

    psys_sim_data_init(&sim);

    /* Gather list of objects or single object. */
    int totcollection = 0;

    const bool use_whole_collection = part->draw & PART_DRAW_WHOLE_GR;
    const bool use_collection_count = part->draw & PART_DRAW_COUNT_GR && !use_whole_collection;
    if (part->ren_as == PART_DRAW_GR) {
      if (use_collection_count) {
        psys_find_group_weights(part);
        LISTBASE_FOREACH (ParticleDupliWeight *, dw, &part->instance_weights) {
          FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_BEGIN (
              part->instance_collection, object, mode) {
            if (dw->ob == object) {
              totcollection += dw->count;
              break;
            }
          }
          FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_END;
        }
      }
      else {
        FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_BEGIN (
            part->instance_collection, object, mode) {
          (void)object;
          totcollection++;
        }
        FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_END;
      }

      oblist = (Object **)MEM_callocN(size_t(totcollection) * sizeof(Object *),
                                      "dupcollection object list");

      if (use_collection_count) {
        a = 0;
        LISTBASE_FOREACH (ParticleDupliWeight *, dw, &part->instance_weights) {
          FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_BEGIN (
              part->instance_collection, object, mode) {
            if (dw->ob == object) {
              for (b = 0; b < dw->count; b++, a++) {
                oblist[a] = dw->ob;
              }
              break;
            }
          }
          FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_END;
        }
      }
      else {
        a = 0;
        FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_BEGIN (
            part->instance_collection, object, mode) {
          oblist[a] = object;
          a++;
        }
        FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_END;
      }
    }
    else {
      ob = part->instance_object;
    }

    if (totchild == 0 || part->draw & PART_DRAW_PARENT) {
      a = 0;
    }
    else {
      a = totpart;
    }

    for (pa = psys->particles; a < totpart + totchild; a++, pa++) {
      if (a < totpart) {
        /* Handle parent particle. */
        if (pa->flag & no_draw_flag) {
          continue;
        }

#if 0 /* UNUSED */
        pa_num = pa->num;
#endif
        size = pa->size;
      }
      else {
        /* Handle child particle. */
        cpa = &psys->child[a - totpart];

#if 0 /* UNUSED */
        pa_num = a;
#endif
        size = psys_get_child_size(psys, cpa, ctime, nullptr);
      }

      /* Some hair paths might be non-existent so they can't be used for duplication. */
      if (hair && psys->pathcache &&
          ((a < totpart && psys->pathcache[a]->segments < 0) ||
           (a >= totpart && psys->childcache[a - totpart]->segments < 0))) {
        continue;
      }

      if (part->ren_as == PART_DRAW_GR) {
        /* Prevent divide by zero below T28336. */
        if (totcollection == 0) {
          continue;
        }

        /* For collections, pick the object based on settings. */
        if (part->draw & PART_DRAW_RAND_GR && !use_whole_collection) {
          b = BLI_rng_get_int(rng) % totcollection;
        }
        else {
          b = a % totcollection;
        }

        ob = oblist[b];
      }

      if (hair) {
        /* Hair we handle separate and compute transform based on hair keys. */
        if (a < totpart) {
          cache = psys->pathcache[a];
          psys_get_dupli_path_transform(&sim, pa, nullptr, cache, pamat, &scale);
        }
        else {
          cache = psys->childcache[a - totpart];
          psys_get_dupli_path_transform(&sim, nullptr, cpa, cache, pamat, &scale);
        }

        copy_v3_v3(pamat[3], cache->co);
        pamat[3][3] = 1.0f;
      }
      else {
        /* First key. */
        state.time = ctime;
        if (psys_get_particle_state(&sim, a, &state, false) == 0) {
          continue;
        }

        float tquat[4];
        normalize_qt_qt(tquat, state.rot);
        quat_to_mat4(pamat, tquat);
        copy_v3_v3(pamat[3], state.co);
        pamat[3][3] = 1.0f;
      }

      if (part->ren_as == PART_DRAW_GR && psys->part->draw & PART_DRAW_WHOLE_GR) {
        b = 0;
        FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_BEGIN (
            part->instance_collection, object, mode) {
          copy_m4_m4(tmat, oblist[b]->object_to_world);

          /* Apply collection instance offset. */
          sub_v3_v3(tmat[3], part->instance_collection->instance_offset);

          /* Apply particle scale. */
          mul_mat3_m4_fl(tmat, size * scale);
          mul_v3_fl(tmat[3], size * scale);

          /* Individual particle transform. */
          mul_m4_m4m4(mat, pamat, tmat);

          dob = make_dupli(ctx, object, mat, a);
          dob->particle_system = psys;

          psys_get_dupli_texture(psys, part, sim.psmd, pa, cpa, dob->uv, dob->orco);

          b++;
        }
        FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_END;
      }
      else {
        float obmat[4][4];
        copy_m4_m4(obmat, ob->object_to_world);

        float vec[3];
        copy_v3_v3(vec, obmat[3]);
        zero_v3(obmat[3]);

        /* Particle rotation uses x-axis as the aligned axis,
         * so pre-rotate the object accordingly. */
        if ((part->draw & PART_DRAW_ROTATE_OB) == 0) {
          float xvec[3], q[4], size_mat[4][4], original_size[3];

          mat4_to_size(original_size, obmat);
          size_to_mat4(size_mat, original_size);

          xvec[0] = -1.0f;
          xvec[1] = xvec[2] = 0;
          vec_to_quat(q, xvec, ob->trackflag, ob->upflag);
          quat_to_mat4(obmat, q);
          obmat[3][3] = 1.0f;

          /* Add scaling if requested. */
          if ((part->draw & PART_DRAW_NO_SCALE_OB) == 0) {
            mul_m4_m4m4(obmat, obmat, size_mat);
          }
        }
        else if (part->draw & PART_DRAW_NO_SCALE_OB) {
          /* Remove scaling. */
          float size_mat[4][4], original_size[3];

          mat4_to_size(original_size, obmat);
          size_to_mat4(size_mat, original_size);
          invert_m4(size_mat);

          mul_m4_m4m4(obmat, obmat, size_mat);
        }

        mul_m4_m4m4(tmat, pamat, obmat);
        mul_mat3_m4_fl(tmat, size * scale);

        copy_m4_m4(mat, tmat);

        if (part->draw & PART_DRAW_GLOBAL_OB) {
          add_v3_v3v3(mat[3], mat[3], vec);
        }

        dob = make_dupli(ctx, ob, mat, a);
        dob->particle_system = psys;
        psys_get_dupli_texture(psys, part, sim.psmd, pa, cpa, dob->uv, dob->orco);
      }
    }

    BLI_rng_free(rng);
    psys_sim_data_free(&sim);
  }

  /* Clean up. */
  if (oblist) {
    MEM_freeN(oblist);
  }
}

static void make_duplis_particles(const DupliContext *ctx)
{
  /* Particle system take up one level in id, the particles another. */
  int psysid;
  LISTBASE_FOREACH_INDEX (ParticleSystem *, psys, &ctx->object->particlesystem, psysid) {
    /* Particles create one more level for persistent `psys` index. */
    DupliContext pctx;
    if (copy_dupli_context(&pctx, ctx, ctx->object, nullptr, psysid)) {
      make_duplis_particle_system(&pctx, psys);
    }
  }
}

static const DupliGenerator gen_dupli_particles = {
    OB_DUPLIPARTS,        /* type */
    make_duplis_particles /* make_duplis */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dupli-Generator Selector For The Given Context
 * \{ */

static const DupliGenerator *get_dupli_generator(const DupliContext *ctx)
{
  int transflag = ctx->object->transflag;
  int visibility_flag = ctx->object->visibility_flag;

  if ((transflag & OB_DUPLI) == 0 && ctx->object->runtime.geometry_set_eval == nullptr) {
    return nullptr;
  }

  /* Metaball objects can't create instances, but the dupli system is used to "instance" their
   * evaluated mesh to render engines. We need to exit early to avoid recursively instancing the
   * evaluated metaball mesh on metaball instances that already contribute to the basis. */
  if (ctx->object->type == OB_MBALL && ctx->level > 0) {
    return nullptr;
  }

  /* Should the dupli's be generated for this object? - Respect restrict flags. */
  if (DEG_get_mode(ctx->depsgraph) == DAG_EVAL_RENDER ? (visibility_flag & OB_HIDE_RENDER) :
                                                        (visibility_flag & OB_HIDE_VIEWPORT)) {
    return nullptr;
  }

  /* Give "Object as Font" instances higher priority than geometry set instances, to retain
   * the behavior from before curve object meshes were processed as instances internally. */
  if (transflag & OB_DUPLIVERTS) {
    if (ctx->object->type == OB_FONT) {
      return &gen_dupli_verts_font;
    }
  }

  if (ctx->object->runtime.geometry_set_eval != nullptr) {
    if (BKE_object_has_geometry_set_instances(ctx->object)) {
      return &gen_dupli_geometry_set;
    }
  }

  if (transflag & OB_DUPLIPARTS) {
    return &gen_dupli_particles;
  }
  if (transflag & OB_DUPLIVERTS) {
    if (ctx->object->type == OB_MESH) {
      return &gen_dupli_verts;
    }
  }
  else if (transflag & OB_DUPLIFACES) {
    if (ctx->object->type == OB_MESH) {
      return &gen_dupli_faces;
    }
  }
  else if (transflag & OB_DUPLICOLLECTION) {
    return &gen_dupli_collection;
  }

  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dupli-Container Implementation
 * \{ */

ListBase *object_duplilist(Depsgraph *depsgraph, Scene *sce, Object *ob)
{
  ListBase *duplilist = MEM_cnew<ListBase>("duplilist");
  DupliContext ctx;
  Vector<Object *> instance_stack;
  Vector<short> dupli_gen_type_stack({0});
  instance_stack.append(ob);
  init_context(&ctx, depsgraph, sce, ob, nullptr, instance_stack, dupli_gen_type_stack);
  if (ctx.gen) {
    ctx.duplilist = duplilist;
    ctx.gen->make_duplis(&ctx);
  }

  return duplilist;
}

ListBase *object_duplilist_preview(Depsgraph *depsgraph,
                                   Scene *sce,
                                   Object *ob_eval,
                                   const ViewerPath *viewer_path)
{
  ListBase *duplilist = MEM_cnew<ListBase>("duplilist");
  DupliContext ctx;
  Vector<Object *> instance_stack;
  Vector<short> dupli_gen_type_stack({0});
  instance_stack.append(ob_eval);
  init_context(&ctx, depsgraph, sce, ob_eval, nullptr, instance_stack, dupli_gen_type_stack);
  ctx.duplilist = duplilist;

  Object *ob_orig = DEG_get_original_object(ob_eval);

  LISTBASE_FOREACH (ModifierData *, md_orig, &ob_orig->modifiers) {
    if (md_orig->type != eModifierType_Nodes) {
      continue;
    }
    NodesModifierData *nmd_orig = reinterpret_cast<NodesModifierData *>(md_orig);
    if (nmd_orig->runtime_eval_log == nullptr) {
      continue;
    }
    if (const geo_log::ViewerNodeLog *viewer_log =
            geo_log::GeoModifierLog::find_viewer_node_log_for_path(*viewer_path)) {
      ctx.preview_base_geometry = &viewer_log->geometry;
      make_duplis_geometry_set_impl(
          &ctx, viewer_log->geometry, ob_eval->object_to_world, true, ob_eval->type == OB_CURVES);
    }
  }
  return duplilist;
}

void free_object_duplilist(ListBase *lb)
{
  BLI_freelistN(lb);
  MEM_freeN(lb);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Uniform attribute lookup
 * \{ */

/** Lookup instance attributes assigned via geometry nodes. */
static bool find_geonode_attribute_rgba(const DupliObject *dupli,
                                        const char *name,
                                        float r_value[4])
{
  using namespace blender;

  /* Loop over layers from innermost to outermost. */
  for (const int i : IndexRange(sizeof(dupli->instance_data) / sizeof(void *))) {
    /* Skip non-geonode layers. */
    if (dupli->instance_data[i] == nullptr) {
      continue;
    }

    const InstancesComponent *component =
        dupli->instance_data[i]->get_component_for_read<InstancesComponent>();

    if (component == nullptr) {
      continue;
    }

    /* Attempt to look up the attribute. */
    std::optional<bke::AttributeAccessor> attributes = component->attributes();
    const VArray data = attributes->lookup<ColorGeometry4f>(name);

    /* If the attribute was found and converted to float RGBA successfully, output it. */
    if (data) {
      copy_v4_v4(r_value, data[dupli->instance_idx[i]]);
      return true;
    }
  }

  return false;
}

/** Lookup an arbitrary RNA property and convert it to RGBA if possible. */
static bool find_rna_property_rgba(PointerRNA *id_ptr, const char *name, float r_data[4])
{
  if (id_ptr->data == nullptr) {
    return false;
  }

  /* First, check custom properties. */
  IDProperty *group = RNA_struct_idprops(id_ptr, false);
  PropertyRNA *prop = nullptr;

  if (group && group->type == IDP_GROUP) {
    prop = (PropertyRNA *)IDP_GetPropertyFromGroup(group, name);
  }

  /* If not found, do full path lookup. */
  PointerRNA ptr;

  if (prop != nullptr) {
    ptr = *id_ptr;
  }
  else if (!RNA_path_resolve(id_ptr, name, &ptr, &prop)) {
    return false;
  }

  if (prop == nullptr) {
    return false;
  }

  /* Convert the value to RGBA if possible. */
  PropertyType type = RNA_property_type(prop);
  int array_len = RNA_property_array_length(&ptr, prop);

  if (array_len == 0) {
    float value;

    if (type == PROP_FLOAT) {
      value = RNA_property_float_get(&ptr, prop);
    }
    else if (type == PROP_INT) {
      value = float(RNA_property_int_get(&ptr, prop));
    }
    else if (type == PROP_BOOLEAN) {
      value = RNA_property_boolean_get(&ptr, prop) ? 1.0f : 0.0f;
    }
    else {
      return false;
    }

    copy_v4_fl4(r_data, value, value, value, 1);
    return true;
  }

  if (type == PROP_FLOAT && array_len <= 4) {
    copy_v4_fl4(r_data, 0, 0, 0, 1);
    RNA_property_float_get_array(&ptr, prop, r_data);
    return true;
  }

  if (type == PROP_INT && array_len <= 4) {
    int tmp[4] = {0, 0, 0, 1};
    RNA_property_int_get_array(&ptr, prop, tmp);
    for (int i = 0; i < 4; i++) {
      r_data[i] = float(tmp[i]);
    }
    return true;
  }

  return false;
}

static bool find_rna_property_rgba(ID *id, const char *name, float r_data[4])
{
  PointerRNA ptr;
  RNA_id_pointer_create(id, &ptr);
  return find_rna_property_rgba(&ptr, name, r_data);
}

bool BKE_object_dupli_find_rgba_attribute(
    Object *ob, DupliObject *dupli, Object *dupli_parent, const char *name, float r_value[4])
{
  /* Check the dupli particle system. */
  if (dupli && dupli->particle_system) {
    ParticleSettings *settings = dupli->particle_system->part;

    if (find_rna_property_rgba(&settings->id, name, r_value)) {
      return true;
    }
  }

  /* Check geometry node dupli instance attributes. */
  if (dupli && find_geonode_attribute_rgba(dupli, name, r_value)) {
    return true;
  }

  /* Check the dupli parent object. */
  if (dupli_parent && find_rna_property_rgba(&dupli_parent->id, name, r_value)) {
    return true;
  }

  /* Check the main object. */
  if (ob) {
    if (find_rna_property_rgba(&ob->id, name, r_value)) {
      return true;
    }

    /* Check the main object data (e.g. mesh). */
    if (ob->data && find_rna_property_rgba((ID *)ob->data, name, r_value)) {
      return true;
    }
  }

  copy_v4_fl(r_value, 0.0f);
  return false;
}

bool BKE_view_layer_find_rgba_attribute(struct Scene *scene,
                                        struct ViewLayer *layer,
                                        const char *name,
                                        float r_value[4])
{
  if (layer) {
    PointerRNA layer_ptr;
    RNA_pointer_create(&scene->id, &RNA_ViewLayer, layer, &layer_ptr);

    if (find_rna_property_rgba(&layer_ptr, name, r_value)) {
      return true;
    }
  }

  if (find_rna_property_rgba(&scene->id, name, r_value)) {
    return true;
  }

  if (scene->world && find_rna_property_rgba(&scene->world->id, name, r_value)) {
    return true;
  }

  copy_v4_fl(r_value, 0.0f);
  return false;
}

/** \} */
