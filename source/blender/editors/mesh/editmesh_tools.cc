/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 */

#include <cstddef>
#include <fmt/format.h>

#include "MEM_guardedalloc.h"

#include "DNA_key_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_array.hh"
#include "BLI_bitmap.h"
#include "BLI_heap_simple.h"
#include "BLI_linklist.h"
#include "BLI_linklist_stack.h"
#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_rand.h"
#include "BLI_sort_utils.h"

#include "BKE_attribute.h"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_deform.hh"
#include "BKE_editmesh.hh"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "BLT_translation.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_mesh.hh"
#include "ED_object.hh"
#include "ED_outliner.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_transform.hh"
#include "ED_uvedit.hh"
#include "ED_view3d.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "mesh_intern.hh" /* own include */

#include "bmesh_tools.hh"

using blender::Vector;

#define USE_FACE_CREATE_SEL_EXTEND

/* -------------------------------------------------------------------- */
/** \name Subdivide Operator
 * \{ */

static wmOperatorStatus edbm_subdivide_exec(bContext *C, wmOperator *op)
{
  const int cuts = RNA_int_get(op->ptr, "number_cuts");
  const float smooth = RNA_float_get(op->ptr, "smoothness");
  const float fractal = RNA_float_get(op->ptr, "fractal") / 2.5f;
  const float along_normal = RNA_float_get(op->ptr, "fractal_along_normal");
  const bool use_quad_tri = !RNA_boolean_get(op->ptr, "ngon");

  if (use_quad_tri && RNA_enum_get(op->ptr, "quadcorner") == SUBD_CORNER_STRAIGHT_CUT) {
    RNA_enum_set(op->ptr, "quadcorner", SUBD_CORNER_INNERVERT);
  }
  const int quad_corner_type = RNA_enum_get(op->ptr, "quadcorner");
  const int seed = RNA_int_get(op->ptr, "seed");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (!(em->bm->totedgesel || em->bm->totfacesel)) {
      continue;
    }

    BM_mesh_esubdivide(em->bm,
                       BM_ELEM_SELECT,
                       smooth,
                       SUBD_FALLOFF_LIN,
                       false,
                       fractal,
                       along_normal,
                       cuts,
                       SUBDIV_SELECT_ORIG,
                       quad_corner_type,
                       use_quad_tri,
                       true,
                       false,
                       seed);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

/* NOTE: these values must match delete_mesh() event values. */
static const EnumPropertyItem prop_mesh_cornervert_types[] = {
    {SUBD_CORNER_INNERVERT, "INNERVERT", 0, "Inner Vert", ""},
    {SUBD_CORNER_PATH, "PATH", 0, "Path", ""},
    {SUBD_CORNER_STRAIGHT_CUT, "STRAIGHT_CUT", 0, "Straight Cut", ""},
    {SUBD_CORNER_FAN, "FAN", 0, "Fan", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

void MESH_OT_subdivide(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Subdivide";
  ot->description = "Subdivide selected edges";
  ot->idname = "MESH_OT_subdivide";

  /* API callbacks. */
  ot->exec = edbm_subdivide_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_int(ot->srna, "number_cuts", 1, 1, 100, "Number of Cuts", "", 1, 10);
  /* avoid re-using last var because it can cause
   * _very_ high poly meshes and annoy users (or worse crash) */
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  RNA_def_float(
      ot->srna, "smoothness", 0.0f, 0.0f, 1e3f, "Smoothness", "Smoothness factor", 0.0f, 1.0f);

  WM_operatortype_props_advanced_begin(ot);

  RNA_def_boolean(ot->srna,
                  "ngon",
                  true,
                  "Create N-Gons",
                  "When disabled, newly created faces are limited to 3 and 4 sided faces");
  RNA_def_enum(
      ot->srna,
      "quadcorner",
      prop_mesh_cornervert_types,
      SUBD_CORNER_STRAIGHT_CUT,
      "Quad Corner Type",
      "How to subdivide quad corners (anything other than Straight Cut will prevent n-gons)");

  RNA_def_float(ot->srna,
                "fractal",
                0.0f,
                0.0f,
                1e6f,
                "Fractal",
                "Fractal randomness factor",
                0.0f,
                1000.0f);
  RNA_def_float(ot->srna,
                "fractal_along_normal",
                0.0f,
                0.0f,
                1.0f,
                "Along Normal",
                "Apply fractal displacement along normal only",
                0.0f,
                1.0f);
  RNA_def_int(ot->srna,
              "seed",
              0,
              0,
              INT_MAX,
              "Random Seed",
              "Seed for the random number generator",
              0,
              255);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge Ring Subdivide Operator
 *
 * Bridge code shares props.
 *
 * \{ */

struct EdgeRingOpSubdProps {
  int interp_mode;
  int cuts;
  float smooth;

  int profile_shape;
  float profile_shape_factor;
};

static void mesh_operator_edgering_props(wmOperatorType *ot,
                                         const int cuts_min,
                                         const int cuts_default)
{
  /* NOTE: these values must match delete_mesh() event values. */
  static const EnumPropertyItem prop_subd_edgering_types[] = {
      {SUBD_RING_INTERP_LINEAR, "LINEAR", 0, "Linear", ""},
      {SUBD_RING_INTERP_PATH, "PATH", 0, "Blend Path", ""},
      {SUBD_RING_INTERP_SURF, "SURFACE", 0, "Blend Surface", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  PropertyRNA *prop;

  prop = RNA_def_int(
      ot->srna, "number_cuts", cuts_default, 0, 1000, "Number of Cuts", "", cuts_min, 64);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  RNA_def_enum(ot->srna,
               "interpolation",
               prop_subd_edgering_types,
               SUBD_RING_INTERP_PATH,
               "Interpolation",
               "Interpolation method");

  RNA_def_float(
      ot->srna, "smoothness", 1.0f, 0.0f, 1e3f, "Smoothness", "Smoothness factor", 0.0f, 2.0f);

  /* profile-shape */
  RNA_def_float(ot->srna,
                "profile_shape_factor",
                0.0f,
                -1e3f,
                1e3f,
                "Profile Factor",
                "How much intermediary new edges are shrunk/expanded",
                -2.0f,
                2.0f);

  prop = RNA_def_property(ot->srna, "profile_shape", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_proportional_falloff_curve_only_items);
  RNA_def_property_enum_default(prop, PROP_SMOOTH);
  RNA_def_property_ui_text(prop, "Profile Shape", "Shape of the profile");
  RNA_def_property_translation_context(prop,
                                       BLT_I18NCONTEXT_ID_CURVE_LEGACY); /* Abusing id_curve :/ */
}

static void mesh_operator_edgering_props_get(wmOperator *op, EdgeRingOpSubdProps *op_props)
{
  op_props->interp_mode = RNA_enum_get(op->ptr, "interpolation");
  op_props->cuts = RNA_int_get(op->ptr, "number_cuts");
  op_props->smooth = RNA_float_get(op->ptr, "smoothness");

  op_props->profile_shape = RNA_enum_get(op->ptr, "profile_shape");
  op_props->profile_shape_factor = RNA_float_get(op->ptr, "profile_shape_factor");
}

static wmOperatorStatus edbm_subdivide_edge_ring_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  EdgeRingOpSubdProps op_props;

  mesh_operator_edgering_props_get(op, &op_props);

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totedgesel == 0) {
      continue;
    }

    if (!EDBM_op_callf(em,
                       op,
                       "subdivide_edgering edges=%he interp_mode=%i cuts=%i smooth=%f "
                       "profile_shape=%i profile_shape_factor=%f",
                       BM_ELEM_SELECT,
                       op_props.interp_mode,
                       op_props.cuts,
                       op_props.smooth,
                       op_props.profile_shape,
                       op_props.profile_shape_factor))
    {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_subdivide_edgering(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Subdivide Edge-Ring";
  ot->description = "Subdivide perpendicular edges to the selected edge-ring";
  ot->idname = "MESH_OT_subdivide_edgering";

  /* API callbacks. */
  ot->exec = edbm_subdivide_edge_ring_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  mesh_operator_edgering_props(ot, 1, 10);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Un-Subdivide Operator
 * \{ */

static wmOperatorStatus edbm_unsubdivide_exec(bContext *C, wmOperator *op)
{
  const int iterations = RNA_int_get(op->ptr, "iterations");
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if ((em->bm->totvertsel == 0) && (em->bm->totedgesel == 0) && (em->bm->totfacesel == 0)) {
      continue;
    }

    BMOperator bmop;
    EDBM_op_init(em, &bmop, op, "unsubdivide verts=%hv iterations=%i", BM_ELEM_SELECT, iterations);

    BMO_op_exec(em->bm, &bmop);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    if ((em->selectmode & SCE_SELECT_VERTEX) == 0) {
      EDBM_selectmode_flush_ex(em, SCE_SELECT_VERTEX); /* need to flush vert->face first */
    }
    EDBM_selectmode_flush(em);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_unsubdivide(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Un-Subdivide";
  ot->description = "Un-subdivide selected edges and faces";
  ot->idname = "MESH_OT_unsubdivide";

  /* API callbacks. */
  ot->exec = edbm_unsubdivide_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_int(
      ot->srna, "iterations", 2, 1, 1000, "Iterations", "Number of times to un-subdivide", 1, 100);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete Operator
 * \{ */

/* NOTE: these values must match delete_mesh() event values. */
enum {
  MESH_DELETE_VERT = 0,
  MESH_DELETE_EDGE = 1,
  MESH_DELETE_FACE = 2,
  MESH_DELETE_EDGE_FACE = 3,
  MESH_DELETE_ONLY_FACE = 4,
};

static void edbm_report_delete_info(ReportList *reports,
                                    const int totelem_old[3],
                                    const int totelem_new[3])
{
  BKE_reportf(reports,
              RPT_INFO,
              "Removed: %d vertices, %d edges, %d faces",
              totelem_old[0] - totelem_new[0],
              totelem_old[1] - totelem_new[1],
              totelem_old[2] - totelem_new[2]);
}

static wmOperatorStatus edbm_delete_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  bool changed_multi = false;

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    const int type = RNA_enum_get(op->ptr, "type");

    switch (type) {
      case MESH_DELETE_VERT: /* Erase Vertices */
        if (em->bm->totvertsel == 0) {
          continue;
        }
        BM_custom_loop_normals_to_vector_layer(em->bm);
        if (!EDBM_op_callf(em, op, "delete geom=%hv context=%i", BM_ELEM_SELECT, DEL_VERTS)) {
          continue;
        }
        break;
      case MESH_DELETE_EDGE: /* Erase Edges */
        if (em->bm->totedgesel == 0) {
          continue;
        }
        BM_custom_loop_normals_to_vector_layer(em->bm);
        if (!EDBM_op_callf(em, op, "delete geom=%he context=%i", BM_ELEM_SELECT, DEL_EDGES)) {
          continue;
        }
        break;
      case MESH_DELETE_FACE: /* Erase Faces */
        if (em->bm->totfacesel == 0) {
          continue;
        }
        BM_custom_loop_normals_to_vector_layer(em->bm);
        if (!EDBM_op_callf(em, op, "delete geom=%hf context=%i", BM_ELEM_SELECT, DEL_FACES)) {
          continue;
        }
        break;
      case MESH_DELETE_EDGE_FACE: /* Edges and Faces */
        if ((em->bm->totedgesel == 0) && (em->bm->totfacesel == 0)) {
          continue;
        }
        BM_custom_loop_normals_to_vector_layer(em->bm);
        if (!EDBM_op_callf(em, op, "delete geom=%hef context=%i", BM_ELEM_SELECT, DEL_EDGESFACES))
        {
          continue;
        }
        break;
      case MESH_DELETE_ONLY_FACE: /* Only faces. */
        if (em->bm->totfacesel == 0) {
          continue;
        }
        BM_custom_loop_normals_to_vector_layer(em->bm);
        if (!EDBM_op_callf(em, op, "delete geom=%hf context=%i", BM_ELEM_SELECT, DEL_ONLYFACES)) {
          continue;
        }
        break;
      default:
        BLI_assert(0);
        break;
    }

    changed_multi = true;

    EDBM_flag_disable_all(em, BM_ELEM_SELECT);

    BM_custom_loop_normals_from_vector_layer(em->bm, false);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);

    DEG_id_tag_update(static_cast<ID *>(obedit->data), ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }

  return changed_multi ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void MESH_OT_delete(wmOperatorType *ot)
{
  static const EnumPropertyItem prop_mesh_delete_types[] = {
      {MESH_DELETE_VERT, "VERT", 0, "Vertices", ""},
      {MESH_DELETE_EDGE, "EDGE", 0, "Edges", ""},
      {MESH_DELETE_FACE, "FACE", 0, "Faces", ""},
      {MESH_DELETE_EDGE_FACE, "EDGE_FACE", 0, "Only Edges & Faces", ""},
      {MESH_DELETE_ONLY_FACE, "ONLY_FACE", 0, "Only Faces", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "Delete";
  ot->description = "Delete selected vertices, edges or faces";
  ot->idname = "MESH_OT_delete";

  /* API callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = edbm_delete_exec;

  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  ot->prop = RNA_def_enum(ot->srna,
                          "type",
                          prop_mesh_delete_types,
                          MESH_DELETE_VERT,
                          "Type",
                          "Method used for deleting mesh data");
  RNA_def_property_flag(ot->prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete Loose Operator
 * \{ */

static bool bm_face_is_loose(BMFace *f)
{
  BMLoop *l_iter, *l_first;

  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    if (!BM_edge_is_boundary(l_iter->e)) {
      return false;
    }
  } while ((l_iter = l_iter->next) != l_first);

  return true;
}

static wmOperatorStatus edbm_delete_loose_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  int totelem_old_sel[3];
  int totelem_old[3];

  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  EDBM_mesh_stats_multi(objects, totelem_old, totelem_old_sel);

  const bool use_verts = (RNA_boolean_get(op->ptr, "use_verts") && totelem_old_sel[0]);
  const bool use_edges = (RNA_boolean_get(op->ptr, "use_edges") && totelem_old_sel[1]);
  const bool use_faces = (RNA_boolean_get(op->ptr, "use_faces") && totelem_old_sel[2]);

  for (Object *obedit : objects) {

    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    BMIter iter;

    BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

    if (use_faces) {
      BMFace *f;

      BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
          BM_elem_flag_set(f, BM_ELEM_TAG, bm_face_is_loose(f));
        }
      }

      BM_mesh_delete_hflag_context(bm, BM_ELEM_TAG, DEL_FACES);
    }

    if (use_edges) {
      BMEdge *e;

      BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
        if (BM_elem_flag_test(e, BM_ELEM_SELECT)) {
          BM_elem_flag_set(e, BM_ELEM_TAG, BM_edge_is_wire(e));
        }
      }

      BM_mesh_delete_hflag_context(bm, BM_ELEM_TAG, DEL_EDGES);
    }

    if (use_verts) {
      BMVert *v;

      BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
        if (BM_elem_flag_test(v, BM_ELEM_SELECT)) {
          BM_elem_flag_set(v, BM_ELEM_TAG, (v->e == nullptr));
        }
      }

      BM_mesh_delete_hflag_context(bm, BM_ELEM_TAG, DEL_VERTS);
    }

    EDBM_flag_disable_all(em, BM_ELEM_SELECT);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  int totelem_new[3];
  EDBM_mesh_stats_multi(objects, totelem_new, nullptr);

  edbm_report_delete_info(op->reports, totelem_old, totelem_new);

  return OPERATOR_FINISHED;
}

void MESH_OT_delete_loose(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Delete Loose";
  ot->description = "Delete loose vertices, edges or faces";
  ot->idname = "MESH_OT_delete_loose";

  /* API callbacks. */
  ot->exec = edbm_delete_loose_exec;

  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_boolean(ot->srna, "use_verts", true, "Vertices", "Remove loose vertices");
  RNA_def_boolean(ot->srna, "use_edges", true, "Edges", "Remove loose edges");
  RNA_def_boolean(ot->srna, "use_faces", false, "Faces", "Remove loose faces");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Collapse Edge Operator
 * \{ */

static wmOperatorStatus edbm_collapse_edge_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totedgesel == 0) {
      continue;
    }

    if (!EDBM_op_callf(em, op, "collapse edges=%he uvs=%b", BM_ELEM_SELECT, true)) {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_edge_collapse(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Collapse Edges & Faces";
  ot->description =
      "Collapse isolated edge and face regions, merging data such as UVs and color attributes. "
      "This can collapse edge-rings as well as regions of connected faces into vertices";
  ot->idname = "MESH_OT_edge_collapse";

  /* API callbacks. */
  ot->exec = edbm_collapse_edge_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Create Edge/Face Operator
 * \{ */

static bool edbm_add_edge_face__smooth_get(BMesh *bm)
{
  BMEdge *e;
  BMIter iter;

  uint vote_on_smooth[2] = {0, 0};

  BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
    if (BM_elem_flag_test(e, BM_ELEM_SELECT) && e->l) {
      vote_on_smooth[BM_elem_flag_test_bool(e->l->f, BM_ELEM_SMOOTH)]++;
    }
  }

  return (vote_on_smooth[0] < vote_on_smooth[1]);
}

#ifdef USE_FACE_CREATE_SEL_EXTEND
/**
 * Function used to get a fixed number of edges linked to a vertex that passes a test function.
 * This is used so we can request all boundary edges connected to a vertex for eg.
 */
static int edbm_add_edge_face_exec__vert_edge_lookup(
    BMVert *v, BMEdge *e_used, BMEdge **e_arr, const int e_arr_len, bool (*func)(const BMEdge *))
{
  BMIter iter;
  BMEdge *e_iter;
  int i = 0;
  BM_ITER_ELEM (e_iter, &iter, v, BM_EDGES_OF_VERT) {
    if (BM_elem_flag_test(e_iter, BM_ELEM_HIDDEN) == false) {
      if ((e_used == nullptr) || (e_used != e_iter)) {
        if (func(e_iter)) {
          e_arr[i++] = e_iter;
          if (i >= e_arr_len) {
            break;
          }
        }
      }
    }
  }
  return i;
}

static BMElem *edbm_add_edge_face_exec__tricky_extend_sel(BMesh *bm)
{
  BMIter iter;
  bool found = false;

  if (bm->totvertsel == 1 && bm->totedgesel == 0 && bm->totfacesel == 0) {
    /* first look for 2 boundary edges */
    BMVert *v;

    BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
      if (BM_elem_flag_test(v, BM_ELEM_SELECT)) {
        found = true;
        break;
      }
    }

    if (found) {
      BMEdge *ed_pair[3];
      if (((edbm_add_edge_face_exec__vert_edge_lookup(v, nullptr, ed_pair, 3, BM_edge_is_wire) ==
            2) &&
           (BM_edge_share_face_check(ed_pair[0], ed_pair[1]) == false)) ||

          ((edbm_add_edge_face_exec__vert_edge_lookup(
                v, nullptr, ed_pair, 3, BM_edge_is_boundary) == 2) &&
           (BM_edge_share_face_check(ed_pair[0], ed_pair[1]) == false)))
      {
        BMEdge *e_other = BM_edge_exists(BM_edge_other_vert(ed_pair[0], v),
                                         BM_edge_other_vert(ed_pair[1], v));
        BM_edge_select_set(bm, ed_pair[0], true);
        BM_edge_select_set(bm, ed_pair[1], true);
        if (e_other) {
          BM_edge_select_set(bm, e_other, true);
        }
        return (BMElem *)v;
      }
    }
  }
  else if (bm->totvertsel == 2 && bm->totedgesel == 1 && bm->totfacesel == 0) {
    /* first look for 2 boundary edges */
    BMEdge *e;

    BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
      if (BM_elem_flag_test(e, BM_ELEM_SELECT)) {
        found = true;
        break;
      }
    }
    if (found) {
      BMEdge *ed_pair_v1[2];
      BMEdge *ed_pair_v2[2];
      if (((edbm_add_edge_face_exec__vert_edge_lookup(e->v1, e, ed_pair_v1, 2, BM_edge_is_wire) ==
            1) &&
           (edbm_add_edge_face_exec__vert_edge_lookup(e->v2, e, ed_pair_v2, 2, BM_edge_is_wire) ==
            1) &&
           (BM_edge_share_face_check(e, ed_pair_v1[0]) == false) &&
           (BM_edge_share_face_check(e, ed_pair_v2[0]) == false)) ||

#  if 1 /* better support mixed cases #37203. */
          ((edbm_add_edge_face_exec__vert_edge_lookup(e->v1, e, ed_pair_v1, 2, BM_edge_is_wire) ==
            1) &&
           (edbm_add_edge_face_exec__vert_edge_lookup(
                e->v2, e, ed_pair_v2, 2, BM_edge_is_boundary) == 1) &&
           (BM_edge_share_face_check(e, ed_pair_v1[0]) == false) &&
           (BM_edge_share_face_check(e, ed_pair_v2[0]) == false)) ||

          ((edbm_add_edge_face_exec__vert_edge_lookup(
                e->v1, e, ed_pair_v1, 2, BM_edge_is_boundary) == 1) &&
           (edbm_add_edge_face_exec__vert_edge_lookup(e->v2, e, ed_pair_v2, 2, BM_edge_is_wire) ==
            1) &&
           (BM_edge_share_face_check(e, ed_pair_v1[0]) == false) &&
           (BM_edge_share_face_check(e, ed_pair_v2[0]) == false)) ||
#  endif

          ((edbm_add_edge_face_exec__vert_edge_lookup(
                e->v1, e, ed_pair_v1, 2, BM_edge_is_boundary) == 1) &&
           (edbm_add_edge_face_exec__vert_edge_lookup(
                e->v2, e, ed_pair_v2, 2, BM_edge_is_boundary) == 1) &&
           (BM_edge_share_face_check(e, ed_pair_v1[0]) == false) &&
           (BM_edge_share_face_check(e, ed_pair_v2[0]) == false)))
      {
        BMVert *v1_other = BM_edge_other_vert(ed_pair_v1[0], e->v1);
        BMVert *v2_other = BM_edge_other_vert(ed_pair_v2[0], e->v2);
        BMEdge *e_other = (v1_other != v2_other) ? BM_edge_exists(v1_other, v2_other) : nullptr;
        BM_edge_select_set(bm, ed_pair_v1[0], true);
        BM_edge_select_set(bm, ed_pair_v2[0], true);
        if (e_other) {
          BM_edge_select_set(bm, e_other, true);
        }
        return (BMElem *)e;
      }
    }
  }

  return nullptr;
}
static void edbm_add_edge_face_exec__tricky_finalize_sel(BMesh *bm, BMElem *ele_desel, BMFace *f)
{
  /* Now we need to find the edge that isn't connected to this element. */
  BM_select_history_clear(bm);

  /* Notes on hidden geometry:
   * - Un-hide the face since its possible hidden was copied when copying
   *   surrounding face attributes.
   * - Un-hide before adding to select history
   *   since we may extend into an existing, hidden vert/edge.
   */

  BM_elem_flag_disable(f, BM_ELEM_HIDDEN);
  BM_face_select_set(bm, f, false);

  if (ele_desel->head.htype == BM_VERT) {
    BMLoop *l = BM_face_vert_share_loop(f, (BMVert *)ele_desel);
    BLI_assert(f->len == 3);
    BM_vert_select_set(bm, (BMVert *)ele_desel, false);
    BM_edge_select_set(bm, l->next->e, true);
    BM_select_history_store(bm, l->next->e);
  }
  else {
    BMLoop *l = BM_face_edge_share_loop(f, (BMEdge *)ele_desel);
    BLI_assert(ELEM(f->len, 4, 3));

    BM_edge_select_set(bm, (BMEdge *)ele_desel, false);
    if (f->len == 4) {
      BMEdge *e_active = l->next->next->e;
      BM_elem_flag_disable(e_active, BM_ELEM_HIDDEN);
      BM_edge_select_set(bm, e_active, true);
      BM_select_history_store(bm, e_active);
    }
    else {
      BMVert *v_active = l->next->next->v;
      BM_elem_flag_disable(v_active, BM_ELEM_HIDDEN);
      BM_vert_select_set(bm, v_active, true);
      BM_select_history_store(bm, v_active);
    }
  }
}
#endif /* USE_FACE_CREATE_SEL_EXTEND */

static wmOperatorStatus edbm_add_edge_face_exec(bContext *C, wmOperator *op)
{
  /* When this is used to dissolve we could avoid this, but checking isn't too slow. */
  bool changed_multi = false;
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if ((em->bm->totvertsel == 0) && (em->bm->totedgesel == 0) && (em->bm->totfacesel == 0)) {
      continue;
    }

    bool use_smooth = edbm_add_edge_face__smooth_get(em->bm);
    int totedge_orig = em->bm->totedge;
    int totface_orig = em->bm->totface;

    BMOperator bmop;
#ifdef USE_FACE_CREATE_SEL_EXTEND
    BMElem *ele_desel;
    BMFace *ele_desel_face;

    /* be extra clever, figure out if a partial selection should be extended so we can create
     * geometry with single vert or single edge selection. */
    ele_desel = edbm_add_edge_face_exec__tricky_extend_sel(em->bm);
#endif
    if (!EDBM_op_init(em,
                      &bmop,
                      op,
                      "contextual_create geom=%hfev mat_nr=%i use_smooth=%b",
                      BM_ELEM_SELECT,
                      em->mat_nr,
                      use_smooth))
    {
      continue;
    }

    BMO_op_exec(em->bm, &bmop);

    /* cancel if nothing was done */
    if ((totedge_orig == em->bm->totedge) && (totface_orig == em->bm->totface)) {
      EDBM_op_finish(em, &bmop, op, true);
      continue;
    }
#ifdef USE_FACE_CREATE_SEL_EXTEND
    /* normally we would want to leave the new geometry selected,
     * but being able to press F many times to add geometry is too useful! */
    if (ele_desel && (BMO_slot_buffer_len(bmop.slots_out, "faces.out") == 1) &&
        (ele_desel_face = static_cast<BMFace *>(
             BMO_slot_buffer_get_first(bmop.slots_out, "faces.out"))))
    {
      edbm_add_edge_face_exec__tricky_finalize_sel(em->bm, ele_desel, ele_desel_face);
    }
    else
#endif
    {
      /* Newly created faces may include existing hidden edges,
       * copying face data from surrounding, may have copied hidden face flag too.
       *
       * Important that faces use flushing since 'edges.out'
       * won't include hidden edges that already existed.
       */
      BMO_slot_buffer_hflag_disable(
          em->bm, bmop.slots_out, "faces.out", BM_FACE, BM_ELEM_HIDDEN, true);
      BMO_slot_buffer_hflag_disable(
          em->bm, bmop.slots_out, "edges.out", BM_EDGE, BM_ELEM_HIDDEN, false);

      BMO_slot_buffer_hflag_enable(
          em->bm, bmop.slots_out, "faces.out", BM_FACE, BM_ELEM_SELECT, true);
      BMO_slot_buffer_hflag_enable(
          em->bm, bmop.slots_out, "edges.out", BM_EDGE, BM_ELEM_SELECT, true);
    }

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
    changed_multi = true;
  }

  if (!changed_multi) {
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_edge_face_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Make Edge/Face";
  ot->description = "Add an edge or face to selected";
  ot->idname = "MESH_OT_edge_face_add";

  /* API callbacks. */
  ot->exec = edbm_add_edge_face_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mark Edge (Seam) Operator
 * \{ */

static wmOperatorStatus edbm_mark_seam_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BMEdge *eed;
  BMIter iter;
  const bool clear = RNA_boolean_get(op->ptr, "clear");

  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if (bm->totedgesel == 0) {
      continue;
    }

    if (clear) {
      BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
        if (!BM_elem_flag_test(eed, BM_ELEM_SELECT) || BM_elem_flag_test(eed, BM_ELEM_HIDDEN)) {
          continue;
        }

        BM_elem_flag_disable(eed, BM_ELEM_SEAM);
      }
    }
    else {
      BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
        if (!BM_elem_flag_test(eed, BM_ELEM_SELECT) || BM_elem_flag_test(eed, BM_ELEM_HIDDEN)) {
          continue;
        }
        BM_elem_flag_enable(eed, BM_ELEM_SEAM);
      }
    }
  }

  ED_uvedit_live_unwrap(scene, objects);

  for (Object *obedit : objects) {
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_mark_seam(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Mark Seam";
  ot->idname = "MESH_OT_mark_seam";
  ot->description = "(Un)mark selected edges as a seam";

  /* API callbacks. */
  ot->exec = edbm_mark_seam_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_boolean(ot->srna, "clear", false, "Clear", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  WM_operatortype_props_advanced_begin(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mark Edge (Sharp) Operator
 * \{ */

static wmOperatorStatus edbm_mark_sharp_exec(bContext *C, wmOperator *op)
{
  BMEdge *eed;
  BMIter iter;
  const bool clear = RNA_boolean_get(op->ptr, "clear");
  const bool use_verts = RNA_boolean_get(op->ptr, "use_verts");
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if ((use_verts && bm->totvertsel == 0) || (!use_verts && bm->totedgesel == 0)) {
      continue;
    }

    BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
      if (use_verts) {
        if (!(BM_elem_flag_test(eed->v1, BM_ELEM_SELECT) ||
              BM_elem_flag_test(eed->v2, BM_ELEM_SELECT)))
        {
          continue;
        }
      }
      else if (!BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
        continue;
      }

      BM_elem_flag_set(eed, BM_ELEM_SMOOTH, clear);
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_mark_sharp(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Mark Sharp";
  ot->idname = "MESH_OT_mark_sharp";
  ot->description = "(Un)mark selected edges as sharp";

  /* API callbacks. */
  ot->exec = edbm_mark_sharp_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_boolean(ot->srna, "clear", false, "Clear", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_boolean(
      ot->srna,
      "use_verts",
      false,
      "Vertices",
      "Consider vertices instead of edges to select which edges to (un)tag as sharp");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Connect Vertex Path Operator
 * \{ */

static bool edbm_connect_vert_pair(BMEditMesh *em, Mesh *mesh, wmOperator *op)
{
  BMesh *bm = em->bm;
  BMOperator bmop;
  const int verts_len = bm->totvertsel;
  bool is_pair = (verts_len == 2);
  int len = 0;
  bool check_degenerate = true;

  bool checks_succeded = true;

  /* sanity check */
  if (verts_len < 2) {
    return false;
  }

  BMVert **verts = MEM_malloc_arrayN<BMVert *>(verts_len, __func__);
  {
    BMIter iter;
    BMVert *v;
    int i = 0;

    BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
      if (BM_elem_flag_test(v, BM_ELEM_SELECT)) {
        verts[i++] = v;
      }
    }

    if (BM_vert_pair_share_face_check_cb(
            verts[0], verts[1], BM_elem_cb_check_hflag_disabled_simple(BMFace *, BM_ELEM_HIDDEN)))
    {
      check_degenerate = false;
      is_pair = false;
    }
  }

  if (is_pair) {
    if (!EDBM_op_init(em,
                      &bmop,
                      op,
                      "connect_vert_pair verts=%eb verts_exclude=%hv faces_exclude=%hf",
                      verts,
                      verts_len,
                      BM_ELEM_HIDDEN,
                      BM_ELEM_HIDDEN))
    {
      checks_succeded = false;
    }
  }
  else {
    if (!EDBM_op_init(em,
                      &bmop,
                      op,
                      "connect_verts verts=%eb faces_exclude=%hf check_degenerate=%b",
                      verts,
                      verts_len,
                      BM_ELEM_HIDDEN,
                      check_degenerate))
    {
      checks_succeded = false;
    }
  }
  if (checks_succeded) {
    BMBackup em_backup = EDBM_redo_state_store(em);

    BM_custom_loop_normals_to_vector_layer(bm);

    BMO_op_exec(bm, &bmop);
    const bool failure = BMO_error_occurred_at_level(bm, BMO_ERROR_FATAL);
    len = BMO_slot_get(bmop.slots_out, "edges.out")->len;

    if (len && is_pair) {
      /* new verts have been added, we have to select the edges, not just flush */
      BMO_slot_buffer_hflag_enable(
          em->bm, bmop.slots_out, "edges.out", BM_EDGE, BM_ELEM_SELECT, true);
    }

    bool em_backup_free = true;
    if (!EDBM_op_finish(em, &bmop, op, false)) {
      len = 0;
    }
    else if (failure) {
      len = 0;
      EDBM_redo_state_restore_and_free(&em_backup, em, true);
      em_backup_free = false;
    }
    else {
      /* so newly created edges get the selection state from the vertex */
      EDBM_selectmode_flush(em);

      BM_custom_loop_normals_from_vector_layer(bm, false);

      EDBMUpdate_Params params{};
      params.calc_looptris = true;
      params.calc_normals = false;
      params.is_destructive = true;
      EDBM_update(mesh, &params);
    }

    if (em_backup_free) {
      EDBM_redo_state_free(&em_backup);
    }
  }
  MEM_freeN(verts);

  return len;
}

static wmOperatorStatus edbm_vert_connect_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint failed_objects_len = 0;
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (!edbm_connect_vert_pair(em, static_cast<Mesh *>(obedit->data), op)) {
      failed_objects_len++;
    }
  }
  return failed_objects_len == objects.size() ? OPERATOR_CANCELLED : OPERATOR_FINISHED;
}

void MESH_OT_vert_connect(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Vertex Connect";
  ot->idname = "MESH_OT_vert_connect";
  ot->description = "Connect selected vertices of faces, splitting the face";

  /* API callbacks. */
  ot->exec = edbm_vert_connect_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Connect Vertex Path Operator
 * \{ */

/**
 * check that endpoints are verts and only have a single selected edge connected.
 */
static bool bm_vert_is_select_history_open(BMesh *bm)
{
  BMEditSelection *ele_a = static_cast<BMEditSelection *>(bm->selected.first);
  BMEditSelection *ele_b = static_cast<BMEditSelection *>(bm->selected.last);
  if ((ele_a->htype == BM_VERT) && (ele_b->htype == BM_VERT)) {
    if ((BM_iter_elem_count_flag(BM_EDGES_OF_VERT, (BMVert *)ele_a->ele, BM_ELEM_SELECT, true) ==
         1) &&
        (BM_iter_elem_count_flag(BM_EDGES_OF_VERT, (BMVert *)ele_b->ele, BM_ELEM_SELECT, true) ==
         1))
    {
      return true;
    }
  }

  return false;
}

static bool bm_vert_connect_pair(BMesh *bm, BMVert *v_a, BMVert *v_b)
{
  BMOperator bmop;
  BMVert **verts;
  const int totedge_orig = bm->totedge;

  BMO_op_init(bm, &bmop, BMO_FLAG_DEFAULTS, "connect_vert_pair");

  verts = static_cast<BMVert **>(BMO_slot_buffer_alloc(&bmop, bmop.slots_in, "verts", 2));
  verts[0] = v_a;
  verts[1] = v_b;

  BM_vert_normal_update(verts[0]);
  BM_vert_normal_update(verts[1]);

  BMO_op_exec(bm, &bmop);
  BMO_slot_buffer_hflag_enable(bm, bmop.slots_out, "edges.out", BM_EDGE, BM_ELEM_SELECT, true);
  BMO_op_finish(bm, &bmop);
  return (bm->totedge != totedge_orig);
}

static bool bm_vert_connect_select_history(BMesh *bm)
{
  /* Logic is as follows:
   *
   * - If there are any isolated/wire verts - connect as edges.
   * - Otherwise connect faces.
   * - If all edges have been created already, closed the loop.
   */
  if (BLI_listbase_count_at_most(&bm->selected, 2) == 2 && (bm->totvertsel > 2)) {
    BMEditSelection *ese;
    int tot = 0;
    bool changed = false;
    bool has_wire = false;
    // bool all_verts;

    /* ensure all verts have history */
    for (ese = static_cast<BMEditSelection *>(bm->selected.first); ese; ese = ese->next, tot++) {
      BMVert *v;
      if (ese->htype != BM_VERT) {
        break;
      }
      v = (BMVert *)ese->ele;
      if ((has_wire == false) && ((v->e == nullptr) || BM_vert_is_wire(v))) {
        has_wire = true;
      }
    }
    // all_verts = (ese == nullptr);

    if (has_wire == false) {
      /* all verts have faces , connect verts via faces! */
      if (tot == bm->totvertsel) {
        BMEditSelection *ese_last;
        ese_last = static_cast<BMEditSelection *>(bm->selected.first);
        ese = ese_last->next;

        do {

          if (BM_edge_exists((BMVert *)ese_last->ele, (BMVert *)ese->ele)) {
            /* pass, edge exists (and will be selected) */
          }
          else {
            changed |= bm_vert_connect_pair(bm, (BMVert *)ese_last->ele, (BMVert *)ese->ele);
          }
        } while ((void)(ese_last = ese), (ese = ese->next));

        if (changed) {
          return true;
        }
      }

      if (changed == false) {
        /* existing loops: close the selection */
        if (bm_vert_is_select_history_open(bm)) {
          changed |= bm_vert_connect_pair(bm,
                                          (BMVert *)((BMEditSelection *)bm->selected.first)->ele,
                                          (BMVert *)((BMEditSelection *)bm->selected.last)->ele);

          if (changed) {
            return true;
          }
        }
      }
    }

    else {
      /* no faces, simply connect the verts by edges */
      BMEditSelection *ese_prev;
      ese_prev = static_cast<BMEditSelection *>(bm->selected.first);
      ese = ese_prev->next;

      do {
        if (BM_edge_exists((BMVert *)ese_prev->ele, (BMVert *)ese->ele)) {
          /* pass, edge exists (and will be selected) */
        }
        else {
          BMEdge *e;
          e = BM_edge_create(
              bm, (BMVert *)ese_prev->ele, (BMVert *)ese->ele, nullptr, eBMCreateFlag(0));
          BM_edge_select_set(bm, e, true);
          changed = true;
        }
      } while ((void)(ese_prev = ese), (ese = ese->next));

      if (changed == false) {
        /* existing loops: close the selection */
        if (bm_vert_is_select_history_open(bm)) {
          BMEdge *e;
          ese_prev = static_cast<BMEditSelection *>(bm->selected.first);
          ese = static_cast<BMEditSelection *>(bm->selected.last);
          e = BM_edge_create(
              bm, (BMVert *)ese_prev->ele, (BMVert *)ese->ele, nullptr, eBMCreateFlag(0));
          BM_edge_select_set(bm, e, true);
        }
      }

      return true;
    }
  }

  return false;
}

/**
 * Convert an edge selection to a temp vertex selection
 * (which must be cleared after use as a path to connect).
 */
static bool bm_vert_connect_select_history_edge_to_vert_path(BMesh *bm, ListBase *r_selected)
{
  ListBase selected_orig = {nullptr, nullptr};
  int edges_len = 0;
  bool side = false;

  /* first check all edges are OK */
  LISTBASE_FOREACH (BMEditSelection *, ese, &bm->selected) {
    if (ese->htype == BM_EDGE) {
      edges_len += 1;
    }
    else {
      return false;
    }
  }
  /* if this is a mixed selection, bail out! */
  if (bm->totedgesel != edges_len) {
    return false;
  }

  std::swap(bm->selected, selected_orig);

  /* convert edge selection into 2 ordered loops (where the first edge ends up in the middle) */
  LISTBASE_FOREACH (BMEditSelection *, ese, &selected_orig) {
    BMEdge *e_curr = (BMEdge *)ese->ele;
    BMEdge *e_prev = ese->prev ? (BMEdge *)ese->prev->ele : nullptr;
    BMLoop *l_curr;
    BMLoop *l_prev;
    BMVert *v;

    if (e_prev) {
      BMFace *f = BM_edge_pair_share_face_by_len(e_curr, e_prev, &l_curr, &l_prev, true);
      if (f) {
        if ((e_curr->v1 != l_curr->v) == (e_prev->v1 != l_prev->v)) {
          side = !side;
        }
      }
      else if (is_quad_flip_v3(e_curr->v1->co, e_curr->v2->co, e_prev->v2->co, e_prev->v1->co)) {
        side = !side;
      }
    }

    v = (&e_curr->v1)[side];
    if (!bm->selected.last || (BMVert *)((BMEditSelection *)bm->selected.last)->ele != v) {
      BM_select_history_store_notest(bm, v);
    }

    v = (&e_curr->v1)[!side];
    if (!bm->selected.first || (BMVert *)((BMEditSelection *)bm->selected.first)->ele != v) {
      BM_select_history_store_head_notest(bm, v);
    }

    e_prev = e_curr;
  }

  *r_selected = bm->selected;
  bm->selected = selected_orig;

  return true;
}

static wmOperatorStatus edbm_vert_connect_path_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint failed_selection_order_len = 0;
  uint failed_connect_len = 0;
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    const bool is_pair = (em->bm->totvertsel == 2);
    ListBase selected_orig = {nullptr, nullptr};

    if (bm->totvertsel == 0) {
      continue;
    }

    /* when there is only 2 vertices, we can ignore selection order */
    if (is_pair) {
      if (!edbm_connect_vert_pair(em, static_cast<Mesh *>(obedit->data), op)) {
        failed_connect_len++;
      }
      continue;
    }

    if (bm->selected.first) {
      BMEditSelection *ese = static_cast<BMEditSelection *>(bm->selected.first);
      if (ese->htype == BM_EDGE) {
        if (bm_vert_connect_select_history_edge_to_vert_path(bm, &selected_orig)) {
          std::swap(bm->selected, selected_orig);
        }
      }
    }

    BM_custom_loop_normals_to_vector_layer(bm);

    if (bm_vert_connect_select_history(bm)) {
      EDBM_selectmode_flush(em);

      BM_custom_loop_normals_from_vector_layer(bm, false);

      EDBMUpdate_Params params{};
      params.calc_looptris = true;
      params.calc_normals = false;
      params.is_destructive = true;
      EDBM_update(static_cast<Mesh *>(obedit->data), &params);
    }
    else {
      failed_selection_order_len++;
    }

    if (!BLI_listbase_is_empty(&selected_orig)) {
      BM_select_history_clear(bm);
      bm->selected = selected_orig;
    }
  }

  if (failed_selection_order_len == objects.size()) {
    BKE_report(op->reports, RPT_ERROR, "Invalid selection order");
    return OPERATOR_CANCELLED;
  }
  if (failed_connect_len == objects.size()) {
    BKE_report(op->reports, RPT_ERROR, "Could not connect vertices");
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_vert_connect_path(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Vertex Connect Path";
  ot->idname = "MESH_OT_vert_connect_path";
  ot->description = "Connect vertices by their selection order, creating edges, splitting faces";

  /* API callbacks. */
  ot->exec = edbm_vert_connect_path_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Connect Concave Operator
 * \{ */

static wmOperatorStatus edbm_vert_connect_concave_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    if (!EDBM_op_call_and_selectf(
            em, op, "faces.out", true, "connect_verts_concave faces=%hf", BM_ELEM_SELECT))
    {
      continue;
    }
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_vert_connect_concave(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Split Concave Faces";
  ot->idname = "MESH_OT_vert_connect_concave";
  ot->description = "Make all faces convex";

  /* API callbacks. */
  ot->exec = edbm_vert_connect_concave_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Split Non-Planar Faces Operator
 * \{ */

static wmOperatorStatus edbm_vert_connect_nonplaner_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const float angle_limit = RNA_float_get(op->ptr, "angle_limit");
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    if (!EDBM_op_call_and_selectf(em,
                                  op,
                                  "faces.out",
                                  true,
                                  "connect_verts_nonplanar faces=%hf angle_limit=%f",
                                  BM_ELEM_SELECT,
                                  angle_limit))
    {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_vert_connect_nonplanar(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Split Non-Planar Faces";
  ot->idname = "MESH_OT_vert_connect_nonplanar";
  ot->description = "Split non-planar faces that exceed the angle threshold";

  /* API callbacks. */
  ot->exec = edbm_vert_connect_nonplaner_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  prop = RNA_def_float_rotation(ot->srna,
                                "angle_limit",
                                0,
                                nullptr,
                                0.0f,
                                DEG2RADF(180.0f),
                                "Max Angle",
                                "Angle limit",
                                0.0f,
                                DEG2RADF(180.0f));
  RNA_def_property_float_default(prop, DEG2RADF(5.0f));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Make Planar Faces Operator
 * \{ */

static wmOperatorStatus edbm_face_make_planar_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  const int repeat = RNA_int_get(op->ptr, "repeat");
  const float fac = RNA_float_get(op->ptr, "factor");

  int totobjects = 0;

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    if (blender::ed::object::shape_key_report_if_locked(obedit, op->reports)) {
      continue;
    }

    totobjects++;

    if (!EDBM_op_callf(
            em, op, "planar_faces faces=%hf iterations=%i factor=%f", BM_ELEM_SELECT, repeat, fac))
    {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return totobjects ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void MESH_OT_face_make_planar(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Make Planar Faces";
  ot->idname = "MESH_OT_face_make_planar";
  ot->description = "Flatten selected faces";

  /* API callbacks. */
  ot->exec = edbm_face_make_planar_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_float(ot->srna, "factor", 1.0f, -10.0f, 10.0f, "Factor", "", 0.0f, 1.0f);
  RNA_def_int(ot->srna, "repeat", 1, 1, 10000, "Iterations", "", 1, 200);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Split Edge Operator
 * \{ */

static bool edbm_edge_split_selected_edges(wmOperator *op, Object *obedit, BMEditMesh *em)
{
  BMesh *bm = em->bm;
  if (bm->totedgesel == 0) {
    return false;
  }

  BM_custom_loop_normals_to_vector_layer(em->bm);

  if (!EDBM_op_call_and_selectf(
          em, op, "edges.out", false, "split_edges edges=%he", BM_ELEM_SELECT))
  {
    return false;
  }

  BM_custom_loop_normals_from_vector_layer(em->bm, false);

  EDBM_select_flush(em);
  EDBMUpdate_Params params{};
  params.calc_looptris = true;
  params.calc_normals = false;
  params.is_destructive = true;
  EDBM_update(static_cast<Mesh *>(obedit->data), &params);

  return true;
}

static bool edbm_edge_split_selected_verts(wmOperator *op, Object *obedit, BMEditMesh *em)
{
  BMesh *bm = em->bm;

  /* Note that tracking vertices through the 'split_edges' operator is complicated.
   * Instead, tag loops for selection. */
  if (bm->totvertsel == 0) {
    return false;
  }

  BM_custom_loop_normals_to_vector_layer(em->bm);

  /* Flush from vertices to edges. */
  BMIter iter;
  BMEdge *eed;
  BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
    BM_elem_flag_disable(eed, BM_ELEM_TAG);
    if (eed->l != nullptr) {
      if (!BM_elem_flag_test(eed, BM_ELEM_HIDDEN) && (BM_elem_flag_test(eed->v1, BM_ELEM_SELECT) ||
                                                      BM_elem_flag_test(eed->v2, BM_ELEM_SELECT)))
      {
        BM_elem_flag_enable(eed, BM_ELEM_TAG);
      }
      /* Store selection in loop tags. */
      BMLoop *l_iter = eed->l;
      do {
        BM_elem_flag_set(l_iter, BM_ELEM_TAG, BM_elem_flag_test(l_iter->v, BM_ELEM_SELECT));
      } while ((l_iter = l_iter->radial_next) != eed->l);
    }
  }

  if (!EDBM_op_callf(em,
                     op,
                     "split_edges edges=%he verts=%hv use_verts=%b",
                     BM_ELEM_TAG,
                     BM_ELEM_SELECT,
                     true))
  {
    return false;
  }

  BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
    if (eed->l != nullptr) {
      BMLoop *l_iter = eed->l;
      do {
        if (BM_elem_flag_test(l_iter, BM_ELEM_TAG)) {
          BM_vert_select_set(em->bm, l_iter->v, true);
        }
      } while ((l_iter = l_iter->radial_next) != eed->l);
    }
    else {
      /* Split out wire. */
      for (int i = 0; i < 2; i++) {
        BMVert *v = *(&eed->v1 + i);
        if (BM_elem_flag_test(v, BM_ELEM_SELECT)) {
          if (eed != BM_DISK_EDGE_NEXT(eed, v)) {
            BM_vert_separate(bm, v, &eed, 1, true, nullptr, nullptr);
          }
        }
      }
    }
  }

  BM_custom_loop_normals_from_vector_layer(em->bm, false);

  EDBM_select_flush(em);
  EDBMUpdate_Params params{};
  params.calc_looptris = true;
  params.calc_normals = false;
  params.is_destructive = true;
  EDBM_update(static_cast<Mesh *>(obedit->data), &params);

  return true;
}

static wmOperatorStatus edbm_edge_split_exec(bContext *C, wmOperator *op)
{
  const int type = RNA_enum_get(op->ptr, "type");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    switch (type) {
      case BM_VERT:
        if (!edbm_edge_split_selected_verts(op, obedit, em)) {
          continue;
        }
        break;
      case BM_EDGE:
        if (!edbm_edge_split_selected_edges(op, obedit, em)) {
          continue;
        }
        break;
      default:
        BLI_assert(0);
    }
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_edge_split(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Edge Split";
  ot->idname = "MESH_OT_edge_split";
  ot->description = "Split selected edges so that each neighbor face gets its own copy";

  /* API callbacks. */
  ot->exec = edbm_edge_split_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  static const EnumPropertyItem split_type_items[] = {
      {BM_EDGE, "EDGE", 0, "Faces by Edges", "Split faces along selected edges"},
      {BM_VERT,
       "VERT",
       0,
       "Faces & Edges by Vertices",
       "Split faces and edges connected to selected vertices"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->prop = RNA_def_enum(
      ot->srna, "type", split_type_items, BM_EDGE, "Type", "Method to use for splitting");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Operator
 * \{ */

static wmOperatorStatus edbm_duplicate_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  bool changed = false;

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    if (em->bm->totvertsel == 0) {
      continue;
    }

    BMOperator bmop;
    BMesh *bm = em->bm;
    changed = true;

    EDBM_op_init(em,
                 &bmop,
                 op,
                 "duplicate geom=%hvef use_select_history=%b use_edge_flip_from_face=%b",
                 BM_ELEM_SELECT,
                 true,
                 true);

    BMO_op_exec(bm, &bmop);

    /* de-select all would clear otherwise */
    BM_SELECT_HISTORY_BACKUP(bm);

    EDBM_flag_disable_all(em, BM_ELEM_SELECT);

    BMO_slot_buffer_hflag_enable(
        bm, bmop.slots_out, "geom.out", BM_ALL_NOLOOP, BM_ELEM_SELECT, true);

    /* Rebuild edit-selection. */
    BM_SELECT_HISTORY_RESTORE(bm);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return (changed) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static wmOperatorStatus edbm_duplicate_invoke(bContext *C,
                                              wmOperator *op,
                                              const wmEvent * /*event*/)
{
  WM_cursor_wait(true);
  const wmOperatorStatus retval = edbm_duplicate_exec(C, op);
  WM_cursor_wait(false);

  return retval;
}

void MESH_OT_duplicate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Duplicate";
  ot->description = "Duplicate selected vertices, edges or faces";
  ot->idname = "MESH_OT_duplicate";

  /* API callbacks. */
  ot->invoke = edbm_duplicate_invoke;
  ot->exec = edbm_duplicate_exec;

  ot->poll = ED_operator_editmesh;

  /* to give to transform */
  RNA_def_int(ot->srna,
              "mode",
              blender::ed::transform::TFM_TRANSLATION,
              0,
              INT_MAX,
              "Mode",
              "",
              0,
              INT_MAX);
}

static BMLoopNorEditDataArray *flip_custom_normals_init_data(BMesh *bm)
{
  BMLoopNorEditDataArray *lnors_ed_arr = nullptr;
  if (CustomData_has_layer_named(&bm->ldata, CD_PROP_INT16_2D, "custom_normal")) {
    /* The mesh has custom normal data, update these too.
     * Otherwise they will be left in a mangled state.
     */
    BM_lnorspace_update(bm);
    lnors_ed_arr = BM_loop_normal_editdata_array_init(bm, true);
  }

  return lnors_ed_arr;
}

static bool flip_custom_normals(BMesh *bm, BMLoopNorEditDataArray *lnors_ed_arr)
{
  if (!lnors_ed_arr) {
    return false;
  }

  if (lnors_ed_arr->totloop == 0) {
    /* No loops normals to flip, exit early! */
    return false;
  }

  bm->spacearr_dirty |= BM_SPACEARR_DIRTY_ALL;
  BM_lnorspace_update(bm);

  /* We need to recreate the custom normal array because the clnors_data will
   * be mangled because we swapped the loops around when we flipped the faces. */
  BMLoopNorEditDataArray *lnors_ed_arr_new_full = BM_loop_normal_editdata_array_init(bm, true);

  {
    /* We need to recalculate all loop normals in the affected area. Even the ones that are not
     * going to be flipped because the clnors data is mangled. */

    BMLoopNorEditData *lnor_ed_new_full = lnors_ed_arr_new_full->lnor_editdata;
    for (int i = 0; i < lnors_ed_arr_new_full->totloop; i++, lnor_ed_new_full++) {

      BMLoopNorEditData *lnor_ed =
          lnors_ed_arr->lidx_to_lnor_editdata[lnor_ed_new_full->loop_index];

      BLI_assert(lnor_ed != nullptr);

      BKE_lnor_space_custom_normal_to_data(
          bm->lnor_spacearr->lspacearr[lnor_ed_new_full->loop_index],
          lnor_ed->nloc,
          lnor_ed_new_full->clnors_data);
    }
  }

  BMFace *f;
  BMLoop *l, *l_start;
  BMIter iter_f;
  BM_ITER_MESH (f, &iter_f, bm, BM_FACES_OF_MESH) {
    /* Flip all the custom loop normals on the selected faces. */
    if (!BM_elem_flag_test(f, BM_ELEM_SELECT)) {
      continue;
    }

    /* Because the winding has changed, we need to go the reverse way around the face to get the
     * correct placement of the normals. However we need to derive the old loop index to get the
     * correct data. Note that the first loop index is the same though. So the loop starts and ends
     * in the same place as before the flip.
     */

    l_start = l = BM_FACE_FIRST_LOOP(f);
    int old_index = BM_elem_index_get(l);
    do {
      int loop_index = BM_elem_index_get(l);

      BMLoopNorEditData *lnor_ed = lnors_ed_arr->lidx_to_lnor_editdata[old_index];
      BMLoopNorEditData *lnor_ed_new = lnors_ed_arr_new_full->lidx_to_lnor_editdata[loop_index];
      BLI_assert(lnor_ed != nullptr && lnor_ed_new != nullptr);

      negate_v3(lnor_ed->nloc);

      BKE_lnor_space_custom_normal_to_data(
          bm->lnor_spacearr->lspacearr[loop_index], lnor_ed->nloc, lnor_ed_new->clnors_data);

      old_index++;
      l = l->prev;
    } while (l != l_start);
  }
  BM_loop_normal_editdata_array_free(lnors_ed_arr_new_full);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Flip Normals Operator
 * \{ */

static void edbm_flip_normals_custom_loop_normals(Object *obedit, BMEditMesh *em)
{
  if (!CustomData_has_layer_named(&em->bm->ldata, CD_PROP_INT16_2D, "custom_normal")) {
    return;
  }

  /* The mesh has custom normal data, flip them. */
  BMesh *bm = em->bm;

  BM_lnorspace_update(bm);
  BMLoopNorEditDataArray *lnors_ed_arr = BM_loop_normal_editdata_array_init(bm, false);
  BMLoopNorEditData *lnor_ed = lnors_ed_arr->lnor_editdata;

  for (int i = 0; i < lnors_ed_arr->totloop; i++, lnor_ed++) {
    negate_v3(lnor_ed->nloc);

    BKE_lnor_space_custom_normal_to_data(
        bm->lnor_spacearr->lspacearr[lnor_ed->loop_index], lnor_ed->nloc, lnor_ed->clnors_data);
  }
  BM_loop_normal_editdata_array_free(lnors_ed_arr);
  EDBMUpdate_Params params{};
  params.calc_looptris = true;
  params.calc_normals = false;
  params.is_destructive = false;
  EDBM_update(static_cast<Mesh *>(obedit->data), &params);
}

static void edbm_flip_quad_tessellation(wmOperator *op, Object *obedit, BMEditMesh *em)
{
  if (EDBM_op_callf(em, op, "flip_quad_tessellation faces=%hf", BM_ELEM_SELECT)) {
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }
}

static void edbm_flip_normals_face_winding(wmOperator *op, Object *obedit, BMEditMesh *em)
{

  bool has_flipped_faces = false;

  /* See if we have any custom normals to flip. */
  BMLoopNorEditDataArray *lnors_ed_arr = flip_custom_normals_init_data(em->bm);

  if (EDBM_op_callf(em, op, "reverse_faces faces=%hf flip_multires=%b", BM_ELEM_SELECT, true)) {
    has_flipped_faces = true;
  }

  if (flip_custom_normals(em->bm, lnors_ed_arr) || has_flipped_faces) {
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  if (lnors_ed_arr != nullptr) {
    BM_loop_normal_editdata_array_free(lnors_ed_arr);
  }
}

static wmOperatorStatus edbm_flip_quad_tessellation_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    if (em->bm->totfacesel == 0) {
      continue;
    }
    edbm_flip_quad_tessellation(op, obedit, em);
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus edbm_flip_normals_exec(bContext *C, wmOperator *op)
{
  const bool only_clnors = RNA_boolean_get(op->ptr, "only_clnors");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (only_clnors) {
      if ((em->bm->totvertsel == 0) && (em->bm->totedgesel == 0) && (em->bm->totfacesel == 0)) {
        continue;
      }
      edbm_flip_normals_custom_loop_normals(obedit, em);
    }
    else {
      if (em->bm->totfacesel == 0) {
        continue;
      }
      edbm_flip_normals_face_winding(op, obedit, em);
    }
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_flip_normals(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Flip Normals";
  ot->description = "Flip the direction of selected faces' normals (and of their vertices)";
  ot->idname = "MESH_OT_flip_normals";

  /* API callbacks. */
  ot->exec = edbm_flip_normals_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "only_clnors",
                  false,
                  "Custom Normals Only",
                  "Only flip the custom loop normals of the selected elements");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rotate Edge Operator
 * \{ */

/**
 * Rotate the edges between selected faces, otherwise rotate the selected edges.
 */
static wmOperatorStatus edbm_edge_rotate_selected_exec(bContext *C, wmOperator *op)
{
  BMEdge *eed;
  BMIter iter;
  const bool use_ccw = RNA_boolean_get(op->ptr, "use_ccw");

  int tot_failed_all = 0;
  bool no_selected_edges = true, invalid_selected_edges = true;

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    int tot = 0;

    if (em->bm->totedgesel == 0) {
      continue;
    }
    no_selected_edges = false;

    /* first see if we have two adjacent faces */
    BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
      BM_elem_flag_disable(eed, BM_ELEM_TAG);
      if (BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
        BMFace *fa, *fb;
        if (BM_edge_face_pair(eed, &fa, &fb)) {
          /* if both faces are selected we rotate between them,
           * otherwise - rotate between 2 unselected - but not mixed */
          if (BM_elem_flag_test(fa, BM_ELEM_SELECT) == BM_elem_flag_test(fb, BM_ELEM_SELECT)) {
            BM_elem_flag_enable(eed, BM_ELEM_TAG);
            tot++;
          }
        }
      }
    }

    /* OK, we don't have two adjacent faces, but we do have two selected ones.
     * that's an error condition. */
    if (tot == 0) {
      continue;
    }
    invalid_selected_edges = false;

    BMOperator bmop;
    EDBM_op_init(em, &bmop, op, "rotate_edges edges=%he use_ccw=%b", BM_ELEM_TAG, use_ccw);

    /* avoids leaving old verts selected which can be a problem running multiple times,
     * since this means the edges become selected around the face
     * which then attempt to rotate */
    BMO_slot_buffer_hflag_disable(em->bm, bmop.slots_in, "edges", BM_EDGE, BM_ELEM_SELECT, true);

    BMO_op_exec(em->bm, &bmop);
    /* edges may rotate into hidden vertices, if this does _not_ run we get an illogical state */
    BMO_slot_buffer_hflag_disable(
        em->bm, bmop.slots_out, "edges.out", BM_EDGE, BM_ELEM_HIDDEN, true);
    BMO_slot_buffer_hflag_enable(
        em->bm, bmop.slots_out, "edges.out", BM_EDGE, BM_ELEM_SELECT, true);

    const int tot_rotate = BMO_slot_buffer_len(bmop.slots_out, "edges.out");
    const int tot_failed = tot - tot_rotate;

    tot_failed_all += tot_failed;

    if (tot_failed != 0) {
      /* If some edges fail to rotate, we need to re-select them,
       * otherwise we can end up with invalid selection
       * (unselected edge between 2 selected faces). */
      BM_mesh_elem_hflag_enable_test(em->bm, BM_EDGE, BM_ELEM_SELECT, true, false, BM_ELEM_TAG);
    }

    EDBM_selectmode_flush(em);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  if (no_selected_edges) {
    BKE_report(
        op->reports, RPT_ERROR, "Select edges or face pairs for edge loops to rotate about");
    return OPERATOR_CANCELLED;
  }

  /* Ok, we don't have two adjacent faces, but we do have two selected ones.
   * that's an error condition. */
  if (invalid_selected_edges) {
    BKE_report(op->reports, RPT_ERROR, "Could not find any selected edges that can be rotated");
    return OPERATOR_CANCELLED;
  }

  if (tot_failed_all != 0) {
    BKE_reportf(op->reports, RPT_WARNING, "Unable to rotate %d edge(s)", tot_failed_all);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_edge_rotate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Rotate Selected Edge";
  ot->description = "Rotate selected edge or adjoining faces";
  ot->idname = "MESH_OT_edge_rotate";

  /* API callbacks. */
  ot->exec = edbm_edge_rotate_selected_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_boolean(ot->srna, "use_ccw", false, "Counter Clockwise", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hide Operator
 * \{ */

static wmOperatorStatus edbm_hide_exec(bContext *C, wmOperator *op)
{
  const bool unselected = RNA_boolean_get(op->ptr, "unselected");
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  bool changed = false;

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if (unselected) {
      if (em->selectmode & SCE_SELECT_VERTEX) {
        if (bm->totvertsel == bm->totvert) {
          continue;
        }
      }
      else if (em->selectmode & SCE_SELECT_EDGE) {
        if (bm->totedgesel == bm->totedge) {
          continue;
        }
      }
      else if (em->selectmode & SCE_SELECT_FACE) {
        if (bm->totfacesel == bm->totface) {
          continue;
        }
      }
    }
    else {
      if (bm->totvertsel == 0) {
        continue;
      }
    }

    /* Only if symmetry is enabled. */
    EDBM_select_mirrored_extend_all(obedit, em);

    if (EDBM_mesh_hide(em, unselected)) {
      EDBMUpdate_Params params{};
      params.calc_looptris = true;
      params.calc_normals = false;
      params.is_destructive = false;
      EDBM_update(static_cast<Mesh *>(obedit->data), &params);
      changed = true;
    }
  }

  if (!changed) {
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_hide(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Hide Selected";
  ot->idname = "MESH_OT_hide";
  ot->description = "Hide (un)selected vertices, edges or faces";

  /* API callbacks. */
  ot->exec = edbm_hide_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_boolean(
      ot->srna, "unselected", false, "Unselected", "Hide unselected rather than selected");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reveal Operator
 * \{ */

static wmOperatorStatus edbm_reveal_exec(bContext *C, wmOperator *op)
{
  const bool select = RNA_boolean_get(op->ptr, "select");
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (EDBM_mesh_reveal(em, select)) {
      EDBMUpdate_Params params{};
      params.calc_looptris = true;
      params.calc_normals = false;
      params.is_destructive = false;
      EDBM_update(static_cast<Mesh *>(obedit->data), &params);
    }
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_reveal(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reveal Hidden";
  ot->idname = "MESH_OT_reveal";
  ot->description = "Reveal all hidden vertices, edges and faces";

  /* API callbacks. */
  ot->exec = edbm_reveal_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "select", true, "Select", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Recalculate Normals Operator
 * \{ */

static wmOperatorStatus edbm_normals_make_consistent_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const bool inside = RNA_boolean_get(op->ptr, "inside");

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    BMLoopNorEditDataArray *lnors_ed_arr = nullptr;

    if (inside) {
      /* Save custom normal data for later so we can flip them correctly. */
      lnors_ed_arr = flip_custom_normals_init_data(em->bm);
    }

    if (!EDBM_op_callf(em, op, "recalc_face_normals faces=%hf", BM_ELEM_SELECT)) {
      continue;
    }

    if (inside) {
      EDBM_op_callf(em, op, "reverse_faces faces=%hf flip_multires=%b", BM_ELEM_SELECT, true);
      flip_custom_normals(em->bm, lnors_ed_arr);
      if (lnors_ed_arr != nullptr) {
        BM_loop_normal_editdata_array_free(lnors_ed_arr);
      }
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_normals_make_consistent(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Recalculate Normals";
  ot->description = "Make face and vertex normals point either outside or inside the mesh";
  ot->idname = "MESH_OT_normals_make_consistent";

  /* API callbacks. */
  ot->exec = edbm_normals_make_consistent_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "inside", false, "Inside", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Smooth Vertices Operator
 * \{ */

static wmOperatorStatus edbm_do_smooth_vertex_exec(bContext *C, wmOperator *op)
{
  const float fac = RNA_float_get(op->ptr, "factor");

  const bool xaxis = RNA_boolean_get(op->ptr, "xaxis");
  const bool yaxis = RNA_boolean_get(op->ptr, "yaxis");
  const bool zaxis = RNA_boolean_get(op->ptr, "zaxis");
  int repeat = RNA_int_get(op->ptr, "repeat");

  if (!repeat) {
    repeat = 1;
  }

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  int tot_selected = 0, tot_locked = 0;
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    Mesh *mesh = static_cast<Mesh *>(obedit->data);
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    bool mirrx = false, mirry = false, mirrz = false;
    float clip_dist = 0.0f;
    const bool use_topology = (mesh->editflag & ME_EDIT_MIRROR_TOPO) != 0;

    if (em->bm->totvertsel == 0) {
      continue;
    }

    if (blender::ed::object::shape_key_report_if_locked(obedit, op->reports)) {
      tot_locked++;
      continue;
    }

    tot_selected++;

    /* mirror before smooth */
    if (((Mesh *)obedit->data)->symmetry & ME_SYMMETRY_X) {
      EDBM_verts_mirror_cache_begin(em, 0, false, true, false, use_topology);
    }

    /* if there is a mirror modifier with clipping, flag the verts that
     * are within tolerance of the plane(s) of reflection
     */
    LISTBASE_FOREACH (ModifierData *, md, &obedit->modifiers) {
      if (md->type == eModifierType_Mirror && (md->mode & eModifierMode_Realtime)) {
        MirrorModifierData *mmd = (MirrorModifierData *)md;

        if (mmd->flag & MOD_MIR_CLIPPING) {
          if (mmd->flag & MOD_MIR_AXIS_X) {
            mirrx = true;
          }
          if (mmd->flag & MOD_MIR_AXIS_Y) {
            mirry = true;
          }
          if (mmd->flag & MOD_MIR_AXIS_Z) {
            mirrz = true;
          }

          clip_dist = mmd->tolerance;
        }
      }
    }

    for (int i = 0; i < repeat; i++) {
      if (!EDBM_op_callf(
              em,
              op,
              "smooth_vert verts=%hv factor=%f mirror_clip_x=%b mirror_clip_y=%b mirror_clip_z=%b "
              "clip_dist=%f use_axis_x=%b use_axis_y=%b use_axis_z=%b",
              BM_ELEM_SELECT,
              fac,
              mirrx,
              mirry,
              mirrz,
              clip_dist,
              xaxis,
              yaxis,
              zaxis))
      {
        continue;
      }
    }

    /* NOTE: redundant calculation could be avoided if the EDBM API could skip calculation. */
    bool calc_normals = false;

    /* apply mirror */
    if (((Mesh *)obedit->data)->symmetry & ME_SYMMETRY_X) {
      EDBM_verts_mirror_apply(em, BM_ELEM_SELECT, 0);
      EDBM_verts_mirror_cache_end(em);
      calc_normals = true;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = calc_normals;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  if (tot_selected == 0 && !tot_locked) {
    BKE_report(op->reports, RPT_WARNING, "No selected vertex");
  }

  return tot_selected ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void MESH_OT_vertices_smooth(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Smooth Vertices";
  ot->description = "Flatten angles of selected vertices";
  ot->idname = "MESH_OT_vertices_smooth";

  /* API callbacks. */
  ot->exec = edbm_do_smooth_vertex_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_float_factor(
      ot->srna, "factor", 0.0f, -10.0f, 10.0f, "Smoothing", "Smoothing factor", 0.0f, 1.0f);
  RNA_def_int(
      ot->srna, "repeat", 1, 1, 1000, "Repeat", "Number of times to smooth the mesh", 1, 100);

  WM_operatortype_props_advanced_begin(ot);

  RNA_def_boolean(ot->srna, "xaxis", true, "X-Axis", "Smooth along the X axis");
  RNA_def_boolean(ot->srna, "yaxis", true, "Y-Axis", "Smooth along the Y axis");
  RNA_def_boolean(ot->srna, "zaxis", true, "Z-Axis", "Smooth along the Z axis");

  /* Set generic modal callbacks. */
  WM_operator_type_modal_from_exec_for_object_edit_coords(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Laplacian Smooth Vertices Operator
 * \{ */

static wmOperatorStatus edbm_do_smooth_laplacian_vertex_exec(bContext *C, wmOperator *op)
{
  int tot_selected = 0, tot_locked = 0;
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const float lambda_factor = RNA_float_get(op->ptr, "lambda_factor");
  const float lambda_border = RNA_float_get(op->ptr, "lambda_border");
  const bool usex = RNA_boolean_get(op->ptr, "use_x");
  const bool usey = RNA_boolean_get(op->ptr, "use_y");
  const bool usez = RNA_boolean_get(op->ptr, "use_z");
  const bool preserve_volume = RNA_boolean_get(op->ptr, "preserve_volume");
  int repeat = RNA_int_get(op->ptr, "repeat");

  if (!repeat) {
    repeat = 1;
  }

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    Mesh *mesh = static_cast<Mesh *>(obedit->data);
    bool use_topology = (mesh->editflag & ME_EDIT_MIRROR_TOPO) != 0;

    if (em->bm->totvertsel == 0) {
      continue;
    }

    if (blender::ed::object::shape_key_report_if_locked(obedit, op->reports)) {
      tot_locked++;
      continue;
    }

    tot_selected++;

    /* Mirror before smooth. */
    if (((Mesh *)obedit->data)->symmetry & ME_SYMMETRY_X) {
      EDBM_verts_mirror_cache_begin(em, 0, false, true, false, use_topology);
    }

    bool failed_repeat_loop = false;
    for (int i = 0; i < repeat; i++) {
      if (!EDBM_op_callf(em,
                         op,
                         "smooth_laplacian_vert verts=%hv lambda_factor=%f lambda_border=%f "
                         "use_x=%b use_y=%b use_z=%b preserve_volume=%b",
                         BM_ELEM_SELECT,
                         lambda_factor,
                         lambda_border,
                         usex,
                         usey,
                         usez,
                         preserve_volume))
      {
        failed_repeat_loop = true;
        break;
      }
    }
    if (failed_repeat_loop) {
      continue;
    }

    /* NOTE: redundant calculation could be avoided if the EDBM API could skip calculation. */
    bool calc_normals = false;

    /* Apply mirror. */
    if (((Mesh *)obedit->data)->symmetry & ME_SYMMETRY_X) {
      EDBM_verts_mirror_apply(em, BM_ELEM_SELECT, 0);
      EDBM_verts_mirror_cache_end(em);
      calc_normals = true;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = calc_normals;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  if (tot_selected == 0 && !tot_locked) {
    BKE_report(op->reports, RPT_WARNING, "No selected vertex");
  }

  return tot_selected ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void MESH_OT_vertices_smooth_laplacian(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Laplacian Smooth Vertices";
  ot->description = "Laplacian smooth of selected vertices";
  ot->idname = "MESH_OT_vertices_smooth_laplacian";

  /* API callbacks. */
  ot->exec = edbm_do_smooth_laplacian_vertex_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(
      ot->srna, "repeat", 1, 1, 1000, "Number of iterations to smooth the mesh", "", 1, 200);
  RNA_def_float(
      ot->srna, "lambda_factor", 1.0f, 1e-7f, 1000.0f, "Lambda factor", "", 1e-7f, 1000.0f);
  RNA_def_float(ot->srna,
                "lambda_border",
                5e-5f,
                1e-7f,
                1000.0f,
                "Lambda factor in border",
                "",
                1e-7f,
                1000.0f);

  WM_operatortype_props_advanced_begin(ot);

  RNA_def_boolean(ot->srna, "use_x", true, "Smooth X Axis", "Smooth object along X axis");
  RNA_def_boolean(ot->srna, "use_y", true, "Smooth Y Axis", "Smooth object along Y axis");
  RNA_def_boolean(ot->srna, "use_z", true, "Smooth Z Axis", "Smooth object along Z axis");
  RNA_def_boolean(ot->srna,
                  "preserve_volume",
                  true,
                  "Preserve Volume",
                  "Apply volume preservation after smooth");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Faces Smooth Shading Operator
 * \{ */

static void mesh_set_smooth_faces(BMEditMesh *em, short smooth)
{
  BMIter iter;
  BMFace *efa;

  if (em == nullptr) {
    return;
  }

  BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
      BM_elem_flag_set(efa, BM_ELEM_SMOOTH, smooth);
    }
  }
}

static wmOperatorStatus edbm_faces_shade_smooth_exec(bContext *C, wmOperator * /*op*/)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    mesh_set_smooth_faces(em, 1);
    EDBMUpdate_Params params{};
    params.calc_looptris = false;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_faces_shade_smooth(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Shade Smooth";
  ot->description = "Display faces smooth (using vertex normals)";
  ot->idname = "MESH_OT_faces_shade_smooth";

  /* API callbacks. */
  ot->exec = edbm_faces_shade_smooth_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Faces Flat Shading Operator
 * \{ */

static wmOperatorStatus edbm_faces_shade_flat_exec(bContext *C, wmOperator * /*op*/)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    mesh_set_smooth_faces(em, 0);
    EDBMUpdate_Params params{};
    params.calc_looptris = false;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_faces_shade_flat(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Shade Flat";
  ot->description = "Display faces flat";
  ot->idname = "MESH_OT_faces_shade_flat";

  /* API callbacks. */
  ot->exec = edbm_faces_shade_flat_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV/Color Rotate/Reverse Operator
 * \{ */

static wmOperatorStatus edbm_rotate_uvs_exec(bContext *C, wmOperator *op)
{
  /* get the direction from RNA */
  const bool use_ccw = RNA_boolean_get(op->ptr, "use_ccw");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    BMOperator bmop;

    EDBM_op_init(em, &bmop, op, "rotate_uvs faces=%hf use_ccw=%b", BM_ELEM_SELECT, use_ccw);

    BMO_op_exec(em->bm, &bmop);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = false;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus edbm_reverse_uvs_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    BMOperator bmop;

    EDBM_op_init(em, &bmop, op, "reverse_uvs faces=%hf", BM_ELEM_SELECT);

    BMO_op_exec(em->bm, &bmop);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }
    EDBMUpdate_Params params{};
    params.calc_looptris = false;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus edbm_rotate_colors_exec(bContext *C, wmOperator *op)
{
  /* get the direction from RNA */
  const bool use_ccw = RNA_boolean_get(op->ptr, "use_ccw");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (uint ob_index = 0; ob_index < objects.size(); ob_index++) {
    Object *ob = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(ob);
    if (em->bm->totfacesel == 0) {
      continue;
    }

    BMOperator bmop;

    Mesh *mesh = BKE_object_get_original_mesh(ob);
    AttributeOwner owner = AttributeOwner::from_id(&mesh->id);
    const CustomDataLayer *layer = BKE_attribute_search(
        owner, mesh->active_color_attribute, CD_MASK_COLOR_ALL, ATTR_DOMAIN_MASK_CORNER);
    if (!layer) {
      continue;
    }

    int color_index = BKE_attribute_to_index(
        owner, layer, ATTR_DOMAIN_MASK_CORNER, CD_MASK_COLOR_ALL);
    EDBM_op_init(em,
                 &bmop,
                 op,
                 "rotate_colors faces=%hf use_ccw=%b color_index=%i",
                 BM_ELEM_SELECT,
                 use_ccw,
                 color_index);

    BMO_op_exec(em->bm, &bmop);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    /* dependencies graph and notification stuff */
    EDBMUpdate_Params params{};
    params.calc_looptris = false;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(ob->data), &params);
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus edbm_reverse_colors_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    Mesh *mesh = BKE_object_get_original_mesh(obedit);
    AttributeOwner owner = AttributeOwner::from_id(&mesh->id);
    const CustomDataLayer *layer = BKE_attribute_search(
        owner, mesh->active_color_attribute, CD_MASK_COLOR_ALL, ATTR_DOMAIN_MASK_CORNER);
    if (!layer) {
      continue;
    }

    BMOperator bmop;

    int color_index = BKE_attribute_to_index(
        owner, layer, ATTR_DOMAIN_MASK_CORNER, CD_MASK_COLOR_ALL);
    EDBM_op_init(
        em, &bmop, op, "reverse_colors faces=%hf color_index=%i", BM_ELEM_SELECT, color_index);

    BMO_op_exec(em->bm, &bmop);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = false;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_uvs_rotate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Rotate UVs";
  ot->idname = "MESH_OT_uvs_rotate";
  ot->description = "Rotate UV coordinates inside faces";

  /* API callbacks. */
  ot->exec = edbm_rotate_uvs_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_boolean(ot->srna, "use_ccw", false, "Counter Clockwise", "");
}

void MESH_OT_uvs_reverse(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reverse UVs";
  ot->idname = "MESH_OT_uvs_reverse";
  ot->description = "Flip direction of UV coordinates inside faces";

  /* API callbacks. */
  ot->exec = edbm_reverse_uvs_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  // RNA_def_enum(ot->srna, "axis", axis_items, DIRECTION_CW, "Axis", "Axis to mirror UVs around");
}

void MESH_OT_colors_rotate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Rotate Colors";
  ot->idname = "MESH_OT_colors_rotate";
  ot->description = "Rotate face corner color attribute inside faces";

  /* API callbacks. */
  ot->exec = edbm_rotate_colors_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_boolean(ot->srna, "use_ccw", false, "Counter Clockwise", "");
}

void MESH_OT_colors_reverse(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reverse Colors";
  ot->idname = "MESH_OT_colors_reverse";
  ot->description = "Flip direction of face corner color attribute inside faces";

  /* API callbacks. */
  ot->exec = edbm_reverse_colors_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
#if 0
  RNA_def_enum(ot->srna, "axis", axis_items, DIRECTION_CW, "Axis", "Axis to mirror colors around");
#endif
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Merge Vertices Operator
 * \{ */

enum {
  MESH_MERGE_LAST = 1,
  MESH_MERGE_CENTER = 3,
  MESH_MERGE_CURSOR = 4,
  MESH_MERGE_COLLAPSE = 5,
  MESH_MERGE_FIRST = 6,
};

static bool merge_firstlast(BMEditMesh *em,
                            const bool use_first,
                            const bool use_uvmerge,
                            wmOperator *wmop)
{
  BMVert *mergevert;
  BMEditSelection *ese;

  /* operator could be called directly from shortcut or python,
   * so do extra check for data here
   */

  /* While #merge_type_itemf does a sanity check, this operation runs on all edit-mode objects.
   * Some of them may not have the expected selection state. */
  if (use_first == false) {
    if (!em->bm->selected.last || ((BMEditSelection *)em->bm->selected.last)->htype != BM_VERT) {
      return false;
    }

    ese = static_cast<BMEditSelection *>(em->bm->selected.last);
    mergevert = (BMVert *)ese->ele;
  }
  else {
    if (!em->bm->selected.first || ((BMEditSelection *)em->bm->selected.first)->htype != BM_VERT) {
      return false;
    }

    ese = static_cast<BMEditSelection *>(em->bm->selected.first);
    mergevert = (BMVert *)ese->ele;
  }

  if (!BM_elem_flag_test(mergevert, BM_ELEM_SELECT)) {
    return false;
  }

  if (use_uvmerge) {
    if (!EDBM_op_callf(
            em, wmop, "pointmerge_facedata verts=%hv vert_snap=%e", BM_ELEM_SELECT, mergevert))
    {
      return false;
    }
  }

  if (!EDBM_op_callf(em, wmop, "pointmerge verts=%hv merge_co=%v", BM_ELEM_SELECT, mergevert->co))
  {
    return false;
  }

  return true;
}

static bool merge_target(BMEditMesh *em,
                         Scene *scene,
                         Object *ob,
                         const bool use_cursor,
                         const bool use_uvmerge,
                         wmOperator *wmop)
{
  BMIter iter;
  BMVert *v;
  float co[3], cent[3] = {0.0f, 0.0f, 0.0f};
  const float *vco = nullptr;

  if (use_cursor) {
    vco = scene->cursor.location;
    copy_v3_v3(co, vco);
    invert_m4_m4(ob->runtime->world_to_object.ptr(), ob->object_to_world().ptr());
    mul_m4_v3(ob->world_to_object().ptr(), co);
  }
  else {
    float fac;
    int i = 0;
    BM_ITER_MESH (v, &iter, em->bm, BM_VERTS_OF_MESH) {
      if (!BM_elem_flag_test(v, BM_ELEM_SELECT)) {
        continue;
      }
      add_v3_v3(cent, v->co);
      i++;
    }

    if (!i) {
      return false;
    }

    fac = 1.0f / float(i);
    mul_v3_fl(cent, fac);
    copy_v3_v3(co, cent);
    vco = co;
  }

  if (!vco) {
    return false;
  }

  if (use_uvmerge) {
    if (!EDBM_op_callf(em, wmop, "average_vert_facedata verts=%hv", BM_ELEM_SELECT)) {
      return false;
    }
  }

  if (!EDBM_op_callf(em, wmop, "pointmerge verts=%hv merge_co=%v", BM_ELEM_SELECT, co)) {
    return false;
  }

  return true;
}

static wmOperatorStatus edbm_merge_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  const int type = RNA_enum_get(op->ptr, "type");
  const bool uvs = RNA_boolean_get(op->ptr, "uvs");

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totvertsel == 0) {
      continue;
    }

    BM_custom_loop_normals_to_vector_layer(em->bm);

    bool ok = false;
    switch (type) {
      case MESH_MERGE_CENTER:
        ok = merge_target(em, scene, obedit, false, uvs, op);
        break;
      case MESH_MERGE_CURSOR:
        ok = merge_target(em, scene, obedit, true, uvs, op);
        break;
      case MESH_MERGE_LAST:
        ok = merge_firstlast(em, false, uvs, op);
        break;
      case MESH_MERGE_FIRST:
        ok = merge_firstlast(em, true, uvs, op);
        break;
      case MESH_MERGE_COLLAPSE:
        ok = EDBM_op_callf(em, op, "collapse edges=%he uvs=%b", BM_ELEM_SELECT, uvs);
        break;
      default:
        BLI_assert(0);
        break;
    }

    if (!ok) {
      continue;
    }

    BM_custom_loop_normals_from_vector_layer(em->bm, false);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);

    /* once collapsed, we can't have edge/face selection */
    if ((em->selectmode & SCE_SELECT_VERTEX) == 0) {
      EDBM_flag_disable_all(em, BM_ELEM_SELECT);
    }
    /* Only active object supported, see comment below. */
    if (ELEM(type, MESH_MERGE_FIRST, MESH_MERGE_LAST)) {
      break;
    }
  }

  return OPERATOR_FINISHED;
}

static const EnumPropertyItem merge_type_items[] = {
    {MESH_MERGE_CENTER, "CENTER", 0, "At Center", ""},
    {MESH_MERGE_CURSOR, "CURSOR", 0, "At Cursor", ""},
    {MESH_MERGE_COLLAPSE, "COLLAPSE", 0, "Collapse", ""},
    {MESH_MERGE_FIRST, "FIRST", 0, "At First", ""},
    {MESH_MERGE_LAST, "LAST", 0, "At Last", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem *merge_type_itemf(bContext *C,
                                                PointerRNA * /*ptr*/,
                                                PropertyRNA * /*prop*/,
                                                bool *r_free)
{
  if (!C) { /* needed for docs */
    return merge_type_items;
  }

  Object *obedit = CTX_data_edit_object(C);
  if (obedit && obedit->type == OB_MESH) {
    EnumPropertyItem *item = nullptr;
    int totitem = 0;
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    /* Keep these first so that their automatic shortcuts don't change. */
    RNA_enum_items_add_value(&item, &totitem, merge_type_items, MESH_MERGE_CENTER);
    RNA_enum_items_add_value(&item, &totitem, merge_type_items, MESH_MERGE_CURSOR);
    RNA_enum_items_add_value(&item, &totitem, merge_type_items, MESH_MERGE_COLLAPSE);

    /* Only active object supported:
     * In practice it doesn't make sense to run this operation on non-active meshes
     * since selecting will activate - we could have a separate code-path for these but it's a
     * hassle for now just apply to the active (first) object. */
    if (em->selectmode & SCE_SELECT_VERTEX) {
      if (em->bm->selected.first && em->bm->selected.last &&
          ((BMEditSelection *)em->bm->selected.first)->htype == BM_VERT &&
          ((BMEditSelection *)em->bm->selected.last)->htype == BM_VERT)
      {
        RNA_enum_items_add_value(&item, &totitem, merge_type_items, MESH_MERGE_FIRST);
        RNA_enum_items_add_value(&item, &totitem, merge_type_items, MESH_MERGE_LAST);
      }
      else if (em->bm->selected.first &&
               ((BMEditSelection *)em->bm->selected.first)->htype == BM_VERT)
      {
        RNA_enum_items_add_value(&item, &totitem, merge_type_items, MESH_MERGE_FIRST);
      }
      else if (em->bm->selected.last &&
               ((BMEditSelection *)em->bm->selected.last)->htype == BM_VERT)
      {
        RNA_enum_items_add_value(&item, &totitem, merge_type_items, MESH_MERGE_LAST);
      }
    }

    RNA_enum_item_end(&item, &totitem);

    *r_free = true;

    return item;
  }

  /* Get all items e.g. when creating keymap item. */
  return merge_type_items;
}

void MESH_OT_merge(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Merge";
  ot->description = "Merge selected vertices";
  ot->idname = "MESH_OT_merge";

  /* API callbacks. */
  ot->exec = edbm_merge_exec;
  ot->invoke = WM_menu_invoke;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(
      ot->srna, "type", merge_type_items, MESH_MERGE_CENTER, "Type", "Merge method to use");
  RNA_def_enum_funcs(ot->prop, merge_type_itemf);

  WM_operatortype_props_advanced_begin(ot);

  RNA_def_boolean(ot->srna, "uvs", false, "UVs", "Move UVs according to merge");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Merge By Distance Operator
 * \{ */

static wmOperatorStatus edbm_remove_doubles_exec(bContext *C, wmOperator *op)
{
  const float threshold = RNA_float_get(op->ptr, "threshold");
  const bool use_unselected = RNA_boolean_get(op->ptr, "use_unselected");
  const bool use_sharp_edge_from_normals = RNA_boolean_get(op->ptr, "use_sharp_edge_from_normals");

  int count_multi = 0;

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    /* Selection used as target with 'use_unselected'. */
    if (em->bm->totvertsel == 0) {
      continue;
    }

    BMOperator bmop;
    const int totvert_orig = em->bm->totvert;

    /* avoid losing selection state (select -> tags) */
    char htype_select;
    if (em->selectmode & SCE_SELECT_VERTEX) {
      htype_select = BM_VERT;
    }
    else if (em->selectmode & SCE_SELECT_EDGE) {
      htype_select = BM_EDGE;
    }
    else {
      htype_select = BM_FACE;
    }

    BM_custom_loop_normals_to_vector_layer(em->bm);

    /* store selection as tags */
    BM_mesh_elem_hflag_enable_test(em->bm, htype_select, BM_ELEM_TAG, true, true, BM_ELEM_SELECT);

    if (use_unselected) {
      EDBM_automerge(obedit, false, BM_ELEM_SELECT, threshold);
    }
    else {
      EDBM_op_init(em, &bmop, op, "find_doubles verts=%hv dist=%f", BM_ELEM_SELECT, threshold);

      BMO_op_exec(em->bm, &bmop);

      if (!EDBM_op_callf(em, op, "weld_verts targetmap=%S", &bmop, "targetmap.out")) {
        BMO_op_finish(em->bm, &bmop);
        continue;
      }

      if (!EDBM_op_finish(em, &bmop, op, true)) {
        continue;
      }
    }

    const int count = (totvert_orig - em->bm->totvert);

    /* restore selection from tags */
    BM_mesh_elem_hflag_enable_test(em->bm, htype_select, BM_ELEM_SELECT, true, true, BM_ELEM_TAG);
    EDBM_selectmode_flush(em);

    BM_custom_loop_normals_from_vector_layer(em->bm, use_sharp_edge_from_normals);

    if (count) {
      count_multi += count;
      EDBMUpdate_Params params{};
      params.calc_looptris = true;
      params.calc_normals = false;
      params.is_destructive = true;
      EDBM_update(static_cast<Mesh *>(obedit->data), &params);
    }
  }

  BKE_reportf(op->reports,
              RPT_INFO,
              count_multi == 1 ? RPT_("Removed %d vertex") : RPT_("Removed %d vertices"),
              count_multi);

  return OPERATOR_FINISHED;
}

void MESH_OT_remove_doubles(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Merge by Distance";
  ot->description = "Merge vertices based on their proximity";
  ot->idname = "MESH_OT_remove_doubles";

  /* API callbacks. */
  ot->exec = edbm_remove_doubles_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_float_distance(ot->srna,
                         "threshold",
                         1e-4f,
                         1e-6f,
                         50.0f,
                         "Merge Distance",
                         "Maximum distance between elements to merge",
                         1e-5f,
                         10.0f);
  RNA_def_boolean(ot->srna,
                  "use_unselected",
                  false,
                  "Unselected",
                  "Merge selected to other unselected vertices");

  RNA_def_boolean(ot->srna,
                  "use_sharp_edge_from_normals",
                  false,
                  "Sharp Edges",
                  "Calculate sharp edges using custom normal data (when available)");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shape Key Propagate Operator
 * \{ */

/* BMESH_TODO this should be properly encapsulated in a bmop.  but later. */
static bool shape_propagate(BMEditMesh *em, bool use_symmetry)
{
  BMIter iter;
  BMVert *eve = nullptr;
  float *co;
  int totshape = CustomData_number_of_layers(&em->bm->vdata, CD_SHAPEKEY);

  if (!CustomData_has_layer(&em->bm->vdata, CD_SHAPEKEY)) {
    return false;
  }

  BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
    if (!BM_elem_flag_test(eve, BM_ELEM_SELECT) || BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
      BMVert *mirr = use_symmetry ? EDBM_verts_mirror_get(em, eve) : nullptr;

      if (!mirr || !BM_elem_flag_test(mirr, BM_ELEM_SELECT) ||
          BM_elem_flag_test(mirr, BM_ELEM_HIDDEN))
      {
        continue;
      }
    }

    for (int i = 0; i < totshape; i++) {
      co = static_cast<float *>(
          CustomData_bmesh_get_n(&em->bm->vdata, eve->head.data, CD_SHAPEKEY, i));
      copy_v3_v3(co, eve->co);
    }
  }
  return true;
}

static wmOperatorStatus edbm_shape_propagate_to_all_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  int tot_shapekeys = 0;
  int tot_selected_verts_objects = 0;
  int tot_locked = 0;

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    Mesh *mesh = static_cast<Mesh *>(obedit->data);
    BMEditMesh *em = mesh->runtime->edit_mesh.get();

    if (em->bm->totvertsel == 0) {
      continue;
    }

    /* Check for locked shape keys. */
    if (blender::ed::object::shape_key_report_if_any_locked(obedit, op->reports)) {
      tot_locked++;
      continue;
    }

    tot_selected_verts_objects++;

    const bool use_symmetry = (mesh->symmetry & ME_SYMMETRY_X) != 0;

    if (use_symmetry) {
      const bool use_topology = (mesh->editflag & ME_EDIT_MIRROR_TOPO) != 0;

      EDBM_verts_mirror_cache_begin(em, 0, false, false, false, use_topology);
    }

    if (shape_propagate(em, use_symmetry)) {
      tot_shapekeys++;
    }

    if (use_symmetry) {
      EDBM_verts_mirror_cache_end(em);
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = false;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(mesh, &params);
  }

  if (tot_selected_verts_objects == 0) {
    if (!tot_locked) {
      BKE_report(op->reports, RPT_ERROR, "No selected vertex");
    }
    return OPERATOR_CANCELLED;
  }
  if (tot_shapekeys == 0) {
    BKE_report(op->reports, RPT_ERROR, "Mesh(es) do not have shape keys");
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_shape_propagate_to_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Shape Propagate";
  ot->description = "Apply selected vertex locations to all other shape keys";
  ot->idname = "MESH_OT_shape_propagate_to_all";

  /* API callbacks. */
  ot->exec = edbm_shape_propagate_to_all_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend from Shape Operator
 * \{ */

/* BMESH_TODO this should be properly encapsulated in a bmop.  but later. */
static wmOperatorStatus edbm_blend_from_shape_exec(bContext *C, wmOperator *op)
{
  Object *obedit_ref = CTX_data_edit_object(C);
  Mesh *me_ref = static_cast<Mesh *>(obedit_ref->data);
  Key *key_ref = me_ref->key;
  KeyBlock *kb_ref = nullptr;
  BMEditMesh *em_ref = me_ref->runtime->edit_mesh.get();
  BMVert *eve;
  BMIter iter;
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  float co[3], *sco;
  int totshape_ref = 0;

  const float blend = RNA_float_get(op->ptr, "blend");
  int shape_ref = RNA_enum_get(op->ptr, "shape");
  const bool use_add = RNA_boolean_get(op->ptr, "add");

  /* Sanity check. */
  totshape_ref = CustomData_number_of_layers(&em_ref->bm->vdata, CD_SHAPEKEY);

  if (totshape_ref == 0 || shape_ref < 0) {
    BKE_report(op->reports, RPT_ERROR, "Active mesh does not have shape keys");
    return OPERATOR_CANCELLED;
  }
  if (shape_ref >= totshape_ref) {
    /* This case occurs if operator was used before on object with more keys than current one. */
    shape_ref = 0; /* default to basis */
  }

  /* Get shape key - needed for finding reference shape (for add mode only). */
  if (key_ref) {
    kb_ref = static_cast<KeyBlock *>(BLI_findlink(&key_ref->block, shape_ref));
  }

  int tot_selected_verts_objects = 0, tot_locked = 0;
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    Mesh *mesh = static_cast<Mesh *>(obedit->data);
    Key *key = mesh->key;
    KeyBlock *kb = nullptr;
    BMEditMesh *em = mesh->runtime->edit_mesh.get();
    int shape;

    if (em->bm->totvertsel == 0) {
      continue;
    }

    if (blender::ed::object::shape_key_report_if_locked(obedit, op->reports)) {
      tot_locked++;
      continue;
    }

    tot_selected_verts_objects++;

    if (!key) {
      continue;
    }
    kb = BKE_keyblock_find_name(key, kb_ref->name);
    shape = BLI_findindex(&key->block, kb);

    if (kb) {
      const bool use_symmetry = (mesh->symmetry & ME_SYMMETRY_X) != 0;

      if (use_symmetry) {
        const bool use_topology = (mesh->editflag & ME_EDIT_MIRROR_TOPO) != 0;

        EDBM_verts_mirror_cache_begin(em, 0, false, true, false, use_topology);
      }

      /* Perform blending on selected vertices. */
      BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
        if (!BM_elem_flag_test(eve, BM_ELEM_SELECT) || BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
          continue;
        }

        /* Get coordinates of shapekey we're blending from. */
        sco = static_cast<float *>(
            CustomData_bmesh_get_n(&em->bm->vdata, eve->head.data, CD_SHAPEKEY, shape));
        copy_v3_v3(co, sco);

        if (use_add) {
          /* In add mode, we add relative shape key offset. */
          const float *rco = static_cast<const float *>(
              CustomData_bmesh_get_n(&em->bm->vdata, eve->head.data, CD_SHAPEKEY, kb->relative));
          sub_v3_v3v3(co, co, rco);

          madd_v3_v3fl(eve->co, co, blend);
        }
        else {
          /* In blend mode, we interpolate to the shape key. */
          interp_v3_v3v3(eve->co, eve->co, co, blend);
        }
      }

      if (use_symmetry) {
        EDBM_verts_mirror_apply(em, BM_ELEM_SELECT, 0);
        EDBM_verts_mirror_cache_end(em);
      }

      EDBMUpdate_Params params{};
      params.calc_looptris = true;
      params.calc_normals = true;
      params.is_destructive = false;
      EDBM_update(mesh, &params);
    }
  }

  if (tot_selected_verts_objects == 0 && !tot_locked) {
    BKE_report(op->reports, RPT_ERROR, "No selected vertex");
  }

  return tot_selected_verts_objects ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static const EnumPropertyItem *shape_itemf(bContext *C,
                                           PointerRNA * /*ptr*/,
                                           PropertyRNA * /*prop*/,
                                           bool *r_free)
{
  Object *obedit = CTX_data_edit_object(C);
  BMEditMesh *em;
  EnumPropertyItem *item = nullptr;
  int totitem = 0;

  if ((obedit && obedit->type == OB_MESH) && (em = BKE_editmesh_from_object(obedit)) &&
      CustomData_has_layer(&em->bm->vdata, CD_SHAPEKEY))
  {
    EnumPropertyItem tmp = {0, "", 0, "", ""};
    int a;

    for (a = 0; a < em->bm->vdata.totlayer; a++) {
      if (em->bm->vdata.layers[a].type != CD_SHAPEKEY) {
        continue;
      }

      tmp.value = totitem;
      tmp.identifier = em->bm->vdata.layers[a].name;
      tmp.name = em->bm->vdata.layers[a].name;
      /* RNA_enum_item_add sets totitem itself! */
      RNA_enum_item_add(&item, &totitem, &tmp);
    }
  }

  RNA_enum_item_end(&item, &totitem);
  *r_free = true;

  return item;
}

static void edbm_blend_from_shape_ui(bContext *C, wmOperator *op)
{
  uiLayout *layout = op->layout;
  Object *obedit = CTX_data_edit_object(C);
  Mesh *mesh = static_cast<Mesh *>(obedit->data);

  PointerRNA ptr_key = RNA_id_pointer_create((ID *)mesh->key);

  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  layout->prop_search(op->ptr, "shape", &ptr_key, "key_blocks", std::nullopt, ICON_SHAPEKEY_DATA);
  layout->prop(op->ptr, "blend", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout->prop(op->ptr, "add", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

void MESH_OT_blend_from_shape(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Blend from Shape";
  ot->description = "Blend in shape from a shape key";
  ot->idname = "MESH_OT_blend_from_shape";

  /* API callbacks. */
  ot->exec = edbm_blend_from_shape_exec;
  /* disable because search popup closes too easily */
  //  ot->invoke = WM_operator_props_popup_call;
  ot->ui = edbm_blend_from_shape_ui;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_enum(
      ot->srna, "shape", rna_enum_dummy_NULL_items, 0, "Shape", "Shape key to use for blending");
  RNA_def_enum_funcs(prop, shape_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE | PROP_NEVER_UNLINK);
  RNA_def_float(ot->srna, "blend", 1.0f, -1e3f, 1e3f, "Blend", "Blending factor", -2.0f, 2.0f);
  RNA_def_boolean(ot->srna, "add", true, "Add", "Add rather than blend between shapes");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Solidify Mesh Operator
 * \{ */

static wmOperatorStatus edbm_solidify_exec(bContext *C, wmOperator *op)
{
  const float thickness = RNA_float_get(op->ptr, "thickness");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if (em->bm->totfacesel == 0) {
      continue;
    }

    BMOperator bmop;

    if (!EDBM_op_init(em, &bmop, op, "solidify geom=%hf thickness=%f", BM_ELEM_SELECT, thickness))
    {
      continue;
    }

    /* deselect only the faces in the region to be solidified (leave wire
     * edges and loose verts selected, as there will be no corresponding
     * geometry selected below) */
    BMO_slot_buffer_hflag_disable(bm, bmop.slots_in, "geom", BM_FACE, BM_ELEM_SELECT, true);

    /* run the solidify operator */
    BMO_op_exec(bm, &bmop);

    /* select the newly generated faces */
    BMO_slot_buffer_hflag_enable(bm, bmop.slots_out, "geom.out", BM_FACE, BM_ELEM_SELECT, true);

    /* No need to flush the selection, any selection history is no longer valid. */
    BM_select_history_clear(bm);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_solidify(wmOperatorType *ot)
{
  PropertyRNA *prop;
  /* identifiers */
  ot->name = "Solidify";
  ot->description = "Create a solid skin by extruding, compensating for sharp angles";
  ot->idname = "MESH_OT_solidify";

  /* API callbacks. */
  ot->exec = edbm_solidify_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_float_distance(
      ot->srna, "thickness", 0.01f, -1e4f, 1e4f, "Thickness", "", -10.0f, 10.0f);
  RNA_def_property_ui_range(prop, -10.0, 10.0, 0.1, 4);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Separate Parts Operator
 * \{ */

enum {
  MESH_SEPARATE_SELECTED = 0,
  MESH_SEPARATE_MATERIAL = 1,
  MESH_SEPARATE_LOOSE = 2,
};

/** TODO: Use #mesh_separate_arrays since it's more efficient. */
static Base *mesh_separate_tagged(
    Main *bmain, Scene *scene, ViewLayer *view_layer, Base *base_old, BMesh *bm_old)
{
  Object *obedit = base_old->object;
  BMeshCreateParams create_params{};
  create_params.use_toolflags = true;
  BMesh *bm_new = BM_mesh_create(&bm_mesh_allocsize_default, &create_params);
  BM_mesh_elem_toolflags_ensure(bm_new); /* Needed for 'duplicate' BMO. */

  BM_mesh_copy_init_customdata(bm_new, bm_old, &bm_mesh_allocsize_default);

  /* Take into account user preferences for duplicating actions. */
  const eDupli_ID_Flags dupflag = eDupli_ID_Flags(USER_DUP_MESH | (U.dupflag & USER_DUP_ACT));
  Base *base_new = blender::ed::object::add_duplicate(bmain, scene, view_layer, base_old, dupflag);

  /* normally would call directly after but in this case delay recalc */
  // DAG_relations_tag_update(bmain);

  /* new in 2.5 */
  BKE_object_material_array_assign(bmain,
                                   base_new->object,
                                   BKE_object_material_array_p(obedit),
                                   *BKE_object_material_len_p(obedit),
                                   false);

  blender::ed::object::base_select(base_new, blender::ed::object::BA_SELECT);

  BMO_op_callf(bm_old,
               (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
               "duplicate geom=%hvef dest=%p",
               BM_ELEM_TAG,
               bm_new);
  BMO_op_callf(bm_old,
               (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
               "delete geom=%hvef context=%i",
               BM_ELEM_TAG,
               DEL_FACES);

  /* deselect loose data - this used to get deleted,
   * we could de-select edges and verts only, but this turns out to be less complicated
   * since de-selecting all skips selection flushing logic */
  BM_mesh_elem_hflag_disable_all(bm_old, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);

  BM_mesh_normals_update(bm_new);

  BMeshToMeshParams to_mesh_params{};
  BM_mesh_bm_to_me(bmain, bm_new, static_cast<Mesh *>(base_new->object->data), &to_mesh_params);

  BM_mesh_free(bm_new);
  ((Mesh *)base_new->object->data)->runtime->edit_mesh = nullptr;

  return base_new;
}

static Base *mesh_separate_arrays(Main *bmain,
                                  Scene *scene,
                                  ViewLayer *view_layer,
                                  Base *base_old,
                                  BMesh *bm_old,
                                  BMVert **verts,
                                  uint verts_len,
                                  BMEdge **edges,
                                  uint edges_len,
                                  BMFace **faces,
                                  uint faces_len)
{
  BMAllocTemplate bm_new_allocsize{};
  bm_new_allocsize.totvert = verts_len;
  bm_new_allocsize.totedge = edges_len;
  bm_new_allocsize.totloop = faces_len * 3;
  bm_new_allocsize.totface = faces_len;

  const bool use_custom_normals = (bm_old->lnor_spacearr != nullptr);

  Object *obedit = base_old->object;

  BMeshCreateParams create_params{};
  BMesh *bm_new = BM_mesh_create(&bm_new_allocsize, &create_params);

  if (use_custom_normals) {
    /* Needed so the temporary normal layer is copied too. */
    BM_mesh_copy_init_customdata_all_layers(bm_new, bm_old, BM_ALL, &bm_new_allocsize);
  }
  else {
    BM_mesh_copy_init_customdata(bm_new, bm_old, &bm_new_allocsize);
  }

  /* Take into account user preferences for duplicating actions. */
  const eDupli_ID_Flags dupflag = eDupli_ID_Flags(USER_DUP_MESH | (U.dupflag & USER_DUP_ACT));
  Base *base_new = blender::ed::object::add_duplicate(bmain, scene, view_layer, base_old, dupflag);

  /* normally would call directly after but in this case delay recalc */
  // DAG_relations_tag_update(bmain);

  /* new in 2.5 */
  BKE_object_material_array_assign(bmain,
                                   base_new->object,
                                   BKE_object_material_array_p(obedit),
                                   *BKE_object_material_len_p(obedit),
                                   false);

  blender::ed::object::base_select(base_new, blender::ed::object::BA_SELECT);

  BM_mesh_copy_arrays(bm_old, bm_new, verts, verts_len, edges, edges_len, faces, faces_len);

  if (use_custom_normals) {
    BM_custom_loop_normals_from_vector_layer(bm_new, false);
  }

  for (uint i = 0; i < verts_len; i++) {
    BM_vert_kill(bm_old, verts[i]);
  }
  BMeshToMeshParams to_mesh_params{};
  BM_mesh_bm_to_me(bmain, bm_new, static_cast<Mesh *>(base_new->object->data), &to_mesh_params);

  BM_mesh_free(bm_new);
  ((Mesh *)base_new->object->data)->runtime->edit_mesh = nullptr;

  return base_new;
}

static bool mesh_separate_selected(
    Main *bmain, Scene *scene, ViewLayer *view_layer, Base *base_old, BMesh *bm_old)
{
  /* we may have tags from previous operators */
  BM_mesh_elem_hflag_disable_all(bm_old, BM_FACE | BM_EDGE | BM_VERT, BM_ELEM_TAG, false);

  /* sel -> tag */
  BM_mesh_elem_hflag_enable_test(
      bm_old, BM_FACE | BM_EDGE | BM_VERT, BM_ELEM_TAG, true, false, BM_ELEM_SELECT);

  return (mesh_separate_tagged(bmain, scene, view_layer, base_old, bm_old) != nullptr);
}

/**
 * Sets an object to a single material. from one of its slots.
 *
 * \note This could be used for split-by-material for non mesh types.
 * \note This could take material data from another object or args.
 */
static void mesh_separate_material_assign_mat_nr(Main *bmain, Object *ob, const short mat_nr)
{
  ID *obdata = static_cast<ID *>(ob->data);

  const short *totcolp = BKE_id_material_len_p(obdata);
  Material ***matarar = BKE_id_material_array_p(obdata);

  if ((totcolp && matarar) == 0) {
    BLI_assert(0);
    return;
  }

  if (*totcolp) {
    Material *ma_ob;
    Material *ma_obdata;
    char matbit;

    if (mat_nr < ob->totcol) {
      ma_ob = ob->mat[mat_nr];
      matbit = ob->matbits[mat_nr];
    }
    else {
      ma_ob = nullptr;
      matbit = 0;
    }

    if (mat_nr < *totcolp) {
      ma_obdata = (*matarar)[mat_nr];
    }
    else {
      ma_obdata = nullptr;
    }

    BKE_id_material_clear(bmain, obdata);
    BKE_id_material_resize(bmain, obdata, 1, true);
    BKE_objects_materials_sync_length_all(bmain, obdata);

    ob->mat[0] = ma_ob;
    id_us_plus((ID *)ma_ob);
    ob->matbits[0] = matbit;
    (*matarar)[0] = ma_obdata;
    id_us_plus((ID *)ma_obdata);
  }
  else {
    BKE_id_material_clear(bmain, obdata);
  }
}

static bool mesh_separate_material(
    Main *bmain, Scene *scene, ViewLayer *view_layer, Base *base_old, BMesh *bm_old)
{
  BMFace *f_cmp, *f;
  BMIter iter;
  bool result = false;

  while ((f_cmp = static_cast<BMFace *>(BM_iter_at_index(bm_old, BM_FACES_OF_MESH, nullptr, 0)))) {
    Base *base_new;
    const short mat_nr = f_cmp->mat_nr;
    int tot = 0;

    BM_mesh_elem_hflag_disable_all(bm_old, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

    BM_ITER_MESH (f, &iter, bm_old, BM_FACES_OF_MESH) {
      if (f->mat_nr == mat_nr) {
        BMLoop *l_iter;
        BMLoop *l_first;

        BM_elem_flag_enable(f, BM_ELEM_TAG);
        l_iter = l_first = BM_FACE_FIRST_LOOP(f);
        do {
          BM_elem_flag_enable(l_iter->v, BM_ELEM_TAG);
          BM_elem_flag_enable(l_iter->e, BM_ELEM_TAG);
        } while ((l_iter = l_iter->next) != l_first);

        tot++;
      }
    }

    /* leave the current object with some materials */
    if (tot == bm_old->totface) {
      mesh_separate_material_assign_mat_nr(bmain, base_old->object, mat_nr);

      /* since we're in editmode, must set faces here */
      BM_ITER_MESH (f, &iter, bm_old, BM_FACES_OF_MESH) {
        f->mat_nr = 0;
      }
      break;
    }

    /* Move selection into a separate object */
    base_new = mesh_separate_tagged(bmain, scene, view_layer, base_old, bm_old);
    if (base_new) {
      mesh_separate_material_assign_mat_nr(bmain, base_new->object, mat_nr);
    }

    result |= (base_new != nullptr);
  }

  return result;
}

static bool mesh_separate_loose(
    Main *bmain, Scene *scene, ViewLayer *view_layer, Base *base_old, BMesh *bm_old)
{
  /* Without this, we duplicate the object mode mesh for each loose part.
   * This can get very slow especially for large meshes with many parts
   * which would duplicate the mesh on entering edit-mode. */
  const bool clear_object_data = true;

  bool result = false;

  blender::Array<BMVert *> vert_groups(bm_old->totvert);
  blender::Array<BMEdge *> edge_groups(bm_old->totedge);
  blender::Array<BMFace *> face_groups(bm_old->totface);

  int(*groups)[3] = nullptr;
  int groups_len = BM_mesh_calc_edge_groups_as_arrays(
      bm_old, vert_groups.data(), edge_groups.data(), face_groups.data(), &groups);
  if (groups_len <= 1) {
    MEM_SAFE_FREE(groups);
    return false;
  }

  if (clear_object_data) {
    ED_mesh_geometry_clear(static_cast<Mesh *>(base_old->object->data));
  }

  BM_custom_loop_normals_to_vector_layer(bm_old);

  /* Separate out all groups except the first. */
  uint group_ofs[3] = {uint(groups[0][0]), uint(groups[0][1]), uint(groups[0][2])};
  for (int i = 1; i < groups_len; i++) {
    Base *base_new = mesh_separate_arrays(bmain,
                                          scene,
                                          view_layer,
                                          base_old,
                                          bm_old,
                                          vert_groups.data() + group_ofs[0],
                                          groups[i][0],
                                          edge_groups.data() + group_ofs[1],
                                          groups[i][1],
                                          face_groups.data() + group_ofs[2],
                                          groups[i][2]);
    result |= (base_new != nullptr);

    group_ofs[0] += groups[i][0];
    group_ofs[1] += groups[i][1];
    group_ofs[2] += groups[i][2];
  }

  Mesh *me_old = static_cast<Mesh *>(base_old->object->data);
  BM_mesh_elem_hflag_disable_all(bm_old, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);

  if (clear_object_data) {
    BMeshToMeshParams to_mesh_params{};
    to_mesh_params.update_shapekey_indices = true;
    BM_mesh_bm_to_me(nullptr, bm_old, me_old, &to_mesh_params);
  }

  MEM_freeN(groups);
  return result;
}

static wmOperatorStatus edbm_separate_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const int type = RNA_enum_get(op->ptr, "type");
  bool changed_multi = false;

  if (ED_operator_editmesh(C)) {
    uint empty_selection_len = 0;
    Vector<Base *> bases = BKE_view_layer_array_from_bases_in_edit_mode_unique_data(
        scene, view_layer, CTX_wm_view3d(C));
    for (const int base_index : bases.index_range()) {
      Base *base = bases[base_index];
      BMEditMesh *em = BKE_editmesh_from_object(base->object);

      if (type == 0) {
        if ((em->bm->totvertsel == 0) && (em->bm->totedgesel == 0) && (em->bm->totfacesel == 0)) {
          /* when all objects has no selection */
          if (++empty_selection_len == bases.size()) {
            BKE_report(op->reports, RPT_ERROR, "Nothing selected");
          }
          continue;
        }
      }

      /* editmode separate */
      bool changed = false;
      switch (type) {
        case MESH_SEPARATE_SELECTED:
          changed = mesh_separate_selected(bmain, scene, view_layer, base, em->bm);
          break;
        case MESH_SEPARATE_MATERIAL:
          changed = mesh_separate_material(bmain, scene, view_layer, base, em->bm);
          break;
        case MESH_SEPARATE_LOOSE:
          changed = mesh_separate_loose(bmain, scene, view_layer, base, em->bm);
          break;
        default:
          BLI_assert(0);
          break;
      }

      if (changed) {
        EDBMUpdate_Params params{};
        params.calc_looptris = true;
        params.calc_normals = false;
        params.is_destructive = true;
        EDBM_update(static_cast<Mesh *>(base->object->data), &params);
      }
      changed_multi |= changed;
    }
  }
  else {
    if (type == MESH_SEPARATE_SELECTED) {
      BKE_report(op->reports, RPT_ERROR, "Selection not supported in object mode");
      return OPERATOR_CANCELLED;
    }

    /* object mode separate */
    CTX_DATA_BEGIN (C, Base *, base_iter, selected_editable_bases) {
      Object *ob = base_iter->object;
      if (ob->type != OB_MESH) {
        continue;
      }
      Mesh *mesh = static_cast<Mesh *>(ob->data);
      if (!BKE_id_is_editable(bmain, &mesh->id)) {
        continue;
      }

      BMeshCreateParams create_params{};
      create_params.use_toolflags = true;
      BMesh *bm_old = BM_mesh_create(&bm_mesh_allocsize_default, &create_params);

      BMeshFromMeshParams from_mesh_params{};
      BM_mesh_bm_from_me(bm_old, mesh, &from_mesh_params);

      bool changed = false;
      switch (type) {
        case MESH_SEPARATE_MATERIAL:
          changed = mesh_separate_material(bmain, scene, view_layer, base_iter, bm_old);
          break;
        case MESH_SEPARATE_LOOSE:
          changed = mesh_separate_loose(bmain, scene, view_layer, base_iter, bm_old);
          break;
        default:
          BLI_assert(0);
          break;
      }

      if (changed) {
        BMeshToMeshParams to_mesh_params{};
        to_mesh_params.calc_object_remap = true;
        BM_mesh_bm_to_me(bmain, bm_old, mesh, &to_mesh_params);

        DEG_id_tag_update(&mesh->id, ID_RECALC_GEOMETRY_ALL_MODES);
        WM_event_add_notifier(C, NC_GEOM | ND_DATA, mesh);
      }

      BM_mesh_free(bm_old);

      changed_multi |= changed;
    }
    CTX_DATA_END;
  }

  if (changed_multi) {
    /* delay depsgraph recalc until all objects are duplicated */
    DEG_relations_tag_update(bmain);
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, nullptr);
    ED_outliner_select_sync_from_object_tag(C);

    return OPERATOR_FINISHED;
  }

  return OPERATOR_CANCELLED;
}

void MESH_OT_separate(wmOperatorType *ot)
{
  static const EnumPropertyItem prop_separate_types[] = {
      {MESH_SEPARATE_SELECTED, "SELECTED", 0, "Selection", ""},
      {MESH_SEPARATE_MATERIAL, "MATERIAL", 0, "By Material", ""},
      {MESH_SEPARATE_LOOSE, "LOOSE", 0, "By Loose Parts", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "Separate";
  ot->description = "Separate selected geometry into a new mesh";
  ot->idname = "MESH_OT_separate";

  /* API callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = edbm_separate_exec;
  ot->poll = ED_operator_scene_editable; /* object and editmode */

  /* flags */
  ot->flag = OPTYPE_UNDO;

  ot->prop = RNA_def_enum(
      ot->srna, "type", prop_separate_types, MESH_SEPARATE_SELECTED, "Type", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Triangle Fill Operator
 * \{ */

static wmOperatorStatus edbm_fill_exec(bContext *C, wmOperator *op)
{
  const bool use_beauty = RNA_boolean_get(op->ptr, "use_beauty");

  bool has_selected_edges = false, has_faces_filled = false;

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    const int totface_orig = em->bm->totface;

    if (em->bm->totedgesel == 0) {
      continue;
    }
    has_selected_edges = true;

    BMOperator bmop;
    if (!EDBM_op_init(
            em, &bmop, op, "triangle_fill edges=%he use_beauty=%b", BM_ELEM_SELECT, use_beauty))
    {
      continue;
    }

    BMO_op_exec(em->bm, &bmop);

    /* cancel if nothing was done */
    if (totface_orig == em->bm->totface) {
      EDBM_op_finish(em, &bmop, op, true);
      continue;
    }
    has_faces_filled = true;

    /* select new geometry */
    BMO_slot_buffer_hflag_enable(
        em->bm, bmop.slots_out, "geom.out", BM_FACE | BM_EDGE, BM_ELEM_SELECT, true);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  if (!has_selected_edges) {
    BKE_report(op->reports, RPT_ERROR, "No edges selected");
    return OPERATOR_CANCELLED;
  }

  if (!has_faces_filled) {
    BKE_report(op->reports, RPT_WARNING, "No faces filled");
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_fill(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Fill";
  ot->idname = "MESH_OT_fill";
  ot->description = "Fill a selected edge loop with faces";
  ot->translation_context = BLT_I18NCONTEXT_ID_MESH;

  /* API callbacks. */
  ot->exec = edbm_fill_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "use_beauty", true, "Beauty", "Use best triangulation division");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid Fill Operator
 * \{ */

static bool bm_edge_test_fill_grid_cb(BMEdge *e, void * /*bm_v*/)
{
  return BM_elem_flag_test_bool(e, BM_ELEM_SELECT);
}

static float edbm_fill_grid_vert_tag_angle(BMVert *v)
{
  BMIter iter;
  BMEdge *e_iter;
  BMVert *v_pair[2];
  int i = 0;
  BM_ITER_ELEM (e_iter, &iter, v, BM_EDGES_OF_VERT) {
    if (BM_elem_flag_test(e_iter, BM_ELEM_TAG)) {
      v_pair[i++] = BM_edge_other_vert(e_iter, v);
    }
  }
  BLI_assert(i == 2);

  return fabsf(float(M_PI) - angle_v3v3v3(v_pair[0]->co, v->co, v_pair[1]->co));
}

/**
 * non-essential utility function to select 2 open edge loops from a closed loop.
 */
static bool edbm_fill_grid_prepare(BMesh *bm, int offset, int *span_p, const bool span_calc)
{
  /* angle differences below this value are considered 'even'
   * in that they shouldn't be used to calculate corners used for the 'span' */
  const float eps_even = 1e-3f;
  BMEdge *e;
  BMIter iter;
  int count;
  int span = *span_p;

  ListBase eloops = {nullptr};
  BMEdgeLoopStore *el_store;
  // LinkData *el_store;

  count = BM_mesh_edgeloops_find(bm, &eloops, bm_edge_test_fill_grid_cb, bm);
  el_store = static_cast<BMEdgeLoopStore *>(eloops.first);

  if (count != 1) {
    /* Let the operator use the selection flags,
     * most likely failing with an error in this case. */
    BM_mesh_edgeloops_free(&eloops);
    return false;
  }

  /* Only tag edges that are part of a loop. */
  BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
    BM_elem_flag_disable(e, BM_ELEM_TAG);
  }
  const int verts_len = BM_edgeloop_length_get(el_store);
  const int edges_len = verts_len - (BM_edgeloop_is_closed(el_store) ? 0 : 1);
  BMEdge **edges = MEM_malloc_arrayN<BMEdge *>(edges_len, __func__);
  BM_edgeloop_edges_get(el_store, edges);
  for (int i = 0; i < edges_len; i++) {
    BM_elem_flag_enable(edges[i], BM_ELEM_TAG);
  }

  if (span_calc) {
    span = verts_len / 4;
  }
  else {
    span = min_ii(span, (verts_len / 2) - 1);
  }
  offset = mod_i(offset, verts_len);

  if ((count == 1) && ((verts_len & 1) == 0) && (verts_len == edges_len)) {

    /* be clever! detect 2 edge loops from one closed edge loop */
    ListBase *verts = BM_edgeloop_verts_get(el_store);
    BMVert *v_act = BM_mesh_active_vert_get(bm);
    LinkData *v_act_link;
    int i;

    if (v_act && (v_act_link = static_cast<LinkData *>(
                      BLI_findptr(verts, v_act, offsetof(LinkData, data)))))
    {
      /* pass */
    }
    else {
      /* find the vertex with the best angle (a corner vertex) */
      LinkData *v_link_best = nullptr;
      float angle_best = -1.0f;
      LISTBASE_FOREACH (LinkData *, v_link, verts) {
        const float angle = edbm_fill_grid_vert_tag_angle(static_cast<BMVert *>(v_link->data));
        if ((angle > angle_best) || (v_link_best == nullptr)) {
          angle_best = angle;
          v_link_best = v_link;
        }
      }

      v_act_link = v_link_best;
      v_act = static_cast<BMVert *>(v_act_link->data);
    }

    /* set this vertex first */
    BLI_listbase_rotate_first(verts, v_act_link);

    if (offset != 0) {
      v_act_link = static_cast<LinkData *>(BLI_findlink(verts, offset));
      v_act = static_cast<BMVert *>(v_act_link->data);
      BLI_listbase_rotate_first(verts, v_act_link);
    }

    /* Run again to update the edge order from the rotated vertex list. */
    BM_edgeloop_edges_get(el_store, edges);

    if (span_calc) {
      /* calculate the span by finding the next corner in 'verts'
       * we don't know what defines a corner exactly so find the 4 verts
       * in the loop with the greatest angle.
       * Tag them and use the first tagged vertex to calculate the span.
       *
       * NOTE: we may have already checked 'edbm_fill_grid_vert_tag_angle()' on each
       * vert, but advantage of de-duplicating is minimal. */
      SortPtrByFloat *ele_sort = MEM_malloc_arrayN<SortPtrByFloat>(verts_len, __func__);
      LinkData *v_link;
      for (v_link = static_cast<LinkData *>(verts->first), i = 0; v_link;
           v_link = v_link->next, i++)
      {
        BMVert *v = static_cast<BMVert *>(v_link->data);
        const float angle = edbm_fill_grid_vert_tag_angle(v);
        ele_sort[i].sort_value = angle;
        ele_sort[i].data = v_link;

        /* Do not allow the best corner or the diagonally opposite corner to be detected. */
        if (ELEM(i, 0, verts_len / 2)) {
          ele_sort[i].sort_value = 0;
        }
      }

      qsort(ele_sort, verts_len, sizeof(*ele_sort), BLI_sortutil_cmp_float_reverse);

      /* Check that we have at least 3 corners.
       * The excluded corners are the last and second from last elements (both reset to 0).
       * The best remaining corner is `ele_sort[0]`
       * if the angle on the best remaining corner is roughly the same as the third-last,
       * then we can't calculate 3+ corners - fallback to the even span. */
      if ((ele_sort[0].sort_value - ele_sort[verts_len - 3].sort_value) > eps_even) {
        span = BLI_findindex(verts, ele_sort[0].data);
      }
      MEM_freeN(ele_sort);
    }
    /* end span calc */
    int start = 0;

    /* The algorithm needs to iterate the shorter distance, between the best and second best vert.
     * If the second best vert is near the beginning of the loop, it starts at 0 and walks forward.
     * If, instead, the second best vert is near the end of the loop, then it starts at the second
     * best vertex and walks to the end of the loop. */
    if (span > verts_len / 2) {
      span = (verts_len)-span;
      start = (verts_len / 2) - span;
    }

    /* un-flag 'rails' */
    for (i = start; i < start + span; i++) {
      BM_elem_flag_disable(edges[i], BM_ELEM_TAG);
      BM_elem_flag_disable(edges[(verts_len / 2) + i], BM_ELEM_TAG);
    }
  }
  /* else let the bmesh-operator handle it */

  BM_mesh_edgeloops_free(&eloops);
  MEM_freeN(edges);

  *span_p = span;

  return true;
}

struct FillGridSplitJoin {
  BMOperator weld_op;
  BMOperator delete_op;
};

/**
 * Split the current selection into a separate island and prepare to rejoin it.
 *
 * This is done only when there are faces selected. Once split this way, fill_grid will
 * interpolate using only the data from the selected faces, not the data from the surrounding
 * faces. This matters for UV edges and face corner colors - the data from the faces being
 * replaced is the right data to use for the interpolation. This relies on the fact that the
 * "exterior" edge of an island is topologically the same as the "interior" edge around a hole.
 *
 * \param em: The edit-mesh to operate on.
 * \return the split join state.
 */
static FillGridSplitJoin *edbm_fill_grid_split_join_init(BMEditMesh *em)
{
  FillGridSplitJoin *split_join = MEM_callocN<FillGridSplitJoin>(__func__);

  /* Split the selection into an island. */
  BMOperator split_op;
  BMO_op_init(em->bm, &split_op, 0, "split");
  BMO_slot_buffer_from_enabled_hflag(
      em->bm, &split_op, split_op.slots_in, "geom", BM_FACE | BM_EDGE | BM_VERT, BM_ELEM_SELECT);
  BMO_op_exec(em->bm, &split_op);

  /* Setup the weld op that will undo the split.
   * Switch the selection to the corresponding edges on the island instead of the edges around the
   * hole, so fill_grid will interpolate using the face and loop data from the island.
   * Also create a new map for the weld, which maps pairs of verts instead of pairs of edges. */
  BMO_op_init(em->bm, &split_join->weld_op, 0, "weld_verts");
  BMOpSlot *weld_target_map = BMO_slot_get(split_join->weld_op.slots_in, "targetmap");
  BMOIter siter;
  BMEdge *e;
  BMO_ITER (e, &siter, split_op.slots_out, "boundary_map.out", 0) {
    BMEdge *e_dst = static_cast<BMEdge *>(BMO_iter_map_value_ptr(&siter));

    BLI_assert(e_dst);

    /* For edges, flip the selection from the edge of the hole to the edge of the island. */
    BM_elem_flag_enable(e_dst, BM_ELEM_SELECT);

    /* When these match, the source edge has been deleted. */
    if (e != e_dst) {
      BM_elem_flag_disable(e, BM_ELEM_SELECT);

      /* For verts, flip the selection from the edge of the hole to the edge of the island.
       * Also add it to the weld map. But check selection first. Don't try to add the same vert to
       * the map more than once. If the selection was changed false, it's already been processed.
       */
      if (BM_elem_flag_test(e->v1, BM_ELEM_SELECT)) {
        BM_elem_flag_disable(e->v1, BM_ELEM_SELECT);
        BM_elem_flag_enable(e_dst->v1, BM_ELEM_SELECT);
        BMO_slot_map_elem_insert(&split_join->weld_op, weld_target_map, e->v1, e_dst->v1);
      }
      if (BM_elem_flag_test(e->v2, BM_ELEM_SELECT)) {
        BM_elem_flag_disable(e->v2, BM_ELEM_SELECT);
        BM_elem_flag_enable(e_dst->v2, BM_ELEM_SELECT);
        BMO_slot_map_elem_insert(&split_join->weld_op, weld_target_map, e->v2, e_dst->v2);
      }
    }
  }

  /* Store the island for removal once it has been replaced by new fill_grid geometry. */
  BMO_op_init(em->bm, &split_join->delete_op, 0, "delete");
  BMO_slot_int_set(split_join->delete_op.slots_in, "context", DEL_FACES);
  BMO_slot_buffer_hflag_enable(
      em->bm, split_op.slots_out, "geom.out", BM_FACE, BM_ELEM_SELECT, false);
  BMO_slot_buffer_from_enabled_hflag(em->bm,
                                     &split_join->delete_op,
                                     split_join->delete_op.slots_in,
                                     "geom",
                                     BM_FACE,
                                     BM_ELEM_SELECT);

  /* Clean up the split operator. */
  BMO_op_finish(em->bm, &split_op);

  return split_join;
}

/**
 * Restore the mesh after split and fill_grid.
 *
 * \param em: The editmesh to work on
 * \param op: the wmOperator being run
 * \param split_join: the saved split join state.
 * \param changed: true, if the fill_grid that was run worked.
 */
static void edbm_fill_grid_split_join_finish(BMEditMesh *em,
                                             wmOperator *op,
                                             FillGridSplitJoin *split_join,
                                             bool changed)
{

  /* If fill_grid worked, delete the replaced faces. Otherwise, restore original selection. */
  if (changed) {
    BMO_op_exec(em->bm, &split_join->delete_op);
  }
  else {
    BMO_slot_buffer_hflag_enable(
        em->bm, split_join->delete_op.slots_in, "geom", BM_ALL_NOLOOP, BM_ELEM_SELECT, true);
  }
  BMO_op_finish(em->bm, &split_join->delete_op);

  /* If fill_grid created geometry from faces after those faces had been split
   * from the rest of the mesh, the geometry it generated will be inward-facing.
   * (using the fill_grid on an island instead of a hole is 'inside out'.) Fix it.
   * This is the same as #edbm_flip_normals_face_winding without the #EDBM_update
   * since that will happen later. */
  if (changed) {
    BMLoopNorEditDataArray *lnors_ed_arr = flip_custom_normals_init_data(em->bm);
    EDBM_op_callf(em, op, "reverse_faces faces=%hf flip_multires=%b", BM_ELEM_SELECT, true);
    if (lnors_ed_arr) {
      flip_custom_normals(em->bm, lnors_ed_arr);
      BM_loop_normal_editdata_array_free(lnors_ed_arr);
    }
  }

  /* Put the mesh back together. */
  BMO_op_exec(em->bm, &split_join->weld_op);
  BMO_op_finish(em->bm, &split_join->weld_op);

  MEM_freeN(split_join);
}

static wmOperatorStatus edbm_fill_grid_exec(bContext *C, wmOperator *op)
{
  const bool use_interp_simple = RNA_boolean_get(op->ptr, "use_interp_simple");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (uint ob_index = 0; ob_index < objects.size(); ob_index++) {

    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    if (em->bm->totedgesel == 0) {
      continue;
    }

    bool use_prepare = true;
    const bool use_smooth = edbm_add_edge_face__smooth_get(em->bm);

    FillGridSplitJoin *split_join = nullptr;
    if (em->bm->totfacesel != 0) {
      split_join = edbm_fill_grid_split_join_init(em);
    }

    const int totedge_orig = em->bm->totedge;
    const int totface_orig = em->bm->totface;

    if (use_prepare) {
      /* use when we have a single loop selected */
      PropertyRNA *prop_span = RNA_struct_find_property(op->ptr, "span");
      PropertyRNA *prop_offset = RNA_struct_find_property(op->ptr, "offset");
      bool calc_span;

      int span;
      int offset;

      /* Only reuse on redo because these settings need to match the current selection.
       * We never want to use them on other geometry, repeat last for eg, see: #60777. */
      if (((op->flag & OP_IS_INVOKE) || (op->flag & OP_IS_REPEAT_LAST) == 0) &&
          RNA_property_is_set(op->ptr, prop_span))
      {
        span = RNA_property_int_get(op->ptr, prop_span);
        calc_span = false;
      }
      else {
        /* Will be overwritten if possible. */
        span = 0;
        calc_span = true;
      }

      offset = RNA_property_int_get(op->ptr, prop_offset);

      /* in simple cases, move selection for tags, but also support more advanced cases */
      use_prepare = edbm_fill_grid_prepare(em->bm, offset, &span, calc_span);

      RNA_property_int_set(op->ptr, prop_span, span);
    }
    /* end tricky prepare code */

    bool changed = EDBM_op_call_and_selectf(
        em,
        op,
        "faces.out",
        true,
        "grid_fill edges=%he mat_nr=%i use_smooth=%b use_interp_simple=%b",
        use_prepare ? BM_ELEM_TAG : BM_ELEM_SELECT,
        em->mat_nr,
        use_smooth,
        use_interp_simple);

    /* Check that the results match the return value. */
    const bool has_geometry_changed = totedge_orig != em->bm->totedge ||
                                      totface_orig != em->bm->totface;
    BLI_assert(changed == has_geometry_changed);
    UNUSED_VARS_NDEBUG(has_geometry_changed);

    /* If a split/join in progress, finish it. */
    if (split_join) {
      edbm_fill_grid_split_join_finish(em, op, split_join, changed);
    }

    /* Update the object. */
    if (changed || split_join) {
      EDBMUpdate_Params params{};
      params.calc_looptris = true;
      params.calc_normals = false;
      params.is_destructive = true;
      EDBM_update(static_cast<Mesh *>(obedit->data), &params);
    }
    else {
      /* NOTE: Even if there were no mesh changes, #EDBM_op_finish() changed the BMesh pointer
       * inside of edit mesh, so need to tell evaluated objects to sync new BMesh pointer to their
       * edit mesh structures. */
      DEG_id_tag_update(&obedit->id, 0);
    }
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_fill_grid(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Grid Fill";
  ot->description = "Fill grid from two loops";
  ot->idname = "MESH_OT_fill_grid";

  /* API callbacks. */
  ot->exec = edbm_fill_grid_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_int(ot->srna, "span", 1, 1, 1000, "Span", "Number of grid columns", 1, 100);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_int(ot->srna,
                     "offset",
                     0,
                     -1000,
                     1000,
                     "Offset",
                     "Vertex that is the corner of the grid",
                     -100,
                     100);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_boolean(ot->srna,
                  "use_interp_simple",
                  false,
                  "Simple Blending",
                  "Use simple interpolation of grid vertices");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hole Fill Operator
 * \{ */

static wmOperatorStatus edbm_fill_holes_exec(bContext *C, wmOperator *op)
{
  const int sides = RNA_int_get(op->ptr, "sides");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totedgesel == 0) {
      continue;
    }

    if (!EDBM_op_call_and_selectf(
            em, op, "faces.out", true, "holes_fill edges=%he sides=%i", BM_ELEM_SELECT, sides))
    {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_fill_holes(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Fill Holes";
  ot->idname = "MESH_OT_fill_holes";
  ot->description = "Fill in holes (boundary edge loops)";

  /* API callbacks. */
  ot->exec = edbm_fill_holes_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "sides",
              4,
              0,
              1000,
              "Sides",
              "Number of sides in hole required to fill (zero fills all holes)",
              0,
              100);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Beauty Fill Operator
 * \{ */

static wmOperatorStatus edbm_beautify_fill_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  const float angle_max = M_PI;
  const float angle_limit = RNA_float_get(op->ptr, "angle_limit");
  char hflag;

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    if (angle_limit >= angle_max) {
      hflag = BM_ELEM_SELECT;
    }
    else {
      BMIter iter;
      BMEdge *e;

      BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
        BM_elem_flag_set(e,
                         BM_ELEM_TAG,
                         (BM_elem_flag_test(e, BM_ELEM_SELECT) &&
                          BM_edge_calc_face_angle_ex(e, angle_max) < angle_limit));
      }
      hflag = BM_ELEM_TAG;
    }

    if (!EDBM_op_call_and_selectf(
            em, op, "geom.out", true, "beautify_fill faces=%hf edges=%he", BM_ELEM_SELECT, hflag))
    {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_beautify_fill(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Beautify Faces";
  ot->idname = "MESH_OT_beautify_fill";
  ot->description = "Rearrange some faces to try to get less degenerated geometry";

  /* API callbacks. */
  ot->exec = edbm_beautify_fill_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  prop = RNA_def_float_rotation(ot->srna,
                                "angle_limit",
                                0,
                                nullptr,
                                0.0f,
                                DEG2RADF(180.0f),
                                "Max Angle",
                                "Angle limit",
                                0.0f,
                                DEG2RADF(180.0f));
  RNA_def_property_float_default(prop, DEG2RADF(180.0f));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Poke Face Operator
 * \{ */

static wmOperatorStatus edbm_poke_face_exec(bContext *C, wmOperator *op)
{
  const float offset = RNA_float_get(op->ptr, "offset");
  const bool use_relative_offset = RNA_boolean_get(op->ptr, "use_relative_offset");
  const int center_mode = RNA_enum_get(op->ptr, "center_mode");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    BMOperator bmop;
    EDBM_op_init(em,
                 &bmop,
                 op,
                 "poke faces=%hf offset=%f use_relative_offset=%b center_mode=%i",
                 BM_ELEM_SELECT,
                 offset,
                 use_relative_offset,
                 center_mode);
    BMO_op_exec(em->bm, &bmop);

    EDBM_flag_disable_all(em, BM_ELEM_SELECT);

    BMO_slot_buffer_hflag_enable(
        em->bm, bmop.slots_out, "verts.out", BM_VERT, BM_ELEM_SELECT, true);
    BMO_slot_buffer_hflag_enable(
        em->bm, bmop.slots_out, "faces.out", BM_FACE, BM_ELEM_SELECT, true);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_poke(wmOperatorType *ot)
{
  static const EnumPropertyItem poke_center_modes[] = {
      {BMOP_POKE_MEDIAN_WEIGHTED,
       "MEDIAN_WEIGHTED",
       0,
       "Weighted Median",
       "Weighted median face center"},
      {BMOP_POKE_MEDIAN, "MEDIAN", 0, "Median", "Median face center"},
      {BMOP_POKE_BOUNDS, "BOUNDS", 0, "Bounds", "Face bounds center"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "Poke Faces";
  ot->idname = "MESH_OT_poke";
  ot->description = "Split a face into a fan";

  /* API callbacks. */
  ot->exec = edbm_poke_face_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_float_distance(
      ot->srna, "offset", 0.0f, -1e3f, 1e3f, "Poke Offset", "Poke Offset", -1.0f, 1.0f);
  RNA_def_boolean(ot->srna,
                  "use_relative_offset",
                  false,
                  "Offset Relative",
                  "Scale the offset by surrounding geometry");
  RNA_def_enum(ot->srna,
               "center_mode",
               poke_center_modes,
               BMOP_POKE_MEDIAN_WEIGHTED,
               "Poke Center",
               "Poke face center calculation");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Triangulate Face Operator
 * \{ */

static wmOperatorStatus edbm_quads_convert_to_tris_exec(bContext *C, wmOperator *op)
{
  const int quad_method = RNA_enum_get(op->ptr, "quad_method");
  const int ngon_method = RNA_enum_get(op->ptr, "ngon_method");
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    BMOperator bmop;
    BMOIter oiter;
    BMFace *f;

    BM_custom_loop_normals_to_vector_layer(em->bm);

    EDBM_op_init(em,
                 &bmop,
                 op,
                 "triangulate faces=%hf quad_method=%i ngon_method=%i",
                 BM_ELEM_SELECT,
                 quad_method,
                 ngon_method);
    BMO_op_exec(em->bm, &bmop);

    /* select the output */
    BMO_slot_buffer_hflag_enable(
        em->bm, bmop.slots_out, "faces.out", BM_FACE, BM_ELEM_SELECT, true);

    /* remove the doubles */
    BMO_ITER (f, &oiter, bmop.slots_out, "face_map_double.out", BM_FACE) {
      BM_face_kill(em->bm, f);
    }

    EDBM_selectmode_flush(em);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    BM_custom_loop_normals_from_vector_layer(em->bm, false);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_quads_convert_to_tris(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Triangulate Faces";
  ot->idname = "MESH_OT_quads_convert_to_tris";
  ot->description = "Triangulate selected faces";

  /* API callbacks. */
  ot->exec = edbm_quads_convert_to_tris_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "quad_method",
               rna_enum_modifier_triangulate_quad_method_items,
               MOD_TRIANGULATE_QUAD_BEAUTY,
               "Quad Method",
               "Method for splitting the quads into triangles");
  RNA_def_enum(ot->srna,
               "ngon_method",
               rna_enum_modifier_triangulate_ngon_method_items,
               MOD_TRIANGULATE_NGON_BEAUTY,
               "N-gon Method",
               "Method for splitting the n-gons into triangles");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Convert to Quads Operator
 * \{ */

#if 0 /* See comments at top of bmo_join_triangles.cc */
#  define USE_JOIN_TRIANGLE_TESTING_API
#endif

static wmOperatorStatus edbm_tris_convert_to_quads_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  const bool do_seam = RNA_boolean_get(op->ptr, "seam");
  const bool do_sharp = RNA_boolean_get(op->ptr, "sharp");
  const bool do_uvs = RNA_boolean_get(op->ptr, "uvs");
  const bool do_vcols = RNA_boolean_get(op->ptr, "vcols");
  const bool do_materials = RNA_boolean_get(op->ptr, "materials");

#ifdef USE_JOIN_TRIANGLE_TESTING_API
  int merge_limit = RNA_int_get(op->ptr, "merge_limit");
  int neighbor_debug = RNA_int_get(op->ptr, "neighbor_debug");
#endif

  const float topology_influence = RNA_float_get(op->ptr, "topology_influence");
  const bool deselect_joined = RNA_boolean_get(op->ptr, "deselect_joined");

  float angle_face_threshold, angle_shape_threshold;
  bool is_face_pair;
  {
    int totelem_sel[3];
    EDBM_mesh_stats_multi(objects, nullptr, totelem_sel);
    is_face_pair = (totelem_sel[2] == 2);
  }

  /* When joining exactly 2 faces, no limit.
   * this is useful for one off joins while editing. */
  {
    PropertyRNA *prop;
    prop = RNA_struct_find_property(op->ptr, "face_threshold");
    if (is_face_pair && (RNA_property_is_set(op->ptr, prop) == false)) {
      angle_face_threshold = DEG2RADF(180.0f);
    }
    else {
      angle_face_threshold = RNA_property_float_get(op->ptr, prop);
    }

    prop = RNA_struct_find_property(op->ptr, "shape_threshold");
    if (is_face_pair && (RNA_property_is_set(op->ptr, prop) == false)) {
      angle_shape_threshold = DEG2RADF(180.0f);
    }
    else {
      angle_shape_threshold = RNA_property_float_get(op->ptr, prop);
    }
  }

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    BM_custom_loop_normals_to_vector_layer(em->bm);

    bool extend_selection = (deselect_joined == false);

#ifdef USE_JOIN_TRIANGLE_TESTING_API
    if (merge_limit != -1) {
      extend_selection = false;
    }
#endif

    if (!EDBM_op_call_and_selectf(
            em,
            op,
            "faces.out",
            extend_selection,
            "join_triangles faces=%hf angle_face_threshold=%f angle_shape_threshold=%f "
            "cmp_seam=%b cmp_sharp=%b cmp_uvs=%b cmp_vcols=%b cmp_materials=%b "
#ifdef USE_JOIN_TRIANGLE_TESTING_API
            "merge_limit=%i neighbor_debug=%i "
#endif
            "topology_influence=%f deselect_joined=%b",
            BM_ELEM_SELECT,
            angle_face_threshold,
            angle_shape_threshold,
            do_seam,
            do_sharp,
            do_uvs,
            do_vcols,
            do_materials,
#ifdef USE_JOIN_TRIANGLE_TESTING_API
            merge_limit + 1,
            neighbor_debug,
#endif
            topology_influence,
            deselect_joined))
    {
      continue;
    }

    if (deselect_joined) {
      /* When de-selecting faces outside of face mode:
       * failing to flush would leave an invalid selection. */
      if (em->selectmode & (SCE_SELECT_VERTEX | SCE_SELECT_EDGE)) {
        EDBM_selectmode_flush_ex(em, em->selectmode);
      }
    }

    BM_custom_loop_normals_from_vector_layer(em->bm, false);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

static void join_triangle_props(wmOperatorType *ot)
{
  PropertyRNA *prop;

#ifdef USE_JOIN_TRIANGLE_TESTING_API
  prop = RNA_def_int(ot->srna,
                     "merge_limit",
                     0,
                     -1,
                     INT_MAX,
                     "Merge Limit",
                     "Maximum number of merges",
                     -1,
                     INT_MAX);
  RNA_def_property_int_default(prop, -1);

  prop = RNA_def_int(ot->srna,
                     "neighbor_debug",
                     0,
                     0,
                     INT_MAX,
                     "Neighbor Debug",
                     "Neighbor to highlight",
                     0,
                     INT_MAX);
  RNA_def_property_int_default(prop, 0);
#endif

  prop = RNA_def_float_rotation(ot->srna,
                                "face_threshold",
                                0,
                                nullptr,
                                0.0f,
                                DEG2RADF(180.0f),
                                "Max Face Angle",
                                "Face angle limit",
                                0.0f,
                                DEG2RADF(180.0f));
  RNA_def_property_float_default(prop, DEG2RADF(40.0f));

  prop = RNA_def_float_rotation(ot->srna,
                                "shape_threshold",
                                0,
                                nullptr,
                                0.0f,
                                DEG2RADF(180.0f),
                                "Max Shape Angle",
                                "Shape angle limit",
                                0.0f,
                                DEG2RADF(180.0f));
  RNA_def_property_float_default(prop, DEG2RADF(40.0f));

  prop = RNA_def_float_factor(ot->srna,
                              "topology_influence",
                              0.0f,
                              0.0f,
                              2.0f,
                              "Topology Influence",
                              "How much to prioritize regular grids of quads as well as "
                              "quads that touch existing quads",
                              0.0f,
                              2.0f);

  RNA_def_boolean(ot->srna, "uvs", false, "Compare UVs", "");
  RNA_def_boolean(ot->srna, "vcols", false, "Compare Color Attributes", "");
  RNA_def_boolean(ot->srna, "seam", false, "Compare Seam", "");
  RNA_def_boolean(ot->srna, "sharp", false, "Compare Sharp", "");
  RNA_def_boolean(ot->srna, "materials", false, "Compare Materials", "");

  RNA_def_boolean(ot->srna,
                  "deselect_joined",
                  false,
                  "Deselect Joined",
                  "Only select remaining triangles that were not merged");
}

void MESH_OT_tris_convert_to_quads(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Triangles to Quads";
  ot->idname = "MESH_OT_tris_convert_to_quads";
  ot->description = "Merge triangles into four sided polygons where possible";

  /* API callbacks. */
  ot->exec = edbm_tris_convert_to_quads_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  join_triangle_props(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Decimate Operator
 *
 * \note The function to decimate is intended for use as a modifier,
 * while its handy allow access as a tool - this does cause access to be a little awkward
 * (passing selection as weights for eg).
 *
 * \{ */

static wmOperatorStatus edbm_decimate_exec(bContext *C, wmOperator *op)
{
  const float ratio = RNA_float_get(op->ptr, "ratio");
  bool use_vertex_group = RNA_boolean_get(op->ptr, "use_vertex_group");
  const float vertex_group_factor = RNA_float_get(op->ptr, "vertex_group_factor");
  const bool invert_vertex_group = RNA_boolean_get(op->ptr, "invert_vertex_group");
  const bool use_symmetry = RNA_boolean_get(op->ptr, "use_symmetry");
  const float symmetry_eps = 0.00002f;
  const int symmetry_axis = use_symmetry ? RNA_enum_get(op->ptr, "symmetry_axis") : -1;

  /* nop */
  if (ratio == 1.0f) {
    return OPERATOR_FINISHED;
  }

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    if (bm->totedgesel == 0) {
      continue;
    }

    float *vweights = MEM_malloc_arrayN<float>(bm->totvert, __func__);
    {
      const int cd_dvert_offset = CustomData_get_offset(&bm->vdata, CD_MDEFORMVERT);
      const int defbase_act = BKE_object_defgroup_active_index_get(obedit) - 1;

      if (use_vertex_group && (cd_dvert_offset == -1)) {
        BKE_report(op->reports, RPT_WARNING, "No active vertex group");
        use_vertex_group = false;
      }

      BMIter iter;
      BMVert *v;
      int i;
      BM_ITER_MESH_INDEX (v, &iter, bm, BM_VERTS_OF_MESH, i) {
        float weight = 0.0f;
        if (BM_elem_flag_test(v, BM_ELEM_SELECT)) {
          if (use_vertex_group) {
            const MDeformVert *dv = static_cast<const MDeformVert *>(
                BM_ELEM_CD_GET_VOID_P(v, cd_dvert_offset));
            weight = BKE_defvert_find_weight(dv, defbase_act);
            if (invert_vertex_group) {
              weight = 1.0f - weight;
            }
          }
          else {
            weight = 1.0f;
          }
        }

        vweights[i] = weight;
        BM_elem_index_set(v, i); /* set_inline */
      }
      bm->elem_index_dirty &= ~BM_VERT;
    }

    float ratio_adjust;

    if ((bm->totface == bm->totfacesel) || (ratio == 0.0f)) {
      ratio_adjust = ratio;
    }
    else {
      /**
       * Calculate a new ratio based on faces that could be removed during decimation.
       * needed so 0..1 has a meaningful range when operating on the selection.
       *
       * This doesn't have to be totally accurate,
       * but needs to be greater than the number of selected faces
       */

      int totface_basis = 0;
      int totface_adjacent = 0;
      BMIter iter;
      BMFace *f;
      BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
        /* count faces during decimation, ngons are triangulated */
        const int f_len = f->len > 4 ? (f->len - 2) : 1;
        totface_basis += f_len;

        BMLoop *l_iter, *l_first;
        l_iter = l_first = BM_FACE_FIRST_LOOP(f);
        do {
          if (vweights[BM_elem_index_get(l_iter->v)] != 0.0f) {
            totface_adjacent += f_len;
            break;
          }
        } while ((l_iter = l_iter->next) != l_first);
      }

      ratio_adjust = ratio;
      ratio_adjust = 1.0f - ratio_adjust;
      ratio_adjust *= float(totface_adjacent) / float(totface_basis);
      ratio_adjust = 1.0f - ratio_adjust;
    }

    BM_mesh_decimate_collapse(
        em->bm, ratio_adjust, vweights, vertex_group_factor, false, symmetry_axis, symmetry_eps);

    MEM_freeN(vweights);

    {
      short selectmode = em->selectmode;
      if ((selectmode & (SCE_SELECT_VERTEX | SCE_SELECT_EDGE)) == 0) {
        /* ensure we flush edges -> faces */
        selectmode |= SCE_SELECT_EDGE;
      }
      EDBM_selectmode_flush_ex(em, selectmode);
    }
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

static bool edbm_decimate_check(bContext * /*C*/, wmOperator * /*op*/)
{
  return true;
}

static void edbm_decimate_ui(bContext * /*C*/, wmOperator *op)
{
  uiLayout *layout = op->layout, *row, *col, *sub;

  layout->use_property_split_set(true);

  layout->prop(op->ptr, "ratio", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout->prop(op->ptr, "use_vertex_group", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col = &layout->column(false);
  col->active_set(RNA_boolean_get(op->ptr, "use_vertex_group"));
  col->prop(op->ptr, "vertex_group_factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(op->ptr, "invert_vertex_group", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(true, IFACE_("Symmetry"));
  row->prop(op->ptr, "use_symmetry", UI_ITEM_NONE, "", ICON_NONE);
  sub = &row->row(true);
  sub->active_set(RNA_boolean_get(op->ptr, "use_symmetry"));
  sub->prop(op->ptr, "symmetry_axis", UI_ITEM_R_EXPAND, std::nullopt, ICON_NONE);
}

void MESH_OT_decimate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Decimate Geometry";
  ot->idname = "MESH_OT_decimate";
  ot->description = "Simplify geometry by collapsing edges";

  /* API callbacks. */
  ot->exec = edbm_decimate_exec;
  ot->check = edbm_decimate_check;
  ot->ui = edbm_decimate_ui;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* NOTE: keep in sync with 'rna_def_modifier_decimate'. */
  RNA_def_float(ot->srna, "ratio", 1.0f, 0.0f, 1.0f, "Ratio", "", 0.0f, 1.0f);

  RNA_def_boolean(ot->srna,
                  "use_vertex_group",
                  false,
                  "Vertex Group",
                  "Use active vertex group as an influence");
  RNA_def_float(ot->srna,
                "vertex_group_factor",
                1.0f,
                0.0f,
                1000.0f,
                "Weight",
                "Vertex group strength",
                0.0f,
                10.0f);
  RNA_def_boolean(
      ot->srna, "invert_vertex_group", false, "Invert", "Invert vertex group influence");

  RNA_def_boolean(ot->srna, "use_symmetry", false, "Symmetry", "Maintain symmetry on an axis");

  RNA_def_enum(ot->srna, "symmetry_axis", rna_enum_axis_xyz_items, 1, "Axis", "Axis of symmetry");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dissolve Vertices Operator
 * \{ */

static void edbm_dissolve_prop__use_verts(wmOperatorType *ot, bool value, int flag)
{
  PropertyRNA *prop;

  prop = RNA_def_boolean(ot->srna,
                         "use_verts",
                         value,
                         "Dissolve Vertices",
                         "Dissolve remaining vertices which connect to only two edges");

  if (flag) {
    RNA_def_property_flag(prop, PropertyFlag(flag));
  }
}
static void edbm_dissolve_prop__use_face_split(wmOperatorType *ot)
{
  RNA_def_boolean(ot->srna,
                  "use_face_split",
                  false,
                  "Face Split",
                  "Split off face corners to maintain surrounding geometry");
}
static void edbm_dissolve_prop__use_boundary_tear(wmOperatorType *ot)
{
  RNA_def_boolean(ot->srna,
                  "use_boundary_tear",
                  false,
                  "Tear Boundary",
                  "Split off face corners instead of merging faces");
}
static void edbm_dissolve_prop__use_angle_threshold(wmOperatorType *ot, int flag)
{
  PropertyRNA *prop = RNA_def_float_rotation(
      ot->srna,
      "angle_threshold",
      0,
      nullptr,
      0.0f,
      DEG2RADF(180.0f),
      "Angle Threshold",
      "Remaining vertices which separate edge pairs are preserved if their edge angle exceeds "
      "this threshold.",
      0.0f,
      DEG2RADF(180.0f));
  RNA_def_property_float_default(prop, DEG2RADF(180.0f));
  if (flag) {
    RNA_def_property_flag(prop, PropertyFlag(flag));
  }
}

static wmOperatorStatus edbm_dissolve_verts_exec(bContext *C, wmOperator *op)
{
  const bool use_face_split = RNA_boolean_get(op->ptr, "use_face_split");
  const bool use_boundary_tear = RNA_boolean_get(op->ptr, "use_boundary_tear");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totvertsel == 0) {
      continue;
    }

    BM_custom_loop_normals_to_vector_layer(em->bm);

    if (!EDBM_op_callf(em,
                       op,
                       "dissolve_verts verts=%hv use_face_split=%b use_boundary_tear=%b",
                       BM_ELEM_SELECT,
                       use_face_split,
                       use_boundary_tear))
    {
      continue;
    }

    BM_custom_loop_normals_from_vector_layer(em->bm, false);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_dissolve_verts(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Dissolve Vertices";
  ot->description = "Dissolve vertices, merge edges and faces";
  ot->idname = "MESH_OT_dissolve_verts";

  /* API callbacks. */
  ot->exec = edbm_dissolve_verts_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  edbm_dissolve_prop__use_face_split(ot);
  edbm_dissolve_prop__use_boundary_tear(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dissolve Edges Operator
 * \{ */

static wmOperatorStatus edbm_dissolve_edges_exec(bContext *C, wmOperator *op)
{
  const bool use_verts = RNA_boolean_get(op->ptr, "use_verts");
  const bool use_face_split = RNA_boolean_get(op->ptr, "use_face_split");
  const float angle_threshold = RNA_float_get(op->ptr, "angle_threshold");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totedgesel == 0) {
      continue;
    }

    BM_custom_loop_normals_to_vector_layer(em->bm);

    if (!EDBM_op_callf(
            em,
            op,
            "dissolve_edges edges=%he use_verts=%b use_face_split=%b angle_threshold=%f",
            BM_ELEM_SELECT,
            use_verts,
            use_face_split,
            angle_threshold))
    {
      continue;
    }

    BM_custom_loop_normals_from_vector_layer(em->bm, false);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_dissolve_edges(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Dissolve Edges";
  ot->description = "Dissolve edges, merging faces";
  ot->idname = "MESH_OT_dissolve_edges";

  /* API callbacks. */
  ot->exec = edbm_dissolve_edges_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  edbm_dissolve_prop__use_verts(ot, true, 0);
  edbm_dissolve_prop__use_angle_threshold(ot, 0);
  edbm_dissolve_prop__use_face_split(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dissolve Faces Operator
 * \{ */

static wmOperatorStatus edbm_dissolve_faces_exec(bContext *C, wmOperator *op)
{
  const bool use_verts = RNA_boolean_get(op->ptr, "use_verts");
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    BM_custom_loop_normals_to_vector_layer(em->bm);

    if (!EDBM_op_call_and_selectf(em,
                                  op,
                                  "region.out",
                                  true,
                                  "dissolve_faces faces=%hf use_verts=%b",
                                  BM_ELEM_SELECT,
                                  use_verts))
    {
      continue;
    }

    BM_custom_loop_normals_from_vector_layer(em->bm, false);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_dissolve_faces(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Dissolve Faces";
  ot->description = "Dissolve faces";
  ot->idname = "MESH_OT_dissolve_faces";

  /* API callbacks. */
  ot->exec = edbm_dissolve_faces_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  edbm_dissolve_prop__use_verts(ot, false, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dissolve (Context Sensitive) Operator
 * \{ */

static wmOperatorStatus edbm_dissolve_mode_exec(bContext *C, wmOperator *op)
{
  Object *obedit = CTX_data_edit_object(C);
  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  PropertyRNA *prop;

  prop = RNA_struct_find_property(op->ptr, "use_verts");
  if (!RNA_property_is_set(op->ptr, prop)) {
    /* always enable in edge-mode */
    if ((em->selectmode & SCE_SELECT_FACE) == 0) {
      RNA_property_boolean_set(op->ptr, prop, true);
    }
  }

  if (em->selectmode & SCE_SELECT_VERTEX) {
    return edbm_dissolve_verts_exec(C, op);
  }
  if (em->selectmode & SCE_SELECT_EDGE) {
    return edbm_dissolve_edges_exec(C, op);
  }
  return edbm_dissolve_faces_exec(C, op);
}

static bool dissolve_mode_poll_property(const bContext *C, wmOperator *op, const PropertyRNA *prop)
{
  UNUSED_VARS(op);

  const char *prop_id = RNA_property_identifier(prop);

  Object *obedit = CTX_data_edit_object(C);
  const BMEditMesh *em = BKE_editmesh_from_object(obedit);
  bool is_edge_select_mode = false;

  if (em->selectmode & SCE_SELECT_VERTEX) {
    /* Pass. */
  }
  if (em->selectmode & SCE_SELECT_EDGE) {
    is_edge_select_mode = true;
  }

  if (!is_edge_select_mode) {
    /* Angle Threshold is only used in edge select mode. */
    if (STREQ(prop_id, "angle_threshold")) {
      return false;
    }
  }
  return true;
}

void MESH_OT_dissolve_mode(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Dissolve Selection";
  ot->description = "Dissolve geometry based on the selection mode";
  ot->idname = "MESH_OT_dissolve_mode";

  /* API callbacks. */
  ot->exec = edbm_dissolve_mode_exec;
  ot->poll = ED_operator_editmesh;
  ot->poll_property = dissolve_mode_poll_property;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  edbm_dissolve_prop__use_verts(ot, false, PROP_SKIP_SAVE);
  edbm_dissolve_prop__use_angle_threshold(ot, PROP_SKIP_SAVE);
  edbm_dissolve_prop__use_face_split(ot);
  edbm_dissolve_prop__use_boundary_tear(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Limited Dissolve Operator
 * \{ */

static wmOperatorStatus edbm_dissolve_limited_exec(bContext *C, wmOperator *op)
{
  const float angle_limit = RNA_float_get(op->ptr, "angle_limit");
  const bool use_dissolve_boundaries = RNA_boolean_get(op->ptr, "use_dissolve_boundaries");
  const int delimit = RNA_enum_get(op->ptr, "delimit");
  char dissolve_flag;

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if ((bm->totvertsel == 0) && (bm->totedgesel == 0) && (bm->totfacesel == 0)) {
      continue;
    }

    BM_custom_loop_normals_to_vector_layer(em->bm);

    if (em->selectmode == SCE_SELECT_FACE) {
      /* flush selection to tags and untag edges/verts with partially selected faces */
      BMIter iter;
      BMIter liter;

      BMElem *ele;
      BMFace *f;
      BMLoop *l;

      BM_ITER_MESH (ele, &iter, bm, BM_VERTS_OF_MESH) {
        BM_elem_flag_set(ele, BM_ELEM_TAG, BM_elem_flag_test(ele, BM_ELEM_SELECT));
      }
      BM_ITER_MESH (ele, &iter, bm, BM_EDGES_OF_MESH) {
        BM_elem_flag_set(ele, BM_ELEM_TAG, BM_elem_flag_test(ele, BM_ELEM_SELECT));
      }

      BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
        if (!BM_elem_flag_test(f, BM_ELEM_SELECT)) {
          BM_ITER_ELEM (l, &liter, f, BM_LOOPS_OF_FACE) {
            BM_elem_flag_disable(l->v, BM_ELEM_TAG);
            BM_elem_flag_disable(l->e, BM_ELEM_TAG);
          }
        }
      }

      dissolve_flag = BM_ELEM_TAG;
    }
    else {
      dissolve_flag = BM_ELEM_SELECT;
    }

    EDBM_op_call_and_selectf(
        em,
        op,
        "region.out",
        true,
        "dissolve_limit edges=%he verts=%hv angle_limit=%f use_dissolve_boundaries=%b delimit=%i",
        dissolve_flag,
        dissolve_flag,
        angle_limit,
        use_dissolve_boundaries,
        delimit);

    BM_custom_loop_normals_from_vector_layer(em->bm, false);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_dissolve_limited(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Limited Dissolve";
  ot->idname = "MESH_OT_dissolve_limited";
  ot->description =
      "Dissolve selected edges and vertices, limited by the angle of surrounding geometry";

  /* API callbacks. */
  ot->exec = edbm_dissolve_limited_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_float_rotation(ot->srna,
                                "angle_limit",
                                0,
                                nullptr,
                                0.0f,
                                DEG2RADF(180.0f),
                                "Max Angle",
                                "Angle limit",
                                0.0f,
                                DEG2RADF(180.0f));
  RNA_def_property_float_default(prop, DEG2RADF(5.0f));
  RNA_def_boolean(ot->srna,
                  "use_dissolve_boundaries",
                  false,
                  "All Boundaries",
                  "Dissolve all vertices in between face boundaries");
  RNA_def_enum_flag(ot->srna,
                    "delimit",
                    rna_enum_mesh_delimit_mode_items,
                    BMO_DELIM_NORMAL,
                    "Delimit",
                    "Delimit dissolve operation");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Degenerate Dissolve Operator
 * \{ */

static wmOperatorStatus edbm_dissolve_degenerate_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  int totelem_old[3] = {0, 0, 0};
  int totelem_new[3] = {0, 0, 0};

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    totelem_old[0] += bm->totvert;
    totelem_old[1] += bm->totedge;
    totelem_old[2] += bm->totface;
  } /* objects */

  const float thresh = RNA_float_get(op->ptr, "threshold");

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if (!EDBM_op_callf(em, op, "dissolve_degenerate edges=%he dist=%f", BM_ELEM_SELECT, thresh)) {
      continue;
    }

    /* tricky to maintain correct selection here, so just flush up from verts */
    EDBM_select_flush(em);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);

    totelem_new[0] += bm->totvert;
    totelem_new[1] += bm->totedge;
    totelem_new[2] += bm->totface;
  }

  edbm_report_delete_info(op->reports, totelem_old, totelem_new);

  return OPERATOR_FINISHED;
}

void MESH_OT_dissolve_degenerate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Degenerate Dissolve";
  ot->idname = "MESH_OT_dissolve_degenerate";
  ot->description = "Dissolve zero area faces and zero length edges";

  /* API callbacks. */
  ot->exec = edbm_dissolve_degenerate_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_float_distance(ot->srna,
                         "threshold",
                         1e-4f,
                         1e-6f,
                         50.0f,
                         "Merge Distance",
                         "Maximum distance between elements to merge",
                         1e-5f,
                         10.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete Edge-Loop Operator
 * \{ */

/* internally uses dissolve */
static wmOperatorStatus edbm_delete_edgeloop_exec(bContext *C, wmOperator *op)
{
  const bool use_face_split = RNA_boolean_get(op->ptr, "use_face_split");
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totedgesel == 0) {
      continue;
    }

    /* deal with selection */
    {
      BMEdge *e;
      BMIter iter;

      BM_mesh_elem_hflag_disable_all(em->bm, BM_FACE, BM_ELEM_TAG, false);

      BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
        if (BM_elem_flag_test(e, BM_ELEM_SELECT) && e->l) {
          BMLoop *l_iter = e->l;
          do {
            BM_elem_flag_enable(l_iter->f, BM_ELEM_TAG);
          } while ((l_iter = l_iter->radial_next) != e->l);
        }
      }
    }

    if (!EDBM_op_callf(
            em,
            op,
            "dissolve_edges edges=%he use_verts=%b use_face_split=%b angle_threshold=%f",
            BM_ELEM_SELECT,
            true,
            use_face_split,
            M_PI))
    {
      continue;
    }

    BM_mesh_elem_hflag_enable_test(em->bm, BM_FACE, BM_ELEM_SELECT, true, false, BM_ELEM_TAG);

    EDBM_selectmode_flush_ex(em, SCE_SELECT_VERTEX);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_delete_edgeloop(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Delete Edge Loop";
  ot->description = "Delete an edge loop by merging the faces on each side";
  ot->idname = "MESH_OT_delete_edgeloop";

  /* API callbacks. */
  ot->exec = edbm_delete_edgeloop_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "use_face_split",
                  true,
                  "Face Split",
                  "Split off face corners to maintain surrounding geometry");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Split Geometry Operator
 * \{ */

static wmOperatorStatus edbm_split_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    if ((em->bm->totvertsel == 0) && (em->bm->totedgesel == 0) && (em->bm->totfacesel == 0)) {
      continue;
    }
    BM_custom_loop_normals_to_vector_layer(em->bm);

    BMOperator bmop;
    EDBM_op_init(em, &bmop, op, "split geom=%hvef use_only_faces=%b", BM_ELEM_SELECT, false);
    BMO_op_exec(em->bm, &bmop);
    BM_mesh_elem_hflag_disable_all(em->bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
    BMO_slot_buffer_hflag_enable(
        em->bm, bmop.slots_out, "geom.out", BM_ALL_NOLOOP, BM_ELEM_SELECT, true);

    BM_custom_loop_normals_from_vector_layer(em->bm, false);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    /* Geometry has changed, need to recalculate normals and tessellation. */
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_split(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Split";
  ot->idname = "MESH_OT_split";
  ot->description = "Split off selected geometry from connected unselected geometry";

  /* API callbacks. */
  ot->exec = edbm_split_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sort Geometry Elements Operator
 *
 * Unified for vertices/edges/faces.
 *
 * \{ */

enum {
  /** Use view Z (deep) axis. */
  SRT_VIEW_ZAXIS = 1,
  /** Use view X (left to right) axis. */
  SRT_VIEW_XAXIS,
  /** Use distance from element to 3D cursor. */
  SRT_CURSOR_DISTANCE,
  /** Face only: use mat number. */
  SRT_MATERIAL,
  /** Move selected elements in first, without modifying
   * relative order of selected and unselected elements. */
  SRT_SELECTED,
  /** Randomize selected elements. */
  SRT_RANDOMIZE,
  /** Reverse current order of selected elements. */
  SRT_REVERSE,
};

struct BMElemSort {
  /** Sort factor */
  float srt;
  /** Original index of this element (in its #BLI_mempool). */
  int org_idx;
};

static int bmelemsort_comp(const void *v1, const void *v2)
{
  const BMElemSort *x1 = static_cast<const BMElemSort *>(v1);
  const BMElemSort *x2 = static_cast<const BMElemSort *>(v2);

  return (x1->srt > x2->srt) - (x1->srt < x2->srt);
}

/* Reorders vertices/edges/faces using a given methods. Loops are not supported. */
static void sort_bmelem_flag(bContext *C,
                             Scene *scene,
                             Object *ob,
                             RegionView3D *rv3d,
                             const int types,
                             const int flag,
                             const int action,
                             const int reverse,
                             const uint seed)
{
  BMEditMesh *em = BKE_editmesh_from_object(ob);

  BMVert *ve;
  BMEdge *ed;
  BMFace *fa;
  BMIter iter;

  /* In all five elements below, 0 = vertices, 1 = edges, 2 = faces. */
  /* Just to mark protected elements. */
  char *pblock[3] = {nullptr, nullptr, nullptr}, *pb;
  BMElemSort *sblock[3] = {nullptr, nullptr, nullptr}, *sb;
  uint *map[3] = {nullptr, nullptr, nullptr}, *mp;
  int totelem[3] = {0, 0, 0};
  int affected[3] = {0, 0, 0};
  int i, j;

  if (!(types && flag && action)) {
    return;
  }

  if (types & BM_VERT) {
    totelem[0] = em->bm->totvert;
  }
  if (types & BM_EDGE) {
    totelem[1] = em->bm->totedge;
  }
  if (types & BM_FACE) {
    totelem[2] = em->bm->totface;
  }

  if (ELEM(action, SRT_VIEW_ZAXIS, SRT_VIEW_XAXIS)) {
    float mat[4][4];
    float fact = reverse ? -1.0 : 1.0;
    int coidx = (action == SRT_VIEW_ZAXIS) ? 2 : 0;

    /* Apply the view matrix to the object matrix. */
    mul_m4_m4m4(mat, rv3d->viewmat, ob->object_to_world().ptr());

    if (totelem[0]) {
      pb = pblock[0] = MEM_calloc_arrayN<char>(totelem[0], __func__);
      sb = sblock[0] = MEM_calloc_arrayN<BMElemSort>(totelem[0], __func__);

      BM_ITER_MESH_INDEX (ve, &iter, em->bm, BM_VERTS_OF_MESH, i) {
        if (BM_elem_flag_test(ve, flag)) {
          float co[3];
          mul_v3_m4v3(co, mat, ve->co);

          pb[i] = false;
          sb[affected[0]].org_idx = i;
          sb[affected[0]++].srt = co[coidx] * fact;
        }
        else {
          pb[i] = true;
        }
      }
    }

    if (totelem[1]) {
      pb = pblock[1] = MEM_calloc_arrayN<char>(totelem[1], __func__);
      sb = sblock[1] = MEM_calloc_arrayN<BMElemSort>(totelem[1], __func__);

      BM_ITER_MESH_INDEX (ed, &iter, em->bm, BM_EDGES_OF_MESH, i) {
        if (BM_elem_flag_test(ed, flag)) {
          float co[3];
          mid_v3_v3v3(co, ed->v1->co, ed->v2->co);
          mul_m4_v3(mat, co);

          pb[i] = false;
          sb[affected[1]].org_idx = i;
          sb[affected[1]++].srt = co[coidx] * fact;
        }
        else {
          pb[i] = true;
        }
      }
    }

    if (totelem[2]) {
      pb = pblock[2] = MEM_calloc_arrayN<char>(totelem[2], __func__);
      sb = sblock[2] = MEM_calloc_arrayN<BMElemSort>(totelem[2], __func__);

      BM_ITER_MESH_INDEX (fa, &iter, em->bm, BM_FACES_OF_MESH, i) {
        if (BM_elem_flag_test(fa, flag)) {
          float co[3];
          BM_face_calc_center_median(fa, co);
          mul_m4_v3(mat, co);

          pb[i] = false;
          sb[affected[2]].org_idx = i;
          sb[affected[2]++].srt = co[coidx] * fact;
        }
        else {
          pb[i] = true;
        }
      }
    }
  }

  else if (action == SRT_CURSOR_DISTANCE) {
    float cur[3];
    float mat[4][4];
    float fact = reverse ? -1.0 : 1.0;

    copy_v3_v3(cur, scene->cursor.location);

    invert_m4_m4(mat, ob->object_to_world().ptr());
    mul_m4_v3(mat, cur);

    if (totelem[0]) {
      pb = pblock[0] = MEM_calloc_arrayN<char>(totelem[0], __func__);
      sb = sblock[0] = MEM_calloc_arrayN<BMElemSort>(totelem[0], __func__);

      BM_ITER_MESH_INDEX (ve, &iter, em->bm, BM_VERTS_OF_MESH, i) {
        if (BM_elem_flag_test(ve, flag)) {
          pb[i] = false;
          sb[affected[0]].org_idx = i;
          sb[affected[0]++].srt = len_squared_v3v3(cur, ve->co) * fact;
        }
        else {
          pb[i] = true;
        }
      }
    }

    if (totelem[1]) {
      pb = pblock[1] = MEM_calloc_arrayN<char>(totelem[1], __func__);
      sb = sblock[1] = MEM_calloc_arrayN<BMElemSort>(totelem[1], __func__);

      BM_ITER_MESH_INDEX (ed, &iter, em->bm, BM_EDGES_OF_MESH, i) {
        if (BM_elem_flag_test(ed, flag)) {
          float co[3];
          mid_v3_v3v3(co, ed->v1->co, ed->v2->co);

          pb[i] = false;
          sb[affected[1]].org_idx = i;
          sb[affected[1]++].srt = len_squared_v3v3(cur, co) * fact;
        }
        else {
          pb[i] = true;
        }
      }
    }

    if (totelem[2]) {
      pb = pblock[2] = MEM_calloc_arrayN<char>(totelem[2], __func__);
      sb = sblock[2] = MEM_calloc_arrayN<BMElemSort>(totelem[2], __func__);

      BM_ITER_MESH_INDEX (fa, &iter, em->bm, BM_FACES_OF_MESH, i) {
        if (BM_elem_flag_test(fa, flag)) {
          float co[3];
          BM_face_calc_center_median(fa, co);

          pb[i] = false;
          sb[affected[2]].org_idx = i;
          sb[affected[2]++].srt = len_squared_v3v3(cur, co) * fact;
        }
        else {
          pb[i] = true;
        }
      }
    }
  }

  /* Faces only! */
  else if (action == SRT_MATERIAL && totelem[2]) {
    pb = pblock[2] = MEM_calloc_arrayN<char>(totelem[2], __func__);
    sb = sblock[2] = MEM_calloc_arrayN<BMElemSort>(totelem[2], __func__);

    BM_ITER_MESH_INDEX (fa, &iter, em->bm, BM_FACES_OF_MESH, i) {
      if (BM_elem_flag_test(fa, flag)) {
        /* Reverse materials' order, not order of faces inside each mat! */
        /* NOTE: cannot use totcol, as mat_nr may sometimes be greater... */
        float srt = reverse ? float(MAXMAT - fa->mat_nr) : float(fa->mat_nr);
        pb[i] = false;
        sb[affected[2]].org_idx = i;
        /* Multiplying with totface and adding i ensures us
         * we keep current order for all faces of same mat. */
        sb[affected[2]++].srt = srt * float(totelem[2]) + float(i);
        // printf("e: %d; srt: %f; final: %f\n", i, srt, srt * float(totface) + float(i));
      }
      else {
        pb[i] = true;
      }
    }
  }

  else if (action == SRT_SELECTED) {
    uint *tbuf[3] = {nullptr, nullptr, nullptr}, *tb;

    if (totelem[0]) {
      tb = tbuf[0] = MEM_calloc_arrayN<uint>(totelem[0], __func__);
      mp = map[0] = MEM_calloc_arrayN<uint>(totelem[0], __func__);

      BM_ITER_MESH_INDEX (ve, &iter, em->bm, BM_VERTS_OF_MESH, i) {
        if (BM_elem_flag_test(ve, flag)) {
          mp[affected[0]++] = i;
        }
        else {
          *tb = i;
          tb++;
        }
      }
    }

    if (totelem[1]) {
      tb = tbuf[1] = MEM_calloc_arrayN<uint>(totelem[1], __func__);
      mp = map[1] = MEM_calloc_arrayN<uint>(totelem[1], __func__);

      BM_ITER_MESH_INDEX (ed, &iter, em->bm, BM_EDGES_OF_MESH, i) {
        if (BM_elem_flag_test(ed, flag)) {
          mp[affected[1]++] = i;
        }
        else {
          *tb = i;
          tb++;
        }
      }
    }

    if (totelem[2]) {
      tb = tbuf[2] = MEM_calloc_arrayN<uint>(totelem[2], __func__);
      mp = map[2] = MEM_calloc_arrayN<uint>(totelem[2], __func__);

      BM_ITER_MESH_INDEX (fa, &iter, em->bm, BM_FACES_OF_MESH, i) {
        if (BM_elem_flag_test(fa, flag)) {
          mp[affected[2]++] = i;
        }
        else {
          *tb = i;
          tb++;
        }
      }
    }

    for (j = 3; j--;) {
      int tot = totelem[j];
      int aff = affected[j];
      tb = tbuf[j];
      mp = map[j];
      if (!(tb && mp)) {
        continue;
      }
      if (ELEM(aff, 0, tot)) {
        MEM_freeN(tb);
        MEM_freeN(mp);
        map[j] = nullptr;
        continue;
      }
      if (reverse) {
        memcpy(tb + (tot - aff), mp, aff * sizeof(int));
      }
      else {
        memcpy(mp + aff, tb, (tot - aff) * sizeof(int));
        tb = mp;
        mp = map[j] = tbuf[j];
        tbuf[j] = tb;
      }

      /* Reverse mapping, we want an org2new one! */
      for (i = tot, tb = tbuf[j] + tot - 1; i--; tb--) {
        mp[*tb] = i;
      }
      MEM_freeN(tbuf[j]);
    }
  }

  else if (action == SRT_RANDOMIZE) {
    if (totelem[0]) {
      /* Re-init random generator for each element type, to get consistent random when
       * enabling/disabling an element type. */
      RNG *rng = BLI_rng_new_srandom(seed);
      pb = pblock[0] = MEM_calloc_arrayN<char>(totelem[0], __func__);
      sb = sblock[0] = MEM_calloc_arrayN<BMElemSort>(totelem[0], __func__);

      BM_ITER_MESH_INDEX (ve, &iter, em->bm, BM_VERTS_OF_MESH, i) {
        if (BM_elem_flag_test(ve, flag)) {
          pb[i] = false;
          sb[affected[0]].org_idx = i;
          sb[affected[0]++].srt = BLI_rng_get_float(rng);
        }
        else {
          pb[i] = true;
        }
      }

      BLI_rng_free(rng);
    }

    if (totelem[1]) {
      RNG *rng = BLI_rng_new_srandom(seed);
      pb = pblock[1] = MEM_calloc_arrayN<char>(totelem[1], __func__);
      sb = sblock[1] = MEM_calloc_arrayN<BMElemSort>(totelem[1], __func__);

      BM_ITER_MESH_INDEX (ed, &iter, em->bm, BM_EDGES_OF_MESH, i) {
        if (BM_elem_flag_test(ed, flag)) {
          pb[i] = false;
          sb[affected[1]].org_idx = i;
          sb[affected[1]++].srt = BLI_rng_get_float(rng);
        }
        else {
          pb[i] = true;
        }
      }

      BLI_rng_free(rng);
    }

    if (totelem[2]) {
      RNG *rng = BLI_rng_new_srandom(seed);
      pb = pblock[2] = MEM_calloc_arrayN<char>(totelem[2], __func__);
      sb = sblock[2] = MEM_calloc_arrayN<BMElemSort>(totelem[2], __func__);

      BM_ITER_MESH_INDEX (fa, &iter, em->bm, BM_FACES_OF_MESH, i) {
        if (BM_elem_flag_test(fa, flag)) {
          pb[i] = false;
          sb[affected[2]].org_idx = i;
          sb[affected[2]++].srt = BLI_rng_get_float(rng);
        }
        else {
          pb[i] = true;
        }
      }

      BLI_rng_free(rng);
    }
  }

  else if (action == SRT_REVERSE) {
    if (totelem[0]) {
      pb = pblock[0] = MEM_calloc_arrayN<char>(totelem[0], __func__);
      sb = sblock[0] = MEM_calloc_arrayN<BMElemSort>(totelem[0], __func__);

      BM_ITER_MESH_INDEX (ve, &iter, em->bm, BM_VERTS_OF_MESH, i) {
        if (BM_elem_flag_test(ve, flag)) {
          pb[i] = false;
          sb[affected[0]].org_idx = i;
          sb[affected[0]++].srt = float(-i);
        }
        else {
          pb[i] = true;
        }
      }
    }

    if (totelem[1]) {
      pb = pblock[1] = MEM_calloc_arrayN<char>(totelem[1], __func__);
      sb = sblock[1] = MEM_calloc_arrayN<BMElemSort>(totelem[1], __func__);

      BM_ITER_MESH_INDEX (ed, &iter, em->bm, BM_EDGES_OF_MESH, i) {
        if (BM_elem_flag_test(ed, flag)) {
          pb[i] = false;
          sb[affected[1]].org_idx = i;
          sb[affected[1]++].srt = float(-i);
        }
        else {
          pb[i] = true;
        }
      }
    }

    if (totelem[2]) {
      pb = pblock[2] = MEM_calloc_arrayN<char>(totelem[2], __func__);
      sb = sblock[2] = MEM_calloc_arrayN<BMElemSort>(totelem[2], __func__);

      BM_ITER_MESH_INDEX (fa, &iter, em->bm, BM_FACES_OF_MESH, i) {
        if (BM_elem_flag_test(fa, flag)) {
          pb[i] = false;
          sb[affected[2]].org_idx = i;
          sb[affected[2]++].srt = float(-i);
        }
        else {
          pb[i] = true;
        }
      }
    }
  }

  // printf("%d vertices: %d to be affected...\n", totelem[0], affected[0]);
  // printf("%d edges: %d to be affected...\n", totelem[1], affected[1]);
  // printf("%d faces: %d to be affected...\n", totelem[2], affected[2]);
  if (affected[0] == 0 && affected[1] == 0 && affected[2] == 0) {
    for (j = 3; j--;) {
      if (pblock[j]) {
        MEM_freeN(pblock[j]);
      }
      if (sblock[j]) {
        MEM_freeN(sblock[j]);
      }
      if (map[j]) {
        MEM_freeN(map[j]);
      }
    }
    return;
  }

  /* Sort affected elements, and populate mapping arrays, if needed. */
  for (j = 3; j--;) {
    pb = pblock[j];
    sb = sblock[j];
    if (pb && sb && !map[j]) {
      const char *p_blk;
      BMElemSort *s_blk;
      int tot = totelem[j];
      int aff = affected[j];

      qsort(sb, aff, sizeof(BMElemSort), bmelemsort_comp);

      mp = map[j] = MEM_malloc_arrayN<uint>(tot, __func__);
      p_blk = pb + tot - 1;
      s_blk = sb + aff - 1;
      for (i = tot; i--; p_blk--) {
        if (*p_blk) { /* Protected! */
          mp[i] = i;
        }
        else {
          mp[s_blk->org_idx] = i;
          s_blk--;
        }
      }
    }
    if (pb) {
      MEM_freeN(pb);
    }
    if (sb) {
      MEM_freeN(sb);
    }
  }

  BM_mesh_remap(em->bm, map[0], map[1], map[2]);

  EDBMUpdate_Params params{};
  params.calc_looptris = (totelem[2] != 0);
  params.calc_normals = false;
  params.is_destructive = true;
  EDBM_update(static_cast<Mesh *>(ob->data), &params);

  DEG_id_tag_update(static_cast<ID *>(ob->data), ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob->data);

  for (j = 3; j--;) {
    if (map[j]) {
      MEM_freeN(map[j]);
    }
  }
}

static wmOperatorStatus edbm_sort_elements_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *ob_active = CTX_data_edit_object(C);

  /* may be nullptr */
  RegionView3D *rv3d = ED_view3d_context_rv3d(C);

  const int action = RNA_enum_get(op->ptr, "type");
  PropertyRNA *prop_elem_types = RNA_struct_find_property(op->ptr, "elements");
  const bool use_reverse = RNA_boolean_get(op->ptr, "reverse");
  uint seed = RNA_int_get(op->ptr, "seed");
  int elem_types = 0;

  if (ELEM(action, SRT_VIEW_ZAXIS, SRT_VIEW_XAXIS)) {
    if (rv3d == nullptr) {
      BKE_report(op->reports, RPT_ERROR, "View not found, cannot sort by view axis");
      return OPERATOR_CANCELLED;
    }
  }

  /* If no elem_types set, use current selection mode to set it! */
  if (RNA_property_is_set(op->ptr, prop_elem_types)) {
    elem_types = RNA_property_enum_get(op->ptr, prop_elem_types);
  }
  else {
    BMEditMesh *em = BKE_editmesh_from_object(ob_active);
    if (em->selectmode & SCE_SELECT_VERTEX) {
      elem_types |= BM_VERT;
    }
    if (em->selectmode & SCE_SELECT_EDGE) {
      elem_types |= BM_EDGE;
    }
    if (em->selectmode & SCE_SELECT_FACE) {
      elem_types |= BM_FACE;
    }
    RNA_enum_set(op->ptr, "elements", elem_types);
  }

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (uint ob_index = 0; ob_index < objects.size(); ob_index++) {
    Object *ob = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(ob);
    BMesh *bm = em->bm;

    if (!((elem_types & BM_VERT && bm->totvertsel > 0) ||
          (elem_types & BM_EDGE && bm->totedgesel > 0) ||
          (elem_types & BM_FACE && bm->totfacesel > 0)))
    {
      continue;
    }

    int seed_iter = seed;

    /* This gives a consistent result regardless of object order */
    if (ob_index) {
      seed_iter += BLI_ghashutil_strhash_p(ob->id.name);
    }

    sort_bmelem_flag(
        C, scene, ob, rv3d, elem_types, BM_ELEM_SELECT, action, use_reverse, seed_iter);
  }
  return OPERATOR_FINISHED;
}

static bool edbm_sort_elements_poll_property(const bContext * /*C*/,
                                             wmOperator *op,
                                             const PropertyRNA *prop)
{
  const char *prop_id = RNA_property_identifier(prop);
  const int action = RNA_enum_get(op->ptr, "type");

  /* Only show seed for randomize action! */
  if (STREQ(prop_id, "seed")) {
    if (action == SRT_RANDOMIZE) {
      return true;
    }
    return false;
  }

  /* Hide seed for reverse and randomize actions! */
  if (STREQ(prop_id, "reverse")) {
    if (ELEM(action, SRT_RANDOMIZE, SRT_REVERSE)) {
      return false;
    }
    return true;
  }

  return true;
}

void MESH_OT_sort_elements(wmOperatorType *ot)
{
  static const EnumPropertyItem type_items[] = {
      {SRT_VIEW_ZAXIS,
       "VIEW_ZAXIS",
       0,
       "View Z Axis",
       "Sort selected elements from farthest to nearest one in current view"},
      {SRT_VIEW_XAXIS,
       "VIEW_XAXIS",
       0,
       "View X Axis",
       "Sort selected elements from left to right one in current view"},
      {SRT_CURSOR_DISTANCE,
       "CURSOR_DISTANCE",
       0,
       "Cursor Distance",
       "Sort selected elements from nearest to farthest from 3D cursor"},
      {SRT_MATERIAL,
       "MATERIAL",
       0,
       "Material",
       "Sort selected faces from smallest to greatest material index"},
      {SRT_SELECTED,
       "SELECTED",
       0,
       "Selected",
       "Move all selected elements in first places, preserving their relative order.\n"
       "Warning: This will affect unselected elements' indices as well"},
      {SRT_RANDOMIZE, "RANDOMIZE", 0, "Randomize", "Randomize order of selected elements"},
      {SRT_REVERSE, "REVERSE", 0, "Reverse", "Reverse current order of selected elements"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem elem_items[] = {
      {BM_VERT, "VERT", 0, "Vertices", ""},
      {BM_EDGE, "EDGE", 0, "Edges", ""},
      {BM_FACE, "FACE", 0, "Faces", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "Sort Mesh Elements";
  ot->description =
      "The order of selected vertices/edges/faces is modified, based on a given method";
  ot->idname = "MESH_OT_sort_elements";

  /* API callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = edbm_sort_elements_exec;
  ot->poll = ED_operator_editmesh;
  ot->poll_property = edbm_sort_elements_poll_property;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(ot->srna,
                          "type",
                          type_items,
                          SRT_VIEW_ZAXIS,
                          "Type",
                          "Type of reordering operation to apply");
  RNA_def_enum_flag(ot->srna,
                    "elements",
                    elem_items,
                    BM_VERT,
                    "Elements",
                    "Which elements to affect (vertices, edges and/or faces)");
  RNA_def_boolean(ot->srna, "reverse", false, "Reverse", "Reverse the sorting effect");
  RNA_def_int(ot->srna, "seed", 0, 0, INT_MAX, "Seed", "Seed for random-based operations", 0, 255);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bridge Operator
 * \{ */

enum {
  MESH_BRIDGELOOP_SINGLE = 0,
  MESH_BRIDGELOOP_CLOSED = 1,
  MESH_BRIDGELOOP_PAIRS = 2,
};

static int edbm_bridge_tag_boundary_edges(BMesh *bm)
{
  /* tags boundary edges from a face selection */
  BMIter iter;
  BMFace *f;
  BMEdge *e;
  int totface_del = 0;

  BM_mesh_elem_hflag_disable_all(bm, BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
    if (BM_elem_flag_test(e, BM_ELEM_SELECT)) {
      if (BM_edge_is_wire(e) || BM_edge_is_boundary(e)) {
        BM_elem_flag_enable(e, BM_ELEM_TAG);
      }
      else {
        BMIter fiter;
        bool is_all_sel = true;
        /* check if its only used by selected faces */
        BM_ITER_ELEM (f, &fiter, e, BM_FACES_OF_EDGE) {
          if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
            /* Tag face for removal. */
            if (!BM_elem_flag_test(f, BM_ELEM_TAG)) {
              BM_elem_flag_enable(f, BM_ELEM_TAG);
              totface_del++;
            }
          }
          else {
            is_all_sel = false;
          }
        }

        if (is_all_sel == false) {
          BM_elem_flag_enable(e, BM_ELEM_TAG);
        }
      }
    }
  }

  return totface_del;
}

static int edbm_bridge_edge_loops_for_single_editmesh(wmOperator *op,
                                                      BMEditMesh *em,
                                                      Mesh *mesh,
                                                      const bool use_pairs,
                                                      const bool use_cyclic,
                                                      const bool use_merge,
                                                      const float merge_factor,
                                                      const int twist_offset)
{
  BMOperator bmop;
  char edge_hflag;
  int totface_del = 0;
  BMFace **totface_del_arr = nullptr;
  const bool use_faces = (em->bm->totfacesel != 0);
  bool changed = false;

  if (use_faces) {
    /* NOTE: When all faces are selected, all faces will be deleted with no edge-loops remaining.
     * In this case bridge will fail with a waning and delete all faces.
     * Ideally it's possible to detect cases when deleting faces leaves remaining edge-loops.
     * While this can be done in trivial cases - by checking the number of selected faces matches
     * the number of faces, that won't work for more involved cases involving hidden faces
     * and wire edges. One option could be to copy & restore the edit-mesh however
     * this is quite an expensive operation - to properly handle clearly invalid input.
     * Accept this limitation, the user must undo to restore the previous state, see: #123405. */

    BMIter iter;
    BMFace *f;
    int i;

    totface_del = edbm_bridge_tag_boundary_edges(em->bm);
    totface_del_arr = static_cast<BMFace **>(
        MEM_mallocN(sizeof(*totface_del_arr) * totface_del, __func__));

    i = 0;
    BM_ITER_MESH (f, &iter, em->bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_TAG)) {
        totface_del_arr[i++] = f;
      }
    }
    edge_hflag = BM_ELEM_TAG;
  }
  else {
    edge_hflag = BM_ELEM_SELECT;
  }

  EDBM_op_init(em,
               &bmop,
               op,
               "bridge_loops edges=%he use_pairs=%b use_cyclic=%b use_merge=%b merge_factor=%f "
               "twist_offset=%i",
               edge_hflag,
               use_pairs,
               use_cyclic,
               use_merge,
               merge_factor,
               twist_offset);

  if (use_faces && totface_del) {
    int i;
    BM_mesh_elem_hflag_disable_all(em->bm, BM_FACE, BM_ELEM_TAG, false);
    for (i = 0; i < totface_del; i++) {
      BM_elem_flag_enable(totface_del_arr[i], BM_ELEM_TAG);
    }
    BMO_op_callf(em->bm,
                 BMO_FLAG_DEFAULTS,
                 "delete geom=%hf context=%i",
                 BM_ELEM_TAG,
                 DEL_FACES_KEEP_BOUNDARY);
    changed = true;
  }

  BMO_op_exec(em->bm, &bmop);

  if (!BMO_error_occurred_at_level(em->bm, BMO_ERROR_CANCEL)) {
    /* when merge is used the edges are joined and remain selected */
    if (use_merge == false) {
      EDBM_flag_disable_all(em, BM_ELEM_SELECT);
      BMO_slot_buffer_hflag_enable(
          em->bm, bmop.slots_out, "faces.out", BM_FACE, BM_ELEM_SELECT, true);

      changed = true;
    }

    if (use_merge == false) {
      EdgeRingOpSubdProps op_props;
      mesh_operator_edgering_props_get(op, &op_props);

      if (op_props.cuts) {
        BMOperator bmop_subd;
        /* we only need face normals updated */
        EDBM_mesh_normals_update(em);

        BMO_op_initf(em->bm,
                     &bmop_subd,
                     0,
                     "subdivide_edgering edges=%S interp_mode=%i cuts=%i smooth=%f "
                     "profile_shape=%i profile_shape_factor=%f",
                     &bmop,
                     "edges.out",
                     op_props.interp_mode,
                     op_props.cuts,
                     op_props.smooth,
                     op_props.profile_shape,
                     op_props.profile_shape_factor);
        BMO_op_exec(em->bm, &bmop_subd);
        BMO_slot_buffer_hflag_enable(
            em->bm, bmop_subd.slots_out, "faces.out", BM_FACE, BM_ELEM_SELECT, true);
        BMO_op_finish(em->bm, &bmop_subd);

        changed = true;
      }
    }
  }

  if (totface_del_arr) {
    MEM_freeN(totface_del_arr);
  }

  if (EDBM_op_finish(em, &bmop, op, true)) {
    changed = true;
  }

  if (changed) {
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(mesh, &params);
  }

  /* Always return finished so the user can select different options. */
  return OPERATOR_FINISHED;
}

static wmOperatorStatus edbm_bridge_edge_loops_exec(bContext *C, wmOperator *op)
{
  const int type = RNA_enum_get(op->ptr, "type");
  const bool use_pairs = (type == MESH_BRIDGELOOP_PAIRS);
  const bool use_cyclic = (type == MESH_BRIDGELOOP_CLOSED);
  const bool use_merge = RNA_boolean_get(op->ptr, "use_merge");
  const float merge_factor = RNA_float_get(op->ptr, "merge_factor");
  const int twist_offset = RNA_int_get(op->ptr, "twist_offset");
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totvertsel == 0) {
      continue;
    }

    edbm_bridge_edge_loops_for_single_editmesh(op,
                                               em,
                                               static_cast<Mesh *>(obedit->data),
                                               use_pairs,
                                               use_cyclic,
                                               use_merge,
                                               merge_factor,
                                               twist_offset);
  }
  return OPERATOR_FINISHED;
}

void MESH_OT_bridge_edge_loops(wmOperatorType *ot)
{
  static const EnumPropertyItem type_items[] = {
      {MESH_BRIDGELOOP_SINGLE, "SINGLE", 0, "Open Loop", ""},
      {MESH_BRIDGELOOP_CLOSED, "CLOSED", 0, "Closed Loop", ""},
      {MESH_BRIDGELOOP_PAIRS, "PAIRS", 0, "Loop Pairs", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "Bridge Edge Loops";
  ot->description = "Create a bridge of faces between two or more selected edge loops";
  ot->idname = "MESH_OT_bridge_edge_loops";

  /* API callbacks. */
  ot->exec = edbm_bridge_edge_loops_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "type",
                          type_items,
                          MESH_BRIDGELOOP_SINGLE,
                          "Connect Loops",
                          "Method of bridging multiple loops");

  RNA_def_boolean(ot->srna, "use_merge", false, "Merge", "Merge rather than creating faces");
  RNA_def_float(ot->srna, "merge_factor", 0.5f, 0.0f, 1.0f, "Merge Factor", "", 0.0f, 1.0f);
  RNA_def_int(ot->srna,
              "twist_offset",
              0,
              -1000,
              1000,
              "Twist",
              "Twist offset for closed loops",
              -1000,
              1000);

  mesh_operator_edgering_props(ot, 0, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Wire-Frame Operator
 * \{ */

static wmOperatorStatus edbm_wireframe_exec(bContext *C, wmOperator *op)
{
  const bool use_boundary = RNA_boolean_get(op->ptr, "use_boundary");
  const bool use_even_offset = RNA_boolean_get(op->ptr, "use_even_offset");
  const bool use_replace = RNA_boolean_get(op->ptr, "use_replace");
  const bool use_relative_offset = RNA_boolean_get(op->ptr, "use_relative_offset");
  const bool use_crease = RNA_boolean_get(op->ptr, "use_crease");
  const float crease_weight = RNA_float_get(op->ptr, "crease_weight");
  const float thickness = RNA_float_get(op->ptr, "thickness");
  const float offset = RNA_float_get(op->ptr, "offset");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }

    BMOperator bmop;

    EDBM_op_init(em,
                 &bmop,
                 op,
                 "wireframe faces=%hf use_replace=%b use_boundary=%b use_even_offset=%b "
                 "use_relative_offset=%b "
                 "use_crease=%b crease_weight=%f thickness=%f offset=%f",
                 BM_ELEM_SELECT,
                 use_replace,
                 use_boundary,
                 use_even_offset,
                 use_relative_offset,
                 use_crease,
                 crease_weight,
                 thickness,
                 offset);

    BMO_op_exec(em->bm, &bmop);

    BM_mesh_elem_hflag_disable_all(em->bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
    BMO_slot_buffer_hflag_enable(
        em->bm, bmop.slots_out, "faces.out", BM_FACE, BM_ELEM_SELECT, true);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_wireframe(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Wireframe";
  ot->idname = "MESH_OT_wireframe";
  ot->description = "Create a solid wireframe from faces";

  /* API callbacks. */
  ot->exec = edbm_wireframe_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna, "use_boundary", true, "Boundary", "Inset face boundaries");
  RNA_def_boolean(ot->srna,
                  "use_even_offset",
                  true,
                  "Offset Even",
                  "Scale the offset to give more even thickness");
  RNA_def_boolean(ot->srna,
                  "use_relative_offset",
                  false,
                  "Offset Relative",
                  "Scale the offset by surrounding geometry");
  RNA_def_boolean(ot->srna, "use_replace", true, "Replace", "Remove original faces");
  prop = RNA_def_float_distance(
      ot->srna, "thickness", 0.01f, 0.0f, 1e4f, "Thickness", "", 0.0f, 10.0f);
  /* use 1 rather than 10 for max else dragging the button moves too far */
  RNA_def_property_ui_range(prop, 0.0, 1.0, 0.01, 4);
  RNA_def_float_distance(ot->srna, "offset", 0.01f, 0.0f, 1e4f, "Offset", "", 0.0f, 10.0f);
  RNA_def_boolean(ot->srna,
                  "use_crease",
                  false,
                  "Crease",
                  "Crease hub edges for an improved subdivision surface");
  prop = RNA_def_float(
      ot->srna, "crease_weight", 0.01f, 0.0f, 1e3f, "Crease Weight", "", 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0, 1.0, 0.1, 2);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Offset Edge-Loop Operator
 * \{ */

static wmOperatorStatus edbm_offset_edgeloop_exec(bContext *C, wmOperator *op)
{
  const bool use_cap_endpoint = RNA_boolean_get(op->ptr, "use_cap_endpoint");
  bool changed_multi = false;
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Base *> bases = BKE_view_layer_array_from_bases_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Base *base : bases) {
    Object *obedit = base->object;
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totedgesel == 0) {
      continue;
    }

    BMOperator bmop;
    EDBM_op_init(em,
                 &bmop,
                 op,
                 "offset_edgeloops edges=%he use_cap_endpoint=%b",
                 BM_ELEM_SELECT,
                 use_cap_endpoint);

    BMO_op_exec(em->bm, &bmop);

    BM_mesh_elem_hflag_disable_all(em->bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);

    BMO_slot_buffer_hflag_enable(
        em->bm, bmop.slots_out, "edges.out", BM_EDGE, BM_ELEM_SELECT, true);

    if (EDBM_op_finish(em, &bmop, op, true)) {
      EDBMUpdate_Params params{};
      params.calc_looptris = true;
      params.calc_normals = false;
      params.is_destructive = true;
      EDBM_update(static_cast<Mesh *>(obedit->data), &params);
      changed_multi = true;
    }
  }

  if (changed_multi) {
    /** If in face-only select mode, switch to edge select mode so that
     * an edge-only selection is not inconsistent state.
     *
     * We need to run this for all objects, even when nothing is selected.
     * This way we keep them in sync. */
    if (scene->toolsettings->selectmode == SCE_SELECT_FACE) {
      EDBM_selectmode_disable_multi_ex(scene, bases, SCE_SELECT_FACE, SCE_SELECT_EDGE);
    }
  }

  return changed_multi ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void MESH_OT_offset_edge_loops(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Offset Edge Loop";
  ot->idname = "MESH_OT_offset_edge_loops";
  ot->description = "Create offset edge loop from the current selection";

  /* API callbacks. */
  ot->exec = edbm_offset_edgeloop_exec;
  ot->poll = ED_operator_editmesh;

  /* Keep internal, since this is only meant to be accessed via
   * `MESH_OT_offset_edge_loops_slide`. */

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  RNA_def_boolean(
      ot->srna, "use_cap_endpoint", false, "Cap Endpoint", "Extend loop around end-points");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Convex Hull Operator
 * \{ */

#ifdef WITH_BULLET
static wmOperatorStatus edbm_convex_hull_exec(bContext *C, wmOperator *op)
{
  const bool use_existing_faces = RNA_boolean_get(op->ptr, "use_existing_faces");
  const bool delete_unused = RNA_boolean_get(op->ptr, "delete_unused");
  const bool make_holes = RNA_boolean_get(op->ptr, "make_holes");
  const bool join_triangles = RNA_boolean_get(op->ptr, "join_triangles");

  float angle_face_threshold = RNA_float_get(op->ptr, "face_threshold");
  float angle_shape_threshold = RNA_float_get(op->ptr, "shape_threshold");

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totvertsel == 0) {
      continue;
    }

    BMOperator bmop;

    EDBM_op_init(em,
                 &bmop,
                 op,
                 "convex_hull input=%hvef "
                 "use_existing_faces=%b",
                 BM_ELEM_SELECT,
                 use_existing_faces);
    BMO_op_exec(em->bm, &bmop);

    /* Hull fails if input is coplanar */
    if (BMO_error_occurred_at_level(em->bm, BMO_ERROR_CANCEL)) {
      EDBM_op_finish(em, &bmop, op, true);
      continue;
    }

    BMO_slot_buffer_hflag_enable(
        em->bm, bmop.slots_out, "geom.out", BM_FACE, BM_ELEM_SELECT, true);

    /* Delete unused vertices, edges, and faces */
    if (delete_unused) {
      if (!EDBM_op_callf(
              em, op, "delete geom=%S context=%i", &bmop, "geom_unused.out", DEL_ONLYTAGGED))
      {
        EDBM_op_finish(em, &bmop, op, true);
        continue;
      }
    }

    /* Delete hole edges/faces */
    if (make_holes) {
      if (!EDBM_op_callf(
              em, op, "delete geom=%S context=%i", &bmop, "geom_holes.out", DEL_ONLYTAGGED))
      {
        EDBM_op_finish(em, &bmop, op, true);
        continue;
      }
    }

    /* Merge adjacent triangles */
    if (join_triangles) {
      if (!EDBM_op_call_and_selectf(em,
                                    op,
                                    "faces.out",
                                    true,
                                    "join_triangles faces=%S "
                                    "angle_face_threshold=%f angle_shape_threshold=%f",
                                    &bmop,
                                    "geom.out",
                                    angle_face_threshold,
                                    angle_shape_threshold))
      {
        EDBM_op_finish(em, &bmop, op, true);
        continue;
      }
    }

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
    EDBM_selectmode_flush(em);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_convex_hull(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Convex Hull";
  ot->description = "Enclose selected vertices in a convex polyhedron";
  ot->idname = "MESH_OT_convex_hull";

  /* API callbacks. */
  ot->exec = edbm_convex_hull_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_boolean(ot->srna,
                  "delete_unused",
                  true,
                  "Delete Unused",
                  "Delete selected elements that are not used by the hull");

  RNA_def_boolean(ot->srna,
                  "use_existing_faces",
                  true,
                  "Use Existing Faces",
                  "Skip hull triangles that are covered by a pre-existing face");

  RNA_def_boolean(ot->srna,
                  "make_holes",
                  false,
                  "Make Holes",
                  "Delete selected faces that are used by the hull");

  RNA_def_boolean(
      ot->srna, "join_triangles", true, "Join Triangles", "Merge adjacent triangles into quads");

  join_triangle_props(ot);
}
#endif /* WITH_BULLET */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Symmetrize Operator
 * \{ */

static wmOperatorStatus mesh_symmetrize_exec(bContext *C, wmOperator *op)
{
  const float thresh = RNA_float_get(op->ptr, "threshold");
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totvertsel == 0) {
      continue;
    }

    BMOperator bmop;
    EDBM_op_init(em,
                 &bmop,
                 op,
                 "symmetrize input=%hvef direction=%i dist=%f",
                 BM_ELEM_SELECT,
                 RNA_enum_get(op->ptr, "direction"),
                 thresh);
    BMO_op_exec(em->bm, &bmop);

    EDBM_flag_disable_all(em, BM_ELEM_SELECT);

    BMO_slot_buffer_hflag_enable(
        em->bm, bmop.slots_out, "geom.out", BM_ALL_NOLOOP, BM_ELEM_SELECT, true);

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
    EDBM_selectmode_flush(em);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_symmetrize(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Symmetrize";
  ot->description = "Enforce symmetry (both form and topological) across an axis";
  ot->idname = "MESH_OT_symmetrize";

  /* API callbacks. */
  ot->exec = mesh_symmetrize_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "direction",
                          rna_enum_symmetrize_direction_items,
                          BMO_SYMMETRIZE_NEGATIVE_X,
                          "Direction",
                          "Which sides to copy from and to");
  RNA_def_float(ot->srna,
                "threshold",
                1e-4f,
                0.0f,
                10.0f,
                "Threshold",
                "Limit for snap middle vertices to the axis center",
                1e-5f,
                0.1f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap to Symmetry Operator
 * \{ */

static wmOperatorStatus mesh_symmetry_snap_exec(bContext *C, wmOperator *op)
{
  const float eps = 0.00001f;
  const float eps_sq = eps * eps;
  const bool use_topology = false;

  const float thresh = RNA_float_get(op->ptr, "threshold");
  const float fac = RNA_float_get(op->ptr, "factor");
  const bool use_center = RNA_boolean_get(op->ptr, "use_center");
  const int axis_dir = RNA_enum_get(op->ptr, "direction");

  /* Vertices stats (total over all selected objects). */
  int totvertfound = 0, totvertmirr = 0, totvertfail = 0, totobjects = 0;

  /* Axis. */
  int axis = axis_dir % 3;
  bool axis_sign = axis != axis_dir;

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if (em->bm->totvertsel == 0) {
      continue;
    }

    if (blender::ed::object::shape_key_report_if_locked(obedit, op->reports)) {
      continue;
    }

    totobjects++;

    /* Only allocate memory after checking whether to skip object. */
    int *index = MEM_malloc_arrayN<int>(bm->totvert, __func__);

    /* Vertex iter. */
    BMIter iter;
    BMVert *v;
    int i;

    EDBM_verts_mirror_cache_begin_ex(em, axis, true, true, false, use_topology, thresh, index);

    BM_mesh_elem_table_ensure(bm, BM_VERT);

    BM_mesh_elem_hflag_disable_all(bm, BM_VERT, BM_ELEM_TAG, false);

    BM_ITER_MESH_INDEX (v, &iter, bm, BM_VERTS_OF_MESH, i) {
      if ((BM_elem_flag_test(v, BM_ELEM_SELECT) != false) &&
          (BM_elem_flag_test(v, BM_ELEM_TAG) == false))
      {
        int i_mirr = index[i];
        if (i_mirr != -1) {

          BMVert *v_mirr = BM_vert_at_index(bm, index[i]);

          if (v != v_mirr) {
            float co[3], co_mirr[3];

            if ((v->co[axis] > v_mirr->co[axis]) == axis_sign) {
              std::swap(v, v_mirr);
            }

            copy_v3_v3(co_mirr, v_mirr->co);
            co_mirr[axis] *= -1.0f;

            if (len_squared_v3v3(v->co, co_mirr) > eps_sq) {
              totvertmirr++;
            }

            interp_v3_v3v3(co, v->co, co_mirr, fac);

            copy_v3_v3(v->co, co);

            co[axis] *= -1.0f;
            copy_v3_v3(v_mirr->co, co);

            BM_elem_flag_enable(v, BM_ELEM_TAG);
            BM_elem_flag_enable(v_mirr, BM_ELEM_TAG);
            totvertfound++;
          }
          else {
            if (use_center) {

              if (fabsf(v->co[axis]) > eps) {
                totvertmirr++;
              }

              v->co[axis] = 0.0f;
            }
            BM_elem_flag_enable(v, BM_ELEM_TAG);
            totvertfound++;
          }
        }
        else {
          totvertfail++;
        }
      }
    }
    EDBMUpdate_Params params{};
    params.calc_looptris = false;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);

    /* No need to end cache, just free the array. */
    MEM_freeN(index);
  }

  if (totvertfail) {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "%d already symmetrical, %d pairs mirrored, %d failed",
                totvertfound - totvertmirr,
                totvertmirr,
                totvertfail);
  }
  else if (totobjects) {
    BKE_reportf(op->reports,
                RPT_INFO,
                "%d already symmetrical, %d pairs mirrored",
                totvertfound - totvertmirr,
                totvertmirr);
  }

  return totobjects ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void MESH_OT_symmetry_snap(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Snap to Symmetry";
  ot->description = "Snap vertex pairs to their mirrored locations";
  ot->idname = "MESH_OT_symmetry_snap";

  /* API callbacks. */
  ot->exec = mesh_symmetry_snap_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "direction",
                          rna_enum_symmetrize_direction_items,
                          BMO_SYMMETRIZE_NEGATIVE_X,
                          "Direction",
                          "Which sides to copy from and to");
  RNA_def_float_distance(ot->srna,
                         "threshold",
                         0.05f,
                         0.0f,
                         10.0f,
                         "Threshold",
                         "Distance within which matching vertices are searched",
                         1e-4f,
                         1.0f);
  RNA_def_float(ot->srna,
                "factor",
                0.5f,
                0.0f,
                1.0f,
                "Factor",
                "Mix factor of the locations of the vertices",
                0.0f,
                1.0f);
  RNA_def_boolean(
      ot->srna, "use_center", true, "Center", "Snap middle vertices to the axis center");
}

/** \} */

#if defined(WITH_FREESTYLE)

/* -------------------------------------------------------------------- */
/** \name Mark Edge (Freestyle) Operator
 * \{ */

static wmOperatorStatus edbm_mark_freestyle_edge_exec(bContext *C, wmOperator *op)
{
  BMEdge *eed;
  BMIter iter;
  const bool clear = RNA_boolean_get(op->ptr, "clear");
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em == nullptr) {
      continue;
    }

    BMesh *bm = em->bm;

    if (bm->totedgesel == 0) {
      continue;
    }

    BM_data_layer_ensure_named(bm, &em->bm->edata, CD_PROP_BOOL, "freestyle_edge");
    const int offset = CustomData_get_offset_named(&em->bm->edata, CD_PROP_BOOL, "freestyle_edge");
    if (offset == -1) {
      continue;
    }

    if (clear) {
      BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
        if (BM_elem_flag_test(eed, BM_ELEM_SELECT) && !BM_elem_flag_test(eed, BM_ELEM_HIDDEN)) {
          BM_ELEM_CD_SET_BOOL(eed, offset, false);
        }
      }
    }
    else {
      BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
        if (BM_elem_flag_test(eed, BM_ELEM_SELECT) && !BM_elem_flag_test(eed, BM_ELEM_HIDDEN)) {
          BM_ELEM_CD_SET_BOOL(eed, offset, true);
        }
      }
    }

    DEG_id_tag_update(static_cast<ID *>(obedit->data), ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_mark_freestyle_edge(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Mark Freestyle Edge";
  ot->description = "(Un)mark selected edges as Freestyle feature edges";
  ot->idname = "MESH_OT_mark_freestyle_edge";

  /* API callbacks. */
  ot->exec = edbm_mark_freestyle_edge_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_boolean(ot->srna, "clear", false, "Clear", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mark Face (Freestyle) Operator
 * \{ */

static wmOperatorStatus edbm_mark_freestyle_face_exec(bContext *C, wmOperator *op)
{
  BMFace *efa;
  BMIter iter;
  const bool clear = RNA_boolean_get(op->ptr, "clear");
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em == nullptr) {
      continue;
    }

    if (em->bm->totfacesel == 0) {
      continue;
    }

    BM_data_layer_ensure_named(em->bm, &em->bm->edata, CD_PROP_BOOL, "freestyle_edge");
    const int offset = CustomData_get_offset_named(&em->bm->edata, CD_PROP_BOOL, "freestyle_edge");
    if (offset == -1) {
      continue;
    }

    if (clear) {
      BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(efa, BM_ELEM_SELECT) && !BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
          BM_ELEM_CD_SET_BOOL(efa, offset, false);
        }
      }
    }
    else {
      BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(efa, BM_ELEM_SELECT) && !BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
          BM_ELEM_CD_SET_BOOL(efa, offset, true);
        }
      }
    }

    DEG_id_tag_update(static_cast<ID *>(obedit->data), ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_mark_freestyle_face(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Mark Freestyle Face";
  ot->description = "(Un)mark selected faces for exclusion from Freestyle feature edge detection";
  ot->idname = "MESH_OT_mark_freestyle_face";

  /* API callbacks. */
  ot->exec = edbm_mark_freestyle_face_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_boolean(ot->srna, "clear", false, "Clear", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

#endif /* WITH_FREESTYLE */

/* -------------------------------------------------------------------- */
/** \name Loop Normals Editing Tools Modal Map
 * \{ */

/* NOTE: these defines are saved in keymap files, do not change values but just add new ones */
/* NOTE: We could add more here, like e.g. a switch between local or global coordinates of target,
 *       use number-input to type in explicit vector values. */
enum {
  /* Generic commands. */
  EDBM_CLNOR_MODAL_CANCEL = 1,
  EDBM_CLNOR_MODAL_CONFIRM = 2,

  /* Point To operator. */
  EDBM_CLNOR_MODAL_POINTTO_RESET = 101,
  EDBM_CLNOR_MODAL_POINTTO_INVERT = 102,
  EDBM_CLNOR_MODAL_POINTTO_SPHERIZE = 103,
  EDBM_CLNOR_MODAL_POINTTO_ALIGN = 104,

  EDBM_CLNOR_MODAL_POINTTO_USE_MOUSE = 110,
  EDBM_CLNOR_MODAL_POINTTO_USE_PIVOT = 111,
  EDBM_CLNOR_MODAL_POINTTO_USE_OBJECT = 112,
  EDBM_CLNOR_MODAL_POINTTO_SET_USE_3DCURSOR = 113,
  EDBM_CLNOR_MODAL_POINTTO_SET_USE_SELECTED = 114,
};

wmKeyMap *point_normals_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {EDBM_CLNOR_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
      {EDBM_CLNOR_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},

      /* Point To operator. */
      {EDBM_CLNOR_MODAL_POINTTO_RESET, "RESET", 0, "Reset", "Reset normals to initial ones"},
      {EDBM_CLNOR_MODAL_POINTTO_INVERT,
       "INVERT",
       0,
       "Invert",
       "Toggle inversion of affected normals"},
      {EDBM_CLNOR_MODAL_POINTTO_SPHERIZE,
       "SPHERIZE",
       0,
       "Spherize",
       "Interpolate between new and original normals"},
      {EDBM_CLNOR_MODAL_POINTTO_ALIGN, "ALIGN", 0, "Align", "Make all affected normals parallel"},

      {EDBM_CLNOR_MODAL_POINTTO_USE_MOUSE,
       "USE_MOUSE",
       0,
       "Use Mouse",
       "Follow mouse cursor position"},
      {EDBM_CLNOR_MODAL_POINTTO_USE_PIVOT,
       "USE_PIVOT",
       0,
       "Use Pivot",
       "Use current rotation/scaling pivot point coordinates"},
      {EDBM_CLNOR_MODAL_POINTTO_USE_OBJECT,
       "USE_OBJECT",
       0,
       "Use Object",
       "Use current edited object's location"},
      {EDBM_CLNOR_MODAL_POINTTO_SET_USE_3DCURSOR,
       "SET_USE_3DCURSOR",
       0,
       "Set and Use 3D Cursor",
       "Set new 3D cursor position and use it"},
      {EDBM_CLNOR_MODAL_POINTTO_SET_USE_SELECTED,
       "SET_USE_SELECTED",
       0,
       "Select and Use Mesh Item",
       "Select new active mesh element and use its location"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  static const char *keymap_name = "Custom Normals Modal Map";

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, keymap_name);

  /* We only need to add map once */
  if (keymap && keymap->modal_items) {
    return nullptr;
  }

  keymap = WM_modalkeymap_ensure(keyconf, keymap_name, modal_items);

  WM_modalkeymap_assign(keymap, "MESH_OT_point_normals");

  return keymap;
}

#define CLNORS_VALID_VEC_LEN (1e-4f)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Loop Normals 'Point To' Operator
 * \{ */

enum {
  EDBM_CLNOR_POINTTO_MODE_COORDINATES = 1,
  EDBM_CLNOR_POINTTO_MODE_MOUSE = 2,
};

static EnumPropertyItem clnors_pointto_mode_items[] = {
    {EDBM_CLNOR_POINTTO_MODE_COORDINATES,
     "COORDINATES",
     0,
     "Coordinates",
     "Use static coordinates (defined by various means)"},
    {EDBM_CLNOR_POINTTO_MODE_MOUSE, "MOUSE", 0, "Mouse", "Follow mouse cursor"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Initialize loop normal data */
static bool point_normals_init(bContext *C, wmOperator *op)
{
  Object *obedit = CTX_data_edit_object(C);
  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  BMesh *bm = em->bm;

  BKE_editmesh_lnorspace_update(em);
  BMLoopNorEditDataArray *lnors_ed_arr = BM_loop_normal_editdata_array_init(bm, false);

  op->customdata = lnors_ed_arr;

  return (lnors_ed_arr->totloop != 0);
}

static bool point_normals_ensure(bContext *C, wmOperator *op)
{
  if (op->customdata != nullptr) {
    return true;
  }
  return point_normals_init(C, op);
}

static void point_normals_free(wmOperator *op)
{
  if (op->customdata != nullptr) {
    BMLoopNorEditDataArray *lnors_ed_arr = static_cast<BMLoopNorEditDataArray *>(op->customdata);
    BM_loop_normal_editdata_array_free(lnors_ed_arr);
    op->customdata = nullptr;
  }
}

static void point_normals_cancel(bContext *C, wmOperator *op)
{
  point_normals_free(op);
  ED_workspace_status_text(C, nullptr);
}

static void point_normals_update_statusbar(bContext *C, wmOperator *op)
{
  WorkspaceStatus status(C);

  status.opmodal(IFACE_("Confirm"), op->type, EDBM_CLNOR_MODAL_CONFIRM);
  status.opmodal(IFACE_("Cancel"), op->type, EDBM_CLNOR_MODAL_CANCEL);
  status.opmodal(IFACE_("Reset"), op->type, EDBM_CLNOR_MODAL_POINTTO_RESET);

  status.opmodal(IFACE_("Invert"),
                 op->type,
                 EDBM_CLNOR_MODAL_POINTTO_INVERT,
                 RNA_boolean_get(op->ptr, "invert"));
  status.opmodal(IFACE_("Spherize"),
                 op->type,
                 EDBM_CLNOR_MODAL_POINTTO_SPHERIZE,
                 RNA_boolean_get(op->ptr, "spherize"));
  status.opmodal(IFACE_("Align"),
                 op->type,
                 EDBM_CLNOR_MODAL_POINTTO_ALIGN,
                 RNA_boolean_get(op->ptr, "align"));

  status.opmodal(IFACE_("Use mouse"),
                 op->type,
                 EDBM_CLNOR_MODAL_POINTTO_USE_MOUSE,
                 RNA_enum_get(op->ptr, "mode") == EDBM_CLNOR_POINTTO_MODE_MOUSE);

  status.opmodal(IFACE_("Use Pivot"), op->type, EDBM_CLNOR_MODAL_POINTTO_USE_PIVOT);
  status.opmodal(IFACE_("Use Object"), op->type, EDBM_CLNOR_MODAL_POINTTO_USE_OBJECT);
  status.opmodal(
      IFACE_("Set and use 3D cursor"), op->type, EDBM_CLNOR_MODAL_POINTTO_SET_USE_3DCURSOR);
  status.opmodal(
      IFACE_("Select and use mesh item"), op->type, EDBM_CLNOR_MODAL_POINTTO_SET_USE_SELECTED);
}

/* TODO: move that to generic function in BMesh? */
static void bmesh_selected_verts_center_calc(BMesh *bm, float *r_center)
{
  BMVert *v;
  BMIter viter;
  int i = 0;

  zero_v3(r_center);
  BM_ITER_MESH (v, &viter, bm, BM_VERTS_OF_MESH) {
    if (BM_elem_flag_test(v, BM_ELEM_SELECT)) {
      add_v3_v3(r_center, v->co);
      i++;
    }
  }
  mul_v3_fl(r_center, 1.0f / float(i));
}

static void point_normals_apply(bContext *C, wmOperator *op, float target[3], const bool do_reset)
{
  Object *obedit = CTX_data_edit_object(C);
  BMesh *bm = BKE_editmesh_from_object(obedit)->bm;
  BMLoopNorEditDataArray *lnors_ed_arr = static_cast<BMLoopNorEditDataArray *>(op->customdata);

  const bool do_invert = RNA_boolean_get(op->ptr, "invert");
  const bool do_spherize = RNA_boolean_get(op->ptr, "spherize");
  const bool do_align = RNA_boolean_get(op->ptr, "align");
  float center[3];

  if (do_align && !do_reset) {
    bmesh_selected_verts_center_calc(bm, center);
  }

  sub_v3_v3(target, obedit->loc); /* Move target to local coordinates. */

  BMLoopNorEditData *lnor_ed = lnors_ed_arr->lnor_editdata;
  for (int i = 0; i < lnors_ed_arr->totloop; i++, lnor_ed++) {
    if (do_reset) {
      copy_v3_v3(lnor_ed->nloc, lnor_ed->niloc);
    }
    else if (do_spherize) {
      /* Note that this is *not* real spherical interpolation.
       * Probably good enough in this case though? */
      const float strength = RNA_float_get(op->ptr, "spherize_strength");
      float spherized_normal[3];

      sub_v3_v3v3(spherized_normal, target, lnor_ed->loc);

      /* otherwise, multiplication by strength is meaningless... */
      normalize_v3(spherized_normal);

      mul_v3_fl(spherized_normal, strength);
      mul_v3_v3fl(lnor_ed->nloc, lnor_ed->niloc, 1.0f - strength);
      add_v3_v3(lnor_ed->nloc, spherized_normal);
    }
    else if (do_align) {
      sub_v3_v3v3(lnor_ed->nloc, target, center);
    }
    else {
      sub_v3_v3v3(lnor_ed->nloc, target, lnor_ed->loc);
    }

    if (do_invert && !do_reset) {
      negate_v3(lnor_ed->nloc);
    }
    if (normalize_v3(lnor_ed->nloc) >= CLNORS_VALID_VEC_LEN) {
      BKE_lnor_space_custom_normal_to_data(
          bm->lnor_spacearr->lspacearr[lnor_ed->loop_index], lnor_ed->nloc, lnor_ed->clnors_data);
    }
  }
}

static wmOperatorStatus edbm_point_normals_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* As this operator passes events through, we can't be sure the user didn't exit edit-mode.
   * or performed some other operation. */
  if (!WM_operator_poll(C, op->type)) {
    point_normals_cancel(C, op);
    return OPERATOR_CANCELLED;
  }

  View3D *v3d = CTX_wm_view3d(C);
  Scene *scene = CTX_data_scene(C);
  Object *obedit = CTX_data_edit_object(C);
  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  BMesh *bm = em->bm;

  float target[3];

  wmOperatorStatus ret = OPERATOR_PASS_THROUGH;
  int mode = RNA_enum_get(op->ptr, "mode");
  int new_mode = mode;
  bool force_mousemove = false;
  bool do_reset = false;

  PropertyRNA *prop_target = RNA_struct_find_property(op->ptr, "target_location");

  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case EDBM_CLNOR_MODAL_CONFIRM:
        RNA_property_float_get_array(op->ptr, prop_target, target);
        ret = OPERATOR_FINISHED;
        break;

      case EDBM_CLNOR_MODAL_CANCEL:
        do_reset = true;
        ret = OPERATOR_CANCELLED;
        break;

      case EDBM_CLNOR_MODAL_POINTTO_RESET:
        do_reset = true;
        ret = OPERATOR_RUNNING_MODAL;
        break;

      case EDBM_CLNOR_MODAL_POINTTO_INVERT: {
        PropertyRNA *prop_invert = RNA_struct_find_property(op->ptr, "invert");
        RNA_property_boolean_set(
            op->ptr, prop_invert, !RNA_property_boolean_get(op->ptr, prop_invert));
        RNA_property_float_get_array(op->ptr, prop_target, target);
        ret = OPERATOR_RUNNING_MODAL;
        break;
      }

      case EDBM_CLNOR_MODAL_POINTTO_SPHERIZE: {
        PropertyRNA *prop_spherize = RNA_struct_find_property(op->ptr, "spherize");
        RNA_property_boolean_set(
            op->ptr, prop_spherize, !RNA_property_boolean_get(op->ptr, prop_spherize));
        RNA_property_float_get_array(op->ptr, prop_target, target);
        ret = OPERATOR_RUNNING_MODAL;
        break;
      }

      case EDBM_CLNOR_MODAL_POINTTO_ALIGN: {
        PropertyRNA *prop_align = RNA_struct_find_property(op->ptr, "align");
        RNA_property_boolean_set(
            op->ptr, prop_align, !RNA_property_boolean_get(op->ptr, prop_align));
        RNA_property_float_get_array(op->ptr, prop_target, target);
        ret = OPERATOR_RUNNING_MODAL;
        break;
      }

      case EDBM_CLNOR_MODAL_POINTTO_USE_MOUSE:
        new_mode = EDBM_CLNOR_POINTTO_MODE_MOUSE;
        /* We want to immediately update to mouse cursor position... */
        force_mousemove = true;
        ret = OPERATOR_RUNNING_MODAL;
        break;

      case EDBM_CLNOR_MODAL_POINTTO_USE_OBJECT:
        new_mode = EDBM_CLNOR_POINTTO_MODE_COORDINATES;
        copy_v3_v3(target, obedit->loc);
        ret = OPERATOR_RUNNING_MODAL;
        break;

      case EDBM_CLNOR_MODAL_POINTTO_SET_USE_3DCURSOR:
        new_mode = EDBM_CLNOR_POINTTO_MODE_COORDINATES;
        ED_view3d_cursor3d_update(C, event->mval, false, V3D_CURSOR_ORIENT_NONE);
        copy_v3_v3(target, scene->cursor.location);
        ret = OPERATOR_RUNNING_MODAL;
        break;

      case EDBM_CLNOR_MODAL_POINTTO_SET_USE_SELECTED: {
        new_mode = EDBM_CLNOR_POINTTO_MODE_COORDINATES;
        view3d_operator_needs_gpu(C);
        SelectPick_Params params{};
        params.sel_op = SEL_OP_SET;
        if (EDBM_select_pick(C, event->mval, params)) {
          /* Point to newly selected active. */
          blender::ed::object::calc_active_center_for_editmode(obedit, false, target);

          add_v3_v3(target, obedit->loc);
          ret = OPERATOR_RUNNING_MODAL;
        }
        break;
      }
      case EDBM_CLNOR_MODAL_POINTTO_USE_PIVOT: {
        new_mode = EDBM_CLNOR_POINTTO_MODE_COORDINATES;
        switch (scene->toolsettings->transform_pivot_point) {
          case V3D_AROUND_CENTER_BOUNDS: /* calculateCenterBound */
          {
            BMVert *v;
            BMIter viter;
            float min[3], max[3];
            int i = 0;

            BM_ITER_MESH (v, &viter, bm, BM_VERTS_OF_MESH) {
              if (BM_elem_flag_test(v, BM_ELEM_SELECT)) {
                if (i) {
                  minmax_v3v3_v3(min, max, v->co);
                }
                else {
                  copy_v3_v3(min, v->co);
                  copy_v3_v3(max, v->co);
                }
                i++;
              }
            }
            mid_v3_v3v3(target, min, max);
            add_v3_v3(target, obedit->loc);
            break;
          }

          case V3D_AROUND_CENTER_MEDIAN: {
            bmesh_selected_verts_center_calc(bm, target);
            add_v3_v3(target, obedit->loc);
            break;
          }

          case V3D_AROUND_CURSOR:
            copy_v3_v3(target, scene->cursor.location);
            break;

          case V3D_AROUND_ACTIVE:
            if (!blender::ed::object::calc_active_center_for_editmode(obedit, false, target)) {
              zero_v3(target);
            }
            add_v3_v3(target, obedit->loc);
            break;

          default:
            BKE_report(op->reports, RPT_WARNING, "Does not support Individual Origins as pivot");
            copy_v3_v3(target, obedit->loc);
        }
        ret = OPERATOR_RUNNING_MODAL;
        break;
      }
      default:
        break;
    }
  }

  if (new_mode != mode) {
    mode = new_mode;
    RNA_enum_set(op->ptr, "mode", mode);
  }

  /* Only handle mouse-move event in case we are in mouse mode. */
  if (event->type == MOUSEMOVE || force_mousemove) {
    if (mode == EDBM_CLNOR_POINTTO_MODE_MOUSE) {
      ARegion *region = CTX_wm_region(C);
      float center[3];

      bmesh_selected_verts_center_calc(bm, center);

      ED_view3d_win_to_3d_int(v3d, region, center, event->mval, target);

      ret = OPERATOR_RUNNING_MODAL;
    }
  }

  if (ret != OPERATOR_PASS_THROUGH) {
    if (!ELEM(ret, OPERATOR_CANCELLED, OPERATOR_FINISHED)) {
      RNA_property_float_set_array(op->ptr, prop_target, target);
    }

    if (point_normals_ensure(C, op)) {
      point_normals_apply(C, op, target, do_reset);
      EDBMUpdate_Params params{};
      params.calc_looptris = true;
      params.calc_normals = false;
      params.is_destructive = false;
      /* Recheck booleans. */
      EDBM_update(static_cast<Mesh *>(obedit->data), &params);

      point_normals_update_statusbar(C, op);
    }
    else {
      ret = OPERATOR_CANCELLED;
    }
  }

  if (ELEM(ret, OPERATOR_CANCELLED, OPERATOR_FINISHED)) {
    point_normals_cancel(C, op);
  }

  /* If we allow other tools to run, we can't be sure if they will re-allocate
   * the data this operator uses, see: #68159.
   * Free the data here, then use #point_normals_ensure to add it back on demand. */
  if (ret == OPERATOR_PASS_THROUGH) {
    /* Don't free on mouse-move, causes creation/freeing of the loop data in an inefficient way. */
    if (!ISMOUSE_MOTION(event->type)) {
      point_normals_free(op);
    }
  }
  return ret;
}

static wmOperatorStatus edbm_point_normals_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent * /*event*/)
{
  if (!point_normals_init(C, op)) {
    point_normals_cancel(C, op);
    return OPERATOR_CANCELLED;
  }

  WM_event_add_modal_handler(C, op);

  point_normals_update_statusbar(C, op);

  op->flag |= OP_IS_MODAL_GRAB_CURSOR;
  return OPERATOR_RUNNING_MODAL;
}

/* TODO: make this work on multiple objects at once */
static wmOperatorStatus edbm_point_normals_exec(bContext *C, wmOperator *op)
{
  Object *obedit = CTX_data_edit_object(C);

  if (!point_normals_init(C, op)) {
    point_normals_cancel(C, op);
    return OPERATOR_CANCELLED;
  }

  /* Note that 'mode' is ignored in exec case,
   * we directly use vector stored in target_location, whatever that is. */

  float target[3];
  RNA_float_get_array(op->ptr, "target_location", target);

  point_normals_apply(C, op, target, false);

  EDBMUpdate_Params params{};
  params.calc_looptris = true;
  params.calc_normals = false;
  params.is_destructive = false;
  EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  point_normals_cancel(C, op);

  return OPERATOR_FINISHED;
}

static bool point_normals_draw_check_prop(PointerRNA *ptr, PropertyRNA *prop, void * /*user_data*/)
{
  const char *prop_id = RNA_property_identifier(prop);

  /* Only show strength option if spherize is enabled. */
  if (STREQ(prop_id, "spherize_strength")) {
    return RNA_boolean_get(ptr, "spherize");
  }

  /* Else, show it! */
  return true;
}

static void edbm_point_normals_ui(bContext *C, wmOperator *op)
{
  uiLayout *layout = op->layout;
  wmWindowManager *wm = CTX_wm_manager(C);

  PointerRNA ptr = RNA_pointer_create_discrete(&wm->id, op->type->srna, op->properties);

  layout->use_property_split_set(true);

  /* Main auto-draw call */
  uiDefAutoButsRNA(layout,
                   &ptr,
                   point_normals_draw_check_prop,
                   nullptr,
                   nullptr,
                   UI_BUT_LABEL_ALIGN_NONE,
                   false);
}

void MESH_OT_point_normals(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Point Normals to Target";
  ot->description = "Point selected custom normals to specified Target";
  ot->idname = "MESH_OT_point_normals";

  /* API callbacks. */
  ot->exec = edbm_point_normals_exec;
  ot->invoke = edbm_point_normals_invoke;
  ot->modal = edbm_point_normals_modal;
  ot->poll = ED_operator_editmesh;
  ot->ui = edbm_point_normals_ui;
  ot->cancel = point_normals_cancel;

  /* flags */
  ot->flag = OPTYPE_BLOCKING | OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "mode",
                          clnors_pointto_mode_items,
                          EDBM_CLNOR_POINTTO_MODE_COORDINATES,
                          "Mode",
                          "How to define coordinates to point custom normals to");
  RNA_def_property_flag(ot->prop, PROP_HIDDEN);

  RNA_def_boolean(ot->srna, "invert", false, "Invert", "Invert affected normals");

  RNA_def_boolean(ot->srna, "align", false, "Align", "Make all affected normals parallel");

  RNA_def_float_vector_xyz(ot->srna,
                           "target_location",
                           3,
                           nullptr,
                           -FLT_MAX,
                           FLT_MAX,
                           "Target",
                           "Target location to which normals will point",
                           -1000.0f,
                           1000.0f);

  RNA_def_boolean(
      ot->srna, "spherize", false, "Spherize", "Interpolate between original and new normals");

  RNA_def_float(ot->srna,
                "spherize_strength",
                0.1,
                0.0f,
                1.0f,
                "Spherize Strength",
                "Ratio of spherized normal to original normal",
                0.0f,
                1.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Split/Merge Loop Normals Operator
 * \{ */

static void normals_merge(BMesh *bm, BMLoopNorEditDataArray *lnors_ed_arr)
{
  BMLoopNorEditData *lnor_ed = lnors_ed_arr->lnor_editdata;

  BLI_SMALLSTACK_DECLARE(clnors, short *);

  BLI_assert(bm->lnor_spacearr->data_type == MLNOR_SPACEARR_BMLOOP_PTR);

  BM_normals_loops_edges_tag(bm, false);

  for (int i = 0; i < lnors_ed_arr->totloop; i++, lnor_ed++) {
    BLI_assert(BLI_SMALLSTACK_IS_EMPTY(clnors));

    if (BM_elem_flag_test(lnor_ed->loop, BM_ELEM_TAG)) {
      continue;
    }

    MLoopNorSpace *lnor_space = bm->lnor_spacearr->lspacearr[lnor_ed->loop_index];

    if ((lnor_space->flags & MLNOR_SPACE_IS_SINGLE) == 0) {
      LinkNode *loops = lnor_space->loops;
      float avg_normal[3] = {0.0f, 0.0f, 0.0f};
      short *clnors_data;

      for (; loops; loops = loops->next) {
        BMLoop *l = static_cast<BMLoop *>(loops->link);
        const int loop_index = BM_elem_index_get(l);

        BMLoopNorEditData *lnor_ed_tmp = lnors_ed_arr->lidx_to_lnor_editdata[loop_index];
        BLI_assert(lnor_ed_tmp->loop_index == loop_index && lnor_ed_tmp->loop == l);
        add_v3_v3(avg_normal, lnor_ed_tmp->nloc);
        BLI_SMALLSTACK_PUSH(clnors, lnor_ed_tmp->clnors_data);
        BM_elem_flag_enable(l, BM_ELEM_TAG);
      }
      if (normalize_v3(avg_normal) < CLNORS_VALID_VEC_LEN) {
        /* If avg normal is nearly 0, set clnor to default value. */
        zero_v3(avg_normal);
      }
      while ((clnors_data = static_cast<short *>(BLI_SMALLSTACK_POP(clnors)))) {
        BKE_lnor_space_custom_normal_to_data(lnor_space, avg_normal, clnors_data);
      }
    }
  }
}

static void normals_split(BMesh *bm)
{
  BMFace *f;
  BMLoop *l, *l_curr, *l_first;
  BMIter fiter;

  BLI_assert(bm->lnor_spacearr->data_type == MLNOR_SPACEARR_BMLOOP_PTR);

  BM_normals_loops_edges_tag(bm, true);

  BLI_SMALLSTACK_DECLARE(loop_stack, BMLoop *);

  const int cd_clnors_offset = CustomData_get_offset_named(
      &bm->ldata, CD_PROP_INT16_2D, "custom_normal");
  BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
    BLI_assert(BLI_SMALLSTACK_IS_EMPTY(loop_stack));

    l_curr = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      if (BM_elem_flag_test(l_curr->v, BM_ELEM_SELECT) &&
          (!BM_elem_flag_test(l_curr->e, BM_ELEM_TAG) ||
           (!BM_elem_flag_test(l_curr, BM_ELEM_TAG) && BM_loop_check_cyclic_smooth_fan(l_curr))))
      {
        if (!BM_elem_flag_test(l_curr->e, BM_ELEM_TAG) &&
            !BM_elem_flag_test(l_curr->prev->e, BM_ELEM_TAG))
        {
          const int loop_index = BM_elem_index_get(l_curr);
          short *clnors = static_cast<short *>(BM_ELEM_CD_GET_VOID_P(l_curr, cd_clnors_offset));
          BKE_lnor_space_custom_normal_to_data(
              bm->lnor_spacearr->lspacearr[loop_index], f->no, clnors);
        }
        else {
          BMVert *v_pivot = l_curr->v;
          UNUSED_VARS_NDEBUG(v_pivot);
          BMEdge *e_next;
          const BMEdge *e_org = l_curr->e;
          BMLoop *lfan_pivot, *lfan_pivot_next;

          lfan_pivot = l_curr;
          e_next = lfan_pivot->e;
          float avg_normal[3] = {0.0f};

          while (true) {
            lfan_pivot_next = BM_vert_step_fan_loop(lfan_pivot, &e_next);
            if (lfan_pivot_next) {
              BLI_assert(lfan_pivot_next->v == v_pivot);
            }
            else {
              e_next = (lfan_pivot->e == e_next) ? lfan_pivot->prev->e : lfan_pivot->e;
            }

            BLI_SMALLSTACK_PUSH(loop_stack, lfan_pivot);
            add_v3_v3(avg_normal, lfan_pivot->f->no);

            if (!BM_elem_flag_test(e_next, BM_ELEM_TAG) || (e_next == e_org)) {
              break;
            }
            lfan_pivot = lfan_pivot_next;
          }
          if (normalize_v3(avg_normal) < CLNORS_VALID_VEC_LEN) {
            /* If avg normal is nearly 0, set clnor to default value. */
            zero_v3(avg_normal);
          }
          while ((l = static_cast<BMLoop *>(BLI_SMALLSTACK_POP(loop_stack)))) {
            const int l_index = BM_elem_index_get(l);
            short *clnors = static_cast<short *>(BM_ELEM_CD_GET_VOID_P(l, cd_clnors_offset));
            BKE_lnor_space_custom_normal_to_data(
                bm->lnor_spacearr->lspacearr[l_index], avg_normal, clnors);
          }
        }
      }
    } while ((l_curr = l_curr->next) != l_first);
  }
}

static wmOperatorStatus normals_split_merge(bContext *C, const bool do_merge)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    BMEdge *e;
    BMIter eiter;

    BKE_editmesh_lnorspace_update(em);

    /* Note that we need temp lnor editing data for all loops of all affected vertices, since by
     * setting some faces/edges as smooth we are going to change clnors spaces... See also #65809.
     */
    BMLoopNorEditDataArray *lnors_ed_arr = do_merge ?
                                               BM_loop_normal_editdata_array_init(bm, true) :
                                               nullptr;

    mesh_set_smooth_faces(em, do_merge);

    BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
      if (BM_elem_flag_test(e, BM_ELEM_SELECT)) {
        BM_elem_flag_set(e, BM_ELEM_SMOOTH, do_merge);
      }
    }

    bm->spacearr_dirty |= BM_SPACEARR_DIRTY_ALL;
    BKE_editmesh_lnorspace_update(em);

    if (do_merge) {
      normals_merge(bm, lnors_ed_arr);
    }
    else {
      normals_split(bm);
    }

    if (lnors_ed_arr) {
      BM_loop_normal_editdata_array_free(lnors_ed_arr);
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus edbm_merge_normals_exec(bContext *C, wmOperator * /*op*/)
{
  return normals_split_merge(C, true);
}

void MESH_OT_merge_normals(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Merge Normals";
  ot->description = "Merge custom normals of selected vertices";
  ot->idname = "MESH_OT_merge_normals";

  /* API callbacks. */
  ot->exec = edbm_merge_normals_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus edbm_split_normals_exec(bContext *C, wmOperator * /*op*/)
{
  return normals_split_merge(C, false);
}

void MESH_OT_split_normals(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Split Normals";
  ot->description = "Split custom normals of selected vertices";
  ot->idname = "MESH_OT_split_normals";

  /* API callbacks. */
  ot->exec = edbm_split_normals_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Average Loop Normals Operator
 * \{ */

enum {
  EDBM_CLNOR_AVERAGE_LOOP = 1,
  EDBM_CLNOR_AVERAGE_FACE_AREA = 2,
  EDBM_CLNOR_AVERAGE_ANGLE = 3,
};

static EnumPropertyItem average_method_items[] = {
    {EDBM_CLNOR_AVERAGE_LOOP,
     "CUSTOM_NORMAL",
     0,
     "Custom Normal",
     "Take average of vertex normals"},
    {EDBM_CLNOR_AVERAGE_FACE_AREA,
     "FACE_AREA",
     0,
     "Face Area",
     "Set all vertex normals by face area"},
    {EDBM_CLNOR_AVERAGE_ANGLE,
     "CORNER_ANGLE",
     0,
     "Corner Angle",
     "Set all vertex normals by corner angle"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus edbm_average_normals_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  const int average_type = RNA_enum_get(op->ptr, "average_type");
  const float absweight = float(RNA_int_get(op->ptr, "weight"));
  const float threshold = RNA_float_get(op->ptr, "threshold");

  HeapSimple *loop_weight = BLI_heapsimple_new();
  BLI_SMALLSTACK_DECLARE(loop_stack, BMLoop *);

  for (uint ob_index = 0; ob_index < objects.size(); ob_index++) {
    BLI_assert(BLI_SMALLSTACK_IS_EMPTY(loop_stack));
    BLI_assert(BLI_heapsimple_is_empty(loop_weight));

    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    BMFace *f;
    BMLoop *l, *l_curr, *l_first;
    BMIter fiter;

    bm->spacearr_dirty |= BM_SPACEARR_DIRTY_ALL;
    BKE_editmesh_lnorspace_update(em);

    const int cd_clnors_offset = CustomData_get_offset_named(
        &bm->ldata, CD_PROP_INT16_2D, "custom_normal");

    float weight = absweight / 50.0f;
    if (absweight == 100.0f) {
      weight = float(SHRT_MAX);
    }
    else if (absweight == 1.0f) {
      weight = 1 / float(SHRT_MAX);
    }
    else if ((weight - 1) * 25 > 1) {
      weight = (weight - 1) * 25;
    }

    BM_normals_loops_edges_tag(bm, true);

    BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
      l_curr = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        if (BM_elem_flag_test(l_curr->v, BM_ELEM_SELECT) &&
            (!BM_elem_flag_test(l_curr->e, BM_ELEM_TAG) ||
             (!BM_elem_flag_test(l_curr, BM_ELEM_TAG) && BM_loop_check_cyclic_smooth_fan(l_curr))))
        {
          if (!BM_elem_flag_test(l_curr->e, BM_ELEM_TAG) &&
              !BM_elem_flag_test(l_curr->prev->e, BM_ELEM_TAG))
          {
            const int loop_index = BM_elem_index_get(l_curr);
            short *clnors = static_cast<short *>(BM_ELEM_CD_GET_VOID_P(l_curr, cd_clnors_offset));
            BKE_lnor_space_custom_normal_to_data(
                bm->lnor_spacearr->lspacearr[loop_index], f->no, clnors);
          }
          else {
            BMVert *v_pivot = l_curr->v;
            UNUSED_VARS_NDEBUG(v_pivot);
            BMEdge *e_next;
            const BMEdge *e_org = l_curr->e;
            BMLoop *lfan_pivot, *lfan_pivot_next;

            lfan_pivot = l_curr;
            e_next = lfan_pivot->e;

            while (true) {
              lfan_pivot_next = BM_vert_step_fan_loop(lfan_pivot, &e_next);
              if (lfan_pivot_next) {
                BLI_assert(lfan_pivot_next->v == v_pivot);
              }
              else {
                e_next = (lfan_pivot->e == e_next) ? lfan_pivot->prev->e : lfan_pivot->e;
              }

              float val = 1.0f;
              if (average_type == EDBM_CLNOR_AVERAGE_FACE_AREA) {
                val = 1.0f / BM_face_calc_area(lfan_pivot->f);
              }
              else if (average_type == EDBM_CLNOR_AVERAGE_ANGLE) {
                val = 1.0f / BM_loop_calc_face_angle(lfan_pivot);
              }

              BLI_heapsimple_insert(loop_weight, val, lfan_pivot);

              if (!BM_elem_flag_test(e_next, BM_ELEM_TAG) || (e_next == e_org)) {
                break;
              }
              lfan_pivot = lfan_pivot_next;
            }

            float wnor[3], avg_normal[3] = {0.0f}, count = 0;
            float val = BLI_heapsimple_top_value(loop_weight);

            while (!BLI_heapsimple_is_empty(loop_weight)) {
              const float cur_val = BLI_heapsimple_top_value(loop_weight);
              if (!compare_ff(val, cur_val, threshold)) {
                count++;
                val = cur_val;
              }
              l = static_cast<BMLoop *>(BLI_heapsimple_pop_min(loop_weight));
              BLI_SMALLSTACK_PUSH(loop_stack, l);

              const float n_weight = pow(weight, count);

              if (average_type == EDBM_CLNOR_AVERAGE_LOOP) {
                const int l_index = BM_elem_index_get(l);
                short *clnors = static_cast<short *>(BM_ELEM_CD_GET_VOID_P(l, cd_clnors_offset));
                BKE_lnor_space_custom_data_to_normal(
                    bm->lnor_spacearr->lspacearr[l_index], clnors, wnor);
              }
              else {
                copy_v3_v3(wnor, l->f->no);
              }
              mul_v3_fl(wnor, (1.0f / cur_val) * (1.0f / n_weight));
              add_v3_v3(avg_normal, wnor);
            }

            if (normalize_v3(avg_normal) < CLNORS_VALID_VEC_LEN) {
              /* If avg normal is nearly 0, set clnor to default value. */
              zero_v3(avg_normal);
            }
            while ((l = static_cast<BMLoop *>(BLI_SMALLSTACK_POP(loop_stack)))) {
              const int l_index = BM_elem_index_get(l);
              short *clnors = static_cast<short *>(BM_ELEM_CD_GET_VOID_P(l, cd_clnors_offset));
              BKE_lnor_space_custom_normal_to_data(
                  bm->lnor_spacearr->lspacearr[l_index], avg_normal, clnors);
            }
          }
        }
      } while ((l_curr = l_curr->next) != l_first);
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  BLI_heapsimple_free(loop_weight, nullptr);

  return OPERATOR_FINISHED;
}

static bool average_normals_draw_check_prop(PointerRNA *ptr,
                                            PropertyRNA *prop,
                                            void * /*user_data*/)
{
  const char *prop_id = RNA_property_identifier(prop);
  const int average_type = RNA_enum_get(ptr, "average_type");

  /* Only show weight/threshold options when not in loop average type. */
  const bool is_clor_average_loop = average_type == EDBM_CLNOR_AVERAGE_LOOP;
  if (STREQ(prop_id, "weight")) {
    return !is_clor_average_loop;
  }
  if (STREQ(prop_id, "threshold")) {
    return !is_clor_average_loop;
  }

  /* Else, show it! */
  return true;
}

static void edbm_average_normals_ui(bContext *C, wmOperator *op)
{
  uiLayout *layout = op->layout;
  wmWindowManager *wm = CTX_wm_manager(C);

  PointerRNA ptr = RNA_pointer_create_discrete(&wm->id, op->type->srna, op->properties);

  layout->use_property_split_set(true);

  /* Main auto-draw call */
  uiDefAutoButsRNA(layout,
                   &ptr,
                   average_normals_draw_check_prop,
                   nullptr,
                   nullptr,
                   UI_BUT_LABEL_ALIGN_NONE,
                   false);
}

void MESH_OT_average_normals(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Average Normals";
  ot->description = "Average custom normals of selected vertices";
  ot->idname = "MESH_OT_average_normals";

  /* API callbacks. */
  ot->exec = edbm_average_normals_exec;
  ot->poll = ED_operator_editmesh;
  ot->ui = edbm_average_normals_ui;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "average_type",
                          average_method_items,
                          EDBM_CLNOR_AVERAGE_LOOP,
                          "Type",
                          "Averaging method");

  RNA_def_int(ot->srna, "weight", 50, 1, 100, "Weight", "Weight applied per face", 1, 100);

  RNA_def_float(ot->srna,
                "threshold",
                0.01f,
                0,
                10,
                "Threshold",
                "Threshold value for different weights to be considered equal",
                0,
                5);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Custom Normal Interface Tools Operator
 * \{ */

enum {
  EDBM_CLNOR_TOOLS_COPY = 1,
  EDBM_CLNOR_TOOLS_PASTE = 2,
  EDBM_CLNOR_TOOLS_MULTIPLY = 3,
  EDBM_CLNOR_TOOLS_ADD = 4,
  EDBM_CLNOR_TOOLS_RESET = 5,
};

static EnumPropertyItem normal_vector_tool_items[] = {
    {EDBM_CLNOR_TOOLS_COPY, "COPY", 0, "Copy Normal", "Copy normal to the internal clipboard"},
    {EDBM_CLNOR_TOOLS_PASTE,
     "PASTE",
     0,
     "Paste Normal",
     "Paste normal from the internal clipboard"},
    {EDBM_CLNOR_TOOLS_ADD, "ADD", 0, "Add Normal", "Add normal vector with selection"},
    {EDBM_CLNOR_TOOLS_MULTIPLY,
     "MULTIPLY",
     0,
     "Multiply Normal",
     "Multiply normal vector with selection"},
    {EDBM_CLNOR_TOOLS_RESET,
     "RESET",
     0,
     "Reset Normal",
     "Reset the internal clipboard and/or normal of selected element"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus edbm_normals_tools_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  const int mode = RNA_enum_get(op->ptr, "mode");
  const bool absolute = RNA_boolean_get(op->ptr, "absolute");
  float *normal_vector = scene->toolsettings->normal_vector;
  bool done_copy = false;

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if (bm->totloop == 0) {
      continue;
    }

    BKE_editmesh_lnorspace_update(em);
    BMLoopNorEditDataArray *lnors_ed_arr = BM_loop_normal_editdata_array_init(bm, false);
    BMLoopNorEditData *lnor_ed = lnors_ed_arr->lnor_editdata;

    switch (mode) {
      case EDBM_CLNOR_TOOLS_COPY:
        if (bm->totfacesel == 0 && bm->totvertsel == 0) {
          BM_loop_normal_editdata_array_free(lnors_ed_arr);
          continue;
        }

        if (done_copy ||
            (bm->totfacesel != 1 && lnors_ed_arr->totloop != 1 && bm->totvertsel != 1))
        {
          BKE_report(op->reports,
                     RPT_ERROR,
                     "Can only copy one custom normal, vertex normal or face normal");
          BM_loop_normal_editdata_array_free(lnors_ed_arr);
          continue;
        }
        if (lnors_ed_arr->totloop == 1) {
          copy_v3_v3(scene->toolsettings->normal_vector, lnors_ed_arr->lnor_editdata->nloc);
        }
        else if (bm->totfacesel == 1) {
          BMFace *f;
          BMIter fiter;
          BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
            if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
              copy_v3_v3(scene->toolsettings->normal_vector, f->no);
            }
          }
        }
        else {
          /* 'Vertex' normal, i.e. common set of loop normals on the same vertex,
           * only if they are all the same. */
          bool are_same_lnors = true;
          for (int i = 0; i < lnors_ed_arr->totloop; i++, lnor_ed++) {
            if (!compare_v3v3(lnors_ed_arr->lnor_editdata->nloc, lnor_ed->nloc, 1e-4f)) {
              are_same_lnors = false;
            }
          }
          if (are_same_lnors) {
            copy_v3_v3(scene->toolsettings->normal_vector, lnors_ed_arr->lnor_editdata->nloc);
          }
        }
        done_copy = true;
        break;

      case EDBM_CLNOR_TOOLS_PASTE:
        if (!absolute) {
          if (normalize_v3(normal_vector) < CLNORS_VALID_VEC_LEN) {
            /* If normal is nearly 0, do nothing. */
            break;
          }
        }
        for (int i = 0; i < lnors_ed_arr->totloop; i++, lnor_ed++) {
          if (absolute) {
            float abs_normal[3];
            copy_v3_v3(abs_normal, lnor_ed->loc);
            negate_v3(abs_normal);
            add_v3_v3(abs_normal, normal_vector);

            if (normalize_v3(abs_normal) < CLNORS_VALID_VEC_LEN) {
              /* If abs normal is nearly 0, set clnor to initial value. */
              copy_v3_v3(abs_normal, lnor_ed->niloc);
            }
            BKE_lnor_space_custom_normal_to_data(bm->lnor_spacearr->lspacearr[lnor_ed->loop_index],
                                                 abs_normal,
                                                 lnor_ed->clnors_data);
          }
          else {
            BKE_lnor_space_custom_normal_to_data(bm->lnor_spacearr->lspacearr[lnor_ed->loop_index],
                                                 normal_vector,
                                                 lnor_ed->clnors_data);
          }
        }
        break;

      case EDBM_CLNOR_TOOLS_MULTIPLY:
        for (int i = 0; i < lnors_ed_arr->totloop; i++, lnor_ed++) {
          mul_v3_v3(lnor_ed->nloc, normal_vector);

          if (normalize_v3(lnor_ed->nloc) < CLNORS_VALID_VEC_LEN) {
            /* If abs normal is nearly 0, set clnor to initial value. */
            copy_v3_v3(lnor_ed->nloc, lnor_ed->niloc);
          }
          BKE_lnor_space_custom_normal_to_data(bm->lnor_spacearr->lspacearr[lnor_ed->loop_index],
                                               lnor_ed->nloc,
                                               lnor_ed->clnors_data);
        }
        break;

      case EDBM_CLNOR_TOOLS_ADD:
        for (int i = 0; i < lnors_ed_arr->totloop; i++, lnor_ed++) {
          add_v3_v3(lnor_ed->nloc, normal_vector);

          if (normalize_v3(lnor_ed->nloc) < CLNORS_VALID_VEC_LEN) {
            /* If abs normal is nearly 0, set clnor to initial value. */
            copy_v3_v3(lnor_ed->nloc, lnor_ed->niloc);
          }
          BKE_lnor_space_custom_normal_to_data(bm->lnor_spacearr->lspacearr[lnor_ed->loop_index],
                                               lnor_ed->nloc,
                                               lnor_ed->clnors_data);
        }
        break;

      case EDBM_CLNOR_TOOLS_RESET:
        zero_v3(normal_vector);
        for (int i = 0; i < lnors_ed_arr->totloop; i++, lnor_ed++) {
          BKE_lnor_space_custom_normal_to_data(bm->lnor_spacearr->lspacearr[lnor_ed->loop_index],
                                               normal_vector,
                                               lnor_ed->clnors_data);
        }
        break;

      default:
        BLI_assert(0);
        break;
    }

    BM_loop_normal_editdata_array_free(lnors_ed_arr);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

static bool normals_tools_draw_check_prop(PointerRNA *ptr, PropertyRNA *prop, void * /*user_data*/)
{
  const char *prop_id = RNA_property_identifier(prop);
  const int mode = RNA_enum_get(ptr, "mode");

  /* Only show absolute option in paste mode. */
  if (STREQ(prop_id, "absolute")) {
    return (mode == EDBM_CLNOR_TOOLS_PASTE);
  }

  /* Else, show it! */
  return true;
}

static void edbm_normals_tools_ui(bContext *C, wmOperator *op)
{
  uiLayout *layout = op->layout;
  wmWindowManager *wm = CTX_wm_manager(C);

  PointerRNA ptr = RNA_pointer_create_discrete(&wm->id, op->type->srna, op->properties);

  /* Main auto-draw call */
  uiDefAutoButsRNA(layout,
                   &ptr,
                   normals_tools_draw_check_prop,
                   nullptr,
                   nullptr,
                   UI_BUT_LABEL_ALIGN_NONE,
                   false);
}

void MESH_OT_normals_tools(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Normals Vector Tools";
  ot->description = "Custom normals tools using Normal Vector of UI";
  ot->idname = "MESH_OT_normals_tools";

  /* API callbacks. */
  ot->exec = edbm_normals_tools_exec;
  ot->poll = ED_operator_editmesh;
  ot->ui = edbm_normals_tools_ui;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "mode",
                          normal_vector_tool_items,
                          EDBM_CLNOR_TOOLS_COPY,
                          "Mode",
                          "Mode of tools taking input from interface");
  RNA_def_property_flag(ot->prop, PROP_HIDDEN);

  RNA_def_boolean(ot->srna,
                  "absolute",
                  false,
                  "Absolute Coordinates",
                  "Copy Absolute coordinates of Normal vector");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Normals from Faces Operator
 * \{ */

static wmOperatorStatus edbm_set_normals_from_faces_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    if (bm->totfacesel == 0) {
      continue;
    }

    BMFace *f;
    BMVert *v;
    BMEdge *e;
    BMLoop *l;
    BMIter fiter, viter, eiter, liter;

    const bool keep_sharp = RNA_boolean_get(op->ptr, "keep_sharp");

    BKE_editmesh_lnorspace_update(em);

    float(*vert_normals)[3] = static_cast<float(*)[3]>(
        MEM_mallocN(sizeof(*vert_normals) * bm->totvert, __func__));
    {
      int v_index;
      BM_ITER_MESH_INDEX (v, &viter, bm, BM_VERTS_OF_MESH, v_index) {
        BM_vert_calc_normal_ex(v, BM_ELEM_SELECT, vert_normals[v_index]);
      }
    }

    BLI_bitmap *loop_set = BLI_BITMAP_NEW(bm->totloop, __func__);
    const int cd_clnors_offset = CustomData_get_offset_named(
        &bm->ldata, CD_PROP_INT16_2D, "custom_normal");

    BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
      BM_ITER_ELEM (e, &eiter, f, BM_EDGES_OF_FACE) {
        if (!keep_sharp ||
            (BM_elem_flag_test(e, BM_ELEM_SMOOTH) && BM_elem_flag_test(e, BM_ELEM_SELECT)))
        {
          BM_ITER_ELEM (v, &viter, e, BM_VERTS_OF_EDGE) {
            l = BM_face_vert_share_loop(f, v);
            const int l_index = BM_elem_index_get(l);
            const int v_index = BM_elem_index_get(l->v);

            if (!is_zero_v3(vert_normals[v_index])) {
              short *clnors = static_cast<short *>(BM_ELEM_CD_GET_VOID_P(l, cd_clnors_offset));
              BKE_lnor_space_custom_normal_to_data(
                  bm->lnor_spacearr->lspacearr[l_index], vert_normals[v_index], clnors);

              if (bm->lnor_spacearr->lspacearr[l_index]->flags & MLNOR_SPACE_IS_SINGLE) {
                BLI_BITMAP_ENABLE(loop_set, l_index);
              }
              else {
                LinkNode *loops = bm->lnor_spacearr->lspacearr[l_index]->loops;
                for (; loops; loops = loops->next) {
                  BLI_BITMAP_ENABLE(loop_set, BM_elem_index_get((BMLoop *)loops->link));
                }
              }
            }
          }
        }
      }
    }

    int v_index;
    BM_ITER_MESH_INDEX (v, &viter, bm, BM_VERTS_OF_MESH, v_index) {
      BM_ITER_ELEM (l, &liter, v, BM_LOOPS_OF_VERT) {
        if (BLI_BITMAP_TEST(loop_set, BM_elem_index_get(l))) {
          const int loop_index = BM_elem_index_get(l);
          short *clnors = static_cast<short *>(BM_ELEM_CD_GET_VOID_P(l, cd_clnors_offset));
          BKE_lnor_space_custom_normal_to_data(
              bm->lnor_spacearr->lspacearr[loop_index], vert_normals[v_index], clnors);
        }
      }
    }

    MEM_freeN(loop_set);
    MEM_freeN(vert_normals);
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_set_normals_from_faces(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Set Normals from Faces";
  ot->description = "Set the custom normals from the selected faces ones";
  ot->idname = "MESH_OT_set_normals_from_faces";

  /* API callbacks. */
  ot->exec = edbm_set_normals_from_faces_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "keep_sharp", false, "Keep Sharp Edges", "Do not set sharp edges to face");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Smooth Normal Vectors Operator
 * \{ */

static wmOperatorStatus edbm_smooth_normals_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    BMFace *f;
    BMLoop *l;
    BMIter fiter, liter;

    BKE_editmesh_lnorspace_update(em);
    BMLoopNorEditDataArray *lnors_ed_arr = BM_loop_normal_editdata_array_init(bm, false);

    float(*smooth_normal)[3] = static_cast<float(*)[3]>(
        MEM_callocN(sizeof(*smooth_normal) * lnors_ed_arr->totloop, __func__));

    /* NOTE(@mont29): This is weird choice of operation, taking all loops of faces of current
     * vertex. Could lead to some rather far away loops weighting as much as very close ones
     * (topologically speaking), with complex polygons.
     * Using topological distance here (rather than geometrical one)
     * makes sense IMHO, but would rather go with a more consistent and flexible code,
     * we could even add max topological distance to take into account, and a weighting curve.
     * Would do that later though, think for now we can live with that choice. */
    BMLoopNorEditData *lnor_ed = lnors_ed_arr->lnor_editdata;
    for (int i = 0; i < lnors_ed_arr->totloop; i++, lnor_ed++) {
      l = lnor_ed->loop;
      float loop_normal[3];

      BM_ITER_ELEM (f, &fiter, l->v, BM_FACES_OF_VERT) {
        BMLoop *l_other;
        BM_ITER_ELEM (l_other, &liter, f, BM_LOOPS_OF_FACE) {
          const int l_index_other = BM_elem_index_get(l_other);
          short *clnors = static_cast<short *>(
              BM_ELEM_CD_GET_VOID_P(l_other, lnors_ed_arr->cd_custom_normal_offset));
          BKE_lnor_space_custom_data_to_normal(
              bm->lnor_spacearr->lspacearr[l_index_other], clnors, loop_normal);
          add_v3_v3(smooth_normal[i], loop_normal);
        }
      }
    }

    const float factor = RNA_float_get(op->ptr, "factor");

    lnor_ed = lnors_ed_arr->lnor_editdata;
    for (int i = 0; i < lnors_ed_arr->totloop; i++, lnor_ed++) {
      float current_normal[3];

      if (normalize_v3(smooth_normal[i]) < CLNORS_VALID_VEC_LEN) {
        /* Skip in case the smooth normal is invalid. */
        continue;
      }

      BKE_lnor_space_custom_data_to_normal(
          bm->lnor_spacearr->lspacearr[lnor_ed->loop_index], lnor_ed->clnors_data, current_normal);

      /* NOTE: again, this is not true spherical interpolation that normals would need...
       * But it's probably good enough for now. */
      mul_v3_fl(current_normal, 1.0f - factor);
      mul_v3_fl(smooth_normal[i], factor);
      add_v3_v3(current_normal, smooth_normal[i]);

      if (normalize_v3(current_normal) < CLNORS_VALID_VEC_LEN) {
        /* Skip in case the smoothed normal is invalid. */
        continue;
      }

      BKE_lnor_space_custom_normal_to_data(
          bm->lnor_spacearr->lspacearr[lnor_ed->loop_index], current_normal, lnor_ed->clnors_data);
    }

    BM_loop_normal_editdata_array_free(lnors_ed_arr);
    MEM_freeN(smooth_normal);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_smooth_normals(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Smooth Normals Vectors";
  ot->description = "Smooth custom normals based on adjacent vertex normals";
  ot->idname = "MESH_OT_smooth_normals";

  /* API callbacks. */
  ot->exec = edbm_smooth_normals_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_float(ot->srna,
                "factor",
                0.5f,
                0.0f,
                1.0f,
                "Factor",
                "Specifies weight of smooth vs original normal",
                0.0f,
                1.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Weighted Normal Modifier Face Strength
 * \{ */

static wmOperatorStatus edbm_mod_weighted_strength_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    BMFace *f;
    BMIter fiter;
    const int face_strength = RNA_enum_get(op->ptr, "face_strength");
    const bool set = RNA_boolean_get(op->ptr, "set");

    BM_select_history_clear(bm);

    const char *layer_id = MOD_WEIGHTEDNORMALS_FACEWEIGHT_CDLAYER_ID;
    int cd_prop_int_index = CustomData_get_named_layer_index(&bm->pdata, CD_PROP_INT32, layer_id);
    if (cd_prop_int_index == -1) {
      BM_data_layer_add_named(bm, &bm->pdata, CD_PROP_INT32, layer_id);
      cd_prop_int_index = CustomData_get_named_layer_index(&bm->pdata, CD_PROP_INT32, layer_id);
    }
    cd_prop_int_index -= CustomData_get_layer_index(&bm->pdata, CD_PROP_INT32);
    const int cd_prop_int_offset = CustomData_get_n_offset(
        &bm->pdata, CD_PROP_INT32, cd_prop_int_index);

    BM_mesh_elem_index_ensure(bm, BM_FACE);

    if (set) {
      BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
          int *strength = static_cast<int *>(BM_ELEM_CD_GET_VOID_P(f, cd_prop_int_offset));
          *strength = face_strength;
        }
      }
    }
    else {
      BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
        const int *strength = static_cast<int *>(BM_ELEM_CD_GET_VOID_P(f, cd_prop_int_offset));
        if (*strength == face_strength) {
          BM_face_select_set(bm, f, true);
          BM_select_history_store(bm, f);
        }
        else {
          BM_face_select_set(bm, f, false);
        }
      }
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = false;
    params.calc_normals = false;
    params.is_destructive = false;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

static const EnumPropertyItem prop_mesh_face_strength_types[] = {
    {FACE_STRENGTH_WEAK, "WEAK", 0, "Weak", ""},
    {FACE_STRENGTH_MEDIUM, "MEDIUM", 0, "Medium", ""},
    {FACE_STRENGTH_STRONG, "STRONG", 0, "Strong", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

void MESH_OT_mod_weighted_strength(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Face Normals Strength";
  ot->description = "Set/Get strength of face (used in Weighted Normal modifier)";
  ot->idname = "MESH_OT_mod_weighted_strength";

  /* API callbacks. */
  ot->exec = edbm_mod_weighted_strength_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_boolean(ot->srna, "set", false, "Set Value", "Set value of faces");

  ot->prop = RNA_def_enum(
      ot->srna,
      "face_strength",
      prop_mesh_face_strength_types,
      FACE_STRENGTH_MEDIUM,
      "Face Strength",
      "Strength to use for assigning or selecting face influence for weighted normal modifier");
}

void MESH_OT_flip_quad_tessellation(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Flip Quad Tessellation";
  ot->description = "Flips the tessellation of selected quads";
  ot->idname = "MESH_OT_flip_quad_tessellation";

  ot->exec = edbm_flip_quad_tessellation_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */
