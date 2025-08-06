/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"

#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_set_curve_type.hh"

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_spline_type_cc {

NODE_STORAGE_FUNCS(NodeGeometryCurveSplineType)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  b.add_input<decl::Geometry>("Curve")
      .supported_type(GeometryComponent::Type::Curve)
      .description("Curves to change the type of");
  b.add_output<decl::Geometry>("Curve").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "spline_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryCurveSplineType *data = MEM_callocN<NodeGeometryCurveSplineType>(__func__);

  data->spline_type = CURVE_TYPE_POLY;
  node->storage = data;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeGeometryCurveSplineType &storage = node_storage(params.node());
  const CurveType dst_type = CurveType(storage.spline_type);

  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curve");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (!geometry_set.has_curves()) {
      return;
    }
    const Curves &src_curves_id = *geometry_set.get_curves();
    const bke::CurvesGeometry &src_curves = src_curves_id.geometry.wrap();
    if (src_curves.is_single_type(dst_type)) {
      return;
    }

    const bke::CurvesFieldContext field_context{src_curves_id, AttrDomain::Curve};
    fn::FieldEvaluator evaluator{field_context, src_curves.curves_num()};
    evaluator.set_selection(selection_field);
    evaluator.evaluate();
    const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
    if (selection.is_empty()) {
      return;
    }

    bke::CurvesGeometry dst_curves = geometry::convert_curves(
        src_curves, selection, dst_type, params.get_attribute_filter("Curve"));
    Curves *dst_curves_id = bke::curves_new_nomain(std::move(dst_curves));
    bke::curves_copy_parameters(src_curves_id, *dst_curves_id);
    geometry_set.replace_curves(dst_curves_id);
  });

  params.set_output("Curve", std::move(geometry_set));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "spline_type",
                    "Type",
                    "The curve type to change the selected curves to",
                    rna_enum_curves_type_items,
                    NOD_storage_enum_accessors(spline_type),
                    CURVE_TYPE_POLY,
                    nullptr,
                    true);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeCurveSplineType", GEO_NODE_CURVE_SPLINE_TYPE);
  ntype.ui_name = "Set Spline Type";
  ntype.ui_description = "Change the type of curves";
  ntype.enum_name_legacy = "CURVE_SPLINE_TYPE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(ntype,
                                  "NodeGeometryCurveSplineType",
                                  node_free_standard_storage,
                                  node_copy_standard_storage);
  ntype.draw_buttons = node_layout;

  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curve_spline_type_cc
