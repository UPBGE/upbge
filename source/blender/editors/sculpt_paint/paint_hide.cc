/* SPDX-FileCopyrightText: 2010 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * Implements the bke::pbvh::Tree node hiding operator.
 */
#include "paint_hide.hh"

#include "MEM_guardedalloc.h"

#include "BLI_array_utils.hh"
#include "BLI_bit_span_ops.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_ccg.hh"
#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_subsurf.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "bmesh.hh"

#include "mesh_brush_common.hh"
#include "paint_intern.hh"
#include "sculpt_gesture.hh"
#include "sculpt_intern.hh"
#include "sculpt_islands.hh"
#include "sculpt_undo.hh"

namespace blender::ed::sculpt_paint::hide {

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

void sync_all_from_faces(Object &object)
{
  SculptSession &ss = *object.sculpt;
  Mesh &mesh = *static_cast<Mesh *>(object.data);

  islands::invalidate(ss);

  switch (bke::object::pbvh_get(object)->type()) {
    case bke::pbvh::Type::Mesh: {
      /* We may have adjusted the ".hide_poly" attribute, now make the hide status attributes for
       * vertices and edges consistent. */
      bke::mesh_hide_face_flush(mesh);
      break;
    }
    case bke::pbvh::Type::Grids: {
      /* In addition to making the hide status of the base mesh consistent, we also have to
       * propagate the status to the Multires grids. */
      bke::mesh_hide_face_flush(mesh);
      BKE_sculpt_sync_face_visibility_to_grids(mesh, *ss.subdiv_ccg);
      break;
    }
    case bke::pbvh::Type::BMesh: {
      BMesh &bm = *ss.bm;
      BMIter iter;
      BMFace *f;

      /* Hide all verts and edges attached to faces. */
      BM_ITER_MESH (f, &iter, &bm, BM_FACES_OF_MESH) {
        BMLoop *l = f->l_first;
        do {
          BM_elem_flag_enable(l->v, BM_ELEM_HIDDEN);
          BM_elem_flag_enable(l->e, BM_ELEM_HIDDEN);
        } while ((l = l->next) != f->l_first);
      }

      /* Unhide verts and edges attached to visible faces. */
      BM_ITER_MESH (f, &iter, &bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
          continue;
        }

        BMLoop *l = f->l_first;
        do {
          BM_elem_flag_disable(l->v, BM_ELEM_HIDDEN);
          BM_elem_flag_disable(l->e, BM_ELEM_HIDDEN);
        } while ((l = l->next) != f->l_first);
      }
      break;
    }
  }
}

void tag_update_visibility(const bContext &C)
{
  ARegion *region = CTX_wm_region(&C);
  ED_region_tag_redraw(region);

  Object *ob = CTX_data_active_object(&C);
  WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, ob);

  DEG_id_tag_update(&ob->id, ID_RECALC_SHADING);
  const RegionView3D *rv3d = CTX_wm_region_view3d(&C);
  if (!BKE_sculptsession_use_pbvh_draw(ob, rv3d)) {
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }
}

void mesh_show_all(const Depsgraph &depsgraph, Object &object, const IndexMask &node_mask)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);

  if (!hide_vert.is_empty()) {
    IndexMaskMemory memory;
    const IndexMask changed_nodes = IndexMask::from_predicate(
        node_mask, GrainSize(1), memory, [&](const int i) {
          const Span<int> verts = nodes[i].verts();
          return std::any_of(
              verts.begin(), verts.end(), [&](const int i) { return hide_vert[i]; });
        });
    undo::push_nodes(depsgraph, object, changed_nodes, undo::Type::HideVert);
    pbvh.tag_visibility_changed(changed_nodes);
  }

  attributes.remove(".hide_vert");
  bke::mesh_hide_vert_flush(mesh);
  pbvh.update_visibility(object);
}

void grids_show_all(Depsgraph &depsgraph, Object &object, const IndexMask &node_mask)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;
  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;

  if (!grid_hidden.is_empty()) {
    IndexMaskMemory memory;
    const IndexMask changed_nodes = IndexMask::from_predicate(
        node_mask, GrainSize(1), memory, [&](const int i) {
          const Span<int> grids = nodes[i].grids();
          return std::any_of(grids.begin(), grids.end(), [&](const int i) {
            return bits::any_bit_set(grid_hidden[i]);
          });
        });
    if (changed_nodes.is_empty()) {
      return;
    }
    undo::push_nodes(depsgraph, object, changed_nodes, undo::Type::HideVert);
    pbvh.tag_visibility_changed(changed_nodes);
  }

  BKE_subdiv_ccg_grid_hidden_free(subdiv_ccg);
  BKE_pbvh_sync_visibility_from_verts(object);
  pbvh.update_visibility(object);
  multires_mark_as_modified(&depsgraph, &object, MULTIRES_HIDDEN_MODIFIED);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Visibility Utilities
 * Functions that assist with applying changes to the different bke::pbvh::Tree types.
 * \{ */

enum class VisAction {
  Hide = 0,
  Show = 1,
};

static bool action_to_hide(const VisAction action)
{
  return action == VisAction::Hide;
}

/* Calculates whether a face should be hidden based on all of its corner vertices. */
static void calc_face_hide(const Span<int> node_faces,
                           const OffsetIndices<int> faces,
                           const Span<int> corner_verts,
                           const Span<bool> hide_vert,
                           MutableSpan<bool> hide_face)
{
  for (const int i : node_faces.index_range()) {
    Span<int> face_verts = corner_verts.slice(faces[node_faces[i]]);
    hide_face[i] = std::any_of(
        face_verts.begin(), face_verts.end(), [&](const int v) { return hide_vert[v]; });
  }
}

/* Updates a node's face's visibility based on the updated vertex visibility. */
static void flush_face_changes_node(Mesh &mesh,
                                    bke::pbvh::Tree &pbvh,
                                    const IndexMask &node_mask,
                                    const Span<bool> hide_vert)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();

  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  bke::SpanAttributeWriter<bool> hide_poly = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_poly", bke::AttrDomain::Face);

  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  Array<bool> node_changed(node_mask.min_array_size(), false);

  struct TLS {
    Vector<bool> new_hide;
  };
  threading::EnumerableThreadSpecific<TLS> all_tls;
  node_mask.foreach_index(GrainSize(1), [&](const int i) {
    TLS &tls = all_tls.local();
    const Span<int> node_faces = nodes[i].faces();

    tls.new_hide.resize(node_faces.size());
    gather_data_mesh(hide_poly.span.as_span(), node_faces, tls.new_hide.as_mutable_span());

    calc_face_hide(node_faces, faces, corner_verts, hide_vert, tls.new_hide.as_mutable_span());

    if (array_utils::indexed_data_equal<bool>(hide_poly.span, node_faces, tls.new_hide)) {
      return;
    }

    scatter_data_mesh(tls.new_hide.as_span(), node_faces, hide_poly.span);
    node_changed[i] = true;
    bke::pbvh::node_update_visibility_mesh(hide_vert, nodes[i]);
  });
  hide_poly.finish();

  IndexMaskMemory memory;
  const IndexMask changed_nodes = IndexMask::from_bools(node_changed, memory);
  if (changed_nodes.is_empty()) {
    return;
  }
  pbvh.tag_visibility_changed(node_mask);
}

