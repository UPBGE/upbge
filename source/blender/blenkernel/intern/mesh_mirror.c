/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include "BLI_math.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "BKE_deform.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mirror.h"
#include "BKE_modifier.h"

#include "bmesh.h"
#include "bmesh_tools.h"

#include "MEM_guardedalloc.h"

#include "MOD_modifiertypes.h"

Mesh *BKE_mesh_mirror_bisect_on_mirror_plane_for_modifier(MirrorModifierData *mmd,
                                                          const Mesh *mesh,
                                                          int axis,
                                                          const float plane_co[3],
                                                          float plane_no[3])
{
  bool do_bisect_flip_axis = ((axis == 0 && mmd->flag & MOD_MIR_BISECT_FLIP_AXIS_X) ||
                              (axis == 1 && mmd->flag & MOD_MIR_BISECT_FLIP_AXIS_Y) ||
                              (axis == 2 && mmd->flag & MOD_MIR_BISECT_FLIP_AXIS_Z));

  const float bisect_distance = mmd->bisect_threshold;

  Mesh *result;
  BMesh *bm;
  BMIter viter;
  BMVert *v, *v_next;

  bm = BKE_mesh_to_bmesh_ex(mesh,
                            &(struct BMeshCreateParams){0},
                            &(struct BMeshFromMeshParams){
                                .calc_face_normal = true,
                                .calc_vert_normal = true,
                                .cd_mask_extra = {.vmask = CD_MASK_ORIGINDEX,
                                                  .emask = CD_MASK_ORIGINDEX,
                                                  .pmask = CD_MASK_ORIGINDEX},
                            });

  /* Define bisecting plane (aka mirror plane). */
  float plane[4];
  if (!do_bisect_flip_axis) {
    /* That reversed condition is a little weird, but for some reason that's how you keep
     * the part of the mesh which is on the non-mirrored side when flip option is disabled.
     * I think this is the expected behavior. */
    negate_v3(plane_no);
  }
  plane_from_point_normal_v3(plane, plane_co, plane_no);

  BM_mesh_bisect_plane(bm, plane, true, false, 0, 0, bisect_distance);

  /* Plane definitions for vert killing. */
  float plane_offset[4];
  copy_v3_v3(plane_offset, plane);
  plane_offset[3] = plane[3] - bisect_distance;

  /* Delete verts across the mirror plane. */
  BM_ITER_MESH_MUTABLE (v, v_next, &viter, bm, BM_VERTS_OF_MESH) {
    if (plane_point_side_v3(plane_offset, v->co) > 0.0f) {
      BM_vert_kill(bm, v);
    }
  }

  result = BKE_mesh_from_bmesh_for_eval_nomain(bm, NULL, mesh);
  BM_mesh_free(bm);

  return result;
}

