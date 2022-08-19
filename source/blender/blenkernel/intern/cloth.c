/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

#include "DNA_cloth_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_edgehash.h"
#include "BLI_linklist.h"
#include "BLI_math.h"
#include "BLI_rand.h"
#include "BLI_utildefines.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "BKE_bvhutils.h"
#include "BKE_cloth.h"
#include "BKE_effect.h"
#include "BKE_global.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_modifier.h"
#include "BKE_pointcache.h"

#include "SIM_mass_spring.h"

// #include "PIL_time.h"  /* timing for debug prints */

/* ********** cloth engine ******* */
/* Prototypes for internal functions.
 */
static void cloth_to_object(Object *ob, ClothModifierData *clmd, float (*vertexCos)[3]);
static void cloth_from_mesh(ClothModifierData *clmd, const Object *ob, Mesh *mesh);
static bool cloth_from_object(
    Object *ob, ClothModifierData *clmd, Mesh *mesh, float framenr, int first);
static void cloth_update_springs(ClothModifierData *clmd);
static void cloth_update_verts(Object *ob, ClothModifierData *clmd, Mesh *mesh);
static void cloth_update_spring_lengths(ClothModifierData *clmd, Mesh *mesh);
static bool cloth_build_springs(ClothModifierData *clmd, Mesh *mesh);
static void cloth_apply_vgroup(ClothModifierData *clmd, Mesh *mesh);

typedef struct BendSpringRef {
  int index;
  int polys;
  ClothSpring *spring;
} BendSpringRef;

/******************************************************************************
 *
 * External interface called by modifier.c clothModifier functions.
 *
 ******************************************************************************/

static BVHTree *bvhtree_build_from_cloth(ClothModifierData *clmd, float epsilon)
{
  if (!clmd) {
    return NULL;
  }

  Cloth *cloth = clmd->clothObject;

  if (!cloth) {
    return NULL;
  }

  ClothVertex *verts = cloth->verts;
  const MVertTri *vt = cloth->tri;

  /* in the moment, return zero if no faces there */
  if (!cloth->primitive_num) {
    return NULL;
  }

  /* Create quad-tree with k=26. */
  BVHTree *bvhtree = BLI_bvhtree_new(cloth->primitive_num, epsilon, 4, 26);

  /* fill tree */
  if (clmd->hairdata == NULL) {
    for (int i = 0; i < cloth->primitive_num; i++, vt++) {
      float co[3][3];

      copy_v3_v3(co[0], verts[vt->tri[0]].xold);
      copy_v3_v3(co[1], verts[vt->tri[1]].xold);
      copy_v3_v3(co[2], verts[vt->tri[2]].xold);

      BLI_bvhtree_insert(bvhtree, i, co[0], 3);
    }
  }
  else {
    MEdge *edges = cloth->edges;

    for (int i = 0; i < cloth->primitive_num; i++) {
      float co[2][3];

      copy_v3_v3(co[0], verts[edges[i].v1].xold);
      copy_v3_v3(co[1], verts[edges[i].v2].xold);

      BLI_bvhtree_insert(bvhtree, i, co[0], 2);
    }
  }

  /* balance tree */
  BLI_bvhtree_balance(bvhtree);

  return bvhtree;
}

void bvhtree_update_from_cloth(ClothModifierData *clmd, bool moving, bool self)
{
  unsigned int i = 0;
  Cloth *cloth = clmd->clothObject;
  BVHTree *bvhtree;
  ClothVertex *verts = cloth->verts;
  const MVertTri *vt;

  BLI_assert(!(clmd->hairdata != NULL && self));

  if (self) {
    bvhtree = cloth->bvhselftree;
  }
  else {
    bvhtree = cloth->bvhtree;
  }

  if (!bvhtree) {
    return;
  }

  vt = cloth->tri;

  /* update vertex position in bvh tree */
  if (clmd->hairdata == NULL) {
    if (verts && vt) {
      for (i = 0; i < cloth->primitive_num; i++, vt++) {
        float co[3][3], co_moving[3][3];
        bool ret;

        /* copy new locations into array */
        if (moving) {
          copy_v3_v3(co[0], verts[vt->tri[0]].txold);
          copy_v3_v3(co[1], verts[vt->tri[1]].txold);
          copy_v3_v3(co[2], verts[vt->tri[2]].txold);

          /* update moving positions */
          copy_v3_v3(co_moving[0], verts[vt->tri[0]].tx);
          copy_v3_v3(co_moving[1], verts[vt->tri[1]].tx);
          copy_v3_v3(co_moving[2], verts[vt->tri[2]].tx);

          ret = BLI_bvhtree_update_node(bvhtree, i, co[0], co_moving[0], 3);
        }
        else {
          copy_v3_v3(co[0], verts[vt->tri[0]].tx);
          copy_v3_v3(co[1], verts[vt->tri[1]].tx);
          copy_v3_v3(co[2], verts[vt->tri[2]].tx);

          ret = BLI_bvhtree_update_node(bvhtree, i, co[0], NULL, 3);
        }

        /* check if tree is already full */
        if (ret == false) {
          break;
        }
      }

      BLI_bvhtree_update_tree(bvhtree);
    }
  }
  else {
    if (verts) {
      MEdge *edges = cloth->edges;

      for (i = 0; i < cloth->primitive_num; i++) {
        float co[2][3];

        copy_v3_v3(co[0], verts[edges[i].v1].tx);
        copy_v3_v3(co[1], verts[edges[i].v2].tx);

        if (!BLI_bvhtree_update_node(bvhtree, i, co[0], NULL, 2)) {
          break;
        }
      }

      BLI_bvhtree_update_tree(bvhtree);
    }
  }
}

void cloth_clear_cache(Object *ob, ClothModifierData *clmd, float framenr)
{
  PTCacheID pid;

  BKE_ptcache_id_from_cloth(&pid, ob, clmd);

  /* don't do anything as long as we're in editmode! */
  if (pid.cache->edit && ob->mode & OB_MODE_PARTICLE_EDIT) {
    return;
  }

  BKE_ptcache_id_clear(&pid, PTCACHE_CLEAR_AFTER, framenr);
}

static bool do_init_cloth(Object *ob, ClothModifierData *clmd, Mesh *result, int framenr)
{
  PointCache *cache;

  cache = clmd->point_cache;

  /* initialize simulation data if it didn't exist already */
  if (clmd->clothObject == NULL) {
    if (!cloth_from_object(ob, clmd, result, framenr, 1)) {
      BKE_ptcache_invalidate(cache);
      BKE_modifier_set_error(ob, &(clmd->modifier), "Can't initialize cloth");
      return false;
    }

    if (clmd->clothObject == NULL) {
      BKE_ptcache_invalidate(cache);
      BKE_modifier_set_error(ob, &(clmd->modifier), "Null cloth object");
      return false;
    }

    SIM_cloth_solver_set_positions(clmd);

    ClothSimSettings *parms = clmd->sim_parms;
    if (parms->flags & CLOTH_SIMSETTINGS_FLAG_PRESSURE &&
        !(parms->flags & CLOTH_SIMSETTINGS_FLAG_PRESSURE_VOL)) {
      SIM_cloth_solver_set_volume(clmd);
    }

    clmd->clothObject->last_frame = MINFRAME - 1;
    clmd->sim_parms->dt = 1.0f / clmd->sim_parms->stepsPerFrame;
  }

  return true;
}