/* Updates a node's face's visibility based on the updated vertex visibility. */
static void flush_face_changes(Mesh &mesh, const Span<bool> hide_vert)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();

  bke::SpanAttributeWriter<bool> hide_poly = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_poly", bke::AttrDomain::Face);

  bke::mesh_face_hide_from_vert(mesh.faces(), mesh.corner_verts(), hide_vert, hide_poly.span);
  hide_poly.finish();
}

/* Updates all of a mesh's edge visibility based on vertex visibility. */
static void flush_edge_changes(Mesh &mesh, const Span<bool> hide_vert)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();

  bke::SpanAttributeWriter<bool> hide_edge = attributes.lookup_or_add_for_write_only_span<bool>(
      ".hide_edge", bke::AttrDomain::Edge);
  bke::mesh_edge_hide_from_vert(mesh.edges(), hide_vert, hide_edge.span);
  hide_edge.finish();
}

static void vert_hide_update(const Depsgraph &depsgraph,
                             Object &object,
                             const IndexMask &node_mask,
                             const FunctionRef<void(Span<int>, MutableSpan<bool>)> calc_hide)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<bool> hide_vert = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_vert", bke::AttrDomain::Point);

  bool any_changed = false;
  threading::EnumerableThreadSpecific<Vector<bool>> all_new_hide;
  node_mask.foreach_index(GrainSize(1), [&](const int i) {
    Vector<bool> &new_hide = all_new_hide.local();
    const Span<int> verts = nodes[i].verts();

    new_hide.resize(verts.size());
    gather_data_mesh(hide_vert.span.as_span(), verts, new_hide.as_mutable_span());
    calc_hide(verts, new_hide);
    if (array_utils::indexed_data_equal<bool>(hide_vert.span, verts, new_hide)) {
      return;
    }

    any_changed = true;
    undo::push_node(depsgraph, object, &nodes[i], undo::Type::HideVert);
    scatter_data_mesh(new_hide.as_span(), verts, hide_vert.span);
  });

  hide_vert.finish();
  if (any_changed) {
    /* We handle flushing ourselves at the node level instead of delegating to
     * #bke::mesh_hide_vert_flush because we need to tag node visibility changes as well in cases
     * where the vertices hidden are on a node boundary. */
    flush_face_changes_node(mesh, pbvh, node_mask, hide_vert.span);
    flush_edge_changes(mesh, hide_vert.span);
  }
}

static void grid_hide_update(Depsgraph &depsgraph,
                             Object &object,
                             const IndexMask &node_mask,
                             const FunctionRef<void(const int, MutableBoundedBitSpan)> calc_hide)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();

  SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;
  BitGroupVector<> &grid_hidden = BKE_subdiv_ccg_grid_hidden_ensure(subdiv_ccg);

  Array<bool> node_changed(node_mask.min_array_size(), false);

  bool any_changed = false;
  node_mask.foreach_index(GrainSize(1), [&](const int i) {
    const Span<int> grids = nodes[i].grids();
    BitGroupVector<> new_hide(grids.size(), grid_hidden.group_size());
    for (const int i : grids.index_range()) {
      new_hide[i].copy_from(grid_hidden[grids[i]].as_span());
    }

    for (const int i : grids.index_range()) {
      calc_hide(grids[i], new_hide[i]);
    }

    if (std::all_of(grids.index_range().begin(), grids.index_range().end(), [&](const int i) {
          return bits::spans_equal(grid_hidden[grids[i]], new_hide[i]);
        }))
    {
      return;
    }

    any_changed = true;
    undo::push_node(depsgraph, object, &nodes[i], undo::Type::HideVert);

    for (const int i : grids.index_range()) {
      grid_hidden[grids[i]].copy_from(new_hide[i].as_span());
    }

    node_changed[i] = true;
    bke::pbvh::node_update_visibility_grids(grid_hidden, nodes[i]);
  });

  IndexMaskMemory memory;
  const IndexMask changed_nodes = IndexMask::from_bools(node_changed, memory);
  if (changed_nodes.is_empty()) {
    return;
  }
  pbvh.tag_visibility_changed(node_mask);
  multires_mark_as_modified(&depsgraph, &object, MULTIRES_HIDDEN_MODIFIED);
  BKE_pbvh_sync_visibility_from_verts(object);
}

