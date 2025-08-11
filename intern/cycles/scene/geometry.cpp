/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "bvh/bvh.h"

#include "device/device.h"

#include "scene/attribute.h"
#include "scene/camera.h"
#include "scene/geometry.h"
#include "scene/hair.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/osl.h"
#include "scene/pointcloud.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_nodes.h"
#include "scene/stats.h"
#include "scene/volume.h"

#include "subd/split.h"

#ifdef WITH_OSL
#  include "kernel/osl/globals.h"
#endif

#include "util/log.h"
#include "util/progress.h"
#include "util/task.h"

CCL_NAMESPACE_BEGIN

/* Geometry */

NODE_ABSTRACT_DEFINE(Geometry)
{
  NodeType *type = NodeType::add("geometry_base", nullptr);

  SOCKET_UINT(motion_steps, "Motion Steps", 0);
  SOCKET_BOOLEAN(use_motion_blur, "Use Motion Blur", false);
  SOCKET_NODE_ARRAY(used_shaders, "Shaders", Shader::get_node_type());

  return type;
}

Geometry::Geometry(const NodeType *node_type, const Type type)
    : Node(node_type), geometry_type(type), attributes(this, ATTR_PRIM_GEOMETRY)
{
  need_update_rebuild = false;
  need_update_bvh_for_offset = false;

  transform_applied = false;
  transform_negative_scaled = false;
  transform_normal = transform_identity();
  bounds = BoundBox::empty;

  has_volume = false;
  has_surface_bssrdf = false;

  attr_map_offset = 0;
  prim_offset = 0;
}

Geometry::~Geometry()
{
  dereference_all_used_nodes();
}

void Geometry::clear(bool preserve_shaders)
{
  if (!preserve_shaders) {
    used_shaders.clear();
  }

  transform_applied = false;
  transform_negative_scaled = false;
  transform_normal = transform_identity();
  tag_modified();
}

float Geometry::motion_time(const int step) const
{
  return (motion_steps > 1) ? 2.0f * step / (motion_steps - 1) - 1.0f : 0.0f;
}

int Geometry::motion_step(const float time) const
{
  if (motion_steps > 1) {
    int attr_step = 0;

    for (int step = 0; step < motion_steps; step++) {
      const float step_time = motion_time(step);
      if (step_time == time) {
        return attr_step;
      }

      /* Center step is stored in a separate attribute. */
      if (step != motion_steps / 2) {
        attr_step++;
      }
    }
  }

  return -1;
}

bool Geometry::need_build_bvh(BVHLayout layout) const
{
  return is_instanced() || layout == BVH_LAYOUT_OPTIX || layout == BVH_LAYOUT_MULTI_OPTIX ||
         layout == BVH_LAYOUT_METAL || layout == BVH_LAYOUT_MULTI_OPTIX_EMBREE ||
         layout == BVH_LAYOUT_MULTI_METAL || layout == BVH_LAYOUT_MULTI_METAL_EMBREE ||
         layout == BVH_LAYOUT_HIPRT || layout == BVH_LAYOUT_MULTI_HIPRT ||
         layout == BVH_LAYOUT_MULTI_HIPRT_EMBREE || layout == BVH_LAYOUT_EMBREEGPU ||
         layout == BVH_LAYOUT_MULTI_EMBREEGPU || layout == BVH_LAYOUT_MULTI_EMBREEGPU_EMBREE;
}

bool Geometry::is_instanced() const
{
  /* Currently we treat subsurface objects as instanced.
   *
   * While it might be not very optimal for ray traversal, it avoids having
   * duplicated BVH in the memory, saving quite some space.
   */
  return !transform_applied || has_surface_bssrdf;
}

bool Geometry::has_true_displacement() const
{
  for (Node *node : used_shaders) {
    Shader *shader = static_cast<Shader *>(node);
    if (shader->has_displacement && shader->get_displacement_method() != DISPLACE_BUMP) {
      return true;
    }
  }

  return false;
}

bool Geometry::has_motion_blur() const
{
  return (use_motion_blur && attributes.find(ATTR_STD_MOTION_VERTEX_POSITION));
}

void Geometry::tag_update(Scene *scene, bool rebuild)
{
  if (rebuild) {
    need_update_rebuild = true;
    scene->light_manager->tag_update(scene, LightManager::MESH_NEED_REBUILD);
  }
  else {
    for (Node *node : used_shaders) {
      Shader *shader = static_cast<Shader *>(node);
      if (shader->emission_sampling != EMISSION_SAMPLING_NONE) {
        scene->light_manager->tag_update(scene, LightManager::EMISSIVE_MESH_MODIFIED);
        break;
      }
    }
  }

  scene->geometry_manager->tag_update(scene, GeometryManager::GEOMETRY_MODIFIED);
}

