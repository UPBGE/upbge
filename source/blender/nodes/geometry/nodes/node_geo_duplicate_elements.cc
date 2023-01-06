/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_map.hh"
#include "BLI_noise.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_pointcloud_types.h"

#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"
#include "BKE_instances.hh"
#include "BKE_mesh.h"
#include "BKE_pointcloud.h"

#include "node_geometry_util.hh"

#include "UI_interface.h"
#include "UI_resources.h"

namespace blender::nodes::node_geo_duplicate_elements_cc {

NODE_STORAGE_FUNCS(NodeGeometryDuplicateElements);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Geometry"));
  b.add_input<decl::Bool>(N_("Selection")).hide_value().default_value(true).field_on_all();
  b.add_input<decl::Int>(N_("Amount"))
      .min(0)
      .default_value(1)
      .field_on_all()
      .description(N_("The number of duplicates to create for each element"));

  b.add_output<decl::Geometry>(N_("Geometry"))
      .propagate_all()
      .description(N_("The duplicated geometry, not including the original geometry"));
  b.add_output<decl::Int>(N_("Duplicate Index"))
      .field_on_all()
      .description(N_("The indices of the duplicates for each element"));
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryDuplicateElements *data = MEM_cnew<NodeGeometryDuplicateElements>(__func__);
  data->domain = ATTR_DOMAIN_POINT;
  node->storage = data;
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "domain", 0, "", ICON_NONE);
}

struct IndexAttributes {
  AutoAnonymousAttributeID duplicate_index;
};

/* -------------------------------------------------------------------- */
/** \name Utility Functions
 * \{ */

static Map<AttributeIDRef, AttributeKind> gather_attributes_without_id(
    const GeometrySet &geometry_set,
    const GeometryComponentType component_type,
    const AnonymousAttributePropagationInfo &propagation_info)
{
  Map<AttributeIDRef, AttributeKind> attributes;
  geometry_set.gather_attributes_for_propagation(
      {component_type}, component_type, false, propagation_info, attributes);
  attributes.remove("id");
  return attributes;
};

static IndexRange range_for_offsets_index(const Span<int> offsets, const int index)
{
  return {offsets[index], offsets[index + 1] - offsets[index]};
}

static Array<int> accumulate_counts_to_offsets(const IndexMask selection,
                                               const VArray<int> &counts)
{
  Array<int> offsets(selection.size() + 1);
  int total = 0;
  for (const int i : selection.index_range()) {
    offsets[i] = total;
    total += std::max(counts[selection[i]], 0);
  }
  offsets.last() = total;
  return offsets;
}

/* Utility functions for threaded copying of attribute data where possible. */
template<typename T>
static void threaded_slice_fill(Span<int> offsets,
                                const IndexMask selection,
                                Span<T> src,
                                MutableSpan<T> dst)
{
  BLI_assert(offsets.last() == dst.size());
  BLI_assert(selection.size() == offsets.size() - 1);
  threading::parallel_for(IndexRange(offsets.size() - 1), 512, [&](IndexRange range) {
    for (const int i : range) {
      dst.slice(range_for_offsets_index(offsets, i)).fill(src[selection[i]]);
    }
  });
}

static void copy_hashed_ids(const Span<int> src, const int hash, MutableSpan<int> dst)
{
  for (const int i : src.index_range()) {
    dst[i] = noise::hash(src[i], hash);
  }
}

static void threaded_id_offset_copy(const Span<int> offsets,
                                    const Span<int> src,
                                    MutableSpan<int> dst)
{
  BLI_assert(offsets.last() == dst.size());
  threading::parallel_for(IndexRange(offsets.size() - 1), 512, [&](IndexRange range) {
    for (const int i : range) {
      dst[offsets[i]] = src[i];
      const int count = offsets[i + 1] - offsets[i];
      if (count == 0) {
        continue;
      }
      for (const int i_duplicate : IndexRange(1, count - 1)) {
        dst[offsets[i] + i_duplicate] = noise::hash(src[i], i_duplicate);
      }
    }
  });
}

/** Create the copy indices for the duplication domain. */
static void create_duplicate_index_attribute(bke::MutableAttributeAccessor attributes,
                                             const eAttrDomain output_domain,
                                             const IndexMask selection,
                                             const IndexAttributes &attribute_outputs,
                                             const Span<int> offsets)
{
  SpanAttributeWriter<int> duplicate_indices = attributes.lookup_or_add_for_write_only_span<int>(
      attribute_outputs.duplicate_index.get(), output_domain);
  for (const int i : IndexRange(selection.size())) {
    const IndexRange range = range_for_offsets_index(offsets, i);
    MutableSpan<int> indices = duplicate_indices.span.slice(range);
    for (const int i : indices.index_range()) {
      indices[i] = i;
    }
  }
  duplicate_indices.finish();
}

/**
 * Copy the stable ids to the first duplicate and create new ids based on a hash of the original id
 * and the duplicate number. This function is used for the point domain elements.
 */
static void copy_stable_id_point(const Span<int> offsets,
                                 const bke::AttributeAccessor src_attributes,
                                 bke::MutableAttributeAccessor dst_attributes)
{
  GAttributeReader src_attribute = src_attributes.lookup("id");
  if (!src_attribute) {
    return;
  }
  GSpanAttributeWriter dst_attribute = dst_attributes.lookup_or_add_for_write_only_span(
      "id", ATTR_DOMAIN_POINT, CD_PROP_INT32);
  if (!dst_attribute) {
    return;
  }

  VArraySpan<int> src{src_attribute.varray.typed<int>()};
  MutableSpan<int> dst = dst_attribute.span.typed<int>();
  threaded_id_offset_copy(offsets, src, dst);
  dst_attribute.finish();
}

