/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation.
 */

/** \file
 * \ingroup eevee
 *
 * World rendering with material handling. Also take care of lookdev
 * HDRI and default material.
 */

#pragma once

#include "DNA_world_types.h"

namespace blender::eevee {

class Instance;

/* -------------------------------------------------------------------- */
/** \name Default World Node-Tree
 *
 * In order to support worlds without nodetree we reuse and configure a standalone nodetree that
 * we pass for shader generation. The GPUMaterial is still stored inside the World even if
 * it does not use a nodetree.
 * \{ */

class DefaultWorldNodeTree {
 private:
  bNodeTree *ntree_;
  bNodeSocketValueRGBA *color_socket_;

 public:
  DefaultWorldNodeTree();
  ~DefaultWorldNodeTree();

  bNodeTree *nodetree_get(::World *world);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name World
 *
 * \{ */

class World {
 private:
  Instance &inst_;

  DefaultWorldNodeTree default_tree;

  /* Used to detect if world change. */
  ::World *prev_original_world = nullptr;

 public:
  World(Instance &inst) : inst_(inst){};

  void sync();
};

/** \} */

}  // namespace blender::eevee