static int do_step_cloth(
    Depsgraph *depsgraph, Object *ob, ClothModifierData *clmd, Mesh *result, int framenr)
{
  /* simulate 1 frame forward */
  ClothVertex *verts = NULL;
  Cloth *cloth;
  ListBase *effectors = NULL;
  MVert *mvert;
  unsigned int i = 0;
  int ret = 0;
  bool vert_mass_changed = false;

  cloth = clmd->clothObject;
  verts = cloth->verts;
  mvert = result->mvert;
  vert_mass_changed = verts->mass != clmd->sim_parms->mass;

  /* force any pinned verts to their constrained location. */
  for (i = 0; i < clmd->clothObject->mvert_num; i++, verts++) {
    /* save the previous position. */
    copy_v3_v3(verts->xold, verts->xconst);
    copy_v3_v3(verts->txold, verts->x);

    /* Get the current position. */
    copy_v3_v3(verts->xconst, mvert[i].co);
    mul_m4_v3(ob->obmat, verts->xconst);

    if (vert_mass_changed) {
      verts->mass = clmd->sim_parms->mass;
      SIM_mass_spring_set_implicit_vertex_mass(cloth->implicit, i, verts->mass);
    }
  }

  effectors = BKE_effectors_create(depsgraph, ob, NULL, clmd->sim_parms->effector_weights, false);

  if (clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_DYNAMIC_BASEMESH) {
    cloth_update_verts(ob, clmd, result);
  }

  /* Support for dynamic vertex groups, changing from frame to frame */
  cloth_apply_vgroup(clmd, result);

  if ((clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_DYNAMIC_BASEMESH) ||
      (clmd->sim_parms->vgroup_shrink > 0) || (clmd->sim_parms->shrink_min != 0.0f)) {
    cloth_update_spring_lengths(clmd, result);
  }

  cloth_update_springs(clmd);

  // TIMEIT_START(cloth_step)

  /* call the solver. */
  ret = SIM_cloth_solve(depsgraph, ob, framenr, clmd, effectors);

  // TIMEIT_END(cloth_step)

  BKE_effectors_free(effectors);

  // printf ( "%f\n", ( float ) tval() );

  return ret;
}

/************************************************
 * clothModifier_do - main simulation function
 ************************************************/

void clothModifier_do(ClothModifierData *clmd,
                      Depsgraph *depsgraph,
                      Scene *scene,
                      Object *ob,
                      Mesh *mesh,
                      float (*vertexCos)[3])
{
  PointCache *cache;
  PTCacheID pid;
  float timescale;
  int framenr, startframe, endframe;
  int cache_result;

  framenr = DEG_get_ctime(depsgraph);
  cache = clmd->point_cache;

  BKE_ptcache_id_from_cloth(&pid, ob, clmd);
  BKE_ptcache_id_time(&pid, scene, framenr, &startframe, &endframe, &timescale);
  clmd->sim_parms->timescale = timescale * clmd->sim_parms->time_scale;

  if (clmd->sim_parms->reset ||
      (clmd->clothObject && mesh->totvert != clmd->clothObject->mvert_num)) {
    clmd->sim_parms->reset = 0;
    cache->flag |= PTCACHE_OUTDATED;
    BKE_ptcache_id_reset(scene, &pid, PTCACHE_RESET_OUTDATED);
    BKE_ptcache_validate(cache, 0);
    cache->last_exact = 0;
    cache->flag &= ~PTCACHE_REDO_NEEDED;
  }

  /* simulation is only active during a specific period */
  if (framenr < startframe) {
    BKE_ptcache_invalidate(cache);
    return;
  }
  if (framenr > endframe) {
    framenr = endframe;
  }

  /* initialize simulation data if it didn't exist already */
  if (!do_init_cloth(ob, clmd, mesh, framenr)) {
    return;
  }

  if (framenr == startframe) {
    BKE_ptcache_id_reset(scene, &pid, PTCACHE_RESET_OUTDATED);
    do_init_cloth(ob, clmd, mesh, framenr);
    BKE_ptcache_validate(cache, framenr);
    cache->flag &= ~PTCACHE_REDO_NEEDED;
    clmd->clothObject->last_frame = framenr;
    return;
  }

  /* try to read from cache */
  bool can_simulate = (framenr == clmd->clothObject->last_frame + 1) &&
                      !(cache->flag & PTCACHE_BAKED);

  cache_result = BKE_ptcache_read(&pid, (float)framenr + scene->r.subframe, can_simulate);

  if (cache_result == PTCACHE_READ_EXACT || cache_result == PTCACHE_READ_INTERPOLATED ||
      (!can_simulate && cache_result == PTCACHE_READ_OLD)) {
    SIM_cloth_solver_set_positions(clmd);
    cloth_to_object(ob, clmd, vertexCos);

    BKE_ptcache_validate(cache, framenr);

    if (cache_result == PTCACHE_READ_INTERPOLATED && cache->flag & PTCACHE_REDO_NEEDED) {
      BKE_ptcache_write(&pid, framenr);
    }

    clmd->clothObject->last_frame = framenr;

    return;
  }
  if (cache_result == PTCACHE_READ_OLD) {
    SIM_cloth_solver_set_positions(clmd);
  }
  else if (
      /* 2.4x disabled lib, but this can be used in some cases, testing further - campbell */
      /*ob->id.lib ||*/ (cache->flag & PTCACHE_BAKED)) {
    /* if baked and nothing in cache, do nothing */
    BKE_ptcache_invalidate(cache);
    return;
  }

  /* if on second frame, write cache for first frame */
  if (cache->simframe == startframe &&
      (cache->flag & PTCACHE_OUTDATED || cache->last_exact == 0)) {
    BKE_ptcache_write(&pid, startframe);
  }

  clmd->sim_parms->timescale *= framenr - cache->simframe;

  /* do simulation */
  BKE_ptcache_validate(cache, framenr);

  if (!do_step_cloth(depsgraph, ob, clmd, mesh, framenr)) {
    BKE_ptcache_invalidate(cache);
  }
  else {
    BKE_ptcache_write(&pid, framenr);
  }

  cloth_to_object(ob, clmd, vertexCos);
  clmd->clothObject->last_frame = framenr;
}

void cloth_free_modifier(ClothModifierData *clmd)
{
  Cloth *cloth = NULL;

  if (!clmd) {
    return;
  }

  cloth = clmd->clothObject;

  if (cloth) {
    SIM_cloth_solver_free(clmd);

    /* Free the verts. */
    MEM_SAFE_FREE(cloth->verts);
    cloth->mvert_num = 0;

    /* Free the springs. */
    if (cloth->springs != NULL) {
      LinkNode *search = cloth->springs;
      while (search) {
        ClothSpring *spring = search->link;

        MEM_SAFE_FREE(spring->pa);
        MEM_SAFE_FREE(spring->pb);

        MEM_freeN(spring);
        search = search->next;
      }
      BLI_linklist_free(cloth->springs, NULL);

      cloth->springs = NULL;
    }

    cloth->springs = NULL;
    cloth->numsprings = 0;

    /* free BVH collision tree */
    if (cloth->bvhtree) {
      BLI_bvhtree_free(cloth->bvhtree);
    }

    if (cloth->bvhselftree) {
      BLI_bvhtree_free(cloth->bvhselftree);
    }

    /* we save our faces for collision objects */
    if (cloth->tri) {
      MEM_freeN(cloth->tri);
    }

    if (cloth->edgeset) {
      BLI_edgeset_free(cloth->edgeset);
    }

    if (cloth->sew_edge_graph) {
      BLI_edgeset_free(cloth->sew_edge_graph);
      cloth->sew_edge_graph = NULL;
    }

#if 0
    if (clmd->clothObject->facemarks) {
      MEM_freeN(clmd->clothObject->facemarks);
    }
#endif
    MEM_freeN(cloth);
    clmd->clothObject = NULL;
  }
}