/* Geometry Manager */

GeometryManager::GeometryManager()
{
  update_flags = UPDATE_ALL;
  need_flags_update = true;
}

GeometryManager::~GeometryManager() = default;

void GeometryManager::update_osl_globals(Device *device, Scene *scene)
{
#ifdef WITH_OSL
  OSLGlobals *og = device->get_cpu_osl_memory();
  if (og == nullptr) {
    /* Can happen when rendering with multiple GPUs, but no CPU (in which case the name maps filled
     * below are not used anyway) */
    return;
  }

  og->object_name_map.clear();
  og->object_names.clear();

  for (size_t i = 0; i < scene->objects.size(); i++) {
    /* set object name to object index map */
    Object *object = scene->objects[i];
    og->object_name_map[object->name] = i;
    og->object_names.push_back(object->name);
  }
#else
  (void)device;
  (void)scene;
#endif
}

static void update_device_flags_attribute(uint32_t &device_update_flags,
                                          const AttributeSet &attributes)
{
  for (const Attribute &attr : attributes.attributes) {
    if (!attr.modified) {
      continue;
    }

    const AttrKernelDataType kernel_type = Attribute::kernel_type(attr);

    switch (kernel_type) {
      case AttrKernelDataType::FLOAT: {
        device_update_flags |= ATTR_FLOAT_MODIFIED;
        break;
      }
      case AttrKernelDataType::FLOAT2: {
        device_update_flags |= ATTR_FLOAT2_MODIFIED;
        break;
      }
      case AttrKernelDataType::FLOAT3: {
        device_update_flags |= ATTR_FLOAT3_MODIFIED;
        break;
      }
      case AttrKernelDataType::FLOAT4: {
        device_update_flags |= ATTR_FLOAT4_MODIFIED;
        break;
      }
      case AttrKernelDataType::UCHAR4: {
        device_update_flags |= ATTR_UCHAR4_MODIFIED;
        break;
      }
      case AttrKernelDataType::NUM: {
        break;
      }
    }
  }
}

static void update_attribute_realloc_flags(uint32_t &device_update_flags,
                                           const AttributeSet &attributes)
{
  if (attributes.modified(AttrKernelDataType::FLOAT)) {
    device_update_flags |= ATTR_FLOAT_NEEDS_REALLOC;
  }
  if (attributes.modified(AttrKernelDataType::FLOAT2)) {
    device_update_flags |= ATTR_FLOAT2_NEEDS_REALLOC;
  }
  if (attributes.modified(AttrKernelDataType::FLOAT3)) {
    device_update_flags |= ATTR_FLOAT3_NEEDS_REALLOC;
  }
  if (attributes.modified(AttrKernelDataType::FLOAT4)) {
    device_update_flags |= ATTR_FLOAT4_NEEDS_REALLOC;
  }
  if (attributes.modified(AttrKernelDataType::UCHAR4)) {
    device_update_flags |= ATTR_UCHAR4_NEEDS_REALLOC;
  }
}

void GeometryManager::geom_calc_offset(Scene *scene, BVHLayout bvh_layout)
{
  size_t vert_size = 0;
  size_t tri_size = 0;

  size_t curve_size = 0;
  size_t curve_key_size = 0;
  size_t curve_segment_size = 0;

  size_t point_size = 0;

  size_t face_size = 0;
  size_t corner_size = 0;

  for (Geometry *geom : scene->geometry) {
    bool prim_offset_changed = false;

    if (geom->is_mesh() || geom->is_volume()) {
      Mesh *mesh = static_cast<Mesh *>(geom);

      prim_offset_changed = (mesh->prim_offset != tri_size);

      mesh->vert_offset = vert_size;
      mesh->prim_offset = tri_size;

      mesh->face_offset = face_size;
      mesh->corner_offset = corner_size;

      vert_size += mesh->verts.size();
      tri_size += mesh->num_triangles();

      face_size += mesh->get_num_subd_faces();
      corner_size += mesh->subd_face_corners.size();
    }
    else if (geom->is_hair()) {
      Hair *hair = static_cast<Hair *>(geom);

      prim_offset_changed = (hair->curve_segment_offset != curve_segment_size);
      hair->curve_key_offset = curve_key_size;
      hair->curve_segment_offset = curve_segment_size;
      hair->prim_offset = curve_size;

      curve_size += hair->num_curves();
      curve_key_size += hair->get_curve_keys().size();
      curve_segment_size += hair->num_segments();
    }
    else if (geom->is_pointcloud()) {
      PointCloud *pointcloud = static_cast<PointCloud *>(geom);

      prim_offset_changed = (pointcloud->prim_offset != point_size);

      pointcloud->prim_offset = point_size;
      point_size += pointcloud->num_points();
    }

    if (prim_offset_changed) {
      /* Need to rebuild BVH in OptiX, since refit only allows modified mesh data.
       * Metal has optimization for static BVH, that also require a rebuild. */
      const bool need_update_rebuild = (bvh_layout == BVH_LAYOUT_OPTIX ||
                                        bvh_layout == BVH_LAYOUT_MULTI_OPTIX ||
                                        bvh_layout == BVH_LAYOUT_MULTI_OPTIX_EMBREE) ||
                                       ((bvh_layout == BVH_LAYOUT_METAL ||
                                         bvh_layout == BVH_LAYOUT_MULTI_METAL ||
                                         bvh_layout == BVH_LAYOUT_MULTI_METAL_EMBREE) &&
                                        scene->params.bvh_type == BVH_TYPE_STATIC);
      geom->need_update_rebuild |= need_update_rebuild;
      geom->need_update_bvh_for_offset = true;
    }
  }
}

