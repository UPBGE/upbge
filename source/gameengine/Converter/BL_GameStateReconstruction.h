/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file BL_GameStateReconstruction.h
 *  \ingroup bgeconv
 *
 * Bounded, runtime-only reconstruction for C++ Logic Nodes checkpoints. This deliberately does
 * not expose the broad single-object conversion path.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BLI_uuid.h"
#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"
#include "MT_Vector4.h"

namespace blender {
struct Depsgraph;
struct Object;
}  // namespace blender

class EXP_Value;
class KX_GameObject;
class KX_Scene;
class RAS_MeshObject;

enum class BL_GameStatePropertyType : uint8_t {
  Bool = 0,
  Int,
  Float,
  String,
  /** A Vector, Color, Object, List or Table property, kept in `value`. */
  Value,
};

struct BL_GameStatePropertyRecipe {
  std::string name;
  BL_GameStatePropertyType type = BL_GameStatePropertyType::String;
  bool bool_value = false;
  int64_t int_value = 0;
  float float_value = 0.0f;
  std::string string_value;
  /** A copy of the authored value; each rebuilt object gets its own copy of it. */
  std::shared_ptr<EXP_Value> value;
  bool timer = false;
  bool save_game = false;
};

enum class BL_GameStatePhysicsKind : uint8_t {
  None = 0,
  JoltRigid,
  JoltSoft,
};

enum class BL_GameStateObjectKind : uint8_t {
  Empty = 0,
  Mesh,
};

/** Compact immutable facts captured immediately after initial scene conversion. */
struct BL_GameStateReconstructionRecipe {
  blender::Object *authored_source = nullptr;
  std::vector<RAS_MeshObject *> render_meshes;
  std::vector<BL_GameStatePropertyRecipe> properties;
  blender::bUUID source_uuid;
  std::string name;
  MT_Vector3 world_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 world_orientation = MT_Matrix3x3::Identity();
  MT_Vector3 world_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  MT_Vector4 object_color = MT_Vector4(1.0f, 1.0f, 1.0f, 1.0f);
  int layer = 0;
  int activity_flags = 0;
  float activity_physics_radius = 0.0f;
  float activity_logic_radius = 0.0f;
  bool visible = true;
  bool occluder = false;
  bool save_object_state = false;
  BL_GameStateObjectKind object_kind = BL_GameStateObjectKind::Empty;
  BL_GameStatePhysicsKind physics_kind = BL_GameStatePhysicsKind::None;
};

enum class BL_GameStateReconstructionResult : uint8_t {
  Success = 0,
  InvalidRecipe,
  UnsupportedContent,
  ConstructionFailed,
  InternalError,
};

/** Capture value data only; the recipe retains no hidden KX/Jolt prototype. */
BL_GameStateReconstructionResult BL_CaptureGameStateReconstructionRecipe(
    KX_Scene &scene,
    KX_GameObject &object,
    BL_GameStateReconstructionRecipe &r_recipe,
    std::string &r_error);

/**
 * Create one list-owned, independently reconstructable runtime object. The authored source is
 * read-only; the returned object's Blender render ID is a normal tracked runtime-owned copy.
 * C++ Logic Node bindings are installed separately and remain inactive until transaction commit.
 */
BL_GameStateReconstructionResult BL_ReconstructGameStateObject(
    KX_Scene &scene,
    blender::Depsgraph &depsgraph,
    const BL_GameStateReconstructionRecipe &recipe,
    KX_GameObject *&r_object,
    std::string &r_error);
