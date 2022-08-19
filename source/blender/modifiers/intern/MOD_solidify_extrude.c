/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "BLI_utildefines.h"

#include "BLI_bitmap.h"
#include "BLI_math.h"
#include "BLI_utildefines_stack.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "MEM_guardedalloc.h"

#include "BKE_deform.h"
#include "BKE_mesh.h"
#include "BKE_particle.h"

#include "MOD_modifiertypes.h"
#include "MOD_solidify_util.h" /* own include */
#include "MOD_util.h"

#ifdef __GNUC__
#  pragma GCC diagnostic error "-Wsign-conversion"
#endif

/* -------------------------------------------------------------------- */
/** \name High Quality Normal Calculation Function
 * \{ */

/* skip shell thickness for non-manifold edges, see T35710. */
#define USE_NONMANIFOLD_WORKAROUND

/* *** derived mesh high quality normal calculation function  *** */
/* could be exposed for other functions to use */

typedef struct EdgeFaceRef {
  int p1; /* init as -1 */
  int p2;
} EdgeFaceRef;

BLI_INLINE bool edgeref_is_init(const EdgeFaceRef *edge_ref)
{
  return !((edge_ref->p1 == 0) && (edge_ref->p2 == 0));
}

/**
 * \param mesh: Mesh to calculate normals for.
 * \param poly_nors: Precalculated face normals.
 * \param r_vert_nors: Return vert normals.
 */
