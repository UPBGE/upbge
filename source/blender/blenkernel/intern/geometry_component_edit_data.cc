/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"

using namespace blender;
using namespace blender::bke;

GeometryComponentEditData::GeometryComponentEditData() : GeometryComponent(GEO_COMPONENT_TYPE_EDIT)
{
}

GeometryComponent *GeometryComponentEditData::copy() const
{
  GeometryComponentEditData *new_component = new GeometryComponentEditData();
  if (curves_edit_hints_) {
    new_component->curves_edit_hints_ = std::make_unique<CurvesEditHints>(*curves_edit_hints_);
  }
  return new_component;
}

bool GeometryComponentEditData::owns_direct_data() const
{
  return true;
}

void GeometryComponentEditData::ensure_owns_direct_data()
{
  /* Nothing to do. */
}

void GeometryComponentEditData::remember_deformed_curve_positions_if_necessary(
    GeometrySet &geometry)
{
  /* This component should be created at the start of object evaluation if it's necessary. */
  if (!geometry.has<GeometryComponentEditData>()) {
    return;
  }
  GeometryComponentEditData &edit_component =
      geometry.get_component_for_write<GeometryComponentEditData>();
  if (!edit_component.curves_edit_hints_) {
    return;
  }
  if (edit_component.curves_edit_hints_->positions.has_value()) {
    return;
  }
  const Curves *curves_id = geometry.get_curves_for_read();
  if (curves_id == nullptr) {
    return;
  }
  const bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id->geometry);
  const int points_num = curves.points_num();
  if (points_num != edit_component.curves_edit_hints_->curves_id_orig.geometry.point_num) {
    return;
  }
  edit_component.curves_edit_hints_->positions.emplace(points_num);
  edit_component.curves_edit_hints_->positions->as_mutable_span().copy_from(curves.positions());
}