static void partialvis_update_bmesh_verts(const Set<BMVert *, 0> &verts,
                                          const VisAction action,
                                          const FunctionRef<bool(BMVert *v)> should_update,
                                          bool *any_changed,
                                          bool *any_visible)
{
  for (BMVert *v : verts) {
    if (should_update(v)) {
      if (action == VisAction::Hide) {
        BM_elem_flag_enable(v, BM_ELEM_HIDDEN);
      }
      else {
        BM_elem_flag_disable(v, BM_ELEM_HIDDEN);
      }
      (*any_changed) = true;
    }

    if (!BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
      (*any_visible) = true;
    }
  }
}

static void partialvis_update_bmesh_faces(const Set<BMFace *, 0> &faces)
{
  for (BMFace *f : faces) {
    if (paint_is_bmesh_face_hidden(f)) {
      BM_elem_flag_enable(f, BM_ELEM_HIDDEN);
    }
    else {
      BM_elem_flag_disable(f, BM_ELEM_HIDDEN);
    }
  }
}

static void partialvis_update_bmesh_nodes(const Depsgraph &depsgraph,
                                          Object &ob,
                                          const IndexMask &node_mask,
                                          const VisAction action,
                                          const FunctionRef<bool(BMVert *v)> vert_test_fn)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();

  node_mask.foreach_index([&](const int i) {
    bool any_changed = false;
    bool any_visible = false;

    undo::push_node(depsgraph, ob, &nodes[i], undo::Type::HideVert);

    partialvis_update_bmesh_verts(BKE_pbvh_bmesh_node_unique_verts(&nodes[i]),
                                  action,
                                  vert_test_fn,
                                  &any_changed,
                                  &any_visible);

    partialvis_update_bmesh_verts(BKE_pbvh_bmesh_node_other_verts(&nodes[i]),
                                  action,
                                  vert_test_fn,
                                  &any_changed,
                                  &any_visible);

    /* Finally loop over node faces and tag the ones that are fully hidden. */
    partialvis_update_bmesh_faces(BKE_pbvh_bmesh_node_faces(&nodes[i]));

    if (any_changed) {
      BKE_pbvh_node_fully_hidden_set(nodes[i], !any_visible);
    }
  });

  pbvh.tag_visibility_changed(node_mask);
  pbvh.update_visibility(ob);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Global Visibility Operators
 * Operators that act upon the entirety of a given object's mesh.
 * \{ */

static void partialvis_all_update_mesh(const Depsgraph &depsgraph,
                                       Object &object,
                                       const VisAction action,
                                       const IndexMask &node_mask)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  if (action == VisAction::Show && !attributes.contains(".hide_vert")) {
    /* If everything is already visible, don't do anything. */
    return;
  }

  switch (action) {
    case VisAction::Hide:
      vert_hide_update(
          depsgraph, object, node_mask, [&](const Span<int> /*verts*/, MutableSpan<bool> hide) {
            hide.fill(true);
          });
      break;
    case VisAction::Show:
      mesh_show_all(depsgraph, object, node_mask);
      break;
  }
}

static void partialvis_all_update_grids(Depsgraph &depsgraph,
                                        Object &object,
                                        const VisAction action,
                                        const IndexMask &node_mask)
{
  switch (action) {
    case VisAction::Hide:
      grid_hide_update(depsgraph,
                       object,
                       node_mask,
                       [&](const int /*verts*/, MutableBoundedBitSpan hide) { hide.fill(true); });
      break;
    case VisAction::Show:
      grids_show_all(depsgraph, object, node_mask);
      break;
  }
}

static void partialvis_all_update_bmesh(const Depsgraph &depsgraph,
                                        Object &ob,
                                        const VisAction action,
                                        const IndexMask &node_mask)
{
  partialvis_update_bmesh_nodes(
      depsgraph, ob, node_mask, action, [](const BMVert * /*vert*/) { return true; });
}

static wmOperatorStatus hide_show_all_exec(bContext *C, wmOperator *op)
{
  const Scene &scene = *CTX_data_scene(C);
  Object &ob = *CTX_data_active_object(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  const VisAction action = VisAction(RNA_enum_get(op->ptr, "action"));

  bke::pbvh::Tree &pbvh = bke::object::pbvh_ensure(*depsgraph, ob);

  /* Start undo. */
  switch (action) {
    case VisAction::Hide:
      undo::push_begin_ex(scene, ob, "Hide area");
      break;
    case VisAction::Show:
      undo::push_begin_ex(scene, ob, "Show area");
      break;
  }

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      partialvis_all_update_mesh(*depsgraph, ob, action, node_mask);
      break;
    case bke::pbvh::Type::Grids:
      partialvis_all_update_grids(*depsgraph, ob, action, node_mask);
      break;
    case bke::pbvh::Type::BMesh:
      partialvis_all_update_bmesh(*depsgraph, ob, action, node_mask);
      break;
  }

  /* End undo. */
  undo::push_end(ob);

  islands::invalidate(*ob.sculpt);
  tag_update_visibility(*C);

  return OPERATOR_FINISHED;
}