void BKE_mesh_mirror_apply_mirror_on_axis(struct Main *bmain,
                                          Mesh *mesh,
                                          const int axis,
                                          const float dist)
{
  BMesh *bm = BKE_mesh_to_bmesh_ex(mesh,
                                   &(struct BMeshCreateParams){
                                       .use_toolflags = 1,
                                   },
                                   &(struct BMeshFromMeshParams){
                                       .calc_face_normal = true,
                                       .calc_vert_normal = true,
                                       .cd_mask_extra =
                                           {
                                               .vmask = CD_MASK_SHAPEKEY,
                                           },
                                   });
  BMO_op_callf(bm,
               (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
               "symmetrize input=%avef direction=%i dist=%f use_shapekey=%b",
               axis,
               dist,
               true);

  BM_mesh_bm_to_me(bmain,
                   bm,
                   mesh,
                   (&(struct BMeshToMeshParams){
                       .calc_object_remap = true,

                   }));
  BM_mesh_free(bm);
}

Mesh *BKE_mesh_mirror_apply_mirror_on_axis_for_modifier(MirrorModifierData *mmd,
                                                        Object *ob,
                                                        const Mesh *mesh,
                                                        const int axis,
                                                        const bool use_correct_order_on_merge)
{
  const float tolerance_sq = mmd->tolerance * mmd->tolerance;
  const bool do_vtargetmap = (mmd->flag & MOD_MIR_NO_MERGE) == 0;
  int tot_vtargetmap = 0; /* total merge vertices */

  const bool do_bisect = ((axis == 0 && mmd->flag & MOD_MIR_BISECT_AXIS_X) ||
                          (axis == 1 && mmd->flag & MOD_MIR_BISECT_AXIS_Y) ||
                          (axis == 2 && mmd->flag & MOD_MIR_BISECT_AXIS_Z));

  Mesh *result;
  MVert *mv, *mv_prev;
  MEdge *me;
  MLoop *ml;
  MPoly *mp;
  float mtx[4][4];
  float plane_co[3], plane_no[3];
  int i;
  int a, totshape;
  int *vtargetmap = NULL, *vtmap_a = NULL, *vtmap_b = NULL;

  /* mtx is the mirror transformation */
  unit_m4(mtx);
  mtx[axis][axis] = -1.0f;

  Object *mirror_ob = mmd->mirror_ob;
  if (mirror_ob != NULL) {
    float tmp[4][4];
    float itmp[4][4];

    /* tmp is a transform from coords relative to the object's own origin,
     * to coords relative to the mirror object origin */
    invert_m4_m4(tmp, mirror_ob->obmat);
    mul_m4_m4m4(tmp, tmp, ob->obmat);

    /* itmp is the reverse transform back to origin-relative coordinates */
    invert_m4_m4(itmp, tmp);

    /* combine matrices to get a single matrix that translates coordinates into
     * mirror-object-relative space, does the mirror, and translates back to
     * origin-relative space */
    mul_m4_series(mtx, itmp, mtx, tmp);

    if (do_bisect) {
      copy_v3_v3(plane_co, itmp[3]);
      copy_v3_v3(plane_no, itmp[axis]);

      /* Account for non-uniform scale in `ob`, see: T87592. */
      float ob_scale[3] = {
          len_squared_v3(ob->obmat[0]),
          len_squared_v3(ob->obmat[1]),
          len_squared_v3(ob->obmat[2]),
      };
      /* Scale to avoid precision loss with extreme values. */
      const float ob_scale_max = max_fff(UNPACK3(ob_scale));
      if (LIKELY(ob_scale_max != 0.0f)) {
        mul_v3_fl(ob_scale, 1.0f / ob_scale_max);
        mul_v3_v3(plane_no, ob_scale);
      }
    }
  }
  else if (do_bisect) {
    copy_v3_v3(plane_co, mtx[3]);
    /* Need to negate here, since that axis is inverted (for mirror transform). */
    negate_v3_v3(plane_no, mtx[axis]);
  }

  Mesh *mesh_bisect = NULL;
  if (do_bisect) {
    mesh_bisect = BKE_mesh_mirror_bisect_on_mirror_plane_for_modifier(
        mmd, mesh, axis, plane_co, plane_no);
    mesh = mesh_bisect;
  }

  const int maxVerts = mesh->totvert;
  const int maxEdges = mesh->totedge;
  const int maxLoops = mesh->totloop;
  const int maxPolys = mesh->totpoly;

  result = BKE_mesh_new_nomain_from_template(
      mesh, maxVerts * 2, maxEdges * 2, 0, maxLoops * 2, maxPolys * 2);

  /* Copy custom-data to original geometry. */
  CustomData_copy_data(&mesh->vdata, &result->vdata, 0, 0, maxVerts);
  CustomData_copy_data(&mesh->edata, &result->edata, 0, 0, maxEdges);
  CustomData_copy_data(&mesh->ldata, &result->ldata, 0, 0, maxLoops);
  CustomData_copy_data(&mesh->pdata, &result->pdata, 0, 0, maxPolys);

  /* Subsurf for eg won't have mesh data in the custom-data arrays.
   * now add mvert/medge/mpoly layers. */
  if (!CustomData_has_layer(&mesh->vdata, CD_MVERT)) {
    memcpy(result->mvert, mesh->mvert, sizeof(*result->mvert) * mesh->totvert);
  }
  if (!CustomData_has_layer(&mesh->edata, CD_MEDGE)) {
    memcpy(result->medge, mesh->medge, sizeof(*result->medge) * mesh->totedge);
  }
  if (!CustomData_has_layer(&mesh->pdata, CD_MPOLY)) {
    memcpy(result->mloop, mesh->mloop, sizeof(*result->mloop) * mesh->totloop);
    memcpy(result->mpoly, mesh->mpoly, sizeof(*result->mpoly) * mesh->totpoly);
  }

  /* Copy custom-data to new geometry,
   * copy from itself because this data may have been created in the checks above. */
  CustomData_copy_data(&result->vdata, &result->vdata, 0, maxVerts, maxVerts);
  CustomData_copy_data(&result->edata, &result->edata, 0, maxEdges, maxEdges);
  /* loops are copied later */
  CustomData_copy_data(&result->pdata, &result->pdata, 0, maxPolys, maxPolys);

  if (do_vtargetmap) {
    /* second half is filled with -1 */
    vtargetmap = MEM_malloc_arrayN(maxVerts, sizeof(int[2]), "MOD_mirror tarmap");

    vtmap_a = vtargetmap;
    vtmap_b = vtargetmap + maxVerts;
  }

  /* mirror vertex coordinates */
  mv_prev = result->mvert;
  mv = mv_prev + maxVerts;
  for (i = 0; i < maxVerts; i++, mv++, mv_prev++) {
    mul_m4_v3(mtx, mv->co);

    if (do_vtargetmap) {
      /* Compare location of the original and mirrored vertex,
       * to see if they should be mapped for merging.
       *
       * Always merge from the copied into the original vertices so it's possible to
       * generate a 1:1 mapping by scanning vertices from the beginning of the array
       * as is done in #BKE_editmesh_vert_coords_when_deformed. Without this,
       * the coordinates returned will sometimes point to the copied vertex locations, see:
       * T91444.
       *
       * However, such a change also affects non-versionable things like some modifiers binding, so
       * we cannot enforce that behavior on existing modifiers, in which case we keep using the
       * old, incorrect behavior of merging the source vertex into its copy.
       */
      if (use_correct_order_on_merge) {
        if (UNLIKELY(len_squared_v3v3(mv_prev->co, mv->co) < tolerance_sq)) {
          *vtmap_b = i;
          tot_vtargetmap++;

          /* average location */
          mid_v3_v3v3(mv->co, mv_prev->co, mv->co);
          copy_v3_v3(mv_prev->co, mv->co);
        }
        else {
          *vtmap_b = -1;
        }

        /* Fill here to avoid 2x loops. */
        *vtmap_a = -1;
      }
      else {
        if (UNLIKELY(len_squared_v3v3(mv_prev->co, mv->co) < tolerance_sq)) {
          *vtmap_a = maxVerts + i;
          tot_vtargetmap++;

          /* average location */
          mid_v3_v3v3(mv->co, mv_prev->co, mv->co);
          copy_v3_v3(mv_prev->co, mv->co);
        }
        else {
          *vtmap_a = -1;
        }

        /* Fill here to avoid 2x loops. */
        *vtmap_b = -1;
      }

      vtmap_a++;
      vtmap_b++;
    }
  }

  /* handle shape keys */
  totshape = CustomData_number_of_layers(&result->vdata, CD_SHAPEKEY);
  for (a = 0; a < totshape; a++) {
    float(*cos)[3] = CustomData_get_layer_n(&result->vdata, CD_SHAPEKEY, a);
    for (i = maxVerts; i < result->totvert; i++) {
      mul_m4_v3(mtx, cos[i]);
    }
  }

  /* adjust mirrored edge vertex indices */
  me = result->medge + maxEdges;
  for (i = 0; i < maxEdges; i++, me++) {
    me->v1 += maxVerts;
    me->v2 += maxVerts;
  }

  /* adjust mirrored poly loopstart indices, and reverse loop order (normals) */
  mp = result->mpoly + maxPolys;
  ml = result->mloop;
  for (i = 0; i < maxPolys; i++, mp++) {
    MLoop *ml2;
    int j, e;

    /* reverse the loop, but we keep the first vertex in the face the same,
     * to ensure that quads are split the same way as on the other side */
    CustomData_copy_data(
        &result->ldata, &result->ldata, mp->loopstart, mp->loopstart + maxLoops, 1);

    for (j = 1; j < mp->totloop; j++) {
      CustomData_copy_data(&result->ldata,
                           &result->ldata,
                           mp->loopstart + j,
                           mp->loopstart + maxLoops + mp->totloop - j,
                           1);
    }

    ml2 = ml + mp->loopstart + maxLoops;
    e = ml2[0].e;
    for (j = 0; j < mp->totloop - 1; j++) {
      ml2[j].e = ml2[j + 1].e;
    }
    ml2[mp->totloop - 1].e = e;

    mp->loopstart += maxLoops;
  }

  /* adjust mirrored loop vertex and edge indices */
  ml = result->mloop + maxLoops;
  for (i = 0; i < maxLoops; i++, ml++) {
    ml->v += maxVerts;
    ml->e += maxEdges;
  }

  /* handle uvs,
   * let tessface recalc handle updating the MTFace data */
  if (mmd->flag & (MOD_MIR_MIRROR_U | MOD_MIR_MIRROR_V) ||
      (is_zero_v2(mmd->uv_offset_copy) == false)) {
    const bool do_mirr_u = (mmd->flag & MOD_MIR_MIRROR_U) != 0;
    const bool do_mirr_v = (mmd->flag & MOD_MIR_MIRROR_V) != 0;
    /* If set, flip around center of each tile. */
    const bool do_mirr_udim = (mmd->flag & MOD_MIR_MIRROR_UDIM) != 0;

    const int totuv = CustomData_number_of_layers(&result->ldata, CD_MLOOPUV);

    for (a = 0; a < totuv; a++) {
      MLoopUV *dmloopuv = CustomData_get_layer_n(&result->ldata, CD_MLOOPUV, a);
      int j = maxLoops;
      dmloopuv += j; /* second set of loops only */
      for (; j-- > 0; dmloopuv++) {
        if (do_mirr_u) {
          float u = dmloopuv->uv[0];
          if (do_mirr_udim) {
            dmloopuv->uv[0] = ceilf(u) - fmodf(u, 1.0f) + mmd->uv_offset[0];
          }
          else {
            dmloopuv->uv[0] = 1.0f - u + mmd->uv_offset[0];
          }
        }
        if (do_mirr_v) {
          float v = dmloopuv->uv[1];
          if (do_mirr_udim) {
            dmloopuv->uv[1] = ceilf(v) - fmodf(v, 1.0f) + mmd->uv_offset[1];
          }
          else {
            dmloopuv->uv[1] = 1.0f - v + mmd->uv_offset[1];
          }
        }
        dmloopuv->uv[0] += mmd->uv_offset_copy[0];
        dmloopuv->uv[1] += mmd->uv_offset_copy[1];
      }
    }
  }

  /* handle custom split normals */
  if (ob->type == OB_MESH && (((Mesh *)ob->data)->flag & ME_AUTOSMOOTH) &&
      CustomData_has_layer(&result->ldata, CD_CUSTOMLOOPNORMAL)) {
    const int totloop = result->totloop;
    const int totpoly = result->totpoly;
    float(*loop_normals)[3] = MEM_calloc_arrayN((size_t)totloop, sizeof(*loop_normals), __func__);
    CustomData *ldata = &result->ldata;
    short(*clnors)[2] = CustomData_get_layer(ldata, CD_CUSTOMLOOPNORMAL);
    MLoopNorSpaceArray lnors_spacearr = {NULL};

    /* The transform matrix of a normal must be
     * the transpose of inverse of transform matrix of the geometry... */
    float mtx_nor[4][4];
    invert_m4_m4(mtx_nor, mtx);
    transpose_m4(mtx_nor);

    /* calculate custom normals into loop_normals, then mirror first half into second half */

    BKE_mesh_normals_loop_split(result->mvert,
                                BKE_mesh_vertex_normals_ensure(result),
                                result->totvert,
                                result->medge,
                                result->totedge,
                                result->mloop,
                                loop_normals,
                                totloop,
                                result->mpoly,
                                BKE_mesh_poly_normals_ensure(result),
                                totpoly,
                                true,
                                mesh->smoothresh,
                                &lnors_spacearr,
                                clnors,
                                NULL);

    /* mirroring has to account for loops being reversed in polys in second half */
    mp = result->mpoly;
    for (i = 0; i < maxPolys; i++, mp++) {
      MPoly *mpmirror = result->mpoly + maxPolys + i;
      int j;

      for (j = mp->loopstart; j < mp->loopstart + mp->totloop; j++) {
        int mirrorj = mpmirror->loopstart;
        if (j > mp->loopstart) {
          mirrorj += mpmirror->totloop - (j - mp->loopstart);
        }
        copy_v3_v3(loop_normals[mirrorj], loop_normals[j]);
        mul_m4_v3(mtx_nor, loop_normals[mirrorj]);
        BKE_lnor_space_custom_normal_to_data(
            lnors_spacearr.lspacearr[mirrorj], loop_normals[mirrorj], clnors[mirrorj]);
      }
    }

    MEM_freeN(loop_normals);
    BKE_lnor_spacearr_free(&lnors_spacearr);
  }

  /* handle vgroup stuff */
  if ((mmd->flag & MOD_MIR_VGROUP) && CustomData_has_layer(&result->vdata, CD_MDEFORMVERT)) {
    MDeformVert *dvert = (MDeformVert *)CustomData_get_layer(&result->vdata, CD_MDEFORMVERT) +
                         maxVerts;
    int *flip_map = NULL, flip_map_len = 0;

    flip_map = BKE_object_defgroup_flip_map(ob, &flip_map_len, false);

    if (flip_map) {
      for (i = 0; i < maxVerts; dvert++, i++) {
        /* merged vertices get both groups, others get flipped */
        if (use_correct_order_on_merge && do_vtargetmap && (vtargetmap[i + maxVerts] != -1)) {
          BKE_defvert_flip_merged(dvert - maxVerts, flip_map, flip_map_len);
        }
        else if (!use_correct_order_on_merge && do_vtargetmap && (vtargetmap[i] != -1)) {
          BKE_defvert_flip_merged(dvert, flip_map, flip_map_len);
        }
        else {
          BKE_defvert_flip(dvert, flip_map, flip_map_len);
        }
      }

      MEM_freeN(flip_map);
    }
  }

  if (do_vtargetmap) {
    /* slow - so only call if one or more merge verts are found,
     * users may leave this on and not realize there is nothing to merge - campbell */
    if (tot_vtargetmap) {
      result = BKE_mesh_merge_verts(
          result, vtargetmap, tot_vtargetmap, MESH_MERGE_VERTS_DUMP_IF_MAPPED);
    }
    MEM_freeN(vtargetmap);
  }

  if (mesh_bisect != NULL) {
    BKE_id_free(NULL, mesh_bisect);
  }

  return result;
}