void GeometryManager::device_update_preprocess(Device *device, Scene *scene, Progress &progress)
{
  if (!need_update() && !need_flags_update) {
    return;
  }

  uint32_t device_update_flags = 0;

  const scoped_callback_timer timer([scene](double time) {
    if (scene->update_stats) {
      scene->update_stats->geometry.times.add_entry({"device_update_preprocess", time});
    }
  });

  progress.set_status("Updating Meshes Flags");

  /* Update flags. */
  bool volume_images_updated = false;

  for (Geometry *geom : scene->geometry) {
    geom->has_volume = false;

    update_attribute_realloc_flags(device_update_flags, geom->attributes);

    if (geom->is_mesh()) {
      Mesh *mesh = static_cast<Mesh *>(geom);
      update_attribute_realloc_flags(device_update_flags, mesh->subd_attributes);
    }

    for (Node *node : geom->get_used_shaders()) {
      Shader *shader = static_cast<Shader *>(node);
      if (shader->has_volume) {
        geom->has_volume = true;
      }

      if (shader->has_surface_bssrdf) {
        geom->has_surface_bssrdf = true;
      }

      if (shader->need_update_uvs) {
        device_update_flags |= ATTR_FLOAT2_NEEDS_REALLOC;

        /* Attributes might need to be tessellated if added. */
        if (geom->is_mesh()) {
          Mesh *mesh = static_cast<Mesh *>(geom);
          if (mesh->need_tesselation()) {
            mesh->tag_modified();
          }
        }
      }

      if (shader->need_update_attribute) {
        device_update_flags |= ATTRS_NEED_REALLOC;

        /* Attributes might need to be tessellated if added. */
        if (geom->is_mesh()) {
          Mesh *mesh = static_cast<Mesh *>(geom);
          if (mesh->need_tesselation()) {
            mesh->tag_modified();
          }
        }
      }

      if (shader->need_update_displacement) {
        /* tag displacement related sockets as modified */
        if (geom->is_mesh()) {
          Mesh *mesh = static_cast<Mesh *>(geom);
          mesh->tag_verts_modified();
          mesh->tag_subd_dicing_rate_modified();
          mesh->tag_subd_max_level_modified();
          mesh->tag_subd_objecttoworld_modified();

          device_update_flags |= ATTRS_NEED_REALLOC;
        }
      }
    }

    /* only check for modified attributes if we do not need to reallocate them already */
    if ((device_update_flags & ATTRS_NEED_REALLOC) == 0) {
      update_device_flags_attribute(device_update_flags, geom->attributes);
      /* don't check for subd_attributes, as if they were modified, we would need to reallocate
       * anyway */
    }

    /* Re-create volume mesh if we will rebuild or refit the BVH. Note we
     * should only do it in that case, otherwise the BVH and mesh can go
     * out of sync. */
    if (geom->is_modified() && geom->is_volume()) {
      /* Create volume meshes if there is voxel data. */
      if (!volume_images_updated) {
        progress.set_status("Updating Meshes Volume Bounds");
        device_update_volume_images(device, scene, progress);
        volume_images_updated = true;
      }

      Volume *volume = static_cast<Volume *>(geom);
      create_volume_mesh(scene, volume, progress);

      /* always reallocate when we have a volume, as we need to rebuild the BVH */
      device_update_flags |= DEVICE_MESH_DATA_NEEDS_REALLOC;
    }

    if (geom->is_hair()) {
      Hair *hair = static_cast<Hair *>(geom);

      if (hair->need_update_rebuild) {
        device_update_flags |= DEVICE_CURVE_DATA_NEEDS_REALLOC;
      }
      else if (hair->is_modified()) {
        device_update_flags |= DEVICE_CURVE_DATA_MODIFIED;
      }
    }

    if (geom->is_mesh()) {
      Mesh *mesh = static_cast<Mesh *>(geom);

      if (mesh->need_update_rebuild) {
        device_update_flags |= DEVICE_MESH_DATA_NEEDS_REALLOC;
      }
      else if (mesh->is_modified()) {
        device_update_flags |= DEVICE_MESH_DATA_MODIFIED;
      }
    }

    if (geom->is_pointcloud()) {
      PointCloud *pointcloud = static_cast<PointCloud *>(geom);

      if (pointcloud->need_update_rebuild) {
        device_update_flags |= DEVICE_POINT_DATA_NEEDS_REALLOC;
      }
      else if (pointcloud->is_modified()) {
        device_update_flags |= DEVICE_POINT_DATA_MODIFIED;
      }
    }
  }

  if (update_flags & (MESH_ADDED | MESH_REMOVED)) {
    device_update_flags |= DEVICE_MESH_DATA_NEEDS_REALLOC;
  }

  if (update_flags & (HAIR_ADDED | HAIR_REMOVED)) {
    device_update_flags |= DEVICE_CURVE_DATA_NEEDS_REALLOC;
  }

  if (update_flags & (POINT_ADDED | POINT_REMOVED)) {
    device_update_flags |= DEVICE_POINT_DATA_NEEDS_REALLOC;
  }

  /* tag the device arrays for reallocation or modification */
  DeviceScene *dscene = &scene->dscene;

  if (device_update_flags & (DEVICE_MESH_DATA_NEEDS_REALLOC | DEVICE_CURVE_DATA_NEEDS_REALLOC |
                             DEVICE_POINT_DATA_NEEDS_REALLOC))
  {
    scene->bvh.reset();

    dscene->bvh_nodes.tag_realloc();
    dscene->bvh_leaf_nodes.tag_realloc();
    dscene->object_node.tag_realloc();
    dscene->prim_type.tag_realloc();
    dscene->prim_visibility.tag_realloc();
    dscene->prim_index.tag_realloc();
    dscene->prim_object.tag_realloc();
    dscene->prim_time.tag_realloc();

    if (device_update_flags & DEVICE_MESH_DATA_NEEDS_REALLOC) {
      dscene->tri_verts.tag_realloc();
      dscene->tri_vnormal.tag_realloc();
      dscene->tri_vindex.tag_realloc();
      dscene->tri_shader.tag_realloc();
    }

    if (device_update_flags & DEVICE_CURVE_DATA_NEEDS_REALLOC) {
      dscene->curves.tag_realloc();
      dscene->curve_keys.tag_realloc();
      dscene->curve_segments.tag_realloc();
    }

    if (device_update_flags & DEVICE_POINT_DATA_NEEDS_REALLOC) {
      dscene->points.tag_realloc();
      dscene->points_shader.tag_realloc();
    }
  }

  if ((update_flags & VISIBILITY_MODIFIED) != 0) {
    dscene->prim_visibility.tag_modified();
  }

  if (device_update_flags & ATTR_FLOAT_NEEDS_REALLOC) {
    dscene->attributes_map.tag_realloc();
    dscene->attributes_float.tag_realloc();
  }
  else if (device_update_flags & ATTR_FLOAT_MODIFIED) {
    dscene->attributes_float.tag_modified();
  }

  if (device_update_flags & ATTR_FLOAT2_NEEDS_REALLOC) {
    dscene->attributes_map.tag_realloc();
    dscene->attributes_float2.tag_realloc();
  }
  else if (device_update_flags & ATTR_FLOAT2_MODIFIED) {
    dscene->attributes_float2.tag_modified();
  }

  if (device_update_flags & ATTR_FLOAT3_NEEDS_REALLOC) {
    dscene->attributes_map.tag_realloc();
    dscene->attributes_float3.tag_realloc();
  }
  else if (device_update_flags & ATTR_FLOAT3_MODIFIED) {
    dscene->attributes_float3.tag_modified();
  }

  if (device_update_flags & ATTR_FLOAT4_NEEDS_REALLOC) {
    dscene->attributes_map.tag_realloc();
    dscene->attributes_float4.tag_realloc();
  }
  else if (device_update_flags & ATTR_FLOAT4_MODIFIED) {
    dscene->attributes_float4.tag_modified();
  }

  if (device_update_flags & ATTR_UCHAR4_NEEDS_REALLOC) {
    dscene->attributes_map.tag_realloc();
    dscene->attributes_uchar4.tag_realloc();
  }
  else if (device_update_flags & ATTR_UCHAR4_MODIFIED) {
    dscene->attributes_uchar4.tag_modified();
  }

  if (device_update_flags & DEVICE_MESH_DATA_MODIFIED) {
    /* if anything else than vertices or shaders are modified, we would need to reallocate, so
     * these are the only arrays that can be updated */
    dscene->tri_verts.tag_modified();
    dscene->tri_vnormal.tag_modified();
    dscene->tri_shader.tag_modified();
  }

  if (device_update_flags & DEVICE_CURVE_DATA_MODIFIED) {
    dscene->curve_keys.tag_modified();
    dscene->curves.tag_modified();
    dscene->curve_segments.tag_modified();
  }

  if (device_update_flags & DEVICE_POINT_DATA_MODIFIED) {
    dscene->points.tag_modified();
    dscene->points_shader.tag_modified();
  }

  need_flags_update = false;
}

