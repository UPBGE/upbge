/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_collection.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"

#include "DNA_collection_types.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"

#include "DEG_depsgraph_query.hh"

namespace blender::bke {

static void add_final_mesh_as_geometry_component(const Object &object,
                                                 GeometrySet &geometry_set,
                                                 const bool apply_subdiv)
{
  if (apply_subdiv) {
    Mesh *mesh = BKE_modifier_get_evaluated_mesh_from_evaluated_object(
        &const_cast<Object &>(object));

    if (mesh != nullptr) {
      BKE_mesh_wrapper_ensure_mdata(mesh);
      geometry_set.replace_mesh(mesh, GeometryOwnershipType::ReadOnly);
    }
    return;
  }
  Mesh *mesh = BKE_object_get_evaluated_mesh_no_subsurf(&object);
  if (mesh != nullptr) {
    geometry_set.replace_mesh(mesh, GeometryOwnershipType::ReadOnly);
  }
}

GeometrySet object_get_evaluated_geometry_set(const Object &object, const bool apply_subdiv)
{
  if (!DEG_object_geometry_is_evaluated(object)) {
    return {};
  }

  if (object.runtime->geometry_set_eval != nullptr) {
    GeometrySet geometry_set = *object.runtime->geometry_set_eval;
    /* Ensure that subdivision is performed on the CPU. */
    if (geometry_set.has_mesh()) {
      add_final_mesh_as_geometry_component(object, geometry_set, apply_subdiv);
    }
    return geometry_set;
  }

  /* Otherwise, construct a new geometry set with the component based on the object type. */
  if (object.type == OB_MESH) {
    GeometrySet geometry_set;
    add_final_mesh_as_geometry_component(object, geometry_set, apply_subdiv);
    return geometry_set;
  }
  if (object.type == OB_EMPTY && object.instance_collection != nullptr) {
    Collection &collection = *object.instance_collection;
    std::unique_ptr<Instances> instances = std::make_unique<Instances>();
    const int handle = instances->add_reference(collection);
    instances->add_instance(handle, float4x4::identity());
    return GeometrySet::from_instances(instances.release());
  }

  /* Return by value since there is not always an existing geometry set owned elsewhere to use. */
  return {};
}

void Instances::foreach_referenced_geometry(
    FunctionRef<void(const GeometrySet &geometry_set)> callback) const
{
  for (const InstanceReference &reference : references_) {
    switch (reference.type()) {
      case InstanceReference::Type::Object: {
        const Object &object = reference.object();
        const GeometrySet object_geometry_set = object_get_evaluated_geometry_set(object);
        callback(object_geometry_set);
        break;
      }
      case InstanceReference::Type::Collection: {
        Collection &collection = reference.collection();
        FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (&collection, object) {
          const GeometrySet object_geometry_set = object_get_evaluated_geometry_set(*object);
          callback(object_geometry_set);
        }
        FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
        break;
      }
      case InstanceReference::Type::GeometrySet: {
        const GeometrySet &instance_geometry_set = reference.geometry_set();
        callback(instance_geometry_set);
        break;
      }
      case InstanceReference::Type::None: {
        break;
      }
    }
  }
}

void Instances::ensure_geometry_instances()
{
  Vector<InstanceReference> new_references;
  new_references.reserve(references_.size());
  for (const InstanceReference &reference : references_) {
    switch (reference.type()) {
      case InstanceReference::Type::None: {
        new_references.append(InstanceReference(GeometrySet{}));
        break;
      }
      case InstanceReference::Type::GeometrySet: {
        /* Those references can stay as their were. */
        new_references.append(reference);
        break;
      }
      case InstanceReference::Type::Object: {
        /* Create a new reference that contains the geometry set of the object. We may want to
         * treat e.g. lamps and similar object types separately here. */
        Object &object = reference.object();
        if (ELEM(object.type, OB_LAMP, OB_CAMERA, OB_SPEAKER, OB_ARMATURE)) {
          new_references.append(InstanceReference(object));
          break;
        }
        GeometrySet object_geometry_set = object_get_evaluated_geometry_set(object);
        object_geometry_set.name = BKE_id_name(object.id);
        if (object_geometry_set.has_instances()) {
          object_geometry_set.get_instances_for_write()->ensure_geometry_instances();
        }
        new_references.append(std::move(object_geometry_set));
        break;
      }
      case InstanceReference::Type::Collection: {
        /* Create a new reference that contains a geometry set that contains all objects from the
         * collection as instances. */
        std::unique_ptr<Instances> instances = std::make_unique<Instances>();
        Collection &collection = reference.collection();

        Vector<Object *, 8> objects;
        FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (&collection, object) {
          objects.append(object);
        }
        FOREACH_COLLECTION_OBJECT_RECURSIVE_END;

        instances->resize(objects.size());
        MutableSpan<int> handles = instances->reference_handles_for_write();
        MutableSpan<float4x4> transforms = instances->transforms_for_write();
        for (const int i : objects.index_range()) {
          handles[i] = instances->add_reference(*objects[i]);
          transforms[i] = objects[i]->object_to_world();
          transforms[i].location() -= collection.instance_offset;
        }
        instances->ensure_geometry_instances();
        GeometrySet geometry_set = GeometrySet::from_instances(instances.release());
        geometry_set.name = BKE_id_name(collection.id);
        new_references.append(std::move(geometry_set));
        break;
      }
    }
  }
  references_ = std::move(new_references);
}

}  // namespace blender::bke
