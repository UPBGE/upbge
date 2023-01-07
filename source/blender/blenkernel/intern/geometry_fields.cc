/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"
#include "BKE_mesh.h"
#include "BKE_pointcloud.h"
#include "BKE_type_conversions.hh"

#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "BLT_translation.h"

namespace blender::bke {

MeshFieldContext::MeshFieldContext(const Mesh &mesh, const eAttrDomain domain)
    : mesh_(mesh), domain_(domain)
{
  BLI_assert(mesh.attributes().domain_supported(domain_));
}

CurvesFieldContext::CurvesFieldContext(const CurvesGeometry &curves, const eAttrDomain domain)
    : curves_(curves), domain_(domain)
{
  BLI_assert(curves.attributes().domain_supported(domain));
}

GeometryFieldContext::GeometryFieldContext(const void *geometry,
                                           const GeometryComponentType type,
                                           const eAttrDomain domain)
    : geometry_(geometry), type_(type), domain_(domain)
{
  BLI_assert(ELEM(type,
                  GEO_COMPONENT_TYPE_MESH,
                  GEO_COMPONENT_TYPE_CURVE,
                  GEO_COMPONENT_TYPE_POINT_CLOUD,
                  GEO_COMPONENT_TYPE_INSTANCES));
}

GeometryFieldContext::GeometryFieldContext(const GeometryComponent &component,
                                           const eAttrDomain domain)
    : type_(component.type()), domain_(domain)
{
  switch (component.type()) {
    case GEO_COMPONENT_TYPE_MESH: {
      const MeshComponent &mesh_component = static_cast<const MeshComponent &>(component);
      geometry_ = mesh_component.get_for_read();
      break;
    }
    case GEO_COMPONENT_TYPE_CURVE: {
      const CurveComponent &curve_component = static_cast<const CurveComponent &>(component);
      const Curves *curves = curve_component.get_for_read();
      geometry_ = curves ? &CurvesGeometry::wrap(curves->geometry) : nullptr;
      break;
    }
    case GEO_COMPONENT_TYPE_POINT_CLOUD: {
      const PointCloudComponent &pointcloud_component = static_cast<const PointCloudComponent &>(
          component);
      geometry_ = pointcloud_component.get_for_read();
      break;
    }
    case GEO_COMPONENT_TYPE_INSTANCES: {
      const InstancesComponent &instances_component = static_cast<const InstancesComponent &>(
          component);
      geometry_ = instances_component.get_for_read();
      break;
    }
    case GEO_COMPONENT_TYPE_VOLUME:
    case GEO_COMPONENT_TYPE_EDIT:
      BLI_assert_unreachable();
      break;
  }
}

GeometryFieldContext::GeometryFieldContext(const Mesh &mesh, eAttrDomain domain)
    : geometry_(&mesh), type_(GEO_COMPONENT_TYPE_MESH), domain_(domain)
{
}
GeometryFieldContext::GeometryFieldContext(const CurvesGeometry &curves, eAttrDomain domain)
    : geometry_(&curves), type_(GEO_COMPONENT_TYPE_CURVE), domain_(domain)
{
}
GeometryFieldContext::GeometryFieldContext(const PointCloud &points)
    : geometry_(&points), type_(GEO_COMPONENT_TYPE_POINT_CLOUD), domain_(ATTR_DOMAIN_POINT)
{
}
GeometryFieldContext::GeometryFieldContext(const Instances &instances)
    : geometry_(&instances), type_(GEO_COMPONENT_TYPE_INSTANCES), domain_(ATTR_DOMAIN_INSTANCE)
{
}

std::optional<AttributeAccessor> GeometryFieldContext::attributes() const
{
  if (const Mesh *mesh = this->mesh()) {
    return mesh->attributes();
  }
  if (const CurvesGeometry *curves = this->curves()) {
    return curves->attributes();
  }
  if (const PointCloud *pointcloud = this->pointcloud()) {
    return pointcloud->attributes();
  }
  if (const Instances *instances = this->instances()) {
    return instances->attributes();
  }
  return {};
}

const Mesh *GeometryFieldContext::mesh() const
{
  return this->type() == GEO_COMPONENT_TYPE_MESH ? static_cast<const Mesh *>(geometry_) : nullptr;
}
const CurvesGeometry *GeometryFieldContext::curves() const
{
  return this->type() == GEO_COMPONENT_TYPE_CURVE ?
             static_cast<const CurvesGeometry *>(geometry_) :
             nullptr;
}
const PointCloud *GeometryFieldContext::pointcloud() const
{
  return this->type() == GEO_COMPONENT_TYPE_POINT_CLOUD ?
             static_cast<const PointCloud *>(geometry_) :
             nullptr;
}
const Instances *GeometryFieldContext::instances() const
{
  return this->type() == GEO_COMPONENT_TYPE_INSTANCES ? static_cast<const Instances *>(geometry_) :
                                                        nullptr;
}

GVArray GeometryFieldInput::get_varray_for_context(const fn::FieldContext &context,
                                                   const IndexMask mask,
                                                   ResourceScope & /*scope*/) const
{
  if (const GeometryFieldContext *geometry_context = dynamic_cast<const GeometryFieldContext *>(
          &context)) {
    return this->get_varray_for_context(*geometry_context, mask);
  }
  if (const MeshFieldContext *mesh_context = dynamic_cast<const MeshFieldContext *>(&context)) {
    return this->get_varray_for_context({mesh_context->mesh(), mesh_context->domain()}, mask);
  }
  if (const CurvesFieldContext *curve_context = dynamic_cast<const CurvesFieldContext *>(
          &context)) {
    return this->get_varray_for_context({curve_context->curves(), curve_context->domain()}, mask);
  }
  if (const PointCloudFieldContext *point_context = dynamic_cast<const PointCloudFieldContext *>(
          &context)) {
    return this->get_varray_for_context({point_context->pointcloud()}, mask);
  }
  if (const InstancesFieldContext *instances_context = dynamic_cast<const InstancesFieldContext *>(
          &context)) {
    return this->get_varray_for_context({instances_context->instances()}, mask);
  }
  return {};
}

std::optional<eAttrDomain> GeometryFieldInput::preferred_domain(
    const GeometryComponent & /*component*/) const
{
  return std::nullopt;
}

GVArray MeshFieldInput::get_varray_for_context(const fn::FieldContext &context,
                                               const IndexMask mask,
                                               ResourceScope & /*scope*/) const
{
  if (const GeometryFieldContext *geometry_context = dynamic_cast<const GeometryFieldContext *>(
          &context)) {
    if (const Mesh *mesh = geometry_context->mesh()) {
      return this->get_varray_for_context(*mesh, geometry_context->domain(), mask);
    }
  }
  if (const MeshFieldContext *mesh_context = dynamic_cast<const MeshFieldContext *>(&context)) {
    return this->get_varray_for_context(mesh_context->mesh(), mesh_context->domain(), mask);
  }
  return {};
}

std::optional<eAttrDomain> MeshFieldInput::preferred_domain(const Mesh & /*mesh*/) const
{
  return std::nullopt;
}

GVArray CurvesFieldInput::get_varray_for_context(const fn::FieldContext &context,
                                                 IndexMask mask,
                                                 ResourceScope & /*scope*/) const
{
  if (const GeometryFieldContext *geometry_context = dynamic_cast<const GeometryFieldContext *>(
          &context)) {
    if (const CurvesGeometry *curves = geometry_context->curves()) {
      return this->get_varray_for_context(*curves, geometry_context->domain(), mask);
    }
  }
  if (const CurvesFieldContext *curves_context = dynamic_cast<const CurvesFieldContext *>(
          &context)) {
    return this->get_varray_for_context(curves_context->curves(), curves_context->domain(), mask);
  }
  return {};
}

std::optional<eAttrDomain> CurvesFieldInput::preferred_domain(
    const CurvesGeometry & /*curves*/) const
{
  return std::nullopt;
}

GVArray PointCloudFieldInput::get_varray_for_context(const fn::FieldContext &context,
                                                     IndexMask mask,
                                                     ResourceScope & /*scope*/) const
{
  if (const GeometryFieldContext *geometry_context = dynamic_cast<const GeometryFieldContext *>(
          &context)) {
    if (const PointCloud *pointcloud = geometry_context->pointcloud()) {
      return this->get_varray_for_context(*pointcloud, mask);
    }
  }
  if (const PointCloudFieldContext *point_context = dynamic_cast<const PointCloudFieldContext *>(
          &context)) {
    return this->get_varray_for_context(point_context->pointcloud(), mask);
  }
  return {};
}

GVArray InstancesFieldInput::get_varray_for_context(const fn::FieldContext &context,
                                                    IndexMask mask,
                                                    ResourceScope & /*scope*/) const
{
  if (const GeometryFieldContext *geometry_context = dynamic_cast<const GeometryFieldContext *>(
          &context)) {
    if (const Instances *instances = geometry_context->instances()) {
      return this->get_varray_for_context(*instances, mask);
    }
  }
  if (const InstancesFieldContext *instances_context = dynamic_cast<const InstancesFieldContext *>(
          &context)) {
    return this->get_varray_for_context(instances_context->instances(), mask);
  }
  return {};
}

GVArray AttributeFieldInput::get_varray_for_context(const GeometryFieldContext &context,
                                                    const IndexMask /*mask*/) const
{
  const eCustomDataType data_type = cpp_type_to_custom_data_type(*type_);
  if (auto attributes = context.attributes()) {
    return attributes->lookup(name_, context.domain(), data_type);
  }
  return {};
}

std::string AttributeFieldInput::socket_inspection_name() const
{
  std::stringstream ss;
  ss << '"' << name_ << '"' << TIP_(" attribute from geometry");
  return ss.str();
}

uint64_t AttributeFieldInput::hash() const
{
  return get_default_hash_2(name_, type_);
}

bool AttributeFieldInput::is_equal_to(const fn::FieldNode &other) const
{
  if (const AttributeFieldInput *other_typed = dynamic_cast<const AttributeFieldInput *>(&other)) {
    return name_ == other_typed->name_ && type_ == other_typed->type_;
  }
  return false;
}

std::optional<eAttrDomain> AttributeFieldInput::preferred_domain(
    const GeometryComponent &component) const
{
  const std::optional<AttributeAccessor> attributes = component.attributes();
  if (!attributes.has_value()) {
    return std::nullopt;
  }
  const std::optional<AttributeMetaData> meta_data = attributes->lookup_meta_data(name_);
  if (!meta_data.has_value()) {
    return std::nullopt;
  }
  return meta_data->domain;
}

static StringRef get_random_id_attribute_name(const eAttrDomain domain)
{
  switch (domain) {
    case ATTR_DOMAIN_POINT:
    case ATTR_DOMAIN_INSTANCE:
      return "id";
    default:
      return "";
  }
}

GVArray IDAttributeFieldInput::get_varray_for_context(const GeometryFieldContext &context,
                                                      const IndexMask mask) const
{

  const StringRef name = get_random_id_attribute_name(context.domain());
  if (auto attributes = context.attributes()) {
    if (GVArray attribute = attributes->lookup(name, context.domain(), CD_PROP_INT32)) {
      return attribute;
    }
  }

  /* Use the index as the fallback if no random ID attribute exists. */
  return fn::IndexFieldInput::get_index_varray(mask);
}

std::string IDAttributeFieldInput::socket_inspection_name() const
{
  return TIP_("ID / Index");
}

uint64_t IDAttributeFieldInput::hash() const
{
  /* All random ID attribute inputs are the same within the same evaluation context. */
  return 92386459827;
}

bool IDAttributeFieldInput::is_equal_to(const fn::FieldNode &other) const
{
  /* All random ID attribute inputs are the same within the same evaluation context. */
  return dynamic_cast<const IDAttributeFieldInput *>(&other) != nullptr;
}

GVArray AnonymousAttributeFieldInput::get_varray_for_context(const GeometryFieldContext &context,
                                                             const IndexMask /*mask*/) const
{
  const eCustomDataType data_type = cpp_type_to_custom_data_type(*type_);
  return context.attributes()->lookup(*anonymous_id_, context.domain(), data_type);
}

std::string AnonymousAttributeFieldInput::socket_inspection_name() const
{
  std::stringstream ss;
  ss << '"' << debug_name_ << '"' << TIP_(" from ") << producer_name_;
  return ss.str();
}

uint64_t AnonymousAttributeFieldInput::hash() const
{
  return get_default_hash_2(anonymous_id_.get(), type_);
}

bool AnonymousAttributeFieldInput::is_equal_to(const fn::FieldNode &other) const
{
  if (const AnonymousAttributeFieldInput *other_typed =
          dynamic_cast<const AnonymousAttributeFieldInput *>(&other)) {
    return anonymous_id_.get() == other_typed->anonymous_id_.get() && type_ == other_typed->type_;
  }
  return false;
}

std::optional<eAttrDomain> AnonymousAttributeFieldInput::preferred_domain(
    const GeometryComponent &component) const
{
  const std::optional<AttributeAccessor> attributes = component.attributes();
  if (!attributes.has_value()) {
    return std::nullopt;
  }
  const std::optional<AttributeMetaData> meta_data = attributes->lookup_meta_data(*anonymous_id_);
  if (!meta_data.has_value()) {
    return std::nullopt;
  }
  return meta_data->domain;
}

}  // namespace blender::bke