void GeometryManager::device_update_displacement_images(Device *device,
                                                        Scene *scene,
                                                        Progress &progress)
{
  progress.set_status("Updating Displacement Images");
  TaskPool pool;
  ImageManager *image_manager = scene->image_manager.get();
  set<int> bump_images;
#ifdef WITH_OSL
  bool has_osl_node = false;
#endif
  for (Geometry *geom : scene->geometry) {
    if (geom->is_modified()) {
      /* Geometry-level check for hair shadow transparency.
       * This matches the logic in the `Hair::update_shadow_transparency()`, avoiding access to
       * possible non-loaded images. */
      bool need_shadow_transparency = false;
      if (geom->is_hair()) {
        Hair *hair = static_cast<Hair *>(geom);
        need_shadow_transparency = hair->need_shadow_transparency();
      }

      for (Node *node : geom->get_used_shaders()) {
        Shader *shader = static_cast<Shader *>(node);
        const bool is_true_displacement = (shader->has_displacement &&
                                           shader->get_displacement_method() != DISPLACE_BUMP);
        if (!is_true_displacement && !need_shadow_transparency) {
          continue;
        }
        for (ShaderNode *node : shader->graph->nodes) {
#ifdef WITH_OSL
          if (node->special_type == SHADER_SPECIAL_TYPE_OSL) {
            has_osl_node = true;
          }
#endif
          if (node->special_type != SHADER_SPECIAL_TYPE_IMAGE_SLOT) {
            continue;
          }

          ImageSlotTextureNode *image_node = static_cast<ImageSlotTextureNode *>(node);
          for (int i = 0; i < image_node->handle.num_svm_slots(); i++) {
            const int slot = image_node->handle.svm_slot(i);
            if (slot != -1) {
              bump_images.insert(slot);
            }
          }
        }
      }
    }
  }

#ifdef WITH_OSL
  /* If any OSL node is used for displacement, it may reference a texture. But it's
   * unknown which ones, so have to load them all. */
  if (has_osl_node) {
    OSLShaderManager::osl_image_slots(device, image_manager, bump_images);
  }
#endif

  for (const int slot : bump_images) {
    pool.push([image_manager, device, scene, slot, &progress] {
      image_manager->device_update_slot(device, scene, slot, progress);
    });
  }
  pool.wait_work();
}