static void partialvis_masked_update_mesh(const Depsgraph &depsgraph,
                                          Object &object,
                                          const VisAction action,
                                          const IndexMask &node_mask)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  if (action == VisAction::Show && !attributes.contains(".hide_vert")) {
    /* If everything is already visible, don't do anything. */
    return;
  }

  const bool value = action_to_hide(action);
  const VArraySpan<float> mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);
  if (action == VisAction::Show && mask.is_empty()) {
    mesh_show_all(depsgraph, object, node_mask);
  }
  else if (!mask.is_empty()) {
    vert_hide_update(
        depsgraph, object, node_mask, [&](const Span<int> verts, MutableSpan<bool> hide) {
          for (const int i : verts.index_range()) {
            if (mask[verts[i]] > 0.5f) {
              hide[i] = value;
            }
          }
        });
  }
}

static void partialvis_masked_update_grids(Depsgraph &depsgraph,
                                           Object &object,
                                           const VisAction action,
                                           const IndexMask &node_mask)
{
  SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;

  const bool value = action_to_hide(action);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<float> masks = subdiv_ccg.masks;
  if (masks.is_empty()) {
    grid_hide_update(depsgraph,
                     object,
                     node_mask,
                     [&](const int /*verts*/, MutableBoundedBitSpan hide) { hide.fill(value); });
  }
  else {
    grid_hide_update(
        depsgraph, object, node_mask, [&](const int grid, MutableBoundedBitSpan hide) {
          const Span<float> grid_masks = masks.slice(bke::ccg::grid_range(key, grid));
          for (const int i : grid_masks.index_range()) {
            if (grid_masks[i] > 0.5f) {
              hide[i].set(value);
            }
          }
        });
  }
}

static void partialvis_masked_update_bmesh(const Depsgraph &depsgraph,
                                           Object &ob,
                                           const VisAction action,
                                           const IndexMask &node_mask)
{
  BMesh *bm = ob.sculpt->bm;
  const int mask_offset = CustomData_get_offset_named(&bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
  const auto mask_test_fn = [&](const BMVert *v) {
    const float vmask = BM_ELEM_CD_GET_FLOAT(v, mask_offset);
    return vmask > 0.5f;
  };

  partialvis_update_bmesh_nodes(depsgraph, ob, node_mask, action, mask_test_fn);
}

static wmOperatorStatus hide_show_masked_exec(bContext *C, wmOperator *op)
{
  const Scene &scene = *CTX_data_scene(C);
  Object &ob = *CTX_data_active_object(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  const VisAction action = VisAction(RNA_enum_get(op->ptr, "action"));

  bke::pbvh::Tree &pbvh = bke::object::pbvh_ensure(*depsgraph, ob);

  /* Start undo. */
  switch (action) {
    case VisAction::Hide:
      undo::push_begin_ex(scene, ob, "Hide area");
      break;
    case VisAction::Show:
      undo::push_begin_ex(scene, ob, "Show area");
      break;
  }

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      partialvis_masked_update_mesh(*depsgraph, ob, action, node_mask);
      break;
    case bke::pbvh::Type::Grids:
      partialvis_masked_update_grids(*depsgraph, ob, action, node_mask);
      break;
    case bke::pbvh::Type::BMesh:
      partialvis_masked_update_bmesh(*depsgraph, ob, action, node_mask);
      break;
  }

  /* End undo. */
  undo::push_end(ob);

  islands::invalidate(*ob.sculpt);
  tag_update_visibility(*C);

  return OPERATOR_FINISHED;
}

static void hide_show_operator_properties(wmOperatorType *ot)
{
  static const EnumPropertyItem action_items[] = {
      {int(VisAction::Hide), "HIDE", 0, "Hide", "Hide vertices"},
      {int(VisAction::Show), "SHOW", 0, "Show", "Show vertices"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_enum(ot->srna,
               "action",
               action_items,
               int(VisAction::Hide),
               "Visibility Action",
               "Whether to hide or show vertices");
}

void PAINT_OT_hide_show_masked(wmOperatorType *ot)
{
  ot->name = "Hide/Show Masked";
  ot->idname = "PAINT_OT_hide_show_masked";
  ot->description = "Hide/show all masked vertices above a threshold";

  ot->exec = hide_show_masked_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  hide_show_operator_properties(ot);
}

void PAINT_OT_hide_show_all(wmOperatorType *ot)
{
  ot->name = "Hide/Show All";
  ot->idname = "PAINT_OT_hide_show_all";
  ot->description = "Hide/show all vertices";

  ot->exec = hide_show_all_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  hide_show_operator_properties(ot);
}

static void invert_visibility_mesh(const Depsgraph &depsgraph,
                                   Object &object,
                                   const IndexMask &node_mask)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<bool> hide_poly = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_poly", bke::AttrDomain::Face);

  undo::push_nodes(depsgraph, object, node_mask, undo::Type::HideFace);

  node_mask.foreach_index(GrainSize(1), [&](const int i) {
    for (const int face : nodes[i].faces()) {
      hide_poly.span[face] = !hide_poly.span[face];
    }
  });

  hide_poly.finish();
  bke::mesh_hide_face_flush(mesh);
  pbvh.tag_visibility_changed(node_mask);
  pbvh.update_visibility(object);
}

static void invert_visibility_grids(Depsgraph &depsgraph,
                                    Object &object,
                                    const IndexMask &node_mask)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;

  undo::push_nodes(depsgraph, object, node_mask, undo::Type::HideVert);

  BitGroupVector<> &grid_hidden = BKE_subdiv_ccg_grid_hidden_ensure(subdiv_ccg);
  node_mask.foreach_index(GrainSize(1), [&](const int i) {
    for (const int i : nodes[i].grids()) {
      bits::invert(grid_hidden[i]);
    }
    bke::pbvh::node_update_visibility_grids(grid_hidden, nodes[i]);
  });

  pbvh.tag_visibility_changed(node_mask);
  multires_mark_as_modified(&depsgraph, &object, MULTIRES_HIDDEN_MODIFIED);
  BKE_pbvh_sync_visibility_from_verts(object);
}

static void invert_visibility_bmesh(const Depsgraph &depsgraph,
                                    Object &object,
                                    const IndexMask &node_mask)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  undo::push_nodes(depsgraph, object, node_mask, undo::Type::HideVert);

  node_mask.foreach_index(GrainSize(1), [&](const int i) {
    bool fully_hidden = true;
    for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(&nodes[i])) {
      BM_elem_flag_toggle(vert, BM_ELEM_HIDDEN);
      fully_hidden &= BM_elem_flag_test_bool(vert, BM_ELEM_HIDDEN);
    }
    BKE_pbvh_node_fully_hidden_set(nodes[i], fully_hidden);
  });
  node_mask.foreach_index(GrainSize(1), [&](const int i) {
    partialvis_update_bmesh_faces(BKE_pbvh_bmesh_node_faces(&nodes[i]));
  });
  pbvh.tag_visibility_changed(node_mask);
}

