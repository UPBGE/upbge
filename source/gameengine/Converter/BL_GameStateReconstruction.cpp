/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file BL_GameStateReconstruction.cpp
 *  \ingroup bgeconv
 */

#include "BL_GameStateReconstruction.h"

#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include "BKE_layer.hh"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "DEG_depsgraph_query.hh"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BL_GameStateReconstructionInternal.h"
#include "BL_SceneConverter.h"
#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"
#include "EXP_Value.h"
#include "KX_EmptyObject.h"
#include "KX_GameObject.h"
#include "KX_Scene.h"
#include "PHY_IPhysicsController.h"
#include "SCA_LogicManager.h"
#include "SCA_TimeEventManager.h"
#include "SG_Node.h"

static constexpr size_t kGameStateReconstructionMaxProperties = 4096;
static constexpr size_t kGameStateReconstructionMaxNameBytes = 4096;
static constexpr size_t kGameStateReconstructionMaxStringBytes = 1024u * 1024u;
static constexpr size_t kGameStateReconstructionMaxTextBytes = 64u * 1024u * 1024u;
static constexpr float kGameStateOrientationTolerance = 1.0e-3f;

static bool game_state_is_finite(const MT_Vector3 &value)
{
  return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

static bool game_state_is_finite(const MT_Vector4 &value)
{
  return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z()) &&
         std::isfinite(value.w());
}

static bool game_state_orientation_is_valid(const MT_Matrix3x3 &value)
{
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      if (!std::isfinite(value[row][column])) {
        return false;
      }
    }
    if (std::abs(value[row].dot(value[row]) - 1.0f) > kGameStateOrientationTolerance) {
      return false;
    }
    for (int other = row + 1; other < 3; other++) {
      if (std::abs(value[row].dot(value[other])) > kGameStateOrientationTolerance) {
        return false;
      }
    }
  }
  return std::abs(value.determinant() - 1.0f) <= kGameStateOrientationTolerance;
}

static bool game_state_text_is_valid(const std::string &text, const size_t maximum)
{
  return text.size() <= maximum && text.find('\0') == std::string::npos &&
         blender::BLI_str_utf8_invalid_byte(text.data(), text.size()) == -1;
}

/** The Save Game marker kind of a Vector, Color, Object, List or Table value. */
static std::optional<KX_GameObject::SaveGamePropertyMetadata::Type> game_state_value_marker(
    EXP_Value &value)
{
  using Type = KX_GameObject::SaveGamePropertyMetadata::Type;
  switch (value.GetValueType()) {
    case VALUE_VECTOR3_TYPE:
      return Type::Vector3;
    case VALUE_VECTOR4_TYPE:
      return Type::Vector4;
    case VALUE_OBJECT_TYPE:
      return Type::Object;
    case VALUE_ARRAY_TYPE:
      return Type::List;
    case VALUE_DICT_TYPE:
      return Type::Dict;
    default:
      return std::nullopt;
  }
}