void GeometryManager::device_update_volume_images(Device *device, Scene *scene, Progress &progress)
{
  progress.set_status("Updating Volume Images");
  TaskPool pool;
  ImageManager *image_manager = scene->image_manager.get();
  set<int> volume_images;

  for (Geometry *geom : scene->geometry) {
    if (!geom->is_modified()) {
      continue;
    }

    for (Attribute &attr : geom->attributes.attributes) {
      if (attr.element != ATTR_ELEMENT_VOXEL) {
        continue;
      }

      const ImageHandle &handle = attr.data_voxel();
      const int slot = handle.svm_slot();
      if (slot != -1) {
        volume_images.insert(slot);
      }
    }
  }

  for (const int slot : volume_images) {
    pool.push([image_manager, device, scene, slot, &progress] {
      image_manager->device_update_slot(device, scene, slot, progress);
    });
  }
  pool.wait_work();
}

void GeometryManager::device_update(Device *device,
                                    DeviceScene *dscene,
                                    Scene *scene,
                                    Progress &progress)
{
  if (!need_update()) {
    return;
  }

  LOG_INFO << "Total " << scene->geometry.size() << " meshes.";

  bool true_displacement_used = false;
  bool curve_shadow_transparency_used = false;
  size_t num_tessellation = 0;

  {
    const scoped_callback_timer timer([scene](double time) {
      if (scene->update_stats) {
        scene->update_stats->geometry.times.add_entry({"device_update (normals)", time});
      }
    });

    for (Geometry *geom : scene->geometry) {
      if (geom->is_modified()) {
        if (geom->is_mesh() || geom->is_volume()) {
          Mesh *mesh = static_cast<Mesh *>(geom);

          /* Test if we need tessellation and setup normals if required. */
          if (mesh->need_tesselation()) {
            num_tessellation++;
            /* OPENSUBDIV Catmull-Clark does not make use of input normals and will overwrite them.
             */
#ifdef WITH_OPENSUBDIV
            if (mesh->get_subdivision_type() != Mesh::SUBDIVISION_CATMULL_CLARK)
#endif
            {
              mesh->add_vertex_normals();
            }
          }
          else {
            mesh->add_vertex_normals();
          }

          /* Test if we need displacement. */
          if (mesh->has_true_displacement()) {
            true_displacement_used = true;
          }
        }
        else if (geom->is_hair()) {
          Hair *hair = static_cast<Hair *>(geom);
          if (hair->need_shadow_transparency()) {
            curve_shadow_transparency_used = true;
          }
        }

        if (progress.get_cancel()) {
          return;
        }
      }
    }
  }

  if (progress.get_cancel()) {
    return;
  }

  /* Tessellate meshes that are using subdivision */
  const scoped_callback_timer timer([scene, num_tessellation](double time) {
    if (scene->update_stats) {
      scene->update_stats->geometry.times.add_entry(
          {(num_tessellation) ? "device_update (tessellation and tangents)" :
                                "device_update (tangents)",
           time});
    }
  });

  Camera *dicing_camera = scene->dicing_camera;
  if (num_tessellation) {
    dicing_camera->set_screen_size(dicing_camera->get_full_width(),
                                   dicing_camera->get_full_height());
    dicing_camera->update(scene);
  }

  size_t i = 0;
  for (Geometry *geom : scene->geometry) {
    if (!(geom->is_modified() && geom->is_mesh())) {
      continue;
    }

    Mesh *mesh = static_cast<Mesh *>(geom);

    if (num_tessellation && mesh->need_tesselation()) {
      string msg = "Tessellating ";
      if (mesh->name.empty()) {
        msg += string_printf("%u/%u", (uint)(i + 1), (uint)num_tessellation);
      }
      else {
        msg += string_printf(
            "%s %u/%u", mesh->name.c_str(), (uint)(i + 1), (uint)num_tessellation);
      }

      progress.set_status("Updating Mesh", msg);

      SubdParams subd_params(mesh);
      subd_params.dicing_rate = mesh->get_subd_dicing_rate();
      subd_params.max_level = mesh->get_subd_max_level();
      subd_params.objecttoworld = mesh->get_subd_objecttoworld();
      subd_params.camera = dicing_camera;

      mesh->tessellate(subd_params);

      i++;
    }

    /* Apply generated attribute if needed or remove if not needed */
    mesh->update_generated(scene);
    /* Apply tangents for generated and UVs (if any need them) or remove if not needed */
    mesh->update_tangents(scene, true);
    if (!mesh->has_true_displacement()) {
      mesh->update_tangents(scene, false);
    }

    if (progress.get_cancel()) {
      return;
    }
  }

  if (progress.get_cancel()) {
    return;
  }

  /* Update images needed for true displacement. */
  if (true_displacement_used || curve_shadow_transparency_used) {
    const scoped_callback_timer timer([scene](double time) {
      if (scene->update_stats) {
        scene->update_stats->geometry.times.add_entry(
            {"device_update (displacement: load images)", time});
      }
    });
    device_update_displacement_images(device, scene, progress);
    scene->object_manager->device_update_flags(device, dscene, scene, progress, false);
  }

  /* Device update. */
  device_free(device, dscene, false);

  const BVHLayout bvh_layout = BVHParams::best_bvh_layout(
      scene->params.bvh_layout, device->get_bvh_layout_mask(dscene->data.kernel_features));
  geom_calc_offset(scene, bvh_layout);
  if (true_displacement_used || curve_shadow_transparency_used) {
    const scoped_callback_timer timer([scene](double time) {
      if (scene->update_stats) {
        scene->update_stats->geometry.times.add_entry(
            {"device_update (displacement: copy meshes to device)", time});
      }
    });
    device_update_mesh(device, dscene, scene, progress);
  }

  if (progress.get_cancel()) {
    return;
  }

  /* Apply transforms, to prepare for static BVH building. */
  if (scene->params.bvh_type == BVH_TYPE_STATIC) {
    const scoped_callback_timer timer([scene](double time) {
      if (scene->update_stats) {
        scene->update_stats->object.times.add_entry(
            {"device_update (apply static transforms)", time});
      }
    });

    progress.set_status("Updating Objects", "Applying Static Transformations");
    scene->object_manager->apply_static_transforms(dscene, scene, progress);
  }

  if (progress.get_cancel()) {
    return;
  }

  {
    const scoped_callback_timer timer([scene](double time) {
      if (scene->update_stats) {
        scene->update_stats->geometry.times.add_entry({"device_update (attributes)", time});
      }
    });
    device_update_attributes(device, dscene, scene, progress);
    if (progress.get_cancel()) {
      return;
    }
  }

  /* Update displacement and hair shadow transparency. */
  bool displacement_done = false;
  bool curve_shadow_transparency_done = false;

  {
    /* Copy constant data needed by shader evaluation. */
    device->const_copy_to("data", &dscene->data, sizeof(dscene->data));

    const scoped_callback_timer timer([scene](double time) {
      if (scene->update_stats) {
        scene->update_stats->geometry.times.add_entry({"device_update (displacement)", time});
      }
    });

    for (Geometry *geom : scene->geometry) {
      if (geom->is_modified()) {
        if (geom->is_mesh()) {
          Mesh *mesh = static_cast<Mesh *>(geom);
          if (displace(device, scene, mesh, progress)) {
            displacement_done = true;
          }
        }
        else if (geom->is_hair()) {
          Hair *hair = static_cast<Hair *>(geom);
          if (hair->update_shadow_transparency(device, scene, progress)) {
            curve_shadow_transparency_done = true;
          }
        }
      }

      if (progress.get_cancel()) {
        return;
      }
    }
  }

  if (progress.get_cancel()) {
    return;
  }

  /* Device re-update after applying transforms and displacement. */
  if (displacement_done || curve_shadow_transparency_done) {
    const scoped_callback_timer timer([scene](double time) {
      if (scene->update_stats) {
        scene->update_stats->geometry.times.add_entry(
            {"device_update (displacement: attributes)", time});
      }
    });
    device_free(device, dscene, false);

    device_update_attributes(device, dscene, scene, progress);
    if (progress.get_cancel()) {
      return;
    }
  }

  /* Update the BVH even when there is no geometry so the kernel's BVH data is still valid,
   * especially when removing all of the objects during interactive renders.
   * Also update the BVH if the transformations change, we cannot rely on tagging the Geometry
   * as modified in this case, as we may accumulate displacement if the vertices do not also
   * change. */
  bool need_update_scene_bvh = (scene->bvh == nullptr ||
                                (update_flags & (TRANSFORM_MODIFIED | VISIBILITY_MODIFIED)) != 0);
  {
    const scoped_callback_timer timer([scene](double time) {
      if (scene->update_stats) {
        scene->update_stats->geometry.times.add_entry({"device_update (build object BVHs)", time});
      }
    });
    TaskPool pool;

    /* Work around Embree/oneAPI bug #129596 with BVH updates. */
    const bool use_multithreaded_build = first_bvh_build ||
                                         !device->info.contains_device_type(DEVICE_ONEAPI);
    first_bvh_build = false;

    size_t i = 0;
    size_t num_bvh = 0;
    for (Geometry *geom : scene->geometry) {
      if (geom->is_modified() || geom->need_update_bvh_for_offset) {
        need_update_scene_bvh = true;

        if (geom->need_build_bvh(bvh_layout)) {
          i++;
          num_bvh++;
        }

        if (use_multithreaded_build) {
          pool.push([geom, device, dscene, scene, &progress, i, &num_bvh] {
            geom->compute_bvh(device, dscene, &scene->params, &progress, i, num_bvh);
          });
        }
        else {
          geom->compute_bvh(device, dscene, &scene->params, &progress, i, num_bvh);
        }
      }
    }

    TaskPool::Summary summary;
    pool.wait_work(&summary);
    LOG_WORK << "Objects BVH build pool statistics:\n" << summary.full_report();
  }

  for (Shader *shader : scene->shaders) {
    shader->need_update_uvs = false;
    shader->need_update_attribute = false;
    shader->need_update_displacement = false;
  }

  const Scene::MotionType need_motion = scene->need_motion();
  const bool motion_blur = need_motion == Scene::MOTION_BLUR;

  /* Update objects. */
  {
    const scoped_callback_timer timer([scene](double time) {
      if (scene->update_stats) {
        scene->update_stats->geometry.times.add_entry({"device_update (compute bounds)", time});
      }
    });
    for (Object *object : scene->objects) {
      object->compute_bounds(motion_blur);
    }
  }

  if (progress.get_cancel()) {
    return;
  }

  if (need_update_scene_bvh) {
    const scoped_callback_timer timer([scene](double time) {
      if (scene->update_stats) {
        scene->update_stats->geometry.times.add_entry({"device_update (build scene BVH)", time});
      }
    });
    device_update_bvh(device, dscene, scene, progress);
    if (progress.get_cancel()) {
      return;
    }
  }

  /* Always set BVH layout again after displacement where it was set to none,
   * to avoid ray-tracing at that stage. */
  dscene->data.bvh.bvh_layout = BVHParams::best_bvh_layout(
      scene->params.bvh_layout, device->get_bvh_layout_mask(dscene->data.kernel_features));

  {
    const scoped_callback_timer timer([scene](double time) {
      if (scene->update_stats) {
        scene->update_stats->geometry.times.add_entry(
            {"device_update (copy meshes to device)", time});
      }
    });
    device_update_mesh(device, dscene, scene, progress);
    if (progress.get_cancel()) {
      return;
    }
  }

  /* unset flags */

  for (Geometry *geom : scene->geometry) {
    geom->clear_modified();
    geom->attributes.clear_modified();

    if (geom->is_mesh()) {
      Mesh *mesh = static_cast<Mesh *>(geom);
      mesh->subd_attributes.clear_modified();
    }
  }

  update_flags = UPDATE_NONE;

  dscene->bvh_nodes.clear_modified();
  dscene->bvh_leaf_nodes.clear_modified();
  dscene->object_node.clear_modified();
  dscene->prim_type.clear_modified();
  dscene->prim_visibility.clear_modified();
  dscene->prim_index.clear_modified();
  dscene->prim_object.clear_modified();
  dscene->prim_time.clear_modified();
  dscene->tri_verts.clear_modified();
  dscene->tri_shader.clear_modified();
  dscene->tri_vindex.clear_modified();
  dscene->tri_vnormal.clear_modified();
  dscene->curves.clear_modified();
  dscene->curve_keys.clear_modified();
  dscene->curve_segments.clear_modified();
  dscene->points.clear_modified();
  dscene->points_shader.clear_modified();
  dscene->attributes_map.clear_modified();
  dscene->attributes_float.clear_modified();
  dscene->attributes_float2.clear_modified();
  dscene->attributes_float3.clear_modified();
  dscene->attributes_float4.clear_modified();
  dscene->attributes_uchar4.clear_modified();
}