void cloth_free_modifier_extern(ClothModifierData *clmd)
{
  Cloth *cloth = NULL;
  if (G.debug & G_DEBUG_SIMDATA) {
    printf("cloth_free_modifier_extern\n");
  }

  if (!clmd) {
    return;
  }

  cloth = clmd->clothObject;

  if (cloth) {
    if (G.debug & G_DEBUG_SIMDATA) {
      printf("cloth_free_modifier_extern in\n");
    }

    SIM_cloth_solver_free(clmd);

    /* Free the verts. */
    MEM_SAFE_FREE(cloth->verts);
    cloth->mvert_num = 0;

    /* Free the springs. */
    if (cloth->springs != NULL) {
      LinkNode *search = cloth->springs;
      while (search) {
        ClothSpring *spring = search->link;

        MEM_SAFE_FREE(spring->pa);
        MEM_SAFE_FREE(spring->pb);

        MEM_freeN(spring);
        search = search->next;
      }
      BLI_linklist_free(cloth->springs, NULL);

      cloth->springs = NULL;
    }

    cloth->springs = NULL;
    cloth->numsprings = 0;

    /* free BVH collision tree */
    if (cloth->bvhtree) {
      BLI_bvhtree_free(cloth->bvhtree);
    }

    if (cloth->bvhselftree) {
      BLI_bvhtree_free(cloth->bvhselftree);
    }

    /* we save our faces for collision objects */
    if (cloth->tri) {
      MEM_freeN(cloth->tri);
    }

    if (cloth->edgeset) {
      BLI_edgeset_free(cloth->edgeset);
    }

    if (cloth->sew_edge_graph) {
      BLI_edgeset_free(cloth->sew_edge_graph);
      cloth->sew_edge_graph = NULL;
    }

#if 0
    if (clmd->clothObject->facemarks) {
      MEM_freeN(clmd->clothObject->facemarks);
    }
#endif
    MEM_freeN(cloth);
    clmd->clothObject = NULL;
  }
}

/******************************************************************************
 *
 * Internal functions.
 *
 ******************************************************************************/

/**
 * Copies the deformed vertices to the object.
 */
static void cloth_to_object(Object *ob, ClothModifierData *clmd, float (*vertexCos)[3])
{
  unsigned int i = 0;
  Cloth *cloth = clmd->clothObject;

  if (clmd->clothObject) {
    /* Inverse matrix is not up to date. */
    invert_m4_m4(ob->imat, ob->obmat);

    for (i = 0; i < cloth->mvert_num; i++) {
      copy_v3_v3(vertexCos[i], cloth->verts[i].x);
      mul_m4_v3(ob->imat, vertexCos[i]); /* cloth is in global coords */
    }
  }
}

int cloth_uses_vgroup(ClothModifierData *clmd)
{
  return (((clmd->coll_parms->flags & CLOTH_COLLSETTINGS_FLAG_SELF) &&
           (clmd->coll_parms->vgroup_selfcol > 0)) ||
          ((clmd->coll_parms->flags & CLOTH_COLLSETTINGS_FLAG_ENABLED) &&
           (clmd->coll_parms->vgroup_objcol > 0)) ||
          (clmd->sim_parms->vgroup_pressure > 0) || (clmd->sim_parms->vgroup_struct > 0) ||
          (clmd->sim_parms->vgroup_bend > 0) || (clmd->sim_parms->vgroup_shrink > 0) ||
          (clmd->sim_parms->vgroup_intern > 0) || (clmd->sim_parms->vgroup_mass > 0));
}

/**
 * Applies a vertex group as specified by type.
 */
static void cloth_apply_vgroup(ClothModifierData *clmd, Mesh *mesh)
{
  if (!clmd || !mesh) {
    return;
  }

  int mvert_num = mesh->totvert;

  ClothVertex *verts = clmd->clothObject->verts;

  if (cloth_uses_vgroup(clmd)) {
    for (int i = 0; i < mvert_num; i++, verts++) {

      /* Reset Goal values to standard */
      if (clmd->sim_parms->vgroup_mass > 0) {
        verts->goal = clmd->sim_parms->defgoal;
      }
      else {
        verts->goal = 0.0f;
      }

      /* Compute base cloth shrink weight */
      verts->shrink_factor = 0.0f;

      /* Reset vertex flags */
      verts->flags &= ~(CLOTH_VERT_FLAG_PINNED | CLOTH_VERT_FLAG_NOSELFCOLL |
                        CLOTH_VERT_FLAG_NOOBJCOLL);

      const MDeformVert *dvert = CustomData_get(&mesh->vdata, i, CD_MDEFORMVERT);
      if (dvert) {
        for (int j = 0; j < dvert->totweight; j++) {
          if (dvert->dw[j].def_nr == (clmd->sim_parms->vgroup_mass - 1)) {
            verts->goal = dvert->dw[j].weight;

            /* goalfac= 1.0f; */ /* UNUSED */

            /* Kicking goal factor to simplify things...who uses that anyway? */
            // ABS (clmd->sim_parms->maxgoal - clmd->sim_parms->mingoal);

            verts->goal = pow4f(verts->goal);
            if (verts->goal >= SOFTGOALSNAP) {
              verts->flags |= CLOTH_VERT_FLAG_PINNED;
            }
          }

          if (dvert->dw[j].def_nr == (clmd->sim_parms->vgroup_struct - 1)) {
            verts->struct_stiff = dvert->dw[j].weight;
          }

          if (dvert->dw[j].def_nr == (clmd->sim_parms->vgroup_shear - 1)) {
            verts->shear_stiff = dvert->dw[j].weight;
          }

          if (dvert->dw[j].def_nr == (clmd->sim_parms->vgroup_bend - 1)) {
            verts->bend_stiff = dvert->dw[j].weight;
          }

          if (dvert->dw[j].def_nr == (clmd->coll_parms->vgroup_selfcol - 1)) {
            if (dvert->dw[j].weight > 0.0f) {
              verts->flags |= CLOTH_VERT_FLAG_NOSELFCOLL;
            }
          }

          if (dvert->dw[j].def_nr == (clmd->coll_parms->vgroup_objcol - 1)) {
            if (dvert->dw[j].weight > 0.0f) {
              verts->flags |= CLOTH_VERT_FLAG_NOOBJCOLL;
            }
          }

          if (dvert->dw[j].def_nr == (clmd->sim_parms->vgroup_shrink - 1)) {
            /* Used for linear interpolation between min and max
             * shrink factor based on weight. */
            verts->shrink_factor = dvert->dw[j].weight;
          }

          if (dvert->dw[j].def_nr == (clmd->sim_parms->vgroup_intern - 1)) {
            /* Used to define the stiffness weight on the internal spring connected to this vertex.
             */
            verts->internal_stiff = dvert->dw[j].weight;
          }

          if (dvert->dw[j].def_nr == (clmd->sim_parms->vgroup_pressure - 1)) {
            /* Used to define how much the pressure settings should affect the given vertex. */
            verts->pressure_factor = dvert->dw[j].weight;
          }
        }
      }
    }
  }
}

static float cloth_shrink_factor(ClothModifierData *clmd, ClothVertex *verts, int i1, int i2)
{
  /* Linear interpolation between min and max shrink factor based on weight. */
  float base = 1.0f - clmd->sim_parms->shrink_min;
  float shrink_factor_delta = clmd->sim_parms->shrink_min - clmd->sim_parms->shrink_max;

  float k1 = base + shrink_factor_delta * verts[i1].shrink_factor;
  float k2 = base + shrink_factor_delta * verts[i2].shrink_factor;

  /* Use geometrical mean to average two factors since it behaves better
   * for diagonals when a rectangle transforms into a trapezoid. */
  return sqrtf(k1 * k2);
}