static wmOperatorStatus visibility_invert_exec(bContext *C, wmOperator *op)
{
  const Scene &scene = *CTX_data_scene(C);
  Object &object = *CTX_data_active_object(C);
  Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(C);

  bke::pbvh::Tree &pbvh = bke::object::pbvh_ensure(depsgraph, object);

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
  undo::push_begin(scene, object, op);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      invert_visibility_mesh(depsgraph, object, node_mask);
      break;
    case bke::pbvh::Type::Grids:
      invert_visibility_grids(depsgraph, object, node_mask);
      break;
    case bke::pbvh::Type::BMesh:
      invert_visibility_bmesh(depsgraph, object, node_mask);
      break;
  }

  undo::push_end(object);

  islands::invalidate(*object.sculpt);
  tag_update_visibility(*C);

  return OPERATOR_FINISHED;
}

void PAINT_OT_visibility_invert(wmOperatorType *ot)
{
  ot->name = "Invert Visibility";
  ot->idname = "PAINT_OT_visibility_invert";
  ot->description = "Invert the visibility of all vertices";

  ot->exec = visibility_invert_exec;
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;
}

/* Number of vertices per iteration step size when growing or shrinking visibility. */
static constexpr float VERTEX_ITERATION_THRESHOLD = 50000.0f;

/* Extracting the loop and comparing against / writing with a constant `false` or `true` instead of
 * using #action_to_hide results in a nearly 600ms speedup on a mesh with 1.5m verts. */
template<bool value>
static void affect_visibility_mesh(const IndexRange face,
                                   const Span<int> corner_verts,
                                   const Span<bool> read_buffer,
                                   MutableSpan<bool> write_buffer)
{
  for (const int corner : face) {
    int vert = corner_verts[corner];
    if (read_buffer[vert] != value) {
      continue;
    }

    const int prev = bke::mesh::face_corner_prev(face, corner);
    const int prev_vert = corner_verts[prev];
    write_buffer[prev_vert] = value;

    const int next = bke::mesh::face_corner_next(face, corner);
    const int next_vert = corner_verts[next];
    write_buffer[next_vert] = value;
  }
}

struct DualBuffer {
  Array<bool> front;
  Array<bool> back;

  MutableSpan<bool> write_buffer(int count)
  {
    return count % 2 == 0 ? back.as_mutable_span() : front.as_mutable_span();
  }

  Span<bool> read_buffer(int count)
  {
    return count % 2 == 0 ? front.as_span() : back.as_span();
  }
};

static void propagate_vertex_visibility(Mesh &mesh,
                                        DualBuffer &buffers,
                                        const VArraySpan<bool> &hide_poly,
                                        const VisAction action,
                                        const int iterations)
{
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  for (const int i : IndexRange(iterations)) {
    Span<bool> read_buffer = buffers.read_buffer(i);
    MutableSpan<bool> write_buffer = buffers.write_buffer(i);
    threading::parallel_for(faces.index_range(), 1024, [&](const IndexRange range) {
      for (const int face_index : range) {
        if (!hide_poly[face_index]) {
          continue;
        }
        const IndexRange face = faces[face_index];
        if (action == VisAction::Hide) {
          affect_visibility_mesh<true>(face, corner_verts, read_buffer, write_buffer);
        }
        else {
          affect_visibility_mesh<false>(face, corner_verts, read_buffer, write_buffer);
        }
      }
    });

    flush_face_changes(mesh, write_buffer);
  }
}

static void update_undo_state(const Depsgraph &depsgraph,
                              Object &object,
                              const IndexMask &node_mask,
                              const Span<bool> old_hide_vert,
                              const Span<bool> new_hide_vert)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  node_mask.foreach_index(GrainSize(1), [&](const int i) {
    for (const int vert : nodes[i].verts()) {
      if (old_hide_vert[vert] != new_hide_vert[vert]) {
        undo::push_node(depsgraph, object, &nodes[i], undo::Type::HideVert);
        break;
      }
    }
  });
}

static void update_node_visibility_from_face_changes(bke::pbvh::Tree &pbvh,
                                                     const IndexMask &node_mask,
                                                     const Span<bool> orig_hide_poly,
                                                     const Span<bool> new_hide_poly,
                                                     const Span<bool> hide_vert)
{
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  Array<bool> node_changed(node_mask.min_array_size(), false);

  node_mask.foreach_index(GrainSize(1), [&](const int i) {
    bool any_changed = false;
    const Span<int> indices = nodes[i].faces();
    for (const int face_index : indices) {
      if (orig_hide_poly[face_index] != new_hide_poly[face_index]) {
        any_changed = true;
        break;
      }
    }

    if (any_changed) {
      node_changed[i] = true;
      bke::pbvh::node_update_visibility_mesh(hide_vert, nodes[i]);
    }
  });

  IndexMaskMemory memory;
  const IndexMask changed_nodes = IndexMask::from_bools(node_changed, memory);
  if (pbvh.draw_data) {
    /* Only tag draw data. Nodes have already been updated above. */
    pbvh.draw_data->tag_visibility_changed(changed_nodes);
  }
}