BL_GameStateReconstructionResult BL_CaptureGameStateReconstructionRecipe(
    KX_Scene &scene,
    KX_GameObject &object,
    BL_GameStateReconstructionRecipe &r_recipe,
    std::string &r_error)
{
  r_error.clear();
  if (typeid(object) != typeid(KX_EmptyObject) || object.GetScene() != &scene ||
      object.GetBlenderObject() == nullptr || object.GetSGNode() == nullptr)
  {
    r_error = "authored object is outside the bounded reconstruction runtime class";
    return BL_GameStateReconstructionResult::UnsupportedContent;
  }
  blender::Object *source = object.GetBlenderObject();
  const KX_GameObject::ActivityCullingInfo &activity = object.GetActivityCullingInfo();
  if (!ELEM(source->type, blender::OB_EMPTY, blender::OB_MESH)) {
    r_error = "authored reconstruction supports only Empty and Mesh objects";
    return BL_GameStateReconstructionResult::UnsupportedContent;
  }
  if (BLI_uuid_is_nil(object.GetPersistenceSourceUuid()) ||
      !game_state_text_is_valid(object.GetName(), kGameStateReconstructionMaxNameBytes) ||
      object.GetName().empty() || object.GetLayer() == 0 ||
      !game_state_is_finite(object.NodeGetWorldPosition()) ||
      !game_state_orientation_is_valid(object.NodeGetWorldOrientation()) ||
      !game_state_is_finite(object.NodeGetWorldScaling()) ||
      !game_state_is_finite(object.GetObjectColor()) ||
      !std::isfinite(activity.m_physicsRadius) || !std::isfinite(activity.m_logicRadius))
  {
    r_error = "authored reconstruction baseline state is invalid";
    return BL_GameStateReconstructionResult::UnsupportedContent;
  }
  if ((source->type == blender::OB_EMPTY && object.GetMeshCount() != 0) ||
      (source->type == blender::OB_MESH && object.GetMeshCount() <= 0))
  {
    r_error = "authored reconstruction render data does not match its object kind";
    return BL_GameStateReconstructionResult::UnsupportedContent;
  }
  if (object.GetPropertyCount() < 0 ||
      size_t(object.GetPropertyCount()) > kGameStateReconstructionMaxProperties ||
      object.GetSaveGameProperties().size() > kGameStateReconstructionMaxProperties)
  {
    r_error = "authored reconstruction property catalog exceeds its safety limit";
    return BL_GameStateReconstructionResult::UnsupportedContent;
  }

  BL_GameStateReconstructionRecipe recipe;
  recipe.authored_source = source;
  recipe.source_uuid = object.GetPersistenceSourceUuid();
  recipe.name = object.GetName();
  recipe.world_position = object.NodeGetWorldPosition();
  recipe.world_orientation = object.NodeGetWorldOrientation();
  recipe.world_scale = object.NodeGetWorldScaling();
  recipe.object_color = object.GetObjectColor();
  recipe.layer = object.GetLayer();
  recipe.activity_flags = int(activity.m_flags);
  recipe.activity_physics_radius = activity.m_physicsRadius;
  recipe.activity_logic_radius = activity.m_logicRadius;
  recipe.visible = object.GetVisible();
  recipe.occluder = object.GetOccluder();
  recipe.save_object_state = object.GetSaveGameObjectState();
  recipe.object_kind = source->type == blender::OB_MESH ? BL_GameStateObjectKind::Mesh :
                                                         BL_GameStateObjectKind::Empty;

  PHY_IPhysicsController *controller = object.GetPhysicsController();
  if (controller != nullptr) {
    if (!controller->SupportsGameStatePersistence() || !controller->HasValidGameStateBody()) {
      r_error = "authored object does not have a valid reconstructable Jolt body";
      return BL_GameStateReconstructionResult::UnsupportedContent;
    }
    recipe.physics_kind = controller->IsSoftBody() ? BL_GameStatePhysicsKind::JoltSoft :
                                                    BL_GameStatePhysicsKind::JoltRigid;
  }

  try {
    recipe.render_meshes.reserve(size_t(object.GetMeshCount()));
    for (int index = 0; index < object.GetMeshCount(); index++) {
      RAS_MeshObject *mesh = object.GetMesh(index);
      if (mesh == nullptr) {
        r_error = "authored reconstruction recipe contains a null render mesh";
        return BL_GameStateReconstructionResult::UnsupportedContent;
      }
      recipe.render_meshes.push_back(mesh);
    }

    const std::vector<std::string> property_names = object.GetPropertyNames();
    if (property_names.size() != size_t(object.GetPropertyCount())) {
      r_error = "authored reconstruction property catalog is inconsistent";
      return BL_GameStateReconstructionResult::InternalError;
    }
    recipe.properties.reserve(property_names.size());
    size_t text_bytes = 0;
    for (const std::string &name : property_names) {
      if (name.empty() ||
          !game_state_text_is_valid(name, kGameStateReconstructionMaxNameBytes) ||
          text_bytes > kGameStateReconstructionMaxTextBytes - name.size())
      {
        r_error = "authored reconstruction contains an invalid property name";
        return BL_GameStateReconstructionResult::UnsupportedContent;
      }
      text_bytes += name.size();
      EXP_Value *value = object.GetProperty(name);
      if (value == nullptr) {
        r_error = "authored reconstruction property catalog changed during capture";
        return BL_GameStateReconstructionResult::InternalError;
      }
      BL_GameStatePropertyRecipe property;
      property.name = name;
      property.timer = value->GetProperty("timer") != nullptr;
      if (value->GetPropertyCount() != int(property.timer)) {
        r_error = "authored reconstruction property '" + name +
                  "' contains unsupported nested state";
        return BL_GameStateReconstructionResult::UnsupportedContent;
      }
      if (property.timer) {
        EXP_Value *timer_marker = value->GetProperty("timer");
        if (timer_marker == nullptr || timer_marker->GetValueType() != VALUE_BOOL_TYPE ||
            !static_cast<EXP_BoolValue *>(timer_marker)->GetBool() ||
            timer_marker->GetPropertyCount() != 0)
        {
          r_error = "authored reconstruction property '" + name +
                    "' has an invalid timer marker";
          return BL_GameStateReconstructionResult::UnsupportedContent;
        }
      }
      if (const KX_GameObject::SaveGamePropertyMetadata *metadata =
              object.GetSaveGamePropertyMetadata(name))
      {
        property.save_game = true;
        property.timer = metadata->timer;
      }
      switch (value->GetValueType()) {
        case VALUE_BOOL_TYPE:
          property.type = BL_GameStatePropertyType::Bool;
          property.bool_value = static_cast<EXP_BoolValue *>(value)->GetBool();
          break;
        case VALUE_INT_TYPE:
          property.type = BL_GameStatePropertyType::Int;
          property.int_value = static_cast<EXP_IntValue *>(value)->GetInt();
          break;
        case VALUE_FLOAT_TYPE:
          property.type = BL_GameStatePropertyType::Float;
          property.float_value = static_cast<EXP_FloatValue *>(value)->GetFloat();
          if (!std::isfinite(property.float_value)) {
            r_error = "authored reconstruction property '" + name +
                      "' contains a non-finite float";
            return BL_GameStateReconstructionResult::UnsupportedContent;
          }
          break;
        case VALUE_STRING_TYPE:
          property.type = BL_GameStatePropertyType::String;
          property.string_value = value->GetText();
          if (!game_state_text_is_valid(property.string_value,
                                        kGameStateReconstructionMaxStringBytes) ||
              text_bytes >
                  kGameStateReconstructionMaxTextBytes - property.string_value.size())
          {
            r_error = "authored reconstruction property '" + name +
                      "' contains an invalid or oversized string";
            return BL_GameStateReconstructionResult::UnsupportedContent;
          }
          text_bytes += property.string_value.size();
          break;
        default:
          if (!game_state_value_marker(*value)) {
            r_error = "authored reconstruction property '" + name +
                      "' is not a converted game property";
            return BL_GameStateReconstructionResult::UnsupportedContent;
          }
          property.type = BL_GameStatePropertyType::Value;
          property.value = std::shared_ptr<EXP_Value>(value->GetReplica(),
                                                      [](EXP_Value *copy) { copy->Release(); });
          break;
      }
      if (property.save_game) {
        const KX_GameObject::SaveGamePropertyMetadata *metadata =
            object.GetSaveGamePropertyMetadata(name);
        const auto expected_type = [&]() {
          switch (metadata->type) {
            case KX_GameObject::SaveGamePropertyMetadata::Type::Bool:
              return BL_GameStatePropertyType::Bool;
            case KX_GameObject::SaveGamePropertyMetadata::Type::Int:
              return BL_GameStatePropertyType::Int;
            case KX_GameObject::SaveGamePropertyMetadata::Type::Float:
              return BL_GameStatePropertyType::Float;
            case KX_GameObject::SaveGamePropertyMetadata::Type::String:
              return BL_GameStatePropertyType::String;
            default:
              return BL_GameStatePropertyType::Value;
          }
        }();
        if (property.type != expected_type ||
            (property.value && game_state_value_marker(*property.value) != metadata->type) ||
            (value->GetProperty("timer") != nullptr) != metadata->timer)
        {
          r_error = "authored reconstruction property '" + name +
                    "' does not match its Save Game marker contract";
          return BL_GameStateReconstructionResult::UnsupportedContent;
        }
      }
      recipe.properties.push_back(std::move(property));
    }
    for (const auto &item : object.GetSaveGameProperties()) {
      if (object.GetProperty(item.first) == nullptr) {
        r_error = "authored reconstruction Save Game marker has no baseline property";
        return BL_GameStateReconstructionResult::UnsupportedContent;
      }
    }
  }
  catch (const std::exception &exception) {
    r_error = std::string("could not allocate authored reconstruction recipe: ") +
              exception.what();
    return BL_GameStateReconstructionResult::InternalError;
  }
  catch (...) {
    r_error = "could not allocate authored reconstruction recipe";
    return BL_GameStateReconstructionResult::InternalError;
  }

  r_recipe = std::move(recipe);
  return BL_GameStateReconstructionResult::Success;
}

