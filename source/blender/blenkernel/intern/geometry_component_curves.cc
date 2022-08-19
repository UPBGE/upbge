/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_task.hh"

#include "DNA_ID_enums.h"
#include "DNA_curve_types.h"

#include "BKE_attribute_math.hh"
#include "BKE_curve.h"
#include "BKE_curves.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_geometry_set.hh"
#include "BKE_lib_id.h"

#include "attribute_access_intern.hh"

using blender::GVArray;

/* -------------------------------------------------------------------- */
/** \name Geometry Component Implementation
 * \{ */

CurveComponent::CurveComponent() : GeometryComponent(GEO_COMPONENT_TYPE_CURVE)
{
}

CurveComponent::~CurveComponent()
{
  this->clear();
}

GeometryComponent *CurveComponent::copy() const
{
  CurveComponent *new_component = new CurveComponent();
  if (curves_ != nullptr) {
    new_component->curves_ = BKE_curves_copy_for_eval(curves_, false);
    new_component->ownership_ = GeometryOwnershipType::Owned;
  }
  return new_component;
}

void CurveComponent::clear()
{
  BLI_assert(this->is_mutable());
  if (curves_ != nullptr) {
    if (ownership_ == GeometryOwnershipType::Owned) {
      BKE_id_free(nullptr, curves_);
    }
    if (curve_for_render_ != nullptr) {
      /* The curve created by this component should not have any edit mode data. */
      BLI_assert(curve_for_render_->editfont == nullptr && curve_for_render_->editnurb == nullptr);
      BKE_id_free(nullptr, curve_for_render_);
      curve_for_render_ = nullptr;
    }

    curves_ = nullptr;
  }
}

bool CurveComponent::has_curves() const
{
  return curves_ != nullptr;
}

void CurveComponent::replace(Curves *curves, GeometryOwnershipType ownership)
{
  BLI_assert(this->is_mutable());
  this->clear();
  curves_ = curves;
  ownership_ = ownership;
}

Curves *CurveComponent::release()
{
  BLI_assert(this->is_mutable());
  Curves *curves = curves_;
  curves_ = nullptr;
  return curves;
}

const Curves *CurveComponent::get_for_read() const
{
  return curves_;
}

Curves *CurveComponent::get_for_write()
{
  BLI_assert(this->is_mutable());
  if (ownership_ == GeometryOwnershipType::ReadOnly) {
    curves_ = BKE_curves_copy_for_eval(curves_, false);
    ownership_ = GeometryOwnershipType::Owned;
  }
  return curves_;
}

bool CurveComponent::is_empty() const
{
  return curves_ == nullptr;
}

bool CurveComponent::owns_direct_data() const
{
  return ownership_ == GeometryOwnershipType::Owned;
}

void CurveComponent::ensure_owns_direct_data()
{
  BLI_assert(this->is_mutable());
  if (ownership_ != GeometryOwnershipType::Owned) {
    curves_ = BKE_curves_copy_for_eval(curves_, false);
    ownership_ = GeometryOwnershipType::Owned;
  }
}

const Curve *CurveComponent::get_curve_for_render() const
{
  if (curves_ == nullptr) {
    return nullptr;
  }
  if (curve_for_render_ != nullptr) {
    return curve_for_render_;
  }
  std::lock_guard lock{curve_for_render_mutex_};
  if (curve_for_render_ != nullptr) {
    return curve_for_render_;
  }

  curve_for_render_ = (Curve *)BKE_id_new_nomain(ID_CU_LEGACY, nullptr);
  curve_for_render_->curve_eval = curves_;

  return curve_for_render_;
}

/** \} */

