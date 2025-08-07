/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Edge-Net for filling in open edge-loops.
 */

#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_vector.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

#include "intern/bmesh_operators_private.hh" /* own include */

using blender::Vector;

#define EDGE_MARK 1
#define EDGE_VIS 2

#define ELE_NEW 1

void bmo_edgenet_fill_exec(BMesh *bm, BMOperator *op)
{
  BMOperator op_attr;
  BMOIter siter;
  BMFace *f;
  const short mat_nr = BMO_slot_int_get(op->slots_in, "mat_nr");
  const bool use_smooth = BMO_slot_bool_get(op->slots_in, "use_smooth");
  //  const int sides           = BMO_slot_int_get(op->slots_in,  "sides");

  if (!bm->totvert || !bm->totedge) {
    return;
  }

  BM_mesh_elem_hflag_disable_all(bm, BM_EDGE, BM_ELEM_TAG, false);
  BMO_slot_buffer_hflag_enable(bm, op->slots_in, "edges", BM_EDGE, BM_ELEM_TAG, false);

  BM_mesh_elem_hflag_disable_all(bm, BM_FACE, BM_ELEM_TAG, false);
  BM_mesh_edgenet(bm, true, true); /* TODO: sides. */

  BMO_slot_buffer_from_enabled_hflag(bm, op, op->slots_out, "faces.out", BM_FACE, BM_ELEM_TAG);

  BMO_ITER (f, &siter, op->slots_out, "faces.out", BM_FACE) {
    f->mat_nr = mat_nr;
    if (use_smooth) {
      BM_elem_flag_enable(f, BM_ELEM_SMOOTH);
    }
    /* Normals are zeroed. */
    BM_face_normal_update(f);
  }

  /* --- Attribute Fill --- */
  /* may as well since we have the faces already in a buffer */
  BMO_op_initf(bm,
               &op_attr,
               op->flag,
               "face_attribute_fill faces=%S use_normals=%b use_data=%b",
               op,
               "faces.out",
               true,
               true);

  BMO_op_exec(bm, &op_attr);

  /* check if some faces couldn't be touched */
  if (BMO_slot_buffer_len(op_attr.slots_out, "faces_fail.out")) {
    BMO_op_callf(bm, op->flag, "recalc_face_normals faces=%S", &op_attr, "faces_fail.out");
  }
  BMO_op_finish(bm, &op_attr);
}

static BMEdge *edge_next(BMesh *bm, BMEdge *e)
{
  BMIter iter;
  BMEdge *e2;
  int i;

  for (i = 0; i < 2; i++) {
    BM_ITER_ELEM (e2, &iter, i ? e->v2 : e->v1, BM_EDGES_OF_VERT) {
      if (BMO_edge_flag_test(bm, e2, EDGE_MARK) &&
          (BMO_edge_flag_test(bm, e2, EDGE_VIS) == false) && (e2 != e))
      {
        return e2;
      }
    }
  }

  return nullptr;
}