static void grow_shrink_visibility_mesh(const Depsgraph &depsgraph,
                                        Object &object,
                                        const IndexMask &node_mask,
                                        const VisAction action,
                                        const int iterations)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  if (!attributes.contains(".hide_vert")) {
    /* If the entire mesh is visible, we can neither grow nor shrink the boundary. */
    return;
  }

  bke::SpanAttributeWriter<bool> hide_vert = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_vert", bke::AttrDomain::Point);
  const VArraySpan hide_poly = *attributes.lookup_or_default<bool>(
      ".hide_poly", bke::AttrDomain::Face, false);

  DualBuffer buffers;
  buffers.back.reinitialize(hide_vert.span.size());
  buffers.front.reinitialize(hide_vert.span.size());
  array_utils::copy(hide_vert.span.as_span(), buffers.back.as_mutable_span());
  array_utils::copy(hide_vert.span.as_span(), buffers.front.as_mutable_span());

  Array<bool> orig_hide_poly(hide_poly);
  propagate_vertex_visibility(mesh, buffers, hide_poly, action, iterations);

  const Span<bool> last_buffer = buffers.write_buffer(iterations - 1);

  update_undo_state(depsgraph, object, node_mask, hide_vert.span, last_buffer);

  /* We can wait until after all iterations are done to flush edge changes as they are
   * not used for coarse filtering while iterating. */
  flush_edge_changes(mesh, last_buffer);

  update_node_visibility_from_face_changes(
      *bke::object::pbvh_get(object), node_mask, orig_hide_poly, hide_poly, last_buffer);
  array_utils::copy(last_buffer, hide_vert.span);
  hide_vert.finish();
}

struct DualBitBuffer {
  BitGroupVector<> front;
  BitGroupVector<> back;

  BitGroupVector<> &write_buffer(int count)
  {
    return count % 2 == 0 ? back : front;
  }

  BitGroupVector<> &read_buffer(int count)
  {
    return count % 2 == 0 ? front : back;
  }
};

static void grow_shrink_visibility_grid(Depsgraph &depsgraph,
                                        Object &object,
                                        const IndexMask &node_mask,
                                        const VisAction action,
                                        const int iterations)
{
  SculptSession &ss = *object.sculpt;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();

  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  BitGroupVector<> &grid_hidden = BKE_subdiv_ccg_grid_hidden_ensure(subdiv_ccg);

  const bool desired_state = action_to_hide(action);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

  DualBitBuffer buffers;
  buffers.front = grid_hidden;
  buffers.back = grid_hidden;

  Array<bool> node_changed(nodes.size(), false);

  for (const int i : IndexRange(iterations)) {
    BitGroupVector<> &read_buffer = buffers.read_buffer(i);
    BitGroupVector<> &write_buffer = buffers.write_buffer(i);

    node_mask.foreach_index(GrainSize(1), [&](const int i) {
      for (const int grid : nodes[i].grids()) {
        for (const int y : IndexRange(key.grid_size)) {
          for (const int x : IndexRange(key.grid_size)) {
            const int grid_elem_idx = CCG_grid_xy_to_index(key.grid_size, x, y);
            if (read_buffer[grid][grid_elem_idx] != desired_state) {
              continue;
            }

            SubdivCCGCoord coord{};
            coord.grid_index = grid;
            coord.x = x;
            coord.y = y;

            SubdivCCGNeighbors neighbors;
            BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, coord, true, neighbors);

            for (const SubdivCCGCoord neighbor : neighbors.coords) {
              const int neighbor_grid_elem_idx = CCG_grid_xy_to_index(
                  key.grid_size, neighbor.x, neighbor.y);

              write_buffer[neighbor.grid_index][neighbor_grid_elem_idx].set(desired_state);
            }
          }
        }
      }

      node_changed[i] = true;
    });
  }

  IndexMaskMemory memory;
  const IndexMask changed_nodes = IndexMask::from_bools(node_changed, memory);

  undo::push_nodes(depsgraph, object, changed_nodes, undo::Type::HideVert);

  BitGroupVector<> &last_buffer = buffers.write_buffer(iterations - 1);
  grid_hidden = std::move(last_buffer);

  pbvh.tag_visibility_changed(node_mask);
  pbvh.update_visibility(object);

  multires_mark_as_modified(&depsgraph, &object, MULTIRES_HIDDEN_MODIFIED);
  BKE_pbvh_sync_visibility_from_verts(object);
}

static Array<bool> duplicate_visibility_bmesh(const Object &object)
{
  const SculptSession &ss = *object.sculpt;
  BMesh &bm = *ss.bm;
  Array<bool> result(bm.totvert);
  vert_random_access_ensure(const_cast<Object &>(object));
  for (const int i : result.index_range()) {
    result[i] = BM_elem_flag_test_bool(BM_vert_at_index(&bm, i), BM_ELEM_HIDDEN);
  }
  return result;
}

