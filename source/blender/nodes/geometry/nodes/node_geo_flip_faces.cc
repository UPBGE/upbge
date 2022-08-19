/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_attribute_math.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_flip_faces_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Mesh")).supported_type(GEO_COMPONENT_TYPE_MESH);
  b.add_input<decl::Bool>(N_("Selection")).default_value(true).hide_value().supports_field();
  b.add_output<decl::Geometry>(N_("Mesh"));
}

static void mesh_flip_faces(MeshComponent &component, const Field<bool> &selection_field)
{
  GeometryComponentFieldContext field_context{component, ATTR_DOMAIN_FACE};
  const int domain_size = component.attribute_domain_size(ATTR_DOMAIN_FACE);
  if (domain_size == 0) {
    return;
  }
  fn::FieldEvaluator evaluator{field_context, domain_size};
  evaluator.add(selection_field);
  evaluator.evaluate();
  const IndexMask selection = evaluator.get_evaluated_as_mask(0);

  Mesh *mesh = component.get_for_write();

  mesh->mloop = (MLoop *)CustomData_duplicate_referenced_layer(
      &mesh->ldata, CD_MLOOP, mesh->totloop);
  Span<MPoly> polys{mesh->mpoly, mesh->totpoly};
  MutableSpan<MLoop> loops{mesh->mloop, mesh->totloop};

  for (const int i : selection.index_range()) {
    const MPoly &poly = polys[selection[i]];
    int start = poly.loopstart;
    for (const int j : IndexRange(poly.totloop / 2)) {
      const int index1 = start + j + 1;
      const int index2 = start + poly.totloop - j - 1;
      std::swap(loops[index1].v, loops[index2].v);
      std::swap(loops[index1 - 1].e, loops[index2].e);
    }
  }

  MutableAttributeAccessor attributes = *component.attributes_for_write();
  attributes.for_all(
      [&](const bke::AttributeIDRef &attribute_id, const AttributeMetaData &meta_data) {
        if (meta_data.domain == ATTR_DOMAIN_CORNER) {
          GSpanAttributeWriter attribute = attributes.lookup_or_add_for_write_span(
              attribute_id, ATTR_DOMAIN_CORNER, meta_data.data_type);
          attribute_math::convert_to_static_type(meta_data.data_type, [&](auto dummy) {
            using T = decltype(dummy);
            MutableSpan<T> dst_span = attribute.span.typed<T>();
            for (const int j : selection.index_range()) {
              const MPoly &poly = polys[selection[j]];
              dst_span.slice(poly.loopstart + 1, poly.totloop - 1).reverse();
            }
          });
          attribute.finish();
        }
        return true;
      });
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");

  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (!geometry_set.has_mesh()) {
      return;
    }
    MeshComponent &mesh_component = geometry_set.get_component_for_write<MeshComponent>();
    mesh_flip_faces(mesh_component, selection_field);
  });

  params.set_output("Mesh", std::move(geometry_set));
}

}  // namespace blender::nodes::node_geo_flip_faces_cc

void register_node_type_geo_flip_faces()
{
  namespace file_ns = blender::nodes::node_geo_flip_faces_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_FLIP_FACES, "Flip Faces", NODE_CLASS_GEOMETRY);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  nodeRegisterType(&ntype);
}