void bmo_edgenet_prepare_exec(BMesh *bm, BMOperator *op)
{
  BMOIter siter;
  BMEdge *e;
  bool ok = true;
  int i, count;

  BMO_slot_buffer_flag_enable(bm, op->slots_in, "edges", BM_EDGE, EDGE_MARK);

  /* validate that each edge has at most one other tagged edge in the
   * disk cycle around each of its vertices */
  BMO_ITER (e, &siter, op->slots_in, "edges", BM_EDGE) {
    for (i = 0; i < 2; i++) {
      count = BMO_iter_elem_count_flag(bm, BM_EDGES_OF_VERT, (i ? e->v2 : e->v1), EDGE_MARK, true);
      if (count > 2) {
        ok = false;
        break;
      }
    }

    if (!ok) {
      break;
    }
  }

  /* we don't have valid edge layouts, return */
  if (!ok) {
    return;
  }

  Vector<BMEdge *> edges1;
  Vector<BMEdge *> edges2;
  Vector<BMEdge *> *edges;

  /* find connected loops within the input edge */
  count = 0;
  while (true) {
    BMO_ITER (e, &siter, op->slots_in, "edges", BM_EDGE) {
      if (!BMO_edge_flag_test(bm, e, EDGE_VIS)) {
        if (BMO_iter_elem_count_flag(bm, BM_EDGES_OF_VERT, e->v1, EDGE_MARK, true) == 1 ||
            BMO_iter_elem_count_flag(bm, BM_EDGES_OF_VERT, e->v2, EDGE_MARK, true) == 1)
        {
          break;
        }
      }
    }

    if (!e) {
      break;
    }

    if (!count) {
      edges = &edges1;
    }
    else if (count == 1) {
      edges = &edges2;
    }
    else {
      break;
    }

    i = 0;
    while (e) {
      BMO_edge_flag_enable(bm, e, EDGE_VIS);
      edges->append(e);

      e = edge_next(bm, e);
      i++;
    }

    count++;
  }

  if (edges1.size() > 2 && BM_edge_share_vert_check(edges1.first(), edges1.last())) {
    if (edges2.size() > 2 && BM_edge_share_vert_check(edges2.first(), edges2.last())) {
      return;
    }
    edges1 = edges2;
    edges2.clear();
  }

  if (edges2.size() > 2 && BM_edge_share_vert_check(edges2.first(), edges2.last())) {
    edges2.clear();
  }

  /* two unconnected loops, connect the */
  if (!edges1.is_empty() && !edges2.is_empty()) {
    BMVert *v1, *v2, *v3, *v4;
    float dvec1[3];
    float dvec2[3];

    if (edges1.size() == 1) {
      v1 = edges1[0]->v1;
      v2 = edges1[0]->v2;
    }
    else {
      v1 = BM_vert_in_edge(edges1[1], edges1[0]->v1) ? edges1[0]->v2 : edges1[0]->v1;
      i = edges1.size() - 1;
      v2 = BM_vert_in_edge(edges1[i - 1], edges1[i]->v1) ? edges1[i]->v2 : edges1[i]->v1;
    }

    if (edges2.size() == 1) {
      v3 = edges2[0]->v1;
      v4 = edges2[0]->v2;
    }
    else {
      v3 = BM_vert_in_edge(edges2[1], edges2[0]->v1) ? edges2[0]->v2 : edges2[0]->v1;
      i = edges2.size() - 1;
      v4 = BM_vert_in_edge(edges2[i - 1], edges2[i]->v1) ? edges2[i]->v2 : edges2[i]->v1;
    }

    /* Avoid bow tie quads using most planar the triangle pair, see: #30367 & #143905. */
    normal_tri_v3(dvec1, v1->co, v2->co, v4->co);
    normal_tri_v3(dvec2, v1->co, v4->co, v3->co);
    const float dot_24 = dot_v3v3(dvec1, dvec2);

    normal_tri_v3(dvec1, v1->co, v2->co, v3->co);
    normal_tri_v3(dvec2, v1->co, v3->co, v4->co);
    const float dot_13 = dot_v3v3(dvec1, dvec2);
    if (dot_24 < dot_13) {
      std::swap(v3, v4);
    }

    e = BM_edge_create(bm, v1, v3, nullptr, BM_CREATE_NO_DOUBLE);
    BMO_edge_flag_enable(bm, e, ELE_NEW);
    e = BM_edge_create(bm, v2, v4, nullptr, BM_CREATE_NO_DOUBLE);
    BMO_edge_flag_enable(bm, e, ELE_NEW);
  }
  else if (!edges1.is_empty()) {
    BMVert *v1, *v2;

    if (edges1.size() > 1) {
      v1 = BM_vert_in_edge(edges1[1], edges1[0]->v1) ? edges1[0]->v2 : edges1[0]->v1;
      i = edges1.size() - 1;
      v2 = BM_vert_in_edge(edges1[i - 1], edges1[i]->v1) ? edges1[i]->v2 : edges1[i]->v1;
      e = BM_edge_create(bm, v1, v2, nullptr, BM_CREATE_NO_DOUBLE);
      BMO_edge_flag_enable(bm, e, ELE_NEW);
    }
  }

  BMO_slot_buffer_from_enabled_flag(bm, op, op->slots_out, "edges.out", BM_EDGE, ELE_NEW);
}