static bool cloth_from_object(
    Object *ob, ClothModifierData *clmd, Mesh *mesh, float UNUSED(framenr), int first)
{
  int i = 0;
  MVert *mvert = NULL;
  ClothVertex *verts = NULL;
  const float(*shapekey_rest)[3] = NULL;
  const float tnull[3] = {0, 0, 0};

  /* If we have a clothObject, free it. */
  if (clmd->clothObject != NULL) {
    cloth_free_modifier(clmd);
    if (G.debug & G_DEBUG_SIMDATA) {
      printf("cloth_free_modifier cloth_from_object\n");
    }
  }

  /* Allocate a new cloth object. */
  clmd->clothObject = MEM_callocN(sizeof(Cloth), "cloth");
  if (clmd->clothObject) {
    clmd->clothObject->old_solver_type = 255;
    clmd->clothObject->edgeset = NULL;
  }
  else {
    BKE_modifier_set_error(ob, &(clmd->modifier), "Out of memory on allocating clmd->clothObject");
    return false;
  }

  /* mesh input objects need Mesh */
  if (!mesh) {
    return false;
  }

  cloth_from_mesh(clmd, ob, mesh);

  /* create springs */
  clmd->clothObject->springs = NULL;
  clmd->clothObject->numsprings = -1;

  clmd->clothObject->sew_edge_graph = NULL;

  if (clmd->sim_parms->shapekey_rest &&
      !(clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_DYNAMIC_BASEMESH)) {
    shapekey_rest = CustomData_get_layer(&mesh->vdata, CD_CLOTH_ORCO);
  }

  mvert = mesh->mvert;

  verts = clmd->clothObject->verts;

  /* set initial values */
  for (i = 0; i < mesh->totvert; i++, verts++) {
    if (first) {
      copy_v3_v3(verts->x, mvert[i].co);

      mul_m4_v3(ob->obmat, verts->x);

      if (shapekey_rest) {
        copy_v3_v3(verts->xrest, shapekey_rest[i]);
        mul_m4_v3(ob->obmat, verts->xrest);
      }
      else {
        copy_v3_v3(verts->xrest, verts->x);
      }
    }

    /* no GUI interface yet */
    verts->mass = clmd->sim_parms->mass;
    verts->impulse_count = 0;

    if (clmd->sim_parms->vgroup_mass > 0) {
      verts->goal = clmd->sim_parms->defgoal;
    }
    else {
      verts->goal = 0.0f;
    }

    verts->shrink_factor = 0.0f;

    verts->flags = 0;
    copy_v3_v3(verts->xold, verts->x);
    copy_v3_v3(verts->xconst, verts->x);
    copy_v3_v3(verts->txold, verts->x);
    copy_v3_v3(verts->tx, verts->x);
    mul_v3_fl(verts->v, 0.0f);

    verts->impulse_count = 0;
    copy_v3_v3(verts->impulse, tnull);
  }

  /* apply / set vertex groups */
  /* has to be happen before springs are build! */
  cloth_apply_vgroup(clmd, mesh);

  if (!cloth_build_springs(clmd, mesh)) {
    cloth_free_modifier(clmd);
    BKE_modifier_set_error(ob, &(clmd->modifier), "Cannot build springs");
    return false;
  }

  /* init our solver */
  SIM_cloth_solver_init(ob, clmd);

  if (!first) {
    SIM_cloth_solver_set_positions(clmd);
  }

  clmd->clothObject->bvhtree = bvhtree_build_from_cloth(clmd, clmd->coll_parms->epsilon);
  clmd->clothObject->bvhselftree = bvhtree_build_from_cloth(clmd, clmd->coll_parms->selfepsilon);

  return true;
}

static void cloth_from_mesh(ClothModifierData *clmd, const Object *ob, Mesh *mesh)
{
  const MLoop *mloop = mesh->mloop;
  const MLoopTri *looptri = BKE_mesh_runtime_looptri_ensure(mesh);
  const unsigned int mvert_num = mesh->totvert;
  const unsigned int looptri_num = mesh->runtime.looptris.len;

  /* Allocate our vertices. */
  clmd->clothObject->mvert_num = mvert_num;
  clmd->clothObject->verts = MEM_callocN(sizeof(ClothVertex) * clmd->clothObject->mvert_num,
                                         "clothVertex");
  if (clmd->clothObject->verts == NULL) {
    cloth_free_modifier(clmd);
    BKE_modifier_set_error(
        ob, &(clmd->modifier), "Out of memory on allocating clmd->clothObject->verts");
    printf("cloth_free_modifier clmd->clothObject->verts\n");
    return;
  }

  /* save face information */
  if (clmd->hairdata == NULL) {
    clmd->clothObject->primitive_num = looptri_num;
  }
  else {
    clmd->clothObject->primitive_num = mesh->totedge;
  }

  clmd->clothObject->tri = MEM_mallocN(sizeof(MVertTri) * looptri_num, "clothLoopTris");
  if (clmd->clothObject->tri == NULL) {
    cloth_free_modifier(clmd);
    BKE_modifier_set_error(
        ob, &(clmd->modifier), "Out of memory on allocating clmd->clothObject->looptri");
    printf("cloth_free_modifier clmd->clothObject->looptri\n");
    return;
  }
  BKE_mesh_runtime_verttri_from_looptri(clmd->clothObject->tri, mloop, looptri, looptri_num);

  clmd->clothObject->edges = mesh->medge;

  /* Free the springs since they can't be correct if the vertices
   * changed.
   */
  if (clmd->clothObject->springs != NULL) {
    MEM_freeN(clmd->clothObject->springs);
  }
}

/* -------------------------------------------------------------------- */
/** \name Spring Network Building Implementation
 * \{ */

BLI_INLINE void spring_verts_ordered_set(ClothSpring *spring, int v0, int v1)
{
  if (v0 < v1) {
    spring->ij = v0;
    spring->kl = v1;
  }
  else {
    spring->ij = v1;
    spring->kl = v0;
  }
}

static void cloth_free_edgelist(LinkNodePair *edgelist, unsigned int mvert_num)
{
  if (edgelist) {
    for (uint i = 0; i < mvert_num; i++) {
      BLI_linklist_free(edgelist[i].list, NULL);
    }

    MEM_freeN(edgelist);
  }
}

static void cloth_free_errorsprings(Cloth *cloth,
                                    LinkNodePair *edgelist,
                                    BendSpringRef *spring_ref)
{
  if (cloth->springs != NULL) {
    LinkNode *search = cloth->springs;
    while (search) {
      ClothSpring *spring = search->link;

      MEM_SAFE_FREE(spring->pa);
      MEM_SAFE_FREE(spring->pb);

      MEM_freeN(spring);
      search = search->next;
    }
    BLI_linklist_free(cloth->springs, NULL);

    cloth->springs = NULL;
  }

  cloth_free_edgelist(edgelist, cloth->mvert_num);

  MEM_SAFE_FREE(spring_ref);

  if (cloth->edgeset) {
    BLI_edgeset_free(cloth->edgeset);
    cloth->edgeset = NULL;
  }
}

BLI_INLINE void cloth_bend_poly_dir(
    ClothVertex *verts, int i, int j, const int *inds, int len, float r_dir[3])
{
  float cent[3] = {0};
  float fact = 1.0f / len;

  for (int x = 0; x < len; x++) {
    madd_v3_v3fl(cent, verts[inds[x]].xrest, fact);
  }

  normal_tri_v3(r_dir, verts[i].xrest, verts[j].xrest, cent);
}

static float cloth_spring_angle(
    ClothVertex *verts, int i, int j, int *i_a, int *i_b, int len_a, int len_b)
{
  float dir_a[3], dir_b[3];
  float tmp[3], vec_e[3];
  float sin, cos;

  /* Poly vectors. */
  cloth_bend_poly_dir(verts, j, i, i_a, len_a, dir_a);
  cloth_bend_poly_dir(verts, i, j, i_b, len_b, dir_b);

  /* Edge vector. */
  sub_v3_v3v3(vec_e, verts[i].xrest, verts[j].xrest);
  normalize_v3(vec_e);

  /* Compute angle. */
  cos = dot_v3v3(dir_a, dir_b);

  cross_v3_v3v3(tmp, dir_a, dir_b);
  sin = dot_v3v3(tmp, vec_e);

  return atan2f(sin, cos);
}

static void cloth_hair_update_bending_targets(ClothModifierData *clmd)
{
  Cloth *cloth = clmd->clothObject;
  LinkNode *search = NULL;
  float hair_frame[3][3], dir_old[3], dir_new[3];
  int prev_mn; /* to find hair chains */

  if (!clmd->hairdata) {
    return;
  }

  /* XXX NOTE: we need to propagate frames from the root up,
   * but structural hair springs are stored in reverse order.
   * The bending springs however are then inserted in the same
   * order as vertices again ...
   * This messy situation can be resolved when solver data is
   * generated directly from a dedicated hair system.
   */

  prev_mn = -1;
  for (search = cloth->springs; search; search = search->next) {
    ClothSpring *spring = search->link;
    ClothHairData *hair_ij, *hair_kl;
    bool is_root = spring->kl != prev_mn;

    if (spring->type != CLOTH_SPRING_TYPE_BENDING_HAIR) {
      continue;
    }

    hair_ij = &clmd->hairdata[spring->ij];
    hair_kl = &clmd->hairdata[spring->kl];
    if (is_root) {
      /* initial hair frame from root orientation */
      copy_m3_m3(hair_frame, hair_ij->rot);
      /* surface normal is the initial direction,
       * parallel transport then keeps it aligned to the hair direction
       */
      copy_v3_v3(dir_new, hair_frame[2]);
    }

    copy_v3_v3(dir_old, dir_new);
    sub_v3_v3v3(dir_new, cloth->verts[spring->mn].x, cloth->verts[spring->kl].x);
    normalize_v3(dir_new);

    /* get local targets for kl/mn vertices by putting rest targets into the current frame,
     * then multiply with the rest length to get the actual goals
     */

    mul_v3_m3v3(spring->target, hair_frame, hair_kl->rest_target);
    mul_v3_fl(spring->target, spring->restlen);

    /* move frame to next hair segment */
    cloth_parallel_transport_hair_frame(hair_frame, dir_old, dir_new);

    prev_mn = spring->mn;
  }
}