namespace blender::bke {

/* -------------------------------------------------------------------- */
/** \name Curve Normals Access
 * \{ */

static Array<float3> curve_normal_point_domain(const bke::CurvesGeometry &curves)
{
  const VArray<int8_t> types = curves.curve_types();
  const VArray<int> resolutions = curves.resolution();
  const VArray<bool> curves_cyclic = curves.cyclic();

  const Span<float3> positions = curves.positions();
  const VArray<int8_t> normal_modes = curves.normal_mode();

  const Span<float3> evaluated_normals = curves.evaluated_normals();

  Array<float3> results(curves.points_num());

  threading::parallel_for(curves.curves_range(), 128, [&](IndexRange range) {
    Vector<float3> nurbs_tangents;

    for (const int i_curve : range) {
      const IndexRange points = curves.points_for_curve(i_curve);
      const IndexRange evaluated_points = curves.evaluated_points_for_curve(i_curve);

      MutableSpan<float3> curve_normals = results.as_mutable_span().slice(points);

      switch (types[i_curve]) {
        case CURVE_TYPE_CATMULL_ROM: {
          const Span<float3> normals = evaluated_normals.slice(evaluated_points);
          const int resolution = resolutions[i_curve];
          for (const int i : IndexRange(points.size())) {
            curve_normals[i] = normals[resolution * i];
          }
          break;
        }
        case CURVE_TYPE_POLY:
          curve_normals.copy_from(evaluated_normals.slice(evaluated_points));
          break;
        case CURVE_TYPE_BEZIER: {
          const Span<float3> normals = evaluated_normals.slice(evaluated_points);
          curve_normals.first() = normals.first();
          const Span<int> offsets = curves.bezier_evaluated_offsets_for_curve(i_curve);
          for (const int i : IndexRange(points.size()).drop_front(1)) {
            curve_normals[i] = normals[offsets[i - 1]];
          }
          break;
        }
        case CURVE_TYPE_NURBS: {
          /* For NURBS curves there is no obvious correspondence between specific evaluated points
           * and control points, so normals are determined by treating them as poly curves. */
          nurbs_tangents.clear();
          nurbs_tangents.resize(points.size());
          const bool cyclic = curves_cyclic[i_curve];
          const Span<float3> curve_positions = positions.slice(points);
          bke::curves::poly::calculate_tangents(curve_positions, cyclic, nurbs_tangents);
          switch (NormalMode(normal_modes[i_curve])) {
            case NORMAL_MODE_Z_UP:
              bke::curves::poly::calculate_normals_z_up(nurbs_tangents, curve_normals);
              break;
            case NORMAL_MODE_MINIMUM_TWIST:
              bke::curves::poly::calculate_normals_minimum(nurbs_tangents, cyclic, curve_normals);
              break;
          }
          break;
        }
      }
    }
  });
  return results;
}

VArray<float3> curve_normals_varray(const CurveComponent &component, const eAttrDomain domain)
{
  if (!component.has_curves()) {
    return {};
  }

  const Curves &curves_id = *component.get_for_read();
  const bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id.geometry);

  const VArray<int8_t> types = curves.curve_types();
  if (curves.is_single_type(CURVE_TYPE_POLY)) {
    return component.attributes()->adapt_domain<float3>(
        VArray<float3>::ForSpan(curves.evaluated_normals()), ATTR_DOMAIN_POINT, domain);
  }

  Array<float3> normals = curve_normal_point_domain(curves);

  if (domain == ATTR_DOMAIN_POINT) {
    return VArray<float3>::ForContainer(std::move(normals));
  }

  if (domain == ATTR_DOMAIN_CURVE) {
    return component.attributes()->adapt_domain<float3>(
        VArray<float3>::ForContainer(std::move(normals)), ATTR_DOMAIN_POINT, ATTR_DOMAIN_CURVE);
  }

  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curve Length Field Input
 * \{ */

static VArray<float> construct_curve_length_gvarray(const CurveComponent &component,
                                                    const eAttrDomain domain)
{
  if (!component.has_curves()) {
    return {};
  }
  const Curves &curves_id = *component.get_for_read();
  const bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id.geometry);

  curves.ensure_evaluated_lengths();

  VArray<bool> cyclic = curves.cyclic();
  VArray<float> lengths = VArray<float>::ForFunc(
      curves.curves_num(), [&curves, cyclic = std::move(cyclic)](int64_t index) {
        return curves.evaluated_length_total_for_curve(index, cyclic[index]);
      });

  if (domain == ATTR_DOMAIN_CURVE) {
    return lengths;
  }

  if (domain == ATTR_DOMAIN_POINT) {
    return component.attributes()->adapt_domain<float>(
        std::move(lengths), ATTR_DOMAIN_CURVE, ATTR_DOMAIN_POINT);
  }

  return {};
}

CurveLengthFieldInput::CurveLengthFieldInput()
    : GeometryFieldInput(CPPType::get<float>(), "Spline Length node")
{
  category_ = Category::Generated;
}

GVArray CurveLengthFieldInput::get_varray_for_context(const GeometryComponent &component,
                                                      const eAttrDomain domain,
                                                      IndexMask UNUSED(mask)) const
{
  if (component.type() == GEO_COMPONENT_TYPE_CURVE) {
    const CurveComponent &curve_component = static_cast<const CurveComponent &>(component);
    return construct_curve_length_gvarray(curve_component, domain);
  }
  return {};
}

uint64_t CurveLengthFieldInput::hash() const
{
  /* Some random constant hash. */
  return 3549623580;
}

bool CurveLengthFieldInput::is_equal_to(const fn::FieldNode &other) const
{
  return dynamic_cast<const CurveLengthFieldInput *>(&other) != nullptr;
}

/** \} */

}  // namespace blender::bke

