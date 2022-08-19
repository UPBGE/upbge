/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation.
 */

/** \file
 * \ingroup eevee
 */

#include "BKE_lib_id.h"
#include "BKE_node.h"
#include "BKE_world.h"
#include "DEG_depsgraph_query.h"
#include "NOD_shader.h"

#include "eevee_instance.hh"

namespace blender::eevee {

/* -------------------------------------------------------------------- */
/** \name Default Material
 *
 * \{ */

DefaultWorldNodeTree::DefaultWorldNodeTree()
{
  bNodeTree *ntree = ntreeAddTree(nullptr, "World Nodetree", ntreeType_Shader->idname);
  bNode *background = nodeAddStaticNode(nullptr, ntree, SH_NODE_BACKGROUND);
  bNode *output = nodeAddStaticNode(nullptr, ntree, SH_NODE_OUTPUT_WORLD);
  bNodeSocket *background_out = nodeFindSocket(background, SOCK_OUT, "Background");
  bNodeSocket *output_in = nodeFindSocket(output, SOCK_IN, "Surface");
  nodeAddLink(ntree, background, background_out, output, output_in);
  nodeSetActive(ntree, output);

  color_socket_ =
      (bNodeSocketValueRGBA *)nodeFindSocket(background, SOCK_IN, "Color")->default_value;
  ntree_ = ntree;
}

DefaultWorldNodeTree::~DefaultWorldNodeTree()
{
  ntreeFreeEmbeddedTree(ntree_);
  MEM_SAFE_FREE(ntree_);
}

/* Configure a default nodetree with the given world.  */
bNodeTree *DefaultWorldNodeTree::nodetree_get(::World *wo)
{
  /* WARNING: This function is not threadsafe. Which is not a problem for the moment. */
  copy_v3_fl3(color_socket_->value, wo->horr, wo->horg, wo->horb);
  return ntree_;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name World
 *
 * \{ */

void World::sync()
{
  // if (inst_.lookdev.sync_world()) {
  //   return;
  // }

  ::World *bl_world = inst_.scene->world;
  if (bl_world == nullptr) {
    // bl_world = BKE_world_default();
    return;
  }

  WorldHandle &wo_handle = inst_.sync.sync_world(bl_world);

  if (wo_handle.recalc != 0) {
    // inst_.lightprobes.set_world_dirty();
  }
  wo_handle.reset_recalc_flag();

  /* TODO(fclem) This should be detected to scene level. */
  ::World *orig_world = (::World *)DEG_get_original_id(&bl_world->id);
  if (assign_if_different(prev_original_world, orig_world)) {
    inst_.sampling.reset();
  }

  bNodeTree *ntree = (bl_world->nodetree && bl_world->use_nodes) ?
                         bl_world->nodetree :
                         default_tree.nodetree_get(bl_world);

  GPUMaterial *gpumat = inst_.shaders.world_shader_get(bl_world, ntree);
  inst_.pipelines.world.sync(gpumat);
}

/** \} */

}  // namespace blender::eevee