static void copy_attributes_without_id(GeometrySet &geometry_set,
                                       const GeometryComponentType component_type,
                                       const eAttrDomain domain,
                                       const Span<int> offsets,
                                       const IndexMask selection,
                                       const AnonymousAttributePropagationInfo &propagation_info,
                                       const bke::AttributeAccessor src_attributes,
                                       bke::MutableAttributeAccessor dst_attributes)
{
  const Map<AttributeIDRef, AttributeKind> attributes = gather_attributes_without_id(
      geometry_set, component_type, propagation_info);

  for (const Map<AttributeIDRef, AttributeKind>::Item entry : attributes.items()) {
    const AttributeIDRef attribute_id = entry.key;
    GAttributeReader src_attribute = src_attributes.lookup(attribute_id);
    if (!src_attribute || src_attribute.domain != domain) {
      continue;
    }
    eAttrDomain out_domain = src_attribute.domain;
    const eCustomDataType data_type = bke::cpp_type_to_custom_data_type(
        src_attribute.varray.type());
    GSpanAttributeWriter dst_attribute = dst_attributes.lookup_or_add_for_write_only_span(
        attribute_id, out_domain, data_type);
    if (!dst_attribute) {
      continue;
    }
    attribute_math::convert_to_static_type(data_type, [&](auto dummy) {
      using T = decltype(dummy);
      VArraySpan<T> src = src_attribute.varray.typed<T>();
      MutableSpan<T> dst = dst_attribute.span.typed<T>();
      threaded_slice_fill<T>(offsets, selection, src, dst);
    });
    dst_attribute.finish();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Curves
 * \{ */

/**
 * Copies the attributes for curve duplicates. If copying the curve domain, the attributes are
 * copied with an offset fill, otherwise a mapping is used.
 */
static void copy_curve_attributes_without_id(
    const GeometrySet &geometry_set,
    const bke::CurvesGeometry &src_curves,
    const IndexMask selection,
    const Span<int> curve_offsets,
    const AnonymousAttributePropagationInfo &propagation_info,
    bke::CurvesGeometry &dst_curves)
{
  Map<AttributeIDRef, AttributeKind> attributes = gather_attributes_without_id(
      geometry_set, GEO_COMPONENT_TYPE_CURVE, propagation_info);

  for (const Map<AttributeIDRef, AttributeKind>::Item entry : attributes.items()) {
    const AttributeIDRef attribute_id = entry.key;
    GAttributeReader src_attribute = src_curves.attributes().lookup(attribute_id);
    if (!src_attribute) {
      continue;
    }

    eAttrDomain out_domain = src_attribute.domain;
    const eCustomDataType data_type = bke::cpp_type_to_custom_data_type(
        src_attribute.varray.type());
    GSpanAttributeWriter dst_attribute =
        dst_curves.attributes_for_write().lookup_or_add_for_write_only_span(
            attribute_id, out_domain, data_type);
    if (!dst_attribute) {
      continue;
    }

    attribute_math::convert_to_static_type(data_type, [&](auto dummy) {
      using T = decltype(dummy);
      VArraySpan<T> src{src_attribute.varray.typed<T>()};
      MutableSpan<T> dst = dst_attribute.span.typed<T>();

      switch (out_domain) {
        case ATTR_DOMAIN_CURVE:
          threaded_slice_fill<T>(curve_offsets, selection, src, dst);
          break;
        case ATTR_DOMAIN_POINT:
          threading::parallel_for(selection.index_range(), 512, [&](IndexRange range) {
            for (const int i_selection : range) {
              const int i_src_curve = selection[i_selection];
              const Span<T> curve_src = src.slice(src_curves.points_for_curve(i_src_curve));
              for (const int i_dst_curve : range_for_offsets_index(curve_offsets, i_selection)) {
                dst.slice(dst_curves.points_for_curve(i_dst_curve)).copy_from(curve_src);
              }
            }
          });
          break;
        default:
          break;
      }
    });
    dst_attribute.finish();
  }
}

/**
 * Copy the stable ids to the first duplicate and create new ids based on a hash of the original id
 * and the duplicate number. In the curve case, copy the entire curve's points to the
 * destination,
 * then loop over the remaining ones point by point, hashing their ids to the new ids.
 */
static void copy_stable_id_curves(const bke::CurvesGeometry &src_curves,
                                  const IndexMask selection,
                                  const Span<int> curve_offsets,
                                  bke::CurvesGeometry &dst_curves)
{
  GAttributeReader src_attribute = src_curves.attributes().lookup("id");
  if (!src_attribute) {
    return;
  }
  GSpanAttributeWriter dst_attribute =
      dst_curves.attributes_for_write().lookup_or_add_for_write_only_span(
          "id", ATTR_DOMAIN_POINT, CD_PROP_INT32);
  if (!dst_attribute) {
    return;
  }

  VArraySpan<int> src{src_attribute.varray.typed<int>()};
  MutableSpan<int> dst = dst_attribute.span.typed<int>();

  threading::parallel_for(selection.index_range(), 512, [&](IndexRange range) {
    for (const int i_selection : range) {
      const int i_src_curve = selection[i_selection];
      const Span<int> curve_src = src.slice(src_curves.points_for_curve(i_src_curve));
      const IndexRange duplicates_range = range_for_offsets_index(curve_offsets, i_selection);
      for (const int i_duplicate : IndexRange(duplicates_range.size()).drop_front(1)) {
        const int i_dst_curve = duplicates_range[i_duplicate];
        copy_hashed_ids(
            curve_src, i_duplicate, dst.slice(dst_curves.points_for_curve(i_dst_curve)));
      }
    }
  });
  dst_attribute.finish();
}

static void duplicate_curves(GeometrySet &geometry_set,
                             const Field<int> &count_field,
                             const Field<bool> &selection_field,
                             const IndexAttributes &attribute_outputs,
                             const AnonymousAttributePropagationInfo &propagation_info)
{
  if (!geometry_set.has_curves()) {
    geometry_set.remove_geometry_during_modify();
    return;
  }
  geometry_set.keep_only_during_modify({GEO_COMPONENT_TYPE_CURVE});
  GeometryComponentEditData::remember_deformed_curve_positions_if_necessary(geometry_set);

  const Curves &curves_id = *geometry_set.get_curves_for_read();
  const bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id.geometry);

  bke::CurvesFieldContext field_context{curves, ATTR_DOMAIN_CURVE};
  FieldEvaluator evaluator{field_context, curves.curves_num()};
  evaluator.add(count_field);
  evaluator.set_selection(selection_field);
  evaluator.evaluate();
  const VArray<int> counts = evaluator.get_evaluated<int>(0);
  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();

  /* The offset in the result curve domain at every selected input curve. */
  Array<int> curve_offsets(selection.size() + 1);
  Array<int> point_offsets(selection.size() + 1);

  int dst_curves_num = 0;
  int dst_points_num = 0;
  for (const int i_curve : selection.index_range()) {
    const int count = std::max(counts[selection[i_curve]], 0);
    curve_offsets[i_curve] = dst_curves_num;
    point_offsets[i_curve] = dst_points_num;
    dst_curves_num += count;
    dst_points_num += count * curves.points_for_curve(selection[i_curve]).size();
  }
  curve_offsets.last() = dst_curves_num;
  point_offsets.last() = dst_points_num;

  Curves *new_curves_id = bke::curves_new_nomain(dst_points_num, dst_curves_num);
  bke::curves_copy_parameters(curves_id, *new_curves_id);
  bke::CurvesGeometry &new_curves = bke::CurvesGeometry::wrap(new_curves_id->geometry);
  MutableSpan<int> all_dst_offsets = new_curves.offsets_for_write();

  threading::parallel_for(selection.index_range(), 512, [&](IndexRange range) {
    for (const int i_selection : range) {
      const int i_src_curve = selection[i_selection];
      const IndexRange src_curve_range = curves.points_for_curve(i_src_curve);
      const IndexRange dst_curves_range = range_for_offsets_index(curve_offsets, i_selection);
      MutableSpan<int> dst_offsets = all_dst_offsets.slice(dst_curves_range);
      for (const int i_duplicate : IndexRange(dst_curves_range.size())) {
        dst_offsets[i_duplicate] = point_offsets[i_selection] +
                                   src_curve_range.size() * i_duplicate;
      }
    }
  });
  all_dst_offsets.last() = dst_points_num;

  copy_curve_attributes_without_id(
      geometry_set, curves, selection, curve_offsets, propagation_info, new_curves);

  copy_stable_id_curves(curves, selection, curve_offsets, new_curves);

  if (attribute_outputs.duplicate_index) {
    create_duplicate_index_attribute(new_curves.attributes_for_write(),
                                     ATTR_DOMAIN_CURVE,
                                     selection,
                                     attribute_outputs,
                                     curve_offsets);
  }

  geometry_set.replace_curves(new_curves_id);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Faces
 * \{ */

/**
 * Copies the attributes for face duplicates. If copying the face domain, the attributes are
 * copied with an offset fill, otherwise a mapping is used.
 */
static void copy_face_attributes_without_id(
    GeometrySet &geometry_set,
    const Span<int> edge_mapping,
    const Span<int> vert_mapping,
    const Span<int> loop_mapping,
    const Span<int> offsets,
    const IndexMask selection,
    const AnonymousAttributePropagationInfo &propagation_info,
    const bke::AttributeAccessor src_attributes,
    bke::MutableAttributeAccessor dst_attributes)
{
  Map<AttributeIDRef, AttributeKind> attributes = gather_attributes_without_id(
      geometry_set, GEO_COMPONENT_TYPE_MESH, propagation_info);

  for (const Map<AttributeIDRef, AttributeKind>::Item entry : attributes.items()) {
    const AttributeIDRef attribute_id = entry.key;
    GAttributeReader src_attribute = src_attributes.lookup(attribute_id);
    if (!src_attribute) {
      continue;
    }

    eAttrDomain out_domain = src_attribute.domain;
    const eCustomDataType data_type = bke::cpp_type_to_custom_data_type(
        src_attribute.varray.type());
    GSpanAttributeWriter dst_attribute = dst_attributes.lookup_or_add_for_write_only_span(
        attribute_id, out_domain, data_type);
    if (!dst_attribute) {
      continue;
    }

    attribute_math::convert_to_static_type(data_type, [&](auto dummy) {
      using T = decltype(dummy);
      VArraySpan<T> src{src_attribute.varray.typed<T>()};
      MutableSpan<T> dst = dst_attribute.span.typed<T>();

      switch (out_domain) {
        case ATTR_DOMAIN_POINT:
          array_utils::gather(src, vert_mapping, dst);
          break;
        case ATTR_DOMAIN_EDGE:
          array_utils::gather(src, edge_mapping, dst);
          break;
        case ATTR_DOMAIN_FACE:
          threaded_slice_fill<T>(offsets, selection, src, dst);
          break;
        case ATTR_DOMAIN_CORNER:
          array_utils::gather(src, loop_mapping, dst);
          break;
        default:
          break;
      }
    });
    dst_attribute.finish();
  }
}

/**
 * Copy the stable ids to the first duplicate and create new ids based on a hash of the original id
 * and the duplicate number. This function is used for points when duplicating the face domain.
 *
 * This function could be threaded in the future, but since it is only 1 attribute and the
 * `face->edge->vert` mapping would mean creating a 1/1 mapping to allow for it, is it worth it?
 */
static void copy_stable_id_faces(const Mesh &mesh,
                                 const IndexMask selection,
                                 const Span<int> poly_offsets,
                                 const Span<int> vert_mapping,
                                 const bke::AttributeAccessor src_attributes,
                                 bke::MutableAttributeAccessor dst_attributes)
{
  GAttributeReader src_attribute = src_attributes.lookup("id");
  if (!src_attribute) {
    return;
  }
  GSpanAttributeWriter dst_attribute = dst_attributes.lookup_or_add_for_write_only_span(
      "id", ATTR_DOMAIN_POINT, CD_PROP_INT32);
  if (!dst_attribute) {
    return;
  }

  VArraySpan<int> src{src_attribute.varray.typed<int>()};
  MutableSpan<int> dst = dst_attribute.span.typed<int>();

  const Span<MPoly> polys = mesh.polys();
  int loop_index = 0;
  for (const int i_poly : selection.index_range()) {
    const IndexRange range = range_for_offsets_index(poly_offsets, i_poly);
    if (range.size() == 0) {
      continue;
    }
    const MPoly &source = polys[i_poly];
    for ([[maybe_unused]] const int i_duplicate : IndexRange(range.size())) {
      for ([[maybe_unused]] const int i_loops : IndexRange(source.totloop)) {
        if (i_duplicate == 0) {
          dst[loop_index] = src[vert_mapping[loop_index]];
        }
        else {
          dst[loop_index] = noise::hash(src[vert_mapping[loop_index]], i_duplicate);
        }
        loop_index++;
      }
    }
  }

  dst_attribute.finish();
}

static void duplicate_faces(GeometrySet &geometry_set,
                            const Field<int> &count_field,
                            const Field<bool> &selection_field,
                            const IndexAttributes &attribute_outputs,
                            const AnonymousAttributePropagationInfo &propagation_info)
{
  if (!geometry_set.has_mesh()) {
    geometry_set.remove_geometry_during_modify();
    return;
  }
  geometry_set.keep_only_during_modify({GEO_COMPONENT_TYPE_MESH});

  const Mesh &mesh = *geometry_set.get_mesh_for_read();
  const Span<MVert> verts = mesh.verts();
  const Span<MEdge> edges = mesh.edges();
  const Span<MPoly> polys = mesh.polys();
  const Span<MLoop> loops = mesh.loops();

  bke::MeshFieldContext field_context{mesh, ATTR_DOMAIN_FACE};
  FieldEvaluator evaluator(field_context, polys.size());
  evaluator.add(count_field);
  evaluator.set_selection(selection_field);
  evaluator.evaluate();
  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  const VArray<int> counts = evaluator.get_evaluated<int>(0);

  int total_polys = 0;
  int total_loops = 0;
  Array<int> offsets(selection.size() + 1);
  for (const int i_selection : selection.index_range()) {
    const int count = std::max(counts[selection[i_selection]], 0);
    offsets[i_selection] = total_polys;
    total_polys += count;
    total_loops += count * polys[selection[i_selection]].totloop;
  }
  offsets[selection.size()] = total_polys;

  Mesh *new_mesh = BKE_mesh_new_nomain(total_loops, total_loops, 0, total_loops, total_polys);
  MutableSpan<MVert> new_verts = new_mesh->verts_for_write();
  MutableSpan<MEdge> new_edges = new_mesh->edges_for_write();
  MutableSpan<MPoly> new_polys = new_mesh->polys_for_write();
  MutableSpan<MLoop> new_loops = new_mesh->loops_for_write();

  Array<int> vert_mapping(new_verts.size());
  Array<int> edge_mapping(new_edges.size());
  Array<int> loop_mapping(new_loops.size());

  int poly_index = 0;
  int loop_index = 0;
  for (const int i_selection : selection.index_range()) {
    const IndexRange poly_range = range_for_offsets_index(offsets, i_selection);

    const MPoly &source = polys[selection[i_selection]];
    for ([[maybe_unused]] const int i_duplicate : IndexRange(poly_range.size())) {
      new_polys[poly_index] = source;
      new_polys[poly_index].loopstart = loop_index;
      for (const int i_loops : IndexRange(source.totloop)) {
        const MLoop &current_loop = loops[source.loopstart + i_loops];
        loop_mapping[loop_index] = source.loopstart + i_loops;
        new_verts[loop_index] = verts[current_loop.v];
        vert_mapping[loop_index] = current_loop.v;
        new_edges[loop_index] = edges[current_loop.e];
        edge_mapping[loop_index] = current_loop.e;
        new_edges[loop_index].v1 = loop_index;
        if (i_loops + 1 != source.totloop) {
          new_edges[loop_index].v2 = loop_index + 1;
        }
        else {
          new_edges[loop_index].v2 = new_polys[poly_index].loopstart;
        }
        new_loops[loop_index].v = loop_index;
        new_loops[loop_index].e = loop_index;
        loop_index++;
      }
      poly_index++;
    }
  }

  new_mesh->loose_edges_tag_none();

  copy_face_attributes_without_id(geometry_set,
                                  edge_mapping,
                                  vert_mapping,
                                  loop_mapping,
                                  offsets,
                                  selection,
                                  propagation_info,
                                  mesh.attributes(),
                                  new_mesh->attributes_for_write());

  copy_stable_id_faces(
      mesh, selection, offsets, vert_mapping, mesh.attributes(), new_mesh->attributes_for_write());

  if (attribute_outputs.duplicate_index) {
    create_duplicate_index_attribute(
        new_mesh->attributes_for_write(), ATTR_DOMAIN_FACE, selection, attribute_outputs, offsets);
  }

  geometry_set.replace_mesh(new_mesh);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Edges
 * \{ */

/**
 * Copies the attributes for edge duplicates. If copying the edge domain, the attributes are
 * copied with an offset fill, for point domain a mapping is used.
 */
static void copy_edge_attributes_without_id(
    GeometrySet &geometry_set,
    const Span<int> point_mapping,
    const Span<int> offsets,
    const IndexMask selection,
    const AnonymousAttributePropagationInfo &propagation_info,
    const bke::AttributeAccessor src_attributes,
    bke::MutableAttributeAccessor dst_attributes)
{
  Map<AttributeIDRef, AttributeKind> attributes = gather_attributes_without_id(
      geometry_set, GEO_COMPONENT_TYPE_MESH, propagation_info);

  for (const Map<AttributeIDRef, AttributeKind>::Item entry : attributes.items()) {
    const AttributeIDRef attribute_id = entry.key;
    GAttributeReader src_attribute = src_attributes.lookup(attribute_id);
    if (!src_attribute) {
      continue;
    }

    const eAttrDomain out_domain = src_attribute.domain;
    const eCustomDataType data_type = bke::cpp_type_to_custom_data_type(
        src_attribute.varray.type());
    GSpanAttributeWriter dst_attribute = dst_attributes.lookup_or_add_for_write_only_span(
        attribute_id, out_domain, data_type);
    if (!dst_attribute) {
      continue;
    }
    attribute_math::convert_to_static_type(data_type, [&](auto dummy) {
      using T = decltype(dummy);
      VArraySpan<T> src{src_attribute.varray.typed<T>()};
      MutableSpan<T> dst = dst_attribute.span.typed<T>();

      switch (out_domain) {
        case ATTR_DOMAIN_EDGE:
          threaded_slice_fill<T>(offsets, selection, src, dst);
          break;
        case ATTR_DOMAIN_POINT:
          array_utils::gather(src, point_mapping, dst);
          break;
        default:
          break;
      }
    });
    dst_attribute.finish();
  }
}

/**
 * Copy the stable ids to the first duplicate and create new ids based on a hash of the original id
 * and the duplicate number. This function is used for points when duplicating the edge domain.
 */
static void copy_stable_id_edges(const Mesh &mesh,
                                 const IndexMask selection,
                                 const Span<int> edge_offsets,
                                 const bke::AttributeAccessor src_attributes,
                                 bke::MutableAttributeAccessor dst_attributes)
{
  GAttributeReader src_attribute = src_attributes.lookup("id");
  if (!src_attribute) {
    return;
  }
  GSpanAttributeWriter dst_attribute = dst_attributes.lookup_or_add_for_write_only_span(
      "id", ATTR_DOMAIN_POINT, CD_PROP_INT32);
  if (!dst_attribute) {
    return;
  }

  const Span<MEdge> edges = mesh.edges();

  VArraySpan<int> src{src_attribute.varray.typed<int>()};
  MutableSpan<int> dst = dst_attribute.span.typed<int>();
  threading::parallel_for(IndexRange(selection.size()), 1024, [&](IndexRange range) {
    for (const int i_selection : range) {
      const IndexRange edge_range = range_for_offsets_index(edge_offsets, i_selection);
      if (edge_range.size() == 0) {
        continue;
      }
      const MEdge &edge = edges[selection[i_selection]];
      const IndexRange vert_range = {edge_range.start() * 2, edge_range.size() * 2};

      dst[vert_range[0]] = src[edge.v1];
      dst[vert_range[1]] = src[edge.v2];
      for (const int i_duplicate : IndexRange(1, edge_range.size() - 1)) {
        dst[vert_range[i_duplicate * 2]] = noise::hash(src[edge.v1], i_duplicate);
        dst[vert_range[i_duplicate * 2 + 1]] = noise::hash(src[edge.v2], i_duplicate);
      }
    }
  });
  dst_attribute.finish();
}

static void duplicate_edges(GeometrySet &geometry_set,
                            const Field<int> &count_field,
                            const Field<bool> &selection_field,
                            const IndexAttributes &attribute_outputs,
                            const AnonymousAttributePropagationInfo &propagation_info)
{
  if (!geometry_set.has_mesh()) {
    geometry_set.remove_geometry_during_modify();
    return;
  };
  const Mesh &mesh = *geometry_set.get_mesh_for_read();
  const Span<MEdge> edges = mesh.edges();

  bke::MeshFieldContext field_context{mesh, ATTR_DOMAIN_EDGE};
  FieldEvaluator evaluator{field_context, edges.size()};
  evaluator.add(count_field);
  evaluator.set_selection(selection_field);
  evaluator.evaluate();
  const VArray<int> counts = evaluator.get_evaluated<int>(0);
  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();

  Array<int> edge_offsets = accumulate_counts_to_offsets(selection, counts);

  Mesh *new_mesh = BKE_mesh_new_nomain(edge_offsets.last() * 2, edge_offsets.last(), 0, 0, 0);
  MutableSpan<MEdge> new_edges = new_mesh->edges_for_write();

  Array<int> vert_orig_indices(edge_offsets.last() * 2);
  threading::parallel_for(selection.index_range(), 1024, [&](IndexRange range) {
    for (const int i_selection : range) {
      const MEdge &edge = edges[selection[i_selection]];
      const IndexRange edge_range = range_for_offsets_index(edge_offsets, i_selection);
      const IndexRange vert_range(edge_range.start() * 2, edge_range.size() * 2);

      for (const int i_duplicate : IndexRange(edge_range.size())) {
        vert_orig_indices[vert_range[i_duplicate * 2]] = edge.v1;
        vert_orig_indices[vert_range[i_duplicate * 2 + 1]] = edge.v2;
      }
    }
  });

  threading::parallel_for(selection.index_range(), 1024, [&](IndexRange range) {
    for (const int i_selection : range) {
      const IndexRange edge_range = range_for_offsets_index(edge_offsets, i_selection);
      const IndexRange vert_range(edge_range.start() * 2, edge_range.size() * 2);
      for (const int i_duplicate : IndexRange(edge_range.size())) {
        MEdge &new_edge = new_edges[edge_range[i_duplicate]];
        new_edge.v1 = vert_range[i_duplicate * 2];
        new_edge.v2 = vert_range[i_duplicate * 2] + 1;
      }
    }
  });

  copy_edge_attributes_without_id(geometry_set,
                                  vert_orig_indices,
                                  edge_offsets,
                                  selection,
                                  propagation_info,
                                  mesh.attributes(),
                                  new_mesh->attributes_for_write());

  copy_stable_id_edges(
      mesh, selection, edge_offsets, mesh.attributes(), new_mesh->attributes_for_write());

  if (attribute_outputs.duplicate_index) {
    create_duplicate_index_attribute(new_mesh->attributes_for_write(),
                                     ATTR_DOMAIN_EDGE,
                                     selection,
                                     attribute_outputs,
                                     edge_offsets);
  }

  geometry_set.replace_mesh(new_mesh);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Points (Curves)
 * \{ */

static void duplicate_points_curve(GeometrySet &geometry_set,
                                   const Field<int> &count_field,
                                   const Field<bool> &selection_field,
                                   const IndexAttributes &attribute_outputs,
                                   const AnonymousAttributePropagationInfo &propagation_info)
{
  const Curves &src_curves_id = *geometry_set.get_curves_for_read();
  const bke::CurvesGeometry &src_curves = bke::CurvesGeometry::wrap(src_curves_id.geometry);
  if (src_curves.points_num() == 0) {
    return;
  }

  bke::CurvesFieldContext field_context{src_curves, ATTR_DOMAIN_POINT};
  FieldEvaluator evaluator{field_context, src_curves.points_num()};
  evaluator.add(count_field);
  evaluator.set_selection(selection_field);
  evaluator.evaluate();
  const VArray<int> counts = evaluator.get_evaluated<int>(0);
  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();

  Array<int> offsets = accumulate_counts_to_offsets(selection, counts);
  const int dst_num = offsets.last();

  Array<int> point_to_curve_map(src_curves.points_num());
  threading::parallel_for(src_curves.curves_range(), 1024, [&](const IndexRange range) {
    for (const int i_curve : range) {
      const IndexRange points = src_curves.points_for_curve(i_curve);
      point_to_curve_map.as_mutable_span().slice(points).fill(i_curve);
    }
  });

  Curves *new_curves_id = bke::curves_new_nomain(dst_num, dst_num);
  bke::curves_copy_parameters(src_curves_id, *new_curves_id);
  bke::CurvesGeometry &new_curves = bke::CurvesGeometry::wrap(new_curves_id->geometry);
  MutableSpan<int> new_curve_offsets = new_curves.offsets_for_write();
  for (const int i : new_curves.curves_range()) {
    new_curve_offsets[i] = i;
  }
  new_curve_offsets.last() = dst_num;

  Map<AttributeIDRef, AttributeKind> attributes = gather_attributes_without_id(
      geometry_set, GEO_COMPONENT_TYPE_CURVE, propagation_info);

  for (const Map<AttributeIDRef, AttributeKind>::Item entry : attributes.items()) {
    const AttributeIDRef attribute_id = entry.key;
    GAttributeReader src_attribute = src_curves.attributes().lookup(attribute_id);
    if (!src_attribute) {
      continue;
    }

    eAttrDomain domain = src_attribute.domain;
    const eCustomDataType data_type = bke::cpp_type_to_custom_data_type(
        src_attribute.varray.type());
    GSpanAttributeWriter dst_attribute =
        new_curves.attributes_for_write().lookup_or_add_for_write_only_span(
            attribute_id, domain, data_type);
    if (!dst_attribute) {
      continue;
    }

    attribute_math::convert_to_static_type(data_type, [&](auto dummy) {
      using T = decltype(dummy);
      VArraySpan<T> src{src_attribute.varray.typed<T>()};
      MutableSpan<T> dst = dst_attribute.span.typed<T>();

      switch (domain) {
        case ATTR_DOMAIN_CURVE:
          threading::parallel_for(selection.index_range(), 512, [&](IndexRange range) {
            for (const int i_selection : range) {
              const T &src_value = src[point_to_curve_map[selection[i_selection]]];
              const IndexRange duplicate_range = range_for_offsets_index(offsets, i_selection);
              dst.slice(duplicate_range).fill(src_value);
            }
          });
          break;
        case ATTR_DOMAIN_POINT:
          threaded_slice_fill(offsets, selection, src, dst);
          break;
        default:
          break;
      }
    });
    dst_attribute.finish();
  }

  copy_stable_id_point(offsets, src_curves.attributes(), new_curves.attributes_for_write());

  if (attribute_outputs.duplicate_index) {
    create_duplicate_index_attribute(new_curves.attributes_for_write(),
                                     ATTR_DOMAIN_POINT,
                                     selection,
                                     attribute_outputs,
                                     offsets.as_span());
  }

  geometry_set.replace_curves(new_curves_id);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Points (Mesh)
 * \{ */

static void duplicate_points_mesh(GeometrySet &geometry_set,
                                  const Field<int> &count_field,
                                  const Field<bool> &selection_field,
                                  const IndexAttributes &attribute_outputs,
                                  const AnonymousAttributePropagationInfo &propagation_info)
{
  const Mesh &mesh = *geometry_set.get_mesh_for_read();
  const Span<MVert> src_verts = mesh.verts();

  bke::MeshFieldContext field_context{mesh, ATTR_DOMAIN_POINT};
  FieldEvaluator evaluator{field_context, src_verts.size()};
  evaluator.add(count_field);
  evaluator.set_selection(selection_field);
  evaluator.evaluate();
  const VArray<int> counts = evaluator.get_evaluated<int>(0);
  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();

  Array<int> offsets = accumulate_counts_to_offsets(selection, counts);

  Mesh *new_mesh = BKE_mesh_new_nomain(offsets.last(), 0, 0, 0, 0);
  MutableSpan<MVert> dst_verts = new_mesh->verts_for_write();

  threaded_slice_fill(offsets.as_span(), selection, src_verts, dst_verts);

  copy_attributes_without_id(geometry_set,
                             GEO_COMPONENT_TYPE_MESH,
                             ATTR_DOMAIN_POINT,
                             offsets,
                             selection,
                             propagation_info,
                             mesh.attributes(),
                             new_mesh->attributes_for_write());

  copy_stable_id_point(offsets, mesh.attributes(), new_mesh->attributes_for_write());

  if (attribute_outputs.duplicate_index) {
    create_duplicate_index_attribute(new_mesh->attributes_for_write(),
                                     ATTR_DOMAIN_POINT,
                                     selection,
                                     attribute_outputs,
                                     offsets.as_span());
  }

  geometry_set.replace_mesh(new_mesh);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Points (Point Cloud)
 * \{ */

static void duplicate_points_pointcloud(GeometrySet &geometry_set,
                                        const Field<int> &count_field,
                                        const Field<bool> &selection_field,
                                        const IndexAttributes &attribute_outputs,
                                        const AnonymousAttributePropagationInfo &propagation_info)
{
  const PointCloud &src_points = *geometry_set.get_pointcloud_for_read();

  bke::PointCloudFieldContext field_context{src_points};
  FieldEvaluator evaluator{field_context, src_points.totpoint};
  evaluator.add(count_field);
  evaluator.set_selection(selection_field);
  evaluator.evaluate();
  const VArray<int> counts = evaluator.get_evaluated<int>(0);
  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();

  Array<int> offsets = accumulate_counts_to_offsets(selection, counts);

  PointCloud *pointcloud = BKE_pointcloud_new_nomain(offsets.last());

  copy_attributes_without_id(geometry_set,
                             GEO_COMPONENT_TYPE_POINT_CLOUD,
                             ATTR_DOMAIN_POINT,
                             offsets,
                             selection,
                             propagation_info,
                             src_points.attributes(),
                             pointcloud->attributes_for_write());

  copy_stable_id_point(offsets, src_points.attributes(), pointcloud->attributes_for_write());

  if (attribute_outputs.duplicate_index) {
    create_duplicate_index_attribute(pointcloud->attributes_for_write(),
                                     ATTR_DOMAIN_POINT,
                                     selection,
                                     attribute_outputs,
                                     offsets);
  }
  geometry_set.replace_pointcloud(pointcloud);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Points
 * \{ */

static void duplicate_points(GeometrySet &geometry_set,
                             const Field<int> &count_field,
                             const Field<bool> &selection_field,
                             const IndexAttributes &attribute_outputs,
                             const AnonymousAttributePropagationInfo &propagation_info)
{
  Vector<GeometryComponentType> component_types = geometry_set.gather_component_types(true, true);
  for (const GeometryComponentType component_type : component_types) {
    switch (component_type) {
      case GEO_COMPONENT_TYPE_POINT_CLOUD:
        if (geometry_set.has_pointcloud()) {
          duplicate_points_pointcloud(
              geometry_set, count_field, selection_field, attribute_outputs, propagation_info);
        }
        break;
      case GEO_COMPONENT_TYPE_MESH:
        if (geometry_set.has_mesh()) {
          duplicate_points_mesh(
              geometry_set, count_field, selection_field, attribute_outputs, propagation_info);
        }
        break;
      case GEO_COMPONENT_TYPE_CURVE:
        if (geometry_set.has_curves()) {
          duplicate_points_curve(
              geometry_set, count_field, selection_field, attribute_outputs, propagation_info);
        }
        break;
      default:
        break;
    }
  }
  component_types.append(GEO_COMPONENT_TYPE_INSTANCES);
  geometry_set.keep_only_during_modify(component_types);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Instances
 * \{ */

static void duplicate_instances(GeometrySet &geometry_set,
                                const Field<int> &count_field,
                                const Field<bool> &selection_field,
                                const IndexAttributes &attribute_outputs,
                                const AnonymousAttributePropagationInfo &propagation_info)
{
  if (!geometry_set.has_instances()) {
    geometry_set.clear();
    return;
  }

  const bke::Instances &src_instances = *geometry_set.get_instances_for_read();

  bke::InstancesFieldContext field_context{src_instances};
  FieldEvaluator evaluator{field_context, src_instances.instances_num()};
  evaluator.add(count_field);
  evaluator.set_selection(selection_field);
  evaluator.evaluate();
  IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  const VArray<int> counts = evaluator.get_evaluated<int>(0);

  Array<int> offsets = accumulate_counts_to_offsets(selection, counts);
  if (offsets.last() == 0) {
    geometry_set.clear();
    return;
  }

  std::unique_ptr<bke::Instances> dst_instances = std::make_unique<bke::Instances>();

  dst_instances->resize(offsets.last());
  for (const int i_selection : selection.index_range()) {
    const IndexRange range = range_for_offsets_index(offsets, i_selection);
    if (range.size() == 0) {
      continue;
    }
    const int old_handle = src_instances.reference_handles()[i_selection];
    const bke::InstanceReference reference = src_instances.references()[old_handle];
    const int new_handle = dst_instances->add_reference(reference);
    const float4x4 transform = src_instances.transforms()[i_selection];
    dst_instances->transforms().slice(range).fill(transform);
    dst_instances->reference_handles().slice(range).fill(new_handle);
  }

  copy_attributes_without_id(geometry_set,
                             GEO_COMPONENT_TYPE_INSTANCES,
                             ATTR_DOMAIN_INSTANCE,
                             offsets,
                             selection,
                             propagation_info,
                             src_instances.attributes(),
                             dst_instances->attributes_for_write());

  if (attribute_outputs.duplicate_index) {
    create_duplicate_index_attribute(dst_instances->attributes_for_write(),
                                     ATTR_DOMAIN_INSTANCE,
                                     selection,
                                     attribute_outputs,
                                     offsets);
  }

  geometry_set = GeometrySet::create_with_instances(dst_instances.release());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entry Point
 * \{ */

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");

  const NodeGeometryDuplicateElements &storage = node_storage(params.node());
  const eAttrDomain duplicate_domain = eAttrDomain(storage.domain);

  Field<int> count_field = params.extract_input<Field<int>>("Amount");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  IndexAttributes attribute_outputs;
  attribute_outputs.duplicate_index = params.get_output_anonymous_attribute_id_if_needed(
      "Duplicate Index");

  const AnonymousAttributePropagationInfo &propagation_info = params.get_output_propagation_info(
      "Geometry");

  if (duplicate_domain == ATTR_DOMAIN_INSTANCE) {
    duplicate_instances(
        geometry_set, count_field, selection_field, attribute_outputs, propagation_info);
  }
  else {
    geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
      switch (duplicate_domain) {
        case ATTR_DOMAIN_CURVE:
          duplicate_curves(
              geometry_set, count_field, selection_field, attribute_outputs, propagation_info);
          break;
        case ATTR_DOMAIN_FACE:
          duplicate_faces(
              geometry_set, count_field, selection_field, attribute_outputs, propagation_info);
          break;
        case ATTR_DOMAIN_EDGE:
          duplicate_edges(
              geometry_set, count_field, selection_field, attribute_outputs, propagation_info);
          break;
        case ATTR_DOMAIN_POINT:
          duplicate_points(
              geometry_set, count_field, selection_field, attribute_outputs, propagation_info);
          break;
        default:
          BLI_assert_unreachable();
          break;
      }
    });
  }

  if (geometry_set.is_empty()) {
    params.set_default_remaining_outputs();
    return;
  }

  if (attribute_outputs.duplicate_index) {
    params.set_output(
        "Duplicate Index",
        AnonymousAttributeFieldInput::Create<int>(std::move(attribute_outputs.duplicate_index),
                                                  params.attribute_producer_name()));
  }
  params.set_output("Geometry", std::move(geometry_set));
}

/** \} */

}  // namespace blender::nodes::node_geo_duplicate_elements_cc

void register_node_type_geo_duplicate_elements()
{
  namespace file_ns = blender::nodes::node_geo_duplicate_elements_cc;
  static bNodeType ntype;
  geo_node_type_base(
      &ntype, GEO_NODE_DUPLICATE_ELEMENTS, "Duplicate Elements", NODE_CLASS_GEOMETRY);

  node_type_storage(&ntype,
                    "NodeGeometryDuplicateElements",
                    node_free_standard_storage,
                    node_copy_standard_storage);

  ntype.initfunc = file_ns::node_init;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  nodeRegisterType(&ntype);
}