static void cloth_hair_update_bending_rest_targets(ClothModifierData *clmd)
{
  Cloth *cloth = clmd->clothObject;
  LinkNode *search = NULL;
  float hair_frame[3][3], dir_old[3], dir_new[3];
  int prev_mn; /* to find hair roots */

  if (!clmd->hairdata) {
    return;
  }

  /* XXX NOTE: we need to propagate frames from the root up,
   * but structural hair springs are stored in reverse order.
   * The bending springs however are then inserted in the same
   * order as vertices again ...
   * This messy situation can be resolved when solver data is
   * generated directly from a dedicated hair system.
   */

  prev_mn = -1;
  for (search = cloth->springs; search; search = search->next) {
    ClothSpring *spring = search->link;
    ClothHairData *hair_ij, *hair_kl;
    bool is_root = spring->kl != prev_mn;

    if (spring->type != CLOTH_SPRING_TYPE_BENDING_HAIR) {
      continue;
    }

    hair_ij = &clmd->hairdata[spring->ij];
    hair_kl = &clmd->hairdata[spring->kl];
    if (is_root) {
      /* initial hair frame from root orientation */
      copy_m3_m3(hair_frame, hair_ij->rot);
      /* surface normal is the initial direction,
       * parallel transport then keeps it aligned to the hair direction
       */
      copy_v3_v3(dir_new, hair_frame[2]);
    }

    copy_v3_v3(dir_old, dir_new);
    sub_v3_v3v3(dir_new, cloth->verts[spring->mn].xrest, cloth->verts[spring->kl].xrest);
    normalize_v3(dir_new);

    /* dir expressed in the hair frame defines the rest target direction */
    copy_v3_v3(hair_kl->rest_target, dir_new);
    mul_transposed_m3_v3(hair_frame, hair_kl->rest_target);

    /* move frame to next hair segment */
    cloth_parallel_transport_hair_frame(hair_frame, dir_old, dir_new);

    prev_mn = spring->mn;
  }
}

/* update stiffness if vertex group values are changing from frame to frame */
static void cloth_update_springs(ClothModifierData *clmd)
{
  Cloth *cloth = clmd->clothObject;
  LinkNode *search = NULL;

  search = cloth->springs;
  while (search) {
    ClothSpring *spring = search->link;

    spring->lin_stiffness = 0.0f;

    if (clmd->sim_parms->bending_model == CLOTH_BENDING_ANGULAR) {
      if (spring->type & CLOTH_SPRING_TYPE_BENDING) {
        spring->ang_stiffness = (cloth->verts[spring->kl].bend_stiff +
                                 cloth->verts[spring->ij].bend_stiff) /
                                2.0f;
      }
    }

    if (spring->type & CLOTH_SPRING_TYPE_STRUCTURAL) {
      spring->lin_stiffness = (cloth->verts[spring->kl].struct_stiff +
                               cloth->verts[spring->ij].struct_stiff) /
                              2.0f;
    }
    else if (spring->type & CLOTH_SPRING_TYPE_SHEAR) {
      spring->lin_stiffness = (cloth->verts[spring->kl].shear_stiff +
                               cloth->verts[spring->ij].shear_stiff) /
                              2.0f;
    }
    else if (spring->type == CLOTH_SPRING_TYPE_BENDING) {
      spring->lin_stiffness = (cloth->verts[spring->kl].bend_stiff +
                               cloth->verts[spring->ij].bend_stiff) /
                              2.0f;
    }
    else if (spring->type & CLOTH_SPRING_TYPE_INTERNAL) {
      spring->lin_stiffness = (cloth->verts[spring->kl].internal_stiff +
                               cloth->verts[spring->ij].internal_stiff) /
                              2.0f;
    }
    else if (spring->type == CLOTH_SPRING_TYPE_BENDING_HAIR) {
      ClothVertex *v1 = &cloth->verts[spring->ij];
      ClothVertex *v2 = &cloth->verts[spring->kl];
      if (clmd->hairdata) {
        /* copy extra hair data to generic cloth vertices */
        v1->bend_stiff = clmd->hairdata[spring->ij].bending_stiffness;
        v2->bend_stiff = clmd->hairdata[spring->kl].bending_stiffness;
      }
      spring->lin_stiffness = (v1->bend_stiff + v2->bend_stiff) / 2.0f;
    }
    else if (spring->type == CLOTH_SPRING_TYPE_GOAL) {
      /* WARNING: Appending NEW goal springs does not work
       * because implicit solver would need reset! */

      /* Activate / Deactivate existing springs */
      if ((!(cloth->verts[spring->ij].flags & CLOTH_VERT_FLAG_PINNED)) &&
          (cloth->verts[spring->ij].goal > ALMOST_ZERO)) {
        spring->flags &= ~CLOTH_SPRING_FLAG_DEACTIVATE;
      }
      else {
        spring->flags |= CLOTH_SPRING_FLAG_DEACTIVATE;
      }
    }

    search = search->next;
  }

  cloth_hair_update_bending_targets(clmd);
}

/* Update rest verts, for dynamically deformable cloth */
static void cloth_update_verts(Object *ob, ClothModifierData *clmd, Mesh *mesh)
{
  unsigned int i = 0;
  MVert *mvert = mesh->mvert;
  ClothVertex *verts = clmd->clothObject->verts;

  /* vertex count is already ensured to match */
  for (i = 0; i < mesh->totvert; i++, verts++) {
    copy_v3_v3(verts->xrest, mvert[i].co);
    mul_m4_v3(ob->obmat, verts->xrest);
  }
}

/* Write rest vert locations to a copy of the mesh. */
static Mesh *cloth_make_rest_mesh(ClothModifierData *clmd, Mesh *mesh)
{
  Mesh *new_mesh = BKE_mesh_copy_for_eval(mesh, false);
  ClothVertex *verts = clmd->clothObject->verts;
  MVert *mvert = new_mesh->mvert;

  /* vertex count is already ensured to match */
  for (unsigned i = 0; i < mesh->totvert; i++, verts++) {
    copy_v3_v3(mvert[i].co, verts->xrest);
  }
  BKE_mesh_tag_coords_changed(new_mesh);

  return new_mesh;
}