BL_GameStateReconstructionResult BL_ReconstructGameStateObject(
    KX_Scene &scene,
    blender::Depsgraph &depsgraph,
    const BL_GameStateReconstructionRecipe &recipe,
    KX_GameObject *&r_object,
    std::string &r_error)
{
  r_object = nullptr;
  r_error.clear();
  blender::Scene *blender_scene = scene.GetBlenderScene();
  blender::ViewLayer *view_layer = blender_scene ? BKE_view_layer_default_view(blender_scene) :
                                                  nullptr;
  if (blender_scene == nullptr || view_layer == nullptr ||
      DEG_get_input_scene(&depsgraph) != blender_scene ||
      DEG_get_input_view_layer(&depsgraph) != view_layer)
  {
    r_error = "authored reconstruction received a depsgraph from a different runtime scene";
    return BL_GameStateReconstructionResult::InvalidRecipe;
  }
  if (recipe.authored_source == nullptr || BLI_uuid_is_nil(recipe.source_uuid) ||
      (recipe.object_kind == BL_GameStateObjectKind::Mesh &&
       recipe.authored_source->type != blender::OB_MESH) ||
      (recipe.object_kind == BL_GameStateObjectKind::Empty &&
       recipe.authored_source->type != blender::OB_EMPTY))
  {
    r_error = "authored reconstruction recipe no longer matches its immutable source";
    return BL_GameStateReconstructionResult::InvalidRecipe;
  }
  if ((recipe.physics_kind != BL_GameStatePhysicsKind::None &&
       recipe.object_kind != BL_GameStateObjectKind::Mesh) ||
      (recipe.object_kind == BL_GameStateObjectKind::Mesh && recipe.render_meshes.empty()) ||
      (recipe.object_kind == BL_GameStateObjectKind::Empty && !recipe.render_meshes.empty()))
  {
    r_error = "authored reconstruction recipe has an invalid render/physics combination";
    return BL_GameStateReconstructionResult::InvalidRecipe;
  }
  if (recipe.name.empty() ||
      !game_state_text_is_valid(recipe.name, kGameStateReconstructionMaxNameBytes) ||
      recipe.layer == 0 || !game_state_is_finite(recipe.world_position) ||
      !game_state_orientation_is_valid(recipe.world_orientation) ||
      !game_state_is_finite(recipe.world_scale) || !game_state_is_finite(recipe.object_color) ||
      !std::isfinite(recipe.activity_physics_radius) ||
      !std::isfinite(recipe.activity_logic_radius) ||
      recipe.properties.size() > kGameStateReconstructionMaxProperties)
  {
    r_error = "authored reconstruction recipe contains invalid baseline state";
    return BL_GameStateReconstructionResult::InvalidRecipe;
  }
  for (RAS_MeshObject *mesh : recipe.render_meshes) {
    if (mesh == nullptr) {
      r_error = "authored reconstruction recipe contains a null render mesh";
      return BL_GameStateReconstructionResult::InvalidRecipe;
    }
  }
  size_t recipe_text_bytes = 0;
  const std::string *previous_property_name = nullptr;
  for (const BL_GameStatePropertyRecipe &property : recipe.properties) {
    if (property.name.empty() ||
        !game_state_text_is_valid(property.name, kGameStateReconstructionMaxNameBytes) ||
        (previous_property_name != nullptr && *previous_property_name >= property.name) ||
        recipe_text_bytes > kGameStateReconstructionMaxTextBytes - property.name.size() ||
        !ELEM(property.type,
              BL_GameStatePropertyType::Bool,
              BL_GameStatePropertyType::Int,
              BL_GameStatePropertyType::Float,
              BL_GameStatePropertyType::String,
              BL_GameStatePropertyType::Value) ||
        ((property.type == BL_GameStatePropertyType::Value) !=
         (property.value && game_state_value_marker(*property.value))) ||
        (property.timer && property.type != BL_GameStatePropertyType::Float) ||
        (property.type == BL_GameStatePropertyType::Float &&
         !std::isfinite(property.float_value)))
    {
      r_error = "authored reconstruction recipe contains an invalid baseline property";
      return BL_GameStateReconstructionResult::InvalidRecipe;
    }
    recipe_text_bytes += property.name.size();
    if (property.type == BL_GameStatePropertyType::String &&
        (!game_state_text_is_valid(property.string_value,
                                   kGameStateReconstructionMaxStringBytes) ||
         recipe_text_bytes >
             kGameStateReconstructionMaxTextBytes - property.string_value.size()))
    {
      r_error = "authored reconstruction recipe contains an invalid baseline string";
      return BL_GameStateReconstructionResult::InvalidRecipe;
    }
    if (property.type == BL_GameStatePropertyType::String) {
      recipe_text_bytes += property.string_value.size();
    }
    previous_property_name = &property.name;
  }

  KX_GameObject *object = nullptr;
  bool published = false;
  bool converter_registered = false;
  bool logic_name_registered = false;
  bool logic_object_registered = false;
  bool object_listed = false;
  bool parent_listed = false;
  auto rollback = [&]() {
    if (published && object != nullptr) {
      scene.RemoveObject(object, KX_ObjectRemovalReason::LoadReconcile);
    }
    else if (object != nullptr) {
      if (parent_listed && scene.GetRootParentList()->RemoveValue(object)) {
        object->Release();
      }
      if (object_listed && scene.GetObjectList()->RemoveValue(object)) {
        scene.NoteObjectListChanged();
        object->Release();
      }
      if (logic_object_registered) {
        scene.GetLogicManager()->UnregisterGameObj(object->GetBlenderObject(), object);
      }
      if (logic_name_registered) {
        scene.GetLogicManager()->UnregisterGameObjectName(object->GetName(), object);
      }
      if (converter_registered) {
        scene.GetBlenderSceneConverter()->UnregisterGameObject(object);
      }
      if (object->IsReplica() && object->GetBlenderObject() != nullptr &&
          object->GetBlenderObject() != recipe.authored_source)
      {
        object->DiscardRenderedObject();
      }
      delete object->GetSGNode();
      object->SetSGNode(nullptr);
      object->Release();
    }
    object = nullptr;
  };

  try {
    object = new KX_EmptyObject();
    object->SetScene(&scene);
    /* Seed the cached color while no Blender object is attached. This lets the post-copy setter
     * restore a saved color that differs from the authored source without mutating that source. */
    object->SetObjectColor(MT_Vector4(recipe.authored_source->color));
    object->SetBlenderObject(recipe.authored_source);
    object->SetName(recipe.name);
    object->SetPersistenceSourceUuid(recipe.source_uuid);
    object->ClearPersistenceInstanceUuid();
    object->SetSaveGameObjectState(recipe.save_object_state);
    object->SetLayer(recipe.layer);
    object->SetOccluder(recipe.occluder, false);
    KX_GameObject::ActivityCullingInfo activity;
    activity.m_flags = KX_GameObject::ActivityCullingInfo::Flag(recipe.activity_flags);
    activity.m_physicsRadius = recipe.activity_physics_radius;
    activity.m_logicRadius = recipe.activity_logic_radius;
    object->SetActivityCullingInfo(activity);
    for (RAS_MeshObject *mesh : recipe.render_meshes) {
      object->AddMesh(mesh);
    }
    object->NodeSetLocalPosition(recipe.world_position);
    object->NodeSetLocalOrientation(recipe.world_orientation);
    object->NodeSetLocalScale(recipe.world_scale);
    object->NodeUpdateGS(0.0);

    /* This creates only the existing tracked runtime render copy. It never writes the authored
     * source or uses editor duplication. */
    object->ReplicateBlenderObject();
    blender::Object *runtime_render_object = object->GetBlenderObject();
    if (runtime_render_object == nullptr || runtime_render_object == recipe.authored_source ||
        !object->IsReplica())
    {
      rollback();
      r_error = "could not create the runtime-owned render copy";
      return BL_GameStateReconstructionResult::ConstructionFailed;
    }
    /* Blender-backed state belongs to the runtime copy. Applying it before replication would
     * unhide or recolor the retained authored source and leave a render-only duplicate behind. */
    object->SetObjectColor(recipe.object_color);
    object->SetVisible(recipe.visible, false);
    runtime_render_object->lay = recipe.layer;

    scene.GetBlenderSceneConverter()->RegisterGameObject(object, runtime_render_object);
    converter_registered = true;
    scene.GetLogicManager()->RegisterGameObjectName(object->GetName(), object);
    logic_name_registered = true;
    scene.GetLogicManager()->RegisterGameObj(runtime_render_object, object);
    logic_object_registered = true;

    object->AddRef();
    try {
      scene.GetObjectList()->Add(object);
      scene.NoteObjectListChanged();
      object_listed = true;
    }
    catch (...) {
      object->Release();
      throw;
    }
    object->AddRef();
    try {
      scene.GetRootParentList()->Add(object);
      parent_listed = true;
    }
    catch (...) {
      object->Release();
      throw;
    }
    object->Release();
    published = true;

    for (const BL_GameStatePropertyRecipe &property : recipe.properties) {
      using ScopedExpValue = std::unique_ptr<EXP_Value, void (*)(EXP_Value *)>;
      const auto release_value = [](EXP_Value *value) {
        if (value != nullptr) {
          value->Release();
        }
      };
      ScopedExpValue value(nullptr, release_value);
      switch (property.type) {
        case BL_GameStatePropertyType::Bool:
          value.reset(new EXP_BoolValue(property.bool_value));
          break;
        case BL_GameStatePropertyType::Int:
          value.reset(new EXP_IntValue(property.int_value));
          break;
        case BL_GameStatePropertyType::Float:
          value.reset(new EXP_FloatValue(property.float_value));
          break;
        case BL_GameStatePropertyType::String:
          value.reset(new EXP_StringValue(property.string_value, std::string()));
          break;
        case BL_GameStatePropertyType::Value:
          value.reset(property.value->GetReplica());
          break;
      }
      if (property.timer) {
        ScopedExpValue timer(new EXP_BoolValue(true), release_value);
        value->SetProperty("timer", timer.get());
      }
      object->SetProperty(property.name, value.get());
      if (property.save_game) {
        KX_GameObject::SaveGamePropertyMetadata::Type marker_type;
        switch (property.type) {
          case BL_GameStatePropertyType::Bool:
            marker_type = KX_GameObject::SaveGamePropertyMetadata::Type::Bool;
            break;
          case BL_GameStatePropertyType::Int:
            marker_type = KX_GameObject::SaveGamePropertyMetadata::Type::Int;
            break;
          case BL_GameStatePropertyType::Float:
            marker_type = KX_GameObject::SaveGamePropertyMetadata::Type::Float;
            break;
          case BL_GameStatePropertyType::String:
            marker_type = KX_GameObject::SaveGamePropertyMetadata::Type::String;
            break;
          case BL_GameStatePropertyType::Value:
            marker_type = *game_state_value_marker(*property.value);
            break;
        }
        object->AddSaveGameProperty(property.name, marker_type, property.timer);
      }
      if (property.timer) {
        scene.GetTimeEventManager()->AddTimeProperty(value.get());
      }
    }

    RAS_MeshObject *physics_mesh = recipe.render_meshes.empty() ? nullptr :
                                                                  recipe.render_meshes.front();
    BL_CreateGameStatePhysicsObject(object,
                                    runtime_render_object,
                                    physics_mesh,
                                    &scene,
                                    recipe.layer,
                                    scene.GetBlenderSceneConverter());
    PHY_IPhysicsController *controller = object->GetPhysicsController();
    const bool expects_physics = recipe.physics_kind != BL_GameStatePhysicsKind::None;
    const bool valid_physics =
        controller != nullptr && controller->SupportsGameStatePersistence() &&
        controller->HasValidGameStateBody() &&
        (controller->IsSoftBody() == (recipe.physics_kind == BL_GameStatePhysicsKind::JoltSoft));
    if ((expects_physics && !valid_physics) || (!expects_physics && controller != nullptr)) {
      rollback();
      r_error = expects_physics ? "normal Jolt reconstruction did not create the expected body" :
                                  "plain reconstruction unexpectedly created a physics body";
      return BL_GameStateReconstructionResult::ConstructionFailed;
    }
  }
  catch (const std::exception &exception) {
    rollback();
    r_error = std::string("authored runtime reconstruction failed: ") + exception.what();
    return BL_GameStateReconstructionResult::InternalError;
  }
  catch (...) {
    rollback();
    r_error = "authored runtime reconstruction failed";
    return BL_GameStateReconstructionResult::InternalError;
  }

  r_object = object;
  return BL_GameStateReconstructionResult::Success;
}
