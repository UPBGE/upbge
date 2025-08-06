/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_matrix.hh"

#include "DNA_pointcloud_types.h"

#include "BKE_customdata.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"
#include "BKE_pointcloud.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_join_geometries.hh"
#include "GEO_resample_curves.hh"

#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_to_points_cc {

NODE_STORAGE_FUNCS(NodeGeometryCurveToPoints)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curve")
      .supported_type({GeometryComponent::Type::Curve, GeometryComponent::Type::GreasePencil})
      .description("Curves to convert to points");
  auto &count = b.add_input<decl::Int>("Count")
                    .default_value(10)
                    .min(2)
                    .max(100000)
                    .field_on_all()
                    .make_available([](bNode &node) {
                      node_storage(node).mode = GEO_NODE_CURVE_RESAMPLE_COUNT;
                    });
  auto &length = b.add_input<decl::Float>("Length")
                     .default_value(0.1f)
                     .min(0.001f)
                     .subtype(PROP_DISTANCE)
                     .field_on_all()
                     .make_available([](bNode &node) {
                       node_storage(node).mode = GEO_NODE_CURVE_RESAMPLE_LENGTH;
                     });
  b.add_output<decl::Geometry>("Points").propagate_all();
  b.add_output<decl::Vector>("Tangent").field_on_all();
  b.add_output<decl::Vector>("Normal").field_on_all();
  b.add_output<decl::Rotation>("Rotation").field_on_all();

  const bNode *node = b.node_or_null();
  if (node != nullptr) {
    const NodeGeometryCurveToPoints &storage = node_storage(*node);
    const GeometryNodeCurveResampleMode mode = GeometryNodeCurveResampleMode(storage.mode);

    count.available(mode == GEO_NODE_CURVE_RESAMPLE_COUNT);
    length.available(mode == GEO_NODE_CURVE_RESAMPLE_LENGTH);
  }
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryCurveToPoints *data = MEM_callocN<NodeGeometryCurveToPoints>(__func__);

  data->mode = GEO_NODE_CURVE_RESAMPLE_COUNT;
  node->storage = data;
}

static void fill_rotation_attribute(const Span<float3> tangents,
                                    const Span<float3> normals,
                                    MutableSpan<math::Quaternion> rotations)
{
  threading::parallel_for(IndexRange(rotations.size()), 512, [&](IndexRange range) {
    for (const int i : range) {
      rotations[i] = math::to_quaternion(
          math::from_orthonormal_axes<float4x4>(normals[i], tangents[i]));
    }
  });
}

static void copy_curve_domain_attributes(const AttributeAccessor curve_attributes,
                                         const AttributeFilter &attribute_filter,
                                         MutableAttributeAccessor point_attributes)
{
  curve_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.is_builtin) {
      return;
    }
    if (iter.domain != AttrDomain::Curve) {
      return;
    }
    if (attribute_filter.allow_skip(iter.name)) {
      return;
    }
    if (iter.data_type == bke::AttrType::String) {
      return;
    }
    point_attributes.add(iter.name,
                         AttrDomain::Point,
                         iter.data_type,
                         bke::AttributeInitVArray(*iter.get(AttrDomain::Point)));
  });
}

static PointCloud *curves_to_points(
    const bke::CurvesGeometry &curves,
    const AttributeFilter &attribute_filter,
    const geometry::ResampleCurvesOutputAttributeIDs &resample_attributes,
    const std::optional<StringRef> &rotation_id)
{
  const AttributeAccessor curve_attributes = curves.attributes();

  PointCloud *pointcloud = bke::pointcloud_new_no_attributes(curves.points_num());
  MutableAttributeAccessor point_attributes = pointcloud->attributes_for_write();

  const bke::AttributeFilterFromFunc filter = [&](const StringRef name) {
    if (ELEM(name, resample_attributes.tangent_id, resample_attributes.normal_id, rotation_id)) {
      return bke::AttributeFilter::Result::Process;
    }
    if (attribute_filter.allow_skip(name)) {
      return bke::AttributeFilter::Result::AllowSkip;
    }
    if (curve_attributes.is_builtin(name) && !point_attributes.is_builtin(name)) {
      return bke::AttributeFilter::Result::AllowSkip;
    }
    return bke::AttributeFilter::Result::Process;
  };

  bke::copy_attributes(curves.attributes(),
                       bke::AttrDomain::Point,
                       bke::AttrDomain::Point,
                       filter,
                       pointcloud->attributes_for_write());

  copy_curve_domain_attributes(curve_attributes, filter, point_attributes);

  if (rotation_id) {
    const VArraySpan tangents = *curve_attributes.lookup<float3>(*resample_attributes.tangent_id,
                                                                 AttrDomain::Point);
    const VArraySpan normals = *curve_attributes.lookup<float3>(*resample_attributes.normal_id,
                                                                AttrDomain::Point);
    SpanAttributeWriter rotations =
        point_attributes.lookup_or_add_for_write_only_span<math::Quaternion>(*rotation_id,
                                                                             AttrDomain::Point);
    fill_rotation_attribute(tangents, normals, rotations.span);
    rotations.finish();
  }

  return pointcloud;
}

