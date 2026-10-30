/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender {
struct Object;
}

class BL_SceneConverter;
class KX_GameObject;
class KX_Scene;
class RAS_MeshObject;

/** Use the normal scene-conversion physics path for a bounded checkpoint reconstruction. */
void BL_CreateGameStatePhysicsObject(KX_GameObject *game_object,
                                     blender::Object *render_object,
                                     RAS_MeshObject *mesh_object,
                                     KX_Scene *scene,
                                     int active_layer_bits,
                                     BL_SceneConverter *converter);