/* -------------------------------------------------------------------- */
/** \name Attribute Access Helper Functions
 * \{ */

static void tag_component_topology_changed(void *owner)
{
  blender::bke::CurvesGeometry &curves = *static_cast<blender::bke::CurvesGeometry *>(owner);
  curves.tag_topology_changed();
}

static void tag_component_curve_types_changed(void *owner)
{
  blender::bke::CurvesGeometry &curves = *static_cast<blender::bke::CurvesGeometry *>(owner);
  curves.update_curve_types();
  curves.tag_topology_changed();
}

static void tag_component_positions_changed(void *owner)
{
  blender::bke::CurvesGeometry &curves = *static_cast<blender::bke::CurvesGeometry *>(owner);
  curves.tag_positions_changed();
}

static void tag_component_normals_changed(void *owner)
{
  blender::bke::CurvesGeometry &curves = *static_cast<blender::bke::CurvesGeometry *>(owner);
  curves.tag_normals_changed();
}

/** \} */

namespace blender::bke {

/* -------------------------------------------------------------------- */
/** \name Attribute Provider Declaration
 * \{ */

/**
 * In this function all the attribute providers for a curves component are created.
 * Most data in this function is statically allocated, because it does not change over time.
 */
static ComponentAttributeProviders create_attribute_providers_for_curve()
{
  static CustomDataAccessInfo curve_access = {
      [](void *owner) -> CustomData * {
        CurvesGeometry &curves = *static_cast<CurvesGeometry *>(owner);
        return &curves.curve_data;
      },
      [](const void *owner) -> const CustomData * {
        const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
        return &curves.curve_data;
      },
      [](const void *owner) -> int {
        const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
        return curves.curves_num();
      },
      [](void * /*owner*/) {}};
  static CustomDataAccessInfo point_access = {
      [](void *owner) -> CustomData * {
        CurvesGeometry &curves = *static_cast<CurvesGeometry *>(owner);
        return &curves.point_data;
      },
      [](const void *owner) -> const CustomData * {
        const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
        return &curves.point_data;
      },
      [](const void *owner) -> int {
        const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
        return curves.points_num();
      },
      [](void * /*owner*/) {}};

  static BuiltinCustomDataLayerProvider position("position",
                                                 ATTR_DOMAIN_POINT,
                                                 CD_PROP_FLOAT3,
                                                 CD_PROP_FLOAT3,
                                                 BuiltinAttributeProvider::NonCreatable,
                                                 BuiltinAttributeProvider::Writable,
                                                 BuiltinAttributeProvider::NonDeletable,
                                                 point_access,
                                                 make_array_read_attribute<float3>,
                                                 make_array_write_attribute<float3>,
                                                 tag_component_positions_changed);

  static BuiltinCustomDataLayerProvider radius("radius",
                                               ATTR_DOMAIN_POINT,
                                               CD_PROP_FLOAT,
                                               CD_PROP_FLOAT,
                                               BuiltinAttributeProvider::Creatable,
                                               BuiltinAttributeProvider::Writable,
                                               BuiltinAttributeProvider::Deletable,
                                               point_access,
                                               make_array_read_attribute<float>,
                                               make_array_write_attribute<float>,
                                               nullptr);

  static BuiltinCustomDataLayerProvider id("id",
                                           ATTR_DOMAIN_POINT,
                                           CD_PROP_INT32,
                                           CD_PROP_INT32,
                                           BuiltinAttributeProvider::Creatable,
                                           BuiltinAttributeProvider::Writable,
                                           BuiltinAttributeProvider::Deletable,
                                           point_access,
                                           make_array_read_attribute<int>,
                                           make_array_write_attribute<int>,
                                           nullptr);

  static BuiltinCustomDataLayerProvider tilt("tilt",
                                             ATTR_DOMAIN_POINT,
                                             CD_PROP_FLOAT,
                                             CD_PROP_FLOAT,
                                             BuiltinAttributeProvider::Creatable,
                                             BuiltinAttributeProvider::Writable,
                                             BuiltinAttributeProvider::Deletable,
                                             point_access,
                                             make_array_read_attribute<float>,
                                             make_array_write_attribute<float>,
                                             tag_component_normals_changed);

  static BuiltinCustomDataLayerProvider handle_right("handle_right",
                                                     ATTR_DOMAIN_POINT,
                                                     CD_PROP_FLOAT3,
                                                     CD_PROP_FLOAT3,
                                                     BuiltinAttributeProvider::Creatable,
                                                     BuiltinAttributeProvider::Writable,
                                                     BuiltinAttributeProvider::Deletable,
                                                     point_access,
                                                     make_array_read_attribute<float3>,
                                                     make_array_write_attribute<float3>,
                                                     tag_component_positions_changed);

  static BuiltinCustomDataLayerProvider handle_left("handle_left",
                                                    ATTR_DOMAIN_POINT,
                                                    CD_PROP_FLOAT3,
                                                    CD_PROP_FLOAT3,
                                                    BuiltinAttributeProvider::Creatable,
                                                    BuiltinAttributeProvider::Writable,
                                                    BuiltinAttributeProvider::Deletable,
                                                    point_access,
                                                    make_array_read_attribute<float3>,
                                                    make_array_write_attribute<float3>,
                                                    tag_component_positions_changed);

  static BuiltinCustomDataLayerProvider handle_type_right("handle_type_right",
                                                          ATTR_DOMAIN_POINT,
                                                          CD_PROP_INT8,
                                                          CD_PROP_INT8,
                                                          BuiltinAttributeProvider::Creatable,
                                                          BuiltinAttributeProvider::Writable,
                                                          BuiltinAttributeProvider::Deletable,
                                                          point_access,
                                                          make_array_read_attribute<int8_t>,
                                                          make_array_write_attribute<int8_t>,
                                                          tag_component_topology_changed);

  static BuiltinCustomDataLayerProvider handle_type_left("handle_type_left",
                                                         ATTR_DOMAIN_POINT,
                                                         CD_PROP_INT8,
                                                         CD_PROP_INT8,
                                                         BuiltinAttributeProvider::Creatable,
                                                         BuiltinAttributeProvider::Writable,
                                                         BuiltinAttributeProvider::Deletable,
                                                         point_access,
                                                         make_array_read_attribute<int8_t>,
                                                         make_array_write_attribute<int8_t>,
                                                         tag_component_topology_changed);

  static BuiltinCustomDataLayerProvider nurbs_weight("nurbs_weight",
                                                     ATTR_DOMAIN_POINT,
                                                     CD_PROP_FLOAT,
                                                     CD_PROP_FLOAT,
                                                     BuiltinAttributeProvider::Creatable,
                                                     BuiltinAttributeProvider::Writable,
                                                     BuiltinAttributeProvider::Deletable,
                                                     point_access,
                                                     make_array_read_attribute<float>,
                                                     make_array_write_attribute<float>,
                                                     tag_component_positions_changed);

  static BuiltinCustomDataLayerProvider nurbs_order("nurbs_order",
                                                    ATTR_DOMAIN_CURVE,
                                                    CD_PROP_INT8,
                                                    CD_PROP_INT8,
                                                    BuiltinAttributeProvider::Creatable,
                                                    BuiltinAttributeProvider::Writable,
                                                    BuiltinAttributeProvider::Deletable,
                                                    curve_access,
                                                    make_array_read_attribute<int8_t>,
                                                    make_array_write_attribute<int8_t>,
                                                    tag_component_topology_changed);

  static BuiltinCustomDataLayerProvider normal_mode("normal_mode",
                                                    ATTR_DOMAIN_CURVE,
                                                    CD_PROP_INT8,
                                                    CD_PROP_INT8,
                                                    BuiltinAttributeProvider::Creatable,
                                                    BuiltinAttributeProvider::Writable,
                                                    BuiltinAttributeProvider::Deletable,
                                                    curve_access,
                                                    make_array_read_attribute<int8_t>,
                                                    make_array_write_attribute<int8_t>,
                                                    tag_component_normals_changed);

  static BuiltinCustomDataLayerProvider nurbs_knots_mode("knots_mode",
                                                         ATTR_DOMAIN_CURVE,
                                                         CD_PROP_INT8,
                                                         CD_PROP_INT8,
                                                         BuiltinAttributeProvider::Creatable,
                                                         BuiltinAttributeProvider::Writable,
                                                         BuiltinAttributeProvider::Deletable,
                                                         curve_access,
                                                         make_array_read_attribute<int8_t>,
                                                         make_array_write_attribute<int8_t>,
                                                         tag_component_topology_changed);

  static BuiltinCustomDataLayerProvider curve_type("curve_type",
                                                   ATTR_DOMAIN_CURVE,
                                                   CD_PROP_INT8,
                                                   CD_PROP_INT8,
                                                   BuiltinAttributeProvider::Creatable,
                                                   BuiltinAttributeProvider::Writable,
                                                   BuiltinAttributeProvider::Deletable,
                                                   curve_access,
                                                   make_array_read_attribute<int8_t>,
                                                   make_array_write_attribute<int8_t>,
                                                   tag_component_curve_types_changed);

  static BuiltinCustomDataLayerProvider resolution("resolution",
                                                   ATTR_DOMAIN_CURVE,
                                                   CD_PROP_INT32,
                                                   CD_PROP_INT32,
                                                   BuiltinAttributeProvider::Creatable,
                                                   BuiltinAttributeProvider::Writable,
                                                   BuiltinAttributeProvider::Deletable,
                                                   curve_access,
                                                   make_array_read_attribute<int>,
                                                   make_array_write_attribute<int>,
                                                   tag_component_topology_changed);

  static BuiltinCustomDataLayerProvider cyclic("cyclic",
                                               ATTR_DOMAIN_CURVE,
                                               CD_PROP_BOOL,
                                               CD_PROP_BOOL,
                                               BuiltinAttributeProvider::Creatable,
                                               BuiltinAttributeProvider::Writable,
                                               BuiltinAttributeProvider::Deletable,
                                               curve_access,
                                               make_array_read_attribute<bool>,
                                               make_array_write_attribute<bool>,
                                               tag_component_topology_changed);

  static CustomDataAttributeProvider curve_custom_data(ATTR_DOMAIN_CURVE, curve_access);
  static CustomDataAttributeProvider point_custom_data(ATTR_DOMAIN_POINT, point_access);

  return ComponentAttributeProviders({&position,
                                      &radius,
                                      &id,
                                      &tilt,
                                      &handle_right,
                                      &handle_left,
                                      &handle_type_right,
                                      &handle_type_left,
                                      &normal_mode,
                                      &nurbs_order,
                                      &nurbs_knots_mode,
                                      &nurbs_weight,
                                      &curve_type,
                                      &resolution,
                                      &cyclic},
                                     {&curve_custom_data, &point_custom_data});
}

/** \} */

static AttributeAccessorFunctions get_curves_accessor_functions()
{
  static const ComponentAttributeProviders providers = create_attribute_providers_for_curve();
  AttributeAccessorFunctions fn =
      attribute_accessor_functions::accessor_functions_for_providers<providers>();
  fn.domain_size = [](const void *owner, const eAttrDomain domain) {
    if (owner == nullptr) {
      return 0;
    }
    const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
    switch (domain) {
      case ATTR_DOMAIN_POINT:
        return curves.points_num();
      case ATTR_DOMAIN_CURVE:
        return curves.curves_num();
      default:
        return 0;
    }
  };
  fn.domain_supported = [](const void *UNUSED(owner), const eAttrDomain domain) {
    return ELEM(domain, ATTR_DOMAIN_POINT, ATTR_DOMAIN_CURVE);
  };
  fn.adapt_domain = [](const void *owner,
                       const blender::GVArray &varray,
                       const eAttrDomain from_domain,
                       const eAttrDomain to_domain) -> GVArray {
    if (owner == nullptr) {
      return {};
    }
    const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
    return curves.adapt_domain(varray, from_domain, to_domain);
  };
  return fn;
}

static const AttributeAccessorFunctions &get_curves_accessor_functions_ref()
{
  static const AttributeAccessorFunctions fn = get_curves_accessor_functions();
  return fn;
}

AttributeAccessor CurvesGeometry::attributes() const
{
  return AttributeAccessor(this, get_curves_accessor_functions_ref());
}

MutableAttributeAccessor CurvesGeometry::attributes_for_write()
{
  return MutableAttributeAccessor(this, get_curves_accessor_functions_ref());
}

}  // namespace blender::bke

std::optional<blender::bke::AttributeAccessor> CurveComponent::attributes() const
{
  return blender::bke::AttributeAccessor(curves_ ? &curves_->geometry : nullptr,
                                         blender::bke::get_curves_accessor_functions_ref());
}

std::optional<blender::bke::MutableAttributeAccessor> CurveComponent::attributes_for_write()
{
  Curves *curves = this->get_for_write();
  return blender::bke::MutableAttributeAccessor(curves ? &curves->geometry : nullptr,
                                                blender::bke::get_curves_accessor_functions_ref());
}