static void layer_pointclouds_to_instances(const Span<PointCloud *> pointcloud_by_layer,
                                           const AttributeFilter &attribute_filter,
                                           GeometrySet &geometry)
{
  if (!pointcloud_by_layer.is_empty()) {
    bke::Instances *instances = new bke::Instances();
    for (PointCloud *pointcloud : pointcloud_by_layer) {
      if (!pointcloud) {
        /* Add an empty reference so the number of layers and instances match.
         * This makes it easy to reconstruct the layers afterwards and keep their
         * attributes. */
        const int handle = instances->add_reference(bke::InstanceReference());
        instances->add_instance(handle, float4x4::identity());
        continue;
      }
      GeometrySet temp_set = GeometrySet::from_pointcloud(pointcloud);
      const int handle = instances->add_reference(bke::InstanceReference{temp_set});
      instances->add_instance(handle, float4x4::identity());
    }

    bke::copy_attributes(geometry.get_grease_pencil()->attributes(),
                         bke::AttrDomain::Layer,
                         bke::AttrDomain::Instance,
                         attribute_filter,
                         instances->attributes_for_write());
    InstancesComponent &dst_component = geometry.get_component_for_write<InstancesComponent>();
    GeometrySet new_instances = geometry::join_geometries(
        {GeometrySet::from_instances(dst_component.release()),
         GeometrySet::from_instances(instances)},
        attribute_filter);
    dst_component.replace(new_instances.get_component_for_write<InstancesComponent>().release());
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeGeometryCurveToPoints &storage = node_storage(params.node());
  const GeometryNodeCurveResampleMode mode = (GeometryNodeCurveResampleMode)storage.mode;
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curve");

  GeometryComponentEditData::remember_deformed_positions_if_necessary(geometry_set);

  std::optional<std::string> rotation_anonymous_id =
      params.get_output_anonymous_attribute_id_if_needed("Rotation");
  const bool need_tangent_and_normal = bool(rotation_anonymous_id);
  std::optional<std::string> tangent_anonymous_id =
      params.get_output_anonymous_attribute_id_if_needed("Tangent", need_tangent_and_normal);
  std::optional<std::string> normal_anonymous_id =
      params.get_output_anonymous_attribute_id_if_needed("Normal", need_tangent_and_normal);

  geometry::ResampleCurvesOutputAttributeIDs resample_attributes;
  resample_attributes.tangent_id = tangent_anonymous_id;
  resample_attributes.normal_id = normal_anonymous_id;
  const NodeAttributeFilter &attribute_filter = params.get_attribute_filter("Points");

  switch (mode) {
    case GEO_NODE_CURVE_RESAMPLE_COUNT: {
      const Field<int> count = params.extract_input<Field<int>>("Count");
      geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry) {
        if (const Curves *src_curves_id = geometry.get_curves()) {
          bke::CurvesGeometry dst_curves = geometry::resample_to_count(
              src_curves_id->geometry.wrap(),
              bke::CurvesFieldContext(*src_curves_id, AttrDomain::Curve),
              fn::make_constant_field<bool>(true),
              count,
              resample_attributes);
          PointCloud *pointcloud = curves_to_points(
              dst_curves, attribute_filter, resample_attributes, rotation_anonymous_id);
          geometry.replace_pointcloud(pointcloud);
        }
        if (const GreasePencil *grease_pencil = geometry.get_grease_pencil()) {
          Array<PointCloud *> pointcloud_by_layer(grease_pencil->layers().size(), nullptr);
          for (const int layer_index : grease_pencil->layers().index_range()) {
            const bke::greasepencil::Drawing *drawing = grease_pencil->get_eval_drawing(
                grease_pencil->layer(layer_index));
            if (drawing == nullptr) {
              continue;
            }
            bke::CurvesGeometry dst_curves = geometry::resample_to_count(
                drawing->strokes(),
                bke::GreasePencilLayerFieldContext(*grease_pencil, AttrDomain::Curve, layer_index),
                fn::make_constant_field<bool>(true),
                count,
                resample_attributes);
            pointcloud_by_layer[layer_index] = curves_to_points(
                dst_curves, attribute_filter, resample_attributes, rotation_anonymous_id);
          }
          layer_pointclouds_to_instances(pointcloud_by_layer, attribute_filter, geometry);
        }
        geometry.keep_only({bke::GeometryComponent::Type::PointCloud,
                            bke::GeometryComponent::Type::Instance,
                            bke::GeometryComponent::Type::Edit});
      });
      break;
    }
    case GEO_NODE_CURVE_RESAMPLE_LENGTH: {
      const Field<float> length = params.extract_input<Field<float>>("Length");
      geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry) {
        if (const Curves *src_curves_id = geometry.get_curves()) {
          bke::CurvesGeometry dst_curves = geometry::resample_to_length(
              src_curves_id->geometry.wrap(),
              bke::CurvesFieldContext(*src_curves_id, AttrDomain::Curve),
              fn::make_constant_field<bool>(true),
              length,
              resample_attributes);
          PointCloud *pointcloud = curves_to_points(
              dst_curves, attribute_filter, resample_attributes, rotation_anonymous_id);
          geometry.replace_pointcloud(pointcloud);
        }
        if (const GreasePencil *grease_pencil = geometry.get_grease_pencil()) {
          Array<PointCloud *> pointcloud_by_layer(grease_pencil->layers().size(), nullptr);
          for (const int layer_index : grease_pencil->layers().index_range()) {
            const bke::greasepencil::Drawing *drawing = grease_pencil->get_eval_drawing(
                grease_pencil->layer(layer_index));
            if (drawing == nullptr) {
              continue;
            }
            bke::CurvesGeometry dst_curves = geometry::resample_to_length(
                drawing->strokes(),
                bke::GreasePencilLayerFieldContext(*grease_pencil, AttrDomain::Curve, layer_index),
                fn::make_constant_field<bool>(true),
                length,
                resample_attributes);
            pointcloud_by_layer[layer_index] = curves_to_points(
                dst_curves, attribute_filter, resample_attributes, rotation_anonymous_id);
          }
          layer_pointclouds_to_instances(pointcloud_by_layer, attribute_filter, geometry);
        }
        geometry.keep_only({bke::GeometryComponent::Type::PointCloud,
                            bke::GeometryComponent::Type::Instance,
                            bke::GeometryComponent::Type::Edit});
      });
      break;
    }
    case GEO_NODE_CURVE_RESAMPLE_EVALUATED: {
      geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry) {
        if (const Curves *src_curves_id = geometry.get_curves()) {
          bke::CurvesGeometry dst_curves = geometry::resample_to_evaluated(
              src_curves_id->geometry.wrap(),
              bke::CurvesFieldContext(*src_curves_id, AttrDomain::Curve),
              fn::make_constant_field<bool>(true),
              resample_attributes);
          PointCloud *pointcloud = curves_to_points(
              dst_curves, attribute_filter, resample_attributes, rotation_anonymous_id);
          geometry.replace_pointcloud(pointcloud);
          geometry.replace_curves(nullptr);
        }
        if (const GreasePencil *grease_pencil = geometry.get_grease_pencil()) {
          Array<PointCloud *> pointcloud_by_layer(grease_pencil->layers().size(), nullptr);
          for (const int layer_index : grease_pencil->layers().index_range()) {
            const bke::greasepencil::Drawing *drawing = grease_pencil->get_eval_drawing(
                grease_pencil->layer(layer_index));
            if (drawing == nullptr) {
              continue;
            }
            bke::CurvesGeometry dst_curves = geometry::resample_to_evaluated(
                drawing->strokes(),
                bke::GreasePencilLayerFieldContext(*grease_pencil, AttrDomain::Curve, layer_index),
                fn::make_constant_field<bool>(true),
                resample_attributes);
            pointcloud_by_layer[layer_index] = curves_to_points(
                dst_curves, attribute_filter, resample_attributes, rotation_anonymous_id);
          }
          layer_pointclouds_to_instances(pointcloud_by_layer, attribute_filter, geometry);
        }
        geometry.keep_only({bke::GeometryComponent::Type::PointCloud,
                            bke::GeometryComponent::Type::Instance,
                            bke::GeometryComponent::Type::Edit});
      });
      break;
    }
  }

  params.set_output("Points", std::move(geometry_set));
}

