/* SPDX-FileCopyrightText: 2025 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"
#include "BKE_object_gpu_deform.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BKE_modifier.hh"

#include "DEG_depsgraph.hh"

namespace blender {

void BKE_object_gpu_deform_bump_update_if_needed(Object *ob, bContext *C)
{
  if (!ob || ob->type != OB_MESH) {
    return;
  }

  Mesh *me = (Mesh *)ob->data;
  if (!me || !me->is_running_gpu_animation_playback) {
    return;
  }

  for (ModifierData *md = (ModifierData *)ob->modifiers.first; md; md = md->next) {
    if (md->type != eModifierType_MeshDeform) {
      continue;
    }
    MeshDeformModifierData *mmd = (MeshDeformModifierData *)md;
    if (!mmd || !(mmd->deform_method & MESHDEFORM_DEFORM_METHOD_GPU)) {
      continue;
    }
    /* No need to bump if the cage is an armature child: the armature path
     * already bumps the update count.  Otherwise the cage object is animated
     * directly and we need a depsgraph bump so that DEG_get_evaluated returns
     * the current frame's pose. */
    if (!mmd->object) {
      continue;
    }
    if (mmd->object->parent && mmd->object->parent->type == OB_ARMATURE) {
      continue;
    }

    Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
    if (!depsgraph) {
      continue;
    }

    DEG_bump_update_count(depsgraph);
  }
}

}  // namespace blender
