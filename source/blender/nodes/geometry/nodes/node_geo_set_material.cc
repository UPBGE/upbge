/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_volume_types.h"

#include "BKE_material.h"

namespace blender::nodes::node_geo_set_material_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Geometry"))
      .supported_type({GEO_COMPONENT_TYPE_MESH,
                       GEO_COMPONENT_TYPE_VOLUME,
                       GEO_COMPONENT_TYPE_POINT_CLOUD,
                       GEO_COMPONENT_TYPE_CURVE});
  b.add_input<decl::Bool>(N_("Selection")).default_value(true).hide_value().supports_field();
  b.add_input<decl::Material>(N_("Material")).hide_label();
  b.add_output<decl::Geometry>(N_("Geometry"));
}

static void assign_material_to_faces(Mesh &mesh, const IndexMask selection, Material *material)
{
  if (selection.size() != mesh.totpoly) {
    /* If the entire mesh isn't selected, and there is no material slot yet, add an empty
     * slot so that the faces that aren't selected can still refer to the default material. */
    BKE_id_material_eval_ensure_default_slot(&mesh.id);
  }

  int new_material_index = -1;
  for (const int i : IndexRange(mesh.totcol)) {
    Material *other_material = mesh.mat[i];
    if (other_material == material) {
      new_material_index = i;
      break;
    }
  }
  if (new_material_index == -1) {
    /* Append a new material index. */
    new_material_index = mesh.totcol;
    BKE_id_material_eval_assign(&mesh.id, new_material_index + 1, material);
  }

  mesh.mpoly = (MPoly *)CustomData_duplicate_referenced_layer(&mesh.pdata, CD_MPOLY, mesh.totpoly);
  for (const int i : selection) {
    MPoly &poly = mesh.mpoly[i];
    poly.mat_nr = new_material_index;
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Material *material = params.extract_input<Material *>("Material");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");

  /* Only add the warnings once, even if there are many unique instances. */
  bool point_selection_warning = false;
  bool volume_selection_warning = false;
  bool curves_selection_warning = false;

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (geometry_set.has_mesh()) {
      MeshComponent &mesh_component = geometry_set.get_component_for_write<MeshComponent>();
      Mesh &mesh = *mesh_component.get_for_write();
      GeometryComponentFieldContext field_context{mesh_component, ATTR_DOMAIN_FACE};

      fn::FieldEvaluator selection_evaluator{field_context, mesh.totpoly};
      selection_evaluator.add(selection_field);
      selection_evaluator.evaluate();
      const IndexMask selection = selection_evaluator.get_evaluated_as_mask(0);

      assign_material_to_faces(mesh, selection, material);
    }
    if (Volume *volume = geometry_set.get_volume_for_write()) {
      BKE_id_material_eval_assign(&volume->id, 1, material);
      if (selection_field.node().depends_on_input()) {
        volume_selection_warning = true;
      }
    }
    if (PointCloud *pointcloud = geometry_set.get_pointcloud_for_write()) {
      BKE_id_material_eval_assign(&pointcloud->id, 1, material);
      if (selection_field.node().depends_on_input()) {
        point_selection_warning = true;
      }
    }
    if (Curves *curves = geometry_set.get_curves_for_write()) {
      BKE_id_material_eval_assign(&curves->id, 1, material);
      if (selection_field.node().depends_on_input()) {
        curves_selection_warning = true;
      }
    }
  });

  if (volume_selection_warning) {
    params.error_message_add(
        NodeWarningType::Info,
        TIP_("Volumes only support a single material; selection input can not be a field"));
  }
  if (point_selection_warning) {
    params.error_message_add(
        NodeWarningType::Info,
        TIP_("Point clouds only support a single material; selection input can not be a field"));
  }
  if (curves_selection_warning) {
    params.error_message_add(
        NodeWarningType::Info,
        TIP_("Curves only support a single material; selection input can not be a field"));
  }

  params.set_output("Geometry", std::move(geometry_set));
}

}  // namespace blender::nodes::node_geo_set_material_cc

void register_node_type_geo_set_material()
{
  namespace file_ns = blender::nodes::node_geo_set_material_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_SET_MATERIAL, "Set Material", NODE_CLASS_GEOMETRY);
  ntype.declare = file_ns::node_declare;
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  nodeRegisterType(&ntype);
}
