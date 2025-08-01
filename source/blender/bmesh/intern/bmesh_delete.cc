/* SPDX-FileCopyrightText: 2007 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * BM remove functions.
 */

#include "bmesh.hh"

/* BMO functions */

/* -------------------------------------------------------------------- */
/** \name BMesh Operator Delete Functions
 * \{ */

/**
 * Called by operators to remove elements that they have marked for
 * removal.
 */
static void bmo_remove_tagged_faces(BMesh *bm, const short oflag)
{
  BMFace *f, *f_next;
  BMIter iter;

  BM_ITER_MESH_MUTABLE (f, f_next, &iter, bm, BM_FACES_OF_MESH) {
    if (BMO_face_flag_test(bm, f, oflag)) {
      BM_face_kill(bm, f);
    }
  }
}

static void bmo_remove_tagged_edges(BMesh *bm, const short oflag)
{
  BMEdge *e, *e_next;
  BMIter iter;

  BM_ITER_MESH_MUTABLE (e, e_next, &iter, bm, BM_EDGES_OF_MESH) {
    if (BMO_edge_flag_test(bm, e, oflag)) {
      BM_edge_kill(bm, e);
    }
  }
}

static void bmo_remove_tagged_verts(BMesh *bm, const short oflag)
{
  BMVert *v, *v_next;
  BMIter iter;

  BM_ITER_MESH_MUTABLE (v, v_next, &iter, bm, BM_VERTS_OF_MESH) {
    if (BMO_vert_flag_test(bm, v, oflag)) {
      BM_vert_kill(bm, v);
    }
  }
}

static void bmo_remove_tagged_verts_loose(BMesh *bm, const short oflag)
{
  BMVert *v, *v_next;
  BMIter iter;

  BM_ITER_MESH_MUTABLE (v, v_next, &iter, bm, BM_VERTS_OF_MESH) {
    if (BMO_vert_flag_test(bm, v, oflag) && (v->e == nullptr)) {
      BM_vert_kill(bm, v);
    }
  }
}

void BMO_mesh_delete_oflag_tagged(BMesh *bm, const short oflag, const char htype)
{
  if (htype & BM_FACE) {
    bmo_remove_tagged_faces(bm, oflag);
  }
  if (htype & BM_EDGE) {
    bmo_remove_tagged_edges(bm, oflag);
  }
  if (htype & BM_VERT) {
    bmo_remove_tagged_verts(bm, oflag);
  }
}

void BMO_mesh_delete_oflag_context(BMesh *bm,
                                   const short oflag,
                                   const int type,
                                   blender::FunctionRef<void()> prepare_fn)
{
  BMEdge *e;

  BMIter eiter;
  BMIter fiter;

  switch (type) {
    case DEL_VERTS: {
      if (prepare_fn) {
        prepare_fn();
      }
      bmo_remove_tagged_verts(bm, oflag);
      break;
    }
    case DEL_EDGES: {
      /* flush down to vert */
      BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
        if (BMO_edge_flag_test(bm, e, oflag)) {
          BMO_vert_flag_enable(bm, e->v1, oflag);
          BMO_vert_flag_enable(bm, e->v2, oflag);
        }
      }
      if (prepare_fn) {
        prepare_fn();
      }
      bmo_remove_tagged_edges(bm, oflag);
      bmo_remove_tagged_verts_loose(bm, oflag);
      break;
    }
    case DEL_EDGESFACES: {
      if (prepare_fn) {
        prepare_fn();
      }
      bmo_remove_tagged_edges(bm, oflag);
      break;
    }
    case DEL_ONLYFACES: {
      if (prepare_fn) {
        prepare_fn();
      }
      bmo_remove_tagged_faces(bm, oflag);
      break;
    }
    case DEL_ONLYTAGGED: {
      if (prepare_fn) {
        prepare_fn();
      }
      BMO_mesh_delete_oflag_tagged(bm, oflag, BM_ALL_NOLOOP);
      break;
    }
    case DEL_FACES:
    case DEL_FACES_KEEP_BOUNDARY: {
      /* go through and mark all edges and all verts of all faces for delete */
      BMFace *f;
      BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
        if (BMO_face_flag_test(bm, f, oflag)) {
          BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
          BMLoop *l_iter;

          l_iter = l_first;
          do {
            BMO_vert_flag_enable(bm, l_iter->v, oflag);
            BMO_edge_flag_enable(bm, l_iter->e, oflag);
          } while ((l_iter = l_iter->next) != l_first);
        }
      }
      /* now go through and mark all remaining faces all edges for keeping */
      BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
        if (!BMO_face_flag_test(bm, f, oflag)) {
          BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
          BMLoop *l_iter;

          l_iter = l_first;
          do {
            BMO_vert_flag_disable(bm, l_iter->v, oflag);
            BMO_edge_flag_disable(bm, l_iter->e, oflag);
          } while ((l_iter = l_iter->next) != l_first);
        }
        /* now go through and mark all remaining faces all edges for keeping */
        BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
          if (!BMO_face_flag_test(bm, f, oflag)) {
            BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
            BMLoop *l_iter;

            l_iter = l_first;
            do {
              BMO_vert_flag_disable(bm, l_iter->v, oflag);
              BMO_edge_flag_disable(bm, l_iter->e, oflag);
            } while ((l_iter = l_iter->next) != l_first);
          }
        }
        /* also mark all the vertices of remaining edges for keeping */
        BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {

          /* Only exception to normal 'DEL_FACES' logic. */
          if (type == DEL_FACES_KEEP_BOUNDARY) {
            if (BM_edge_is_boundary(e)) {
              BMO_edge_flag_disable(bm, e, oflag);
            }
          }

          if (!BMO_edge_flag_test(bm, e, oflag)) {
            BMO_vert_flag_disable(bm, e->v1, oflag);
            BMO_vert_flag_disable(bm, e->v2, oflag);
          }
        }
      }

      if (prepare_fn) {
        prepare_fn();
      }

      /* now delete marked face */
      bmo_remove_tagged_faces(bm, oflag);
      /* delete marked edge */
      bmo_remove_tagged_edges(bm, oflag);
      /* remove loose vertices */
      bmo_remove_tagged_verts(bm, oflag);

      break;
    }
  }
}