/* -------------------------------------------------------------------- */
/** \name Mesh and Curve Normals Field Input
 * \{ */

namespace blender::bke {

GVArray NormalFieldInput::get_varray_for_context(const GeometryFieldContext &context,
                                                 const IndexMask mask) const
{
  if (const Mesh *mesh = context.mesh()) {
    return mesh_normals_varray(*mesh, mask, context.domain());
  }
  if (const CurvesGeometry *curves = context.curves()) {
    return curve_normals_varray(*curves, context.domain());
  }
  return {};
}

std::string NormalFieldInput::socket_inspection_name() const
{
  return TIP_("Normal");
}

uint64_t NormalFieldInput::hash() const
{
  return 213980475983;
}

bool NormalFieldInput::is_equal_to(const fn::FieldNode &other) const
{
  return dynamic_cast<const NormalFieldInput *>(&other) != nullptr;
}

bool try_capture_field_on_geometry(GeometryComponent &component,
                                   const AttributeIDRef &attribute_id,
                                   const eAttrDomain domain,
                                   const fn::GField &field)
{
  MutableAttributeAccessor attributes = *component.attributes_for_write();
  const int domain_size = attributes.domain_size(domain);
  const CPPType &type = field.cpp_type();
  const eCustomDataType data_type = bke::cpp_type_to_custom_data_type(type);

  if (domain_size == 0) {
    return attributes.add(attribute_id, domain, data_type, AttributeInitConstruct{});
  }

  bke::GeometryFieldContext field_context{component, domain};
  const IndexMask mask{IndexMask(domain_size)};
  const bke::AttributeValidator validator = attributes.lookup_validator(attribute_id);

  /* Could avoid allocating a new buffer if:
   * - We are writing to an attribute that exists already with the correct domain and type.
   * - The field does not depend on that attribute (we can't easily check for that yet). */
  void *buffer = MEM_mallocN(type.size() * domain_size, __func__);

  fn::FieldEvaluator evaluator{field_context, &mask};
  evaluator.add_with_destination(validator.validate_field_if_necessary(field),
                                 GMutableSpan{type, buffer, domain_size});
  evaluator.evaluate();

  const std::optional<AttributeMetaData> meta_data = attributes.lookup_meta_data(attribute_id);

  if (meta_data && meta_data->domain == domain && meta_data->data_type == data_type) {
    if (GAttributeWriter attribute = attributes.lookup_for_write(attribute_id)) {
      attribute.varray.set_all(buffer);
      attribute.finish();
      type.destruct_n(buffer, domain_size);
      MEM_freeN(buffer);
      return true;
    }
  }

  attributes.remove(attribute_id);
  if (attributes.add(attribute_id, domain, data_type, bke::AttributeInitMoveArray{buffer})) {
    return true;
  }

  /* If the name corresponds to a builtin attribute, removing the attribute might fail if
   * it's required, and adding the attribute might fail if the domain or type is incorrect. */
  type.destruct_n(buffer, domain_size);
  MEM_freeN(buffer);
  return false;
}

std::optional<eAttrDomain> try_detect_field_domain(const GeometryComponent &component,
                                                   const fn::GField &field)
{
  const GeometryComponentType component_type = component.type();
  if (component_type == GEO_COMPONENT_TYPE_POINT_CLOUD) {
    return ATTR_DOMAIN_POINT;
  }
  if (component_type == GEO_COMPONENT_TYPE_INSTANCES) {
    return ATTR_DOMAIN_INSTANCE;
  }
  const std::shared_ptr<const fn::FieldInputs> &field_inputs = field.node().field_inputs();
  if (!field_inputs) {
    return std::nullopt;
  }
  std::optional<eAttrDomain> output_domain;
  auto handle_domain = [&](const std::optional<eAttrDomain> domain) {
    if (!domain.has_value()) {
      return false;
    }
    if (output_domain.has_value()) {
      if (*output_domain != *domain) {
        return false;
      }
      return true;
    }
    output_domain = domain;
    return true;
  };
  if (component_type == GEO_COMPONENT_TYPE_MESH) {
    const MeshComponent &mesh_component = static_cast<const MeshComponent &>(component);
    const Mesh *mesh = mesh_component.get_for_read();
    if (mesh == nullptr) {
      return std::nullopt;
    }
    for (const fn::FieldInput &field_input : field_inputs->deduplicated_nodes) {
      if (const auto *geometry_field_input = dynamic_cast<const GeometryFieldInput *>(
              &field_input)) {
        if (!handle_domain(geometry_field_input->preferred_domain(component))) {
          return std::nullopt;
        }
      }
      else if (const auto *mesh_field_input = dynamic_cast<const MeshFieldInput *>(&field_input)) {
        if (!handle_domain(mesh_field_input->preferred_domain(*mesh))) {
          return std::nullopt;
        }
      }
      else {
        return std::nullopt;
      }
    }
  }
  if (component_type == GEO_COMPONENT_TYPE_CURVE) {
    const CurveComponent &curve_component = static_cast<const CurveComponent &>(component);
    const Curves *curves = curve_component.get_for_read();
    if (curves == nullptr) {
      return std::nullopt;
    }
    for (const fn::FieldInput &field_input : field_inputs->deduplicated_nodes) {
      if (const auto *geometry_field_input = dynamic_cast<const GeometryFieldInput *>(
              &field_input)) {
        if (!handle_domain(geometry_field_input->preferred_domain(component))) {
          return std::nullopt;
        }
      }
      else if (const auto *curves_field_input = dynamic_cast<const CurvesFieldInput *>(
                   &field_input)) {
        if (!handle_domain(
                curves_field_input->preferred_domain(CurvesGeometry::wrap(curves->geometry)))) {
          return std::nullopt;
        }
      }
      else {
        return std::nullopt;
      }
    }
  }
  return output_domain;
}

}  // namespace blender::bke

/** \} */