static void node_rna(StructRNA *srna)
{
  static EnumPropertyItem mode_items[] = {
      {GEO_NODE_CURVE_RESAMPLE_EVALUATED,
       "EVALUATED",
       0,
       "Evaluated",
       "Create points from the curve's evaluated points, based on the resolution attribute for "
       "NURBS and Bézier splines"},
      {GEO_NODE_CURVE_RESAMPLE_COUNT,
       "COUNT",
       0,
       "Count",
       "Sample each spline by evenly distributing the specified number of points"},
      {GEO_NODE_CURVE_RESAMPLE_LENGTH,
       "LENGTH",
       0,
       "Length",
       "Sample each spline by splitting it into segments with the specified length"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_node_enum(srna,
                    "mode",
                    "Mode",
                    "How to generate points from the input curve",
                    mode_items,
                    NOD_storage_enum_accessors(mode),
                    GEO_NODE_CURVE_RESAMPLE_COUNT);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCurveToPoints", GEO_NODE_CURVE_TO_POINTS);
  ntype.ui_name = "Curve to Points";
  ntype.ui_description = "Generate a point cloud by sampling positions along curves";
  ntype.enum_name_legacy = "CURVE_TO_POINTS";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  blender::bke::node_type_storage(
      ntype, "NodeGeometryCurveToPoints", node_free_standard_storage, node_copy_standard_storage);
  ntype.initfunc = node_init;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curve_to_points_cc