void GeometryManager::device_free(Device *device, DeviceScene *dscene, bool force_free)
{
  dscene->bvh_nodes.free_if_need_realloc(force_free);
  dscene->bvh_leaf_nodes.free_if_need_realloc(force_free);
  dscene->object_node.free_if_need_realloc(force_free);
  dscene->prim_type.free_if_need_realloc(force_free);
  dscene->prim_visibility.free_if_need_realloc(force_free);
  dscene->prim_index.free_if_need_realloc(force_free);
  dscene->prim_object.free_if_need_realloc(force_free);
  dscene->prim_time.free_if_need_realloc(force_free);
  dscene->tri_verts.free_if_need_realloc(force_free);
  dscene->tri_shader.free_if_need_realloc(force_free);
  dscene->tri_vnormal.free_if_need_realloc(force_free);
  dscene->tri_vindex.free_if_need_realloc(force_free);
  dscene->curves.free_if_need_realloc(force_free);
  dscene->curve_keys.free_if_need_realloc(force_free);
  dscene->curve_segments.free_if_need_realloc(force_free);
  dscene->points.free_if_need_realloc(force_free);
  dscene->points_shader.free_if_need_realloc(force_free);
  dscene->attributes_map.free_if_need_realloc(force_free);
  dscene->attributes_float.free_if_need_realloc(force_free);
  dscene->attributes_float2.free_if_need_realloc(force_free);
  dscene->attributes_float3.free_if_need_realloc(force_free);
  dscene->attributes_float4.free_if_need_realloc(force_free);
  dscene->attributes_uchar4.free_if_need_realloc(force_free);

  /* Signal for shaders like displacement not to do ray tracing. */
  dscene->data.bvh.bvh_layout = BVH_LAYOUT_NONE;

#ifdef WITH_OSL
  OSLGlobals *og = device->get_cpu_osl_memory();

  if (og) {
    og->object_name_map.clear();
    og->object_names.clear();
  }
#else
  (void)device;
#endif
}

void GeometryManager::tag_update(Scene *scene, const uint32_t flag)
{
  update_flags |= flag;

  /* do not tag the object manager for an update if it is the one who tagged us */
  if ((flag & OBJECT_MANAGER) == 0) {
    scene->object_manager->tag_update(scene, ObjectManager::GEOMETRY_MANAGER);
  }
}

bool GeometryManager::need_update() const
{
  return update_flags != UPDATE_NONE;
}

void GeometryManager::collect_statistics(const Scene *scene, RenderStats *stats)
{
  for (const Geometry *geometry : scene->geometry) {
    stats->mesh.geometry.add_entry(
        NamedSizeEntry(string(geometry->name.c_str()), geometry->get_total_size_in_bytes()));
  }
}

CCL_NAMESPACE_END