static void mesh_calc_hq_normal(Mesh *mesh,
                                const float (*poly_nors)[3],
                                float (*r_vert_nors)[3],
#ifdef USE_NONMANIFOLD_WORKAROUND
                                BLI_bitmap *edge_tmp_tag
#endif
)
{
  int i, verts_num, edges_num, polys_num;
  MPoly *mpoly, *mp;
  MLoop *mloop, *ml;
  MEdge *medge, *ed;

  verts_num = mesh->totvert;
  edges_num = mesh->totedge;
  polys_num = mesh->totpoly;
  mpoly = mesh->mpoly;
  medge = mesh->medge;
  mloop = mesh->mloop;

  /* we don't want to overwrite any referenced layers */

  /* Doesn't work here! */
#if 0
  mv = CustomData_duplicate_referenced_layer(&dm->vertData, CD_MVERT, verts_num);
  cddm->mvert = mv;
#endif

  mp = mpoly;

  {
    EdgeFaceRef *edge_ref_array = MEM_calloc_arrayN(
        (size_t)edges_num, sizeof(EdgeFaceRef), "Edge Connectivity");
    EdgeFaceRef *edge_ref;
    float edge_normal[3];

    /* Add an edge reference if it's not there, pointing back to the face index. */
    for (i = 0; i < polys_num; i++, mp++) {
      int j;

      ml = mloop + mp->loopstart;

      for (j = 0; j < mp->totloop; j++, ml++) {
        /* --- add edge ref to face --- */
        edge_ref = &edge_ref_array[ml->e];
        if (!edgeref_is_init(edge_ref)) {
          edge_ref->p1 = i;
          edge_ref->p2 = -1;
        }
        else if ((edge_ref->p1 != -1) && (edge_ref->p2 == -1)) {
          edge_ref->p2 = i;
        }
        else {
          /* 3+ faces using an edge, we can't handle this usefully */
          edge_ref->p1 = edge_ref->p2 = -1;
#ifdef USE_NONMANIFOLD_WORKAROUND
          BLI_BITMAP_ENABLE(edge_tmp_tag, ml->e);
#endif
        }
        /* --- done --- */
      }
    }

    for (i = 0, ed = medge, edge_ref = edge_ref_array; i < edges_num; i++, ed++, edge_ref++) {
      /* Get the edge vert indices, and edge value (the face indices that use it) */

      if (edgeref_is_init(edge_ref) && (edge_ref->p1 != -1)) {
        if (edge_ref->p2 != -1) {
          /* We have 2 faces using this edge, calculate the edges normal
           * using the angle between the 2 faces as a weighting */
#if 0
          add_v3_v3v3(edge_normal, face_nors[edge_ref->f1], face_nors[edge_ref->f2]);
          normalize_v3_length(
              edge_normal,
              angle_normalized_v3v3(face_nors[edge_ref->f1], face_nors[edge_ref->f2]));
#else
          mid_v3_v3v3_angle_weighted(
              edge_normal, poly_nors[edge_ref->p1], poly_nors[edge_ref->p2]);
#endif
        }
        else {
          /* only one face attached to that edge */
          /* an edge without another attached- the weight on this is undefined */
          copy_v3_v3(edge_normal, poly_nors[edge_ref->p1]);
        }
        add_v3_v3(r_vert_nors[ed->v1], edge_normal);
        add_v3_v3(r_vert_nors[ed->v2], edge_normal);
      }
    }
    MEM_freeN(edge_ref_array);
  }

  /* normalize vertex normals and assign */
  const float(*vert_normals)[3] = BKE_mesh_vertex_normals_ensure(mesh);
  for (i = 0; i < verts_num; i++) {
    if (normalize_v3(r_vert_nors[i]) == 0.0f) {
      copy_v3_v3(r_vert_nors[i], vert_normals[i]);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main Solidify Function
 * \{ */

/* NOLINTNEXTLINE: readability-function-size */
Mesh *MOD_solidify_extrude_modifyMesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  Mesh *result;
  const SolidifyModifierData *smd = (SolidifyModifierData *)md;

  MVert *mv, *mvert, *orig_mvert;
  MEdge *ed, *medge, *orig_medge;
  MLoop *ml, *mloop, *orig_mloop;
  MPoly *mp, *mpoly, *orig_mpoly;
  const uint verts_num = (uint)mesh->totvert;
  const uint edges_num = (uint)mesh->totedge;
  const uint polys_num = (uint)mesh->totpoly;
  const uint loops_num = (uint)mesh->totloop;
  uint newLoops = 0, newPolys = 0, newEdges = 0, newVerts = 0, rimVerts = 0;

  /* Only use material offsets if we have 2 or more materials. */
  const short mat_nr_max = ctx->object->totcol > 1 ? ctx->object->totcol - 1 : 0;
  const short mat_ofs = mat_nr_max ? smd->mat_ofs : 0;
  const short mat_ofs_rim = mat_nr_max ? smd->mat_ofs_rim : 0;

  /* use for edges */
  /* over-alloc new_vert_arr, old_vert_arr */
  uint *new_vert_arr = NULL;
  STACK_DECLARE(new_vert_arr);

  uint *new_edge_arr = NULL;
  STACK_DECLARE(new_edge_arr);

  uint *old_vert_arr = MEM_calloc_arrayN(
      verts_num, sizeof(*old_vert_arr), "old_vert_arr in solidify");

  uint *edge_users = NULL;
  int *edge_order = NULL;

  float(*vert_nors)[3] = NULL;
  const float(*poly_nors)[3] = NULL;

  const bool need_poly_normals = (smd->flag & MOD_SOLIDIFY_NORMAL_CALC) ||
                                 (smd->flag & MOD_SOLIDIFY_EVEN) ||
                                 (smd->flag & MOD_SOLIDIFY_OFFSET_ANGLE_CLAMP) ||
                                 (smd->bevel_convex != 0);

  const float ofs_orig = -(((-smd->offset_fac + 1.0f) * 0.5f) * smd->offset);
  const float ofs_new = smd->offset + ofs_orig;
  const float offset_fac_vg = smd->offset_fac_vg;
  const float offset_fac_vg_inv = 1.0f - smd->offset_fac_vg;
  const float bevel_convex = smd->bevel_convex;
  const bool do_flip = (smd->flag & MOD_SOLIDIFY_FLIP) != 0;
  const bool do_clamp = (smd->offset_clamp != 0.0f);
  const bool do_angle_clamp = do_clamp && (smd->flag & MOD_SOLIDIFY_OFFSET_ANGLE_CLAMP) != 0;
  const bool do_bevel_convex = bevel_convex != 0.0f;
  const bool do_rim = (smd->flag & MOD_SOLIDIFY_RIM) != 0;
  const bool do_shell = !(do_rim && (smd->flag & MOD_SOLIDIFY_NOSHELL) != 0);

  /* weights */
  MDeformVert *dvert;
  const bool defgrp_invert = (smd->flag & MOD_SOLIDIFY_VGROUP_INV) != 0;
  int defgrp_index;
  const int shell_defgrp_index = BKE_id_defgroup_name_index(&mesh->id, smd->shell_defgrp_name);
  const int rim_defgrp_index = BKE_id_defgroup_name_index(&mesh->id, smd->rim_defgrp_name);

  /* array size is doubled in case of using a shell */
  const uint stride = do_shell ? 2 : 1;

  const float(*mesh_vert_normals)[3] = BKE_mesh_vertex_normals_ensure(mesh);

  MOD_get_vgroup(ctx->object, mesh, smd->defgrp_name, &dvert, &defgrp_index);

  orig_mvert = mesh->mvert;
  orig_medge = mesh->medge;
  orig_mloop = mesh->mloop;
  orig_mpoly = mesh->mpoly;

  if (need_poly_normals) {
    /* calculate only face normals */
    poly_nors = BKE_mesh_poly_normals_ensure(mesh);
  }

  STACK_INIT(new_vert_arr, verts_num * 2);
  STACK_INIT(new_edge_arr, edges_num * 2);

  if (do_rim) {
    BLI_bitmap *orig_mvert_tag = BLI_BITMAP_NEW(verts_num, __func__);
    uint eidx;
    uint i;

#define INVALID_UNUSED ((uint)-1)
#define INVALID_PAIR ((uint)-2)

    new_vert_arr = MEM_malloc_arrayN(verts_num, 2 * sizeof(*new_vert_arr), __func__);
    new_edge_arr = MEM_malloc_arrayN(
        ((edges_num * 2) + verts_num), sizeof(*new_edge_arr), __func__);

    edge_users = MEM_malloc_arrayN(edges_num, sizeof(*edge_users), "solid_mod edges");
    edge_order = MEM_malloc_arrayN(edges_num, sizeof(*edge_order), "solid_mod order");

    /* save doing 2 loops here... */
#if 0
    copy_vn_i(edge_users, edges_num, INVALID_UNUSED);
#endif

    for (eidx = 0, ed = orig_medge; eidx < edges_num; eidx++, ed++) {
      edge_users[eidx] = INVALID_UNUSED;
    }

    for (i = 0, mp = orig_mpoly; i < polys_num; i++, mp++) {
      MLoop *ml_prev;
      int j;

      ml = orig_mloop + mp->loopstart;
      ml_prev = ml + (mp->totloop - 1);

      for (j = 0; j < mp->totloop; j++, ml++) {
        /* add edge user */
        eidx = ml_prev->e;
        if (edge_users[eidx] == INVALID_UNUSED) {
          ed = orig_medge + eidx;
          BLI_assert(ELEM(ml_prev->v, ed->v1, ed->v2) && ELEM(ml->v, ed->v1, ed->v2));
          edge_users[eidx] = (ml_prev->v > ml->v) == (ed->v1 < ed->v2) ? i : (i + polys_num);
          edge_order[eidx] = j;
        }
        else {
          edge_users[eidx] = INVALID_PAIR;
        }
        ml_prev = ml;
      }
    }

    for (eidx = 0, ed = orig_medge; eidx < edges_num; eidx++, ed++) {
      if (!ELEM(edge_users[eidx], INVALID_UNUSED, INVALID_PAIR)) {
        BLI_BITMAP_ENABLE(orig_mvert_tag, ed->v1);
        BLI_BITMAP_ENABLE(orig_mvert_tag, ed->v2);
        STACK_PUSH(new_edge_arr, eidx);
        newPolys++;
        newLoops += 4;
      }
    }

    for (i = 0; i < verts_num; i++) {
      if (BLI_BITMAP_TEST(orig_mvert_tag, i)) {
        old_vert_arr[i] = STACK_SIZE(new_vert_arr);
        STACK_PUSH(new_vert_arr, i);
        rimVerts++;
      }
      else {
        old_vert_arr[i] = INVALID_UNUSED;
      }
    }

    MEM_freeN(orig_mvert_tag);
  }

  if (do_shell == false) {
    /* only add rim vertices */
    newVerts = rimVerts;
    /* each extruded face needs an opposite edge */
    newEdges = newPolys;
  }
  else {
    /* (stride == 2) in this case, so no need to add newVerts/newEdges */
    BLI_assert(newVerts == 0);
    BLI_assert(newEdges == 0);
  }

#ifdef USE_NONMANIFOLD_WORKAROUND
  BLI_bitmap *edge_tmp_tag = BLI_BITMAP_NEW(mesh->totedge, __func__);
#endif

  if (smd->flag & MOD_SOLIDIFY_NORMAL_CALC) {
    vert_nors = MEM_calloc_arrayN(verts_num, sizeof(float[3]), "mod_solid_vno_hq");
    mesh_calc_hq_normal(mesh,
                        poly_nors,
                        vert_nors
#ifdef USE_NONMANIFOLD_WORKAROUND
                        ,
                        edge_tmp_tag
#endif
    );
  }

  result = BKE_mesh_new_nomain_from_template(mesh,
                                             (int)((verts_num * stride) + newVerts),
                                             (int)((edges_num * stride) + newEdges + rimVerts),
                                             0,
                                             (int)((loops_num * stride) + newLoops),
                                             (int)((polys_num * stride) + newPolys));

  mpoly = result->mpoly;
  mloop = result->mloop;
  medge = result->medge;
  mvert = result->mvert;

  if (do_bevel_convex) {
    /* Make sure bweight is enabled. */
    result->cd_flag |= ME_CDFLAG_EDGE_BWEIGHT;
  }

  if (do_shell) {
    CustomData_copy_data(&mesh->vdata, &result->vdata, 0, 0, (int)verts_num);
    CustomData_copy_data(&mesh->vdata, &result->vdata, 0, (int)verts_num, (int)verts_num);

    CustomData_copy_data(&mesh->edata, &result->edata, 0, 0, (int)edges_num);
    CustomData_copy_data(&mesh->edata, &result->edata, 0, (int)edges_num, (int)edges_num);

    CustomData_copy_data(&mesh->ldata, &result->ldata, 0, 0, (int)loops_num);
    /* DO NOT copy here the 'copied' part of loop data, we want to reverse loops
     * (so that winding of copied face get reversed, so that normals get reversed
     * and point in expected direction...).
     * If we also copy data here, then this data get overwritten
     * (and allocated memory becomes memleak). */

    CustomData_copy_data(&mesh->pdata, &result->pdata, 0, 0, (int)polys_num);
    CustomData_copy_data(&mesh->pdata, &result->pdata, 0, (int)polys_num, (int)polys_num);
  }
  else {
    int i, j;
    CustomData_copy_data(&mesh->vdata, &result->vdata, 0, 0, (int)verts_num);
    for (i = 0, j = (int)verts_num; i < verts_num; i++) {
      if (old_vert_arr[i] != INVALID_UNUSED) {
        CustomData_copy_data(&mesh->vdata, &result->vdata, i, j, 1);
        j++;
      }
    }

    CustomData_copy_data(&mesh->edata, &result->edata, 0, 0, (int)edges_num);

    for (i = 0, j = (int)edges_num; i < edges_num; i++) {
      if (!ELEM(edge_users[i], INVALID_UNUSED, INVALID_PAIR)) {
        MEdge *ed_src, *ed_dst;
        CustomData_copy_data(&mesh->edata, &result->edata, i, j, 1);

        ed_src = &medge[i];
        ed_dst = &medge[j];
        ed_dst->v1 = old_vert_arr[ed_src->v1] + verts_num;
        ed_dst->v2 = old_vert_arr[ed_src->v2] + verts_num;
        j++;
      }
    }

    /* will be created later */
    CustomData_copy_data(&mesh->ldata, &result->ldata, 0, 0, (int)loops_num);
    CustomData_copy_data(&mesh->pdata, &result->pdata, 0, 0, (int)polys_num);
  }

  /* initializes: (i_end, do_shell_align, mv). */
#define INIT_VERT_ARRAY_OFFSETS(test) \
  if (((ofs_new >= ofs_orig) == do_flip) == test) { \
    i_end = verts_num; \
    do_shell_align = true; \
    mv = mvert; \
  } \
  else { \
    if (do_shell) { \
      i_end = verts_num; \
      do_shell_align = true; \
    } \
    else { \
      i_end = newVerts; \
      do_shell_align = false; \
    } \
    mv = &mvert[verts_num]; \
  } \
  (void)0

  /* flip normals */

  if (do_shell) {
    uint i;

    mp = mpoly + polys_num;
    for (i = 0; i < mesh->totpoly; i++, mp++) {
      const int loop_end = mp->totloop - 1;
      MLoop *ml2;
      uint e;
      int j;

      /* reverses the loop direction (MLoop.v as well as custom-data)
       * MLoop.e also needs to be corrected too, done in a separate loop below. */
      ml2 = mloop + mp->loopstart + mesh->totloop;
#if 0
      for (j = 0; j < mp->totloop; j++) {
        CustomData_copy_data(&mesh->ldata,
                             &result->ldata,
                             mp->loopstart + j,
                             mp->loopstart + (loop_end - j) + mesh->totloop,
                             1);
      }
#else
      /* slightly more involved, keep the first vertex the same for the copy,
       * ensures the diagonals in the new face match the original. */
      j = 0;
      for (int j_prev = loop_end; j < mp->totloop; j_prev = j++) {
        CustomData_copy_data(&mesh->ldata,
                             &result->ldata,
                             mp->loopstart + j,
                             mp->loopstart + (loop_end - j_prev) + mesh->totloop,
                             1);
      }
#endif

      if (mat_ofs) {
        mp->mat_nr += mat_ofs;
        CLAMP(mp->mat_nr, 0, mat_nr_max);
      }

      e = ml2[0].e;
      for (j = 0; j < loop_end; j++) {
        ml2[j].e = ml2[j + 1].e;
      }
      ml2[loop_end].e = e;

      mp->loopstart += mesh->totloop;

      for (j = 0; j < mp->totloop; j++) {
        ml2[j].e += edges_num;
        ml2[j].v += verts_num;
      }
    }

    for (i = 0, ed = medge + edges_num; i < edges_num; i++, ed++) {
      ed->v1 += verts_num;
      ed->v2 += verts_num;
    }
  }

  /* NOTE: copied vertex layers don't have flipped normals yet. do this after applying offset. */
  if ((smd->flag & MOD_SOLIDIFY_EVEN) == 0) {
    /* no even thickness, very simple */
    float ofs_new_vgroup;

    /* for clamping */
    float *vert_lens = NULL;
    float *vert_angs = NULL;
    const float offset = fabsf(smd->offset) * smd->offset_clamp;
    const float offset_sq = offset * offset;

    /* for bevel weight */
    float *edge_angs = NULL;

    if (do_clamp) {
      vert_lens = MEM_malloc_arrayN(verts_num, sizeof(float), "vert_lens");
      copy_vn_fl(vert_lens, (int)verts_num, FLT_MAX);
      for (uint i = 0; i < edges_num; i++) {
        const float ed_len_sq = len_squared_v3v3(mvert[medge[i].v1].co, mvert[medge[i].v2].co);
        vert_lens[medge[i].v1] = min_ff(vert_lens[medge[i].v1], ed_len_sq);
        vert_lens[medge[i].v2] = min_ff(vert_lens[medge[i].v2], ed_len_sq);
      }
    }

    if (do_angle_clamp || do_bevel_convex) {
      uint eidx;
      if (do_angle_clamp) {
        vert_angs = MEM_malloc_arrayN(verts_num, sizeof(float), "vert_angs");
        copy_vn_fl(vert_angs, (int)verts_num, 0.5f * M_PI);
      }
      if (do_bevel_convex) {
        edge_angs = MEM_malloc_arrayN(edges_num, sizeof(float), "edge_angs");
        if (!do_rim) {
          edge_users = MEM_malloc_arrayN(edges_num, sizeof(*edge_users), "solid_mod edges");
        }
      }
      uint(*edge_user_pairs)[2] = MEM_malloc_arrayN(
          edges_num, sizeof(*edge_user_pairs), "edge_user_pairs");
      for (eidx = 0; eidx < edges_num; eidx++) {
        edge_user_pairs[eidx][0] = INVALID_UNUSED;
        edge_user_pairs[eidx][1] = INVALID_UNUSED;
      }
      mp = orig_mpoly;
      for (uint i = 0; i < polys_num; i++, mp++) {
        ml = orig_mloop + mp->loopstart;
        MLoop *ml_prev = ml + (mp->totloop - 1);

        for (uint j = 0; j < mp->totloop; j++, ml++) {
          /* add edge user */
          eidx = ml_prev->e;
          ed = orig_medge + eidx;
          BLI_assert(ELEM(ml_prev->v, ed->v1, ed->v2) && ELEM(ml->v, ed->v1, ed->v2));
          char flip = (char)((ml_prev->v > ml->v) == (ed->v1 < ed->v2));
          if (edge_user_pairs[eidx][flip] == INVALID_UNUSED) {
            edge_user_pairs[eidx][flip] = i;
          }
          else {
            edge_user_pairs[eidx][0] = INVALID_PAIR;
            edge_user_pairs[eidx][1] = INVALID_PAIR;
          }
          ml_prev = ml;
        }
      }
      ed = orig_medge;
      float e[3];
      for (uint i = 0; i < edges_num; i++, ed++) {
        if (!ELEM(edge_user_pairs[i][0], INVALID_UNUSED, INVALID_PAIR) &&
            !ELEM(edge_user_pairs[i][1], INVALID_UNUSED, INVALID_PAIR)) {
          const float *n0 = poly_nors[edge_user_pairs[i][0]];
          const float *n1 = poly_nors[edge_user_pairs[i][1]];
          sub_v3_v3v3(e, orig_mvert[ed->v1].co, orig_mvert[ed->v2].co);
          normalize_v3(e);
          const float angle = angle_signed_on_axis_v3v3_v3(n0, n1, e);
          if (do_angle_clamp) {
            vert_angs[ed->v1] = max_ff(vert_angs[ed->v1], angle);
            vert_angs[ed->v2] = max_ff(vert_angs[ed->v2], angle);
          }
          if (do_bevel_convex) {
            edge_angs[i] = angle;
            if (!do_rim) {
              edge_users[i] = INVALID_PAIR;
            }
          }
        }
      }
      MEM_freeN(edge_user_pairs);
    }

    if (ofs_new != 0.0f) {
      uint i_orig, i_end;
      bool do_shell_align;

      ofs_new_vgroup = ofs_new;

      INIT_VERT_ARRAY_OFFSETS(false);

      for (i_orig = 0; i_orig < i_end; i_orig++, mv++) {
        const uint i = do_shell_align ? i_orig : new_vert_arr[i_orig];
        if (dvert) {
          MDeformVert *dv = &dvert[i];
          if (defgrp_invert) {
            ofs_new_vgroup = 1.0f - BKE_defvert_find_weight(dv, defgrp_index);
          }
          else {
            ofs_new_vgroup = BKE_defvert_find_weight(dv, defgrp_index);
          }
          ofs_new_vgroup = (offset_fac_vg + (ofs_new_vgroup * offset_fac_vg_inv)) * ofs_new;
        }
        if (do_clamp && offset > FLT_EPSILON) {
          /* always reset because we may have set before */
          if (dvert == NULL) {
            ofs_new_vgroup = ofs_new;
          }
          if (do_angle_clamp) {
            float cos_ang = cosf(((2 * M_PI) - vert_angs[i]) * 0.5f);
            if (cos_ang > 0) {
              float max_off = sqrtf(vert_lens[i]) * 0.5f / cos_ang;
              if (max_off < offset * 0.5f) {
                ofs_new_vgroup *= max_off / offset * 2;
              }
            }
          }
          else {
            if (vert_lens[i] < offset_sq) {
              float scalar = sqrtf(vert_lens[i]) / offset;
              ofs_new_vgroup *= scalar;
            }
          }
        }
        if (vert_nors) {
          madd_v3_v3fl(mv->co, vert_nors[i], ofs_new_vgroup);
        }
        else {
          madd_v3_v3fl(mv->co, mesh_vert_normals[i], ofs_new_vgroup);
        }
      }
    }

    if (ofs_orig != 0.0f) {
      uint i_orig, i_end;
      bool do_shell_align;

      ofs_new_vgroup = ofs_orig;

      /* as above but swapped */
      INIT_VERT_ARRAY_OFFSETS(true);

      for (i_orig = 0; i_orig < i_end; i_orig++, mv++) {
        const uint i = do_shell_align ? i_orig : new_vert_arr[i_orig];
        if (dvert) {
          MDeformVert *dv = &dvert[i];
          if (defgrp_invert) {
            ofs_new_vgroup = 1.0f - BKE_defvert_find_weight(dv, defgrp_index);
          }
          else {
            ofs_new_vgroup = BKE_defvert_find_weight(dv, defgrp_index);
          }
          ofs_new_vgroup = (offset_fac_vg + (ofs_new_vgroup * offset_fac_vg_inv)) * ofs_orig;
        }
        if (do_clamp && offset > FLT_EPSILON) {
          /* always reset because we may have set before */
          if (dvert == NULL) {
            ofs_new_vgroup = ofs_orig;
          }
          if (do_angle_clamp) {
            float cos_ang = cosf(vert_angs[i_orig] * 0.5f);
            if (cos_ang > 0) {
              float max_off = sqrtf(vert_lens[i]) * 0.5f / cos_ang;
              if (max_off < offset * 0.5f) {
                ofs_new_vgroup *= max_off / offset * 2;
              }
            }
          }
          else {
            if (vert_lens[i] < offset_sq) {
              float scalar = sqrtf(vert_lens[i]) / offset;
              ofs_new_vgroup *= scalar;
            }
          }
        }
        if (vert_nors) {
          madd_v3_v3fl(mv->co, vert_nors[i], ofs_new_vgroup);
        }
        else {
          madd_v3_v3fl(mv->co, mesh_vert_normals[i], ofs_new_vgroup);
        }
      }
    }

    if (do_bevel_convex) {
      for (uint i = 0; i < edges_num; i++) {
        if (edge_users[i] == INVALID_PAIR) {
          float angle = edge_angs[i];
          medge[i].bweight = (char)clamp_i(
              (int)medge[i].bweight + (int)((angle < M_PI ? clamp_f(bevel_convex, 0.0f, 1.0f) :
                                                            clamp_f(bevel_convex, -1.0f, 0.0f)) *
                                            255),
              0,
              255);
          if (do_shell) {
            medge[i + edges_num].bweight = (char)clamp_i(
                (int)medge[i + edges_num].bweight +
                    (int)((angle > M_PI ? clamp_f(bevel_convex, 0.0f, 1.0f) :
                                          clamp_f(bevel_convex, -1.0f, 0.0f)) *
                          255),
                0,
                255);
          }
        }
      }
      if (!do_rim) {
        MEM_freeN(edge_users);
      }
      MEM_freeN(edge_angs);
    }

    if (do_clamp) {
      MEM_freeN(vert_lens);
      if (do_angle_clamp) {
        MEM_freeN(vert_angs);
      }
    }
  }
  else {
#ifdef USE_NONMANIFOLD_WORKAROUND
    const bool check_non_manifold = (smd->flag & MOD_SOLIDIFY_NORMAL_CALC) != 0;
#endif
    /* same as EM_solidify() in editmesh_lib.c */
    float *vert_angles = MEM_calloc_arrayN(
        verts_num, sizeof(float[2]), "mod_solid_pair"); /* 2 in 1 */
    float *vert_accum = vert_angles + verts_num;
    uint vidx;
    uint i;

    if (vert_nors == NULL) {
      vert_nors = MEM_malloc_arrayN(verts_num, sizeof(float[3]), "mod_solid_vno");
      for (i = 0, mv = mvert; i < verts_num; i++, mv++) {
        copy_v3_v3(vert_nors[i], mesh_vert_normals[i]);
      }
    }

    for (i = 0, mp = mpoly; i < polys_num; i++, mp++) {
      /* #BKE_mesh_calc_poly_angles logic is inlined here */
      float nor_prev[3];
      float nor_next[3];

      int i_curr = mp->totloop - 1;
      int i_next = 0;

      ml = &mloop[mp->loopstart];

      sub_v3_v3v3(nor_prev, mvert[ml[i_curr - 1].v].co, mvert[ml[i_curr].v].co);
      normalize_v3(nor_prev);

      while (i_next < mp->totloop) {
        float angle;
        sub_v3_v3v3(nor_next, mvert[ml[i_curr].v].co, mvert[ml[i_next].v].co);
        normalize_v3(nor_next);
        angle = angle_normalized_v3v3(nor_prev, nor_next);

        /* --- not related to angle calc --- */
        if (angle < FLT_EPSILON) {
          angle = FLT_EPSILON;
        }

        vidx = ml[i_curr].v;
        vert_accum[vidx] += angle;

#ifdef USE_NONMANIFOLD_WORKAROUND
        /* skip 3+ face user edges */
        if ((check_non_manifold == false) ||
            LIKELY(!BLI_BITMAP_TEST(edge_tmp_tag, ml[i_curr].e) &&
                   !BLI_BITMAP_TEST(edge_tmp_tag, ml[i_next].e))) {
          vert_angles[vidx] += shell_v3v3_normalized_to_dist(vert_nors[vidx], poly_nors[i]) *
                               angle;
        }
        else {
          vert_angles[vidx] += angle;
        }
#else
        vert_angles[vidx] += shell_v3v3_normalized_to_dist(vert_nors[vidx], poly_nors[i]) * angle;
#endif
        /* --- end non-angle-calc section --- */

        /* step */
        copy_v3_v3(nor_prev, nor_next);
        i_curr = i_next;
        i_next++;
      }
    }

    /* vertex group support */
    if (dvert) {
      MDeformVert *dv = dvert;
      float scalar;

      if (defgrp_invert) {
        for (i = 0; i < verts_num; i++, dv++) {
          scalar = 1.0f - BKE_defvert_find_weight(dv, defgrp_index);
          scalar = offset_fac_vg + (scalar * offset_fac_vg_inv);
          vert_angles[i] *= scalar;
        }
      }
      else {
        for (i = 0; i < verts_num; i++, dv++) {
          scalar = BKE_defvert_find_weight(dv, defgrp_index);
          scalar = offset_fac_vg + (scalar * offset_fac_vg_inv);
          vert_angles[i] *= scalar;
        }
      }
    }

    /* for angle clamp */
    float *vert_angs = NULL;
    /* for bevel convex */
    float *edge_angs = NULL;

    if (do_angle_clamp || do_bevel_convex) {
      uint eidx;
      if (do_angle_clamp) {
        vert_angs = MEM_malloc_arrayN(verts_num, sizeof(float), "vert_angs even");
        copy_vn_fl(vert_angs, (int)verts_num, 0.5f * M_PI);
      }
      if (do_bevel_convex) {
        edge_angs = MEM_malloc_arrayN(edges_num, sizeof(float), "edge_angs even");
        if (!do_rim) {
          edge_users = MEM_malloc_arrayN(edges_num, sizeof(*edge_users), "solid_mod edges");
        }
      }
      uint(*edge_user_pairs)[2] = MEM_malloc_arrayN(
          edges_num, sizeof(*edge_user_pairs), "edge_user_pairs");
      for (eidx = 0; eidx < edges_num; eidx++) {
        edge_user_pairs[eidx][0] = INVALID_UNUSED;
        edge_user_pairs[eidx][1] = INVALID_UNUSED;
      }
      for (i = 0, mp = orig_mpoly; i < polys_num; i++, mp++) {
        ml = orig_mloop + mp->loopstart;
        MLoop *ml_prev = ml + (mp->totloop - 1);

        for (int j = 0; j < mp->totloop; j++, ml++) {
          /* add edge user */
          eidx = ml_prev->e;
          ed = orig_medge + eidx;
          BLI_assert(ELEM(ml_prev->v, ed->v1, ed->v2) && ELEM(ml->v, ed->v1, ed->v2));
          char flip = (char)((ml_prev->v > ml->v) == (ed->v1 < ed->v2));
          if (edge_user_pairs[eidx][flip] == INVALID_UNUSED) {
            edge_user_pairs[eidx][flip] = i;
          }
          else {
            edge_user_pairs[eidx][0] = INVALID_PAIR;
            edge_user_pairs[eidx][1] = INVALID_PAIR;
          }
          ml_prev = ml;
        }
      }
      ed = orig_medge;
      float e[3];
      for (i = 0; i < edges_num; i++, ed++) {
        if (!ELEM(edge_user_pairs[i][0], INVALID_UNUSED, INVALID_PAIR) &&
            !ELEM(edge_user_pairs[i][1], INVALID_UNUSED, INVALID_PAIR)) {
          const float *n0 = poly_nors[edge_user_pairs[i][0]];
          const float *n1 = poly_nors[edge_user_pairs[i][1]];
          if (do_angle_clamp) {
            const float angle = M_PI - angle_normalized_v3v3(n0, n1);
            vert_angs[ed->v1] = max_ff(vert_angs[ed->v1], angle);
            vert_angs[ed->v2] = max_ff(vert_angs[ed->v2], angle);
          }
          if (do_bevel_convex) {
            sub_v3_v3v3(e, orig_mvert[ed->v1].co, orig_mvert[ed->v2].co);
            normalize_v3(e);
            edge_angs[i] = angle_signed_on_axis_v3v3_v3(n0, n1, e);
            if (!do_rim) {
              edge_users[i] = INVALID_PAIR;
            }
          }
        }
      }
      MEM_freeN(edge_user_pairs);
    }

    if (do_clamp) {
      const float clamp_fac = 1 + (do_angle_clamp ? fabsf(smd->offset_fac) : 0);
      const float offset = fabsf(smd->offset) * smd->offset_clamp * clamp_fac;
      if (offset > FLT_EPSILON) {
        float *vert_lens_sq = MEM_malloc_arrayN(verts_num, sizeof(float), "vert_lens_sq");
        const float offset_sq = offset * offset;
        copy_vn_fl(vert_lens_sq, (int)verts_num, FLT_MAX);
        for (i = 0; i < edges_num; i++) {
          const float ed_len = len_squared_v3v3(mvert[medge[i].v1].co, mvert[medge[i].v2].co);
          vert_lens_sq[medge[i].v1] = min_ff(vert_lens_sq[medge[i].v1], ed_len);
          vert_lens_sq[medge[i].v2] = min_ff(vert_lens_sq[medge[i].v2], ed_len);
        }
        if (do_angle_clamp) {
          for (i = 0; i < verts_num; i++) {
            float cos_ang = cosf(vert_angs[i] * 0.5f);
            if (cos_ang > 0) {
              float max_off = sqrtf(vert_lens_sq[i]) * 0.5f / cos_ang;
              if (max_off < offset * 0.5f) {
                vert_angles[i] *= max_off / offset * 2;
              }
            }
          }
          MEM_freeN(vert_angs);
        }
        else {
          for (i = 0; i < verts_num; i++) {
            if (vert_lens_sq[i] < offset_sq) {
              float scalar = sqrtf(vert_lens_sq[i]) / offset;
              vert_angles[i] *= scalar;
            }
          }
        }
        MEM_freeN(vert_lens_sq);
      }
    }

    if (do_bevel_convex) {
      for (i = 0; i < edges_num; i++) {
        if (edge_users[i] == INVALID_PAIR) {
          float angle = edge_angs[i];
          medge[i].bweight = (char)clamp_i(
              (int)medge[i].bweight + (int)((angle < M_PI ? clamp_f(bevel_convex, 0, 1) :
                                                            clamp_f(bevel_convex, -1, 0)) *
                                            255),
              0,
              255);
          if (do_shell) {
            medge[i + edges_num].bweight = (char)clamp_i(
                (int)medge[i + edges_num].bweight +
                    (int)((angle > M_PI ? clamp_f(bevel_convex, 0, 1) :
                                          clamp_f(bevel_convex, -1, 0)) *
                          255),
                0,
                255);
          }
        }
      }
      if (!do_rim) {
        MEM_freeN(edge_users);
      }
      MEM_freeN(edge_angs);
    }

#undef INVALID_UNUSED
#undef INVALID_PAIR

    if (ofs_new != 0.0f) {
      uint i_orig, i_end;
      bool do_shell_align;

      INIT_VERT_ARRAY_OFFSETS(false);

      for (i_orig = 0; i_orig < i_end; i_orig++, mv++) {
        const uint i_other = do_shell_align ? i_orig : new_vert_arr[i_orig];
        if (vert_accum[i_other]) { /* zero if unselected */
          madd_v3_v3fl(
              mv->co, vert_nors[i_other], ofs_new * (vert_angles[i_other] / vert_accum[i_other]));
        }
      }
    }

    if (ofs_orig != 0.0f) {
      uint i_orig, i_end;
      bool do_shell_align;

      /* same as above but swapped, intentional use of 'ofs_new' */
      INIT_VERT_ARRAY_OFFSETS(true);

      for (i_orig = 0; i_orig < i_end; i_orig++, mv++) {
        const uint i_other = do_shell_align ? i_orig : new_vert_arr[i_orig];
        if (vert_accum[i_other]) { /* zero if unselected */
          madd_v3_v3fl(
              mv->co, vert_nors[i_other], ofs_orig * (vert_angles[i_other] / vert_accum[i_other]));
        }
      }
    }

    MEM_freeN(vert_angles);
  }

#ifdef USE_NONMANIFOLD_WORKAROUND
  MEM_SAFE_FREE(edge_tmp_tag);
#endif

  if (vert_nors) {
    MEM_freeN(vert_nors);
  }

  /* must recalculate normals with vgroups since they can displace unevenly T26888. */
  if (BKE_mesh_vertex_normals_are_dirty(mesh) || do_rim || dvert) {
    BKE_mesh_normals_tag_dirty(result);
  }
  else if (do_shell) {
    uint i;
    /* flip vertex normals for copied verts */
    mv = mvert + verts_num;
    for (i = 0; i < verts_num; i++) {
      negate_v3((float *)mesh_vert_normals[i]);
    }
  }

  /* Add vertex weights for rim and shell vgroups. */
  if (shell_defgrp_index != -1 || rim_defgrp_index != -1) {
    dvert = CustomData_duplicate_referenced_layer(&result->vdata, CD_MDEFORMVERT, result->totvert);
    /* If no vertices were ever added to an object's vgroup, dvert might be NULL. */
    if (dvert == NULL) {
      /* Add a valid data layer! */
      dvert = CustomData_add_layer(
          &result->vdata, CD_MDEFORMVERT, CD_CALLOC, NULL, result->totvert);
    }
    /* Ultimate security check. */
    if (dvert != NULL) {
      result->dvert = dvert;

      if (rim_defgrp_index != -1) {
        for (uint i = 0; i < rimVerts; i++) {
          BKE_defvert_ensure_index(&result->dvert[new_vert_arr[i]], rim_defgrp_index)->weight =
              1.0f;
          BKE_defvert_ensure_index(&result->dvert[(do_shell ? new_vert_arr[i] : i) + verts_num],
                                   rim_defgrp_index)
              ->weight = 1.0f;
        }
      }

      if (shell_defgrp_index != -1) {
        for (uint i = verts_num; i < result->totvert; i++) {
          BKE_defvert_ensure_index(&result->dvert[i], shell_defgrp_index)->weight = 1.0f;
        }
      }
    }
  }
  if (do_rim) {
    uint i;

    /* NOTE(@campbellbarton): Unfortunately re-calculate the normals for the new edge
     * faces is necessary. This could be done in many ways, but probably the quickest
     * way is to calculate the average normals for side faces only.
     * Then blend them with the normals of the edge verts.
     *
     * At the moment its easiest to allocate an entire array for every vertex,
     * even though we only need edge verts. */

#define SOLIDIFY_SIDE_NORMALS

#ifdef SOLIDIFY_SIDE_NORMALS
    /* NOTE(@sybren): due to the code setting normals dirty a few lines above,
     * do_side_normals is always false. */
    const bool do_side_normals = !BKE_mesh_vertex_normals_are_dirty(result);
    /* annoying to allocate these since we only need the edge verts, */
    float(*edge_vert_nos)[3] = do_side_normals ?
                                   MEM_calloc_arrayN(verts_num, sizeof(float[3]), __func__) :
                                   NULL;
    float nor[3];
#endif
    const uchar crease_rim = smd->crease_rim * 255.0f;
    const uchar crease_outer = smd->crease_outer * 255.0f;
    const uchar crease_inner = smd->crease_inner * 255.0f;

    int *origindex_edge;
    int *orig_ed;
    uint j;

    if (crease_rim || crease_outer || crease_inner) {
      result->cd_flag |= ME_CDFLAG_EDGE_CREASE;
    }

    /* add faces & edges */
    origindex_edge = CustomData_get_layer(&result->edata, CD_ORIGINDEX);
    orig_ed = (origindex_edge) ? &origindex_edge[(edges_num * stride) + newEdges] : NULL;
    ed = &medge[(edges_num * stride) + newEdges]; /* start after copied edges */
    for (i = 0; i < rimVerts; i++, ed++) {
      ed->v1 = new_vert_arr[i];
      ed->v2 = (do_shell ? new_vert_arr[i] : i) + verts_num;
      ed->flag |= ME_EDGEDRAW | ME_EDGERENDER;

      if (orig_ed) {
        *orig_ed = ORIGINDEX_NONE;
        orig_ed++;
      }

      if (crease_rim) {
        ed->crease = crease_rim;
      }
    }

    /* faces */
    mp = mpoly + (polys_num * stride);
    ml = mloop + (loops_num * stride);
    j = 0;
    for (i = 0; i < newPolys; i++, mp++) {
      uint eidx = new_edge_arr[i];
      uint pidx = edge_users[eidx];
      int k1, k2;
      bool flip;

      if (pidx >= polys_num) {
        pidx -= polys_num;
        flip = true;
      }
      else {
        flip = false;
      }

      ed = medge + eidx;

      /* copy most of the face settings */
      CustomData_copy_data(
          &mesh->pdata, &result->pdata, (int)pidx, (int)((polys_num * stride) + i), 1);
      mp->loopstart = (int)(j + (loops_num * stride));
      mp->flag = mpoly[pidx].flag;

      /* notice we use 'mp->totloop' which is later overwritten,
       * we could lookup the original face but there's no point since this is a copy
       * and will have the same value, just take care when changing order of assignment */

      /* prev loop */
      k1 = mpoly[pidx].loopstart + (((edge_order[eidx] - 1) + mp->totloop) % mp->totloop);

      k2 = mpoly[pidx].loopstart + (edge_order[eidx]);

      mp->totloop = 4;

      CustomData_copy_data(
          &mesh->ldata, &result->ldata, k2, (int)((loops_num * stride) + j + 0), 1);
      CustomData_copy_data(
          &mesh->ldata, &result->ldata, k1, (int)((loops_num * stride) + j + 1), 1);
      CustomData_copy_data(
          &mesh->ldata, &result->ldata, k1, (int)((loops_num * stride) + j + 2), 1);
      CustomData_copy_data(
          &mesh->ldata, &result->ldata, k2, (int)((loops_num * stride) + j + 3), 1);

      if (flip == false) {
        ml[j].v = ed->v1;
        ml[j++].e = eidx;

        ml[j].v = ed->v2;
        ml[j++].e = (edges_num * stride) + old_vert_arr[ed->v2] + newEdges;

        ml[j].v = (do_shell ? ed->v2 : old_vert_arr[ed->v2]) + verts_num;
        ml[j++].e = (do_shell ? eidx : i) + edges_num;

        ml[j].v = (do_shell ? ed->v1 : old_vert_arr[ed->v1]) + verts_num;
        ml[j++].e = (edges_num * stride) + old_vert_arr[ed->v1] + newEdges;
      }
      else {
        ml[j].v = ed->v2;
        ml[j++].e = eidx;

        ml[j].v = ed->v1;
        ml[j++].e = (edges_num * stride) + old_vert_arr[ed->v1] + newEdges;

        ml[j].v = (do_shell ? ed->v1 : old_vert_arr[ed->v1]) + verts_num;
        ml[j++].e = (do_shell ? eidx : i) + edges_num;

        ml[j].v = (do_shell ? ed->v2 : old_vert_arr[ed->v2]) + verts_num;
        ml[j++].e = (edges_num * stride) + old_vert_arr[ed->v2] + newEdges;
      }

      if (origindex_edge) {
        origindex_edge[ml[j - 3].e] = ORIGINDEX_NONE;
        origindex_edge[ml[j - 1].e] = ORIGINDEX_NONE;
      }

      /* use the next material index if option enabled */
      if (mat_ofs_rim) {
        mp->mat_nr += mat_ofs_rim;
        CLAMP(mp->mat_nr, 0, mat_nr_max);
      }
      if (crease_outer) {
        /* crease += crease_outer; without wrapping */
        char *cr = &(ed->crease);
        int tcr = *cr + crease_outer;
        *cr = tcr > 255 ? 255 : tcr;
      }

      if (crease_inner) {
        /* crease += crease_inner; without wrapping */
        char *cr = &(medge[edges_num + (do_shell ? eidx : i)].crease);
        int tcr = *cr + crease_inner;
        *cr = tcr > 255 ? 255 : tcr;
      }

#ifdef SOLIDIFY_SIDE_NORMALS
      if (do_side_normals) {
        normal_quad_v3(nor,
                       mvert[ml[j - 4].v].co,
                       mvert[ml[j - 3].v].co,
                       mvert[ml[j - 2].v].co,
                       mvert[ml[j - 1].v].co);

        add_v3_v3(edge_vert_nos[ed->v1], nor);
        add_v3_v3(edge_vert_nos[ed->v2], nor);
      }
#endif
    }

#ifdef SOLIDIFY_SIDE_NORMALS
    if (do_side_normals) {
      const MEdge *ed_orig = medge;
      ed = medge + (edges_num * stride);
      for (i = 0; i < rimVerts; i++, ed++, ed_orig++) {
        float nor_cpy[3];
        int k;

        /* NOTE: only the first vertex (lower half of the index) is calculated. */
        BLI_assert(ed->v1 < verts_num);
        normalize_v3_v3(nor_cpy, edge_vert_nos[ed_orig->v1]);

        for (k = 0; k < 2; k++) { /* loop over both verts of the edge */
          copy_v3_v3(nor, mesh_vert_normals[*(&ed->v1 + k)]);
          add_v3_v3(nor, nor_cpy);
          normalize_v3(nor);
          copy_v3_v3((float *)mesh_vert_normals[*(&ed->v1 + k)], nor);
        }
      }

      MEM_freeN(edge_vert_nos);
    }
#endif

    MEM_freeN(new_vert_arr);
    MEM_freeN(new_edge_arr);

    MEM_freeN(edge_users);
    MEM_freeN(edge_order);
  }

  if (old_vert_arr) {
    MEM_freeN(old_vert_arr);
  }

  return result;
}

#undef SOLIDIFY_SIDE_NORMALS

/** \} */
