/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#include "scene/particles.h"
#include "scene/mesh.h"
#include "scene/object.h"

#include "blender/sync.h"
#include "blender/util.h"

#include "util/foreach.h"

CCL_NAMESPACE_BEGIN

/* Utilities */

bool BlenderSync::sync_dupli_particle(BL::Object &b_ob,
                                      BL::DepsgraphObjectInstance &b_instance,
                                      Object *object)
{
  /* Test if this dupli was generated from a particle system. */
  BL::ParticleSystem b_psys = b_instance.particle_system();
  if (!b_psys)
    return false;

  object->set_hide_on_missing_motion(true);

  /* test if we need particle data */
  if (!object->get_geometry()->need_attribute(scene, ATTR_STD_PARTICLE))
    return false;

  /* don't handle child particles yet */
  BL::Array<int, OBJECT_PERSISTENT_ID_SIZE> persistent_id = b_instance.persistent_id();

  if (persistent_id[0] >= b_psys.particles.length())
    return false;

  /* find particle system */
  ParticleSystemKey key(b_ob, persistent_id);
  ParticleSystem *psys;

  bool first_use = !particle_system_map.is_used(key);
  bool need_update = particle_system_map.add_or_update(&psys, b_ob, b_instance.object(), key);

  /* no update needed? */
  if (!need_update && !object->get_geometry()->is_modified() &&
      !scene->object_manager->need_update())
    return true;

  /* first time used in this sync loop? clear and tag update */
  if (first_use) {
    psys->particles.clear();
    psys->tag_update(scene);
  }

  /* add particle */
  BL::Particle b_pa = b_psys.particles[persistent_id[0]];
  Particle pa;

  pa.index = persistent_id[0];
  pa.age = b_scene.frame_current_final() - b_pa.birth_time();
  pa.lifetime = b_pa.lifetime();
  pa.location = get_float3(b_pa.location());
  pa.rotation = get_float4(b_pa.rotation());
  pa.size = b_pa.size();
  pa.velocity = get_float3(b_pa.velocity());
  pa.angular_velocity = get_float3(b_pa.angular_velocity());

  psys->particles.push_back_slow(pa);

  object->set_particle_system(psys);
  object->set_particle_index(psys->particles.size() - 1);

  if (object->particle_index_is_modified())
    scene->object_manager->tag_update(scene, ObjectManager::PARTICLE_MODIFIED);

  /* return that this object has particle data */
  return true;
}

CCL_NAMESPACE_END