static void grow_shrink_visibility_bmesh(const Depsgraph &depsgraph,
                                         Object &object,
                                         const IndexMask &node_mask,
                                         const VisAction action,
                                         const int iterations)
{
  for (const int i : IndexRange(iterations)) {
    UNUSED_VARS(i);
    const Array<bool> prev_visibility = duplicate_visibility_bmesh(object);
    partialvis_update_bmesh_nodes(depsgraph, object, node_mask, action, [&](BMVert *vert) {
      BMeshNeighborVerts neighbors;
      for (BMVert *neighbor : vert_neighbors_get_bmesh(*vert, neighbors)) {
        if (prev_visibility[BM_elem_index_get(neighbor)] == action_to_hide(action)) {
          return true;
        }
      }
      return false;
    });
  }
}

static wmOperatorStatus visibility_filter_exec(bContext *C, wmOperator *op)
{
  const Scene &scene = *CTX_data_scene(C);
  Object &object = *CTX_data_active_object(C);
  Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(C);

  bke::pbvh::Tree &pbvh = bke::object::pbvh_ensure(depsgraph, object);

  const VisAction mode = VisAction(RNA_enum_get(op->ptr, "action"));

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  int num_verts = SCULPT_vertex_count_get(object);

  int iterations = RNA_int_get(op->ptr, "iterations");

  if (RNA_boolean_get(op->ptr, "auto_iteration_count")) {
    /* Automatically adjust the number of iterations based on the number
     * of vertices in the mesh. */
    iterations = int(num_verts / VERTEX_ITERATION_THRESHOLD) + 1;
  }

  undo::push_begin(scene, object, op);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      grow_shrink_visibility_mesh(depsgraph, object, node_mask, mode, iterations);
      break;
    case bke::pbvh::Type::Grids:
      grow_shrink_visibility_grid(depsgraph, object, node_mask, mode, iterations);
      break;
    case bke::pbvh::Type::BMesh:
      grow_shrink_visibility_bmesh(depsgraph, object, node_mask, mode, iterations);
      break;
  }
  undo::push_end(object);

  islands::invalidate(*object.sculpt);
  tag_update_visibility(*C);

  return OPERATOR_FINISHED;
}

void PAINT_OT_visibility_filter(wmOperatorType *ot)
{
  static EnumPropertyItem actions[] = {
      {int(VisAction::Show),
       "GROW",
       0,
       "Grow Visibility",
       "Grow the visibility by one face based on mesh topology"},
      {int(VisAction::Hide),
       "SHRINK",
       0,
       "Shrink Visibility",
       "Shrink the visibility by one face based on mesh topology"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Visibility Filter";
  ot->idname = "PAINT_OT_visibility_filter";
  ot->description = "Edit the visibility of the current mesh";

  ot->exec = visibility_filter_exec;
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "action", actions, int(VisAction::Show), "Action", "");

  RNA_def_int(ot->srna,
              "iterations",
              1,
              1,
              100,
              "Iterations",
              "Number of times that the filter is going to be applied",
              1,
              100);
  RNA_def_boolean(
      ot->srna,
      "auto_iteration_count",
      true,
      "Auto Iteration Count",
      "Use an automatic number of iterations based on the number of vertices of the sculpt");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gesture-based Visibility Operators
 * Operators that act upon a user-selected area.
 * \{ */

struct HideShowOperation {
  gesture::Operation op;

  VisAction action;
};

static void partialvis_gesture_update_mesh(gesture::GestureData &gesture_data)
{
  HideShowOperation *operation = reinterpret_cast<HideShowOperation *>(gesture_data.operation);
  Object *object = gesture_data.vc.obact;
  const Depsgraph &depsgraph = *gesture_data.vc.depsgraph;
  const VisAction action = operation->action;
  const IndexMask &node_mask = gesture_data.node_mask;

  Mesh *mesh = static_cast<Mesh *>(object->data);
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  if (action == VisAction::Show && !attributes.contains(".hide_vert")) {
    /* If everything is already visible, don't do anything. */
    return;
  }

  const bool value = action_to_hide(action);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, *object);
  const Span<float3> normals = bke::pbvh::vert_normals_eval(depsgraph, *object);
  vert_hide_update(
      depsgraph, *object, node_mask, [&](const Span<int> verts, MutableSpan<bool> hide) {
        for (const int i : verts.index_range()) {
          if (gesture::is_affected(gesture_data, positions[verts[i]], normals[verts[i]])) {
            hide[i] = value;
          }
        }
      });
}

static void partialvis_gesture_update_grids(Depsgraph &depsgraph,
                                            gesture::GestureData &gesture_data)
{
  HideShowOperation *operation = reinterpret_cast<HideShowOperation *>(gesture_data.operation);
  Object *object = gesture_data.vc.obact;
  const VisAction action = operation->action;
  const IndexMask &node_mask = gesture_data.node_mask;

  SubdivCCG &subdiv_ccg = *object->sculpt->subdiv_ccg;

  const bool value = action_to_hide(action);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<float3> positions = subdiv_ccg.positions;
  const Span<float3> normals = subdiv_ccg.normals;
  grid_hide_update(depsgraph, *object, node_mask, [&](const int grid, MutableBoundedBitSpan hide) {
    const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
    const Span<float3> grid_normals = normals.slice(bke::ccg::grid_range(key, grid));
    for (const int i : grid_positions.index_range()) {
      if (gesture::is_affected(gesture_data, grid_positions[i], grid_normals[i])) {
        hide[i].set(value);
      }
    }
  });
}

static void partialvis_gesture_update_bmesh(gesture::GestureData &gesture_data)
{
  const auto selection_test_fn = [&](const BMVert *v) {
    return gesture::is_affected(gesture_data, v->co, v->no);
  };

  HideShowOperation *operation = reinterpret_cast<HideShowOperation *>(gesture_data.operation);

  partialvis_update_bmesh_nodes(*gesture_data.vc.depsgraph,
                                *gesture_data.vc.obact,
                                gesture_data.node_mask,
                                operation->action,
                                selection_test_fn);
}

static void hide_show_begin(bContext &C, wmOperator &op, gesture::GestureData & /*gesture_data*/)
{
  const Scene &scene = *CTX_data_scene(&C);
  Object *ob = CTX_data_active_object(&C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);

  undo::push_begin(scene, *ob, &op);
  bke::object::pbvh_ensure(*depsgraph, *ob);
}

static void hide_show_apply_for_symmetry_pass(bContext &C, gesture::GestureData &gesture_data)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);

  switch (bke::object::pbvh_get(*gesture_data.vc.obact)->type()) {
    case bke::pbvh::Type::Mesh:
      partialvis_gesture_update_mesh(gesture_data);
      break;
    case bke::pbvh::Type::Grids:
      partialvis_gesture_update_grids(*depsgraph, gesture_data);
      break;
    case bke::pbvh::Type::BMesh:
      partialvis_gesture_update_bmesh(gesture_data);
      break;
  }
}
static void hide_show_end(bContext &C, gesture::GestureData &gesture_data)
{
  islands::invalidate(*gesture_data.vc.obact->sculpt);
  tag_update_visibility(C);
  undo::push_end(*gesture_data.vc.obact);
}

