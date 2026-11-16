/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_object.hh"

#include "DRW_engine.hh" /* DRW_submission_start() */
#include "GPU_capabilities.hh"

#include "../eevee/eevee_shader.hh" /* VERTEX_COPY */
#include "BLI_map.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_modifier_types.h"
#include "DNA_rigidbody_types.h"

#include "draw_cache.hh"
#include "draw_cache_impl.hh"

#include "eevee_game_instance.hh"
#include "eevee_game_velocity.hh"

namespace blender::eevee_game {

/* -------------------------------------------------------------------- */
/** \name VelocityModule
 * \{ */

void VelocityModule::begin_sync()
{
  /* In game (viewport) mode we only track current and previous.
   * Reset current-step counters; previous-step data is kept from last frame. */
  step_ = eevee::STEP_CURRENT;
  object_steps_usage[eevee::STEP_CURRENT] = 0;
}

bool VelocityModule::step_object_sync(ObjectKey &object_key,
                                       const ObjectRef &object_ref,
                                       int recalc,
                                       ResourceHandleRange resource_handle)
{
  Object *ob = object_ref.object;

  /* Conservative: assume all objects move. Comparison in end_sync() will discard static ones. */
  bool has_motion = object_has_velocity(ob) || (recalc & ID_RECALC_TRANSFORM);
  bool has_deform = object_is_deform(ob)    || (recalc & ID_RECALC_GEOMETRY);

  if (!has_motion && !has_deform) {
    return false;
  }

  /* Register this object's world matrix at the current step. */
  VelocityObjectData &vel = velocity_map.lookup_or_add_default(object_key);
  vel.obj.ofs[eevee::STEP_CURRENT] = object_steps_usage[eevee::STEP_CURRENT]++;
  vel.obj.resource_id              = resource_handle.index();
  vel.id = uint64_t(ob->data);

  (*object_steps[eevee::STEP_CURRENT])[vel.obj.ofs[eevee::STEP_CURRENT]] = ob->object_to_world();

  /* If object has no previous-step slot yet (first frame), copy current as previous
   * so the velocity vector is zero and FSR doesn't ghost on first frame. */
  if (vel.obj.ofs[eevee::STEP_PREVIOUS] == -1) {
    vel.obj.ofs[eevee::STEP_PREVIOUS] = object_steps_usage[eevee::STEP_PREVIOUS]++;
    (*object_steps[eevee::STEP_PREVIOUS])[vel.obj.ofs[eevee::STEP_PREVIOUS]] =
        ob->object_to_world();
  }

  /* Geometry deformation (skinned meshes, shape keys, etc.) */
  if (has_deform) {
    auto add_cb = [&]() {
      VelocityGeometryData data;
      if (ob->type == OB_MESH) {
        data.batch = DRW_cache_mesh_surface_get(ob);
      }
      return data;
    };
    const VelocityGeometryData &geom = geometry_map.lookup_or_add_cb(vel.id, add_cb);
    if (!geom.batch) {
      has_deform = false;
    }
  }

  /* Discard objects that were tagged as moving but whose matrix didn't change. */
  if (has_motion && !has_deform) {
    const float4x4 &curr = (*object_steps[eevee::STEP_CURRENT])[vel.obj.ofs[eevee::STEP_CURRENT]];
    const float4x4 &prev = (*object_steps[eevee::STEP_PREVIOUS])[vel.obj.ofs[eevee::STEP_PREVIOUS]];
    has_motion = (curr != prev);
  }

  return has_motion || has_deform;
}

void VelocityModule::geometry_steps_fill()
{
  /* Compute flat offsets into the geometry step buffer for each deforming object. */
  int dst_ofs = 0;
  for (VelocityGeometryData &geom : geometry_map.values()) {
    gpu::VertBuf *pos_buf = geom.pos_buf_get();
    if (!pos_buf) {
      continue;
    }
    geom.len = int(GPU_vertbuf_get_vertex_len(pos_buf));
    geom.ofs = dst_ofs;
    dst_ofs += geom.len;
  }

  geometry_steps[eevee::STEP_CURRENT]->resize(max_ii(16, dst_ofs));

  /* Positions packed as float4 can be copied buffer to buffer. Everything
   * else (mesh positions are float3, stride 12) goes through EEVEE's vertex
   * copy compute shader, which repacks to float4 on the GPU. */
  DRW_submission_start();

  PassSimple copy_ps("Velocity Copy Pass");
  copy_ps.init();
  copy_ps.state_set(DRW_STATE_NO_DRAW);
  copy_ps.shader_set(eevee::ShaderModule::module_get()->static_shader_get(eevee::VERTEX_COPY));
  copy_ps.bind_ssbo("out_buf", *geometry_steps[eevee::STEP_CURRENT]);

  for (VelocityGeometryData &geom : geometry_map.values()) {
    gpu::VertBuf *pos_buf = geom.pos_buf_get();
    if (!pos_buf || geom.len == 0) {
      continue;
    }
    const GPUVertFormat *fmt = GPU_vertbuf_get_format(pos_buf);
    if (fmt->stride == 16) {
      GPU_storagebuf_copy_sub_from_vertbuf(*geometry_steps[eevee::STEP_CURRENT],
                                           pos_buf,
                                           geom.ofs * sizeof(float4),
                                           0,
                                           geom.len * sizeof(float4));
    }
    else {
      BLI_assert(fmt->stride % 4 == 0);
      copy_ps.bind_ssbo("in_buf", pos_buf);
      copy_ps.push_constant("start_offset", geom.ofs);
      copy_ps.push_constant("vertex_stride", int(fmt->stride / 4));
      copy_ps.push_constant("vertex_count", geom.len);
      const uint group_len_x = divide_ceil_u(uint(geom.len), VERTEX_COPY_GROUP_SIZE);
      const uint verts_per_thread = divide_ceil_u(group_len_x, GPU_max_work_group_count(0));
      copy_ps.dispatch(int3(group_len_x / verts_per_thread, 1, 1));
    }
  }
  copy_ps.barrier(GPU_BARRIER_SHADER_STORAGE);
  inst_.manager->submit(copy_ps);

  DRW_submission_end();

  /* Write per-object geometry offsets back into the velocity map. */
  for (VelocityObjectData &vel : velocity_map.values()) {
    const VelocityGeometryData &geom =
        geometry_map.lookup_default(vel.id, VelocityGeometryData());
    vel.geo.len[eevee::STEP_CURRENT] = geom.len;
    vel.geo.ofs[eevee::STEP_CURRENT] = geom.ofs;
    vel.id = 0;
  }

  geometry_map.clear();
}

void VelocityModule::end_sync()
{
  /* Remove objects that were present in velocity_map but not re-submitted this frame
   * (i.e. objects that were deleted or hidden). */
  Vector<ObjectKey, 0> deleted;
  uint32_t max_resource_id = 0u;

  for (MapItem<ObjectKey, VelocityObjectData> item : velocity_map.items()) {
    if (item.value.obj.resource_id == uint32_t(-1)) {
      deleted.append(item.key);
    }
    else {
      max_resource_id = max_uu(max_resource_id, item.value.obj.resource_id);
    }
  }
  for (auto &key : deleted) {
    velocity_map.remove(key);
  }

  /* Size the indirection buffer to cover all resource IDs seen this frame. */
  indirection_buf.resize(ceil_to_multiple_u(max_resource_id + 1, 128));

  for (VelocityObjectData &vel : velocity_map.values()) {
    /* Deformation is valid only if vertex counts match between previous and current step. */
    const bool geom_match = (vel.geo.len[eevee::STEP_CURRENT] != 0) &&
                            (vel.geo.len[eevee::STEP_CURRENT] ==
                             vel.geo.len[eevee::STEP_PREVIOUS]);
    vel.geo.do_deform = geom_match;

    indirection_buf[vel.obj.resource_id] = vel;
    /* Reset resource_id so deleted objects can be detected next frame. */
    vel.obj.resource_id = uint32_t(-1);
  }

  /* Upload all GPU buffers. */
  object_steps[eevee::STEP_PREVIOUS]->push_update();
  object_steps[eevee::STEP_CURRENT]->push_update();
  geometry_steps[eevee::STEP_PREVIOUS]->push_update();
  indirection_buf.push_update();

  /* Swap current -> previous for the next frame (viewport mode only swap). */
  std::swap(object_steps[eevee::STEP_PREVIOUS],   object_steps[eevee::STEP_CURRENT]);
  std::swap(geometry_steps[eevee::STEP_PREVIOUS], geometry_steps[eevee::STEP_CURRENT]);
  std::swap(object_steps_usage[eevee::STEP_PREVIOUS],
            object_steps_usage[eevee::STEP_CURRENT]);

  for (VelocityObjectData &vel : velocity_map.values()) {
    /* Move current-step offsets into previous-step slots. */
    vel.obj.ofs[eevee::STEP_PREVIOUS] = vel.obj.ofs[eevee::STEP_CURRENT];
    vel.obj.ofs[eevee::STEP_CURRENT]  = -1;
    vel.geo.ofs[eevee::STEP_PREVIOUS] = vel.geo.ofs[eevee::STEP_CURRENT];
    vel.geo.len[eevee::STEP_PREVIOUS] = vel.geo.len[eevee::STEP_CURRENT];
    vel.geo.ofs[eevee::STEP_CURRENT]  = -1;
    vel.geo.len[eevee::STEP_CURRENT]  = -1;
  }
}

bool VelocityModule::object_has_velocity(const Object * /*ob*/)
{
  /* Conservative: assume all objects can move.
   * Static objects are culled later by comparing matrices. */
  return true;
}

bool VelocityModule::object_is_deform(const Object *ob)
{
  RigidBodyOb *rbo = ob->rigidbody_object;
  const bool has_rigidbody = rbo && (rbo->type == RBO_TYPE_ACTIVE);
  return BKE_object_is_deform_modified(inst_.scene, const_cast<Object *>(ob)) ||
         (has_rigidbody && (rbo->flag & RBO_FLAG_USE_DEFORM) != 0);
}

/** \} */

} // namespace blender::eevee_game