/* Update spring rest length, for dynamically deformable cloth */
static void cloth_update_spring_lengths(ClothModifierData *clmd, Mesh *mesh)
{
  Cloth *cloth = clmd->clothObject;
  LinkNode *search = cloth->springs;
  unsigned int struct_springs = 0;
  unsigned int i = 0;
  unsigned int mvert_num = (unsigned int)mesh->totvert;
  float shrink_factor;

  clmd->sim_parms->avg_spring_len = 0.0f;

  for (i = 0; i < mvert_num; i++) {
    cloth->verts[i].avg_spring_len = 0.0f;
  }

  while (search) {
    ClothSpring *spring = search->link;

    if (spring->type != CLOTH_SPRING_TYPE_SEWING) {
      if (spring->type & (CLOTH_SPRING_TYPE_STRUCTURAL | CLOTH_SPRING_TYPE_SHEAR |
                          CLOTH_SPRING_TYPE_BENDING | CLOTH_SPRING_TYPE_INTERNAL)) {
        shrink_factor = cloth_shrink_factor(clmd, cloth->verts, spring->ij, spring->kl);
      }
      else {
        shrink_factor = 1.0f;
      }

      spring->restlen = len_v3v3(cloth->verts[spring->kl].xrest, cloth->verts[spring->ij].xrest) *
                        shrink_factor;

      if (spring->type & CLOTH_SPRING_TYPE_BENDING) {
        spring->restang = cloth_spring_angle(
            cloth->verts, spring->ij, spring->kl, spring->pa, spring->pb, spring->la, spring->lb);
      }
    }

    if (spring->type & CLOTH_SPRING_TYPE_STRUCTURAL) {
      clmd->sim_parms->avg_spring_len += spring->restlen;
      cloth->verts[spring->ij].avg_spring_len += spring->restlen;
      cloth->verts[spring->kl].avg_spring_len += spring->restlen;
      struct_springs++;
    }

    search = search->next;
  }

  if (struct_springs > 0) {
    clmd->sim_parms->avg_spring_len /= struct_springs;
  }

  for (i = 0; i < mvert_num; i++) {
    if (cloth->verts[i].spring_count > 0) {
      cloth->verts[i].avg_spring_len = cloth->verts[i].avg_spring_len * 0.49f /
                                       ((float)cloth->verts[i].spring_count);
    }
  }
}

BLI_INLINE void cross_identity_v3(float r[3][3], const float v[3])
{
  zero_m3(r);
  r[0][1] = v[2];
  r[0][2] = -v[1];
  r[1][0] = -v[2];
  r[1][2] = v[0];
  r[2][0] = v[1];
  r[2][1] = -v[0];
}

BLI_INLINE void madd_m3_m3fl(float r[3][3], const float m[3][3], float f)
{
  r[0][0] += m[0][0] * f;
  r[0][1] += m[0][1] * f;
  r[0][2] += m[0][2] * f;
  r[1][0] += m[1][0] * f;
  r[1][1] += m[1][1] * f;
  r[1][2] += m[1][2] * f;
  r[2][0] += m[2][0] * f;
  r[2][1] += m[2][1] * f;
  r[2][2] += m[2][2] * f;
}

void cloth_parallel_transport_hair_frame(float mat[3][3],
                                         const float dir_old[3],
                                         const float dir_new[3])
{
  float rot[3][3];

  /* rotation between segments */
  rotation_between_vecs_to_mat3(rot, dir_old, dir_new);

  /* rotate the frame */
  mul_m3_m3m3(mat, rot, mat);
}

/* Add a shear and a bend spring between two verts within a poly. */
static bool cloth_add_shear_bend_spring(ClothModifierData *clmd,
                                        LinkNodePair *edgelist,
                                        const MLoop *mloop,
                                        const MPoly *mpoly,
                                        int i,
                                        int j,
                                        int k)
{
  Cloth *cloth = clmd->clothObject;
  ClothSpring *spring;
  const MLoop *tmp_loop;
  float shrink_factor;
  int x, y;

  /* Combined shear/bend properties. */
  spring = (ClothSpring *)MEM_callocN(sizeof(ClothSpring), "cloth spring");

  if (!spring) {
    return false;
  }

  spring_verts_ordered_set(
      spring, mloop[mpoly[i].loopstart + j].v, mloop[mpoly[i].loopstart + k].v);

  shrink_factor = cloth_shrink_factor(clmd, cloth->verts, spring->ij, spring->kl);
  spring->restlen = len_v3v3(cloth->verts[spring->kl].xrest, cloth->verts[spring->ij].xrest) *
                    shrink_factor;
  spring->type |= CLOTH_SPRING_TYPE_SHEAR;
  spring->lin_stiffness = (cloth->verts[spring->kl].shear_stiff +
                           cloth->verts[spring->ij].shear_stiff) /
                          2.0f;

  if (edgelist) {
    BLI_linklist_append(&edgelist[spring->ij], spring);
    BLI_linklist_append(&edgelist[spring->kl], spring);
  }

  /* Bending specific properties. */
  if (clmd->sim_parms->bending_model == CLOTH_BENDING_ANGULAR) {
    spring->type |= CLOTH_SPRING_TYPE_BENDING;

    spring->la = k - j + 1;
    spring->lb = mpoly[i].totloop - k + j + 1;

    spring->pa = MEM_mallocN(sizeof(*spring->pa) * spring->la, "spring poly");
    if (!spring->pa) {
      return false;
    }

    spring->pb = MEM_mallocN(sizeof(*spring->pb) * spring->lb, "spring poly");
    if (!spring->pb) {
      return false;
    }

    tmp_loop = mloop + mpoly[i].loopstart;

    for (x = 0; x < spring->la; x++) {
      spring->pa[x] = tmp_loop[j + x].v;
    }

    for (x = 0; x <= j; x++) {
      spring->pb[x] = tmp_loop[x].v;
    }

    for (y = k; y < mpoly[i].totloop; x++, y++) {
      spring->pb[x] = tmp_loop[y].v;
    }

    spring->mn = -1;

    spring->restang = cloth_spring_angle(
        cloth->verts, spring->ij, spring->kl, spring->pa, spring->pb, spring->la, spring->lb);

    spring->ang_stiffness = (cloth->verts[spring->ij].bend_stiff +
                             cloth->verts[spring->kl].bend_stiff) /
                            2.0f;
  }

  BLI_linklist_prepend(&cloth->springs, spring);

  return true;
}

BLI_INLINE bool cloth_bend_set_poly_vert_array(int **poly, int len, const MLoop *mloop)
{
  int *p = MEM_mallocN(sizeof(int) * len, "spring poly");

  if (!p) {
    return false;
  }

  for (int i = 0; i < len; i++, mloop++) {
    p[i] = mloop->v;
  }

  *poly = p;

  return true;
}

static bool find_internal_spring_target_vertex(BVHTreeFromMesh *treedata,
                                               unsigned int v_idx,
                                               RNG *rng,
                                               float max_length,
                                               float max_diversion,
                                               bool check_normal,
                                               unsigned int *r_tar_v_idx)
{
  float co[3], no[3], new_co[3];
  float radius;

  copy_v3_v3(co, treedata->vert[v_idx].co);
  negate_v3_v3(no, treedata->vert_normals[v_idx]);

  float vec_len = sin(max_diversion);
  float offset[3];

  offset[0] = 0.5f - BLI_rng_get_float(rng);
  offset[1] = 0.5f - BLI_rng_get_float(rng);
  offset[2] = 0.5f - BLI_rng_get_float(rng);

  normalize_v3(offset);
  mul_v3_fl(offset, vec_len);
  add_v3_v3(no, offset);
  normalize_v3(no);

  /* Nudge the start point so we do not hit it with the ray. */
  copy_v3_v3(new_co, no);
  mul_v3_fl(new_co, FLT_EPSILON);
  add_v3_v3(new_co, co);

  radius = 0.0f;
  if (max_length == 0.0f) {
    max_length = FLT_MAX;
  }

  BVHTreeRayHit rayhit = {0};
  rayhit.index = -1;
  rayhit.dist = max_length;

  BLI_bvhtree_ray_cast(
      treedata->tree, new_co, no, radius, &rayhit, treedata->raycast_callback, treedata);

  unsigned int vert_idx = -1;
  const MLoop *mloop = treedata->loop;
  const MLoopTri *lt = NULL;

  if (rayhit.index != -1 && rayhit.dist <= max_length) {
    if (check_normal && dot_v3v3(rayhit.no, no) < 0.0f) {
      /* We hit a point that points in the same direction as our starting point. */
      return false;
    }

    float min_len = FLT_MAX;
    lt = &treedata->looptri[rayhit.index];

    for (int i = 0; i < 3; i++) {
      unsigned int tmp_vert_idx = mloop[lt->tri[i]].v;
      if (tmp_vert_idx == v_idx) {
        /* We managed to hit ourselves. */
        return false;
      }

      float len = len_v3v3(co, rayhit.co);
      if (len < min_len) {
        min_len = len;
        vert_idx = tmp_vert_idx;
      }
    }

    *r_tar_v_idx = vert_idx;
    return true;
  }

  return false;
}