/** \} */

/* BM functions
 *
 * NOTE: this is just a duplicate of the code above (bad!)
 * but for now keep in sync, its less hassle than having to create bmesh operator flags,
 * each time we need to remove some geometry.
 */

/* -------------------------------------------------------------------- */
/** \name BMesh Delete Functions (no oflags)
 * \{ */

static void bm_remove_tagged_faces(BMesh *bm, const char hflag)
{
  BMFace *f, *f_next;
  BMIter iter;

  BM_ITER_MESH_MUTABLE (f, f_next, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, hflag)) {
      BM_face_kill(bm, f);
    }
  }
}

static void bm_remove_tagged_edges(BMesh *bm, const char hflag)
{
  BMEdge *e, *e_next;
  BMIter iter;

  BM_ITER_MESH_MUTABLE (e, e_next, &iter, bm, BM_EDGES_OF_MESH) {
    if (BM_elem_flag_test(e, hflag)) {
      BM_edge_kill(bm, e);
    }
  }
}

static void bm_remove_tagged_verts(BMesh *bm, const char hflag)
{
  BMVert *v, *v_next;
  BMIter iter;

  BM_ITER_MESH_MUTABLE (v, v_next, &iter, bm, BM_VERTS_OF_MESH) {
    if (BM_elem_flag_test(v, hflag)) {
      BM_vert_kill(bm, v);
    }
  }
}

static void bm_remove_tagged_verts_loose(BMesh *bm, const char hflag)
{
  BMVert *v, *v_next;
  BMIter iter;

  BM_ITER_MESH_MUTABLE (v, v_next, &iter, bm, BM_VERTS_OF_MESH) {
    if (BM_elem_flag_test(v, hflag) && (v->e == nullptr)) {
      BM_vert_kill(bm, v);
    }
  }
}

void BM_mesh_delete_hflag_tagged(BMesh *bm, const char hflag, const char htype)
{
  if (htype & BM_FACE) {
    bm_remove_tagged_faces(bm, hflag);
  }
  if (htype & BM_EDGE) {
    bm_remove_tagged_edges(bm, hflag);
  }
  if (htype & BM_VERT) {
    bm_remove_tagged_verts(bm, hflag);
  }
}

void BM_mesh_delete_hflag_context(BMesh *bm, const char hflag, const int type)
{

  BMIter eiter;
  BMIter fiter;

  switch (type) {
    case DEL_VERTS: {
      bm_remove_tagged_verts(bm, hflag);

      break;
    }
    case DEL_EDGES: {
      /* flush down to vert */
      BMEdge *e;
      BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
        if (BM_elem_flag_test(e, hflag)) {
          BM_elem_flag_enable(e->v1, hflag);
          BM_elem_flag_enable(e->v2, hflag);
        }
      }
      bm_remove_tagged_edges(bm, hflag);
      bm_remove_tagged_verts_loose(bm, hflag);

      break;
    }
    case DEL_EDGESFACES: {
      bm_remove_tagged_edges(bm, hflag);

      break;
    }
    case DEL_ONLYFACES: {
      bm_remove_tagged_faces(bm, hflag);

      break;
    }
    case DEL_ONLYTAGGED: {
      BM_mesh_delete_hflag_tagged(bm, hflag, BM_ALL_NOLOOP);

      break;
    }
    case DEL_FACES: {
      /* go through and mark all edges and all verts of all faces for delete */
      BMFace *f;
      BMEdge *e;
      BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(f, hflag)) {
          BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
          BMLoop *l_iter;

          l_iter = l_first;
          do {
            BM_elem_flag_enable(l_iter->v, hflag);
            BM_elem_flag_enable(l_iter->e, hflag);
          } while ((l_iter = l_iter->next) != l_first);
        }
      }
      /* now go through and mark all remaining faces all edges for keeping */
      BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
        if (!BM_elem_flag_test(f, hflag)) {
          BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
          BMLoop *l_iter;

          l_iter = l_first;
          do {
            BM_elem_flag_disable(l_iter->v, hflag);
            BM_elem_flag_disable(l_iter->e, hflag);
          } while ((l_iter = l_iter->next) != l_first);
        }
      }
      /* also mark all the vertices of remaining edges for keeping */
      BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
        if (!BM_elem_flag_test(e, hflag)) {
          BM_elem_flag_disable(e->v1, hflag);
          BM_elem_flag_disable(e->v2, hflag);
        }
      }
      /* now delete marked face */
      bm_remove_tagged_faces(bm, hflag);
      /* delete marked edge */
      bm_remove_tagged_edges(bm, hflag);
      /* remove loose vertices */
      bm_remove_tagged_verts(bm, hflag);

      break;
    }
  }
}

/** \} */
