/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_index_mask_ops.hh"
#include "BLI_virtual_array.hh"

#include "BKE_attribute.hh"
#include "BKE_compute_contexts.hh"
#include "BKE_context.h"
#include "BKE_curves.hh"
#include "BKE_editmesh.h"
#include "BKE_geometry_fields.hh"
#include "BKE_global.h"
#include "BKE_instances.hh"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_wrapper.h"
#include "BKE_modifier.h"
#include "BKE_volume.h"

#include "DNA_ID.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "DEG_depsgraph_query.h"

#include "ED_curves.h"
#include "ED_spreadsheet.h"

#include "NOD_geometry_nodes_lazy_function.hh"
#include "NOD_geometry_nodes_log.hh"

#include "BLT_translation.h"

#include "RNA_access.h"
#include "RNA_enum_types.h"

#include "FN_field_cpp_type.hh"

#include "bmesh.h"

#include "spreadsheet_data_source_geometry.hh"
#include "spreadsheet_intern.hh"

using blender::nodes::geo_eval_log::ViewerNodeLog;

namespace blender::ed::spreadsheet {

void ExtraColumns::foreach_default_column_ids(
    FunctionRef<void(const SpreadsheetColumnID &, bool is_extra)> fn) const
{
  for (const auto item : columns_.items()) {
    SpreadsheetColumnID column_id;
    column_id.name = (char *)item.key.c_str();
    fn(column_id, true);
  }
}

std::unique_ptr<ColumnValues> ExtraColumns::get_column_values(
    const SpreadsheetColumnID &column_id) const
{
  const GSpan *values = columns_.lookup_ptr(column_id.name);
  if (values == nullptr) {
    return {};
  }
  return std::make_unique<ColumnValues>(column_id.name, GVArray::ForSpan(*values));
}

void GeometryDataSource::foreach_default_column_ids(
    FunctionRef<void(const SpreadsheetColumnID &, bool is_extra)> fn) const
{
  if (!component_->attributes().has_value()) {
    return;
  }
  const bke::AttributeAccessor attributes = *component_->attributes();

  if (attributes.domain_size(domain_) == 0) {
    return;
  }

  if (component_->type() == GEO_COMPONENT_TYPE_INSTANCES) {
    fn({(char *)"Name"}, false);
  }

  extra_columns_.foreach_default_column_ids(fn);

  attributes.for_all(
      [&](const bke::AttributeIDRef &attribute_id, const bke::AttributeMetaData &meta_data) {
        if (meta_data.domain != domain_) {
          return true;
        }
        if (attribute_id.is_anonymous()) {
          return true;
        }
        if (!bke::allow_procedural_attribute_access(attribute_id.name())) {
          return true;
        }
        SpreadsheetColumnID column_id;
        column_id.name = (char *)attribute_id.name().data();
        const bool is_front = attribute_id.name() == ".viewer";
        fn(column_id, is_front);
        return true;
      });

  if (component_->type() == GEO_COMPONENT_TYPE_INSTANCES) {
    fn({(char *)"Rotation"}, false);
    fn({(char *)"Scale"}, false);
  }
  else if (G.debug_value == 4001 && component_->type() == GEO_COMPONENT_TYPE_MESH) {
    if (domain_ == ATTR_DOMAIN_EDGE) {
      fn({(char *)"Vertex 1"}, false);
      fn({(char *)"Vertex 2"}, false);
    }
    else if (domain_ == ATTR_DOMAIN_FACE) {
      fn({(char *)"Corner Start"}, false);
      fn({(char *)"Corner Size"}, false);
    }
    else if (domain_ == ATTR_DOMAIN_CORNER) {
      fn({(char *)"Vertex"}, false);
      fn({(char *)"Edge"}, false);
    }
  }
}

std::unique_ptr<ColumnValues> GeometryDataSource::get_column_values(
    const SpreadsheetColumnID &column_id) const
{
  if (!component_->attributes().has_value()) {
    return {};
  }
  const bke::AttributeAccessor attributes = *component_->attributes();
  const int domain_num = attributes.domain_size(domain_);
  if (domain_num == 0) {
    return {};
  }

  std::lock_guard lock{mutex_};

  std::unique_ptr<ColumnValues> extra_column_values = extra_columns_.get_column_values(column_id);
  if (extra_column_values) {
    return extra_column_values;
  }

  if (component_->type() == GEO_COMPONENT_TYPE_INSTANCES) {
    if (const bke::Instances *instances =
            static_cast<const InstancesComponent &>(*component_).get_for_read()) {
      if (STREQ(column_id.name, "Name")) {
        Span<int> reference_handles = instances->reference_handles();
        Span<bke::InstanceReference> references = instances->references();
        return std::make_unique<ColumnValues>(
            column_id.name,
            VArray<bke::InstanceReference>::ForFunc(
                domain_num, [reference_handles, references](int64_t index) {
                  return references[reference_handles[index]];
                }));
      }
      Span<float4x4> transforms = instances->transforms();
      if (STREQ(column_id.name, "Rotation")) {
        return std::make_unique<ColumnValues>(
            column_id.name, VArray<float3>::ForFunc(domain_num, [transforms](int64_t index) {
              return transforms[index].to_euler();
            }));
      }
      if (STREQ(column_id.name, "Scale")) {
        return std::make_unique<ColumnValues>(
            column_id.name, VArray<float3>::ForFunc(domain_num, [transforms](int64_t index) {
              return transforms[index].scale();
            }));
      }
    }
  }
  else if (G.debug_value == 4001 && component_->type() == GEO_COMPONENT_TYPE_MESH) {
    const MeshComponent &component = static_cast<const MeshComponent &>(*component_);
    if (const Mesh *mesh = component.get_for_read()) {
      const Span<MEdge> edges = mesh->edges();
      const Span<MPoly> polys = mesh->polys();
      const Span<MLoop> loops = mesh->loops();

      if (domain_ == ATTR_DOMAIN_EDGE) {
        if (STREQ(column_id.name, "Vertex 1")) {
          return std::make_unique<ColumnValues>(
              column_id.name, VArray<int>::ForFunc(edges.size(), [edges](int64_t index) {
                return edges[index].v1;
              }));
        }
        if (STREQ(column_id.name, "Vertex 2")) {
          return std::make_unique<ColumnValues>(
              column_id.name, VArray<int>::ForFunc(edges.size(), [edges](int64_t index) {
                return edges[index].v2;
              }));
        }
      }
      else if (domain_ == ATTR_DOMAIN_FACE) {
        if (STREQ(column_id.name, "Corner Start")) {
          return std::make_unique<ColumnValues>(
              column_id.name, VArray<int>::ForFunc(polys.size(), [polys](int64_t index) {
                return polys[index].loopstart;
              }));
        }
        if (STREQ(column_id.name, "Corner Size")) {
          return std::make_unique<ColumnValues>(
              column_id.name, VArray<int>::ForFunc(polys.size(), [polys](int64_t index) {
                return polys[index].totloop;
              }));
        }
      }
      else if (domain_ == ATTR_DOMAIN_CORNER) {
        if (STREQ(column_id.name, "Vertex")) {
          return std::make_unique<ColumnValues>(
              column_id.name, VArray<int>::ForFunc(loops.size(), [loops](int64_t index) {
                return loops[index].v;
              }));
        }
        if (STREQ(column_id.name, "Edge")) {
          return std::make_unique<ColumnValues>(
              column_id.name, VArray<int>::ForFunc(loops.size(), [loops](int64_t index) {
                return loops[index].e;
              }));
        }
      }
    }
  }

  bke::GAttributeReader attribute = attributes.lookup(column_id.name);
  if (!attribute) {
    return {};
  }
  GVArray varray = std::move(attribute.varray);
  if (attribute.domain != domain_) {
    return {};
  }

  StringRefNull column_display_name = column_id.name;
  if (column_display_name == ".viewer") {
    column_display_name = "Viewer";
  }

  return std::make_unique<ColumnValues>(column_display_name, std::move(varray));
}

int GeometryDataSource::tot_rows() const
{
  if (!component_->attributes().has_value()) {
    return {};
  }
  const bke::AttributeAccessor attributes = *component_->attributes();
  return attributes.domain_size(domain_);
}

bool GeometryDataSource::has_selection_filter() const
{
  Object *object_orig = DEG_get_original_object(object_eval_);
  switch (component_->type()) {
    case GEO_COMPONENT_TYPE_MESH: {
      if (object_orig->type != OB_MESH) {
        return false;
      }
      if (object_orig->mode != OB_MODE_EDIT) {
        return false;
      }
      return true;
    }
    case GEO_COMPONENT_TYPE_CURVE: {
      if (object_orig->type != OB_CURVES) {
        return false;
      }
      if (!ELEM(object_orig->mode, OB_MODE_SCULPT_CURVES, OB_MODE_EDIT)) {
        return false;
      }
      return true;
    }
    default:
      return false;
  }
}

IndexMask GeometryDataSource::apply_selection_filter(Vector<int64_t> &indices) const
{
  std::lock_guard lock{mutex_};
  const IndexMask full_range(this->tot_rows());
  if (full_range.is_empty()) {
    return full_range;
  }

  switch (component_->type()) {
    case GEO_COMPONENT_TYPE_MESH: {
      BLI_assert(object_eval_->type == OB_MESH);
      BLI_assert(object_eval_->mode == OB_MODE_EDIT);
      Object *object_orig = DEG_get_original_object(object_eval_);
      const Mesh *mesh_eval = geometry_set_.get_mesh_for_read();
      const bke::AttributeAccessor attributes_eval = mesh_eval->attributes();
      Mesh *mesh_orig = (Mesh *)object_orig->data;
      BMesh *bm = mesh_orig->edit_mesh->bm;
      BM_mesh_elem_table_ensure(bm, BM_VERT);

      const int *orig_indices = (int *)CustomData_get_layer(&mesh_eval->vdata, CD_ORIGINDEX);
      if (orig_indices != nullptr) {
        /* Use CD_ORIGINDEX layer if it exists. */
        VArray<bool> selection = attributes_eval.adapt_domain<bool>(
            VArray<bool>::ForFunc(mesh_eval->totvert,
                                  [bm, orig_indices](int vertex_index) -> bool {
                                    const int i_orig = orig_indices[vertex_index];
                                    if (i_orig < 0) {
                                      return false;
                                    }
                                    if (i_orig >= bm->totvert) {
                                      return false;
                                    }
                                    const BMVert *vert = BM_vert_at_index(bm, i_orig);
                                    return BM_elem_flag_test(vert, BM_ELEM_SELECT);
                                  }),
            ATTR_DOMAIN_POINT,
            domain_);
        return index_mask_ops::find_indices_from_virtual_array(
            full_range, selection, 1024, indices);
      }

      if (mesh_eval->totvert == bm->totvert) {
        /* Use a simple heuristic to match original vertices to evaluated ones. */
        VArray<bool> selection = attributes_eval.adapt_domain<bool>(
            VArray<bool>::ForFunc(mesh_eval->totvert,
                                  [bm](int vertex_index) -> bool {
                                    const BMVert *vert = BM_vert_at_index(bm, vertex_index);
                                    return BM_elem_flag_test(vert, BM_ELEM_SELECT);
                                  }),
            ATTR_DOMAIN_POINT,
            domain_);
        return index_mask_ops::find_indices_from_virtual_array(
            full_range, selection, 2048, indices);
      }

      return full_range;
    }
    case GEO_COMPONENT_TYPE_CURVE: {
      BLI_assert(object_eval_->type == OB_CURVES);
      BLI_assert(object_eval_->mode == OB_MODE_SCULPT_CURVES);
      const CurveComponent &component = static_cast<const CurveComponent &>(*component_);
      const Curves &curves_id = *component.get_for_read();
      switch (domain_) {
        case ATTR_DOMAIN_POINT:
          return curves::retrieve_selected_points(curves_id, indices);
        case ATTR_DOMAIN_CURVE:
          return curves::retrieve_selected_curves(curves_id, indices);
        default:
          BLI_assert_unreachable();
      }
      return full_range;
    }
    default:
      return full_range;
  }
}

void VolumeDataSource::foreach_default_column_ids(
    FunctionRef<void(const SpreadsheetColumnID &, bool is_extra)> fn) const
{
  if (component_->is_empty()) {
    return;
  }

  for (const char *name : {"Grid Name", "Data Type", "Class"}) {
    SpreadsheetColumnID column_id{(char *)name};
    fn(column_id, false);
  }
}

std::unique_ptr<ColumnValues> VolumeDataSource::get_column_values(
    const SpreadsheetColumnID &column_id) const
{
  const Volume *volume = component_->get_for_read();
  if (volume == nullptr) {
    return {};
  }

#ifdef WITH_OPENVDB
  const int size = this->tot_rows();
  if (STREQ(column_id.name, "Grid Name")) {
    return std::make_unique<ColumnValues>(
        IFACE_("Grid Name"), VArray<std::string>::ForFunc(size, [volume](int64_t index) {
          const VolumeGrid *volume_grid = BKE_volume_grid_get_for_read(volume, index);
          return BKE_volume_grid_name(volume_grid);
        }));
  }
  if (STREQ(column_id.name, "Data Type")) {
    return std::make_unique<ColumnValues>(
        IFACE_("Data Type"), VArray<std::string>::ForFunc(size, [volume](int64_t index) {
          const VolumeGrid *volume_grid = BKE_volume_grid_get_for_read(volume, index);
          const VolumeGridType type = BKE_volume_grid_type(volume_grid);
          const char *name = nullptr;
          RNA_enum_name_from_value(rna_enum_volume_grid_data_type_items, type, &name);
          return IFACE_(name);
        }));
  }
  if (STREQ(column_id.name, "Class")) {
    return std::make_unique<ColumnValues>(
        IFACE_("Class"), VArray<std::string>::ForFunc(size, [volume](int64_t index) {
          const VolumeGrid *volume_grid = BKE_volume_grid_get_for_read(volume, index);
          openvdb::GridBase::ConstPtr grid = BKE_volume_grid_openvdb_for_read(volume, volume_grid);
          openvdb::GridClass grid_class = grid->getGridClass();
          if (grid_class == openvdb::GridClass::GRID_FOG_VOLUME) {
            return IFACE_("Fog Volume");
          }
          if (grid_class == openvdb::GridClass::GRID_LEVEL_SET) {
            return IFACE_("Level Set");
          }
          return IFACE_("Unknown");
        }));
  }
#else
  UNUSED_VARS(column_id);
#endif

  return {};
}

int VolumeDataSource::tot_rows() const
{
  const Volume *volume = component_->get_for_read();
  if (volume == nullptr) {
    return 0;
  }
  return BKE_volume_num_grids(volume);
}

GeometrySet spreadsheet_get_display_geometry_set(const SpaceSpreadsheet *sspreadsheet,
                                                 Object *object_eval)
{
  GeometrySet geometry_set;
  if (sspreadsheet->object_eval_state == SPREADSHEET_OBJECT_EVAL_STATE_ORIGINAL) {
    Object *object_orig = DEG_get_original_object(object_eval);
    if (object_orig->type == OB_MESH) {
      MeshComponent &mesh_component = geometry_set.get_component_for_write<MeshComponent>();
      if (object_orig->mode == OB_MODE_EDIT) {
        Mesh *mesh = (Mesh *)object_orig->data;
        BMEditMesh *em = mesh->edit_mesh;
        if (em != nullptr) {
          Mesh *new_mesh = (Mesh *)BKE_id_new_nomain(ID_ME, nullptr);
          /* This is a potentially heavy operation to do on every redraw. The best solution here is
           * to display the data directly from the bmesh without a conversion, which can be
           * implemented a bit later. */
          BM_mesh_bm_to_me_for_eval(em->bm, new_mesh, nullptr);
          mesh_component.replace(new_mesh, GeometryOwnershipType::Owned);
        }
      }
      else {
        Mesh *mesh = (Mesh *)object_orig->data;
        mesh_component.replace(mesh, GeometryOwnershipType::ReadOnly);
      }
    }
    else if (object_orig->type == OB_POINTCLOUD) {
      PointCloud *pointcloud = (PointCloud *)object_orig->data;
      PointCloudComponent &pointcloud_component =
          geometry_set.get_component_for_write<PointCloudComponent>();
      pointcloud_component.replace(pointcloud, GeometryOwnershipType::ReadOnly);
    }
    else if (object_orig->type == OB_CURVES) {
      const Curves &curves_id = *(const Curves *)object_orig->data;
      CurveComponent &curve_component = geometry_set.get_component_for_write<CurveComponent>();
      curve_component.replace(&const_cast<Curves &>(curves_id), GeometryOwnershipType::ReadOnly);
    }
  }
  else {
    if (object_eval->mode == OB_MODE_EDIT && object_eval->type == OB_MESH) {
      Mesh *mesh = BKE_modifier_get_evaluated_mesh_from_evaluated_object(object_eval);
      if (mesh == nullptr) {
        return geometry_set;
      }
      BKE_mesh_wrapper_ensure_mdata(mesh);
      MeshComponent &mesh_component = geometry_set.get_component_for_write<MeshComponent>();
      mesh_component.replace(mesh, GeometryOwnershipType::ReadOnly);
    }
    else {
      if (BLI_listbase_count(&sspreadsheet->viewer_path.path) == 1) {
        /* Use final evaluated object. */
        if (object_eval->runtime.geometry_set_eval != nullptr) {
          geometry_set = *object_eval->runtime.geometry_set_eval;
        }
      }
      else {
        if (const ViewerNodeLog *viewer_log =
                nodes::geo_eval_log::GeoModifierLog::find_viewer_node_log_for_path(
                    sspreadsheet->viewer_path)) {
          geometry_set = viewer_log->geometry;
        }
      }
    }
  }
  return geometry_set;
}

std::unique_ptr<DataSource> data_source_from_geometry(const bContext *C, Object *object_eval)
{
  SpaceSpreadsheet *sspreadsheet = CTX_wm_space_spreadsheet(C);
  const eAttrDomain domain = (eAttrDomain)sspreadsheet->attribute_domain;
  const GeometryComponentType component_type = GeometryComponentType(
      sspreadsheet->geometry_component_type);
  GeometrySet geometry_set = spreadsheet_get_display_geometry_set(sspreadsheet, object_eval);
  if (!geometry_set.has(component_type)) {
    return {};
  }

  if (component_type == GEO_COMPONENT_TYPE_VOLUME) {
    return std::make_unique<VolumeDataSource>(std::move(geometry_set));
  }
  return std::make_unique<GeometryDataSource>(
      object_eval, std::move(geometry_set), component_type, domain);
}

}  // namespace blender::ed::spreadsheet