static bool cloth_build_springs(ClothModifierData *clmd, Mesh *mesh)
{
  Cloth *cloth = clmd->clothObject;
  ClothSpring *spring = NULL, *tspring = NULL, *tspring2 = NULL;
  unsigned int struct_springs = 0, shear_springs = 0, bend_springs = 0, struct_springs_real = 0;
  unsigned int mvert_num = (unsigned int)mesh->totvert;
  unsigned int numedges = (unsigned int)mesh->totedge;
  unsigned int numpolys = (unsigned int)mesh->totpoly;
  float shrink_factor;
  const MEdge *medge = mesh->medge;
  const MPoly *mpoly = mesh->mpoly;
  const MLoop *mloop = mesh->mloop;
  int index2 = 0; /* our second vertex index */
  LinkNodePair *edgelist = NULL;
  EdgeSet *edgeset = NULL;
  LinkNode *search = NULL, *search2 = NULL;
  BendSpringRef *spring_ref = NULL;

  /* error handling */
  if (numedges == 0) {
    return false;
  }

  /* NOTE: handling ownership of springs and edgeset is quite sloppy
   * currently they are never initialized but assert just to be sure */
  BLI_assert(cloth->springs == NULL);
  BLI_assert(cloth->edgeset == NULL);

  cloth->springs = NULL;
  cloth->edgeset = NULL;

  if (clmd->sim_parms->bending_model == CLOTH_BENDING_ANGULAR) {
    spring_ref = MEM_callocN(sizeof(*spring_ref) * numedges, "temp bend spring reference");

    if (!spring_ref) {
      return false;
    }
  }
  else {
    edgelist = MEM_callocN(sizeof(*edgelist) * mvert_num, "cloth_edgelist_alloc");

    if (!edgelist) {
      return false;
    }
  }

  bool use_internal_springs = (clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_INTERNAL_SPRINGS);

  if (use_internal_springs && numpolys > 0) {
    BVHTreeFromMesh treedata = {NULL};
    unsigned int tar_v_idx;
    Mesh *tmp_mesh = NULL;
    RNG *rng;

    /* If using the rest shape key, it's necessary to make a copy of the mesh. */
    if (clmd->sim_parms->shapekey_rest &&
        !(clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_DYNAMIC_BASEMESH)) {
      tmp_mesh = cloth_make_rest_mesh(clmd, mesh);
    }

    EdgeSet *existing_vert_pairs = BLI_edgeset_new("cloth_sewing_edges_graph");
    BKE_bvhtree_from_mesh_get(&treedata, tmp_mesh ? tmp_mesh : mesh, BVHTREE_FROM_LOOPTRI, 2);
    rng = BLI_rng_new_srandom(0);

    for (int i = 0; i < mvert_num; i++) {
      if (find_internal_spring_target_vertex(
              &treedata,
              i,
              rng,
              clmd->sim_parms->internal_spring_max_length,
              clmd->sim_parms->internal_spring_max_diversion,
              (clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_INTERNAL_SPRINGS_NORMAL),
              &tar_v_idx)) {
        if (BLI_edgeset_haskey(existing_vert_pairs, i, tar_v_idx)) {
          /* We have already created a spring between these verts! */
          continue;
        }

        BLI_edgeset_insert(existing_vert_pairs, i, tar_v_idx);

        spring = (ClothSpring *)MEM_callocN(sizeof(ClothSpring), "cloth spring");

        if (spring) {
          spring_verts_ordered_set(spring, i, tar_v_idx);

          shrink_factor = cloth_shrink_factor(clmd, cloth->verts, spring->ij, spring->kl);
          spring->restlen = len_v3v3(cloth->verts[spring->kl].xrest,
                                     cloth->verts[spring->ij].xrest) *
                            shrink_factor;
          spring->lin_stiffness = (cloth->verts[spring->kl].internal_stiff +
                                   cloth->verts[spring->ij].internal_stiff) /
                                  2.0f;
          spring->type = CLOTH_SPRING_TYPE_INTERNAL;

          spring->flags = 0;

          BLI_linklist_prepend(&cloth->springs, spring);

          if (spring_ref) {
            spring_ref[i].spring = spring;
          }
        }
        else {
          cloth_free_errorsprings(cloth, edgelist, spring_ref);
          BLI_edgeset_free(existing_vert_pairs);
          free_bvhtree_from_mesh(&treedata);
          if (tmp_mesh) {
            BKE_id_free(NULL, &tmp_mesh->id);
          }
          return false;
        }
      }
    }
    BLI_edgeset_free(existing_vert_pairs);
    free_bvhtree_from_mesh(&treedata);
    if (tmp_mesh) {
      BKE_id_free(NULL, &tmp_mesh->id);
    }
    BLI_rng_free(rng);
  }

  clmd->sim_parms->avg_spring_len = 0.0f;
  for (int i = 0; i < mvert_num; i++) {
    cloth->verts[i].avg_spring_len = 0.0f;
  }

  if (clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_SEW) {
    /* cloth->sew_edge_graph should not exist before this */
    BLI_assert(cloth->sew_edge_graph == NULL);
    cloth->sew_edge_graph = BLI_edgeset_new("cloth_sewing_edges_graph");
  }

  /* Structural springs. */
  for (int i = 0; i < numedges; i++) {
    spring = (ClothSpring *)MEM_callocN(sizeof(ClothSpring), "cloth spring");

    if (spring) {
      spring_verts_ordered_set(spring, medge[i].v1, medge[i].v2);
      if (clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_SEW && medge[i].flag & ME_LOOSEEDGE) {
        /* handle sewing (loose edges will be pulled together) */
        spring->restlen = 0.0f;
        spring->lin_stiffness = 1.0f;
        spring->type = CLOTH_SPRING_TYPE_SEWING;

        BLI_edgeset_insert(cloth->sew_edge_graph, medge[i].v1, medge[i].v2);
      }
      else {
        shrink_factor = cloth_shrink_factor(clmd, cloth->verts, spring->ij, spring->kl);
        spring->restlen = len_v3v3(cloth->verts[spring->kl].xrest,
                                   cloth->verts[spring->ij].xrest) *
                          shrink_factor;
        spring->lin_stiffness = (cloth->verts[spring->kl].struct_stiff +
                                 cloth->verts[spring->ij].struct_stiff) /
                                2.0f;
        spring->type = CLOTH_SPRING_TYPE_STRUCTURAL;

        clmd->sim_parms->avg_spring_len += spring->restlen;
        cloth->verts[spring->ij].avg_spring_len += spring->restlen;
        cloth->verts[spring->kl].avg_spring_len += spring->restlen;
        cloth->verts[spring->ij].spring_count++;
        cloth->verts[spring->kl].spring_count++;
        struct_springs_real++;
      }

      spring->flags = 0;
      struct_springs++;

      BLI_linklist_prepend(&cloth->springs, spring);

      if (spring_ref) {
        spring_ref[i].spring = spring;
      }
    }
    else {
      cloth_free_errorsprings(cloth, edgelist, spring_ref);
      return false;
    }
  }

  if (struct_springs_real > 0) {
    clmd->sim_parms->avg_spring_len /= struct_springs_real;
  }

  for (int i = 0; i < mvert_num; i++) {
    if (cloth->verts[i].spring_count > 0) {
      cloth->verts[i].avg_spring_len = cloth->verts[i].avg_spring_len * 0.49f /
                                       ((float)cloth->verts[i].spring_count);
    }
  }

  edgeset = BLI_edgeset_new_ex(__func__, numedges);
  cloth->edgeset = edgeset;

  if (numpolys) {
    for (int i = 0; i < numpolys; i++) {
      /* Shear springs. */
      /* Triangle faces already have shear springs due to structural geometry. */
      if (mpoly[i].totloop > 3) {
        for (int j = 1; j < mpoly[i].totloop - 1; j++) {
          if (j > 1) {
            if (cloth_add_shear_bend_spring(clmd, edgelist, mloop, mpoly, i, 0, j)) {
              shear_springs++;

              if (clmd->sim_parms->bending_model == CLOTH_BENDING_ANGULAR) {
                bend_springs++;
              }
            }
            else {
              cloth_free_errorsprings(cloth, edgelist, spring_ref);
              return false;
            }
          }

          for (int k = j + 2; k < mpoly[i].totloop; k++) {
            if (cloth_add_shear_bend_spring(clmd, edgelist, mloop, mpoly, i, j, k)) {
              shear_springs++;

              if (clmd->sim_parms->bending_model == CLOTH_BENDING_ANGULAR) {
                bend_springs++;
              }
            }
            else {
              cloth_free_errorsprings(cloth, edgelist, spring_ref);
              return false;
            }
          }
        }
      }

      /* Angular bending springs along struct springs. */
      if (clmd->sim_parms->bending_model == CLOTH_BENDING_ANGULAR) {
        const MLoop *ml = mloop + mpoly[i].loopstart;

        for (int j = 0; j < mpoly[i].totloop; j++, ml++) {
          BendSpringRef *curr_ref = &spring_ref[ml->e];
          curr_ref->polys++;

          /* First poly found for this edge, store poly index. */
          if (curr_ref->polys == 1) {
            curr_ref->index = i;
          }
          /* Second poly found for this edge, add bending data. */
          else if (curr_ref->polys == 2) {
            spring = curr_ref->spring;

            spring->type |= CLOTH_SPRING_TYPE_BENDING;

            spring->la = mpoly[curr_ref->index].totloop;
            spring->lb = mpoly[i].totloop;

            if (!cloth_bend_set_poly_vert_array(
                    &spring->pa, spring->la, &mloop[mpoly[curr_ref->index].loopstart]) ||
                !cloth_bend_set_poly_vert_array(
                    &spring->pb, spring->lb, &mloop[mpoly[i].loopstart])) {
              cloth_free_errorsprings(cloth, edgelist, spring_ref);
              return false;
            }

            spring->mn = ml->e;

            spring->restang = cloth_spring_angle(cloth->verts,
                                                 spring->ij,
                                                 spring->kl,
                                                 spring->pa,
                                                 spring->pb,
                                                 spring->la,
                                                 spring->lb);

            spring->ang_stiffness = (cloth->verts[spring->ij].bend_stiff +
                                     cloth->verts[spring->kl].bend_stiff) /
                                    2.0f;

            bend_springs++;
          }
          /* Third poly found for this edge, remove bending data. */
          else if (curr_ref->polys == 3) {
            spring = curr_ref->spring;

            spring->type &= ~CLOTH_SPRING_TYPE_BENDING;
            MEM_freeN(spring->pa);
            MEM_freeN(spring->pb);
            spring->pa = NULL;
            spring->pb = NULL;

            bend_springs--;
          }
        }
      }
    }

    /* Linear bending springs. */
    if (clmd->sim_parms->bending_model == CLOTH_BENDING_LINEAR) {
      search2 = cloth->springs;

      for (int i = struct_springs; i < struct_springs + shear_springs; i++) {
        if (!search2) {
          break;
        }

        tspring2 = search2->link;
        search = edgelist[tspring2->kl].list;

        while (search) {
          tspring = search->link;
          index2 = ((tspring->ij == tspring2->kl) ? (tspring->kl) : (tspring->ij));

          /* Check for existing spring. */
          /* Check also if startpoint is equal to endpoint. */
          if ((index2 != tspring2->ij) && !BLI_edgeset_haskey(edgeset, tspring2->ij, index2)) {
            spring = (ClothSpring *)MEM_callocN(sizeof(ClothSpring), "cloth spring");

            if (!spring) {
              cloth_free_errorsprings(cloth, edgelist, spring_ref);
              return false;
            }

            spring_verts_ordered_set(spring, tspring2->ij, index2);
            shrink_factor = cloth_shrink_factor(clmd, cloth->verts, spring->ij, spring->kl);
            spring->restlen = len_v3v3(cloth->verts[spring->kl].xrest,
                                       cloth->verts[spring->ij].xrest) *
                              shrink_factor;
            spring->type = CLOTH_SPRING_TYPE_BENDING;
            spring->lin_stiffness = (cloth->verts[spring->kl].bend_stiff +
                                     cloth->verts[spring->ij].bend_stiff) /
                                    2.0f;
            BLI_edgeset_insert(edgeset, spring->ij, spring->kl);
            bend_springs++;

            BLI_linklist_prepend(&cloth->springs, spring);
          }

          search = search->next;
        }

        search2 = search2->next;
      }
    }
  }
  else if (struct_springs > 2) {
    if (G.debug_value != 1112) {
      search = cloth->springs;
      search2 = search->next;
      while (search && search2) {
        tspring = search->link;
        tspring2 = search2->link;

        if (tspring->ij == tspring2->kl) {
          spring = (ClothSpring *)MEM_callocN(sizeof(ClothSpring), "cloth spring");

          if (!spring) {
            cloth_free_errorsprings(cloth, edgelist, spring_ref);
            return false;
          }

          spring->ij = tspring2->ij;
          spring->kl = tspring->ij;
          spring->mn = tspring->kl;
          spring->restlen = len_v3v3(cloth->verts[spring->kl].xrest,
                                     cloth->verts[spring->ij].xrest);
          spring->type = CLOTH_SPRING_TYPE_BENDING_HAIR;
          spring->lin_stiffness = (cloth->verts[spring->kl].bend_stiff +
                                   cloth->verts[spring->ij].bend_stiff) /
                                  2.0f;
          bend_springs++;

          BLI_linklist_prepend(&cloth->springs, spring);
        }

        search = search->next;
        search2 = search2->next;
      }
    }
    else {
      /* bending springs for hair strands
       * The current algorithm only goes through the edges in order of the mesh edges list
       * and makes springs between the outer vert of edges sharing a vertex. This works just
       * fine for hair, but not for user generated string meshes. This could/should be later
       * extended to work with non-ordered edges so that it can be used for general "rope
       * dynamics" without the need for the vertices or edges to be ordered through the length
       * of the strands. -jahka */
      search = cloth->springs;
      search2 = search->next;
      while (search && search2) {
        tspring = search->link;
        tspring2 = search2->link;

        if (tspring->ij == tspring2->kl) {
          spring = (ClothSpring *)MEM_callocN(sizeof(ClothSpring), "cloth spring");

          if (!spring) {
            cloth_free_errorsprings(cloth, edgelist, spring_ref);
            return false;
          }

          spring->ij = tspring2->ij;
          spring->kl = tspring->kl;
          spring->restlen = len_v3v3(cloth->verts[spring->kl].xrest,
                                     cloth->verts[spring->ij].xrest);
          spring->type = CLOTH_SPRING_TYPE_BENDING;
          spring->lin_stiffness = (cloth->verts[spring->kl].bend_stiff +
                                   cloth->verts[spring->ij].bend_stiff) /
                                  2.0f;
          bend_springs++;

          BLI_linklist_prepend(&cloth->springs, spring);
        }

        search = search->next;
        search2 = search2->next;
      }
    }

    cloth_hair_update_bending_rest_targets(clmd);
  }

  /* NOTE: the edges may already exist so run reinsert. */

  /* insert other near springs in edgeset AFTER bending springs are calculated (for selfcolls) */
  for (int i = 0; i < numedges; i++) { /* struct springs */
    BLI_edgeset_add(edgeset, medge[i].v1, medge[i].v2);
  }

  for (int i = 0; i < numpolys; i++) { /* edge springs */
    if (mpoly[i].totloop == 4) {
      BLI_edgeset_add(edgeset, mloop[mpoly[i].loopstart + 0].v, mloop[mpoly[i].loopstart + 2].v);
      BLI_edgeset_add(edgeset, mloop[mpoly[i].loopstart + 1].v, mloop[mpoly[i].loopstart + 3].v);
    }
  }

  MEM_SAFE_FREE(spring_ref);

  cloth->numsprings = struct_springs + shear_springs + bend_springs;

  cloth_free_edgelist(edgelist, mvert_num);

#if 0
  if (G.debug_value > 0) {
    printf("avg_len: %f\n", clmd->sim_parms->avg_spring_len);
  }
#endif

  return true;
}

/** \} */