static void hide_show_init_properties(bContext & /*C*/,
                                      gesture::GestureData &gesture_data,
                                      wmOperator &op)
{
  gesture_data.operation = reinterpret_cast<gesture::Operation *>(
      MEM_callocN<HideShowOperation>(__func__));

  HideShowOperation *operation = reinterpret_cast<HideShowOperation *>(gesture_data.operation);

  operation->op.begin = hide_show_begin;
  operation->op.apply_for_symmetry_pass = hide_show_apply_for_symmetry_pass;
  operation->op.end = hide_show_end;

  operation->action = VisAction(RNA_enum_get(op.ptr, "action"));
  gesture_data.selection_type = gesture::SelectionType(RNA_enum_get(op.ptr, "area"));
}

static wmOperatorStatus hide_show_gesture_box_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_box(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  hide_show_init_properties(*C, *gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus hide_show_gesture_lasso_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_lasso(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  hide_show_init_properties(*C, *gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus hide_show_gesture_line_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_line(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  hide_show_init_properties(*C, *gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus hide_show_gesture_polyline_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_polyline(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  hide_show_init_properties(*C, *gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static void hide_show_operator_gesture_properties(wmOperatorType *ot)
{
  static const EnumPropertyItem area_items[] = {
      {int(gesture::SelectionType::Outside),
       "OUTSIDE",
       0,
       "Outside",
       "Hide or show vertices outside the selection"},
      {int(gesture::SelectionType::Inside),
       "Inside",
       0,
       "Inside",
       "Hide or show vertices inside the selection"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_enum(ot->srna,
               "area",
               area_items,
               int(gesture::SelectionType::Inside),
               "Visibility Area",
               "Which vertices to hide or show");
}

void PAINT_OT_hide_show(wmOperatorType *ot)
{
  ot->name = "Hide/Show";
  ot->idname = "PAINT_OT_hide_show";
  ot->description = "Hide/show some vertices";

  ot->invoke = WM_gesture_box_invoke;
  ot->modal = WM_gesture_box_modal;
  ot->exec = hide_show_gesture_box_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_border(ot);
  hide_show_operator_properties(ot);
  hide_show_operator_gesture_properties(ot);
  gesture::operator_properties(ot, gesture::ShapeType::Box);
}

void PAINT_OT_hide_show_lasso_gesture(wmOperatorType *ot)
{
  ot->name = "Hide/Show Lasso";
  ot->idname = "PAINT_OT_hide_show_lasso_gesture";
  ot->description = "Hide/show some vertices";

  ot->invoke = WM_gesture_lasso_invoke;
  ot->modal = WM_gesture_lasso_modal;
  ot->exec = hide_show_gesture_lasso_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER | OPTYPE_DEPENDS_ON_CURSOR;

  WM_operator_properties_gesture_lasso(ot);
  hide_show_operator_properties(ot);
  hide_show_operator_gesture_properties(ot);
  gesture::operator_properties(ot, gesture::ShapeType::Lasso);
}

void PAINT_OT_hide_show_line_gesture(wmOperatorType *ot)
{
  ot->name = "Hide/Show Line";
  ot->idname = "PAINT_OT_hide_show_line_gesture";
  ot->description = "Hide/show some vertices";

  ot->invoke = WM_gesture_straightline_active_side_invoke;
  ot->modal = WM_gesture_straightline_oneshot_modal;
  ot->exec = hide_show_gesture_line_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
  hide_show_operator_properties(ot);
  hide_show_operator_gesture_properties(ot);
  gesture::operator_properties(ot, gesture::ShapeType::Line);
}

void PAINT_OT_hide_show_polyline_gesture(wmOperatorType *ot)
{
  ot->name = "Hide/Show Polyline";
  ot->idname = "PAINT_OT_hide_show_polyline_gesture";
  ot->description = "Hide/show some vertices";

  ot->invoke = WM_gesture_polyline_invoke;
  ot->modal = WM_gesture_polyline_modal;
  ot->exec = hide_show_gesture_polyline_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER | OPTYPE_DEPENDS_ON_CURSOR;

  WM_operator_properties_gesture_polyline(ot);
  hide_show_operator_properties(ot);
  hide_show_operator_gesture_properties(ot);
  gesture::operator_properties(ot, gesture::ShapeType::Lasso);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::hide
