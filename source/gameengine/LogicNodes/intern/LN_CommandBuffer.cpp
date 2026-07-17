/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_CommandBuffer.cpp
 *  \ingroup logicnodes
 */

#include "LN_CommandBuffer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_constraint.h"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_material.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_node_enum.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_object.hh"
#include "DNA_action_types.h"
#include "DNA_collection_types.h"
#include "../../../blender/depsgraph/DEG_depsgraph.hh"
#include "../../../blender/depsgraph/DEG_depsgraph_build.hh"
#include "DNA_constraint_types.h"
#include "DNA_ID.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#ifdef WITH_AUDASPACE
#  include <Exception.h>
#  include <devices/DeviceManager.h>
#  include <devices/IDevice.h>
#  include <devices/I3DHandle.h>
#  include <devices/IHandle.h>
#  include "BKE_sound.hh"
#  include "DNA_ID_enums.h"
#  include "DNA_sound_types.h"
#endif

#include "BLI_assert.h"
#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_string.h"
#include "CM_Message.h"
#include "RNA_access.hh"
#include "DEV_InputDevice.h"
#include "DEV_Joystick.h"
#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"
#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_ListValue.h"
#include "EXP_StringValue.h"
#include "EXP_Value.h"
#include "EXP_Vector3Value.h"
#include "EXP_Vector4Value.h"
#include "KX_Camera.h"
#include "BL_Action.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_Camera.h"
#include "KX_KetsjiEngine.h"
#include "KX_Light.h"
#include "KX_Scene.h"
#include "SCA_LogicManager.h"
#include "LN_RuntimeTree.h"
#include "LN_CommandDiagnostics.h"
#include "LN_Manager.h"
#include "MT_Matrix3x3.h"
#include "MT_Transform.h"
#include "PHY_ICharacter.h"
#include "PHY_IPhysicsController.h"
#include "PHY_IPhysicsEnvironment.h"
#include "PHY_IVehicle.h"
#include "RAS_ICanvas.h"
#include "../../Converter/BL_SceneConverter.h"
#include "../../Ketsji/KX_BlenderMaterial.h"
#include "../../Rasterizer/RAS_MeshMaterial.h"
#include "../../Rasterizer/RAS_MeshObject.h"
#include "../../../blender/gpu/GPU_material.hh"
#include "../../../blender/windowmanager/WM_api.hh"
#include "../../../blender/windowmanager/WM_types.hh"

namespace blender {
extern StructRNA *RNA_Node;
extern StructRNA *RNA_NodeSocket;
extern StructRNA *RNA_Constraint;
extern StructRNA *RNA_NodesModifier;
extern StructRNA *RNA_Scene;
}  // namespace blender

namespace {

using blender::GameVehicleSettings;
using blender::G;
using blender::Object;
using blender::OB_VEHICLE_TYPE_CHASSIS;
using blender::OB_VEHICLE_TYPE_MOTORCYCLE_CHASSIS;
using blender::OB_VEHICLE_TYPE_MOTORCYCLE_WHEEL;
using blender::OB_VEHICLE_TYPE_WHEEL;
using blender::OB_VEHICLE_WHEEL_OVERRIDE_ENGINE_FORCE;
using blender::OB_VEHICLE_WHEEL_USE_BRAKE;
using blender::OB_VEHICLE_WHEEL_USE_STEERING;

static bool rigid_body_lock_enabled(const float value)
{
  return value != 0.0f;
}

static void apply_rigid_body_attribute(PHY_IPhysicsController &physics_controller,
                                       const LN_RigidBodyAttribute attribute,
                                       const MT_Vector3 &value,
                                       const MT_Vector3 &secondary_value,
                                       const float scalar_value,
                                       const bool bool_value,
                                       const bool secondary_bool_value)
{
  switch (attribute) {
    case LN_RigidBodyAttribute::Mass:
      physics_controller.SetMass(scalar_value);
      break;
    case LN_RigidBodyAttribute::Friction:
      physics_controller.SetFriction(scalar_value);
      break;
    case LN_RigidBodyAttribute::Restitution:
      physics_controller.SetRestitution(scalar_value);
      break;
    case LN_RigidBodyAttribute::Damping:
      physics_controller.SetDamping(std::max(value.x(), 0.0f), std::max(value.y(), 0.0f));
      break;
    case LN_RigidBodyAttribute::MinLinearVelocity:
      physics_controller.SetLinVelocityMin(std::max(scalar_value, 0.0f));
      break;
    case LN_RigidBodyAttribute::MaxLinearVelocity:
      physics_controller.SetLinVelocityMax(std::max(scalar_value, 0.0f));
      break;
    case LN_RigidBodyAttribute::MinAngularVelocity:
      physics_controller.SetAngularVelocityMin(std::max(scalar_value, 0.0f));
      break;
    case LN_RigidBodyAttribute::MaxAngularVelocity:
      physics_controller.SetAngularVelocityMax(std::max(scalar_value, 0.0f));
      break;
    case LN_RigidBodyAttribute::GravityFactor:
      physics_controller.SetGravityFactor(scalar_value);
      break;
    case LN_RigidBodyAttribute::Ccd:
      physics_controller.SetCcdMotionThreshold(bool_value ? 1.0f : 0.0f);
      break;
    case LN_RigidBodyAttribute::Sleeping:
      physics_controller.SetAllowSleeping(bool_value);
      if (secondary_bool_value) {
        physics_controller.SetActive(true);
      }
      break;
    case LN_RigidBodyAttribute::AxisLocks:
      physics_controller.SetRigidBodyAxisLocks(rigid_body_lock_enabled(value.x()),
                                               rigid_body_lock_enabled(value.y()),
                                               rigid_body_lock_enabled(value.z()),
                                               rigid_body_lock_enabled(secondary_value.x()),
                                               rigid_body_lock_enabled(secondary_value.y()),
                                               rigid_body_lock_enabled(secondary_value.z()));
      break;
    case LN_RigidBodyAttribute::AllowPhysicsRotation:
      physics_controller.SetRigidBody(bool_value);
      break;
  }
}

struct SelectedVehicleWheel {
  int32_t index = -1;
  const GameVehicleSettings *settings = nullptr;
};

static void logic_node_set_cursor_position(KX_KetsjiEngine *engine,
                                           const int32_t x,
                                           const int32_t y)
{
  if (engine == nullptr) {
    return;
  }

  if (RAS_ICanvas *canvas = engine->GetCanvas()) {
    canvas->SetMousePosition(x, y);
  }

  if (DEV_InputDevice *input_device = dynamic_cast<DEV_InputDevice *>(
          engine->GetInputDevice()))
  {
    input_device->ConvertMoveEvent(x, y);
  }
}

static const LN_SourceRef *logic_node_command_source_ref(
    const LN_CommandBuffer::Command &command)
{
  if (command.runtime_tree == nullptr) {
    return nullptr;
  }

  const std::shared_ptr<const LN_Program> program = command.runtime_tree->GetProgram();
  if (program == nullptr) {
    return nullptr;
  }

  const std::vector<LN_SourceRef> &source_refs = program->GetSourceRefs();
  if (command.source_ref_index >= source_refs.size()) {
    return nullptr;
  }
  return &source_refs[command.source_ref_index];
}

static const std::string *logic_node_command_string(const LN_CommandBuffer::Command &command,
                                                    const LN_Manager *logic_manager)
{
  if (command.event_subject_id.IsValid() && logic_manager != nullptr) {
    const std::string &value = logic_manager->DebugName(command.event_subject_id);
    if (!value.empty()) {
      return &value;
    }
  }

  if (command.property_name_id.IsValid() && command.runtime_tree != nullptr) {
    const std::shared_ptr<const LN_Program> program = command.runtime_tree->GetProgram();
    if (program != nullptr) {
      const std::string &value = program->GetString(command.property_name_id);
      if (!value.empty()) {
        return &value;
      }
    }
  }

  if (!command.property_name.empty()) {
    return &command.property_name;
  }
  return nullptr;
}

static const std::string *logic_node_command_game_property_name(
    const LN_CommandBuffer::Command &command,
    const LN_Manager *logic_manager)
{
  if (command.property_name_ptr != nullptr && !command.property_name_ptr->empty()) {
    return command.property_name_ptr;
  }

  if (command.game_property_id.IsValid() && logic_manager != nullptr) {
    const std::string &value = logic_manager->DebugName(command.game_property_id);
    if (!value.empty()) {
      return &value;
    }
  }

  if (command.runtime_tree != nullptr && command.property_ref_index != LN_INVALID_INDEX) {
    const std::shared_ptr<const LN_Program> program = command.runtime_tree->GetProgram();
    if (program != nullptr) {
      const std::vector<LN_GamePropertyRef> &property_refs = program->GetGamePropertyRefs();
      if (command.property_ref_index < property_refs.size()) {
        return &property_refs[command.property_ref_index].name;
      }
    }
  }

  return logic_node_command_string(command, logic_manager);
}

static bool logic_value_exp_number(const LN_Value &value, double &r_number)
{
  switch (value.type) {
    case LN_ValueType::Bool:
      r_number = value.bool_value ? 1.0 : 0.0;
      return true;
    case LN_ValueType::Int:
      r_number = double(value.int_value);
      return true;
    case LN_ValueType::Float:
      r_number = double(value.float_value);
      return true;
    case LN_ValueType::String:
      r_number = -1.0;
      return true;
    case LN_ValueType::Vector:
      r_number = double(value.vector_value.length());
      return true;
    case LN_ValueType::Rotation:
      r_number = double(value.rotation_euler_value.length());
      return true;
    case LN_ValueType::Vector4:
      r_number = double(value.vector4_value.length());
      return true;
    case LN_ValueType::Color:
      r_number = double(value.color_value.length());
      return true;
    case LN_ValueType::Matrix:
    case LN_ValueType::ObjectRef:
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
    case LN_ValueType::List:
    case LN_ValueType::Dict:
    case LN_ValueType::Generic:
    case LN_ValueType::None:
      return false;
  }

  return false;
}

static bool try_update_existing_game_property_no_alloc(EXP_Value *old_value, const LN_Value &value)
{
  if (old_value == nullptr) {
    return false;
  }

  const int old_type = old_value->GetValueType();
  double number = 0.0;
  if (logic_value_exp_number(value, number)) {
    if (old_type == VALUE_BOOL_TYPE) {
      static_cast<EXP_BoolValue *>(old_value)->SetBool(number != 0.0);
      return true;
    }
    if (old_type == VALUE_INT_TYPE) {
      static_cast<EXP_IntValue *>(old_value)->SetInt(cInt(number));
      return true;
    }
    if (old_type == VALUE_FLOAT_TYPE) {
      static_cast<EXP_FloatValue *>(old_value)->SetFloat(float(number));
      return true;
    }
  }

  switch (value.type) {
    case LN_ValueType::Bool:
      if (old_type == VALUE_BOOL_TYPE) {
        static_cast<EXP_BoolValue *>(old_value)->SetBool(value.bool_value);
        return true;
      }
      break;
    case LN_ValueType::Int:
      if (old_type == VALUE_INT_TYPE) {
        static_cast<EXP_IntValue *>(old_value)->SetInt(value.int_value);
        return true;
      }
      break;
    case LN_ValueType::Float:
      if (old_type == VALUE_FLOAT_TYPE) {
        static_cast<EXP_FloatValue *>(old_value)->SetFloat(value.float_value);
        return true;
      }
      break;
    case LN_ValueType::String:
      if (old_type == VALUE_STRING_TYPE) {
        static_cast<EXP_StringValue *>(old_value)->SetText(value.string_value);
        return true;
      }
      break;
    case LN_ValueType::Vector:
      if (old_type == VALUE_VECTOR3_TYPE) {
        static_cast<EXP_Vector3Value *>(old_value)->SetVector3(value.vector_value);
        return true;
      }
      break;
    case LN_ValueType::Rotation:
      if (old_type == VALUE_VECTOR3_TYPE) {
        static_cast<EXP_Vector3Value *>(old_value)->SetVector3(value.rotation_euler_value);
        return true;
      }
      break;
    case LN_ValueType::Vector4:
      if (old_type == VALUE_VECTOR4_TYPE) {
        static_cast<EXP_Vector4Value *>(old_value)->SetVector4(value.vector4_value);
        return true;
      }
      break;
    case LN_ValueType::Color:
      if (old_type == VALUE_VECTOR4_TYPE) {
        static_cast<EXP_Vector4Value *>(old_value)->SetVector4(value.color_value);
        return true;
      }
      break;
    case LN_ValueType::Matrix:
    case LN_ValueType::ObjectRef:
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
    case LN_ValueType::List:
    case LN_ValueType::Dict:
    case LN_ValueType::Generic:
    case LN_ValueType::None:
      break;
  }

  return false;
}

static EXP_Value *make_exp_value_for_game_property(const LN_Value &value)
{
  switch (value.type) {
    case LN_ValueType::Bool:
      return new EXP_BoolValue(value.bool_value);
    case LN_ValueType::Int:
      return new EXP_IntValue(value.int_value);
    case LN_ValueType::Float:
      return new EXP_FloatValue(value.float_value);
    case LN_ValueType::String:
      return new EXP_StringValue(value.string_value, "");
    case LN_ValueType::Vector:
      return new EXP_Vector3Value(value.vector_value);
    case LN_ValueType::Rotation:
      return new EXP_Vector3Value(value.rotation_euler_value);
    case LN_ValueType::Vector4:
      return new EXP_Vector4Value(value.vector4_value);
    case LN_ValueType::Color:
      return new EXP_Vector4Value(value.color_value);
    case LN_ValueType::Matrix:
    case LN_ValueType::ObjectRef:
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
    case LN_ValueType::List:
    case LN_ValueType::Dict:
    case LN_ValueType::Generic:
    case LN_ValueType::None:
      return nullptr;
  }

  return nullptr;
}

static bool apply_game_property_value(KX_GameObject *object,
                                      const std::string &property_name,
                                      const LN_Value &value)
{
  if (object == nullptr || property_name.empty()) {
    return false;
  }

  if (EXP_Value *old_value = object->GetProperty(property_name)) {
    if (try_update_existing_game_property_no_alloc(old_value, value)) {
      return true;
    }

    EXP_Value *new_value = make_exp_value_for_game_property(value);
    if (new_value == nullptr) {
      return false;
    }
    old_value->SetValue(new_value);
    new_value->Release();
    return true;
  }

  EXP_Value *new_value = make_exp_value_for_game_property(value);
  if (new_value == nullptr) {
    return false;
  }
  object->SetProperty(property_name, new_value);
  new_value->Release();
  return true;
}

static void warn_command_failure_once(const LN_CommandBuffer::Command &command,
                                      const std::string &reason)
{
  const std::string message = LN_DescribeCommandFailure(command, reason);
  static std::unordered_set<std::string> issued_warnings;
  if (!issued_warnings.insert(message).second) {
    return;
  }
  CM_Warning(message);
}

static bool add_object_source_is_inactive(KX_Scene *scene, KX_GameObject *source)
{
  if (scene == nullptr || source == nullptr) {
    return false;
  }
  EXP_ListValue<KX_GameObject> *inactive_list = scene->GetInactiveList();
  return inactive_list != nullptr && inactive_list->SearchValue(source);
}

static float add_object_life_seconds_to_scene_lifespan(const float life_seconds,
                                                       const bool full_copy)
{
  if (life_seconds <= 0.0f) {
    return 0.0f;
  }

  /* KX_Scene::Add*Object still receives the legacy frame-based life value and
   * stores the actual timebomb in seconds internally. Native Logic Nodes expose
   * Life as seconds, so adapt at the command boundary. */
  const float legacy_frame_seconds = full_copy ? 0.02f : 0.016666667f;
  return life_seconds / legacy_frame_seconds;
}

static std::string game_object_id_name(KX_GameObject *game_object)
{
  if (game_object == nullptr) {
    return std::string();
  }
  if (blender::Object *blender_object = game_object->GetBlenderObject()) {
    return blender_object->id.name + 2;
  }
  return game_object->GetName();
}

static void warn_add_object_source_not_inactive(const LN_CommandBuffer::Command &command,
                                               KX_Scene *scene,
                                               KX_GameObject *source,
                                               const std::string &fallback_name)
{
  const LN_SourceRef *source_ref = logic_node_command_source_ref(command);
  std::ostringstream warning_key;
  warning_key << reinterpret_cast<uintptr_t>(scene) << ':'
              << reinterpret_cast<uintptr_t>(command.runtime_tree) << ':'
              << reinterpret_cast<uintptr_t>(source) << ':' << command.source_ref_index << ':'
              << (source ? source->GetName() : fallback_name);
  if (source_ref != nullptr) {
    warning_key << ':' << source_ref->source_tree_name << ':' << source_ref->node_name << ':'
                << source_ref->node_idname;
  }

  static std::unordered_set<std::string> issued_warnings;
  if (!issued_warnings.insert(warning_key.str()).second) {
    return;
  }

  std::ostringstream warning;
  warning << "Logic Nodes Add Object skipped";
  if (source != nullptr) {
    warning << ": object \"" << source->GetName() << "\"";
  }
  else if (!fallback_name.empty()) {
    warning << ": object \"" << fallback_name << "\"";
  }
  warning << " must be in an inactive layer";

  if (scene != nullptr) {
    warning << " in scene \"" << scene->GetName() << "\"";
  }

  if (source_ref != nullptr) {
    if (!source_ref->source_tree_name.empty()) {
      warning << ", tree \"" << source_ref->source_tree_name << "\"";
    }
    if (!source_ref->node_name.empty()) {
      warning << ", node \"" << source_ref->node_name << "\"";
    }
    else if (!source_ref->node_idname.empty()) {
      warning << ", node \"" << source_ref->node_idname << "\"";
    }
  }
  warning << ". Hide the template object in the viewport before starting the game.";
  CM_Warning(warning.str());
}

#ifdef WITH_AUDASPACE
/**
 * Native logic sound v1 (2D / non-spatial):
 * - `PlaySound` resolves the `Sound` ID in `Main` at flush time, plays on `BKE_sound_get_device()`, and
 *   records `AUD_Handle` entries keyed by the sound's short name (two-letter ID prefix stripped).
 * - `StopSound` stops tracked handles for that name only; `StopAllSounds` clears the list then `stopAll()`.
 * - Cap evicts oldest entries; stopping releases handles only (datablocks remain in `Main`).
 */
constexpr size_t k_logic_native_sound_handle_cap = 256;
static std::vector<std::pair<std::string, AUD_Handle>> g_logic_native_sound_handles;

static void logic_native_sound_handles_stop_all()
{
  for (auto &entry : g_logic_native_sound_handles) {
    if (entry.second) {
      entry.second->stop();
    }
  }
  g_logic_native_sound_handles.clear();
}

static void logic_native_sound_handles_stop_by_name(const std::string &name)
{
  for (auto it = g_logic_native_sound_handles.begin(); it != g_logic_native_sound_handles.end();) {
    if (it->first == name && it->second) {
      it->second->stop();
      it = g_logic_native_sound_handles.erase(it);
    }
    else {
      ++it;
    }
  }
}

static void logic_native_sound_handles_pause_by_name(const std::string &name)
{
  for (const auto &entry : g_logic_native_sound_handles) {
    if (entry.first == name && entry.second) {
      entry.second->pause();
    }
  }
}

static void logic_native_sound_handles_resume_by_name(const std::string &name)
{
  for (const auto &entry : g_logic_native_sound_handles) {
    if (entry.first == name && entry.second) {
      entry.second->resume();
    }
  }
}

#ifdef WITH_AUDASPACE
static void logic_native_sound_apply_3d_from_speaker(const std::shared_ptr<aud::I3DHandle> &h3d,
                                                   KX_GameObject *speaker)
{
  if (!h3d || speaker == nullptr) {
    return;
  }
  KX_Scene *scene = speaker->GetScene();
  if (scene == nullptr) {
    return;
  }
  KX_Camera *camera = scene->GetActiveCamera();
  if (camera == nullptr) {
    return;
  }
  const MT_Matrix3x3 camera_to_world = camera->NodeGetWorldOrientation();
  const MT_Matrix3x3 world_to_camera = camera_to_world.inverse();
  MT_Vector3 position = world_to_camera *
                        (speaker->NodeGetWorldPosition() - camera->NodeGetWorldPosition());
  float data[4];
  position.getValue(data);
  h3d->setLocation(aud::Vector3(data[0], data[1], data[2]));

  MT_Vector3 velocity = world_to_camera *
                        (speaker->GetLinearVelocity() - camera->GetLinearVelocity());
  velocity.getValue(data);
  h3d->setVelocity(aud::Vector3(data[0], data[1], data[2]));

  const MT_Matrix3x3 orientation = world_to_camera * speaker->NodeGetWorldOrientation();
  const MT_Quaternion rotation = orientation.getRotation();
  rotation.getValue(data);
  h3d->setOrientation(aud::Quaternion(data[0], data[1], data[2], data[3]));
}
#endif /* WITH_AUDASPACE */

static void logic_native_sound_handles_register(const std::string &name, AUD_Handle handle)
{
  if (!handle) {
    return;
  }
  if (g_logic_native_sound_handles.size() >= k_logic_native_sound_handle_cap) {
    if (!g_logic_native_sound_handles.empty()) {
      if (g_logic_native_sound_handles.front().second) {
        g_logic_native_sound_handles.front().second->stop();
      }
      g_logic_native_sound_handles.erase(g_logic_native_sound_handles.begin());
    }
  }
  g_logic_native_sound_handles.push_back({name, std::move(handle)});
}

/* Cache of loaded AUD_Sound objects keyed by sound datablock name.
 * Avoids calling BKE_sound_load() every frame which resets and reloads from disk. */
static std::unordered_map<std::string, AUD_Sound> g_logic_native_sound_cache;

static AUD_Sound logic_native_sound_resolve(blender::Main *bmain, const std::string &name)
{
  auto it = g_logic_native_sound_cache.find(name);
  if (it != g_logic_native_sound_cache.end() && it->second) {
    return it->second;
  }
  blender::ID *id = BKE_libblock_find_name(bmain, int16_t(blender::ID_SO), name.c_str());
  blender::bSound *sound = reinterpret_cast<blender::bSound *>(id);
  if (sound == nullptr) {
    return nullptr;
  }
  BKE_sound_load_no_assert(bmain, sound);
  AUD_Sound snd = BKE_sound_playback_handle_get(sound);
  if (snd) {
    g_logic_native_sound_cache[name] = snd;
  }
  return snd;
}
#endif /* WITH_AUDASPACE */

int32_t SanitizeVSyncMode(const int32_t mode)
{
  switch (mode) {
    case VSYNC_ON:
    case VSYNC_OFF:
    case VSYNC_ADAPTIVE:
      return mode;
    default:
      return VSYNC_OFF;
  }
}

int CanvasSwapIntervalFromVSyncMode(const int32_t mode)
{
  switch (SanitizeVSyncMode(mode)) {
    case VSYNC_ON:
      return 1;
    case VSYNC_ADAPTIVE:
      return -1;
    case VSYNC_OFF:
    default:
      return 0;
  }
}

void NotifyLightUpdate(blender::Light *light)
{
  if (light == nullptr) {
    return;
  }

  DEG_id_tag_update(&light->id, 0);
  WM_main_add_notifier(NC_LAMP | ND_LIGHTING_DRAW, light);
}

bool EnsureLightDataUnique(KX_GameObject *game_object)
{
  KX_LightObject *light_object = dynamic_cast<KX_LightObject *>(game_object);
  if (light_object == nullptr) {
    return false;
  }

  Object *blender_object = light_object->GetBlenderObject();
  blender::Light *light = light_object->GetLight();
  if (blender_object == nullptr || light == nullptr) {
    return false;
  }

  if (light->id.us <= 1) {
    return true;
  }

  if (!BKE_id_copy_is_allowed(&light->id)) {
    return false;
  }

  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *context = engine ? engine->GetContext() : nullptr;
  blender::Main *bmain = context ? CTX_data_main(context) : nullptr;
  if (bmain == nullptr) {
    return false;
  }

  blender::Light *unique_light = reinterpret_cast<blender::Light *>(
      BKE_id_copy_ex(bmain, &light->id, nullptr, 0));
  if (unique_light == nullptr) {
    return false;
  }

  blender_object->data = reinterpret_cast<blender::ID *>(unique_light);
  id_us_min(&light->id);
  light_object->SetBlenderObject(blender_object);

  DEG_id_tag_update(&blender_object->id, blender::ID_RECALC_GEOMETRY);
  NotifyLightUpdate(unique_light);
  DEG_relations_tag_update(bmain);
  return true;
}

LN_VehicleAxis VehicleAxisFromInt(const int32_t axis)
{
  switch (axis) {
    case int32_t(LN_VehicleAxis::Rear):
      return LN_VehicleAxis::Rear;
    case int32_t(LN_VehicleAxis::Front):
      return LN_VehicleAxis::Front;
    case int32_t(LN_VehicleAxis::All):
    default:
      return LN_VehicleAxis::All;
  }
}

bool IsVehicleChassisObject(const Object *object)
{
  const GameVehicleSettings *settings = object ? object->vehicle : nullptr;
  if (settings == nullptr) {
    return false;
  }
  return settings->vehicle_type == OB_VEHICLE_TYPE_CHASSIS ||
         settings->vehicle_type == OB_VEHICLE_TYPE_MOTORCYCLE_CHASSIS;
}

bool IsVehicleWheelObject(const Object *object)
{
  const GameVehicleSettings *settings = object ? object->vehicle : nullptr;
  if (settings == nullptr) {
    return false;
  }
  return settings->vehicle_type == OB_VEHICLE_TYPE_WHEEL ||
         settings->vehicle_type == OB_VEHICLE_TYPE_MOTORCYCLE_WHEEL;
}

bool WheelMatchesAxis(const GameVehicleSettings *settings, const LN_VehicleAxis axis)
{
  if (axis == LN_VehicleAxis::All || settings == nullptr) {
    return true;
  }

  const bool has_steering = (settings->wheel_flags & OB_VEHICLE_WHEEL_USE_STEERING) != 0;
  if (axis == LN_VehicleAxis::Front) {
    return has_steering;
  }
  return !has_steering;
}

float EffectiveWheelRadius(const GameVehicleSettings *settings)
{
  return std::max(settings ? settings->wheel_radius : 0.5f, 0.01f);
}

float ConfiguredWheelEngineForce(const GameVehicleSettings *chassis_settings,
                                 const GameVehicleSettings *wheel_settings)
{
  if (wheel_settings != nullptr &&
      (wheel_settings->wheel_flags & OB_VEHICLE_WHEEL_OVERRIDE_ENGINE_FORCE) != 0 &&
      wheel_settings->wheel_engine_force > 1.0e-4f)
  {
    return wheel_settings->wheel_engine_force;
  }
  if (chassis_settings != nullptr && chassis_settings->engine_force > 1.0e-4f) {
    return chassis_settings->engine_force;
  }
  if (chassis_settings != nullptr && chassis_settings->engine_torque > 1.0e-4f) {
    return chassis_settings->engine_torque / EffectiveWheelRadius(wheel_settings);
  }
  return 0.0f;
}

float ConfiguredWheelBrakeTorque(const GameVehicleSettings *chassis_settings,
                                 const GameVehicleSettings *wheel_settings)
{
  if (wheel_settings != nullptr &&
      (wheel_settings->wheel_flags & OB_VEHICLE_WHEEL_USE_BRAKE) != 0)
  {
    return std::max(wheel_settings->wheel_brake, 0.0f);
  }
  if (chassis_settings != nullptr && chassis_settings->brake > 1.0e-4f) {
    return chassis_settings->brake;
  }
  return 0.0f;
}

float ConfiguredWheelSteeringAngle(const GameVehicleSettings *wheel_settings)
{
  if (wheel_settings == nullptr ||
      (wheel_settings->wheel_flags & OB_VEHICLE_WHEEL_USE_STEERING) == 0)
  {
    return 0.0f;
  }
  return std::max(wheel_settings->wheel_steering, 0.0f);
}

void ApplyVehicleActuatorControl(KX_GameObject *game_object,
                                 PHY_IVehicle *vehicle,
                                 const MT_Vector3 &control,
                                 const float steering)
{
  Object *blender_object = game_object ? game_object->GetBlenderObject() : nullptr;
  const GameVehicleSettings *vehicle_settings = blender_object ? blender_object->vehicle : nullptr;
  if (game_object == nullptr || vehicle == nullptr || vehicle_settings == nullptr) {
    return;
  }

  const int num_wheels = vehicle->GetNumWheels();
  if (num_wheels == 0) {
    return;
  }

  const float throttle = control.x();
  const float brake = std::max(control.y(), 0.0f);
  const float handbrake = std::max(control.z(), 0.0f);
  const float normalized_brake = std::clamp(brake, 0.0f, 1.0f);
  const float normalized_handbrake = std::clamp(handbrake, 0.0f, 1.0f);
  const bool is_jolt = game_object->GetScene() && game_object->GetScene()->GetBlenderScene() &&
                       static_cast<e_PhysicsEngine>(
                           game_object->GetScene()->GetBlenderScene()->gm.physicsEngine) ==
                           UseJolt;

  if (is_jolt) {
    vehicle->SetForwardInput(throttle);
    vehicle->SetBrakeInput(normalized_brake);
    vehicle->SetHandBrakeInput(normalized_handbrake);
    vehicle->SetRightInput(steering);
    return;
  }

  for (int wheel_index = 0; wheel_index < num_wheels; ++wheel_index) {
    vehicle->ApplyEngineForce(throttle * vehicle_settings->engine_force, wheel_index);
    vehicle->ApplyBraking(brake * vehicle_settings->brake, wheel_index);
    vehicle->ApplyHandBraking(handbrake * vehicle_settings->handbrake_torque, wheel_index);
    vehicle->SetSteeringValue(steering, wheel_index);
  }
}

std::vector<SelectedVehicleWheel> SelectVehicleWheels(KX_GameObject *chassis_game_object,
                                                      PHY_IVehicle *vehicle,
                                                      const LN_VehicleAxis axis,
                                                      const int32_t wheel_count)
{
  std::vector<SelectedVehicleWheel> wheels;
  if (chassis_game_object == nullptr || vehicle == nullptr) {
    return wheels;
  }

  KX_Scene *scene = chassis_game_object->GetScene();
  Object *chassis_object = chassis_game_object->GetBlenderObject();
  if (scene == nullptr || chassis_object == nullptr || !IsVehicleChassisObject(chassis_object)) {
    return wheels;
  }

  const int32_t vehicle_wheel_count = vehicle->GetNumWheels();
  wheels.reserve(std::max(vehicle_wheel_count, 0));

  int32_t wheel_index = 0;
  for (KX_GameObject *scene_object : *scene->GetObjectList()) {
    Object *blender_object = scene_object ? scene_object->GetBlenderObject() : nullptr;
    if (blender_object == nullptr || !IsVehicleWheelObject(blender_object) ||
        blender_object->vehicle == nullptr || blender_object->vehicle->chassis_object != chassis_object)
    {
      continue;
    }

    if (wheel_index >= vehicle_wheel_count) {
      break;
    }

    if (WheelMatchesAxis(blender_object->vehicle, axis)) {
      SelectedVehicleWheel wheel;
      wheel.index = wheel_index;
      wheel.settings = blender_object->vehicle;
      wheels.push_back(wheel);
    }
    wheel_index++;
  }

  if (wheels.empty()) {
    for (int32_t index = 0; index < vehicle_wheel_count; index++) {
      SelectedVehicleWheel wheel;
      wheel.index = index;
      wheels.push_back(wheel);
    }
  }

  if (axis != LN_VehicleAxis::All && wheel_count > 0 && int32_t(wheels.size()) > wheel_count) {
    wheels.resize(size_t(wheel_count));
  }

  return wheels;
}

bool IdHasCode(const blender::ID *id, const int16_t id_code)
{
  return id != nullptr && int16_t(*reinterpret_cast<const uint16_t *>(id->name)) == id_code;
}

blender::Main *CurrentMain(blender::bContext **r_context)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *context = engine ? engine->GetContext() : nullptr;
  if (r_context != nullptr) {
    *r_context = context;
  }

  blender::Main *bmain = context ? CTX_data_main(context) : nullptr;
  if (bmain == nullptr) {
    bmain = G_MAIN;
  }
  return bmain;
}

blender::Material *ResolveMaterialForCommand(blender::Main *bmain,
                                             LN_RuntimeTree *runtime_tree,
                                             const LN_RuntimeRef &runtime_ref,
                                             const std::string &fallback_name)
{
  if (runtime_tree != nullptr && runtime_ref.IsValid()) {
    blender::ID *id = runtime_tree->ResolveDatablockRef(runtime_ref);
    if (IdHasCode(id, int16_t(blender::ID_MA))) {
      return reinterpret_cast<blender::Material *>(id);
    }
  }

  if (bmain == nullptr || fallback_name.empty()) {
    return nullptr;
  }

  blender::ID *id = BKE_libblock_find_name(
      bmain, int16_t(blender::ID_MA), fallback_name.c_str());
  return reinterpret_cast<blender::Material *>(id);
}

blender::bNodeTree *ResolveGeometryNodeTreeForCommand(blender::Main *bmain,
                                                      LN_RuntimeTree *runtime_tree,
                                                      const LN_Value &value)
{
  blender::ID *id = nullptr;
  if (runtime_tree != nullptr && value.runtime_ref.IsValid()) {
    id = runtime_tree->ResolveDatablockRef(value.runtime_ref);
  }
  if (id == nullptr && bmain != nullptr && !value.reference_name.empty()) {
    id = BKE_libblock_find_name(
        bmain, int16_t(blender::ID_NT), value.reference_name.c_str());
  }
  if (!IdHasCode(id, int16_t(blender::ID_NT)) ||
      (id->tag & blender::ID_TAG_MISSING) != 0)
  {
    return nullptr;
  }
  blender::bNodeTree *tree = reinterpret_cast<blender::bNodeTree *>(id);
  if (tree->type != blender::NTREE_GEOMETRY || tree->geometry_node_asset_traits == nullptr ||
      (tree->geometry_node_asset_traits->flag & blender::GEO_NODE_ASSET_MODIFIER) == 0)
  {
    return nullptr;
  }
  return tree;
}

void WarnAssignGeometryNodesModifierFailureOnce(const blender::Object *object,
                                                const std::string &reason)
{
  std::string message = "Logic Nodes Assign Geometry Nodes Modifier";
  if (object != nullptr) {
    message += " on '" + std::string(object->id.name + 2) + "'";
  }
  message += " failed: " + reason;
  static std::unordered_set<std::string> issued_warnings;
  if (issued_warnings.insert(message).second) {
    CM_Warning(message);
  }
}

bool GeometryNodesModifierCanBeInsertedAt(const blender::Object &object, const int32_t index)
{
  const int32_t count = BLI_listbase_count(&object.modifiers);
  if (index < 0 || index > count) {
    return false;
  }
  int32_t current_index = 0;
  for (const blender::ModifierData &modifier : object.modifiers) {
    if (current_index < index && (modifier.flag & blender::eModifierFlag_PinLast) != 0) {
      return false;
    }
    if (current_index >= index) {
      const blender::ModifierTypeInfo *type_info = BKE_modifier_get_info(
          blender::ModifierType(modifier.type));
      if (type_info != nullptr &&
          (type_info->flags & blender::eModifierTypeFlag_RequiresOriginalData) != 0)
      {
        return false;
      }
    }
    current_index++;
  }
  return true;
}

bool SetGeometryNodesModifierGroup(blender::Main &bmain,
                                   blender::Scene *scene,
                                   blender::Object &object,
                                   blender::NodesModifierData &modifier,
                                   blender::bNodeTree &node_group)
{
  blender::PointerRNA modifier_ptr = blender::RNA_pointer_create_discrete(
      &object.id, blender::RNA_NodesModifier, &modifier);
  blender::PropertyRNA *property = blender::RNA_struct_find_property(&modifier_ptr, "node_group");
  if (property == nullptr) {
    return false;
  }
  blender::PointerRNA tree_ptr = blender::RNA_id_pointer_create(&node_group.id);
  blender::RNA_property_pointer_set(&modifier_ptr, property, tree_ptr, nullptr);
  if (modifier.node_group != &node_group) {
    return false;
  }
  blender::RNA_property_update_main(&bmain, scene, &modifier_ptr, property);
  return true;
}

KX_GameObject *ResolveObjectForCommand(LN_RuntimeTree *runtime_tree, const LN_Value &value)
{
  if (runtime_tree == nullptr || value.type != LN_ValueType::ObjectRef) {
    return nullptr;
  }

  if (value.runtime_ref.IsValid()) {
    if (KX_GameObject *game_object = runtime_tree->ResolveObjectRef(value.runtime_ref)) {
      return game_object;
    }
  }

  if (value.reference_name.empty()) {
    return nullptr;
  }

  KX_GameObject *owner = runtime_tree->GetGameObject();
  KX_Scene *scene = owner ? owner->GetScene() : nullptr;
  if (scene == nullptr) {
    return nullptr;
  }
  return static_cast<KX_GameObject *>(scene->GetObjectList()->FindValue(value.reference_name));
}

bool LogicValueAsBool(const LN_Value &value, bool &r_value);
bool LogicValueAsInt(const LN_Value &value, int &r_value);
bool LogicValueAsFloat(const LN_Value &value, float &r_value);
bool LogicValueAsFloatArray(const LN_Value &value,
                            std::array<float, 4> &r_values,
                            int &r_available);
bool LogicValueAsString(const LN_Value &value, std::string &r_value);
bool LogicValueAsSocketId(LN_RuntimeTree *runtime_tree,
                          const LN_Value &value,
                          blender::eNodeSocketDatatype socket_type,
                          blender::ID *&r_id);
blender::ID *SocketIdDefaultValue(const blender::bNodeSocket &socket);
bool SetSocketIdDefaultValue(blender::bNodeSocket &socket, blender::ID *id);

blender::bNode *FindNodeByUserName(blender::bNodeTree *ntree, const std::string &name)
{
  if (ntree == nullptr || name.empty()) {
    return nullptr;
  }

  if (blender::bNode *node = blender::bke::node_find_node_by_name(*ntree, name.c_str())) {
    return node;
  }

  blender::bNode *label_match = nullptr;
  for (blender::bNode &node : ntree->nodes) {
    if (node.label[0] != '\0' && name == node.label) {
      if (label_match != nullptr) {
        return nullptr;
      }
      label_match = &node;
    }
  }
  return label_match;
}

blender::bNode *FindMaterialNodeByUserName(blender::bNodeTree *ntree, const std::string &name)
{
  if (blender::bNode *node = FindNodeByUserName(ntree, name)) {
    return node;
  }

  if (ntree != nullptr && (name == "Principled BSDF" || name == "ShaderNodeBsdfPrincipled")) {
    for (blender::bNode &node : ntree->nodes) {
      if (STREQ(node.idname, "ShaderNodeBsdfPrincipled")) {
        return &node;
      }
    }
  }
  return nullptr;
}

blender::bNodeSocket *FindInputSocketByIdentifierOrName(blender::bNode &node,
                                                        const std::string &name)
{
  if (name.empty()) {
    return nullptr;
  }

  if (blender::bNodeSocket *socket = blender::bke::node_find_socket(node, blender::SOCK_IN, name)) {
    return socket;
  }

  for (blender::bNodeSocket &socket : node.inputs) {
    if (name == socket.identifier) {
      return &socket;
    }
  }
  blender::bNodeSocket *name_match = nullptr;
  for (blender::bNodeSocket &socket : node.inputs) {
    if (name == socket.name) {
      if (name_match != nullptr) {
        return nullptr;
      }
      name_match = &socket;
    }
  }
  return name_match;
}

blender::bNodeSocket *FindOutputSocketByIdentifierOrName(blender::bNode &node,
                                                         const std::string &name)
{
  if (name.empty()) {
    return nullptr;
  }

  if (blender::bNodeSocket *socket = blender::bke::node_find_socket(
          node, blender::SOCK_OUT, name))
  {
    return socket;
  }

  for (blender::bNodeSocket &socket : node.outputs) {
    if (name == socket.identifier || name == socket.name) {
      return &socket;
    }
  }
  return nullptr;
}

void ApplyRuntimeMaterialSlot(KX_GameObject *game_object,
                              blender::Material *material,
                              int slot_index);

blender::bPoseChannel *FindPoseChannel(KX_GameObject *armature_object,
                                       const std::string &bone_name)
{
  if (armature_object == nullptr || bone_name.empty()) {
    return nullptr;
  }
  blender::Object *ob = armature_object->GetBlenderObject();
  if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr) {
    return nullptr;
  }
  return BKE_pose_channel_find_name(ob->pose, bone_name.c_str());
}

blender::bConstraint *FindPoseConstraint(KX_GameObject *armature_object,
                                         const std::string &bone_name,
                                         const std::string &constraint_name)
{
  blender::bPoseChannel *pchan = FindPoseChannel(armature_object, bone_name);
  if (pchan == nullptr || constraint_name.empty()) {
    return nullptr;
  }
  return BKE_constraints_find_name(&pchan->constraints, constraint_name.c_str());
}

void UpdateArmatureObjectPose(KX_GameObject *armature_object)
{
  blender::Object *ob = armature_object ? armature_object->GetBlenderObject() : nullptr;
  if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr) {
    return;
  }
  if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
    if (blender::bContext *context = engine->GetContext()) {
      DEG_id_tag_update(&ob->id, blender::ID_RECALC_GEOMETRY);
      if (blender::Depsgraph *depsgraph = CTX_data_depsgraph_pointer(context)) {
        if (blender::Scene *scene = CTX_data_scene(context)) {
          BKE_pose_where_is(depsgraph, scene, ob);
        }
      }
    }
  }
}

LN_BonePoseLocationSpace NormalizeBonePoseLocationSpace(const int32_t value)
{
  switch (value) {
    case int32_t(LN_BonePoseLocationSpace::BoneLocalOffset):
      return LN_BonePoseLocationSpace::BoneLocalOffset;
    case int32_t(LN_BonePoseLocationSpace::Armature):
      return LN_BonePoseLocationSpace::Armature;
    case int32_t(LN_BonePoseLocationSpace::World):
      return LN_BonePoseLocationSpace::World;
    case int32_t(LN_BonePoseLocationSpace::PoseChannel):
      return LN_BonePoseLocationSpace::PoseChannel;
    case int32_t(LN_BonePoseLocationSpace::ArmatureOffset):
    default:
      return LN_BonePoseLocationSpace::ArmatureOffset;
  }
}

LN_BonePoseRotationSpace NormalizeBonePoseRotationSpace(const int32_t value)
{
  switch (value) {
    case int32_t(LN_BonePoseRotationSpace::Armature):
      return LN_BonePoseRotationSpace::Armature;
    case int32_t(LN_BonePoseRotationSpace::World):
      return LN_BonePoseRotationSpace::World;
    case int32_t(LN_BonePoseRotationSpace::PoseChannel):
    default:
      return LN_BonePoseRotationSpace::PoseChannel;
  }
}

void MatrixToFloat3x3(const MT_Matrix3x3 &matrix, float r_matrix[3][3])
{
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      r_matrix[row][column] = float(matrix[row][column]);
    }
  }
}

MT_Matrix3x3 MatrixFromFloat3x3(const float matrix[3][3])
{
  return MT_Matrix3x3(matrix[0][0],
                      matrix[0][1],
                      matrix[0][2],
                      matrix[1][0],
                      matrix[1][1],
                      matrix[1][2],
                      matrix[2][0],
                      matrix[2][1],
                      matrix[2][2]);
}

void VectorToFloat3(const MT_Vector3 &vector, float r_vector[3])
{
  r_vector[0] = float(vector.x());
  r_vector[1] = float(vector.y());
  r_vector[2] = float(vector.z());
}

MT_Vector3 ObjectAxesScaleToPoseChannelScale(const blender::bPoseChannel &pchan,
                                             const MT_Vector3 &scale)
{
  if (pchan.bone == nullptr) {
    return scale;
  }

  const float object_scale_mat[3][3] = {
      {float(scale.x()), 0.0f, 0.0f},
      {0.0f, float(scale.y()), 0.0f},
      {0.0f, 0.0f, float(scale.z())},
  };
  float rest_basis[3][3];
  float rest_basis_inv[3][3];
  float tmp_mat[3][3];
  float channel_scale_mat[3][3];
  blender::copy_m3_m4(rest_basis, pchan.bone->arm_mat);
  blender::normalize_m3(rest_basis);
  blender::transpose_m3_m3(rest_basis_inv, rest_basis);
  blender::mul_m3_m3m3(tmp_mat, object_scale_mat, rest_basis);
  blender::mul_m3_m3m3(channel_scale_mat, rest_basis_inv, tmp_mat);
  return MT_Vector3(
      channel_scale_mat[0][0], channel_scale_mat[1][1], channel_scale_mat[2][2]);
}

void ResolveArmatureOffsetBonePoseLocation(blender::bPoseChannel *pchan,
                                           const MT_Vector3 &location,
                                           float r_location[3])
{
  if (pchan != nullptr && pchan->bone != nullptr) {
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    float pose_offset[3];
    VectorToFloat3(location, pose_offset);

    float zero_channel[3];
    float offset_channel[3];
    blender::BKE_armature_loc_pose_to_bone({pchan, pchan->bone}, zero, zero_channel);
    blender::BKE_armature_loc_pose_to_bone({pchan, pchan->bone}, pose_offset, offset_channel);
    r_location[0] = offset_channel[0] - zero_channel[0];
    r_location[1] = offset_channel[1] - zero_channel[1];
    r_location[2] = offset_channel[2] - zero_channel[2];
    return;
  }

  VectorToFloat3(location, r_location);
}

bool ResolveBoneLocalOffsetPoseLocation(blender::bPoseChannel *pchan,
                                        const MT_Vector3 &location,
                                        float r_location[3])
{
  if (pchan == nullptr || pchan->bone == nullptr) {
    return false;
  }

  const float original_loc[3] = {pchan->loc[0], pchan->loc[1], pchan->loc[2]};
  float base_channel_mat[4][4];
  pchan->loc[0] = 0.0f;
  pchan->loc[1] = 0.0f;
  pchan->loc[2] = 0.0f;
  blender::BKE_pchan_to_mat4({pchan, pchan->bone}, base_channel_mat);
  pchan->loc[0] = original_loc[0];
  pchan->loc[1] = original_loc[1];
  pchan->loc[2] = original_loc[2];

  float base_pose_mat[4][4];
  blender::BKE_armature_mat_bone_to_pose({pchan, pchan->bone}, base_channel_mat, base_pose_mat);

  float pose_basis[3][3];
  blender::copy_m3_m4(pose_basis, pchan->pose_mat);
  blender::normalize_m3(pose_basis);

  float rest_basis[3][3];
  blender::copy_m3_m4(rest_basis, pchan->bone->arm_mat);
  blender::normalize_m3(rest_basis);

  float rest_basis_inv[3][3];
  blender::transpose_m3_m3(rest_basis_inv, rest_basis);

  float local_basis[3][3];
  blender::mul_m3_m3m3(local_basis, pose_basis, rest_basis_inv);
  blender::normalize_m3(local_basis);

  float local_offset[3];
  VectorToFloat3(location, local_offset);
  const float pose_offset[3] = {
      local_basis[0][0] * local_offset[0] + local_basis[1][0] * local_offset[1] +
          local_basis[2][0] * local_offset[2],
      local_basis[0][1] * local_offset[0] + local_basis[1][1] * local_offset[1] +
          local_basis[2][1] * local_offset[2],
      local_basis[0][2] * local_offset[0] + local_basis[1][2] * local_offset[1] +
          local_basis[2][2] * local_offset[2],
  };

  const float pose_location[3] = {
      base_pose_mat[3][0] + pose_offset[0],
      base_pose_mat[3][1] + pose_offset[1],
      base_pose_mat[3][2] + pose_offset[2],
  };
  blender::BKE_armature_loc_pose_to_bone({pchan, pchan->bone}, pose_location, r_location);
  return true;
}

LN_BonePoseRotationSpace RotationSpaceFromTransformSpace(const LN_BonePoseLocationSpace space)
{
  switch (space) {
    case LN_BonePoseLocationSpace::Armature:
      return LN_BonePoseRotationSpace::Armature;
    case LN_BonePoseLocationSpace::World:
      return LN_BonePoseRotationSpace::World;
    case LN_BonePoseLocationSpace::ArmatureOffset:
    case LN_BonePoseLocationSpace::BoneLocalOffset:
    case LN_BonePoseLocationSpace::PoseChannel:
    default:
      return LN_BonePoseRotationSpace::PoseChannel;
  }
}

bool ResolveBonePoseLocation(KX_GameObject *armature_object,
                             blender::bPoseChannel *pchan,
                             const MT_Vector3 &location,
                             const LN_BonePoseLocationSpace space,
                             const bool use_center,
                             float r_location[3])
{
  if (space == LN_BonePoseLocationSpace::ArmatureOffset) {
    ResolveArmatureOffsetBonePoseLocation(pchan, location, r_location);
    return true;
  }
  if (space == LN_BonePoseLocationSpace::BoneLocalOffset) {
    UpdateArmatureObjectPose(armature_object);
    return ResolveBoneLocalOffsetPoseLocation(pchan, location, r_location);
  }
  if (space == LN_BonePoseLocationSpace::PoseChannel) {
    VectorToFloat3(location, r_location);
    return true;
  }
  if (armature_object == nullptr || pchan == nullptr || pchan->bone == nullptr) {
    return false;
  }

  float pose_location[3];
  VectorToFloat3(location, pose_location);
  if (space == LN_BonePoseLocationSpace::World) {
    MT_Transform world_to_armature;
    world_to_armature.invert(armature_object->NodeGetWorldTransform());
    const MT_Vector3 local_location = world_to_armature(location);
    VectorToFloat3(local_location, pose_location);
  }
  if (use_center) {
    pose_location[0] -= (pchan->pose_tail[0] - pchan->pose_head[0]) * 0.5f;
    pose_location[1] -= (pchan->pose_tail[1] - pchan->pose_head[1]) * 0.5f;
    pose_location[2] -= (pchan->pose_tail[2] - pchan->pose_head[2]) * 0.5f;
  }

  blender::BKE_armature_loc_pose_to_bone({pchan, pchan->bone}, pose_location, r_location);
  return true;
}

void SetPoseChannelRotationFromEuler(blender::bPoseChannel &pchan,
                                     const MT_Vector3 &rotation_euler)
{
  const float euler[3] = {
      float(rotation_euler.x()), float(rotation_euler.y()), float(rotation_euler.z())};
  float object_axes_mat[3][3];
  eulO_to_mat3(object_axes_mat, euler, blender::EULER_ORDER_DEFAULT);

  if (pchan.bone != nullptr) {
    float rest_mat[3][3];
    float rest_inv[3][3];
    float tmp_mat[3][3];
    float channel_mat[3][3];
    /* Pose channels are evaluated as rest-bone-basis * channel rotation. Convert object-axis
     * input into the channel basis so X/Y/Z match normal armature/object axes, not bone axes. */
    blender::copy_m3_m4(rest_mat, pchan.bone->arm_mat);
    blender::normalize_m3(rest_mat);
    blender::transpose_m3_m3(rest_inv, rest_mat);
    blender::mul_m3_m3m3(tmp_mat, object_axes_mat, rest_mat);
    blender::mul_m3_m3m3(channel_mat, rest_inv, tmp_mat);
    blender::normalize_m3(channel_mat);
    blender::BKE_pchan_mat3_to_rot(&pchan, channel_mat, false);
    return;
  }

  blender::BKE_pchan_mat3_to_rot(&pchan, object_axes_mat, false);
}

bool SetPoseChannelRotation(KX_GameObject *armature_object,
                            blender::Object *armature_blender_object,
                            blender::bPoseChannel *pchan,
                            const MT_Vector3 &rotation_euler,
                            const LN_BonePoseRotationSpace space)
{
  if (pchan == nullptr) {
    return false;
  }
  if (space == LN_BonePoseRotationSpace::PoseChannel) {
    SetPoseChannelRotationFromEuler(*pchan, rotation_euler);
    return true;
  }
  if (armature_object == nullptr || armature_blender_object == nullptr || pchan->bone == nullptr) {
    return false;
  }

  const float euler[3] = {
      float(rotation_euler.x()), float(rotation_euler.y()), float(rotation_euler.z())};
  float pose_rot[3][3];
  eulO_to_mat3(pose_rot, euler, blender::EULER_ORDER_DEFAULT);

  if (space == LN_BonePoseRotationSpace::World) {
    const MT_Matrix3x3 armature_to_world = armature_object->NodeGetWorldOrientation();
    MatrixToFloat3x3(armature_to_world.inverse() * MatrixFromFloat3x3(pose_rot), pose_rot);
  }

  float pose_mat[4][4];
  blender::unit_m4(pose_mat);
  blender::copy_m4_m3(pose_mat, pose_rot);

  float channel_mat[4][4];
  blender::BKE_armature_mat_pose_to_bone_ex(
      nullptr, armature_blender_object, pchan, pose_mat, channel_mat);

  float channel_rot[3][3];
  blender::copy_m3_m4(channel_rot, channel_mat);
  blender::normalize_m3(channel_rot);
  blender::BKE_pchan_mat3_to_rot(pchan, channel_rot, false);
  return true;
}

void PoseChannelCenter(const blender::bPoseChannel &pchan, float r_center[3]);
bool SetPoseChannelCenter(KX_GameObject *armature_object,
                          blender::bPoseChannel *pchan,
                          const float target_center[3]);

bool SetPoseChannelTransform(KX_GameObject *armature_object,
                             blender::Object *armature_blender_object,
                             blender::bPoseChannel *pchan,
                             const MT_Vector3 &location,
                             const MT_Vector3 &rotation_euler,
                             const LN_BonePoseLocationSpace space,
                             const bool use_center)
{
  if (armature_object == nullptr || armature_blender_object == nullptr || pchan == nullptr) {
    return false;
  }

  float pose_location[3];
  if (!ResolveBonePoseLocation(armature_object, pchan, location, space, use_center, pose_location)) {
    return false;
  }
  pchan->loc[0] = pose_location[0];
  pchan->loc[1] = pose_location[1];
  pchan->loc[2] = pose_location[2];
  UpdateArmatureObjectPose(armature_object);

  float center[3] = {0.0f, 0.0f, 0.0f};
  if (use_center) {
    if (pchan->bone == nullptr) {
      return false;
    }
    PoseChannelCenter(*pchan, center);
  }

  if (!SetPoseChannelRotation(armature_object,
                              armature_blender_object,
                              pchan,
                              rotation_euler,
                              RotationSpaceFromTransformSpace(space)))
  {
    return false;
  }
  if (use_center && !SetPoseChannelCenter(armature_object, pchan, center)) {
    return false;
  }
  UpdateArmatureObjectPose(armature_object);
  return true;
}

void PoseChannelCenter(const blender::bPoseChannel &pchan, float r_center[3])
{
  r_center[0] = (pchan.pose_head[0] + pchan.pose_tail[0]) * 0.5f;
  r_center[1] = (pchan.pose_head[1] + pchan.pose_tail[1]) * 0.5f;
  r_center[2] = (pchan.pose_head[2] + pchan.pose_tail[2]) * 0.5f;
}

bool SetPoseChannelCenter(KX_GameObject *armature_object,
                          blender::bPoseChannel *pchan,
                          const float target_center[3])
{
  if (armature_object == nullptr || pchan == nullptr || pchan->bone == nullptr) {
    return false;
  }

  UpdateArmatureObjectPose(armature_object);

  const float target_head[3] = {
      target_center[0] - (pchan->pose_tail[0] - pchan->pose_head[0]) * 0.5f,
      target_center[1] - (pchan->pose_tail[1] - pchan->pose_head[1]) * 0.5f,
      target_center[2] - (pchan->pose_tail[2] - pchan->pose_head[2]) * 0.5f,
  };
  blender::BKE_armature_loc_pose_to_bone({pchan, pchan->bone}, target_head, pchan->loc);
  return true;
}

bool SetBoneFlag(blender::Bone &bone, const blender::eBone_Flag flag, const bool enabled)
{
  if (enabled) {
    bone.flag |= flag;
  }
  else {
    bone.flag &= ~flag;
  }
  return true;
}

bool IsSupportedSetBoneAttribute(const int32_t attribute)
{
  switch (attribute) {
    case 2:
    case 13:
    case 15:
    case 16:
    case 17:
    case 18:
      return true;
    default:
      return false;
  }
}

bool SetSocketDefaultFromLogicValue(LN_RuntimeTree *runtime_tree,
                                    blender::bNodeSocket &socket,
                                    const LN_Value &value,
                                    bool *r_changed = nullptr);

bool LogicFloatAlmostEqual(const float a, const float b)
{
  return std::fabs(a - b) <= 1.0e-6f;
}

bool SocketDefaultNeedsLogicValueUpdate(LN_RuntimeTree *runtime_tree,
                                        const blender::bNodeSocket &socket,
                                        const LN_Value &value,
                                        bool &r_needs_update)
{
  if (socket.default_value == nullptr) {
    return false;
  }

  switch (socket.type) {
    case blender::SOCK_BOOLEAN: {
      bool bool_value = false;
      if (!LogicValueAsBool(value, bool_value)) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueBoolean *>(socket.default_value);
      r_needs_update = socket_value->value != bool_value;
      return true;
    }
    case blender::SOCK_INT: {
      int int_value = 0;
      if (!LogicValueAsInt(value, int_value)) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueInt *>(socket.default_value);
      r_needs_update = socket_value->value != int_value;
      return true;
    }
    case blender::SOCK_MENU: {
      int int_value = 0;
      if (!LogicValueAsInt(value, int_value)) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueMenu *>(socket.default_value);
      if (socket_value->enum_items == nullptr || socket_value->has_conflict() ||
          socket_value->enum_items->find_item_by_identifier(int_value) == nullptr)
      {
        return false;
      }
      r_needs_update = socket_value->value != int_value;
      return true;
    }
    case blender::SOCK_INT_VECTOR: {
      std::array<float, 4> values{};
      int available = 0;
      auto *socket_value = static_cast<blender::bNodeSocketValueIntVector *>(
          socket.default_value);
      const int dimensions = std::clamp(socket_value->dimensions, 1, 3);
      if (!LogicValueAsFloatArray(value, values, available) || available < dimensions) {
        return false;
      }
      r_needs_update = false;
      for (int i = 0; i < dimensions; ++i) {
        r_needs_update |= socket_value->value[i] != int(values[i]);
      }
      return true;
    }
    case blender::SOCK_FLOAT: {
      float float_value = 0.0f;
      if (!LogicValueAsFloat(value, float_value)) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueFloat *>(socket.default_value);
      r_needs_update = !LogicFloatAlmostEqual(socket_value->value, float_value);
      return true;
    }
    case blender::SOCK_VECTOR: {
      std::array<float, 4> values{};
      int available = 0;
      if (!LogicValueAsFloatArray(value, values, available) || available < 3) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueVector *>(socket.default_value);
      r_needs_update = !LogicFloatAlmostEqual(socket_value->value[0], values[0]) ||
                       !LogicFloatAlmostEqual(socket_value->value[1], values[1]) ||
                       !LogicFloatAlmostEqual(socket_value->value[2], values[2]);
      return true;
    }
    case blender::SOCK_ROTATION: {
      std::array<float, 4> values{};
      int available = 0;
      if (!LogicValueAsFloatArray(value, values, available) || available < 3) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueRotation *>(socket.default_value);
      r_needs_update = !LogicFloatAlmostEqual(socket_value->value_euler[0], values[0]) ||
                       !LogicFloatAlmostEqual(socket_value->value_euler[1], values[1]) ||
                       !LogicFloatAlmostEqual(socket_value->value_euler[2], values[2]);
      return true;
    }
    case blender::SOCK_RGBA: {
      std::array<float, 4> values{};
      int available = 0;
      if (!LogicValueAsFloatArray(value, values, available) || available < 3) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueRGBA *>(socket.default_value);
      const float alpha = available >= 4 ? values[3] : 1.0f;
      r_needs_update = !LogicFloatAlmostEqual(socket_value->value[0], values[0]) ||
                       !LogicFloatAlmostEqual(socket_value->value[1], values[1]) ||
                       !LogicFloatAlmostEqual(socket_value->value[2], values[2]) ||
                       !LogicFloatAlmostEqual(socket_value->value[3], alpha);
      return true;
    }
    case blender::SOCK_STRING: {
      std::string string_value;
      if (!LogicValueAsString(value, string_value)) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueString *>(socket.default_value);
      r_needs_update = !STREQ(socket_value->value, string_value.c_str());
      return true;
    }
    case blender::SOCK_OBJECT:
    case blender::SOCK_IMAGE:
    case blender::SOCK_COLLECTION:
    case blender::SOCK_TEXTURE:
    case blender::SOCK_MATERIAL:
    case blender::SOCK_FONT:
    case blender::SOCK_SCENE:
    case blender::SOCK_TEXT_ID:
    case blender::SOCK_MASK:
    case blender::SOCK_SOUND: {
      blender::ID *id = nullptr;
      if (!LogicValueAsSocketId(runtime_tree,
                                value,
                                blender::eNodeSocketDatatype(socket.type),
                                id))
      {
        return false;
      }
      r_needs_update = SocketIdDefaultValue(socket) != id;
      return true;
    }
    default:
      return false;
  }
}

bool SetSocketDefaultFromLogicValue(LN_RuntimeTree *runtime_tree,
                                    blender::bNodeSocket &socket,
                                    const LN_Value &value,
                                    bool *r_changed)
{
  bool needs_update = true;
  if (!SocketDefaultNeedsLogicValueUpdate(runtime_tree, socket, value, needs_update)) {
    return false;
  }
  if (r_changed != nullptr) {
    *r_changed = needs_update;
  }
  if (!needs_update) {
    return true;
  }

  switch (socket.type) {
    case blender::SOCK_BOOLEAN: {
      bool bool_value = false;
      if (!LogicValueAsBool(value, bool_value)) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueBoolean *>(socket.default_value);
      socket_value->value = bool_value;
      return true;
    }
    case blender::SOCK_INT: {
      int int_value = 0;
      if (!LogicValueAsInt(value, int_value)) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueInt *>(socket.default_value);
      socket_value->value = int_value;
      return true;
    }
    case blender::SOCK_MENU: {
      int int_value = 0;
      if (!LogicValueAsInt(value, int_value)) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueMenu *>(socket.default_value);
      if (socket_value->enum_items == nullptr || socket_value->has_conflict() ||
          socket_value->enum_items->find_item_by_identifier(int_value) == nullptr)
      {
        return false;
      }
      socket_value->value = int_value;
      return true;
    }
    case blender::SOCK_INT_VECTOR: {
      std::array<float, 4> values{};
      int available = 0;
      auto *socket_value = static_cast<blender::bNodeSocketValueIntVector *>(
          socket.default_value);
      const int dimensions = std::clamp(socket_value->dimensions, 1, 3);
      if (!LogicValueAsFloatArray(value, values, available) || available < dimensions) {
        return false;
      }
      for (int i = 0; i < dimensions; ++i) {
        socket_value->value[i] = int(values[i]);
      }
      return true;
    }
    case blender::SOCK_FLOAT: {
      float float_value = 0.0f;
      if (!LogicValueAsFloat(value, float_value)) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueFloat *>(socket.default_value);
      socket_value->value = float_value;
      return true;
    }
    case blender::SOCK_VECTOR: {
      std::array<float, 4> values{};
      int available = 0;
      if (!LogicValueAsFloatArray(value, values, available) || available < 3) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueVector *>(socket.default_value);
      socket_value->value[0] = values[0];
      socket_value->value[1] = values[1];
      socket_value->value[2] = values[2];
      return true;
    }
    case blender::SOCK_ROTATION: {
      std::array<float, 4> values{};
      int available = 0;
      if (!LogicValueAsFloatArray(value, values, available) || available < 3) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueRotation *>(socket.default_value);
      socket_value->value_euler[0] = values[0];
      socket_value->value_euler[1] = values[1];
      socket_value->value_euler[2] = values[2];
      return true;
    }
    case blender::SOCK_RGBA: {
      std::array<float, 4> values{};
      int available = 0;
      if (!LogicValueAsFloatArray(value, values, available) || available < 3) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueRGBA *>(socket.default_value);
      socket_value->value[0] = values[0];
      socket_value->value[1] = values[1];
      socket_value->value[2] = values[2];
      socket_value->value[3] = available >= 4 ? values[3] : 1.0f;
      return true;
    }
    case blender::SOCK_STRING: {
      std::string string_value;
      if (!LogicValueAsString(value, string_value)) {
        return false;
      }
      auto *socket_value = static_cast<blender::bNodeSocketValueString *>(socket.default_value);
      blender::BLI_strncpy(socket_value->value, string_value.c_str(), sizeof(socket_value->value));
      return true;
    }
    case blender::SOCK_OBJECT:
    case blender::SOCK_IMAGE:
    case blender::SOCK_COLLECTION:
    case blender::SOCK_TEXTURE:
    case blender::SOCK_MATERIAL:
    case blender::SOCK_FONT:
    case blender::SOCK_SCENE:
    case blender::SOCK_TEXT_ID:
    case blender::SOCK_MASK:
    case blender::SOCK_SOUND: {
      blender::ID *id = nullptr;
      if (!LogicValueAsSocketId(runtime_tree,
                                value,
                                blender::eNodeSocketDatatype(socket.type),
                                id))
      {
        return false;
      }
      return SetSocketIdDefaultValue(socket, id);
    }
    default:
      return false;
  }
}

void NotifyMaterialNodeTreeUpdate(blender::Main *bmain,
                                  blender::Material *material,
                                  blender::bNodeTree *ntree);

bool MaterialParameterSocketSupportsObjectAttribute(const blender::bNodeSocket &socket)
{
  return ELEM(socket.type,
              blender::SOCK_BOOLEAN,
              blender::SOCK_INT,
              blender::SOCK_FLOAT,
              blender::SOCK_VECTOR,
              blender::SOCK_ROTATION,
              blender::SOCK_RGBA);
}

bool LogicValueAsMaterialParameterValues(const blender::bNodeSocket &socket,
                                         const LN_Value &value,
                                         std::array<float, 4> &r_values)
{
  switch (socket.type) {
    case blender::SOCK_BOOLEAN: {
      bool bool_value = false;
      if (!LogicValueAsBool(value, bool_value)) {
        return false;
      }
      const float scalar = bool_value ? 1.0f : 0.0f;
      r_values = {scalar, scalar, scalar, 1.0f};
      return true;
    }
    case blender::SOCK_INT: {
      int int_value = 0;
      if (!LogicValueAsInt(value, int_value)) {
        return false;
      }
      const float scalar = float(int_value);
      r_values = {scalar, scalar, scalar, 1.0f};
      return true;
    }
    case blender::SOCK_FLOAT: {
      float float_value = 0.0f;
      if (!LogicValueAsFloat(value, float_value)) {
        return false;
      }
      r_values = {float_value, float_value, float_value, 1.0f};
      return true;
    }
    case blender::SOCK_VECTOR:
    case blender::SOCK_ROTATION: {
      int available = 0;
      if (!LogicValueAsFloatArray(value, r_values, available) || available < 3) {
        return false;
      }
      r_values[3] = 1.0f;
      return true;
    }
    case blender::SOCK_RGBA: {
      int available = 0;
      if (!LogicValueAsFloatArray(value, r_values, available) || available < 3) {
        return false;
      }
      r_values[3] = available >= 4 ? r_values[3] : 1.0f;
      return true;
    }
    default:
      return false;
  }
}

bool SocketDefaultAsMaterialParameterValues(const blender::bNodeSocket &socket,
                                            std::array<float, 4> &r_values)
{
  if (socket.default_value == nullptr) {
    r_values = {0.0f, 0.0f, 0.0f, 1.0f};
    return MaterialParameterSocketSupportsObjectAttribute(socket);
  }

  switch (socket.type) {
    case blender::SOCK_BOOLEAN: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueBoolean *>(
          socket.default_value);
      const float scalar = socket_value->value ? 1.0f : 0.0f;
      r_values = {scalar, scalar, scalar, 1.0f};
      return true;
    }
    case blender::SOCK_INT: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueInt *>(
          socket.default_value);
      const float scalar = float(socket_value->value);
      r_values = {scalar, scalar, scalar, 1.0f};
      return true;
    }
    case blender::SOCK_FLOAT: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueFloat *>(
          socket.default_value);
      r_values = {socket_value->value, socket_value->value, socket_value->value, 1.0f};
      return true;
    }
    case blender::SOCK_VECTOR: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueVector *>(
          socket.default_value);
      r_values = {socket_value->value[0],
                  socket_value->value[1],
                  socket_value->value[2],
                  1.0f};
      return true;
    }
    case blender::SOCK_ROTATION: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueRotation *>(
          socket.default_value);
      r_values = {socket_value->value_euler[0],
                  socket_value->value_euler[1],
                  socket_value->value_euler[2],
                  1.0f};
      return true;
    }
    case blender::SOCK_RGBA: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueRGBA *>(
          socket.default_value);
      r_values = {socket_value->value[0],
                  socket_value->value[1],
                  socket_value->value[2],
                  socket_value->value[3]};
      return true;
    }
    default:
      return false;
  }
}

bool MaterialParameterValuesAlmostEqual(const std::array<float, 4> &a,
                                        const std::array<float, 4> &b)
{
  return LogicFloatAlmostEqual(a[0], b[0]) && LogicFloatAlmostEqual(a[1], b[1]) &&
         LogicFloatAlmostEqual(a[2], b[2]) && LogicFloatAlmostEqual(a[3], b[3]);
}

bool ObjectMaterialParameterPropertyIsValid(blender::Object *object,
                                            const std::string &property_name)
{
  if (object == nullptr || property_name.empty()) {
    return false;
  }

  blender::IDProperty *properties = blender::IDP_GetProperties(&object->id);
  blender::IDProperty *property = properties ?
                                      blender::IDP_GetPropertyFromGroup_null(
                                          properties, property_name) :
                                      nullptr;
  if (property == nullptr || property->type != blender::IDP_ARRAY ||
      property->subtype != blender::IDP_FLOAT || property->len != 4)
  {
    return false;
  }

  return true;
}

bool SetObjectMaterialParameterProperty(blender::Object *object,
                                        const std::string &property_name,
                                        const std::array<float, 4> &values)
{
  if (object == nullptr || property_name.empty()) {
    return false;
  }

  blender::IDProperty *properties = blender::IDP_EnsureProperties(&object->id);
  if (properties == nullptr) {
    return false;
  }

  blender::IDProperty *property = blender::IDP_GetPropertyFromGroup_null(
      properties, property_name);
  if (property != nullptr && property->type == blender::IDP_ARRAY &&
      property->subtype == blender::IDP_FLOAT && property->len == 4)
  {
    float *data = static_cast<float *>(property->data.pointer);
    const std::array<float, 4> existing = {data[0], data[1], data[2], data[3]};
    if (MaterialParameterValuesAlmostEqual(existing, values)) {
      return false;
    }
    data[0] = values[0];
    data[1] = values[1];
    data[2] = values[2];
    data[3] = values[3];
    return true;
  }

  if (property != nullptr) {
    blender::IDP_FreeFromGroup(properties, property);
  }

  blender::IDPropertyTemplate template_value{};
  template_value.array.len = 4;
  template_value.array.type = blender::IDP_FLOAT;
  property = blender::IDP_New(blender::IDP_ARRAY, &template_value, property_name);
  if (property == nullptr) {
    return false;
  }
  if (!blender::IDP_AddToGroup(properties, property)) {
    blender::IDP_FreeProperty(property);
    return false;
  }

  float *data = static_cast<float *>(property->data.pointer);
  data[0] = values[0];
  data[1] = values[1];
  data[2] = values[2];
  data[3] = values[3];
  return true;
}

bool EnsureObjectMaterialParameterDefaultProperty(blender::Object *object,
                                                  const std::string &property_name,
                                                  const std::array<float, 4> &default_values)
{
  if (ObjectMaterialParameterPropertyIsValid(object, property_name)) {
    return false;
  }
  return SetObjectMaterialParameterProperty(object, property_name, default_values);
}

bool ObjectUsesMaterial(const blender::Object *object, blender::Material *material)
{
  if (object == nullptr || material == nullptr) {
    return false;
  }
  for (int slot = 0; slot < object->totcol; ++slot) {
    if (BKE_object_material_get(const_cast<blender::Object *>(object), short(slot + 1)) ==
        material)
    {
      return true;
    }
  }
  return false;
}

void TagMaterialParameterObjectUpdate(KX_GameObject *game_object, blender::Object *object)
{
  if (game_object == nullptr || object == nullptr) {
    return;
  }
  const blender::IDRecalcFlag recalc_flags = static_cast<blender::IDRecalcFlag>(
      blender::ID_RECALC_SHADING | blender::ID_RECALC_TRANSFORM);
  if (KX_Scene *scene = game_object->GetScene()) {
    const bool overlay_only = (object->gameflag & blender::OB_OVERLAY_COLLECTION) != 0;
    scene->AppendToIdsToUpdate(&object->id, recalc_flags, overlay_only);
  }
  else {
    DEG_id_tag_update(&object->id, recalc_flags);
  }
}

void InitializeMaterialParameterDefaultsInObjectList(EXP_ListValue<KX_GameObject> *objects,
                                                     blender::Material *material,
                                                     const std::string &attribute_name,
                                                     const std::array<float, 4> &default_values)
{
  if (objects == nullptr) {
    return;
  }

  for (KX_GameObject *game_object : *objects) {
    blender::Object *object = game_object ? game_object->GetBlenderObject() : nullptr;
    if (object == nullptr || !ObjectUsesMaterial(object, material)) {
      continue;
    }
    if (EnsureObjectMaterialParameterDefaultProperty(object, attribute_name, default_values)) {
      TagMaterialParameterObjectUpdate(game_object, object);
    }
  }
}

void InitializeMaterialParameterDefaults(blender::Material *material,
                                         const std::string &attribute_name,
                                         const std::array<float, 4> &default_values)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  EXP_ListValue<KX_Scene> *scenes = engine ? engine->CurrentScenes() : nullptr;
  if (scenes == nullptr) {
    return;
  }

  for (int scene_index = 0; scene_index < scenes->GetCount(); ++scene_index) {
    KX_Scene *scene = scenes->GetValue(scene_index);
    if (scene == nullptr) {
      continue;
    }
    InitializeMaterialParameterDefaultsInObjectList(
        scene->GetObjectList(), material, attribute_name, default_values);
    InitializeMaterialParameterDefaultsInObjectList(
        scene->GetInactiveList(), material, attribute_name, default_values);
  }
}

std::string MaterialParameterAttributeName(blender::Material *material,
                                           const blender::bNode &node,
                                           const blender::bNodeSocket &socket)
{
  const uint32_t material_hash = blender::BLI_hash_string(material ? material->id.name : "");
  uint32_t socket_hash = uint32_t(node.identifier);
  socket_hash = blender::BLI_hash_int_2d(socket_hash,
                                         blender::BLI_hash_string(socket.identifier));
  char name[MAX_IDPROP_NAME];
  std::snprintf(name, sizeof(name), "_LNMP_%08x_%08x", material_hash, socket_hash);
  return name;
}

blender::bNodeSocket *FindAttributeOutputForSocket(blender::bNode &attribute_node,
                                                   const blender::bNodeSocket &target_socket)
{
  switch (target_socket.type) {
    case blender::SOCK_RGBA:
      return FindOutputSocketByIdentifierOrName(attribute_node, "Color");
    case blender::SOCK_VECTOR:
    case blender::SOCK_ROTATION:
      return FindOutputSocketByIdentifierOrName(attribute_node, "Vector");
    case blender::SOCK_BOOLEAN:
    case blender::SOCK_INT:
    case blender::SOCK_FLOAT: {
      if (blender::bNodeSocket *socket = FindOutputSocketByIdentifierOrName(attribute_node,
                                                                            "Fac"))
      {
        return socket;
      }
      return FindOutputSocketByIdentifierOrName(attribute_node, "Factor");
    }
    default:
      return nullptr;
  }
}

blender::bNode *FindObjectAttributeNode(blender::bNodeTree &ntree,
                                        const std::string &attribute_name)
{
  for (blender::bNode &node : ntree.nodes) {
    if (node.type_legacy != SH_NODE_ATTRIBUTE || node.storage == nullptr) {
      continue;
    }
    const auto *storage = static_cast<const blender::NodeShaderAttribute *>(node.storage);
    if (storage->type == blender::SHD_ATTRIBUTE_OBJECT && attribute_name == storage->name) {
      return &node;
    }
  }
  return nullptr;
}

bool MaterialSocketAlreadyBoundToAttribute(blender::bNodeTree &ntree,
                                           blender::bNode &target_node,
                                           blender::bNodeSocket &target_socket,
                                           blender::bNode &attribute_node,
                                           blender::bNodeSocket &attribute_output)
{
  bool found_incoming_link = false;
  for (blender::bNodeLink &link : ntree.links) {
    if (link.tonode != &target_node || link.tosock != &target_socket) {
      continue;
    }
    if (link.fromnode != &attribute_node || link.fromsock != &attribute_output) {
      return false;
    }
    found_incoming_link = true;
  }
  return found_incoming_link;
}

bool EnsureMaterialParameterObjectAttributeBinding(blender::Main *bmain,
                                                  blender::Material *material,
                                                  blender::bNodeTree &ntree,
                                                  blender::bNode &target_node,
                                                  blender::bNodeSocket &target_socket,
                                                  const std::string &attribute_name)
{
  if (!MaterialParameterSocketSupportsObjectAttribute(target_socket)) {
    return false;
  }

  blender::bNode *attribute_node = FindObjectAttributeNode(ntree, attribute_name);
  bool changed = false;
  if (attribute_node == nullptr) {
    attribute_node = blender::bke::node_add_static_node(nullptr, ntree, SH_NODE_ATTRIBUTE);
    if (attribute_node == nullptr || attribute_node->storage == nullptr) {
      return false;
    }
    auto *storage = static_cast<blender::NodeShaderAttribute *>(attribute_node->storage);
    storage->type = blender::SHD_ATTRIBUTE_OBJECT;
    blender::BLI_strncpy(storage->name, attribute_name.c_str(), sizeof(storage->name));
    blender::BLI_strncpy(
        attribute_node->label, "Logic Material Parameter", sizeof(attribute_node->label));
    attribute_node->flag &= ~blender::NODE_SELECT;
    changed = true;
  }

  blender::bNodeSocket *attribute_output = FindAttributeOutputForSocket(
      *attribute_node, target_socket);
  if (attribute_output == nullptr) {
    return false;
  }

  if (!MaterialSocketAlreadyBoundToAttribute(
          ntree, target_node, target_socket, *attribute_node, *attribute_output))
  {
    std::vector<blender::bNodeLink *> incoming_links;
    for (blender::bNodeLink &link : ntree.links) {
      if (link.tonode == &target_node && link.tosock == &target_socket) {
        incoming_links.push_back(&link);
      }
    }
    for (blender::bNodeLink *link : incoming_links) {
      blender::bke::node_remove_link(&ntree, *link);
    }
    blender::bke::node_add_link(
        ntree, *attribute_node, *attribute_output, target_node, target_socket);
    changed = true;
  }

  if (changed) {
    NotifyMaterialNodeTreeUpdate(bmain, material, &ntree);
  }
  return true;
}

bool ConstantStringExpressionValue(const LN_Program &program,
                                   const uint32_t expression_index,
                                   std::string &r_value)
{
  if (expression_index == LN_INVALID_INDEX ||
      expression_index >= program.GetStringExpressions().size())
  {
    return false;
  }
  const LN_StringExpression &expression = program.GetStringExpressions()[expression_index];
  if (expression.kind != LN_StringExpressionKind::Constant) {
    return false;
  }
  r_value = expression.string_value;
  if (r_value.empty() && expression.string_id.IsValid()) {
    r_value = program.GetString(expression.string_id);
  }
  return !r_value.empty();
}

bool ConstantIntExpressionValue(const LN_Program &program,
                                const uint32_t expression_index,
                                int32_t &r_value)
{
  if (expression_index == LN_INVALID_INDEX) {
    r_value = 0;
    return true;
  }
  if (expression_index >= program.GetIntExpressions().size()) {
    return false;
  }
  const LN_IntExpression &expression = program.GetIntExpressions()[expression_index];
  if (expression.kind != LN_IntExpressionKind::Constant) {
    return false;
  }
  r_value = expression.int_value;
  return true;
}

KX_GameObject *ConstantMaterialParameterTargetObject(LN_RuntimeTree &runtime_tree,
                                                    const LN_Program &program,
                                                    const uint32_t expression_index)
{
  if (expression_index == LN_INVALID_INDEX) {
    return runtime_tree.GetGameObject();
  }
  if (expression_index >= program.GetValueExpressions().size()) {
    return nullptr;
  }
  const LN_ValueExpression &expression = program.GetValueExpressions()[expression_index];
  switch (expression.kind) {
    case LN_ValueExpressionKind::OwnerObject:
      return runtime_tree.GetGameObject();
    case LN_ValueExpressionKind::Constant:
      return ResolveObjectForCommand(&runtime_tree, expression.value);
    default:
      return nullptr;
  }
}

int MaterialLightLayer(KX_GameObject *game_object)
{
  const Object *blender_object = game_object ? game_object->GetBlenderObject() : nullptr;
  return blender_object ? blender_object->lay : (1 << 20) - 1;
}

KX_BlenderMaterial *EnsureSceneRuntimeMaterial(KX_Scene *scene,
                                              blender::Material *material,
                                              const int light_layer)
{
  if (scene == nullptr || material == nullptr) {
    return nullptr;
  }

  BL_SceneConverter *scene_converter = scene->GetBlenderSceneConverter();
  if (scene_converter == nullptr) {
    return nullptr;
  }

  if (KX_BlenderMaterial *game_material = scene_converter->FindMaterial(material)) {
    return game_material;
  }

  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  RAS_Rasterizer *rasterizer = engine ? engine->GetRasterizer() : nullptr;
  if (rasterizer == nullptr) {
    return nullptr;
  }

  std::string name = material->id.name;
  if (name.empty()) {
    name = "MA";
  }

  auto runtime_material = std::make_unique<KX_BlenderMaterial>(
      rasterizer, scene, material, name, light_layer, true);
  KX_BlenderMaterial *game_material = scene_converter->OwnRuntimeMaterial(
      std::move(runtime_material), material);
  game_material->OnConstruction();
  return game_material;
}

void AppendMaterialIdsToUpdate(blender::Material *material, blender::bNodeTree *ntree)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  if (engine == nullptr) {
    return;
  }

  EXP_ListValue<KX_Scene> *scenes = engine->CurrentScenes();
  if (scenes == nullptr) {
    return;
  }

  for (int scene_index = 0; scene_index < scenes->GetCount(); ++scene_index) {
    KX_Scene *scene = scenes->GetValue(scene_index);
    BL_SceneConverter *scene_converter = scene ? scene->GetBlenderSceneConverter() : nullptr;
    if (scene_converter == nullptr || scene_converter->FindMaterial(material) == nullptr) {
      continue;
    }

    scene->AppendToIdsToUpdate(&material->id, blender::ID_RECALC_SHADING, false);
    if (ntree != nullptr) {
      scene->AppendToIdsToUpdate(&ntree->id, static_cast<blender::IDRecalcFlag>(0), false);
    }
  }
}

void AppendNodeTreeIdToUpdate(blender::bNodeTree *ntree)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  EXP_ListValue<KX_Scene> *scenes = engine ? engine->CurrentScenes() : nullptr;
  if (ntree == nullptr || scenes == nullptr) {
    return;
  }

  for (int scene_index = 0; scene_index < scenes->GetCount(); ++scene_index) {
    if (KX_Scene *scene = scenes->GetValue(scene_index)) {
      scene->AppendToIdsToUpdate(
          &ntree->id, static_cast<blender::IDRecalcFlag>(0), false);
    }
  }
}

void NotifyMaterialNodeTreeUpdate(blender::Main *bmain,
                                  blender::Material *material,
                                  blender::bNodeTree *ntree)
{
  if (material == nullptr || ntree == nullptr) {
    return;
  }

  if (bmain != nullptr) {
    BKE_main_ensure_invariants(*bmain, ntree->id);
  }

  GPU_material_free(&material->gpumaterial);
  DEG_id_tag_update(&material->id, blender::ID_RECALC_SHADING);
  AppendMaterialIdsToUpdate(material, ntree);
}

void ApplyRuntimeMaterialSlot(KX_GameObject *game_object,
                              blender::Material *material,
                              const int slot_index)
{
  if (game_object == nullptr || material == nullptr || slot_index < 0) {
    return;
  }

  KX_Scene *scene = game_object->GetScene();
  KX_BlenderMaterial *game_material = EnsureSceneRuntimeMaterial(
      scene, material, MaterialLightLayer(game_object));
  if (scene == nullptr || game_material == nullptr) {
    return;
  }

  bool bucket_created = false;
  RAS_MaterialBucket *bucket = scene->FindBucket(game_material, bucket_created);
  if (bucket == nullptr) {
    return;
  }

  for (int mesh_index = 0; mesh_index < game_object->GetMeshCount(); ++mesh_index) {
    RAS_MeshObject *mesh = game_object->GetMesh(mesh_index);
    if (mesh == nullptr) {
      continue;
    }

    for (int material_index = 0; material_index < mesh->NumMaterials(); ++material_index) {
      RAS_MeshMaterial *mesh_material = mesh->GetMeshMaterial(material_index);
      if (mesh_material == nullptr || int(mesh_material->GetIndex()) != slot_index) {
        continue;
      }
      mesh_material->ReplaceMaterial(bucket);
    }
  }
}

blender::Material *EnsureObjectSlotMaterialUniqueForEdit(blender::Main *bmain,
                                                         KX_GameObject *game_object,
                                                         const int slot_index)
{
  if (bmain == nullptr || game_object == nullptr || slot_index < 0) {
    return nullptr;
  }

  Object *object = game_object->GetBlenderObject();
  if (object == nullptr) {
    return nullptr;
  }
  if (slot_index >= object->totcol) {
    return nullptr;
  }

  blender::Material *material = BKE_object_material_get(object, short(slot_index + 1));
  if (material == nullptr) {
    return nullptr;
  }

  const bool object_slot = slot_index < object->totcol && object->matbits != nullptr &&
                           object->matbits[slot_index] != 0;
  const blender::ID *object_data = static_cast<const blender::ID *>(object->data);
  const bool shared_object_data = !object_slot && object_data != nullptr && object_data->us > 1;
  if (object_slot && material->id.us <= 1) {
    return material;
  }
  if (!object_slot && !shared_object_data && material->id.us <= 1) {
    return material;
  }

  if (!BKE_id_copy_is_allowed(&material->id)) {
    return nullptr;
  }

  blender::Material *unique_material = reinterpret_cast<blender::Material *>(
      BKE_id_copy_ex(bmain, &material->id, nullptr, 0));
  if (unique_material == nullptr) {
    return nullptr;
  }
  id_us_min(&unique_material->id);

  BKE_object_material_assign(
      bmain, object, unique_material, short(slot_index + 1), blender::BKE_MAT_ASSIGN_OBJECT);
  ApplyRuntimeMaterialSlot(game_object, unique_material, slot_index);
  DEG_id_tag_update(&object->id, blender::ID_RECALC_GEOMETRY);
  DEG_relations_tag_update(bmain);
  return unique_material;
}

bool LogicValueAsBool(const LN_Value &value, bool &r_value)
{
  switch (value.type) {
    case LN_ValueType::Bool:
      r_value = value.bool_value;
      return true;
    case LN_ValueType::Int:
      r_value = value.int_value != 0;
      return true;
    case LN_ValueType::Float:
      r_value = value.float_value != 0.0f;
      return true;
    default:
      return false;
  }
}

bool LogicValueAsInt(const LN_Value &value, int &r_value)
{
  switch (value.type) {
    case LN_ValueType::Bool:
      r_value = value.bool_value ? 1 : 0;
      return true;
    case LN_ValueType::Int:
      r_value = value.int_value;
      return true;
    case LN_ValueType::Float:
      r_value = int(value.float_value);
      return true;
    default:
      return false;
  }
}

bool LogicValueAsFloat(const LN_Value &value, float &r_value)
{
  switch (value.type) {
    case LN_ValueType::Bool:
      r_value = value.bool_value ? 1.0f : 0.0f;
      return true;
    case LN_ValueType::Int:
      r_value = float(value.int_value);
      return true;
    case LN_ValueType::Float:
      r_value = value.float_value;
      return true;
    default:
      return false;
  }
}

bool LogicValueAsFloatArray(const LN_Value &value,
                            std::array<float, 4> &r_values,
                            int &r_available)
{
  switch (value.type) {
    case LN_ValueType::Vector:
      r_values = {value.vector_value.x(), value.vector_value.y(), value.vector_value.z(), 0.0f};
      r_available = 3;
      return true;
    case LN_ValueType::Vector4:
      r_values = {value.vector4_value.x(),
                  value.vector4_value.y(),
                  value.vector4_value.z(),
                  value.vector4_value.w()};
      r_available = 4;
      return true;
    case LN_ValueType::Rotation:
      r_values = {value.rotation_euler_value.x(),
                  value.rotation_euler_value.y(),
                  value.rotation_euler_value.z(),
                  0.0f};
      r_available = 3;
      return true;
    case LN_ValueType::Color:
      r_values = {value.color_value.x(),
                  value.color_value.y(),
                  value.color_value.z(),
                  value.color_value.w()};
      r_available = 4;
      return true;
    default:
      r_available = 0;
      return false;
  }
}

bool LogicValueAsString(const LN_Value &value, std::string &r_value)
{
  switch (value.type) {
    case LN_ValueType::String:
      r_value = value.string_value;
      return true;
    case LN_ValueType::ObjectRef:
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
      r_value = value.reference_name;
      return !r_value.empty();
    default:
      return false;
  }
}

std::string LogicFloatToPrintString(float value)
{
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

std::string LogicValueToPrintString(const LN_Value &value, int depth = 0)
{
  if (!value.exists && value.type == LN_ValueType::None) {
    return "None";
  }

  switch (value.type) {
    case LN_ValueType::Bool:
      return value.bool_value ? "True" : "False";
    case LN_ValueType::Int:
      return std::to_string(value.int_value);
    case LN_ValueType::Float:
      return LogicFloatToPrintString(value.float_value);
    case LN_ValueType::Vector:
      return "(" + LogicFloatToPrintString(value.vector_value.x()) + ", " +
             LogicFloatToPrintString(value.vector_value.y()) + ", " +
             LogicFloatToPrintString(value.vector_value.z()) + ")";
    case LN_ValueType::Vector4:
      return "(" + LogicFloatToPrintString(value.vector4_value.x()) + ", " +
             LogicFloatToPrintString(value.vector4_value.y()) + ", " +
             LogicFloatToPrintString(value.vector4_value.z()) + ", " +
             LogicFloatToPrintString(value.vector4_value.w()) + ")";
    case LN_ValueType::Matrix: {
      std::string result = "(";
      for (int row = 0; row < 3; row++) {
        if (row != 0) {
          result += ", ";
        }
        result += "(" + LogicFloatToPrintString(value.matrix_value[row][0]) + ", " +
                  LogicFloatToPrintString(value.matrix_value[row][1]) + ", " +
                  LogicFloatToPrintString(value.matrix_value[row][2]) + ")";
      }
      result += ")";
      return result;
    }
    case LN_ValueType::Color:
      return "(" + LogicFloatToPrintString(value.color_value.x()) + ", " +
             LogicFloatToPrintString(value.color_value.y()) + ", " +
             LogicFloatToPrintString(value.color_value.z()) + ", " +
             LogicFloatToPrintString(value.color_value.w()) + ")";
    case LN_ValueType::Rotation:
      return "(" + LogicFloatToPrintString(value.rotation_euler_value.x()) + ", " +
             LogicFloatToPrintString(value.rotation_euler_value.y()) + ", " +
             LogicFloatToPrintString(value.rotation_euler_value.z()) + ")";
    case LN_ValueType::String:
      return value.string_value;
    case LN_ValueType::ObjectRef:
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
      return value.reference_name.empty() ? "None" : value.reference_name;
    case LN_ValueType::List: {
      if (depth >= 4) {
        return "[...]";
      }
      std::string result = "[";
      for (size_t i = 0; i < value.list_value.size(); i++) {
        if (i != 0) {
          result += ", ";
        }
        result += LogicValueToPrintString(value.list_value[i], depth + 1);
      }
      result += "]";
      return result;
    }
    case LN_ValueType::Dict: {
      if (depth >= 4) {
        return "{...}";
      }
      std::string result = "{";
      size_t index = 0;
      for (const auto &item : value.dict_value) {
        if (index != 0) {
          result += ", ";
        }
        result += item.first + ": " + LogicValueToPrintString(item.second, depth + 1);
        index++;
      }
      result += "}";
      return result;
    }
    case LN_ValueType::Generic:
    case LN_ValueType::None:
      return "None";
  }

  return "None";
}

bool LogicValueAsPointer(LN_RuntimeTree *runtime_tree,
                         const LN_Value &value,
                         blender::PointerRNA &r_value)
{
  r_value = blender::PointerRNA_NULL;
  if (value.type == LN_ValueType::None) {
    return true;
  }
  if (runtime_tree == nullptr) {
    return false;
  }

  if (value.type == LN_ValueType::ObjectRef) {
    if (!value.runtime_ref.IsValid()) {
      return false;
    }
    KX_GameObject *game_object = runtime_tree->ResolveObjectRef(value.runtime_ref);
    Object *object = game_object ? game_object->GetBlenderObject() : nullptr;
    if (object == nullptr) {
      return false;
    }
    r_value = blender::RNA_id_pointer_create(&object->id);
    return true;
  }
  if (value.type == LN_ValueType::SceneRef && value.runtime_ref.IsValid()) {
    KX_Scene *scene = runtime_tree->ResolveSceneRef(value.runtime_ref);
    blender::Scene *blender_scene = scene ? scene->GetBlenderScene() : nullptr;
    if (blender_scene == nullptr) {
      return false;
    }
    r_value = blender::RNA_id_pointer_create(&blender_scene->id);
    return true;
  }
  if (value.type == LN_ValueType::CollectionRef && value.runtime_ref.IsValid()) {
    blender::Collection *collection = runtime_tree->ResolveCollectionRef(value.runtime_ref);
    if (collection == nullptr) {
      return false;
    }
    r_value = blender::RNA_id_pointer_create(&collection->id);
    return true;
  }
  if (value.type == LN_ValueType::DatablockRef && value.runtime_ref.IsValid()) {
    blender::ID *id = runtime_tree->ResolveDatablockRef(value.runtime_ref);
    if (id == nullptr) {
      return false;
    }
    r_value = blender::RNA_id_pointer_create(id);
    return true;
  }
  return false;
}

int16_t ExpectedIdCodeForSocket(const blender::eNodeSocketDatatype socket_type)
{
  switch (socket_type) {
    case blender::SOCK_OBJECT:
      return int16_t(blender::ID_OB);
    case blender::SOCK_IMAGE:
      return int16_t(blender::ID_IM);
    case blender::SOCK_COLLECTION:
      return int16_t(blender::ID_GR);
    case blender::SOCK_TEXTURE:
      return int16_t(blender::ID_TE);
    case blender::SOCK_MATERIAL:
      return int16_t(blender::ID_MA);
    case blender::SOCK_FONT:
      return int16_t(blender::ID_VF);
    case blender::SOCK_SCENE:
      return int16_t(blender::ID_SCE);
    case blender::SOCK_TEXT_ID:
      return int16_t(blender::ID_TXT);
    case blender::SOCK_MASK:
      return int16_t(blender::ID_MSK);
    case blender::SOCK_SOUND:
      return int16_t(blender::ID_SO);
    default:
      return 0;
  }
}

bool LogicValueAsSocketId(LN_RuntimeTree *runtime_tree,
                          const LN_Value &value,
                          const blender::eNodeSocketDatatype socket_type,
                          blender::ID *&r_id)
{
  r_id = nullptr;
  if (value.type == LN_ValueType::None) {
    return true;
  }
  if (socket_type == blender::SOCK_MATERIAL) {
    blender::Material *material = ResolveMaterialForCommand(
        CurrentMain(nullptr), runtime_tree, value.runtime_ref, value.reference_name);
    if (material == nullptr) {
      return false;
    }
    r_id = &material->id;
    return true;
  }
  blender::PointerRNA value_ptr;
  if (!LogicValueAsPointer(runtime_tree, value, value_ptr) || value_ptr.owner_id == nullptr) {
    return false;
  }
  const int16_t expected_code = ExpectedIdCodeForSocket(socket_type);
  if (expected_code == 0 || !IdHasCode(value_ptr.owner_id, expected_code)) {
    return false;
  }
  r_id = value_ptr.owner_id;
  return true;
}

blender::ID *SocketIdDefaultValue(const blender::bNodeSocket &socket)
{
  switch (socket.type) {
    case blender::SOCK_OBJECT:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueObject *>(socket.default_value)->value);
    case blender::SOCK_IMAGE:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueImage *>(socket.default_value)->value);
    case blender::SOCK_COLLECTION:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueCollection *>(socket.default_value)->value);
    case blender::SOCK_TEXTURE:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueTexture *>(socket.default_value)->value);
    case blender::SOCK_MATERIAL:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueMaterial *>(socket.default_value)->value);
    case blender::SOCK_FONT:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueFont *>(socket.default_value)->value);
    case blender::SOCK_SCENE:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueScene *>(socket.default_value)->value);
    case blender::SOCK_TEXT_ID:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueText *>(socket.default_value)->value);
    case blender::SOCK_MASK:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueMask *>(socket.default_value)->value);
    case blender::SOCK_SOUND:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueSound *>(socket.default_value)->value);
    default:
      return nullptr;
  }
}

bool SetSocketIdDefaultValue(blender::bNodeSocket &socket, blender::ID *id)
{
  blender::ID *previous_id = SocketIdDefaultValue(socket);
  if (previous_id == id) {
    return true;
  }
  if (id != nullptr) {
    blender::id_us_plus(id);
  }
  if (previous_id != nullptr) {
    blender::id_us_min(previous_id);
  }

  switch (socket.type) {
    case blender::SOCK_OBJECT:
      static_cast<blender::bNodeSocketValueObject *>(socket.default_value)->value =
          reinterpret_cast<blender::Object *>(id);
      return true;
    case blender::SOCK_IMAGE:
      static_cast<blender::bNodeSocketValueImage *>(socket.default_value)->value =
          reinterpret_cast<blender::Image *>(id);
      return true;
    case blender::SOCK_COLLECTION:
      static_cast<blender::bNodeSocketValueCollection *>(socket.default_value)->value =
          reinterpret_cast<blender::Collection *>(id);
      return true;
    case blender::SOCK_TEXTURE:
      static_cast<blender::bNodeSocketValueTexture *>(socket.default_value)->value =
          reinterpret_cast<blender::Tex *>(id);
      return true;
    case blender::SOCK_MATERIAL:
      static_cast<blender::bNodeSocketValueMaterial *>(socket.default_value)->value =
          reinterpret_cast<blender::Material *>(id);
      return true;
    case blender::SOCK_FONT:
      static_cast<blender::bNodeSocketValueFont *>(socket.default_value)->value =
          reinterpret_cast<blender::VFont *>(id);
      return true;
    case blender::SOCK_SCENE:
      static_cast<blender::bNodeSocketValueScene *>(socket.default_value)->value =
          reinterpret_cast<blender::Scene *>(id);
      return true;
    case blender::SOCK_TEXT_ID:
      static_cast<blender::bNodeSocketValueText *>(socket.default_value)->value =
          reinterpret_cast<blender::Text *>(id);
      return true;
    case blender::SOCK_MASK:
      static_cast<blender::bNodeSocketValueMask *>(socket.default_value)->value =
          reinterpret_cast<blender::Mask *>(id);
      return true;
    case blender::SOCK_SOUND:
      static_cast<blender::bNodeSocketValueSound *>(socket.default_value)->value =
          reinterpret_cast<blender::bSound *>(id);
      return true;
    default:
      if (id != nullptr) {
        blender::id_us_min(id);
      }
      if (previous_id != nullptr) {
        blender::id_us_plus(previous_id);
      }
      return false;
  }
}

bool RnaPropertyNeedsLogicValueUpdate(blender::bContext *context,
                                      LN_RuntimeTree *runtime_tree,
                                      blender::PointerRNA &target_ptr,
                                      blender::PropertyRNA *prop,
                                      const LN_Value &value,
                                      bool &r_needs_update)
{
  if (prop == nullptr || !blender::RNA_property_editable(&target_ptr, prop)) {
    return false;
  }

  const int array_length = blender::RNA_property_array_check(prop) ?
                               blender::RNA_property_array_length(&target_ptr, prop) :
                               0;
  if (array_length > 4) {
    return false;
  }

  switch (blender::RNA_property_type(prop)) {
    case blender::PROP_BOOLEAN: {
      bool bool_value = false;
      if (!LogicValueAsBool(value, bool_value) || array_length != 0) {
        return false;
      }
      r_needs_update = blender::RNA_property_boolean_get(&target_ptr, prop) != bool_value;
      return true;
    }
    case blender::PROP_INT: {
      if (array_length != 0) {
        std::array<float, 4> values{};
        int available = 0;
        if (!LogicValueAsFloatArray(value, values, available) || available < array_length) {
          return false;
        }
        std::array<int, 4> current{};
        blender::RNA_property_int_get_array_at_most(
            &target_ptr, prop, current.data(), array_length);
        r_needs_update = false;
        for (int i = 0; i < array_length; i++) {
          r_needs_update |= current[i] != int(values[i]);
        }
        return true;
      }
      int int_value = 0;
      if (!LogicValueAsInt(value, int_value)) {
        return false;
      }
      r_needs_update = blender::RNA_property_int_get(&target_ptr, prop) != int_value;
      return true;
    }
    case blender::PROP_FLOAT: {
      if (array_length != 0) {
        std::array<float, 4> values{};
        int available = 0;
        if (!LogicValueAsFloatArray(value, values, available) || available < array_length) {
          return false;
        }
        std::array<float, 4> current{};
        blender::RNA_property_float_get_array_at_most(
            &target_ptr, prop, current.data(), array_length);
        r_needs_update = false;
        for (int i = 0; i < array_length; i++) {
          r_needs_update |= !LogicFloatAlmostEqual(current[i], values[i]);
        }
        return true;
      }
      float float_value = 0.0f;
      if (!LogicValueAsFloat(value, float_value)) {
        return false;
      }
      r_needs_update = !LogicFloatAlmostEqual(blender::RNA_property_float_get(&target_ptr, prop),
                                              float_value);
      return true;
    }
    case blender::PROP_STRING: {
      std::string string_value;
      if (!LogicValueAsString(value, string_value)) {
        return false;
      }
      r_needs_update = blender::RNA_property_string_get(&target_ptr, prop) != string_value;
      return true;
    }
    case blender::PROP_ENUM: {
      int enum_value = 0;
      if (value.type == LN_ValueType::Int) {
        enum_value = value.int_value;
      }
      else if (value.type == LN_ValueType::String) {
        if (!blender::RNA_property_enum_value(
                context, &target_ptr, prop, value.string_value.c_str(), &enum_value))
        {
          return false;
        }
      }
      else {
        return false;
      }
      r_needs_update = blender::RNA_property_enum_get(&target_ptr, prop) != enum_value;
      return true;
    }
    case blender::PROP_POINTER: {
      blender::PointerRNA pointer_value;
      if (!LogicValueAsPointer(runtime_tree, value, pointer_value)) {
        return false;
      }
      const blender::PointerRNA current = blender::RNA_property_pointer_get(&target_ptr, prop);
      r_needs_update = current.data != pointer_value.data;
      return true;
    }
    default:
      return false;
  }
}

bool SetRnaPropertyValue(blender::bContext *context,
                         LN_RuntimeTree *runtime_tree,
                         blender::PointerRNA &target_ptr,
                         blender::PropertyRNA *prop,
                         const LN_Value &value)
{
  if (prop == nullptr || !blender::RNA_property_editable(&target_ptr, prop)) {
    return false;
  }

  switch (blender::RNA_property_type(prop)) {
    case blender::PROP_BOOLEAN: {
      bool bool_value = false;
      if (!LogicValueAsBool(value, bool_value)) {
        return false;
      }
      blender::RNA_property_boolean_set(&target_ptr, prop, bool_value);
      return true;
    }
    case blender::PROP_INT: {
      if (blender::RNA_property_array_check(prop)) {
        std::array<float, 4> float_values{};
        int available = 0;
        if (!LogicValueAsFloatArray(value, float_values, available)) {
          return false;
        }
        const int length = std::min(blender::RNA_property_array_length(&target_ptr, prop),
                                    available);
        if (length <= 0) {
          return false;
        }
        std::array<int, 4> int_values{};
        for (int i = 0; i < length; i++) {
          int_values[i] = int(float_values[i]);
        }
        blender::RNA_property_int_set_array_at_most(&target_ptr, prop, int_values.data(), length);
        return true;
      }
      int int_value = 0;
      if (!LogicValueAsInt(value, int_value)) {
        return false;
      }
      blender::RNA_property_int_set(&target_ptr, prop, int_value);
      return true;
    }
    case blender::PROP_FLOAT: {
      if (blender::RNA_property_array_check(prop)) {
        std::array<float, 4> values{};
        int available = 0;
        if (!LogicValueAsFloatArray(value, values, available)) {
          float scalar = 0.0f;
          if (!LogicValueAsFloat(value, scalar)) {
            return false;
          }
          values[0] = scalar;
          available = 1;
        }
        const int length = blender::RNA_property_array_length(&target_ptr, prop);
        if (length <= 0 || length > available) {
          return false;
        }
        blender::RNA_property_float_set_array_at_most(&target_ptr, prop, values.data(), length);
        return true;
      }
      float float_value = 0.0f;
      if (!LogicValueAsFloat(value, float_value)) {
        return false;
      }
      blender::RNA_property_float_set(&target_ptr, prop, float_value);
      return true;
    }
    case blender::PROP_STRING: {
      std::string string_value;
      if (!LogicValueAsString(value, string_value)) {
        return false;
      }
      blender::RNA_property_string_set(&target_ptr, prop, string_value.c_str());
      return true;
    }
    case blender::PROP_ENUM: {
      int enum_value = 0;
      if (value.type == LN_ValueType::Int) {
        enum_value = value.int_value;
      }
      else if (value.type == LN_ValueType::String) {
        if (!blender::RNA_property_enum_value(
                context, &target_ptr, prop, value.string_value.c_str(), &enum_value))
        {
          return false;
        }
      }
      else {
        return false;
      }
      blender::RNA_property_enum_set(&target_ptr, prop, enum_value);
      return true;
    }
    case blender::PROP_POINTER: {
      blender::PointerRNA pointer_value;
      if (!LogicValueAsPointer(runtime_tree, value, pointer_value)) {
        return false;
      }
      blender::RNA_property_pointer_set(&target_ptr, prop, pointer_value, nullptr);
      return true;
    }
    default:
      return false;
  }
}

bool IsObjectTransformCommand(const LN_CommandBuffer::CommandType type)
{
  switch (type) {
    case LN_CommandBuffer::CommandType::SetWorldPosition:
    case LN_CommandBuffer::CommandType::SetLocalPosition:
    case LN_CommandBuffer::CommandType::SetWorldOrientation:
    case LN_CommandBuffer::CommandType::SetLocalOrientation:
    case LN_CommandBuffer::CommandType::SetWorldScale:
    case LN_CommandBuffer::CommandType::SetLocalScale:
    case LN_CommandBuffer::CommandType::ApplyMovement:
    case LN_CommandBuffer::CommandType::ApplyRotation:
      return true;
    default:
      return false;
  }
}

void ApplyObjectTransformCommand(const LN_CommandBuffer::Command &command)
{
  if (command.object == nullptr) {
    return;
  }

  switch (command.type) {
    case LN_CommandBuffer::CommandType::SetWorldPosition:
      command.object->NodeSetWorldPosition(command.vector_value);
      command.object->NodeUpdateGS(0.0);
      break;
    case LN_CommandBuffer::CommandType::SetLocalPosition:
      command.object->NodeSetLocalPosition(command.vector_value);
      command.object->NodeUpdateGS(0.0);
      break;
    case LN_CommandBuffer::CommandType::SetWorldOrientation:
      command.object->NodeSetGlobalOrientation(MT_Matrix3x3(command.vector_value));
      command.object->NodeUpdateGS(0.0);
      break;
    case LN_CommandBuffer::CommandType::SetLocalOrientation:
      command.object->NodeSetLocalOrientation(MT_Matrix3x3(command.vector_value));
      command.object->NodeUpdateGS(0.0);
      break;
    case LN_CommandBuffer::CommandType::SetWorldScale:
      command.object->NodeSetWorldScale(command.vector_value);
      command.object->NodeUpdateGS(0.0);
      break;
    case LN_CommandBuffer::CommandType::SetLocalScale:
      command.object->NodeSetLocalScale(command.vector_value);
      command.object->NodeUpdateGS(0.0);
      break;
    case LN_CommandBuffer::CommandType::ApplyMovement:
      command.object->ApplyMovement(command.vector_value, command.bool_value);
      break;
    case LN_CommandBuffer::CommandType::ApplyRotation:
      command.object->ApplyRotation(command.vector_value, command.bool_value);
      break;
    default:
      break;
  }
}

}  // namespace

bool LN_CommandBuffer::IsAddObjectSourceInactiveCached(KX_Scene *scene, KX_GameObject *source)
{
  if (scene == nullptr || source == nullptr) {
    return false;
  }

  AddObjectSourceStateKey key;
  key.scene = scene;
  key.source = source;
  if (const auto item = m_addObjectSourceInactiveCache.find(key);
      item != m_addObjectSourceInactiveCache.end())
  {
    return item->second;
  }

  const bool inactive = add_object_source_is_inactive(scene, source);
  m_addObjectSourceInactiveCache.emplace(key, inactive);
  return inactive;
}

bool LN_CommandBuffer::EnsureMaterialParameterBinding(blender::Main *bmain,
                                                      blender::Material *material,
                                                      blender::bNodeTree *ntree,
                                                      blender::bNode *node,
                                                      blender::bNodeSocket *socket,
                                                      std::string *r_attribute_name)
{
  AssertMainThread();
  if (r_attribute_name != nullptr) {
    r_attribute_name->clear();
  }
  if (bmain == nullptr || material == nullptr || ntree == nullptr || node == nullptr ||
      socket == nullptr)
  {
    return false;
  }

  const std::string attribute_name = MaterialParameterAttributeName(material, *node, *socket);
  if (!EnsureMaterialParameterObjectAttributeBinding(
          bmain, material, *ntree, *node, *socket, attribute_name))
  {
    return false;
  }

  MaterialParameterDefaultStateKey default_key;
  default_key.material = material;
  default_key.attribute_name = attribute_name;
  const auto defaults_result = m_initializedMaterialParameterDefaults.emplace(
      std::move(default_key));
  if (defaults_result.second) {
    std::array<float, 4> default_values{};
    if (SocketDefaultAsMaterialParameterValues(*socket, default_values)) {
      InitializeMaterialParameterDefaults(material, attribute_name, default_values);
    }
  }

  if (r_attribute_name != nullptr) {
    *r_attribute_name = attribute_name;
  }
  return true;
}

void LN_CommandBuffer::PrewarmMaterialParameterBindings(LN_RuntimeTree &runtime_tree)
{
  AssertMainThread();
  BLI_assert_msg(!m_isRecording,
                 "Logic Nodes command buffer cannot prewarm material parameters while recording");

  const std::shared_ptr<const LN_Program> program = runtime_tree.GetProgram();
  if (program == nullptr) {
    return;
  }

  blender::Main *bmain = CurrentMain(nullptr);
  if (bmain == nullptr) {
    return;
  }

  std::unordered_set<MaterialParameterDefaultStateKey, MaterialParameterDefaultStateKeyHash>
      prewarmed_bindings;
  auto prewarm_instruction = [&](const LN_Instruction &instruction) {
    if (instruction.opcode != LN_OpCode::SetMaterialParameter) {
      return;
    }
    if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
      return;
    }

    std::string node_name;
    std::string socket_name;
    int32_t slot = 0;
    if (!ConstantStringExpressionValue(*program, instruction.string_expr_index, node_name) ||
        !ConstantStringExpressionValue(
            *program, instruction.secondary_string_expr_index, socket_name) ||
        !ConstantIntExpressionValue(*program, instruction.int_expr_index, slot) || slot < 0)
    {
      return;
    }

    KX_GameObject *game_object = ConstantMaterialParameterTargetObject(
        runtime_tree, *program, instruction.value_expr_index);
    blender::Object *object = game_object ? game_object->GetBlenderObject() : nullptr;
    if (object == nullptr || slot >= object->totcol) {
      return;
    }

    blender::Material *material = BKE_object_material_get(object, short(slot + 1));
    blender::bNodeTree *ntree = material ? material->nodetree : nullptr;
    if (ntree == nullptr) {
      return;
    }

    blender::bNode *node = FindMaterialNodeByUserName(ntree, node_name);
    blender::bNodeSocket *socket = node ? FindInputSocketByIdentifierOrName(*node, socket_name) :
                                          nullptr;
    if (socket == nullptr || !MaterialParameterSocketSupportsObjectAttribute(*socket)) {
      return;
    }

    const std::string attribute_name = MaterialParameterAttributeName(material, *node, *socket);
    MaterialParameterDefaultStateKey binding_key;
    binding_key.material = material;
    binding_key.attribute_name = attribute_name;
    if (prewarmed_bindings.emplace(binding_key).second &&
        !EnsureMaterialParameterBinding(bmain, material, ntree, node, socket, nullptr))
    {
      return;
    }

    std::array<float, 4> default_values{};
    if (SocketDefaultAsMaterialParameterValues(*socket, default_values) &&
        EnsureObjectMaterialParameterDefaultProperty(object, attribute_name, default_values))
    {
      TagMaterialParameterObjectUpdate(game_object, object);
    }
  };

  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnInit)) {
    prewarm_instruction(instruction);
  }
  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnFixedUpdate)) {
    prewarm_instruction(instruction);
  }
}

KX_GameObject *LN_CommandBuffer::ExecuteAddObjectFromRefCommand(const Command &command,
                                                                LN_Value &r_added_object)
{
  r_added_object = LN_Value();
  r_added_object.type = LN_ValueType::ObjectRef;

  if (command.object == nullptr || command.runtime_tree == nullptr) {
    return nullptr;
  }

  KX_Scene *scene = command.object->GetScene();
  if (scene == nullptr) {
    return nullptr;
  }

  KX_GameObject *source = command.runtime_tree->ResolveObjectRef(command.runtime_ref);
  if (source == nullptr) {
    return nullptr;
  }

  if (!IsAddObjectSourceInactiveCached(scene, source)) {
    warn_add_object_source_not_inactive(command, scene, source, command.runtime_ref.debug_name);
    return nullptr;
  }

  const float scene_lifespan = add_object_life_seconds_to_scene_lifespan(command.scalar_value,
                                                                        command.bool_value);
  KX_GameObject *replica = command.bool_value ?
                               scene->AddFullCopyObject(source, command.object, scene_lifespan) :
                               scene->AddReplicaObject(source, command.object, scene_lifespan);
  if (replica == nullptr) {
    return nullptr;
  }

  if (command.property_ref_index != LN_INVALID_INDEX) {
    const std::string replica_id_name = game_object_id_name(replica);
    r_added_object.exists = true;
    r_added_object.runtime_ref = command.runtime_tree->MakeObjectRef(replica, replica_id_name);
    r_added_object.reference_name = replica_id_name;
  }
  if (!command.bool_value) {
    replica->Release();
  }
  return replica;
}

void LN_CommandBuffer::SortCommands(std::vector<Command> &commands)
{
  std::stable_sort(commands.begin(), commands.end(), [](const Command &left, const Command &right) {
    if (left.sort_key != right.sort_key) {
      return left.sort_key < right.sort_key;
    }
    if (left.record_sequence != 0 && right.record_sequence != 0 &&
        left.record_sequence != right.record_sequence)
    {
      return left.record_sequence < right.record_sequence;
    }
    return false;
  });
}

LN_CommandBuffer::CommandSubsystem LN_CommandBuffer::GetCommandSubsystem(const CommandType type)
{
  switch (type) {
    case CommandType::SetWorldPosition:
    case CommandType::SetLocalPosition:
    case CommandType::SetWorldOrientation:
    case CommandType::SetLocalOrientation:
    case CommandType::SetWorldScale:
    case CommandType::SetLocalScale:
    case CommandType::ApplyMovement:
    case CommandType::ApplyRotation:
      return CommandSubsystem::ObjectTransform;
    case CommandType::SetLinearVelocity:
    case CommandType::SetLocalLinearVelocity:
    case CommandType::SetAngularVelocity:
    case CommandType::SetLocalAngularVelocity:
    case CommandType::ApplyImpulse:
    case CommandType::ApplyForce:
    case CommandType::ApplyTorque:
    case CommandType::SetCollisionGroup:
    case CommandType::SetPhysics:
    case CommandType::SetDynamics:
    case CommandType::RebuildCollisionShape:
    case CommandType::SetRigidBodyAttribute:
    case CommandType::CharacterJump:
    case CommandType::SetCharacterGravity:
    case CommandType::SetCharacterJumpSpeed:
    case CommandType::SetCharacterMaxJumps:
    case CommandType::SetCharacterWalkDirection:
    case CommandType::SetCharacterVelocity:
    case CommandType::VehicleControl:
    case CommandType::VehicleApplyEngineForce:
    case CommandType::VehicleApplyBraking:
    case CommandType::VehicleApplySteering:
    case CommandType::SetVehicleSuspensionCompression:
    case CommandType::SetVehicleSuspensionStiffness:
    case CommandType::SetVehicleSuspensionDamping:
    case CommandType::SetVehicleWheelFriction:
      return CommandSubsystem::ObjectPhysics;
    case CommandType::SetVisibility:
    case CommandType::SetObjectColor:
      return CommandSubsystem::ObjectState;
    case CommandType::MakeLightUnique:
    case CommandType::SetLightColor:
    case CommandType::SetLightPower:
    case CommandType::SetLightShadow:
      return CommandSubsystem::Light;
    case CommandType::SetGameProperty:
      return CommandSubsystem::ObjectProperty;
    case CommandType::SetTreeProperty:
    case CommandType::SetLogicTreeEnabled:
    case CommandType::InstallLogicTree:
      return CommandSubsystem::TreeState;
    case CommandType::AddObject:
    case CommandType::AddObjectFromRef:
    case CommandType::RemoveObject:
      return CommandSubsystem::ObjectLifecycle;
    case CommandType::SetGravity:
    case CommandType::SetTimeScale:
      return CommandSubsystem::SceneState;
    case CommandType::SetActiveCamera:
    case CommandType::SetCameraFov:
    case CommandType::SetCameraOrthoScale:
      return CommandSubsystem::Camera;
    case CommandType::SetParent:
    case CommandType::SetParentFromRef:
    case CommandType::RemoveParent:
      return CommandSubsystem::Parenting;
    case CommandType::SetCursorVisibility:
    case CommandType::SetCursorPosition:
    case CommandType::SetGamepadVibration:
    case CommandType::SetWindowSize:
    case CommandType::SetFullscreen:
    case CommandType::SetVSync:
      return CommandSubsystem::Window;
    case CommandType::SetShowFramerate:
    case CommandType::SetShowProfile:
      return CommandSubsystem::Render;
    case CommandType::Print:
      return CommandSubsystem::Render;
    case CommandType::SubsystemCommand:
      return CommandSubsystem::Render;
    case CommandType::QuitGame:
    case CommandType::RestartGame:
    case CommandType::LoadBlendFile:
      return CommandSubsystem::GameLifecycle;
    case CommandType::PlayAction:
    case CommandType::StopAction:
    case CommandType::SetActionFrame:
    case CommandType::SetBonePoseLocation:
    case CommandType::SetBonePoseRotation:
    case CommandType::SetBonePoseScale:
    case CommandType::SetBonePoseTransform:
    case CommandType::SetBoneAttribute:
    case CommandType::SetBoneConstraintInfluence:
    case CommandType::SetBoneConstraintTarget:
    case CommandType::SetBoneConstraintAttribute:
      return CommandSubsystem::Animation;
    case CommandType::StopAllSounds:
    case CommandType::PlaySound:
    case CommandType::PlaySound3D:
    case CommandType::PauseSound:
    case CommandType::ResumeSound:
    case CommandType::StopSound:
      return CommandSubsystem::Sound;
    case CommandType::SendEvent:
      return CommandSubsystem::Events;
    case CommandType::MoveToward:
    case CommandType::SlowFollow:
      return CommandSubsystem::ObjectTransform;
    case CommandType::LoadScene:
    case CommandType::SetScene:
    case CommandType::SaveGame:
    case CommandType::LoadGame:
      return CommandSubsystem::GameLifecycle;
    case CommandType::AlignAxisToVector:
    case CommandType::RotateToward:
      return CommandSubsystem::ObjectTransform;
    case CommandType::ReplaceMesh:
    case CommandType::SetMaterialSlot:
      return CommandSubsystem::ObjectState;
    case CommandType::SetMaterialParameter:
    case CommandType::SetMaterialNodeSocketValue:
    case CommandType::SetGeometryNodesInput:
    case CommandType::SetGeometryNodeSocketValue:
    case CommandType::SetCompositorNodeSocketValue:
    case CommandType::MakeNodeTreeUnique:
    case CommandType::SetNodeMute:
    case CommandType::EnableDisableModifier:
      return CommandSubsystem::Datablock;
    case CommandType::CopyProperty:
      return CommandSubsystem::ObjectProperty;
  }

  BLI_assert_unreachable();
  return CommandSubsystem::ObjectState;
}

LN_CommandBuffer::CommandSubsystem LN_CommandBuffer::GetCommandSubsystem(const Command &command)
{
  if (command.type == CommandType::SubsystemCommand) {
    return command.subsystem;
  }
  return GetCommandSubsystem(command.type);
}

LN_CommandBuffer::CommandSubsystemPolicy LN_CommandBuffer::GetCommandSubsystemPolicy(
    const CommandSubsystem subsystem)
{
  switch (subsystem) {
    case CommandSubsystem::ObjectTransform:
      return {CommandThreadPolicy::SnapshotReadOnly,
              CommandThreadPolicy::MainThreadFlush,
              "LN_Snapshot/KX_GameObject",
              "Transforms are read from the fixed-tick snapshot and written during the "
              "deterministic flush."};
    case CommandSubsystem::ObjectPhysics:
      return {CommandThreadPolicy::SnapshotReadOnly,
              CommandThreadPolicy::MainThreadFlush,
              "LN_Snapshot/PHY_IPhysicsController",
              "Physics reads use captured state where possible; controller writes run on the "
              "game thread."};
    case CommandSubsystem::ObjectState:
      return {CommandThreadPolicy::SnapshotReadOnly,
              CommandThreadPolicy::MainThreadFlush,
              "LN_Snapshot/KX_GameObject",
              "Visibility and color writes are buffered to keep tree execution side-effect free."};
    case CommandSubsystem::ObjectProperty:
      return {CommandThreadPolicy::SnapshotReadOnly,
              CommandThreadPolicy::MainThreadFlush,
              "LN_Snapshot/EXP_Value",
              "Game properties are snapshotted for reads and written through command-buffer "
              "value conversion."};
    case CommandSubsystem::TreeState:
      return {CommandThreadPolicy::SnapshotReadOnly,
              CommandThreadPolicy::MainThreadFlush,
              "LN_RuntimeTree",
              "Tree properties are per-installed-tree state and are only mutated by buffered "
              "commands."};
    case CommandSubsystem::ObjectLifecycle:
      return {CommandThreadPolicy::MainThreadQuery,
              CommandThreadPolicy::MainThreadFlush,
              "KX_Scene",
              "Add/remove operations follow the scene lifecycle and delayed-removal rules."};
    case CommandSubsystem::SceneState:
      return {CommandThreadPolicy::SnapshotReadOnly,
              CommandThreadPolicy::MainThreadFlush,
              "KX_Scene/KX_KetsjiEngine",
              "Scene gravity and engine timescale writes are main-thread engine commands."};
    case CommandSubsystem::Parenting:
      return {CommandThreadPolicy::MainThreadQuery,
              CommandThreadPolicy::MainThreadFlush,
              "KX_GameObject scene graph",
              "Hierarchy changes are flushed on the game thread after all trees finish evaluating."};
    case CommandSubsystem::Animation:
      return {CommandThreadPolicy::MainThreadQuery,
              CommandThreadPolicy::MainThreadFlush,
              "BL_ActionManager",
              "Action playback uses Blender game-object animation owners and must stay on the "
              "game thread."};
    case CommandSubsystem::Sound:
      return {CommandThreadPolicy::MainThreadQuery,
              CommandThreadPolicy::MainThreadFlush,
              "SCA_SoundActuator/audaspace",
              "Sound handles are command-driven resources with main-thread lifecycle ownership."};
    case CommandSubsystem::Camera:
      return {CommandThreadPolicy::MainThreadQuery,
              CommandThreadPolicy::MainThreadFlush,
              "KX_Camera",
              "Camera lens/frustum mutations require the active scene camera service."};
    case CommandSubsystem::Light:
      return {CommandThreadPolicy::MainThreadQuery,
              CommandThreadPolicy::MainThreadFlush,
              "KX_Light/Light DNA",
              "Light data is an ID-backed game object resource and is not mutated from worker "
              "evaluation."};
    case CommandSubsystem::Render:
      return {CommandThreadPolicy::MainThreadQuery,
              CommandThreadPolicy::MainThreadFlush,
              "RAS_ICanvas/RAS_2DFilterManager",
              "Render and viewport state belong to the rasterizer/canvas thread-facing services."};
    case CommandSubsystem::Window:
      return {CommandThreadPolicy::MainThreadQuery,
              CommandThreadPolicy::MainThreadFlush,
              "RAS_ICanvas/DEV_Joystick",
              "Window, cursor and device feedback commands flush through the active engine canvas."};
    case CommandSubsystem::Datablock:
      return {CommandThreadPolicy::MainThreadQuery,
              CommandThreadPolicy::Unsupported,
              "LN_RuntimeRef/Blender ID",
              "Datablock sockets may carry refs; mutation waits for resource-specific command handlers."};
    case CommandSubsystem::Collection:
      return {CommandThreadPolicy::MainThreadQuery,
              CommandThreadPolicy::MainThreadFlush,
              "SCA_CollectionActuator/KX_Scene",
              "Collection operations share scene lifecycle constraints with object add/remove."};
    case CommandSubsystem::Geometry:
      return {CommandThreadPolicy::MainThreadQuery,
              CommandThreadPolicy::Unsupported,
              "Mesh/Geometry data-block services",
              "Geometry reads need an explicit snapshot or evaluated-geometry owner before worker use."};
    case CommandSubsystem::Group:
      return {CommandThreadPolicy::Unsupported,
              CommandThreadPolicy::Unsupported,
              "Logic node group compiler",
              "Node groups require compiler/runtime call-frame support before they are exposed."};
    case CommandSubsystem::Input:
      return {CommandThreadPolicy::SnapshotReadOnly,
              CommandThreadPolicy::Unsupported,
              "LN_InputSnapshot/SCA_InputEvent",
              "Input is captured once per frame and consumed read-only by logic nodes."};
    case CommandSubsystem::GameLifecycle:
      return {CommandThreadPolicy::Unsupported,
              CommandThreadPolicy::MainThreadFlush,
              "KX_KetsjiEngine",
              "Game lifecycle commands (quit, restart, load file) are fire-and-forget engine "
              "requests that execute at the end of the flush cycle."};
    case CommandSubsystem::Events:
      return {CommandThreadPolicy::SnapshotReadOnly,
              CommandThreadPolicy::MainThreadFlush,
              "LN_Manager event queue",
              "Events are dispatched during flush and read from the snapshot on the next tick."};
  }

  return {CommandThreadPolicy::Unsupported, CommandThreadPolicy::Unsupported, nullptr, nullptr};
}

LN_CommandBuffer::CommandThreadPolicy LN_CommandBuffer::GetCommandThreadPolicy(
    const CommandType type)
{
  switch (type) {
    case CommandType::SetWorldPosition:
    case CommandType::SetLocalPosition:
    case CommandType::SetWorldOrientation:
    case CommandType::SetLocalOrientation:
    case CommandType::SetWorldScale:
    case CommandType::SetLocalScale:
    case CommandType::SetLinearVelocity:
    case CommandType::SetLocalLinearVelocity:
    case CommandType::SetAngularVelocity:
    case CommandType::SetLocalAngularVelocity:
    case CommandType::ApplyImpulse:
    case CommandType::SetVisibility:
    case CommandType::SetObjectColor:
    case CommandType::MakeLightUnique:
    case CommandType::SetLightColor:
    case CommandType::SetLightPower:
    case CommandType::SetLightShadow:
    case CommandType::ApplyMovement:
    case CommandType::ApplyRotation:
    case CommandType::ApplyForce:
    case CommandType::ApplyTorque:
    case CommandType::SetGameProperty:
    case CommandType::AddObject:
    case CommandType::AddObjectFromRef:
    case CommandType::SetParent:
    case CommandType::SetParentFromRef:
    case CommandType::RemoveParent:
    case CommandType::RemoveObject:
    case CommandType::SetGravity:
    case CommandType::SetTimeScale:
    case CommandType::SetActiveCamera:
    case CommandType::SetCameraFov:
    case CommandType::SetCameraOrthoScale:
    case CommandType::SetCollisionGroup:
    case CommandType::SetPhysics:
    case CommandType::SetDynamics:
    case CommandType::RebuildCollisionShape:
    case CommandType::SetRigidBodyAttribute:
    case CommandType::CharacterJump:
    case CommandType::SetCharacterGravity:
    case CommandType::SetCharacterJumpSpeed:
    case CommandType::SetCharacterMaxJumps:
    case CommandType::SetCharacterWalkDirection:
    case CommandType::SetCharacterVelocity:
    case CommandType::VehicleControl:
    case CommandType::VehicleApplyEngineForce:
    case CommandType::VehicleApplyBraking:
    case CommandType::VehicleApplySteering:
    case CommandType::SetVehicleSuspensionCompression:
    case CommandType::SetVehicleSuspensionStiffness:
    case CommandType::SetVehicleSuspensionDamping:
    case CommandType::SetVehicleWheelFriction:
    case CommandType::SetTreeProperty:
    case CommandType::SetLogicTreeEnabled:
    case CommandType::InstallLogicTree:
    case CommandType::SetCursorVisibility:
    case CommandType::SetCursorPosition:
    case CommandType::SetGamepadVibration:
    case CommandType::SetWindowSize:
    case CommandType::SetFullscreen:
    case CommandType::SetVSync:
    case CommandType::SetShowFramerate:
    case CommandType::SetShowProfile:
    case CommandType::Print:
    case CommandType::SubsystemCommand:
    case CommandType::QuitGame:
    case CommandType::RestartGame:
    case CommandType::LoadBlendFile:
    case CommandType::PlayAction:
    case CommandType::StopAction:
    case CommandType::SetActionFrame:
    case CommandType::StopAllSounds:
    case CommandType::PlaySound:
    case CommandType::PlaySound3D:
    case CommandType::PauseSound:
    case CommandType::ResumeSound:
    case CommandType::StopSound:
    case CommandType::SendEvent:
    case CommandType::MoveToward:
    case CommandType::SlowFollow:
    case CommandType::LoadScene:
    case CommandType::SetScene:
    case CommandType::SaveGame:
    case CommandType::LoadGame:
    case CommandType::AlignAxisToVector:
    case CommandType::RotateToward:
    case CommandType::ReplaceMesh:
    case CommandType::CopyProperty:
    case CommandType::SetBonePoseLocation:
    case CommandType::SetBonePoseRotation:
    case CommandType::SetBonePoseScale:
    case CommandType::SetBonePoseTransform:
    case CommandType::SetBoneAttribute:
    case CommandType::SetBoneConstraintInfluence:
    case CommandType::SetBoneConstraintTarget:
    case CommandType::SetBoneConstraintAttribute:
    case CommandType::SetMaterialSlot:
    case CommandType::SetMaterialParameter:
    case CommandType::SetMaterialNodeSocketValue:
    case CommandType::SetGeometryNodesInput:
    case CommandType::SetGeometryNodeSocketValue:
    case CommandType::SetCompositorNodeSocketValue:
    case CommandType::MakeNodeTreeUnique:
    case CommandType::SetNodeMute:
    case CommandType::EnableDisableModifier:
      return CommandThreadPolicy::MainThreadFlush;
  }

  return CommandThreadPolicy::MainThreadFlush;
}

LN_CommandBuffer::CommandClass LN_CommandBuffer::GetCommandClass(const CommandType type)
{
  switch (type) {
    case CommandType::SetWorldPosition:
    case CommandType::SetLocalPosition:
    case CommandType::SetWorldOrientation:
    case CommandType::SetLocalOrientation:
    case CommandType::SetWorldScale:
    case CommandType::SetLocalScale:
      return CommandClass::AbsoluteObjectTransform;
    case CommandType::SetLinearVelocity:
    case CommandType::SetLocalLinearVelocity:
    case CommandType::SetAngularVelocity:
    case CommandType::SetLocalAngularVelocity:
      return CommandClass::AbsoluteObjectVelocity;
    case CommandType::ApplyMovement:
    case CommandType::ApplyRotation:
    case CommandType::ApplyForce:
    case CommandType::ApplyTorque:
      return CommandClass::RelativeObjectVector;
    case CommandType::SetGameProperty:
      return CommandClass::GamePropertyWrite;
    case CommandType::SetTreeProperty:
      return CommandClass::TreePropertyWrite;
    case CommandType::SendEvent:
      return CommandClass::EventSend;
    case CommandType::PlaySound:
    case CommandType::PlaySound3D:
    case CommandType::PauseSound:
    case CommandType::ResumeSound:
    case CommandType::StopSound:
      return CommandClass::SimpleAudioSideEffect;
    case CommandType::AddObject:
    case CommandType::AddObjectFromRef:
    case CommandType::SetParent:
    case CommandType::SetParentFromRef:
    case CommandType::RemoveParent:
    case CommandType::RemoveObject:
    case CommandType::ReplaceMesh:
    case CommandType::CopyProperty:
      return CommandClass::ObjectLifecycleMutation;
    case CommandType::SetVisibility:
    case CommandType::SetObjectColor:
    case CommandType::MakeLightUnique:
    case CommandType::SetLightColor:
    case CommandType::SetLightPower:
    case CommandType::SetLightShadow:
    case CommandType::SetGravity:
    case CommandType::SetTimeScale:
    case CommandType::SetActiveCamera:
    case CommandType::SetCameraFov:
    case CommandType::SetCameraOrthoScale:
      return CommandClass::ObjectServiceMutation;
    case CommandType::ApplyImpulse:
    case CommandType::SetCollisionGroup:
    case CommandType::SetPhysics:
    case CommandType::SetDynamics:
    case CommandType::RebuildCollisionShape:
    case CommandType::SetRigidBodyAttribute:
    case CommandType::CharacterJump:
    case CommandType::SetCharacterGravity:
    case CommandType::SetCharacterJumpSpeed:
    case CommandType::SetCharacterMaxJumps:
    case CommandType::SetCharacterWalkDirection:
    case CommandType::SetCharacterVelocity:
    case CommandType::VehicleControl:
    case CommandType::VehicleApplyEngineForce:
    case CommandType::VehicleApplyBraking:
    case CommandType::VehicleApplySteering:
    case CommandType::SetVehicleSuspensionCompression:
    case CommandType::SetVehicleSuspensionStiffness:
    case CommandType::SetVehicleSuspensionDamping:
    case CommandType::SetVehicleWheelFriction:
      return CommandClass::PhysicsServiceMutation;
    default:
      return CommandClass::LegacyFallback;
  }
}

LN_CommandBuffer::CommandBarrier LN_CommandBuffer::GetCommandBarrier(const CommandType type)
{
  switch (type) {
    case CommandType::SetWorldPosition:
    case CommandType::SetLocalPosition:
    case CommandType::SetWorldOrientation:
    case CommandType::SetLocalOrientation:
    case CommandType::SetWorldScale:
    case CommandType::SetLocalScale:
    case CommandType::SetLinearVelocity:
    case CommandType::SetLocalLinearVelocity:
    case CommandType::SetAngularVelocity:
    case CommandType::SetLocalAngularVelocity:
    case CommandType::SetGameProperty:
    case CommandType::SetTreeProperty:
      return CommandBarrier::AbsoluteSetter;
    case CommandType::ApplyMovement:
    case CommandType::ApplyRotation:
    case CommandType::ApplyImpulse:
    case CommandType::ApplyForce:
    case CommandType::ApplyTorque:
    case CommandType::RebuildCollisionShape:
    case CommandType::CharacterJump:
    case CommandType::SetCharacterWalkDirection:
    case CommandType::SetCharacterVelocity:
    case CommandType::VehicleControl:
    case CommandType::VehicleApplyEngineForce:
    case CommandType::VehicleApplyBraking:
    case CommandType::VehicleApplySteering:
    case CommandType::MoveToward:
    case CommandType::SlowFollow:
    case CommandType::AlignAxisToVector:
    case CommandType::RotateToward:
    case CommandType::CopyProperty:
      return CommandBarrier::RelativeMutation;
    case CommandType::AddObject:
    case CommandType::AddObjectFromRef:
    case CommandType::RemoveObject:
    case CommandType::SetParent:
    case CommandType::SetParentFromRef:
    case CommandType::RemoveParent:
    case CommandType::ReplaceMesh:
      return CommandBarrier::ObjectLifecycle;
    case CommandType::SetGravity:
    case CommandType::SetTimeScale:
    case CommandType::SetActiveCamera:
    case CommandType::SetCameraFov:
    case CommandType::SetCameraOrthoScale:
    case CommandType::MakeLightUnique:
    case CommandType::SetLightColor:
    case CommandType::SetLightPower:
    case CommandType::SetLightShadow:
    case CommandType::LoadScene:
    case CommandType::SetScene:
    case CommandType::SetMaterialParameter:
    case CommandType::SetMaterialNodeSocketValue:
    case CommandType::SetGeometryNodesInput:
    case CommandType::SetGeometryNodeSocketValue:
    case CommandType::SetCompositorNodeSocketValue:
    case CommandType::MakeNodeTreeUnique:
    case CommandType::SetNodeMute:
    case CommandType::EnableDisableModifier:
    case CommandType::SetMaterialSlot:
      return CommandBarrier::SceneDatablockMutation;
    case CommandType::SendEvent:
      return CommandBarrier::EventMessage;
    case CommandType::PlaySound:
    case CommandType::PlaySound3D:
    case CommandType::PauseSound:
    case CommandType::ResumeSound:
    case CommandType::StopSound:
    case CommandType::StopAllSounds:
    case CommandType::SetCursorVisibility:
    case CommandType::SetCursorPosition:
    case CommandType::SetGamepadVibration:
    case CommandType::SetWindowSize:
    case CommandType::SetFullscreen:
    case CommandType::SetVSync:
    case CommandType::QuitGame:
    case CommandType::RestartGame:
    case CommandType::LoadBlendFile:
    case CommandType::SaveGame:
    case CommandType::LoadGame:
      return CommandBarrier::ExternalSideEffect;
    case CommandType::Print:
    case CommandType::SetShowFramerate:
    case CommandType::SetShowProfile:
    case CommandType::SubsystemCommand:
      return CommandBarrier::Diagnostic;
    default:
      return CommandBarrier::SceneDatablockMutation;
  }
}

LN_CommandBuffer::CoalescingPolicy LN_CommandBuffer::GetCoalescingPolicy(const CommandType type)
{
  switch (type) {
    case CommandType::SetWorldPosition:
    case CommandType::SetLocalPosition:
    case CommandType::SetWorldOrientation:
    case CommandType::SetLocalOrientation:
    case CommandType::SetWorldScale:
    case CommandType::SetLocalScale:
    case CommandType::SetLinearVelocity:
    case CommandType::SetLocalLinearVelocity:
    case CommandType::SetAngularVelocity:
    case CommandType::SetLocalAngularVelocity:
    case CommandType::SetGameProperty:
    case CommandType::SetTreeProperty:
    case CommandType::SetMaterialParameter:
    case CommandType::SetGeometryNodesInput:
    case CommandType::SetGeometryNodeSocketValue:
    case CommandType::SetCompositorNodeSocketValue:
    case CommandType::SetNodeMute:
      return CoalescingPolicy::LastWriteWinsWithinBarrierSegment;
    default:
      return CoalescingPolicy::Forbidden;
  }
}

bool LN_CommandBuffer::AllowsObjectlessFlush(const CommandType type)
{
  switch (type) {
    case CommandType::SetTreeProperty:
    case CommandType::SetCursorVisibility:
    case CommandType::SetCursorPosition:
    case CommandType::SetGamepadVibration:
    case CommandType::SetWindowSize:
    case CommandType::SetFullscreen:
    case CommandType::SetVSync:
    case CommandType::SetShowFramerate:
    case CommandType::SetShowProfile:
    case CommandType::Print:
    case CommandType::SubsystemCommand:
    case CommandType::QuitGame:
    case CommandType::RestartGame:
    case CommandType::LoadBlendFile:
    case CommandType::LoadScene:
    case CommandType::SetScene:
    case CommandType::StopAllSounds:
    case CommandType::PlaySound:
    case CommandType::PlaySound3D:
    case CommandType::PauseSound:
    case CommandType::ResumeSound:
    case CommandType::StopSound:
    case CommandType::SetMaterialParameter:
    case CommandType::SetMaterialNodeSocketValue:
    case CommandType::SetCompositorNodeSocketValue:
    case CommandType::MakeNodeTreeUnique:
    case CommandType::SetNodeMute:
      return true;
    default:
      return false;
  }
}

void LN_CommandBuffer::SetMainThreadId(std::thread::id main_thread_id)
{
  m_mainThreadId = main_thread_id;
}

void LN_CommandBuffer::SetLogicManager(LN_Manager *logic_manager)
{
  m_logicManager = logic_manager;
}

void LN_CommandBuffer::SetTypedCommandStreamsEnabled(const bool enabled)
{
  m_typedCommandStreamsEnabled = enabled;
}

void LN_CommandBuffer::SetAllowWorkerRecording(bool allow_worker_recording)
{
  m_allowWorkerRecording = allow_worker_recording;
}

bool LN_CommandBuffer::CanFlushPendingObjectTransformCommands() const
{
  if (!m_isRecording || m_isFlushing || m_allowWorkerRecording) {
    return false;
  }
  if (m_mainThreadId != std::thread::id() && std::this_thread::get_id() != m_mainThreadId) {
    return false;
  }
  return true;
}

static bool logic_node_command_order_less_or_equal(uint64_t left_sort_key,
                                                   uint64_t left_record_sequence,
                                                   uint64_t right_sort_key,
                                                   uint64_t right_record_sequence);

LN_CommandBuffer::CommandHeader LN_CommandBuffer::MakeCommandHeader(
    const CommandType type, const uint64_t sort_key, const uint32_t source_ref_index)
{
  CommandHeader header;
  header.sort_key = sort_key;
  header.record_sequence = m_nextRecordSequence++;
  header.source_ref_index = source_ref_index;
  header.command_class = GetCommandClass(type);
  header.barrier = GetCommandBarrier(type);
  header.coalescing = GetCoalescingPolicy(type);
  return header;
}

void LN_CommandBuffer::AppendLegacyCommand(Command command)
{
  const CommandHeader header = MakeCommandHeader(command.type,
                                                 command.sort_key,
                                                 command.source_ref_index);
  if (!m_commands.empty() &&
      !logic_node_command_order_less_or_equal(m_commands.back().sort_key,
                                              m_commands.back().record_sequence,
                                              header.sort_key,
                                              header.record_sequence))
  {
    m_commandStreamsSorted = false;
  }
  if (header.coalescing == CoalescingPolicy::LastWriteWinsWithinBarrierSegment) {
    m_coalescibleCommandCount++;
  }
  command.record_sequence = header.record_sequence;
  m_commands.push_back(std::move(command));
}

template<typename TypedCommand>
void LN_CommandBuffer::AppendTypedCommand(std::vector<TypedCommand> &stream, TypedCommand command)
{
  if (!stream.empty() &&
      !logic_node_command_order_less_or_equal(stream.back().header.sort_key,
                                              stream.back().header.record_sequence,
                                              command.header.sort_key,
                                              command.header.record_sequence))
  {
    m_commandStreamsSorted = false;
  }
  if (command.header.coalescing == CoalescingPolicy::LastWriteWinsWithinBarrierSegment) {
    m_coalescibleCommandCount++;
  }
  stream.push_back(std::move(command));
}

void LN_CommandBuffer::AppendTypedVectorCommand(const CommandType type,
                                                KX_GameObject *gameobj,
                                                const MT_Vector3 &value,
                                                const bool bool_value,
                                                const uint64_t sort_key,
                                                const uint32_t source_ref_index)
{
  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = type;
    command.object = gameobj;
    command.vector_value = value;
    command.bool_value = bool_value;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedVectorCommand command;
  command.header = MakeCommandHeader(type, sort_key, source_ref_index);
  command.type = type;
  command.object = gameobj;
  command.value = value;
  command.bool_value = bool_value;
  if (command.header.command_class == CommandClass::AbsoluteObjectVelocity) {
    AppendTypedCommand(m_velocityCommands, command);
  }
  else if (command.header.command_class == CommandClass::RelativeObjectVector) {
    AppendTypedCommand(m_relativeVectorCommands, command);
  }
  else {
    AppendTypedCommand(m_transformCommands, command);
  }
}

void LN_CommandBuffer::AppendTypedVectorCommand(const CommandType type,
                                                KX_GameObject *gameobj,
                                                const MT_Vector3 &value,
                                                const uint64_t sort_key,
                                                const uint32_t source_ref_index)
{
  AppendTypedVectorCommand(type, gameobj, value, false, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendTypedObjectServiceCommand(const CommandType type,
                                                       KX_GameObject *gameobj,
                                                       const MT_Vector4 &color_value,
                                                       const float scalar_value,
                                                       const bool bool_value,
                                                       const bool secondary_bool_value,
                                                       const uint64_t sort_key,
                                                       const uint32_t source_ref_index,
                                                       const MT_Vector3 &vector_value)
{
  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = type;
    command.object = gameobj;
    command.vector_value = vector_value;
    command.color_value = color_value;
    command.scalar_value = scalar_value;
    command.bool_value = bool_value;
    command.secondary_bool_value = secondary_bool_value;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedObjectServiceCommand command;
  command.header = MakeCommandHeader(type, sort_key, source_ref_index);
  command.type = type;
  command.object = gameobj;
  command.vector_value = vector_value;
  command.color_value = color_value;
  command.scalar_value = scalar_value;
  command.bool_value = bool_value;
  command.secondary_bool_value = secondary_bool_value;
  AppendTypedCommand(m_objectServiceCommands, std::move(command));
}

void LN_CommandBuffer::AppendTypedRuntimeServiceCommand(const CommandType type,
                                                        const int32_t int_value,
                                                        const int32_t secondary_int_value,
                                                        const uint32_t uint_value,
                                                        const float scalar_value,
                                                        const float secondary_scalar_value,
                                                        const bool bool_value,
                                                        const uint64_t sort_key,
                                                        const uint32_t source_ref_index)
{
  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = type;
    command.int_value = int_value;
    command.secondary_int_value = secondary_int_value;
    command.property_ref_index = uint_value;
    command.scalar_value = scalar_value;
    command.vector_value = MT_Vector3(secondary_scalar_value, 0.0f, 0.0f);
    command.bool_value = bool_value;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedRuntimeServiceCommand command;
  command.header = MakeCommandHeader(type, sort_key, source_ref_index);
  command.type = type;
  command.int_value = int_value;
  command.secondary_int_value = secondary_int_value;
  command.uint_value = uint_value;
  command.scalar_value = scalar_value;
  command.secondary_scalar_value = secondary_scalar_value;
  command.bool_value = bool_value;
  AppendTypedCommand(m_runtimeServiceCommands, std::move(command));
}

void LN_CommandBuffer::AppendTypedAnimationCommand(const CommandType type,
                                                   KX_GameObject *gameobj,
                                                   const std::string &action_name,
                                                   const float start_frame,
                                                   const float end_frame,
                                                   const float blendin,
                                                   const float layer_weight,
                                                   const float playback_speed,
                                                   const float frame,
                                                   const int32_t layer,
                                                   const int32_t priority,
                                                   const uint32_t animation_flags,
                                                   const uint64_t sort_key,
                                                   const uint32_t source_ref_index)
{
  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = type;
    command.object = gameobj;
    command.property_name = action_name;
    command.vector_value = MT_Vector3(start_frame, end_frame, blendin);
    command.secondary_vector_value = MT_Vector3(layer_weight, playback_speed, 0.0f);
    command.scalar_value = frame;
    command.int_value = layer;
    command.secondary_int_value = priority;
    command.animation_flags = animation_flags;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedAnimationCommand command;
  command.header = MakeCommandHeader(type, sort_key, source_ref_index);
  command.type = type;
  command.object = gameobj;
  command.action_name = action_name;
  command.start_frame = start_frame;
  command.end_frame = end_frame;
  command.blendin = blendin;
  command.layer_weight = layer_weight;
  command.playback_speed = playback_speed;
  command.frame = frame;
  command.layer = layer;
  command.priority = priority;
  command.animation_flags = animation_flags;
  AppendTypedCommand(m_animationCommands, std::move(command));
}

void LN_CommandBuffer::AppendTypedMaterialCommand(const CommandType type,
                                                  KX_GameObject *gameobj,
                                                  LN_RuntimeTree *runtime_tree,
                                                  const LN_RuntimeRef &runtime_ref,
                                                  const std::string &material_name,
                                                  const std::string &node_name,
                                                  const std::string &internal_name,
                                                  const std::string &attribute_name,
                                                  const LN_Value &value,
                                                  const float scalar_value,
                                                  const int32_t int_value,
                                                  const uint64_t sort_key,
                                                  const uint32_t source_ref_index)
{
  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = type;
    command.object = gameobj;
    command.runtime_tree = runtime_tree;
    command.runtime_ref = runtime_ref;
    command.property_name = material_name;
    command.secondary_property_name = node_name;
    command.tertiary_property_name = internal_name;
    command.quaternary_property_name = attribute_name;
    command.property_value = value;
    command.scalar_value = scalar_value;
    command.int_value = int_value;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedMaterialCommand command;
  command.header = MakeCommandHeader(type, sort_key, source_ref_index);
  command.type = type;
  command.object = gameobj;
  command.runtime_tree = runtime_tree;
  command.runtime_ref = runtime_ref;
  command.material_name = material_name;
  command.node_name = node_name;
  command.internal_name = internal_name;
  command.attribute_name = attribute_name;
  command.value = value;
  command.scalar_value = scalar_value;
  command.int_value = int_value;
  AppendTypedCommand(m_materialCommands, std::move(command));
}

void LN_CommandBuffer::AppendTypedArmatureCommand(const CommandType type,
                                                  KX_GameObject *gameobj,
                                                  LN_RuntimeTree *runtime_tree,
                                                  const std::string &bone_name,
                                                  const std::string &constraint_name,
                                                  const std::string &attribute_name,
                                                  const MT_Vector3 &vector_value,
                                                  const MT_Vector3 &secondary_vector_value,
                                                  const LN_Value &value,
                                                  const float scalar_value,
                                                  const int32_t int_value,
                                                  const int32_t secondary_int_value,
                                                  const uint64_t sort_key,
                                                  const uint32_t source_ref_index)
{
  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = type;
    command.object = gameobj;
    command.runtime_tree = runtime_tree;
    command.property_name = bone_name;
    command.secondary_property_name = constraint_name;
    command.tertiary_property_name = attribute_name;
    command.vector_value = vector_value;
    command.secondary_vector_value = secondary_vector_value;
    command.property_value = value;
    command.scalar_value = scalar_value;
    command.int_value = int_value;
    command.secondary_int_value = secondary_int_value;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedArmatureCommand command;
  command.header = MakeCommandHeader(type, sort_key, source_ref_index);
  command.type = type;
  command.object = gameobj;
  command.runtime_tree = runtime_tree;
  command.bone_name = bone_name;
  command.constraint_name = constraint_name;
  command.attribute_name = attribute_name;
  command.vector_value = vector_value;
  command.secondary_vector_value = secondary_vector_value;
  command.value = value;
  command.scalar_value = scalar_value;
  command.int_value = int_value;
  command.secondary_int_value = secondary_int_value;
  AppendTypedCommand(m_armatureCommands, std::move(command));
}

void LN_CommandBuffer::AppendTypedPhysicsCommand(const CommandType type,
                                                 KX_GameObject *gameobj,
                                                 const MT_Vector3 &vector_value,
                                                 const MT_Vector3 &secondary_vector_value,
                                                 const float scalar_value,
                                                 const int32_t int_value,
                                                 const int32_t secondary_int_value,
                                                 const bool bool_value,
                                                 const bool secondary_bool_value,
                                                 const uint64_t sort_key,
                                                 const uint32_t source_ref_index)
{
  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = type;
    command.object = gameobj;
    command.vector_value = vector_value;
    command.secondary_vector_value = secondary_vector_value;
    command.scalar_value = scalar_value;
    command.int_value = int_value;
    command.secondary_int_value = secondary_int_value;
    command.bool_value = bool_value;
    command.secondary_bool_value = secondary_bool_value;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedPhysicsCommand command;
  command.header = MakeCommandHeader(type, sort_key, source_ref_index);
  command.type = type;
  command.object = gameobj;
  command.vector_value = vector_value;
  command.secondary_vector_value = secondary_vector_value;
  command.scalar_value = scalar_value;
  command.int_value = int_value;
  command.secondary_int_value = secondary_int_value;
  command.bool_value = bool_value;
  command.secondary_bool_value = secondary_bool_value;
  AppendTypedCommand(m_physicsCommands, std::move(command));
}

void LN_CommandBuffer::AppendTypedMotionCommand(const CommandType type,
                                                KX_GameObject *gameobj,
                                                KX_GameObject *target_object,
                                                const MT_Vector3 &vector_value,
                                                const MT_Vector3 &secondary_vector_value,
                                                const float scalar_value,
                                                const int32_t int_value,
                                                const int32_t secondary_int_value,
                                                const bool bool_value,
                                                const bool secondary_bool_value,
                                                const uint64_t sort_key,
                                                const uint32_t source_ref_index)
{
  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = type;
    command.object = gameobj;
    command.runtime_tree = reinterpret_cast<LN_RuntimeTree *>(target_object);
    command.vector_value = vector_value;
    command.secondary_vector_value = secondary_vector_value;
    command.scalar_value = scalar_value;
    command.int_value = int_value;
    command.secondary_int_value = secondary_int_value;
    command.bool_value = bool_value;
    command.secondary_bool_value = secondary_bool_value;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedMotionCommand command;
  command.header = MakeCommandHeader(type, sort_key, source_ref_index);
  command.type = type;
  command.object = gameobj;
  command.target_object = target_object;
  command.vector_value = vector_value;
  command.secondary_vector_value = secondary_vector_value;
  command.scalar_value = scalar_value;
  command.int_value = int_value;
  command.secondary_int_value = secondary_int_value;
  command.bool_value = bool_value;
  command.secondary_bool_value = secondary_bool_value;
  AppendTypedCommand(m_motionCommands, std::move(command));
}

struct LN_CommandCoalescingKey {
  LN_CommandBuffer::CommandType type = LN_CommandBuffer::CommandType::SetWorldPosition;
  uintptr_t object = 0;
  uintptr_t runtime_tree = 0;
  uint32_t property_id = LN_INVALID_INDEX;
  uint32_t property_ref_index = LN_INVALID_INDEX;
  uint32_t secondary_property_id = LN_INVALID_INDEX;
  int32_t int_value = 0;
  std::string property_name;
  std::string secondary_property_name;
  std::string tertiary_property_name;

  bool operator==(const LN_CommandCoalescingKey &other) const
  {
    return type == other.type && object == other.object && runtime_tree == other.runtime_tree &&
           property_id == other.property_id && property_ref_index == other.property_ref_index &&
           secondary_property_id == other.secondary_property_id && int_value == other.int_value &&
           property_name == other.property_name &&
           secondary_property_name == other.secondary_property_name &&
           tertiary_property_name == other.tertiary_property_name;
  }
};

struct LN_CommandCoalescingKeyHash {
  size_t operator()(const LN_CommandCoalescingKey &key) const
  {
    size_t hash = std::hash<int>{}(int(key.type));
    hash ^= std::hash<uintptr_t>{}(key.object) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<uintptr_t>{}(key.runtime_tree) + 0x9e3779b9u + (hash << 6u) +
            (hash >> 2u);
    hash ^= std::hash<uint32_t>{}(key.property_id) + 0x9e3779b9u + (hash << 6u) +
            (hash >> 2u);
    hash ^= std::hash<uint32_t>{}(key.property_ref_index) + 0x9e3779b9u + (hash << 6u) +
            (hash >> 2u);
    hash ^= std::hash<uint32_t>{}(key.secondary_property_id) + 0x9e3779b9u + (hash << 6u) +
            (hash >> 2u);
    hash ^= std::hash<int32_t>{}(key.int_value) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
    if (!key.property_name.empty()) {
      hash ^= std::hash<std::string>{}(key.property_name) + 0x9e3779b9u + (hash << 6u) +
              (hash >> 2u);
    }
    if (!key.secondary_property_name.empty()) {
      hash ^= std::hash<std::string>{}(key.secondary_property_name) + 0x9e3779b9u +
              (hash << 6u) + (hash >> 2u);
    }
    if (!key.tertiary_property_name.empty()) {
      hash ^= std::hash<std::string>{}(key.tertiary_property_name) + 0x9e3779b9u +
              (hash << 6u) + (hash >> 2u);
    }
    return hash;
  }
};

static LN_CommandCoalescingKey logic_node_command_coalescing_key(
    const LN_CommandBuffer::Command &command)
{
  LN_CommandCoalescingKey key;
  key.type = command.type;
  switch (command.type) {
    case LN_CommandBuffer::CommandType::SetWorldPosition:
    case LN_CommandBuffer::CommandType::SetLocalPosition:
    case LN_CommandBuffer::CommandType::SetWorldOrientation:
    case LN_CommandBuffer::CommandType::SetLocalOrientation:
    case LN_CommandBuffer::CommandType::SetWorldScale:
    case LN_CommandBuffer::CommandType::SetLocalScale:
    case LN_CommandBuffer::CommandType::SetLinearVelocity:
    case LN_CommandBuffer::CommandType::SetLocalLinearVelocity:
    case LN_CommandBuffer::CommandType::SetAngularVelocity:
    case LN_CommandBuffer::CommandType::SetLocalAngularVelocity:
      key.object = reinterpret_cast<uintptr_t>(command.object);
      break;
    case LN_CommandBuffer::CommandType::SetGameProperty:
      key.object = reinterpret_cast<uintptr_t>(command.object);
      if (command.game_property_id.IsValid()) {
        key.property_id = command.game_property_id.index;
      }
      else if (command.property_ref_index != LN_INVALID_INDEX) {
        key.runtime_tree = reinterpret_cast<uintptr_t>(command.runtime_tree);
        key.property_ref_index = command.property_ref_index;
      }
      else {
        key.property_name = command.property_name;
      }
      break;
    case LN_CommandBuffer::CommandType::SetTreeProperty:
      key.runtime_tree = reinterpret_cast<uintptr_t>(command.runtime_tree);
      key.property_ref_index = command.property_ref_index;
      break;
    case LN_CommandBuffer::CommandType::SetMaterialParameter:
      key.object = reinterpret_cast<uintptr_t>(command.object);
      key.runtime_tree = reinterpret_cast<uintptr_t>(command.runtime_tree);
      key.property_id = command.runtime_ref.slot;
      key.property_ref_index = command.runtime_ref.generation;
      key.secondary_property_id = uint32_t(command.runtime_ref.kind);
      key.int_value = command.int_value;
      key.property_name = command.property_name;
      key.secondary_property_name = command.secondary_property_name;
      key.tertiary_property_name = command.tertiary_property_name;
      break;
    case LN_CommandBuffer::CommandType::SetGeometryNodesInput:
    case LN_CommandBuffer::CommandType::SetGeometryNodeSocketValue:
      key.object = reinterpret_cast<uintptr_t>(command.object);
      key.property_name = command.property_name;
      key.secondary_property_name = command.secondary_property_name;
      key.tertiary_property_name = command.tertiary_property_name;
      break;
    case LN_CommandBuffer::CommandType::SetCompositorNodeSocketValue:
      key.int_value = command.int_value;
      key.property_name = command.property_name;
      key.secondary_property_name = command.secondary_property_name;
      key.tertiary_property_name = command.tertiary_property_name;
      break;
    case LN_CommandBuffer::CommandType::SetNodeMute:
      key.int_value = command.int_value;
      key.property_name = command.property_name;
      key.secondary_property_name = command.secondary_property_name;
      break;
    default:
      break;
  }
  return key;
}

static void logic_node_command_stream_coalesce(std::vector<LN_CommandBuffer::Command> &commands,
                                               uint32_t &r_coalesced_count)
{
  r_coalesced_count = 0;
  if (commands.empty()) {
    return;
  }

  std::vector<LN_CommandBuffer::Command> coalesced;
  coalesced.reserve(commands.size());
  std::unordered_map<LN_CommandCoalescingKey, size_t, LN_CommandCoalescingKeyHash>
      active_slots;

  for (LN_CommandBuffer::Command &command : commands) {
    const LN_CommandBuffer::CoalescingPolicy policy =
        LN_CommandBuffer::GetCoalescingPolicy(command.type);
    if (policy != LN_CommandBuffer::CoalescingPolicy::LastWriteWinsWithinBarrierSegment) {
      coalesced.push_back(std::move(command));
      active_slots.clear();
      continue;
    }

    const LN_CommandCoalescingKey key = logic_node_command_coalescing_key(command);
    const auto slot = active_slots.find(key);
    if (slot != active_slots.end()) {
      coalesced[slot->second] = std::move(command);
      r_coalesced_count++;
    }
    else {
      active_slots.emplace(std::move(key), coalesced.size());
      coalesced.push_back(std::move(command));
    }
  }

  commands = std::move(coalesced);
}

static bool logic_node_command_stream_has_coalescible_commands(
    const std::vector<LN_CommandBuffer::Command> &commands)
{
  for (const LN_CommandBuffer::Command &command : commands) {
    if (LN_CommandBuffer::GetCoalescingPolicy(command.type) ==
        LN_CommandBuffer::CoalescingPolicy::LastWriteWinsWithinBarrierSegment)
    {
      return true;
    }
  }
  return false;
}

static bool logic_node_command_order_less(const uint64_t left_sort_key,
                                          const uint64_t left_record_sequence,
                                          const uint64_t right_sort_key,
                                          const uint64_t right_record_sequence)
{
  if (left_sort_key != right_sort_key) {
    return left_sort_key < right_sort_key;
  }
  if (left_record_sequence != 0 && right_record_sequence != 0 &&
      left_record_sequence != right_record_sequence)
  {
    return left_record_sequence < right_record_sequence;
  }
  return false;
}

static bool logic_node_command_order_less_or_equal(const uint64_t left_sort_key,
                                                   const uint64_t left_record_sequence,
                                                   const uint64_t right_sort_key,
                                                   const uint64_t right_record_sequence)
{
  return !logic_node_command_order_less(
      right_sort_key, right_record_sequence, left_sort_key, left_record_sequence);
}

static constexpr uint32_t k_logic_node_min_coalescible_commands_for_runtime_flush = 64;

uint32_t LN_CommandBuffer::CountCoalescibleCommands() const
{
  return m_coalescibleCommandCount;
}

void LN_CommandBuffer::RefreshCommandStreamTracking()
{
  m_coalescibleCommandCount = 0;
  m_commandStreamsSorted = true;

  auto track_coalescing = [this](const CommandType type) {
    if (GetCoalescingPolicy(type) == CoalescingPolicy::LastWriteWinsWithinBarrierSegment) {
      m_coalescibleCommandCount++;
    }
  };

  auto track_order = [this](const uint64_t previous_sort_key,
                            const uint64_t previous_record_sequence,
                            const uint64_t current_sort_key,
                            const uint64_t current_record_sequence) {
    if (!logic_node_command_order_less_or_equal(previous_sort_key,
                                                previous_record_sequence,
                                                current_sort_key,
                                                current_record_sequence))
    {
      m_commandStreamsSorted = false;
    }
  };

  auto refresh_typed_vector_stream = [&](const std::vector<TypedVectorCommand> &stream) {
    for (size_t index = 0; index < stream.size(); index++) {
      track_coalescing(stream[index].type);
      if (index > 0) {
        track_order(stream[index - 1].header.sort_key,
                    stream[index - 1].header.record_sequence,
                    stream[index].header.sort_key,
                    stream[index].header.record_sequence);
      }
    }
  };

  auto refresh_typed_property_stream = [&](const std::vector<TypedPropertyCommand> &stream) {
    for (size_t index = 0; index < stream.size(); index++) {
      track_coalescing(stream[index].type);
      if (index > 0) {
        track_order(stream[index - 1].header.sort_key,
                    stream[index - 1].header.record_sequence,
                    stream[index].header.sort_key,
                    stream[index].header.record_sequence);
      }
    }
  };

  auto refresh_typed_event_stream = [&](const std::vector<TypedEventCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      track_order(stream[index - 1].header.sort_key,
                  stream[index - 1].header.record_sequence,
                  stream[index].header.sort_key,
                  stream[index].header.record_sequence);
    }
  };

  auto refresh_typed_audio_stream = [&](const std::vector<TypedAudioCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      track_order(stream[index - 1].header.sort_key,
                  stream[index - 1].header.record_sequence,
                  stream[index].header.sort_key,
                  stream[index].header.record_sequence);
    }
  };

  auto refresh_typed_lifecycle_stream = [&](const std::vector<TypedLifecycleCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      track_order(stream[index - 1].header.sort_key,
                  stream[index - 1].header.record_sequence,
                  stream[index].header.sort_key,
                  stream[index].header.record_sequence);
    }
  };

  auto refresh_typed_object_service_stream = [&](const std::vector<TypedObjectServiceCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      track_order(stream[index - 1].header.sort_key,
                  stream[index - 1].header.record_sequence,
                  stream[index].header.sort_key,
                  stream[index].header.record_sequence);
    }
  };

  auto refresh_typed_runtime_service_stream = [&](const std::vector<TypedRuntimeServiceCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      track_order(stream[index - 1].header.sort_key,
                  stream[index - 1].header.record_sequence,
                  stream[index].header.sort_key,
                  stream[index].header.record_sequence);
    }
  };

  auto refresh_typed_animation_stream = [&](const std::vector<TypedAnimationCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      track_order(stream[index - 1].header.sort_key,
                  stream[index - 1].header.record_sequence,
                  stream[index].header.sort_key,
                  stream[index].header.record_sequence);
    }
  };

  auto refresh_typed_armature_stream = [&](const std::vector<TypedArmatureCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      track_order(stream[index - 1].header.sort_key,
                  stream[index - 1].header.record_sequence,
                  stream[index].header.sort_key,
                  stream[index].header.record_sequence);
    }
  };

  auto refresh_typed_material_stream = [&](const std::vector<TypedMaterialCommand> &stream) {
    for (size_t index = 0; index < stream.size(); index++) {
      track_coalescing(stream[index].type);
      if (index > 0) {
        track_order(stream[index - 1].header.sort_key,
                    stream[index - 1].header.record_sequence,
                    stream[index].header.sort_key,
                    stream[index].header.record_sequence);
      }
    }
  };

  auto refresh_typed_physics_stream = [&](const std::vector<TypedPhysicsCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      track_order(stream[index - 1].header.sort_key,
                  stream[index - 1].header.record_sequence,
                  stream[index].header.sort_key,
                  stream[index].header.record_sequence);
    }
  };

  auto refresh_typed_motion_stream = [&](const std::vector<TypedMotionCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      track_order(stream[index - 1].header.sort_key,
                  stream[index - 1].header.record_sequence,
                  stream[index].header.sort_key,
                  stream[index].header.record_sequence);
    }
  };

  for (const Command &command : m_commands) {
    track_coalescing(command.type);
  }
  for (size_t index = 1; index < m_commands.size(); index++) {
    track_order(m_commands[index - 1].sort_key,
                m_commands[index - 1].record_sequence,
                m_commands[index].sort_key,
                m_commands[index].record_sequence);
  }
  refresh_typed_vector_stream(m_transformCommands);
  refresh_typed_vector_stream(m_velocityCommands);
  refresh_typed_vector_stream(m_relativeVectorCommands);
  refresh_typed_property_stream(m_propertyCommands);
  refresh_typed_event_stream(m_eventCommands);
  refresh_typed_audio_stream(m_audioCommands);
  refresh_typed_lifecycle_stream(m_lifecycleCommands);
  refresh_typed_object_service_stream(m_objectServiceCommands);
  refresh_typed_runtime_service_stream(m_runtimeServiceCommands);
  refresh_typed_animation_stream(m_animationCommands);
  refresh_typed_armature_stream(m_armatureCommands);
  refresh_typed_material_stream(m_materialCommands);
  refresh_typed_physics_stream(m_physicsCommands);
  refresh_typed_motion_stream(m_motionCommands);
}

std::vector<LN_CommandBuffer::Command> LN_CommandBuffer::BuildPlannedCommands(
    const bool enable_coalescing, const bool sort_commands, CommandStreamStats *r_stats) const
{
  CommandStreamStats stats;
  stats.legacy_command_count = uint32_t(m_commands.size());
  stats.typed_transform_count = uint32_t(m_transformCommands.size());
  stats.typed_velocity_count = uint32_t(m_velocityCommands.size());
  stats.typed_relative_vector_count = uint32_t(m_relativeVectorCommands.size());
  stats.typed_motion_count = uint32_t(m_motionCommands.size());
  stats.typed_property_count = uint32_t(m_propertyCommands.size());
  stats.typed_event_count = uint32_t(m_eventCommands.size());
  stats.typed_audio_count = uint32_t(m_audioCommands.size());
  stats.typed_lifecycle_count = uint32_t(m_lifecycleCommands.size());
  stats.typed_object_service_count = uint32_t(m_objectServiceCommands.size());
  stats.typed_runtime_service_count = uint32_t(m_runtimeServiceCommands.size());
  stats.typed_animation_count = uint32_t(m_animationCommands.size());
  stats.typed_armature_count = uint32_t(m_armatureCommands.size());
  stats.typed_material_count = uint32_t(m_materialCommands.size());
  stats.typed_physics_count = uint32_t(m_physicsCommands.size());

  const size_t total_command_count = m_commands.size() + m_transformCommands.size() +
                                     m_velocityCommands.size() +
                                     m_relativeVectorCommands.size() + m_motionCommands.size() +
                                     m_propertyCommands.size() + m_eventCommands.size() +
                                     m_audioCommands.size() +
                                     m_lifecycleCommands.size() +
                                     m_objectServiceCommands.size() +
                                     m_runtimeServiceCommands.size() +
                                     m_animationCommands.size() + m_armatureCommands.size() +
                                     m_materialCommands.size() +
                                     m_physicsCommands.size();

  std::vector<Command> commands;
  commands.reserve(total_command_count);

  auto append_vector_command = [&commands](const TypedVectorCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.vector_value = typed_command.value;
    command.bool_value = typed_command.bool_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_property_command = [&commands](const TypedPropertyCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.runtime_tree = typed_command.runtime_tree;
    command.object_handle = typed_command.object_handle;
    command.game_property_id = typed_command.game_property_id;
    command.property_name_id = typed_command.property_name_id;
    command.property_name_ptr = typed_command.property_name_ptr;
    command.property_ref_index = typed_command.property_ref_index;
    command.property_name = typed_command.property_name;
    command.property_value = typed_command.value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_event_command = [&commands](const TypedEventCommand &typed_command) {
    Command command;
    command.type = CommandType::SendEvent;
    command.runtime_tree = typed_command.runtime_tree;
    command.object = typed_command.messenger;
    command.event_target_object = typed_command.target;
    command.object_handle = typed_command.messenger_handle;
    command.event_target_handle = typed_command.target_handle;
    command.event_subject_id = typed_command.event_subject_id;
    command.property_name_id = typed_command.subject_name_id;
    command.property_name = typed_command.dynamic_subject;
    command.property_value = typed_command.content;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_audio_command = [this, &commands](const TypedAudioCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.speaker;
    command.property_name = typed_command.sound_name;
    if (command.property_name.empty() && typed_command.sound_id.IsValid() &&
        m_logicManager != nullptr)
    {
      command.property_name = m_logicManager->DebugName(typed_command.sound_id);
    }
    command.vector_value = MT_Vector3(typed_command.volume, typed_command.pitch, 0.0f);
    command.bool_value = typed_command.loop;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_lifecycle_command = [&commands](const TypedLifecycleCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.runtime_tree = typed_command.runtime_tree;
    command.runtime_ref = typed_command.runtime_ref;
    command.property_name = typed_command.name;
    command.scalar_value = typed_command.scalar_value;
    command.property_ref_index = typed_command.property_ref_index;
    command.bool_value = typed_command.bool_value;
    command.secondary_bool_value = typed_command.secondary_bool_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_object_service_command = [&commands](const TypedObjectServiceCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.vector_value = typed_command.vector_value;
    command.color_value = typed_command.color_value;
    command.scalar_value = typed_command.scalar_value;
    command.bool_value = typed_command.bool_value;
    command.secondary_bool_value = typed_command.secondary_bool_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_runtime_service_command = [&commands](const TypedRuntimeServiceCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.int_value = typed_command.int_value;
    command.secondary_int_value = typed_command.secondary_int_value;
    command.property_ref_index = typed_command.uint_value;
    command.scalar_value = typed_command.scalar_value;
    command.vector_value = MT_Vector3(typed_command.secondary_scalar_value, 0.0f, 0.0f);
    command.bool_value = typed_command.bool_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_animation_command = [&commands](const TypedAnimationCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.property_name = typed_command.action_name;
    command.vector_value = MT_Vector3(typed_command.start_frame,
                                      typed_command.end_frame,
                                      typed_command.blendin);
    command.secondary_vector_value = MT_Vector3(typed_command.layer_weight,
                                                typed_command.playback_speed,
                                                0.0f);
    command.scalar_value = typed_command.frame;
    command.int_value = typed_command.layer;
    command.secondary_int_value = typed_command.priority;
    command.animation_flags = typed_command.animation_flags;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_material_command = [&commands](const TypedMaterialCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.runtime_tree = typed_command.runtime_tree;
    command.runtime_ref = typed_command.runtime_ref;
    command.property_name = typed_command.material_name;
    command.secondary_property_name = typed_command.node_name;
    command.tertiary_property_name = typed_command.internal_name;
    command.quaternary_property_name = typed_command.attribute_name;
    command.property_value = typed_command.value;
    command.scalar_value = typed_command.scalar_value;
    command.int_value = typed_command.int_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_armature_command = [&commands](const TypedArmatureCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.runtime_tree = typed_command.runtime_tree;
    command.property_name = typed_command.bone_name;
    command.secondary_property_name = typed_command.constraint_name;
    command.tertiary_property_name = typed_command.attribute_name;
    command.vector_value = typed_command.vector_value;
    command.secondary_vector_value = typed_command.secondary_vector_value;
    command.property_value = typed_command.value;
    command.scalar_value = typed_command.scalar_value;
    command.int_value = typed_command.int_value;
    command.secondary_int_value = typed_command.secondary_int_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_physics_command = [&commands](const TypedPhysicsCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.vector_value = typed_command.vector_value;
    command.secondary_vector_value = typed_command.secondary_vector_value;
    command.scalar_value = typed_command.scalar_value;
    command.int_value = typed_command.int_value;
    command.secondary_int_value = typed_command.secondary_int_value;
    command.bool_value = typed_command.bool_value;
    command.secondary_bool_value = typed_command.secondary_bool_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_motion_command = [&commands](const TypedMotionCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.runtime_tree = reinterpret_cast<LN_RuntimeTree *>(typed_command.target_object);
    command.vector_value = typed_command.vector_value;
    command.secondary_vector_value = typed_command.secondary_vector_value;
    command.scalar_value = typed_command.scalar_value;
    command.int_value = typed_command.int_value;
    command.secondary_int_value = typed_command.secondary_int_value;
    command.bool_value = typed_command.bool_value;
    command.secondary_bool_value = typed_command.secondary_bool_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    commands.push_back(std::move(command));
  };

  auto append_commands_by_stream = [&]() {
    for (const Command &legacy_command : m_commands) {
      commands.push_back(legacy_command);
    }
    for (const TypedVectorCommand &typed_command : m_transformCommands) {
      append_vector_command(typed_command);
    }
    for (const TypedVectorCommand &typed_command : m_velocityCommands) {
      append_vector_command(typed_command);
    }
    for (const TypedVectorCommand &typed_command : m_relativeVectorCommands) {
      append_vector_command(typed_command);
    }
    for (const TypedMotionCommand &typed_command : m_motionCommands) {
      append_motion_command(typed_command);
    }
    for (const TypedPropertyCommand &typed_command : m_propertyCommands) {
      append_property_command(typed_command);
    }
    for (const TypedEventCommand &typed_command : m_eventCommands) {
      append_event_command(typed_command);
    }
    for (const TypedAudioCommand &typed_command : m_audioCommands) {
      append_audio_command(typed_command);
    }
    for (const TypedLifecycleCommand &typed_command : m_lifecycleCommands) {
      append_lifecycle_command(typed_command);
    }
    for (const TypedObjectServiceCommand &typed_command : m_objectServiceCommands) {
      append_object_service_command(typed_command);
    }
    for (const TypedRuntimeServiceCommand &typed_command : m_runtimeServiceCommands) {
      append_runtime_service_command(typed_command);
    }
    for (const TypedAnimationCommand &typed_command : m_animationCommands) {
      append_animation_command(typed_command);
    }
    for (const TypedArmatureCommand &typed_command : m_armatureCommands) {
      append_armature_command(typed_command);
    }
    for (const TypedMaterialCommand &typed_command : m_materialCommands) {
      append_material_command(typed_command);
    }
    for (const TypedPhysicsCommand &typed_command : m_physicsCommands) {
      append_physics_command(typed_command);
    }
  };

  auto legacy_stream_sorted = [&]() {
    for (size_t index = 1; index < m_commands.size(); index++) {
      const Command &previous = m_commands[index - 1];
      const Command &current = m_commands[index];
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto vector_stream_sorted = [](const std::vector<TypedVectorCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto property_stream_sorted = [](const std::vector<TypedPropertyCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto event_stream_sorted = [](const std::vector<TypedEventCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto audio_stream_sorted = [](const std::vector<TypedAudioCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto lifecycle_stream_sorted = [](const std::vector<TypedLifecycleCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto object_service_stream_sorted = [](const std::vector<TypedObjectServiceCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto runtime_service_stream_sorted = [](const std::vector<TypedRuntimeServiceCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto animation_stream_sorted = [](const std::vector<TypedAnimationCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto material_stream_sorted = [](const std::vector<TypedMaterialCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto armature_stream_sorted = [](const std::vector<TypedArmatureCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto physics_stream_sorted = [](const std::vector<TypedPhysicsCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto motion_stream_sorted = [](const std::vector<TypedMotionCommand> &stream) {
    for (size_t index = 1; index < stream.size(); index++) {
      const CommandHeader &previous = stream[index - 1].header;
      const CommandHeader &current = stream[index].header;
      if (!logic_node_command_order_less_or_equal(previous.sort_key,
                                                  previous.record_sequence,
                                                  current.sort_key,
                                                  current.record_sequence))
      {
        return false;
      }
    }
    return true;
  };

  auto try_append_sorted_stream_merge = [&]() {
    if (!legacy_stream_sorted() || !vector_stream_sorted(m_transformCommands) ||
        !vector_stream_sorted(m_velocityCommands) ||
        !vector_stream_sorted(m_relativeVectorCommands) ||
        !motion_stream_sorted(m_motionCommands) ||
        !property_stream_sorted(m_propertyCommands) || !event_stream_sorted(m_eventCommands) ||
        !audio_stream_sorted(m_audioCommands) || !lifecycle_stream_sorted(m_lifecycleCommands) ||
        !object_service_stream_sorted(m_objectServiceCommands) ||
        !runtime_service_stream_sorted(m_runtimeServiceCommands) ||
        !animation_stream_sorted(m_animationCommands) ||
        !armature_stream_sorted(m_armatureCommands) ||
        !material_stream_sorted(m_materialCommands) ||
        !physics_stream_sorted(m_physicsCommands))
    {
      return false;
    }

    struct Cursor {
      enum class Kind : uint8_t {
        Legacy = 0,
        Transform,
        Velocity,
        RelativeVector,
        Motion,
        Property,
        Event,
        Audio,
        Lifecycle,
        ObjectService,
        RuntimeService,
        Animation,
        Armature,
        Material,
        Physics,
      };
      Kind kind = Kind::Legacy;
      size_t index = 0;
      size_t size = 0;
      uint64_t sort_key = 0;
      uint64_t record_sequence = 0;
    };

    std::array<Cursor, 15> cursors{};
    size_t cursor_count = 0;
    size_t total_direct_count = 0;
    auto append_cursor = [&](const Cursor::Kind kind, const size_t size) {
      if (size == 0) {
        return;
      }
      Cursor &cursor = cursors[cursor_count++];
      cursor.kind = kind;
      cursor.index = 0;
      cursor.size = size;
      total_direct_count += size;
    };

    append_cursor(Cursor::Kind::Legacy, m_commands.size());
    append_cursor(Cursor::Kind::Transform, m_transformCommands.size());
    append_cursor(Cursor::Kind::Velocity, m_velocityCommands.size());
    append_cursor(Cursor::Kind::RelativeVector, m_relativeVectorCommands.size());
    append_cursor(Cursor::Kind::Motion, m_motionCommands.size());
    append_cursor(Cursor::Kind::Property, m_propertyCommands.size());
    append_cursor(Cursor::Kind::Event, m_eventCommands.size());
    append_cursor(Cursor::Kind::Audio, m_audioCommands.size());
    append_cursor(Cursor::Kind::Lifecycle, m_lifecycleCommands.size());
    append_cursor(Cursor::Kind::ObjectService, m_objectServiceCommands.size());
    append_cursor(Cursor::Kind::RuntimeService, m_runtimeServiceCommands.size());
    append_cursor(Cursor::Kind::Animation, m_animationCommands.size());
    append_cursor(Cursor::Kind::Armature, m_armatureCommands.size());
    append_cursor(Cursor::Kind::Material, m_materialCommands.size());
    append_cursor(Cursor::Kind::Physics, m_physicsCommands.size());

    auto refresh_cursor = [&](Cursor &cursor) {
      if (cursor.index >= cursor.size) {
        return;
      }
      switch (cursor.kind) {
        case Cursor::Kind::Legacy:
          cursor.sort_key = m_commands[cursor.index].sort_key;
          cursor.record_sequence = m_commands[cursor.index].record_sequence;
          break;
        case Cursor::Kind::Transform:
          cursor.sort_key = m_transformCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_transformCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Velocity:
          cursor.sort_key = m_velocityCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_velocityCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::RelativeVector:
          cursor.sort_key = m_relativeVectorCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_relativeVectorCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Motion:
          cursor.sort_key = m_motionCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_motionCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Property:
          cursor.sort_key = m_propertyCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_propertyCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Event:
          cursor.sort_key = m_eventCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_eventCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Audio:
          cursor.sort_key = m_audioCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_audioCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Lifecycle:
          cursor.sort_key = m_lifecycleCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_lifecycleCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::ObjectService:
          cursor.sort_key = m_objectServiceCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_objectServiceCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::RuntimeService:
          cursor.sort_key = m_runtimeServiceCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_runtimeServiceCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Animation:
          cursor.sort_key = m_animationCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_animationCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Armature:
          cursor.sort_key = m_armatureCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_armatureCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Material:
          cursor.sort_key = m_materialCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_materialCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Physics:
          cursor.sort_key = m_physicsCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_physicsCommands[cursor.index].header.record_sequence;
          break;
      }
    };

    for (Cursor &cursor : cursors) {
      refresh_cursor(cursor);
    }

    while (commands.size() < total_command_count) {
      Cursor *selected = nullptr;
      for (Cursor &cursor : cursors) {
        if (cursor.index >= cursor.size) {
          continue;
        }
        if (selected == nullptr ||
            logic_node_command_order_less(cursor.sort_key,
                                          cursor.record_sequence,
                                          selected->sort_key,
                                          selected->record_sequence))
        {
          selected = &cursor;
        }
      }
      if (selected == nullptr) {
        break;
      }

      switch (selected->kind) {
        case Cursor::Kind::Legacy:
          commands.push_back(m_commands[selected->index]);
          break;
        case Cursor::Kind::Transform:
          append_vector_command(m_transformCommands[selected->index]);
          break;
        case Cursor::Kind::Velocity:
          append_vector_command(m_velocityCommands[selected->index]);
          break;
        case Cursor::Kind::RelativeVector:
          append_vector_command(m_relativeVectorCommands[selected->index]);
          break;
        case Cursor::Kind::Motion:
          append_motion_command(m_motionCommands[selected->index]);
          break;
        case Cursor::Kind::Property:
          append_property_command(m_propertyCommands[selected->index]);
          break;
        case Cursor::Kind::Event:
          append_event_command(m_eventCommands[selected->index]);
          break;
        case Cursor::Kind::Audio:
          append_audio_command(m_audioCommands[selected->index]);
          break;
        case Cursor::Kind::Lifecycle:
          append_lifecycle_command(m_lifecycleCommands[selected->index]);
          break;
        case Cursor::Kind::ObjectService:
          append_object_service_command(m_objectServiceCommands[selected->index]);
          break;
        case Cursor::Kind::RuntimeService:
          append_runtime_service_command(m_runtimeServiceCommands[selected->index]);
          break;
        case Cursor::Kind::Animation:
          append_animation_command(m_animationCommands[selected->index]);
          break;
        case Cursor::Kind::Armature:
          append_armature_command(m_armatureCommands[selected->index]);
          break;
        case Cursor::Kind::Material:
          append_material_command(m_materialCommands[selected->index]);
          break;
        case Cursor::Kind::Physics:
          append_physics_command(m_physicsCommands[selected->index]);
          break;
      }
      selected->index++;
      refresh_cursor(*selected);
    }
    return true;
  };

  bool planned_commands_sorted = false;
  if (sort_commands) {
    planned_commands_sorted = try_append_sorted_stream_merge();
  }
  if (!planned_commands_sorted) {
    append_commands_by_stream();
  }

  const bool has_coalescible_commands =
      enable_coalescing && logic_node_command_stream_has_coalescible_commands(commands);

  if ((sort_commands || has_coalescible_commands) && !planned_commands_sorted) {
    SortCommands(commands);
  }
  if (has_coalescible_commands) {
    logic_node_command_stream_coalesce(commands, stats.coalesced_command_count);
    if (stats.coalesced_command_count != 0u) {
      SortCommands(commands);
    }
  }

  if (r_stats != nullptr) {
    *r_stats = stats;
  }
  return commands;
}

void LN_CommandBuffer::ClearTypedStreams()
{
  m_transformCommands.clear();
  m_velocityCommands.clear();
  m_relativeVectorCommands.clear();
  m_propertyCommands.clear();
  m_eventCommands.clear();
  m_audioCommands.clear();
  m_lifecycleCommands.clear();
  m_objectServiceCommands.clear();
  m_runtimeServiceCommands.clear();
  m_animationCommands.clear();
  m_armatureCommands.clear();
  m_materialCommands.clear();
  m_physicsCommands.clear();
  m_motionCommands.clear();
  m_testPlannedCommands.clear();
  m_commandStreamsSorted = true;
  m_coalescibleCommandCount = 0;
  m_addObjectSourceInactiveCache.clear();
}

void LN_CommandBuffer::MergeRecordedCommands(std::vector<Command> commands)
{
  AssertMainThread();
  BLI_assert_msg(!m_isRecording, "Logic Nodes command buffer cannot merge while recording");
  BLI_assert_msg(!m_isFlushing, "Logic Nodes command buffer cannot merge while flushing");

  if (commands.empty()) {
    return;
  }

  for (Command &command : commands) {
    AppendLegacyCommand(std::move(command));
  }
}

void LN_CommandBuffer::MergeRecordedCommandLists(std::vector<RecordedCommandList> command_lists)
{
  AssertMainThread();
  BLI_assert_msg(!m_isRecording,
                 "Logic Nodes command buffer cannot merge command lists while recording");
  BLI_assert_msg(!m_isFlushing,
                 "Logic Nodes command buffer cannot merge command lists while flushing");

  if (command_lists.empty()) {
    return;
  }

  std::stable_sort(command_lists.begin(),
                   command_lists.end(),
                   [](const RecordedCommandList &left, const RecordedCommandList &right) {
                     return left.runtime_tree_index < right.runtime_tree_index;
                   });

  size_t total_commands = 0;
  for (RecordedCommandList &command_list : command_lists) {
    SortCommands(command_list.commands);
    total_commands += command_list.commands.size();
  }
  if (total_commands == 0) {
    return;
  }

  for (RecordedCommandList &command_list : command_lists) {
    for (Command &command : command_list.commands) {
      AppendLegacyCommand(std::move(command));
    }
  }
}

void LN_CommandBuffer::BeginRecording()
{
  AssertMainThread();
  BLI_assert_msg(!m_isRecording, "Logic Nodes command buffer cannot begin recording twice");
  BLI_assert_msg(!m_isFlushing,
                 "Logic Nodes command buffer cannot begin recording while flushing");

  m_commands.clear();
  ClearTypedStreams();
  m_lastCommandStreamStats = CommandStreamStats();
  m_nextRecordSequence = 1;
  m_isRecording = true;
}

void LN_CommandBuffer::EndRecording()
{
  AssertMainThread();
  BLI_assert_msg(m_isRecording, "Logic Nodes command buffer cannot end recording before begin");
  BLI_assert_msg(!m_isFlushing,
                 "Logic Nodes command buffer cannot end recording while flushing");

  m_isRecording = false;
}

std::vector<LN_CommandBuffer::Command> LN_CommandBuffer::TakeRecordedCommands()
{
  AssertMainThread();
  BLI_assert_msg(!m_isRecording,
                 "Logic Nodes command buffer cannot transfer commands while recording");
  BLI_assert_msg(!m_isFlushing,
                 "Logic Nodes command buffer cannot transfer commands while flushing");

  CommandStreamStats stats;
  std::vector<Command> commands = BuildPlannedCommands(false, true, &stats);
  m_lastCommandStreamStats = stats;
  m_commands.clear();
  ClearTypedStreams();
  m_removedObjectsDuringFlush.clear();
  return commands;
}

std::vector<LN_CommandBuffer::Command> LN_CommandBuffer::TakeFlushPlannedCommands()
{
  AssertMainThread();
  BLI_assert_msg(!m_isRecording,
                 "Logic Nodes command buffer cannot plan flush commands while recording");
  BLI_assert_msg(!m_isFlushing,
                 "Logic Nodes command buffer cannot plan flush commands while flushing");

  CommandStreamStats stats;
  std::vector<Command> commands = BuildPlannedCommands(m_typedCommandStreamsEnabled,
                                                       true,
                                                       &stats);
  m_lastCommandStreamStats = stats;
  m_commands.clear();
  ClearTypedStreams();
  m_removedObjectsDuringFlush.clear();
  return commands;
}

const std::vector<LN_CommandBuffer::Command> &LN_CommandBuffer::GetCommandsForTests() const
{
  CommandStreamStats stats;
  m_testPlannedCommands = BuildPlannedCommands(false, false, &stats);
  std::stable_sort(m_testPlannedCommands.begin(),
                   m_testPlannedCommands.end(),
                   [](const Command &left, const Command &right) {
                     if (left.record_sequence != 0 && right.record_sequence != 0 &&
                         left.record_sequence != right.record_sequence)
                     {
                       return left.record_sequence < right.record_sequence;
                     }
                     return false;
                   });
  return m_testPlannedCommands;
}

void LN_CommandBuffer::AppendSetWorldPosition(KX_GameObject *gameobj,
                                              const MT_Vector3 &position,
                                              uint64_t sort_key,
                                              uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(CommandType::SetWorldPosition, gameobj, position, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendSetLocalLinearVelocity(KX_GameObject *gameobj,
                                                    const MT_Vector3 &velocity,
                                                    uint64_t sort_key,
                                                    uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(
      CommandType::SetLocalLinearVelocity, gameobj, velocity, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendSetAngularVelocity(KX_GameObject *gameobj,
                                                const MT_Vector3 &velocity,
                                                uint64_t sort_key,
                                                uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(CommandType::SetAngularVelocity, gameobj, velocity, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendSetLocalAngularVelocity(KX_GameObject *gameobj,
                                                     const MT_Vector3 &velocity,
                                                     uint64_t sort_key,
                                                     uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(
      CommandType::SetLocalAngularVelocity, gameobj, velocity, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendSetLocalPosition(KX_GameObject *gameobj,
                                              const MT_Vector3 &position,
                                              uint64_t sort_key,
                                              uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(CommandType::SetLocalPosition, gameobj, position, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendSetWorldScale(KX_GameObject *gameobj,
                                           const MT_Vector3 &scale,
                                           uint64_t sort_key,
                                           uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(CommandType::SetWorldScale, gameobj, scale, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendSetWorldOrientation(KX_GameObject *gameobj,
                                                 const MT_Vector3 &rotation,
                                                 uint64_t sort_key,
                                                 uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(
      CommandType::SetWorldOrientation, gameobj, rotation, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendSetLocalOrientation(KX_GameObject *gameobj,
                                                 const MT_Vector3 &rotation,
                                                 uint64_t sort_key,
                                                 uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(
      CommandType::SetLocalOrientation, gameobj, rotation, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendSetLocalScale(KX_GameObject *gameobj,
                                           const MT_Vector3 &scale,
                                           uint64_t sort_key,
                                           uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(CommandType::SetLocalScale, gameobj, scale, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendSetGameProperty(KX_GameObject *gameobj,
                                             const std::string &property_name,
                                             const LN_Value &value,
                                             uint64_t sort_key,
                                             uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::SetGameProperty;
    command.object = gameobj;
    command.object_handle = m_logicManager ?
                                m_logicManager->MakeObjectHandle(
                                    gameobj, gameobj ? gameobj->GetName() : std::string()) :
                                LN_ObjectHandle{};
    command.game_property_id = m_logicManager ? m_logicManager->InternGamePropertyName(
                                                   property_name) :
                                               LN_GamePropertyId{};
    command.property_name = property_name;
    command.property_value = value;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedPropertyCommand command;
  command.header = MakeCommandHeader(CommandType::SetGameProperty, sort_key, source_ref_index);
  command.type = CommandType::SetGameProperty;
  command.object = gameobj;
  command.object_handle = m_logicManager ?
                              m_logicManager->MakeObjectHandle(
                                  gameobj, gameobj ? gameobj->GetName() : std::string()) :
                              LN_ObjectHandle{};
  command.game_property_id = m_logicManager ? m_logicManager->InternGamePropertyName(
                                                 property_name) :
                                             LN_GamePropertyId{};
  command.property_name = property_name;
  command.value = value;
  AppendTypedCommand(m_propertyCommands, std::move(command));
}

void LN_CommandBuffer::AppendSetGamePropertyRef(LN_RuntimeTree *runtime_tree,
                                                KX_GameObject *gameobj,
                                                const uint32_t property_ref_index,
                                                const LN_Value &value,
                                                const uint64_t sort_key,
                                                const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (runtime_tree == nullptr || property_ref_index == LN_INVALID_INDEX) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::SetGameProperty;
    command.object = gameobj;
    command.object_handle = gameobj == runtime_tree->GetGameObject() ?
                                runtime_tree->GetOwnerObjectHandle() :
                                LN_ObjectHandle{};
    command.runtime_tree = runtime_tree;
    command.property_ref_index = property_ref_index;
    command.game_property_id = runtime_tree->GetGamePropertyId(property_ref_index);
    command.property_value = value;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    if (const std::shared_ptr<const LN_Program> program = runtime_tree->GetProgram()) {
      const std::vector<LN_GamePropertyRef> &property_refs = program->GetGamePropertyRefs();
      if (property_ref_index < property_refs.size()) {
        command.property_name_id = property_refs[property_ref_index].name_id;
        command.property_name_ptr = &property_refs[property_ref_index].name;
      }
    }
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedPropertyCommand command;
  command.header = MakeCommandHeader(CommandType::SetGameProperty, sort_key, source_ref_index);
  command.type = CommandType::SetGameProperty;
  command.object = gameobj;
  command.object_handle = gameobj == runtime_tree->GetGameObject() ?
                              runtime_tree->GetOwnerObjectHandle() :
                              LN_ObjectHandle{};
  command.runtime_tree = runtime_tree;
  command.property_ref_index = property_ref_index;
  command.game_property_id = runtime_tree->GetGamePropertyId(property_ref_index);
  command.value = value;
  if (const std::shared_ptr<const LN_Program> program = runtime_tree->GetProgram()) {
    const std::vector<LN_GamePropertyRef> &property_refs = program->GetGamePropertyRefs();
    if (property_ref_index < property_refs.size()) {
      command.property_name_id = property_refs[property_ref_index].name_id;
      command.property_name_ptr = &property_refs[property_ref_index].name;
    }
  }
  AppendTypedCommand(m_propertyCommands, std::move(command));
}

void LN_CommandBuffer::AppendSetTreeProperty(LN_RuntimeTree *runtime_tree,
                                             const uint32_t property_ref_index,
                                             const LN_Value &value,
                                             const uint64_t sort_key,
                                             const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (runtime_tree == nullptr || property_ref_index == LN_INVALID_INDEX) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::SetTreeProperty;
    command.runtime_tree = runtime_tree;
    command.property_ref_index = property_ref_index;
    command.property_value = value;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedPropertyCommand command;
  command.header = MakeCommandHeader(CommandType::SetTreeProperty, sort_key, source_ref_index);
  command.type = CommandType::SetTreeProperty;
  command.runtime_tree = runtime_tree;
  command.property_ref_index = property_ref_index;
  command.value = value;
  AppendTypedCommand(m_propertyCommands, std::move(command));
}

void LN_CommandBuffer::AppendAddObject(KX_GameObject *gameobj,
                                       const std::string &object_name,
                                       const float life_time,
                                       const bool full_copy,
                                       const uint64_t sort_key,
                                       const uint32_t source_ref_index)
{
  BLI_assert(m_isRecording);
  if (gameobj == nullptr || object_name.empty()) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::AddObject;
    command.object = gameobj;
    command.property_name = object_name;
    command.scalar_value = life_time;
    command.bool_value = full_copy;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedLifecycleCommand command;
  command.header = MakeCommandHeader(CommandType::AddObject, sort_key, source_ref_index);
  command.type = CommandType::AddObject;
  command.object = gameobj;
  command.name = object_name;
  command.scalar_value = life_time;
  command.bool_value = full_copy;
  AppendTypedCommand(m_lifecycleCommands, std::move(command));
}

void LN_CommandBuffer::AppendAddObjectFromRef(LN_RuntimeTree *runtime_tree,
                                              KX_GameObject *gameobj,
                                              const LN_RuntimeRef &source_ref,
                                              const float life_time,
                                              const bool full_copy,
                                              const uint32_t property_ref_index,
                                              const uint64_t sort_key,
                                              const uint32_t source_ref_index)
{
  BLI_assert(m_isRecording);
  if (runtime_tree == nullptr || gameobj == nullptr ||
      source_ref.kind != LN_RuntimeRefKind::Object)
  {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::AddObjectFromRef;
    command.runtime_tree = runtime_tree;
    command.object = gameobj;
    command.runtime_ref = source_ref;
    command.scalar_value = life_time;
    command.bool_value = full_copy;
    command.property_ref_index = property_ref_index;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedLifecycleCommand command;
  command.header = MakeCommandHeader(CommandType::AddObjectFromRef, sort_key, source_ref_index);
  command.type = CommandType::AddObjectFromRef;
  command.runtime_tree = runtime_tree;
  command.object = gameobj;
  command.runtime_ref = source_ref;
  command.scalar_value = life_time;
  command.bool_value = full_copy;
  command.property_ref_index = property_ref_index;
  AppendTypedCommand(m_lifecycleCommands, std::move(command));
}

KX_GameObject *LN_CommandBuffer::ExecuteAddObjectFromRefImmediate(
    LN_RuntimeTree *runtime_tree,
    KX_GameObject *gameobj,
    const LN_RuntimeRef &source_ref,
    const float life_time,
    const bool full_copy,
    const uint32_t property_ref_index,
    const uint32_t source_ref_index)
{
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (runtime_tree == nullptr || gameobj == nullptr ||
      source_ref.kind != LN_RuntimeRefKind::Object)
  {
    return nullptr;
  }

  if (m_allowWorkerRecording) {
    BLI_assert_unreachable();
    return nullptr;
  }

  AssertMainThread();

  Command command;
  command.type = CommandType::AddObjectFromRef;
  command.runtime_tree = runtime_tree;
  command.object = gameobj;
  command.scalar_value = life_time;
  command.runtime_ref = source_ref;
  command.bool_value = full_copy;
  command.source_ref_index = source_ref_index;
  command.property_ref_index = property_ref_index;

  LN_Value added_object;
  KX_GameObject *replica = ExecuteAddObjectFromRefCommand(command, added_object);
  if (property_ref_index != LN_INVALID_INDEX) {
    runtime_tree->SetTreePropertyValue(property_ref_index, added_object);
  }
  return replica;
}

void LN_CommandBuffer::AppendSetParent(KX_GameObject *gameobj,
                                       const std::string &parent_name,
                                       const bool compound,
                                       const bool ghost,
                                       const uint64_t sort_key,
                                       const uint32_t source_ref_index)
{
  BLI_assert(m_isRecording);
  if (gameobj == nullptr || parent_name.empty()) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::SetParent;
    command.object = gameobj;
    command.property_name = parent_name;
    command.bool_value = compound;
    command.secondary_bool_value = ghost;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedLifecycleCommand command;
  command.header = MakeCommandHeader(CommandType::SetParent, sort_key, source_ref_index);
  command.type = CommandType::SetParent;
  command.object = gameobj;
  command.name = parent_name;
  command.bool_value = compound;
  command.secondary_bool_value = ghost;
  AppendTypedCommand(m_lifecycleCommands, std::move(command));
}

void LN_CommandBuffer::AppendSetParentFromRef(LN_RuntimeTree *runtime_tree,
                                              KX_GameObject *gameobj,
                                              const LN_RuntimeRef &parent_ref,
                                              const bool compound,
                                              const bool ghost,
                                              const uint64_t sort_key,
                                              const uint32_t source_ref_index)
{
  BLI_assert(m_isRecording);
  if (runtime_tree == nullptr || gameobj == nullptr || parent_ref.kind != LN_RuntimeRefKind::Object) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::SetParentFromRef;
    command.runtime_tree = runtime_tree;
    command.object = gameobj;
    command.runtime_ref = parent_ref;
    command.bool_value = compound;
    command.secondary_bool_value = ghost;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedLifecycleCommand command;
  command.header = MakeCommandHeader(CommandType::SetParentFromRef, sort_key, source_ref_index);
  command.type = CommandType::SetParentFromRef;
  command.runtime_tree = runtime_tree;
  command.object = gameobj;
  command.runtime_ref = parent_ref;
  command.bool_value = compound;
  command.secondary_bool_value = ghost;
  AppendTypedCommand(m_lifecycleCommands, std::move(command));
}

void LN_CommandBuffer::AppendRemoveParent(KX_GameObject *gameobj,
                                          const uint64_t sort_key,
                                          const uint32_t source_ref_index)
{
  BLI_assert(m_isRecording);
  if (gameobj == nullptr) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::RemoveParent;
    command.object = gameobj;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedLifecycleCommand command;
  command.header = MakeCommandHeader(CommandType::RemoveParent, sort_key, source_ref_index);
  command.type = CommandType::RemoveParent;
  command.object = gameobj;
  AppendTypedCommand(m_lifecycleCommands, std::move(command));
}

void LN_CommandBuffer::AppendRemoveObject(KX_GameObject *gameobj,
                                          const uint64_t sort_key,
                                          const uint32_t source_ref_index)
{
  BLI_assert(m_isRecording);
  if (gameobj == nullptr) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::RemoveObject;
    command.object = gameobj;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedLifecycleCommand command;
  command.header = MakeCommandHeader(CommandType::RemoveObject, sort_key, source_ref_index);
  command.type = CommandType::RemoveObject;
  command.object = gameobj;
  AppendTypedCommand(m_lifecycleCommands, std::move(command));
}

void LN_CommandBuffer::AppendSetGravity(KX_GameObject *gameobj,
                                        const MT_Vector3 &gravity,
                                        const uint64_t sort_key,
                                        const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedObjectServiceCommand(CommandType::SetGravity,
                                  gameobj,
                                  MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f),
                                  0.0f,
                                  false,
                                  false,
                                  sort_key,
                                  source_ref_index,
                                  gravity);
}

void LN_CommandBuffer::AppendSetTimeScale(KX_GameObject *gameobj,
                                          const float timescale,
                                          const uint64_t sort_key,
                                          const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedObjectServiceCommand(CommandType::SetTimeScale,
                                  gameobj,
                                  MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f),
                                  std::max(0.0f, timescale),
                                  false,
                                  false,
                                  sort_key,
                                  source_ref_index);
}

void LN_CommandBuffer::AppendSetActiveCamera(KX_GameObject *camera,
                                             const uint64_t sort_key,
                                             const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (camera == nullptr) {
    return;
  }

  AppendTypedObjectServiceCommand(CommandType::SetActiveCamera,
                                  camera,
                                  MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f),
                                  0.0f,
                                  false,
                                  false,
                                  sort_key,
                                  source_ref_index);
}

void LN_CommandBuffer::AppendSetCameraFov(KX_GameObject *camera,
                                          const float fov_degrees,
                                          const uint64_t sort_key,
                                          const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (camera == nullptr) {
    return;
  }

  AppendTypedObjectServiceCommand(CommandType::SetCameraFov,
                                  camera,
                                  MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f),
                                  std::max(fov_degrees, 0.001f),
                                  false,
                                  false,
                                  sort_key,
                                  source_ref_index);
}

void LN_CommandBuffer::AppendSetCameraOrthoScale(KX_GameObject *camera,
                                                 const float scale,
                                                 const uint64_t sort_key,
                                                 const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (camera == nullptr) {
    return;
  }

  AppendTypedObjectServiceCommand(CommandType::SetCameraOrthoScale,
                                  camera,
                                  MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f),
                                  std::max(scale, 0.001f),
                                  false,
                                  false,
                                  sort_key,
                                  source_ref_index);
}

void LN_CommandBuffer::AppendSetCollisionGroup(KX_GameObject *gameobj,
                                               const int32_t group,
                                               const uint64_t sort_key,
                                               const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetCollisionGroup,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            0.0f,
                            std::clamp(group, 0, 65535),
                            0,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetPhysics(KX_GameObject *gameobj,
                                        const bool active,
                                        const uint64_t sort_key,
                                        const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetPhysics,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            0.0f,
                            0,
                            0,
                            active,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetDynamics(KX_GameObject *gameobj,
                                         const int32_t mode,
                                         const bool enabled,
                                         const uint64_t sort_key,
                                         const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetDynamics,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            0.0f,
                            std::clamp(mode, 0, 3),
                            0,
                            enabled,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendRebuildCollisionShape(KX_GameObject *gameobj,
                                                   const uint64_t sort_key,
                                                   const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::RebuildCollisionShape,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            0.0f,
                            0,
                            0,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetRigidBodyAttribute(KX_GameObject *gameobj,
                                                   const LN_RigidBodyAttribute attribute,
                                                   const MT_Vector3 &value,
                                                   const MT_Vector3 &secondary_value,
                                                   const float scalar_value,
                                                   const bool bool_value,
                                                   const bool secondary_bool_value,
                                                   const uint64_t sort_key,
                                                   const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetRigidBodyAttribute,
                            gameobj,
                            value,
                            secondary_value,
                            scalar_value,
                            int32_t(attribute),
                            0,
                            bool_value,
                            secondary_bool_value,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendCharacterJump(KX_GameObject *gameobj,
                                           const uint64_t sort_key,
                                           const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::CharacterJump,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            0.0f,
                            0,
                            0,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetCharacterGravity(KX_GameObject *gameobj,
                                                 const MT_Vector3 &gravity,
                                                 const uint64_t sort_key,
                                                 const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetCharacterGravity,
                            gameobj,
                            gravity,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            0.0f,
                            0,
                            0,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetCharacterJumpSpeed(KX_GameObject *gameobj,
                                                   const float jump_speed,
                                                   const uint64_t sort_key,
                                                   const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetCharacterJumpSpeed,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            jump_speed,
                            0,
                            0,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetCharacterMaxJumps(KX_GameObject *gameobj,
                                                  const int32_t max_jumps,
                                                  const uint64_t sort_key,
                                                  const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetCharacterMaxJumps,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            0.0f,
                            max_jumps,
                            0,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetCharacterWalkDirection(KX_GameObject *gameobj,
                                                       const MT_Vector3 &walk_direction,
                                                       const bool local,
                                                       const uint64_t sort_key,
                                                       const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetCharacterWalkDirection,
                            gameobj,
                            walk_direction,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            0.0f,
                            0,
                            0,
                            local,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetCharacterVelocity(KX_GameObject *gameobj,
                                                  const MT_Vector3 &velocity,
                                                  const float time,
                                                  const bool local,
                                                  const uint64_t sort_key,
                                                  const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetCharacterVelocity,
                            gameobj,
                            velocity,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            time,
                            0,
                            0,
                            local,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendVehicleControl(KX_GameObject *gameobj,
                                            const MT_Vector3 &control,
                                            const float steering,
                                            const uint64_t sort_key,
                                            const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::VehicleControl,
                            gameobj,
                            control,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            steering,
                            0,
                            0,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendVehicleApplyEngineForce(KX_GameObject *gameobj,
                                                     const float power,
                                                     const int32_t wheel_count,
                                                     const int32_t axis,
                                                     const uint64_t sort_key,
                                                     const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::VehicleApplyEngineForce,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            power,
                            wheel_count,
                            axis,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendVehicleApplyBraking(KX_GameObject *gameobj,
                                                 const float power,
                                                 const int32_t wheel_count,
                                                 const int32_t axis,
                                                 const uint64_t sort_key,
                                                 const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::VehicleApplyBraking,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            power,
                            wheel_count,
                            axis,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendVehicleApplySteering(KX_GameObject *gameobj,
                                                  const float steering,
                                                  const int32_t wheel_count,
                                                  const int32_t axis,
                                                  const uint64_t sort_key,
                                                  const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::VehicleApplySteering,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            steering,
                            wheel_count,
                            axis,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetVehicleSuspensionCompression(KX_GameObject *gameobj,
                                                             const float value,
                                                             const int32_t wheel_count,
                                                             const int32_t axis,
                                                             const uint64_t sort_key,
                                                             const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetVehicleSuspensionCompression,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            value,
                            wheel_count,
                            axis,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetVehicleSuspensionStiffness(KX_GameObject *gameobj,
                                                           const float value,
                                                           const int32_t wheel_count,
                                                           const int32_t axis,
                                                           const uint64_t sort_key,
                                                           const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetVehicleSuspensionStiffness,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            value,
                            wheel_count,
                            axis,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetVehicleSuspensionDamping(KX_GameObject *gameobj,
                                                         const float value,
                                                         const int32_t wheel_count,
                                                         const int32_t axis,
                                                         const uint64_t sort_key,
                                                         const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetVehicleSuspensionDamping,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            value,
                            wheel_count,
                            axis,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetVehicleWheelFriction(KX_GameObject *gameobj,
                                                     const float value,
                                                     const int32_t wheel_count,
                                                     const int32_t axis,
                                                     const uint64_t sort_key,
                                                     const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);
  if (gameobj == nullptr) {
    return;
  }

  AppendTypedPhysicsCommand(CommandType::SetVehicleWheelFriction,
                            gameobj,
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            MT_Vector3(0.0f, 0.0f, 0.0f),
                            value,
                            wheel_count,
                            axis,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetLinearVelocity(KX_GameObject *gameobj,
                                               const MT_Vector3 &velocity,
                                               uint64_t sort_key,
                                               uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(CommandType::SetLinearVelocity, gameobj, velocity, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendApplyImpulse(KX_GameObject *gameobj,
                                          const MT_Vector3 &attach,
                                          const MT_Vector3 &impulse,
                                          uint64_t sort_key,
                                          uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedPhysicsCommand(CommandType::ApplyImpulse,
                            gameobj,
                            impulse,
                            attach,
                            0.0f,
                            0,
                            0,
                            false,
                            false,
                            sort_key,
                            source_ref_index);
}

void LN_CommandBuffer::AppendSetVisibility(KX_GameObject *gameobj,
                                           bool visible,
                                           bool recursive,
                                           uint64_t sort_key,
                                           uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedObjectServiceCommand(CommandType::SetVisibility,
                                  gameobj,
                                  MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f),
                                  0.0f,
                                  visible,
                                  recursive,
                                  sort_key,
                                  source_ref_index);
}

void LN_CommandBuffer::AppendSetObjectColor(KX_GameObject *gameobj,
                                            const MT_Vector4 &color,
                                            uint64_t sort_key,
                                            uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedObjectServiceCommand(CommandType::SetObjectColor,
                                  gameobj,
                                  color,
                                  0.0f,
                                  false,
                                  false,
                                  sort_key,
                                  source_ref_index);
}

void LN_CommandBuffer::AppendMakeLightUnique(KX_GameObject *gameobj,
                                             const uint64_t sort_key,
                                             const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedObjectServiceCommand(CommandType::MakeLightUnique,
                                  gameobj,
                                  MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f),
                                  0.0f,
                                  false,
                                  false,
                                  sort_key,
                                  source_ref_index);
}

void LN_CommandBuffer::AppendSetLightColor(KX_GameObject *gameobj,
                                           const MT_Vector4 &color,
                                           uint64_t sort_key,
                                           uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedObjectServiceCommand(CommandType::SetLightColor,
                                  gameobj,
                                  color,
                                  0.0f,
                                  false,
                                  false,
                                  sort_key,
                                  source_ref_index);
}

void LN_CommandBuffer::AppendSetLightPower(KX_GameObject *gameobj,
                                           const float power,
                                           const uint64_t sort_key,
                                           const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedObjectServiceCommand(CommandType::SetLightPower,
                                  gameobj,
                                  MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f),
                                  power,
                                  false,
                                  false,
                                  sort_key,
                                  source_ref_index);
}

void LN_CommandBuffer::AppendSetLightShadow(KX_GameObject *gameobj,
                                            const bool use_shadow,
                                            const uint64_t sort_key,
                                            const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedObjectServiceCommand(CommandType::SetLightShadow,
                                  gameobj,
                                  MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f),
                                  0.0f,
                                  use_shadow,
                                  false,
                                  sort_key,
                                  source_ref_index);
}

void LN_CommandBuffer::AppendApplyMovement(KX_GameObject *gameobj,
                                           const MT_Vector3 &movement,
                                           bool local,
                                           uint64_t sort_key,
                                           uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(
      CommandType::ApplyMovement, gameobj, movement, local, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendApplyRotation(KX_GameObject *gameobj,
                                           const MT_Vector3 &rotation,
                                           bool local,
                                           uint64_t sort_key,
                                           uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(
      CommandType::ApplyRotation, gameobj, rotation, local, sort_key, source_ref_index);
}

void LN_CommandBuffer::FlushPendingObjectTransformCommands(KX_GameObject *gameobj,
                                                           const uint64_t max_sort_key)
{
  FlushPendingObjectTransformCommandsInternal(gameobj, max_sort_key, false);
}

void LN_CommandBuffer::FlushPendingObjectHierarchyTransformCommands(KX_GameObject *gameobj,
                                                                    const uint64_t max_sort_key)
{
  FlushPendingObjectTransformCommandsInternal(gameobj, max_sort_key, true);
}

void LN_CommandBuffer::FlushPendingObjectTransformCommandsInternal(KX_GameObject *gameobj,
                                                                   const uint64_t max_sort_key,
                                                                   const bool include_parents)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr || Size() == 0) {
    return;
  }

  if (m_allowWorkerRecording) {
    BLI_assert_unreachable();
    return;
  }

  std::unordered_set<KX_GameObject *> flush_objects;
  for (KX_GameObject *object = gameobj; object != nullptr;) {
    if (!flush_objects.insert(object).second) {
      break;
    }
    if (!include_parents) {
      break;
    }
    object = object->GetParent();
  }

  auto should_flush = [&](const Command &command) {
    return command.object != nullptr && flush_objects.find(command.object) != flush_objects.end() &&
           command.sort_key <= max_sort_key && IsObjectTransformCommand(command.type);
  };

  CommandStreamStats stats;
  const std::vector<Command> planned_commands = BuildPlannedCommands(false, true, &stats);
  std::vector<Command> pending_commands;
  for (const Command &command : planned_commands) {
    if (should_flush(command)) {
      pending_commands.push_back(command);
    }
  }

  if (pending_commands.empty()) {
    return;
  }

  SortCommands(pending_commands);
  for (const Command &command : pending_commands) {
    ApplyObjectTransformCommand(command);
  }

  m_commands.erase(std::remove_if(m_commands.begin(), m_commands.end(), should_flush),
                   m_commands.end());
  auto should_flush_typed = [&](const TypedVectorCommand &command) {
    return command.object != nullptr && flush_objects.find(command.object) != flush_objects.end() &&
           command.header.sort_key <= max_sort_key &&
           IsObjectTransformCommand(command.type);
  };
  m_transformCommands.erase(std::remove_if(m_transformCommands.begin(),
                                           m_transformCommands.end(),
                                           should_flush_typed),
                            m_transformCommands.end());
  m_relativeVectorCommands.erase(std::remove_if(m_relativeVectorCommands.begin(),
                                                m_relativeVectorCommands.end(),
                                                should_flush_typed),
                                 m_relativeVectorCommands.end());
  RefreshCommandStreamTracking();
}

void LN_CommandBuffer::AppendApplyForce(KX_GameObject *gameobj,
                                        const MT_Vector3 &force,
                                        bool local,
                                        uint64_t sort_key,
                                        uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(
      CommandType::ApplyForce, gameobj, force, local, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendApplyTorque(KX_GameObject *gameobj,
                                         const MT_Vector3 &torque,
                                         bool local,
                                         uint64_t sort_key,
                                         uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedVectorCommand(
      CommandType::ApplyTorque, gameobj, torque, local, sort_key, source_ref_index);
}

void LN_CommandBuffer::AppendSetCursorVisibility(const bool visible,
                                                 const uint64_t sort_key,
                                                 const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedRuntimeServiceCommand(CommandType::SetCursorVisibility,
                                   0,
                                   0,
                                   0u,
                                   0.0f,
                                   0.0f,
                                   visible,
                                   sort_key,
                                   source_ref_index);
}

void LN_CommandBuffer::AppendSetCursorPosition(const int32_t x,
                                               const int32_t y,
                                               const uint64_t sort_key,
                                               const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedRuntimeServiceCommand(CommandType::SetCursorPosition,
                                   x,
                                   y,
                                   0u,
                                   0.0f,
                                   0.0f,
                                   false,
                                   sort_key,
                                   source_ref_index);
}

void LN_CommandBuffer::AppendSetGamepadVibration(const int32_t gamepad_index,
                                                 const float strength_left,
                                                 const float strength_right,
                                                 const uint32_t duration_ms,
                                                 const uint64_t sort_key,
                                                 const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedRuntimeServiceCommand(CommandType::SetGamepadVibration,
                                   gamepad_index,
                                   0,
                                   duration_ms,
                                   strength_left,
                                   strength_right,
                                   false,
                                   sort_key,
                                   source_ref_index);
}

void LN_CommandBuffer::AppendSetWindowSize(const int32_t width,
                                           const int32_t height,
                                           const uint64_t sort_key,
                                           const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedRuntimeServiceCommand(CommandType::SetWindowSize,
                                   std::max(width, 1),
                                   std::max(height, 1),
                                   0u,
                                   0.0f,
                                   0.0f,
                                   false,
                                   sort_key,
                                   source_ref_index);
}

void LN_CommandBuffer::AppendSetFullscreen(const bool fullscreen,
                                           const uint64_t sort_key,
                                           const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedRuntimeServiceCommand(CommandType::SetFullscreen,
                                   0,
                                   0,
                                   0u,
                                   0.0f,
                                   0.0f,
                                   fullscreen,
                                   sort_key,
                                   source_ref_index);
}

void LN_CommandBuffer::AppendSetVSync(const int32_t mode,
                                      const uint64_t sort_key,
                                      const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedRuntimeServiceCommand(CommandType::SetVSync,
                                   SanitizeVSyncMode(mode),
                                   0,
                                   0u,
                                   0.0f,
                                   0.0f,
                                   false,
                                   sort_key,
                                   source_ref_index);
}

void LN_CommandBuffer::AppendSetShowFramerate(const bool show,
                                              const uint64_t sort_key,
                                              const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedRuntimeServiceCommand(CommandType::SetShowFramerate,
                                   0,
                                   0,
                                   0u,
                                   0.0f,
                                   0.0f,
                                   show,
                                   sort_key,
                                   source_ref_index);
}

void LN_CommandBuffer::AppendSetShowProfile(const bool show,
                                            const uint64_t sort_key,
                                            const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  AppendTypedRuntimeServiceCommand(CommandType::SetShowProfile,
                                   0,
                                   0,
                                   0u,
                                   0.0f,
                                   0.0f,
                                   show,
                                   sort_key,
                                   source_ref_index);
}

void LN_CommandBuffer::AppendPrint(const std::string &message,
                                   const uint64_t sort_key,
                                   const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  Command command;
  command.type = CommandType::Print;
  command.property_name = message;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendPrintTreeProperty(LN_RuntimeTree *runtime_tree,
                                               const uint32_t property_ref_index,
                                               const uint64_t sort_key,
                                               const uint32_t source_ref_index)
{
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (runtime_tree == nullptr || property_ref_index == LN_INVALID_INDEX) {
    return;
  }

  Command command;
  command.type = CommandType::Print;
  command.runtime_tree = runtime_tree;
  command.property_ref_index = property_ref_index;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendQuitGame(const uint64_t sort_key, const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  Command command;
  command.type = CommandType::QuitGame;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendRestartGame(const uint64_t sort_key, const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  Command command;
  command.type = CommandType::RestartGame;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendPlayAction(KX_GameObject *const gameobj,
                                        const std::string &action_name,
                                        const float start_frame,
                                        const float end_frame,
                                        const int32_t layer,
                                        const int32_t priority,
                                        const float blendin,
                                        const float layer_weight,
                                        const uint32_t animation_flags,
                                        const float playback_speed,
                                        const uint64_t sort_key,
                                        const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr || action_name.empty()) {
    return;
  }

  AppendTypedAnimationCommand(CommandType::PlayAction,
                              gameobj,
                              action_name,
                              start_frame,
                              end_frame,
                              blendin,
                              layer_weight,
                              playback_speed,
                              0.0f,
                              layer,
                              priority,
                              animation_flags,
                              sort_key,
                              source_ref_index);
}

void LN_CommandBuffer::AppendStopAction(KX_GameObject *const gameobj,
                                        const int32_t layer,
                                        const uint64_t sort_key,
                                        const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr) {
    return;
  }

  AppendTypedAnimationCommand(CommandType::StopAction,
                              gameobj,
                              std::string(),
                              0.0f,
                              0.0f,
                              0.0f,
                              1.0f,
                              1.0f,
                              0.0f,
                              layer,
                              0,
                              0u,
                              sort_key,
                              source_ref_index);
}

void LN_CommandBuffer::AppendSetActionFrame(KX_GameObject *const gameobj,
                                            const int32_t layer,
                                            const float frame,
                                            const uint64_t sort_key,
                                            const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr) {
    return;
  }

  AppendTypedAnimationCommand(CommandType::SetActionFrame,
                              gameobj,
                              std::string(),
                              0.0f,
                              0.0f,
                              0.0f,
                              1.0f,
                              1.0f,
                              frame,
                              layer,
                              0,
                              0u,
                              sort_key,
                              source_ref_index);
}

void LN_CommandBuffer::AppendStopAllSounds(const uint64_t sort_key, const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (m_typedCommandStreamsEnabled) {
    TypedAudioCommand command;
    command.header = MakeCommandHeader(CommandType::StopAllSounds, sort_key, source_ref_index);
    command.type = CommandType::StopAllSounds;
    AppendTypedCommand(m_audioCommands, std::move(command));
    return;
  }

  Command command;
  command.type = CommandType::StopAllSounds;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendPlaySound(const std::string &sound_name,
                                      const float volume,
                                      const float pitch,
                                      const bool loop,
                                      const uint64_t sort_key,
                                      const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (sound_name.empty()) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::PlaySound;
    command.property_name = sound_name;
    command.vector_value = MT_Vector3(volume, pitch, 0.0f);
    command.bool_value = loop;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedAudioCommand command;
  command.header = MakeCommandHeader(CommandType::PlaySound, sort_key, source_ref_index);
  command.type = CommandType::PlaySound;
  command.sound_id = m_logicManager ? m_logicManager->InternSoundName(sound_name) : LN_SoundId{};
  command.sound_name = sound_name;
  command.volume = volume;
  command.pitch = pitch;
  command.loop = loop;
  AppendTypedCommand(m_audioCommands, std::move(command));
}

void LN_CommandBuffer::AppendStopSound(const std::string &sound_name,
                                      const uint64_t sort_key,
                                      const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (sound_name.empty()) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::StopSound;
    command.property_name = sound_name;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedAudioCommand command;
  command.header = MakeCommandHeader(CommandType::StopSound, sort_key, source_ref_index);
  command.type = CommandType::StopSound;
  command.sound_id = m_logicManager ? m_logicManager->InternSoundName(sound_name) : LN_SoundId{};
  command.sound_name = sound_name;
  AppendTypedCommand(m_audioCommands, std::move(command));
}

void LN_CommandBuffer::AppendPlaySound3D(KX_GameObject *speaker_object,
                                        const std::string &sound_name,
                                        const float volume,
                                        const float pitch,
                                        const bool loop,
                                        const uint64_t sort_key,
                                        const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (sound_name.empty()) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::PlaySound3D;
    command.object = speaker_object;
    command.property_name = sound_name;
    command.vector_value = MT_Vector3(volume, pitch, 0.0f);
    command.bool_value = loop;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedAudioCommand command;
  command.header = MakeCommandHeader(CommandType::PlaySound3D, sort_key, source_ref_index);
  command.type = CommandType::PlaySound3D;
  command.speaker = speaker_object;
  command.sound_id = m_logicManager ? m_logicManager->InternSoundName(sound_name) : LN_SoundId{};
  command.sound_name = sound_name;
  command.volume = volume;
  command.pitch = pitch;
  command.loop = loop;
  AppendTypedCommand(m_audioCommands, std::move(command));
}

void LN_CommandBuffer::AppendPauseSound(const std::string &sound_name,
                                       const uint64_t sort_key,
                                       const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (sound_name.empty()) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::PauseSound;
    command.property_name = sound_name;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedAudioCommand command;
  command.header = MakeCommandHeader(CommandType::PauseSound, sort_key, source_ref_index);
  command.type = CommandType::PauseSound;
  command.sound_id = m_logicManager ? m_logicManager->InternSoundName(sound_name) : LN_SoundId{};
  command.sound_name = sound_name;
  AppendTypedCommand(m_audioCommands, std::move(command));
}

void LN_CommandBuffer::AppendResumeSound(const std::string &sound_name,
                                        const uint64_t sort_key,
                                        const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (sound_name.empty()) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::ResumeSound;
    command.property_name = sound_name;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedAudioCommand command;
  command.header = MakeCommandHeader(CommandType::ResumeSound, sort_key, source_ref_index);
  command.type = CommandType::ResumeSound;
  command.sound_id = m_logicManager ? m_logicManager->InternSoundName(sound_name) : LN_SoundId{};
  command.sound_name = sound_name;
  AppendTypedCommand(m_audioCommands, std::move(command));
}

void LN_CommandBuffer::AppendLoadBlendFile(const std::string &filepath,
                                           const uint64_t sort_key,
                                           const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  Command command;
  command.type = CommandType::LoadBlendFile;
  command.property_name = filepath;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendSetLogicTreeEnabled(KX_GameObject *target_object,
                                                 const std::string &tree_name,
                                                 const bool enabled,
                                                 const uint64_t sort_key,
                                                 const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (target_object == nullptr || tree_name.empty()) {
    return;
  }

  Command command;
  command.type = CommandType::SetLogicTreeEnabled;
  command.object = target_object;
  command.property_name = tree_name;
  command.bool_value = enabled;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendInstallLogicTree(KX_GameObject *target_object,
                                              const std::string &tree_name,
                                              const bool initial_enabled,
                                              const uint64_t sort_key,
                                              const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (target_object == nullptr || tree_name.empty()) {
    return;
  }

  Command command;
  command.type = CommandType::InstallLogicTree;
  command.object = target_object;
  command.property_name = tree_name;
  command.bool_value = initial_enabled;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendSendEvent(const std::string &subject,
                                        const LN_Value &content,
                                        KX_GameObject *messenger,
                                        KX_GameObject *target,
                                        const uint64_t sort_key,
                                        const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (subject.empty()) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::SendEvent;
    command.object = messenger;
    command.event_target_object = target;
    command.object_handle = m_logicManager ?
                                m_logicManager->MakeObjectHandle(
                                    messenger, messenger ? messenger->GetName() : std::string()) :
                                LN_ObjectHandle{};
    command.event_target_handle = m_logicManager ?
                                      m_logicManager->MakeObjectHandle(
                                          target, target ? target->GetName() : std::string()) :
                                      LN_ObjectHandle{};
    command.event_subject_id = m_logicManager ? m_logicManager->InternEventSubject(subject) :
                                               LN_EventSubjectId{};
    command.property_name = subject;
    command.property_value = content;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedEventCommand command;
  command.header = MakeCommandHeader(CommandType::SendEvent, sort_key, source_ref_index);
  command.event_subject_id = m_logicManager ? m_logicManager->InternEventSubject(subject) :
                                             LN_EventSubjectId{};
  command.dynamic_subject = subject;
  command.content = content;
  command.messenger = messenger;
  command.target = target;
  command.messenger_handle = m_logicManager ?
                                 m_logicManager->MakeObjectHandle(
                                     messenger, messenger ? messenger->GetName() : std::string()) :
                                 LN_ObjectHandle{};
  command.target_handle = m_logicManager ?
                              m_logicManager->MakeObjectHandle(
                                  target, target ? target->GetName() : std::string()) :
                              LN_ObjectHandle{};
  AppendTypedCommand(m_eventCommands, std::move(command));
}

void LN_CommandBuffer::AppendSendEvent(LN_RuntimeTree *runtime_tree,
                                       const LN_StringId subject_id,
                                       const LN_Value &content,
                                       KX_GameObject *messenger,
                                       KX_GameObject *target,
                                       const uint64_t sort_key,
                                       const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (runtime_tree == nullptr || !subject_id.IsValid()) {
    return;
  }

  const std::shared_ptr<const LN_Program> program = runtime_tree->GetProgram();
  if (program == nullptr || program->GetString(subject_id).empty()) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::SendEvent;
    command.runtime_tree = runtime_tree;
    command.object = messenger;
    command.event_target_object = target;
    command.object_handle = messenger == runtime_tree->GetGameObject() ?
                                runtime_tree->GetOwnerObjectHandle() :
                                LN_ObjectHandle{};
    command.event_target_handle = target == runtime_tree->GetGameObject() ?
                                      runtime_tree->GetOwnerObjectHandle() :
                                      LN_ObjectHandle{};
    command.property_name_id = subject_id;
    command.event_subject_id = runtime_tree->GetEventSubjectId(subject_id);
    command.property_value = content;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedEventCommand command;
  command.header = MakeCommandHeader(CommandType::SendEvent, sort_key, source_ref_index);
  command.runtime_tree = runtime_tree;
  command.subject_name_id = subject_id;
  command.event_subject_id = runtime_tree->GetEventSubjectId(subject_id);
  command.content = content;
  command.messenger = messenger;
  command.target = target;
  command.messenger_handle = messenger == runtime_tree->GetGameObject() ?
                                 runtime_tree->GetOwnerObjectHandle() :
                                 LN_ObjectHandle{};
  command.target_handle = target == runtime_tree->GetGameObject() ?
                              runtime_tree->GetOwnerObjectHandle() :
                              LN_ObjectHandle{};
  AppendTypedCommand(m_eventCommands, std::move(command));
}

void LN_CommandBuffer::AppendMoveToward(KX_GameObject *gameobj,
                                        const MT_Vector3 &target_position,
                                        const float speed,
                                        const float stop_distance,
                                        const bool dynamic,
                                        const bool use_frame_delta,
                                        const float frame_delta,
                                        const uint64_t sort_key,
                                        const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr) {
    return;
  }

  AppendTypedMotionCommand(CommandType::MoveToward,
                           gameobj,
                           nullptr,
                           target_position,
                           MT_Vector3(stop_distance, frame_delta, 0.0f),
                           speed,
                           0,
                           0,
                           dynamic,
                           use_frame_delta,
                           sort_key,
                           source_ref_index);
}

void LN_CommandBuffer::AppendSlowFollow(KX_GameObject *gameobj,
                                        KX_GameObject *target,
                                        const float factor,
                                        const uint8_t attribute,
                                        const uint64_t sort_key,
                                        const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr || target == nullptr) {
    return;
  }

  AppendTypedMotionCommand(CommandType::SlowFollow,
                           gameobj,
                           target,
                           MT_Vector3(0.0f, 0.0f, 0.0f),
                           MT_Vector3(0.0f, 0.0f, 0.0f),
                           factor,
                           attribute,
                           0,
                           false,
                           false,
                           sort_key,
                           source_ref_index);
}

void LN_CommandBuffer::AppendLoadScene(const std::string &scene_name,
                                       const uint64_t sort_key,
                                       const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (scene_name.empty()) {
    return;
  }

  Command command;
  command.type = CommandType::LoadScene;
  command.property_name = scene_name;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendSetScene(const std::string &scene_name,
                                      const uint64_t sort_key,
                                      const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (scene_name.empty()) {
    return;
  }

  Command command;
  command.type = CommandType::SetScene;
  command.property_name = scene_name;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendSaveGame(KX_GameObject *gameobj,
                                      const int32_t slot,
                                      const std::string &path,
                                      const uint64_t sort_key,
                                      const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr) {
    return;
  }

  Command command;
  command.type = CommandType::SaveGame;
  command.object = gameobj;
  command.int_value = slot;
  command.property_name = path;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendLoadGame(KX_GameObject *gameobj,
                                      const int32_t slot,
                                      const std::string &path,
                                      const uint64_t sort_key,
                                      const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr) {
    return;
  }

  Command command;
  command.type = CommandType::LoadGame;
  command.object = gameobj;
  command.int_value = slot;
  command.property_name = path;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::AppendAlignAxisToVector(KX_GameObject *gameobj,
                                               const MT_Vector3 &vector,
                                               const int32_t axis,
                                               const float factor,
                                               const uint64_t sort_key,
                                               const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr) {
    return;
  }

  AppendTypedMotionCommand(CommandType::AlignAxisToVector,
                           gameobj,
                           nullptr,
                           vector,
                           MT_Vector3(0.0f, 0.0f, 0.0f),
                           factor,
                           axis,
                           0,
                           false,
                           false,
                           sort_key,
                           source_ref_index);
}

void LN_CommandBuffer::AppendRotateToward(KX_GameObject *gameobj,
                                          const MT_Vector3 &target,
                                          const float factor,
                                          const int32_t rot_axis,
                                          const int32_t front_axis,
                                          const uint64_t sort_key,
                                          const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr) {
    return;
  }

  AppendTypedMotionCommand(CommandType::RotateToward,
                           gameobj,
                           nullptr,
                           target,
                           MT_Vector3(0.0f, 0.0f, 0.0f),
                           factor,
                           rot_axis,
                           front_axis,
                           false,
                           false,
                           sort_key,
                           source_ref_index);
}

void LN_CommandBuffer::AppendReplaceMesh(KX_GameObject *gameobj,
                                         KX_GameObject *mesh_object,
                                         const uint64_t sort_key,
                                         const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr || mesh_object == nullptr) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::ReplaceMesh;
    command.object = gameobj;
    command.runtime_tree = reinterpret_cast<LN_RuntimeTree *>(mesh_object);
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedLifecycleCommand command;
  command.header = MakeCommandHeader(CommandType::ReplaceMesh, sort_key, source_ref_index);
  command.type = CommandType::ReplaceMesh;
  command.object = gameobj;
  command.runtime_tree = reinterpret_cast<LN_RuntimeTree *>(mesh_object);
  AppendTypedCommand(m_lifecycleCommands, std::move(command));
}

void LN_CommandBuffer::AppendSetBonePoseLocation(KX_GameObject *armature_object,
                                                 const std::string &bone_name,
                                                 const MT_Vector3 &location,
                                                 const uint64_t sort_key,
                                                 const uint32_t source_ref_index,
                                                 const int32_t location_space,
                                                 const bool use_center)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (armature_object == nullptr || bone_name.empty()) {
    return;
  }

  AppendTypedArmatureCommand(CommandType::SetBonePoseLocation,
                             armature_object,
                             nullptr,
                             bone_name,
                             std::string(),
                             std::string(),
                             location,
                             MT_Vector3(0.0f, 0.0f, 0.0f),
                             LN_Value{},
                             0.0f,
                             location_space,
                             use_center ? 1 : 0,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetBonePoseTransform(KX_GameObject *armature_object,
                                                  const std::string &bone_name,
                                                  const MT_Vector3 &location,
                                                  const MT_Vector3 &rotation_euler,
                                                  const uint64_t sort_key,
                                                  const uint32_t source_ref_index,
                                                  const int32_t location_space,
                                                  const bool use_center)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (armature_object == nullptr || bone_name.empty()) {
    return;
  }

  AppendTypedArmatureCommand(CommandType::SetBonePoseTransform,
                             armature_object,
                             nullptr,
                             bone_name,
                             std::string(),
                             std::string(),
                             location,
                             rotation_euler,
                             LN_Value{},
                             0.0f,
                             location_space,
                             use_center ? 1 : 0,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetBonePoseRotation(KX_GameObject *armature_object,
                                                const std::string &bone_name,
                                                const MT_Vector3 &rotation_euler,
                                                const uint64_t sort_key,
                                                const uint32_t source_ref_index,
                                                const bool use_center,
                                                const int32_t rotation_space)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (armature_object == nullptr || bone_name.empty()) {
    return;
  }

  AppendTypedArmatureCommand(CommandType::SetBonePoseRotation,
                             armature_object,
                             nullptr,
                             bone_name,
                             std::string(),
                             std::string(),
                             rotation_euler,
                             MT_Vector3(0.0f, 0.0f, 0.0f),
                             LN_Value{},
                             0.0f,
                             rotation_space,
                             use_center ? 1 : 0,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetBonePoseScale(KX_GameObject *armature_object,
                                              const std::string &bone_name,
                                              const MT_Vector3 &scale,
                                              const uint64_t sort_key,
                                              const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (armature_object == nullptr || bone_name.empty()) {
    return;
  }

  AppendTypedArmatureCommand(CommandType::SetBonePoseScale,
                             armature_object,
                             nullptr,
                             bone_name,
                             std::string(),
                             std::string(),
                             scale,
                             MT_Vector3(0.0f, 0.0f, 0.0f),
                             LN_Value{},
                             0.0f,
                             0,
                             0,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetBoneAttribute(KX_GameObject *armature_object,
                                              const std::string &bone_name,
                                              const int32_t attribute,
                                              const int32_t scale_mode,
                                              const LN_Value &value,
                                              const uint64_t sort_key,
                                              const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (armature_object == nullptr || bone_name.empty() || !IsSupportedSetBoneAttribute(attribute)) {
    return;
  }

  AppendTypedArmatureCommand(CommandType::SetBoneAttribute,
                             armature_object,
                             nullptr,
                             bone_name,
                             std::string(),
                             std::string(),
                             MT_Vector3(0.0f, 0.0f, 0.0f),
                             MT_Vector3(0.0f, 0.0f, 0.0f),
                             value,
                             0.0f,
                             attribute,
                             scale_mode,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetBoneConstraintInfluence(KX_GameObject *armature_object,
                                                       const std::string &bone_name,
                                                       const std::string &constraint_name,
                                                       const float influence,
                                                       const uint64_t sort_key,
                                                       const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (armature_object == nullptr || bone_name.empty() || constraint_name.empty()) {
    return;
  }

  AppendTypedArmatureCommand(CommandType::SetBoneConstraintInfluence,
                             armature_object,
                             nullptr,
                             bone_name,
                             constraint_name,
                             std::string(),
                             MT_Vector3(0.0f, 0.0f, 0.0f),
                             MT_Vector3(0.0f, 0.0f, 0.0f),
                             LN_Value{},
                             influence,
                             0,
                             0,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetBoneConstraintTarget(LN_RuntimeTree *runtime_tree,
                                                     KX_GameObject *armature_object,
                                                     const std::string &bone_name,
                                                     const std::string &constraint_name,
                                                     const LN_Value &target_value,
                                                     const uint64_t sort_key,
                                                     const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (armature_object == nullptr || bone_name.empty() || constraint_name.empty()) {
    return;
  }

  AppendTypedArmatureCommand(CommandType::SetBoneConstraintTarget,
                             armature_object,
                             runtime_tree,
                             bone_name,
                             constraint_name,
                             std::string(),
                             MT_Vector3(0.0f, 0.0f, 0.0f),
                             MT_Vector3(0.0f, 0.0f, 0.0f),
                             target_value,
                             0.0f,
                             0,
                             0,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetBoneConstraintAttribute(LN_RuntimeTree *runtime_tree,
                                                        KX_GameObject *armature_object,
                                                        const std::string &bone_name,
                                                        const std::string &constraint_name,
                                                        const std::string &attribute_name,
                                                        const LN_Value &value,
                                                        const uint64_t sort_key,
                                                        const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (armature_object == nullptr || bone_name.empty() || constraint_name.empty() ||
      attribute_name.empty())
  {
    return;
  }

  AppendTypedArmatureCommand(CommandType::SetBoneConstraintAttribute,
                             armature_object,
                             runtime_tree,
                             bone_name,
                             constraint_name,
                             attribute_name,
                             MT_Vector3(0.0f, 0.0f, 0.0f),
                             MT_Vector3(0.0f, 0.0f, 0.0f),
                             value,
                             0.0f,
                             0,
                             0,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendAssignMaterialToSlot(LN_RuntimeTree *runtime_tree,
                                                  KX_GameObject *gameobj,
                                                  const LN_Value &material_value,
                                                  const int32_t slot,
                                                  const uint64_t sort_key,
                                                  const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr || slot < 0) {
    return;
  }

  AppendTypedMaterialCommand(CommandType::SetMaterialSlot,
                             gameobj,
                             runtime_tree,
                             material_value.runtime_ref,
                             material_value.reference_name,
                             std::string(),
                             std::string(),
                             std::string(),
                             LN_Value{},
                             0.0f,
                             slot,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetMaterialNodeSocketValue(LN_RuntimeTree *runtime_tree,
                                                        const LN_Value &material_value,
                                                        const std::string &node_name,
                                                        const std::string &socket_name,
                                                        const LN_Value &value,
                                                        const uint64_t sort_key,
                                                        const uint32_t source_ref_index)
{
  AppendSetMaterialNodeSocketValue(runtime_tree,
                                   nullptr,
                                   material_value,
                                   0,
                                   node_name,
                                   socket_name,
                                   value,
                                   sort_key,
                                   source_ref_index);
}

void LN_CommandBuffer::AppendSetMaterialNodeSocketValue(LN_RuntimeTree *runtime_tree,
                                                        KX_GameObject *gameobj,
                                                        const LN_Value &material_value,
                                                        const int32_t slot,
                                                        const std::string &node_name,
                                                        const std::string &socket_name,
                                                        const LN_Value &value,
                                                        const uint64_t sort_key,
                                                        const uint32_t source_ref_index,
                                                        const bool make_unique)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (node_name.empty() || socket_name.empty()) {
    return;
  }
  const bool explicit_shared_material = material_value.exists ||
                                        material_value.runtime_ref.IsValid() ||
                                        !material_value.reference_name.empty();
  if (!explicit_shared_material && (gameobj == nullptr || slot < 0)) {
    return;
  }

  AppendTypedMaterialCommand(CommandType::SetMaterialNodeSocketValue,
                             explicit_shared_material ? nullptr : gameobj,
                             runtime_tree,
                             material_value.runtime_ref,
                             material_value.reference_name,
                             node_name,
                             socket_name,
                             std::string(),
                             value,
                             make_unique ? 0.0f : 1.0f,
                             slot,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetMaterialParameter(LN_RuntimeTree *runtime_tree,
                                                  const LN_Value &material_value,
                                                  const std::string &node_name,
                                                  const std::string &socket_name,
                                                  const LN_Value &value,
                                                  const uint64_t sort_key,
                                                  const uint32_t source_ref_index)
{
  AppendSetMaterialParameter(runtime_tree,
                             nullptr,
                             material_value,
                             0,
                             node_name,
                             socket_name,
                             value,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetMaterialParameter(LN_RuntimeTree *runtime_tree,
                                                  KX_GameObject *gameobj,
                                                  const LN_Value &material_value,
                                                  const int32_t slot,
                                                  const std::string &node_name,
                                                  const std::string &socket_name,
                                                  const LN_Value &value,
                                                  const uint64_t sort_key,
                                                  const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (node_name.empty() || socket_name.empty()) {
    return;
  }
  const bool explicit_shared_material = material_value.exists ||
                                        material_value.runtime_ref.IsValid() ||
                                        !material_value.reference_name.empty();
  if (!explicit_shared_material && (gameobj == nullptr || slot < 0)) {
    return;
  }

  AppendTypedMaterialCommand(CommandType::SetMaterialParameter,
                             explicit_shared_material ? nullptr : gameobj,
                             runtime_tree,
                             material_value.runtime_ref,
                             material_value.reference_name,
                             node_name,
                             socket_name,
                             std::string(),
                             value,
                             0.0f,
                             slot,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetGeometryNodesInput(LN_RuntimeTree *runtime_tree,
                                                   KX_GameObject *gameobj,
                                                   const std::string &modifier_name,
                                                   const std::string &input_identifier,
                                                   const LN_Value &value,
                                                   const uint64_t sort_key,
                                                   const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr || modifier_name.empty() || input_identifier.empty()) {
    return;
  }
  AppendTypedMaterialCommand(CommandType::SetGeometryNodesInput,
                             gameobj,
                             runtime_tree,
                             LN_RuntimeRef{},
                             modifier_name,
                             input_identifier,
                             std::string(),
                             std::string(),
                             value,
                             0.0f,
                             0,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetGeometryNodeSocketValue(
    LN_RuntimeTree *runtime_tree,
    KX_GameObject *gameobj,
    const std::string &modifier_name,
    const std::string &node_name,
    const std::string &socket_identifier,
    const LN_Value &value,
    const uint64_t sort_key,
    const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr || modifier_name.empty() || node_name.empty() ||
      socket_identifier.empty())
  {
    return;
  }
  AppendTypedMaterialCommand(CommandType::SetGeometryNodeSocketValue,
                             gameobj,
                             runtime_tree,
                             LN_RuntimeRef{},
                             modifier_name,
                             node_name,
                             socket_identifier,
                             std::string(),
                             value,
                             0.0f,
                             0,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetCompositorNodeSocketValue(
    LN_RuntimeTree *runtime_tree,
    const int16_t target_id_code,
    const std::string &target_name,
    const std::string &node_name,
    const std::string &socket_identifier,
    const LN_Value &value,
    const uint64_t sort_key,
    const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (target_name.empty() || node_name.empty() || socket_identifier.empty()) {
    return;
  }
  AppendTypedMaterialCommand(CommandType::SetCompositorNodeSocketValue,
                             nullptr,
                             runtime_tree,
                             LN_RuntimeRef{},
                             target_name,
                             node_name,
                             socket_identifier,
                             std::string(),
                             value,
                             0.0f,
                             int32_t(target_id_code),
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendMakeNodeTreeUnique(KX_GameObject *gameobj,
                                                const int32_t editor_type,
                                                const int32_t slot,
                                                const std::string &target_name,
                                                const uint64_t sort_key,
                                                const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (!ELEM(editor_type, 1, 2, 3) ||
      (editor_type != 3 && gameobj == nullptr) ||
      (editor_type == 1 && slot < 0) ||
      (editor_type != 1 && target_name.empty()))
  {
    return;
  }
  AppendTypedMaterialCommand(CommandType::MakeNodeTreeUnique,
                             gameobj,
                             nullptr,
                             LN_RuntimeRef{},
                             target_name,
                             std::string(),
                             std::string(),
                             std::string(),
                             LN_Value{},
                             float(editor_type),
                             editor_type == 1 ? slot : 0,
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendSetNodeMute(LN_RuntimeTree *runtime_tree,
                                         const int16_t target_id_code,
                                         const std::string &target_name,
                                         const std::string &node_name,
                                         const bool muted,
                                         const uint64_t sort_key,
                                         const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (target_name.empty() || node_name.empty()) {
    return;
  }
  AppendTypedMaterialCommand(CommandType::SetNodeMute,
                             nullptr,
                             runtime_tree,
                             LN_RuntimeRef{},
                             target_name,
                             node_name,
                             std::string(),
                             std::string(),
                             LN_Value{},
                             muted ? 1.0f : 0.0f,
                             int32_t(target_id_code),
                             sort_key,
                             source_ref_index);
}

void LN_CommandBuffer::AppendEnableDisableModifier(KX_GameObject *gameobj,
                                                   const ModifierTarget target,
                                                   const std::string &modifier_name,
                                                   const int32_t index,
                                                   const bool enabled,
                                                   const uint64_t sort_key,
                                                   const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (gameobj == nullptr) {
    return;
  }

  LN_Value target_value;
  target_value.type = LN_ValueType::Int;
  target_value.exists = true;
  target_value.int_value = int32_t(target);
  AppendTypedMaterialCommand(CommandType::EnableDisableModifier,
                             gameobj,
                             nullptr,
                             LN_RuntimeRef{},
                             modifier_name,
                             std::string(),
                             std::string(),
                             std::string(),
                             target_value,
                             enabled ? 1.0f : 0.0f,
                             index,
                             sort_key,
                             source_ref_index);
}

int32_t LN_CommandBuffer::AssignGeometryNodesModifierNow(
    LN_RuntimeTree *runtime_tree,
    KX_GameObject *gameobj,
    const LN_Value &node_group_value,
    const ModifierAssignmentOperation operation,
    const ModifierTarget replace_target,
    const std::string &name,
    const int32_t index_or_id)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  blender::Object *object = gameobj != nullptr ? gameobj->GetBlenderObject() : nullptr;
  if (object == nullptr) {
    WarnAssignGeometryNodesModifierFailureOnce(nullptr, "missing target object");
    return 0;
  }
  blender::Main *bmain = CurrentMain(nullptr);
  if (bmain == nullptr) {
    WarnAssignGeometryNodesModifierFailureOnce(object, "missing Blender main database");
    return 0;
  }
  blender::bNodeTree *node_group = ResolveGeometryNodeTreeForCommand(
      bmain, runtime_tree, node_group_value);
  if (node_group == nullptr) {
    WarnAssignGeometryNodesModifierFailureOnce(
        object, "missing or incompatible Geometry Nodes node group");
    return 0;
  }
  if (!BKE_object_support_modifier_type_check(object, blender::eModifierType_Nodes)) {
    WarnAssignGeometryNodesModifierFailureOnce(
        object, "target object does not support Geometry Nodes modifiers");
    return 0;
  }

  blender::Scene *scene = gameobj->GetScene() != nullptr ?
                              gameobj->GetScene()->GetBlenderScene() :
                              nullptr;
  blender::ModifierData *modifier = nullptr;
  bool created_modifier = false;
  if (operation == ModifierAssignmentOperation::Replace) {
    switch (replace_target) {
      case ModifierTarget::Name:
        if (name.empty()) {
          WarnAssignGeometryNodesModifierFailureOnce(object, "modifier name is empty");
          return 0;
        }
        modifier = BKE_modifiers_findby_name(object, name.c_str());
        break;
      case ModifierTarget::First:
        modifier = static_cast<blender::ModifierData *>(object->modifiers.first);
        break;
      case ModifierTarget::Last:
        modifier = static_cast<blender::ModifierData *>(object->modifiers.last);
        break;
      case ModifierTarget::Index:
        if (index_or_id < 0) {
          WarnAssignGeometryNodesModifierFailureOnce(object,
                                                      "modifier index must be non-negative");
          return 0;
        }
        modifier = static_cast<blender::ModifierData *>(
            BLI_findlink(&object->modifiers, index_or_id));
        break;
      case ModifierTarget::PersistentId:
        if (index_or_id <= 0) {
          WarnAssignGeometryNodesModifierFailureOnce(object, "modifier ID must be positive");
          return 0;
        }
        modifier = BKE_modifiers_findby_persistent_uid(object, index_or_id);
        break;
    }
    if (modifier == nullptr) {
      WarnAssignGeometryNodesModifierFailureOnce(object, "modifier target was not found");
      return 0;
    }
    if (modifier->type != blender::eModifierType_Nodes) {
      WarnAssignGeometryNodesModifierFailureOnce(
          object, "replace target is not a Geometry Nodes modifier");
      return 0;
    }
  }
  else if (ELEM(operation,
                ModifierAssignmentOperation::Append,
                ModifierAssignmentOperation::Insert))
  {
    if (operation == ModifierAssignmentOperation::Insert &&
        !GeometryNodesModifierCanBeInsertedAt(*object, index_or_id))
    {
      WarnAssignGeometryNodesModifierFailureOnce(
          object, "requested insertion index is out of range or violates modifier ordering");
      return 0;
    }
    modifier = BKE_modifier_new(blender::eModifierType_Nodes);
    if (modifier == nullptr) {
      WarnAssignGeometryNodesModifierFailureOnce(object, "modifier allocation failed");
      return 0;
    }
    created_modifier = true;
    if (operation == ModifierAssignmentOperation::Append) {
      BKE_modifiers_add_at_end_if_possible(object, modifier);
    }
    else {
      blender::ModifierData *before = static_cast<blender::ModifierData *>(
          BLI_findlink(&object->modifiers, index_or_id));
      BLI_insertlinkbefore(&object->modifiers, before, modifier);
    }
    const std::string modifier_name = name.empty() ? std::string(node_group->id.name + 2) : name;
    blender::STRNCPY(modifier->name, modifier_name.c_str());
    BKE_modifier_unique_name(&object->modifiers, modifier);
    BKE_modifiers_persistent_uid_init(*object, *modifier);
  }
  else {
    WarnAssignGeometryNodesModifierFailureOnce(object, "unsupported assignment operation");
    return 0;
  }

  blender::NodesModifierData *nodes_modifier =
      reinterpret_cast<blender::NodesModifierData *>(modifier);
  const bool group_changed = nodes_modifier->node_group != node_group;
  if (group_changed &&
      !SetGeometryNodesModifierGroup(*bmain, scene, *object, *nodes_modifier, *node_group))
  {
    if (created_modifier) {
      BKE_modifier_remove_from_list(object, modifier);
      BKE_modifier_free(modifier);
    }
    WarnAssignGeometryNodesModifierFailureOnce(object, "node group assignment was rejected");
    return 0;
  }

  if (modifier->persistent_uid <= 0 ||
      BKE_modifiers_findby_persistent_uid(object, modifier->persistent_uid) != modifier)
  {
    modifier->persistent_uid = 0;
    BKE_modifiers_persistent_uid_init(*object, *modifier);
  }
  if ((created_modifier || group_changed) && gameobj->GetScene() != nullptr) {
    KX_Scene *kx_scene = gameobj->GetScene();
    const bool overlay_only = (object->gameflag & blender::OB_OVERLAY_COLLECTION) != 0;
    kx_scene->AppendToIdsToUpdate(&object->id, blender::ID_RECALC_GEOMETRY, overlay_only);
  }
  return modifier->persistent_uid;
}

void LN_CommandBuffer::AppendCopyProperty(KX_GameObject *source,
                                          KX_GameObject *target,
                                          const std::string &property_name,
                                          const uint64_t sort_key,
                                          const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  if (source == nullptr || target == nullptr || property_name.empty()) {
    return;
  }

  if (!m_typedCommandStreamsEnabled) {
    Command command;
    command.type = CommandType::CopyProperty;
    command.object = source;
    command.runtime_tree = reinterpret_cast<LN_RuntimeTree *>(target);
    command.property_name = property_name;
    command.sort_key = sort_key;
    command.source_ref_index = source_ref_index;
    AppendLegacyCommand(std::move(command));
    return;
  }

  TypedLifecycleCommand command;
  command.header = MakeCommandHeader(CommandType::CopyProperty, sort_key, source_ref_index);
  command.type = CommandType::CopyProperty;
  command.object = source;
  command.runtime_tree = reinterpret_cast<LN_RuntimeTree *>(target);
  command.name = property_name;
  AppendTypedCommand(m_lifecycleCommands, std::move(command));
}

void LN_CommandBuffer::AppendSubsystemCommand(const CommandSubsystem subsystem,
                                              const std::string &action_name,
                                              KX_GameObject *gameobj,
                                              const LN_RuntimeRef &runtime_ref,
                                              const LN_Value &payload,
                                              const uint64_t sort_key,
                                              const uint32_t source_ref_index)
{
  AssertMainThread();
  BLI_assert(m_isRecording);
  BLI_assert(!m_isFlushing);

  const CommandSubsystemPolicy policy = GetCommandSubsystemPolicy(subsystem);
  if (policy.write_policy != CommandThreadPolicy::MainThreadFlush || action_name.empty()) {
    return;
  }

  Command command;
  command.type = CommandType::SubsystemCommand;
  command.subsystem = subsystem;
  command.object = gameobj;
  command.runtime_ref = runtime_ref;
  command.property_value = payload;
  command.property_name = action_name;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  AppendLegacyCommand(std::move(command));
}

void LN_CommandBuffer::RemoveCommandsForGameObject(KX_GameObject *gameobj)
{
  AssertMainThread();

  if (gameobj == nullptr) {
    return;
  }

  if (m_isFlushing) {
    /* Avoid invalidating the flush iterator; remaining commands for this object are skipped. */
    m_removedObjectsDuringFlush.insert(gameobj);
    return;
  }

  if (Size() == 0) {
    return;
  }

  m_commands.erase(std::remove_if(m_commands.begin(),
                                  m_commands.end(),
                                  [gameobj](const Command &command) {
                                    return command.object == gameobj;
                                  }),
                   m_commands.end());
  auto remove_typed_vector = [gameobj](const TypedVectorCommand &command) {
    return command.object == gameobj;
  };
  m_transformCommands.erase(std::remove_if(m_transformCommands.begin(),
                                           m_transformCommands.end(),
                                           remove_typed_vector),
                            m_transformCommands.end());
  m_velocityCommands.erase(std::remove_if(m_velocityCommands.begin(),
                                          m_velocityCommands.end(),
                                          remove_typed_vector),
                           m_velocityCommands.end());
  m_relativeVectorCommands.erase(std::remove_if(m_relativeVectorCommands.begin(),
                                                m_relativeVectorCommands.end(),
                                                remove_typed_vector),
                                 m_relativeVectorCommands.end());
  m_propertyCommands.erase(std::remove_if(m_propertyCommands.begin(),
                                          m_propertyCommands.end(),
                                          [gameobj](const TypedPropertyCommand &command) {
                                            return command.object == gameobj;
                                          }),
                           m_propertyCommands.end());
  m_eventCommands.erase(std::remove_if(m_eventCommands.begin(),
                                       m_eventCommands.end(),
                                       [gameobj](const TypedEventCommand &command) {
                                         return command.messenger == gameobj;
                                       }),
                        m_eventCommands.end());
  m_audioCommands.erase(std::remove_if(m_audioCommands.begin(),
                                       m_audioCommands.end(),
                                       [gameobj](const TypedAudioCommand &command) {
                                         return command.speaker == gameobj;
                                       }),
                        m_audioCommands.end());
  m_lifecycleCommands.erase(std::remove_if(m_lifecycleCommands.begin(),
                                           m_lifecycleCommands.end(),
                                           [gameobj](const TypedLifecycleCommand &command) {
                                             return command.object == gameobj;
                                           }),
                            m_lifecycleCommands.end());
  m_objectServiceCommands.erase(
      std::remove_if(m_objectServiceCommands.begin(),
                     m_objectServiceCommands.end(),
                     [gameobj](const TypedObjectServiceCommand &command) {
                       return command.object == gameobj;
                     }),
      m_objectServiceCommands.end());
  m_animationCommands.erase(std::remove_if(m_animationCommands.begin(),
                                           m_animationCommands.end(),
                                           [gameobj](const TypedAnimationCommand &command) {
                                             return command.object == gameobj;
                                           }),
                            m_animationCommands.end());
  m_armatureCommands.erase(std::remove_if(m_armatureCommands.begin(),
                                          m_armatureCommands.end(),
                                          [gameobj](const TypedArmatureCommand &command) {
                                            return command.object == gameobj;
                                          }),
                           m_armatureCommands.end());
  m_materialCommands.erase(std::remove_if(m_materialCommands.begin(),
                                          m_materialCommands.end(),
                                          [gameobj](const TypedMaterialCommand &command) {
                                            return command.object == gameobj;
                                          }),
                           m_materialCommands.end());
  m_physicsCommands.erase(std::remove_if(m_physicsCommands.begin(),
                                         m_physicsCommands.end(),
                                         [gameobj](const TypedPhysicsCommand &command) {
                                           return command.object == gameobj;
                                         }),
                          m_physicsCommands.end());
  m_motionCommands.erase(std::remove_if(m_motionCommands.begin(),
                                        m_motionCommands.end(),
                                        [gameobj](const TypedMotionCommand &command) {
                                          return command.object == gameobj ||
                                                 command.target_object == gameobj;
                                        }),
                         m_motionCommands.end());
  RefreshCommandStreamTracking();
}

void LN_CommandBuffer::Flush()
{
  AssertMainThread();
  BLI_assert_msg(!m_isRecording, "Logic Nodes command buffer cannot flush while recording");
  BLI_assert_msg(!m_isFlushing, "Logic Nodes command buffer cannot flush recursively");

  m_removedObjectsDuringFlush.clear();
  m_isFlushing = true;

  struct SceneObjectNameCacheKey {
    KX_Scene *scene = nullptr;
    std::string name;

    bool operator==(const SceneObjectNameCacheKey &other) const
    {
      return scene == other.scene && name == other.name;
    }
  };

  struct SceneObjectNameCacheKeyHash {
    size_t operator()(const SceneObjectNameCacheKey &key) const
    {
      const size_t scene_hash = std::hash<const void *>()(key.scene);
      const size_t name_hash = std::hash<std::string>()(key.name);
      return scene_hash ^ (name_hash + 0x9e3779b97f4a7c15ULL + (scene_hash << 6) +
                           (scene_hash >> 2));
    }
  };

  struct AddObjectSourceLookup {
    KX_GameObject *inactive_source = nullptr;
    KX_GameObject *active_source = nullptr;
  };

  std::unordered_map<SceneObjectNameCacheKey,
                     AddObjectSourceLookup,
                     SceneObjectNameCacheKeyHash>
      add_object_source_cache;
  std::unordered_map<SceneObjectNameCacheKey, KX_GameObject *, SceneObjectNameCacheKeyHash>
      active_object_name_cache;

  auto object_removed_during_flush = [&](KX_GameObject *object) -> bool {
    return object != nullptr && !m_removedObjectsDuringFlush.empty() &&
           m_removedObjectsDuringFlush.find(object) != m_removedObjectsDuringFlush.end();
  };

  auto lookup_active_object_by_name = [&](KX_Scene *scene,
                                          const std::string &name) -> KX_GameObject * {
    SceneObjectNameCacheKey key;
    key.scene = scene;
    key.name = name;
    auto [item, inserted] = active_object_name_cache.emplace(std::move(key), nullptr);
    if (!inserted) {
      return item->second;
    }
    item->second = scene != nullptr && scene->GetObjectList() != nullptr ?
                       static_cast<KX_GameObject *>(scene->GetObjectList()->FindValue(name)) :
                       nullptr;
    return item->second;
  };

  auto lookup_add_object_source = [&](KX_Scene *scene,
                                      const std::string &name) -> const AddObjectSourceLookup & {
    SceneObjectNameCacheKey key;
    key.scene = scene;
    key.name = name;
    auto [item, inserted] = add_object_source_cache.emplace(std::move(key),
                                                            AddObjectSourceLookup());
    if (!inserted) {
      return item->second;
    }

    EXP_ListValue<KX_GameObject> *inactive_list = scene != nullptr ? scene->GetInactiveList() :
                                                                   nullptr;
    item->second.inactive_source = inactive_list ?
                                       static_cast<KX_GameObject *>(inactive_list->FindValue(name)) :
                                       nullptr;
    if (item->second.inactive_source == nullptr) {
      item->second.active_source = lookup_active_object_by_name(scene, name);
    }
    return item->second;
  };

  struct MaterialSocketCacheKey {
    blender::Material *material = nullptr;
    std::string node_name;
    std::string socket_name;

    bool operator==(const MaterialSocketCacheKey &other) const
    {
      return material == other.material && node_name == other.node_name &&
             socket_name == other.socket_name;
    }
  };

  struct MaterialSocketCacheKeyHash {
    size_t operator()(const MaterialSocketCacheKey &key) const
    {
      const size_t material_hash = std::hash<const void *>()(key.material);
      size_t hash = material_hash;
      hash ^= std::hash<std::string>()(key.node_name) + 0x9e3779b97f4a7c15ULL +
              (hash << 6) + (hash >> 2);
      hash ^= std::hash<std::string>()(key.socket_name) + 0x9e3779b97f4a7c15ULL +
              (hash << 6) + (hash >> 2);
      return hash;
    }
  };

  struct MaterialSocketLookup {
    blender::bNodeTree *ntree = nullptr;
    blender::bNode *node = nullptr;
    blender::bNodeSocket *socket = nullptr;
  };

  std::unordered_map<MaterialSocketCacheKey, MaterialSocketLookup, MaterialSocketCacheKeyHash>
      material_socket_cache;

  struct MaterialParameterBindingLookup {
    bool bound = false;
    std::string attribute_name;
  };

  std::unordered_map<MaterialSocketCacheKey,
                     MaterialParameterBindingLookup,
                     MaterialSocketCacheKeyHash>
      material_parameter_binding_cache;
  std::unordered_map<blender::Object *, KX_GameObject *> material_parameter_dirty_objects;

  auto mark_material_parameter_object_dirty = [&](KX_GameObject *game_object,
                                                  blender::Object *object) {
    if (game_object == nullptr || object == nullptr || object_removed_during_flush(game_object)) {
      return;
    }
    material_parameter_dirty_objects.emplace(object, game_object);
  };

  auto flush_material_parameter_object_updates = [&]() {
    for (const auto &[object, game_object] : material_parameter_dirty_objects) {
      if (object == nullptr || game_object == nullptr || object_removed_during_flush(game_object)) {
        continue;
      }
      TagMaterialParameterObjectUpdate(game_object, object);
    }
    material_parameter_dirty_objects.clear();
  };

  auto lookup_material_socket = [&](blender::Material *material,
                                    const std::string &node_name,
                                    const std::string &socket_name) -> MaterialSocketLookup {
    MaterialSocketCacheKey key;
    key.material = material;
    key.node_name = node_name;
    key.socket_name = socket_name;
    auto [item, inserted] = material_socket_cache.emplace(std::move(key), MaterialSocketLookup());
    if (!inserted) {
      return item->second;
    }
    if (material == nullptr || material->nodetree == nullptr) {
      return item->second;
    }
    item->second.ntree = material->nodetree;
    item->second.node = FindMaterialNodeByUserName(item->second.ntree, node_name);
    item->second.socket = item->second.node ?
                              FindInputSocketByIdentifierOrName(*item->second.node, socket_name) :
                              nullptr;
    return item->second;
  };

  auto ensure_material_parameter_binding_cached =
      [&](blender::Main *bmain,
          blender::Material *material,
          const std::string &node_name,
          const std::string &socket_name,
          const MaterialSocketLookup &lookup) -> MaterialParameterBindingLookup {
    MaterialSocketCacheKey key;
    key.material = material;
    key.node_name = node_name;
    key.socket_name = socket_name;
    auto [item, inserted] = material_parameter_binding_cache.emplace(
        std::move(key), MaterialParameterBindingLookup());
    if (!inserted) {
      return item->second;
    }

    std::string attribute_name;
    if (!EnsureMaterialParameterBinding(
            bmain, material, lookup.ntree, lookup.node, lookup.socket, &attribute_name))
    {
      return item->second;
    }

    item->second.bound = true;
    item->second.attribute_name = std::move(attribute_name);
    return item->second;
  };

  auto apply_material_socket_value_command = [&](const Command &command) {
    if (command.secondary_property_name.empty() || command.tertiary_property_name.empty()) {
      return;
    }
    blender::Main *bmain = CurrentMain(nullptr);
    if (bmain == nullptr) {
      return;
    }

    const bool explicit_shared_material = command.runtime_ref.IsValid() ||
                                          !command.property_name.empty();
    blender::Material *material = nullptr;
    if (explicit_shared_material) {
      material = ResolveMaterialForCommand(
          bmain, command.runtime_tree, command.runtime_ref, command.property_name);
    }
    else {
      if (command.object == nullptr || command.int_value < 0) {
        return;
      }
      blender::Object *ob = command.object->GetBlenderObject();
      if (ob == nullptr || command.int_value >= ob->totcol) {
        return;
      }
      material = BKE_object_material_get(ob, short(command.int_value + 1));
    }
    if (material == nullptr || material->nodetree == nullptr) {
      return;
    }

    MaterialSocketLookup lookup = lookup_material_socket(
        material, command.secondary_property_name, command.tertiary_property_name);
    bool socket_needs_update = false;
    if (lookup.socket == nullptr ||
        !SocketDefaultNeedsLogicValueUpdate(
            command.runtime_tree, *lookup.socket, command.property_value, socket_needs_update))
    {
      return;
    }
    if (!socket_needs_update) {
      return;
    }

    if (!explicit_shared_material && command.scalar_value == 0.0f) {
      material = EnsureObjectSlotMaterialUniqueForEdit(bmain, command.object, command.int_value);
      if (material == nullptr || material->nodetree == nullptr) {
        return;
      }
      lookup = lookup_material_socket(
          material, command.secondary_property_name, command.tertiary_property_name);
    }

    bool socket_changed = false;
    if (lookup.socket == nullptr ||
        !SetSocketDefaultFromLogicValue(
            command.runtime_tree, *lookup.socket, command.property_value, &socket_changed))
    {
      return;
    }
    if (!socket_changed) {
      return;
    }
    BKE_ntree_update_tag_socket_property(lookup.ntree, lookup.socket);
    NotifyMaterialNodeTreeUpdate(bmain, material, lookup.ntree);
  };

  auto geometry_modifier_for_command = [&](const Command &command,
                                           blender::Object *&r_object,
                                           blender::NodesModifierData *&r_modifier,
                                           blender::bNodeTree *&r_tree) {
    r_object = command.object ? command.object->GetBlenderObject() : nullptr;
    r_modifier = nullptr;
    r_tree = nullptr;
    if (r_object == nullptr || command.property_name.empty()) {
      return false;
    }
    blender::ModifierData *md = BKE_modifiers_findby_name(r_object,
                                                          command.property_name.c_str());
    if (md == nullptr || md->type != blender::eModifierType_Nodes) {
      return false;
    }
    r_modifier = reinterpret_cast<blender::NodesModifierData *>(md);
    r_tree = r_modifier->node_group;
    return r_tree != nullptr && r_tree->type == blender::NTREE_GEOMETRY;
  };

  auto apply_geometry_nodes_input_command = [&](const Command &command) {
    blender::Object *object = nullptr;
    blender::NodesModifierData *modifier = nullptr;
    blender::bNodeTree *ntree = nullptr;
    if (!geometry_modifier_for_command(command, object, modifier, ntree)) {
      warn_command_failure_once(command, "missing Geometry Nodes modifier or node group");
      return;
    }
    if (command.secondary_property_name.empty()) {
      warn_command_failure_once(command, "missing geometry-node interface input identifier");
      return;
    }

    blender::bContext *context = nullptr;
    if (CurrentMain(&context) == nullptr) {
      warn_command_failure_once(command, "missing Blender main database");
      return;
    }
    blender::PointerRNA modifier_ptr = blender::RNA_pointer_create_discrete(
        &object->id, blender::RNA_NodesModifier, modifier);
    blender::PointerRNA properties_ptr = blender::RNA_pointer_get(&modifier_ptr, "properties");
    blender::PointerRNA inputs_ptr = blender::RNA_pointer_get(&properties_ptr, "inputs");
    std::string input_identifier = command.secondary_property_name;
    blender::PointerRNA input_ptr = blender::RNA_pointer_get(&inputs_ptr,
                                                             input_identifier.c_str());
    if (input_ptr.data == nullptr) {
      ntree->ensure_interface_cache();
      const blender::bNodeTreeInterfaceSocket *name_match = nullptr;
      for (const blender::bNodeTreeInterfaceSocket *interface_socket : ntree->interface_inputs()) {
        if (interface_socket->name == command.secondary_property_name) {
          if (name_match != nullptr) {
            name_match = nullptr;
            break;
          }
          name_match = interface_socket;
        }
      }
      if (name_match != nullptr) {
        input_identifier = name_match->identifier;
        input_ptr = blender::RNA_pointer_get(&inputs_ptr, input_identifier.c_str());
      }
    }
    if (input_ptr.data == nullptr) {
      warn_command_failure_once(command, "geometry-node interface input was not found");
      return;
    }

    blender::PropertyRNA *type_prop = blender::RNA_struct_find_property(&input_ptr, "type");
    int value_mode = 0;
    if (type_prop == nullptr ||
        !blender::RNA_property_enum_value(
            context, &input_ptr, type_prop, "VALUE", &value_mode) ||
        blender::RNA_property_enum_get(&input_ptr, type_prop) != value_mode)
    {
      warn_command_failure_once(command,
                                "geometry-node interface input is not in Value mode");
      return;
    }
    blender::PropertyRNA *value_prop = blender::RNA_struct_find_property(&input_ptr, "value");
    bool needs_update = false;
    if (!RnaPropertyNeedsLogicValueUpdate(context,
                                          command.runtime_tree,
                                          input_ptr,
                                          value_prop,
                                          command.property_value,
                                          needs_update))
    {
      warn_command_failure_once(command,
                                "geometry-node interface input cannot accept this value type");
      return;
    }
    if (!needs_update) {
      return;
    }
    if (!SetRnaPropertyValue(
            context, command.runtime_tree, input_ptr, value_prop, command.property_value))
    {
      warn_command_failure_once(command,
                                "geometry-node interface input value conversion failed");
      return;
    }

    blender::RNA_property_update(context, &input_ptr, value_prop);
    if (KX_Scene *scene = command.object->GetScene()) {
      const bool overlay_only = (object->gameflag & blender::OB_OVERLAY_COLLECTION) != 0;
      scene->AppendToIdsToUpdate(&object->id, blender::ID_RECALC_GEOMETRY, overlay_only);
    }
  };

  auto apply_geometry_node_socket_value_command = [&](const Command &command) {
    blender::Object *object = nullptr;
    blender::NodesModifierData *modifier = nullptr;
    blender::bNodeTree *ntree = nullptr;
    if (!geometry_modifier_for_command(command, object, modifier, ntree)) {
      warn_command_failure_once(command, "missing Geometry Nodes modifier or node group");
      return;
    }
    if (command.secondary_property_name.empty() || command.tertiary_property_name.empty()) {
      warn_command_failure_once(command, "missing geometry node or socket identifier");
      return;
    }

    blender::bNode *node = FindNodeByUserName(ntree, command.secondary_property_name);
    blender::bNodeSocket *socket = node ? FindInputSocketByIdentifierOrName(
                                              *node, command.tertiary_property_name) :
                                          nullptr;
    if (socket == nullptr || (socket->flag & blender::SOCK_UNAVAIL) != 0) {
      warn_command_failure_once(command, "geometry node input socket was not found or unavailable");
      return;
    }
    ntree->ensure_topology_cache();
    if (socket->is_directly_linked()) {
      warn_command_failure_once(command,
                                "geometry node input socket is linked; its default is inactive");
      return;
    }

    bool needs_update = false;
    if (!SocketDefaultNeedsLogicValueUpdate(
            command.runtime_tree, *socket, command.property_value, needs_update))
    {
      warn_command_failure_once(command,
                                "geometry node input socket cannot accept this value type");
      return;
    }
    if (!needs_update) {
      return;
    }
    blender::Main *bmain = CurrentMain(nullptr);
    if (bmain == nullptr) {
      warn_command_failure_once(command, "missing Blender main database");
      return;
    }
    bool changed = false;
    if (!SetSocketDefaultFromLogicValue(
            command.runtime_tree, *socket, command.property_value, &changed))
    {
      warn_command_failure_once(command, "geometry node input value conversion failed");
      return;
    }
    if (!changed) {
      return;
    }

    BKE_ntree_update_tag_socket_property(ntree, socket);
    BKE_main_ensure_invariants(*bmain, ntree->id);
    AppendNodeTreeIdToUpdate(ntree);
  };

  auto apply_compositor_node_socket_value_command = [&](const Command &command) {
    blender::Main *bmain = CurrentMain(nullptr);
    if (bmain == nullptr || command.property_name.empty() ||
        command.secondary_property_name.empty() || command.tertiary_property_name.empty())
    {
      warn_command_failure_once(command, "missing compositor node-tree, node, or socket target");
      return;
    }

    const int16_t id_code = int16_t(command.int_value);
    if (!ELEM(id_code, int16_t(blender::ID_SCE), int16_t(blender::ID_NT))) {
      warn_command_failure_once(command, "unsupported compositor node-tree owner type");
      return;
    }
    blender::ID *target_id = BKE_libblock_find_name(
        bmain, id_code, command.property_name.c_str());
    if (target_id == nullptr || (target_id->tag & blender::ID_TAG_MISSING) != 0) {
      warn_command_failure_once(command, "compositor node-tree owner was not found");
      return;
    }

    blender::bNodeTree *ntree = id_code == blender::ID_SCE ?
                                    reinterpret_cast<blender::Scene *>(target_id)
                                        ->compositing_node_group :
                                    reinterpret_cast<blender::bNodeTree *>(target_id);
    if (ntree == nullptr || ntree->type != blender::NTREE_COMPOSIT) {
      warn_command_failure_once(command, "selected node tree is no longer a compositor tree");
      return;
    }

    blender::bNode *node = FindNodeByUserName(ntree, command.secondary_property_name);
    blender::bNodeSocket *socket = node ? FindInputSocketByIdentifierOrName(
                                              *node, command.tertiary_property_name) :
                                          nullptr;
    if (socket == nullptr || (socket->flag & blender::SOCK_UNAVAIL) != 0) {
      warn_command_failure_once(command,
                                "compositor node input socket was not found or unavailable");
      return;
    }
    ntree->ensure_topology_cache();
    if (socket->is_directly_linked()) {
      warn_command_failure_once(command,
                                "compositor node input socket is linked; its default is inactive");
      return;
    }

    bool needs_update = false;
    if (!SocketDefaultNeedsLogicValueUpdate(
            command.runtime_tree, *socket, command.property_value, needs_update))
    {
      warn_command_failure_once(command,
                                "compositor node input socket cannot accept this value type");
      return;
    }
    if (!needs_update) {
      return;
    }
    bool changed = false;
    if (!SetSocketDefaultFromLogicValue(
            command.runtime_tree, *socket, command.property_value, &changed))
    {
      warn_command_failure_once(command, "compositor node input value conversion failed");
      return;
    }
    if (!changed) {
      return;
    }

    BKE_ntree_update_tag_socket_property(ntree, socket);
    BKE_main_ensure_invariants(*bmain, ntree->id);
    AppendNodeTreeIdToUpdate(ntree);
  };

  auto apply_make_node_tree_unique_command = [&](const Command &command) {
    blender::Main *bmain = CurrentMain(nullptr);
    const int32_t editor_type = int32_t(command.scalar_value);
    const int32_t slot = command.int_value;
    if (bmain == nullptr || !ELEM(editor_type, 1, 2, 3)) {
      warn_command_failure_once(command, "missing database or invalid editor type");
      return;
    }

    if (editor_type == 1) {
      if (EnsureObjectSlotMaterialUniqueForEdit(bmain, command.object, slot) == nullptr) {
        warn_command_failure_once(command, "material slot could not be made single-user");
      }
      return;
    }

    if (editor_type == 2) {
      blender::Object *object = command.object != nullptr ? command.object->GetBlenderObject() :
                                                           nullptr;
      blender::ModifierData *md = object != nullptr && !command.property_name.empty() ?
                                      BKE_modifiers_findby_name(
                                          object, command.property_name.c_str()) :
                                      nullptr;
      if (md == nullptr || md->type != blender::eModifierType_Nodes) {
        warn_command_failure_once(command, "Geometry Nodes modifier was not found");
        return;
      }
      blender::NodesModifierData &modifier =
          *reinterpret_cast<blender::NodesModifierData *>(md);
      blender::bNodeTree *source = modifier.node_group;
      if (source == nullptr || source->type != blender::NTREE_GEOMETRY) {
        warn_command_failure_once(command, "modifier has no Geometry Nodes node group");
        return;
      }
      if (source->id.us <= 1) {
        return;
      }
      if (!BKE_id_copy_is_allowed(&source->id)) {
        warn_command_failure_once(command, "linked node group cannot be copied at runtime");
        return;
      }
      blender::bNodeTree *copy = reinterpret_cast<blender::bNodeTree *>(
          BKE_id_copy_ex(bmain, &source->id, nullptr, 0));
      if (copy == nullptr) {
        warn_command_failure_once(command, "Geometry Nodes node group copy failed");
        return;
      }
      id_us_min(&copy->id);
      blender::Scene *scene = command.object->GetScene() != nullptr ?
                                  command.object->GetScene()->GetBlenderScene() :
                                  nullptr;
      if (!SetGeometryNodesModifierGroup(*bmain, scene, *object, modifier, *copy)) {
        BKE_id_delete(bmain, copy);
        warn_command_failure_once(command, "copied node group could not be assigned");
        return;
      }
      if (KX_Scene *kx_scene = command.object->GetScene()) {
        const bool overlay_only = (object->gameflag & blender::OB_OVERLAY_COLLECTION) != 0;
        kx_scene->AppendToIdsToUpdate(&object->id, blender::ID_RECALC_GEOMETRY, overlay_only);
      }
      return;
    }

    blender::Scene *scene = command.property_name.empty() ?
                                nullptr :
                                reinterpret_cast<blender::Scene *>(BKE_libblock_find_name(
                                    bmain, blender::ID_SCE, command.property_name.c_str()));
    blender::bNodeTree *source = scene != nullptr ? scene->compositing_node_group : nullptr;
    if (source == nullptr || source->type != blender::NTREE_COMPOSIT) {
      warn_command_failure_once(command, "compositor Scene or node tree was not found");
      return;
    }
    if (source->id.us <= 1) {
      return;
    }
    if (!BKE_id_copy_is_allowed(&source->id)) {
      warn_command_failure_once(command, "linked compositor node tree cannot be copied");
      return;
    }
    blender::bNodeTree *copy = reinterpret_cast<blender::bNodeTree *>(
        BKE_id_copy_ex(bmain, &source->id, nullptr, 0));
    if (copy == nullptr) {
      warn_command_failure_once(command, "compositor node tree copy failed");
      return;
    }
    id_us_min(&copy->id);
    blender::PointerRNA scene_ptr = blender::RNA_id_pointer_create(&scene->id);
    blender::PropertyRNA *property = blender::RNA_struct_find_property(
        &scene_ptr, "compositing_node_group");
    blender::PointerRNA copy_ptr = blender::RNA_id_pointer_create(&copy->id);
    if (property != nullptr) {
      blender::RNA_property_pointer_set(&scene_ptr, property, copy_ptr, nullptr);
    }
    if (property == nullptr || scene->compositing_node_group != copy) {
      BKE_id_delete(bmain, copy);
      warn_command_failure_once(command, "copied compositor node tree could not be assigned");
      return;
    }
    blender::RNA_property_update_main(bmain, scene, &scene_ptr, property);
    BKE_main_ensure_invariants(*bmain, copy->id);
    AppendNodeTreeIdToUpdate(copy);
  };

  auto apply_set_node_mute_command = [&](const Command &command) {
    blender::Main *bmain = CurrentMain(nullptr);
    if (bmain == nullptr || command.property_name.empty() ||
        command.secondary_property_name.empty())
    {
      warn_command_failure_once(command, "missing node-tree or node target");
      return;
    }

    const int16_t id_code = int16_t(command.int_value);
    if (!ELEM(id_code,
              int16_t(blender::ID_MA),
              int16_t(blender::ID_SCE),
              int16_t(blender::ID_NT)))
    {
      warn_command_failure_once(command, "unsupported node-tree owner type");
      return;
    }
    blender::ID *target_id = BKE_libblock_find_name(
        bmain, id_code, command.property_name.c_str());
    if (target_id == nullptr || (target_id->tag & blender::ID_TAG_MISSING) != 0) {
      warn_command_failure_once(command, "node-tree owner was not found");
      return;
    }

    blender::bNodeTree *ntree = nullptr;
    switch (id_code) {
      case blender::ID_MA:
        ntree = reinterpret_cast<blender::Material *>(target_id)->nodetree;
        break;
      case blender::ID_SCE:
        ntree = reinterpret_cast<blender::Scene *>(target_id)->compositing_node_group;
        break;
      case blender::ID_NT:
        ntree = reinterpret_cast<blender::bNodeTree *>(target_id);
        break;
    }
    if (ntree == nullptr ||
        !ELEM(ntree->type,
              blender::NTREE_SHADER,
              blender::NTREE_GEOMETRY,
              blender::NTREE_COMPOSIT))
    {
      warn_command_failure_once(command, "selected node tree is no longer supported");
      return;
    }

    blender::bNode *node = FindNodeByUserName(ntree, command.secondary_property_name);
    if (node == nullptr) {
      warn_command_failure_once(command, "node was not found or its label is ambiguous");
      return;
    }
    if (node->typeinfo == nullptr || node->typeinfo->no_muting) {
      warn_command_failure_once(command, "selected node type cannot be muted");
      return;
    }

    const bool muted = command.scalar_value != 0.0f;
    if (((node->flag & blender::NODE_MUTED) != 0) == muted) {
      return;
    }
    SET_FLAG_FROM_TEST(node->flag, muted, blender::NODE_MUTED);
    BKE_ntree_update_tag_node_mute(ntree, node);
    BKE_main_ensure_invariants(*bmain, ntree->id);

    if (ntree->type == blender::NTREE_SHADER) {
      for (blender::Material &material : bmain->materials) {
        if (material.nodetree == nullptr ||
            !blender::bke::node_tree_contains_tree(*material.nodetree, *ntree))
        {
          continue;
        }
        GPU_material_free(&material.gpumaterial);
        DEG_id_tag_update(&material.id, blender::ID_RECALC_SHADING);
        AppendMaterialIdsToUpdate(&material, material.nodetree);
      }
    }
    AppendNodeTreeIdToUpdate(ntree);
  };

  auto apply_enable_disable_modifier_command = [&](const Command &command) {
    blender::Object *object = command.object ? command.object->GetBlenderObject() : nullptr;
    if (object == nullptr || command.property_value.type != LN_ValueType::Int) {
      warn_command_failure_once(command, "invalid modifier target");
      return;
    }

    blender::ModifierData *modifier = nullptr;
    switch (static_cast<ModifierTarget>(command.property_value.int_value)) {
      case ModifierTarget::Name:
        if (command.property_name.empty()) {
          warn_command_failure_once(command, "modifier name is empty");
          return;
        }
        modifier = BKE_modifiers_findby_name(object, command.property_name.c_str());
        break;
      case ModifierTarget::First:
        modifier = static_cast<blender::ModifierData *>(object->modifiers.first);
        break;
      case ModifierTarget::Last:
        modifier = static_cast<blender::ModifierData *>(object->modifiers.last);
        break;
      case ModifierTarget::Index:
        if (command.int_value < 0) {
          warn_command_failure_once(command, "modifier index must be non-negative");
          return;
        }
        modifier = static_cast<blender::ModifierData *>(
            BLI_findlink(&object->modifiers, command.int_value));
        break;
      case ModifierTarget::PersistentId:
        if (command.int_value <= 0) {
          warn_command_failure_once(command, "modifier ID must be positive");
          return;
        }
        modifier = BKE_modifiers_findby_persistent_uid(object, command.int_value);
        break;
      default:
        warn_command_failure_once(command, "unsupported modifier target mode");
        return;
    }
    if (modifier == nullptr) {
      warn_command_failure_once(command, "modifier target was not found");
      return;
    }

    const bool enabled = command.scalar_value != 0.0f;
    const bool was_enabled = (modifier->mode & blender::eModifierMode_Realtime) != 0;
    if (enabled == was_enabled) {
      return;
    }
    if (enabled) {
      modifier->mode |= blender::eModifierMode_Realtime;
    }
    else {
      modifier->mode &= ~blender::eModifierMode_Realtime;
    }
    if (KX_Scene *scene = command.object->GetScene()) {
      const bool overlay_only = (object->gameflag & blender::OB_OVERLAY_COLLECTION) != 0;
      scene->AppendToIdsToUpdate(&object->id, blender::ID_RECALC_GEOMETRY, overlay_only);
    }
  };

  auto apply_material_parameter_command = [&](const Command &command) {
    if (command.secondary_property_name.empty() || command.tertiary_property_name.empty()) {
      return;
    }
    blender::Main *bmain = CurrentMain(nullptr);
    if (bmain == nullptr) {
      return;
    }

    const bool explicit_shared_material = command.runtime_ref.IsValid() ||
                                          !command.property_name.empty();
    if (explicit_shared_material) {
      apply_material_socket_value_command(command);
      return;
    }

    if (command.object == nullptr || command.int_value < 0) {
      return;
    }
    blender::Object *object = command.object->GetBlenderObject();
    if (object == nullptr || command.int_value >= object->totcol) {
      return;
    }

    blender::Material *material = BKE_object_material_get(object, short(command.int_value + 1));
    if (material == nullptr || material->nodetree == nullptr) {
      return;
    }

    MaterialSocketLookup lookup = lookup_material_socket(
        material, command.secondary_property_name, command.tertiary_property_name);
    if (lookup.socket == nullptr ||
        !MaterialParameterSocketSupportsObjectAttribute(*lookup.socket))
    {
      return;
    }

    std::array<float, 4> values{};
    if (!LogicValueAsMaterialParameterValues(*lookup.socket, command.property_value, values)) {
      return;
    }

    const MaterialParameterBindingLookup binding = ensure_material_parameter_binding_cached(
        bmain, material, command.secondary_property_name, command.tertiary_property_name, lookup);
    if (!binding.bound || binding.attribute_name.empty()) {
      return;
    }

    if (SetObjectMaterialParameterProperty(object, binding.attribute_name, values)) {
      mark_material_parameter_object_dirty(command.object, object);
    }
  };

  auto flush_command = [&](const Command &command) {
    if (object_removed_during_flush(command.object)) {
      return;
    }
    if (command.object == nullptr && !AllowsObjectlessFlush(command.type)) {
      warn_command_failure_once(command, "missing target object");
      return;
    }

    switch (command.type) {
      case CommandType::SetWorldPosition:
      case CommandType::SetLocalPosition:
      case CommandType::SetWorldOrientation:
      case CommandType::SetLocalOrientation:
      case CommandType::SetWorldScale:
      case CommandType::SetLocalScale:
        ApplyObjectTransformCommand(command);
        break;
      case CommandType::SetLinearVelocity:
        command.object->setLinearVelocity(command.vector_value, false);
        break;
      case CommandType::SetLocalLinearVelocity:
        command.object->setLinearVelocity(command.vector_value, true);
        break;
      case CommandType::SetAngularVelocity:
        command.object->setAngularVelocity(command.vector_value, false);
        break;
      case CommandType::SetLocalAngularVelocity:
        command.object->setAngularVelocity(command.vector_value, true);
        break;
      case CommandType::ApplyImpulse:
        if (PHY_IPhysicsController *physics_controller = command.object->GetPhysicsController()) {
          physics_controller->ApplyImpulse(command.secondary_vector_value,
                                           command.vector_value,
                                           false);
        }
        break;
      case CommandType::SetVisibility:
        command.object->SetVisible(command.bool_value, command.secondary_bool_value);
        break;
      case CommandType::SetObjectColor:
        command.object->SetObjectColor(command.color_value);
        break;
      case CommandType::MakeLightUnique:
        EnsureLightDataUnique(command.object);
        break;
      case CommandType::SetLightColor:
        if (KX_LightObject *light_object = dynamic_cast<KX_LightObject *>(command.object)) {
          if (blender::Light *light = light_object->GetLight()) {
            light->r = command.color_value.x();
            light->g = command.color_value.y();
            light->b = command.color_value.z();
            NotifyLightUpdate(light);
          }
        }
        break;
      case CommandType::SetLightPower:
        if (KX_LightObject *light_object = dynamic_cast<KX_LightObject *>(command.object)) {
          if (blender::Light *light = light_object->GetLight()) {
            light->energy = std::max(command.scalar_value, 0.0f);
            NotifyLightUpdate(light);
          }
        }
        break;
      case CommandType::SetLightShadow:
        if (KX_LightObject *light_object = dynamic_cast<KX_LightObject *>(command.object)) {
          if (blender::Light *light = light_object->GetLight()) {
            if (command.bool_value) {
              light->mode |= blender::LA_SHADOW;
            }
            else {
              light->mode &= ~blender::LA_SHADOW;
            }
            NotifyLightUpdate(light);
          }
        }
        break;
      case CommandType::ApplyMovement:
      case CommandType::ApplyRotation:
        ApplyObjectTransformCommand(command);
        break;
      case CommandType::ApplyForce:
        command.object->ApplyForce(command.vector_value, command.bool_value);
        break;
      case CommandType::ApplyTorque:
        command.object->ApplyTorque(command.vector_value, command.bool_value);
        break;
      case CommandType::SetGameProperty: {
        if (!command.game_property_id.IsValid() && m_logicManager != nullptr &&
            !command.property_name.empty())
        {
          m_logicManager->InternGamePropertyName(command.property_name);
        }
        const std::string *property_name = logic_node_command_game_property_name(command,
                                                                                m_logicManager);
        if (property_name == nullptr || property_name->empty()) {
          break;
        }

        if (!apply_game_property_value(command.object, *property_name, command.property_value)) {
          warn_command_failure_once(command, "unsupported game-property payload type");
        }
        break;
      }
      case CommandType::SetTreeProperty:
        if (command.runtime_tree != nullptr) {
          command.runtime_tree->SetTreePropertyValue(command.property_ref_index,
                                                    command.property_value);
        }
        else {
          warn_command_failure_once(command, "missing runtime tree for tree-property write");
        }
        break;
      case CommandType::AddObject: {
        KX_Scene *scene = command.object->GetScene();
        if (scene == nullptr) {
          break;
        }
        const AddObjectSourceLookup &source_lookup = lookup_add_object_source(
            scene, command.property_name);
        KX_GameObject *source = source_lookup.inactive_source;
        if (source == nullptr) {
          if (source_lookup.active_source != nullptr) {
            warn_add_object_source_not_inactive(command,
                                               scene,
                                               source_lookup.active_source,
                                               command.property_name);
          }
          break;
        }

        const float scene_lifespan = add_object_life_seconds_to_scene_lifespan(
          command.scalar_value, command.bool_value);
        KX_GameObject *replica = command.bool_value ?
                       scene->AddFullCopyObject(source,
                                   command.object,
                                   scene_lifespan) :
                       scene->AddReplicaObject(source,
                                   command.object,
                                   scene_lifespan);
        if (replica != nullptr) {
          if (!command.bool_value) {
            replica->Release();
          }
        }
        break;
      }
      case CommandType::AddObjectFromRef: {
        LN_Value added_object;
        ExecuteAddObjectFromRefCommand(command, added_object);
        if (command.runtime_tree != nullptr && command.property_ref_index != LN_INVALID_INDEX) {
          command.runtime_tree->SetTreePropertyValue(command.property_ref_index, added_object);
        }
        break;
      }
      case CommandType::SetParent: {
        KX_Scene *scene = command.object->GetScene();
        if (scene == nullptr) {
          break;
        }
        KX_GameObject *parent = lookup_active_object_by_name(scene, command.property_name);
        if (parent != nullptr && parent != command.object) {
          command.object->SetParent(parent, command.bool_value, command.secondary_bool_value);
        }
        break;
      }
      case CommandType::SetParentFromRef: {
        if (command.runtime_tree == nullptr) {
          break;
        }
        KX_GameObject *parent = command.runtime_tree->ResolveObjectRef(command.runtime_ref);
        if (parent != nullptr && parent != command.object) {
          command.object->SetParent(parent, command.bool_value, command.secondary_bool_value);
        }
        break;
      }
      case CommandType::RemoveParent:
        command.object->RemoveParent();
        break;
      case CommandType::RemoveObject:
        if (command.object->GetScene() != nullptr) {
          command.object->GetScene()->DelayedRemoveObject(command.object);
        }
        break;
      case CommandType::SetGravity:
        if (KX_Scene *scene = command.object->GetScene()) {
          scene->SetGravity(command.vector_value);
        }
        break;
      case CommandType::SetTimeScale:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          engine->SetTimeScale(command.scalar_value);
        }
        break;
      case CommandType::SetActiveCamera:
        if (KX_Camera *camera = dynamic_cast<KX_Camera *>(command.object)) {
          if (KX_Scene *scene = camera->GetScene()) {
            scene->SetActiveCamera(camera);
          }
        }
        break;
      case CommandType::SetCameraFov:
        if (KX_Camera *camera = dynamic_cast<KX_Camera *>(command.object)) {
          constexpr float radians_per_degree = 0.01745329251994329577f;
          const float fov_radians = std::max(command.scalar_value, 0.001f) * radians_per_degree;
          RAS_CameraData *camera_data = camera->GetCameraData();
          const float width = camera->GetSensorWidth();
          camera_data->m_lens = width / (2.0f * std::tan(0.5f * fov_radians));
          camera->InvalidateProjectionMatrix();
        }
        break;
      case CommandType::SetCameraOrthoScale:
        if (KX_Camera *camera = dynamic_cast<KX_Camera *>(command.object)) {
          camera->GetCameraData()->m_scale = std::max(command.scalar_value, 0.001f);
          camera->InvalidateProjectionMatrix();
        }
        break;
      case CommandType::SetCollisionGroup:
        command.object->SetCollisionGroup(static_cast<unsigned short>(command.int_value));
        break;
      case CommandType::SetPhysics:
        if (command.bool_value) {
          command.object->RestorePhysics(false);
        }
        else {
          command.object->SuspendPhysics(false, false);
        }
        break;
      case CommandType::SetDynamics:
        if (PHY_IPhysicsController *physics_controller = command.object->GetPhysicsController()) {
          if (command.int_value != int(PHY_DynamicsMode::Dynamic) || !command.object->GetParent())
          {
            physics_controller->SetDynamicsMode(static_cast<PHY_DynamicsMode>(command.int_value),
                                                command.bool_value);
          }
        }
        break;
      case CommandType::RebuildCollisionShape:
        if (PHY_IPhysicsController *physics_controller = command.object->GetPhysicsController()) {
          physics_controller->ReinstancePhysicsShape(command.object,
                                                     command.object->GetMesh(0),
                                                     false,
                                                     true);
        }
        break;
      case CommandType::SetRigidBodyAttribute:
        if (PHY_IPhysicsController *physics_controller = command.object->GetPhysicsController()) {
          apply_rigid_body_attribute(
              *physics_controller,
              static_cast<LN_RigidBodyAttribute>(command.int_value),
              command.vector_value,
              command.secondary_vector_value,
              command.scalar_value,
              command.bool_value,
              command.secondary_bool_value);
        }
        break;
      case CommandType::CharacterJump:
        if (KX_Scene *scene = command.object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(command.object)) {
              character->Jump();
            }
          }
        }
        break;
      case CommandType::SetCharacterGravity:
        if (KX_Scene *scene = command.object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(command.object)) {
              character->SetGravity(command.vector_value);
            }
          }
        }
        break;
      case CommandType::SetCharacterJumpSpeed:
        if (KX_Scene *scene = command.object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(command.object)) {
              character->SetJumpSpeed(std::max(command.scalar_value, 0.0f));
            }
          }
        }
        break;
      case CommandType::SetCharacterMaxJumps:
        if (KX_Scene *scene = command.object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(command.object)) {
              character->SetMaxJumps(std::max(command.int_value, 0));
            }
          }
        }
        break;
      case CommandType::SetCharacterWalkDirection:
        if (KX_Scene *scene = command.object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(command.object)) {
              MT_Vector3 walk_direction = command.vector_value;
              if (command.bool_value) {
                walk_direction = command.object->NodeGetWorldOrientation() * walk_direction;
              }
              character->SetWalkDirection(walk_direction);
            }
          }
        }
        break;
      case CommandType::SetCharacterVelocity:
        if (KX_Scene *scene = command.object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(command.object)) {
              character->SetVelocity(command.vector_value,
                                     std::max(command.scalar_value, 0.0f),
                                     command.bool_value);
            }
          }
        }
        break;
      case CommandType::VehicleControl:
        if (KX_Scene *scene = command.object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_IVehicle *vehicle = environment->GetVehicleConstraint(
                    command.object->GetPhysicsController()))
            {
              ApplyVehicleActuatorControl(command.object,
                                          vehicle,
                                          command.vector_value,
                                          command.scalar_value);
            }
          }
        }
        break;
      case CommandType::VehicleApplyEngineForce:
      case CommandType::VehicleApplyBraking:
      case CommandType::VehicleApplySteering:
      case CommandType::SetVehicleSuspensionCompression:
      case CommandType::SetVehicleSuspensionStiffness:
      case CommandType::SetVehicleSuspensionDamping:
      case CommandType::SetVehicleWheelFriction:
        if (KX_Scene *scene = command.object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_IVehicle *vehicle = environment->GetVehicleConstraint(
                    command.object->GetPhysicsController()))
            {
              Object *chassis_object = command.object->GetBlenderObject();
              const GameVehicleSettings *chassis_settings = chassis_object ? chassis_object->vehicle :
                                                                           nullptr;
              const auto selected_wheels = SelectVehicleWheels(command.object,
                                                               vehicle,
                                                               VehicleAxisFromInt(
                                                                   command.secondary_int_value),
                                                               command.int_value);
              for (const SelectedVehicleWheel &wheel : selected_wheels) {
                switch (command.type) {
                  case CommandType::VehicleApplyEngineForce: {
                    const float configured_force = ConfiguredWheelEngineForce(chassis_settings,
                                                                             wheel.settings);
                    const float applied_force = configured_force > 1.0e-4f ?
                                                    command.scalar_value * configured_force :
                                                    command.scalar_value;
                    vehicle->ApplyEngineForce(applied_force, wheel.index);
                    break;
                  }
                  case CommandType::VehicleApplyBraking: {
                    const float configured_brake = ConfiguredWheelBrakeTorque(chassis_settings,
                                                                              wheel.settings);
                    const float applied_brake = configured_brake > 1.0e-4f ?
                                                    std::max(command.scalar_value, 0.0f) *
                                                        configured_brake :
                                                    std::max(command.scalar_value, 0.0f);
                    vehicle->ApplyBraking(applied_brake, wheel.index);
                    break;
                  }
                  case CommandType::VehicleApplySteering: {
                    const float configured_steering = ConfiguredWheelSteeringAngle(wheel.settings);
                    if (configured_steering > 1.0e-4f) {
                      vehicle->SetSteeringValue(command.scalar_value * configured_steering,
                                               wheel.index);
                    }
                    break;
                  }
                  case CommandType::SetVehicleSuspensionCompression:
                    vehicle->SetSuspensionCompression(std::max(command.scalar_value, 0.0f),
                                                      wheel.index);
                    break;
                  case CommandType::SetVehicleSuspensionStiffness:
                    vehicle->SetSuspensionStiffness(std::max(command.scalar_value, 0.0f),
                                                    wheel.index);
                    break;
                  case CommandType::SetVehicleSuspensionDamping:
                    vehicle->SetSuspensionDamping(std::max(command.scalar_value, 0.0f),
                                                  wheel.index);
                    break;
                  case CommandType::SetVehicleWheelFriction:
                    vehicle->SetWheelFriction(std::max(command.scalar_value, 0.0f), wheel.index);
                    break;
                  default:
                    break;
                }
              }
            }
          }
        }
        break;
      case CommandType::SetCursorVisibility:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          if (RAS_ICanvas *canvas = engine->GetCanvas()) {
            canvas->SetMouseState(command.bool_value ? RAS_ICanvas::MOUSE_NORMAL :
                                                       RAS_ICanvas::MOUSE_INVISIBLE);
          }
        }
        break;
      case CommandType::SetCursorPosition:
        logic_node_set_cursor_position(KX_GetActiveEngine(),
                                       command.int_value,
                                       command.secondary_int_value);
        break;
      case CommandType::SetGamepadVibration:
        if (command.int_value >= 0 && command.int_value < JOYINDEX_MAX) {
          if (DEV_Joystick *joystick = DEV_Joystick::GetInstance(short(command.int_value))) {
            joystick->RumblePlay(command.scalar_value,
                                 command.vector_value.x(),
                                 command.property_ref_index);
          }
        }
        break;
      case CommandType::SetWindowSize:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          if (RAS_ICanvas *canvas = engine->GetCanvas()) {
            canvas->ResizeWindow(command.int_value, command.secondary_int_value);
          }
        }
        break;
      case CommandType::SetFullscreen:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          if (RAS_ICanvas *canvas = engine->GetCanvas()) {
            canvas->SetFullScreen(command.bool_value);
          }
        }
        break;
      case CommandType::SetVSync:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          if (RAS_ICanvas *canvas = engine->GetCanvas()) {
            canvas->SetSwapInterval(CanvasSwapIntervalFromVSyncMode(command.int_value));
          }
        }
        break;
      case CommandType::SetShowFramerate:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          engine->SetFlag(KX_KetsjiEngine::SHOW_FRAMERATE, command.bool_value);
        }
        break;
      case CommandType::SetShowProfile:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          engine->SetFlag(KX_KetsjiEngine::SHOW_PROFILE, command.bool_value);
        }
        break;
      case CommandType::SubsystemCommand:
        break;
      case CommandType::Print:
        if (command.runtime_tree != nullptr && command.property_ref_index != LN_INVALID_INDEX) {
          const LN_Value *value = command.runtime_tree->GetTreePropertyValue(
              command.property_ref_index);
          const std::string message = value ? LogicValueToPrintString(*value) : "None";
          printf("%s\n", message.c_str());
        }
        else {
          printf("%s\n", command.property_name.c_str());
        }
        break;
      case CommandType::QuitGame:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          engine->RequestExit(KX_ExitRequest::QUIT_GAME);
        }
        break;
      case CommandType::RestartGame:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          engine->SetNameNextGame(KX_GetMainPath());
          engine->RequestExit(KX_ExitRequest::RESTART_GAME);
        }
        break;
      case CommandType::SetLogicTreeEnabled:
        if (m_logicManager != nullptr) {
          m_logicManager->FlushSetLogicTreeEnabled(
              command.object, command.property_name, command.bool_value);
        }
        break;
      case CommandType::InstallLogicTree:
        if (m_logicManager != nullptr) {
          m_logicManager->FlushInstallLogicTree(
              command.object, command.property_name, command.bool_value);
        }
        break;
      case CommandType::LoadBlendFile:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          engine->SetNameNextGame(command.property_name);
          engine->RequestExit(KX_ExitRequest::START_OTHER_GAME);
        }
        break;
      case CommandType::PlayAction:
        if (command.object != nullptr) {
          const int play_mode = int(command.animation_flags & 0xFFu);
          const int blend_mode = int((command.animation_flags >> 8u) & 0xFFu);
          const int ipo_flags = int((command.animation_flags >> 16u) & 0xFFFFu);
          command.object->PlayAction(command.property_name,
                                     command.vector_value.x(),
                                     command.vector_value.y(),
                                     short(command.int_value),
                                     short(command.secondary_int_value),
                                     command.vector_value.z(),
                                     short(play_mode),
                                     command.secondary_vector_value.x(),
                                     short(ipo_flags),
                                     command.secondary_vector_value.y(),
                                     short(blend_mode));
        }
        break;
      case CommandType::StopAction:
        if (command.object != nullptr) {
          command.object->StopAction(short(command.int_value));
        }
        break;
      case CommandType::SetActionFrame:
        if (command.object != nullptr) {
          command.object->SetActionFrame(short(command.int_value), command.scalar_value);
        }
        break;
      case CommandType::StopAllSounds:
#ifdef WITH_AUDASPACE
        logic_native_sound_handles_stop_all();
        g_logic_native_sound_cache.clear();
        if (auto device = aud::DeviceManager::getDevice()) {
          device->stopAll();
        }
#endif
        break;
      case CommandType::PlaySound: {
#ifdef WITH_AUDASPACE
        if (command.property_name.empty()) {
          break;
        }
        KX_KetsjiEngine *engine = KX_GetActiveEngine();
        blender::bContext *ctx = engine ? engine->GetContext() : nullptr;
        blender::Main *bmain = ctx ? CTX_data_main(ctx) : nullptr;
        if (bmain == nullptr) {
          bmain = G_MAIN;
        }
        if (bmain == nullptr) {
          break;
        }
        auto device = aud::DeviceManager::getDevice();
        if (!device) {
          break;
        }
        try {
          AUD_Sound snd = logic_native_sound_resolve(bmain, command.property_name);
          if (!snd) {
            break;
          }
          logic_native_sound_handles_stop_by_name(command.property_name);
          AUD_Handle handle = device->play(snd, false);
          if (handle) {
            auto h3d = std::dynamic_pointer_cast<aud::I3DHandle>(handle);
            if (h3d) {
              h3d->setRelative(true);
            }
            handle->setVolume(std::max(command.vector_value.x(), 0.0f));
            handle->setPitch(std::max(command.vector_value.y(), 0.0001f));
            if (command.bool_value) {
              handle->setLoopCount(-1);
            }
            logic_native_sound_handles_register(command.property_name, std::move(handle));
          }
        }
        catch (const aud::Exception &) {
        }
#endif
        break;
      }
      case CommandType::StopSound: {
#ifdef WITH_AUDASPACE
        if (!command.property_name.empty()) {
          logic_native_sound_handles_stop_by_name(command.property_name);
        }
#endif
        break;
      }
      case CommandType::PlaySound3D: {
#ifdef WITH_AUDASPACE
        if (command.property_name.empty()) {
          break;
        }
        KX_KetsjiEngine *engine = KX_GetActiveEngine();
        blender::bContext *ctx = engine ? engine->GetContext() : nullptr;
        blender::Main *bmain = ctx ? CTX_data_main(ctx) : nullptr;
        if (bmain == nullptr) {
          bmain = G_MAIN;
        }
        if (bmain == nullptr) {
          break;
        }
        auto device = aud::DeviceManager::getDevice();
        if (!device) {
          break;
        }
        try {
          AUD_Sound snd = logic_native_sound_resolve(bmain, command.property_name);
          if (!snd) {
            break;
          }
          logic_native_sound_handles_stop_by_name(command.property_name);
          AUD_Handle handle = device->play(snd, false);
          if (handle) {
            auto h3d = std::dynamic_pointer_cast<aud::I3DHandle>(handle);
            if (h3d) {
              h3d->setRelative(true);
              logic_native_sound_apply_3d_from_speaker(h3d, command.object);
            }
            handle->setVolume(std::max(command.vector_value.x(), 0.0f));
            handle->setPitch(std::max(command.vector_value.y(), 0.0001f));
            if (command.bool_value) {
              handle->setLoopCount(-1);
            }
            logic_native_sound_handles_register(command.property_name, std::move(handle));
          }
        }
        catch (const aud::Exception &) {
        }
#endif
        break;
      }
      case CommandType::PauseSound: {
#ifdef WITH_AUDASPACE
        if (!command.property_name.empty()) {
          logic_native_sound_handles_pause_by_name(command.property_name);
        }
#endif
        break;
      }
      case CommandType::ResumeSound: {
#ifdef WITH_AUDASPACE
        if (!command.property_name.empty()) {
          logic_native_sound_handles_resume_by_name(command.property_name);
        }
#endif
        break;
      }
      case CommandType::SendEvent: {
        if (m_logicManager == nullptr) {
          break;
        }
        LN_EventSubjectId event_subject_id = command.event_subject_id;
        if (!command.event_subject_id.IsValid() && !command.property_name.empty()) {
          event_subject_id = m_logicManager->InternEventSubject(command.property_name);
        }
        const std::string *subject = logic_node_command_string(command, m_logicManager);
        if (subject == nullptr || subject->empty()) {
          break;
        }
        LN_EventEntry event;
        event.subject_id = event_subject_id;
        event.messenger_handle = command.object_handle.IsValid() ?
                                     command.object_handle :
                                     m_logicManager->MakeObjectHandle(
                                         command.object,
                                         command.object ? command.object->GetName() :
                                                          std::string());
        event.target_handle = command.event_target_handle.IsValid() ?
                                  command.event_target_handle :
                                  m_logicManager->MakeObjectHandle(
                                      command.event_target_object,
                                      command.event_target_object ?
                                          command.event_target_object->GetName() :
                                          std::string());
        event.subject = *subject;
        event.content = command.property_value;
        event.messenger = command.object;
        event.target = command.event_target_object;
        /* Default delivery: visible to all receivers at the start of the next logic tick. */
        m_logicManager->PushEvent(std::move(event));
        break;
      }
      case CommandType::MoveToward: {
        if (command.object == nullptr) {
          break;
        }
        const float dt = std::max(command.secondary_vector_value.y(), 0.0f);
        const MT_Vector3 current = command.object->NodeGetWorldPosition();
        const MT_Vector3 target = command.vector_value;
        const MT_Vector3 direction = target - current;
        const float distance = direction.length();
        const float stop_distance = command.secondary_vector_value.x();
        if (distance <= stop_distance) {
          break;
        }
        const float speed = command.scalar_value;
        if (command.bool_value) {
          MT_Vector3 velocity_direction = direction;
          velocity_direction.z() = 0.0f;
          if (velocity_direction.length2() <= MT_EPSILON * MT_EPSILON) {
            break;
          }
          velocity_direction.normalize();
          MT_Vector3 velocity = velocity_direction * speed;
          velocity.z() = command.object->GetLinearVelocity(false).z();
          command.object->setLinearVelocity(velocity, false);
          break;
        }
        const float step = std::min(speed * (command.secondary_bool_value ? dt : 1.0f),
                                    distance - stop_distance);
        const MT_Vector3 movement = direction.normalized() * step;
        command.object->ApplyMovement(movement, false);
        break;
      }
      case CommandType::SlowFollow: {
        if (command.object == nullptr || command.runtime_tree == nullptr) {
          break;
        }
        KX_GameObject *follow_target = reinterpret_cast<KX_GameObject *>(command.runtime_tree);
        const MT_Vector3 current = command.object->NodeGetWorldPosition();
        const MT_Vector3 target_pos = follow_target->NodeGetWorldPosition();
        const float factor = std::clamp(command.scalar_value, 0.0f, 1.0f);
        const MT_Vector3 new_pos = current.lerp(target_pos, factor);
        command.object->NodeSetWorldPosition(new_pos);
        break;
      }
      case CommandType::LoadScene: {
        if (command.property_name.empty()) {
          break;
        }
        KX_KetsjiEngine *engine = KX_GetActiveEngine();
        if (engine == nullptr) {
          break;
        }
        EXP_ListValue<KX_Scene> *scenes = engine->CurrentScenes();
        if (scenes == nullptr || scenes->GetCount() == 0) {
          break;
        }
        KX_Scene *current_scene = scenes->GetValue(0);
        KX_Scene *load_scene = engine->FindScene(command.property_name);
        if (load_scene != nullptr && current_scene != nullptr) {
          current_scene->MergeScene(load_scene);
        }
        break;
      }
      case CommandType::SetScene: {
        if (command.property_name.empty()) {
          break;
        }
        KX_KetsjiEngine *engine = KX_GetActiveEngine();
        if (engine == nullptr) {
          break;
        }
        EXP_ListValue<KX_Scene> *scenes = engine->CurrentScenes();
        if (scenes == nullptr || scenes->GetCount() == 0) {
          break;
        }
        KX_Scene *current_scene = scenes->GetValue(0);
        if (current_scene != nullptr) {
          engine->ReplaceScene(current_scene->GetName(), command.property_name);
        }
        break;
      }
      case CommandType::SaveGame: {
        KX_Scene *scene = command.object ? command.object->GetScene() : nullptr;
        if (scene == nullptr) {
          KX_KetsjiEngine *engine = KX_GetActiveEngine();
          if (engine == nullptr) {
            break;
          }
          EXP_ListValue<KX_Scene> *scenes = engine->CurrentScenes();
          if (scenes == nullptr || scenes->GetCount() == 0) {
            break;
          }
          scene = scenes->GetValue(0);
        }
        if (scene == nullptr) {
          break;
        }
        std::string save_path = command.property_name;
        if (save_path.empty()) {
          save_path = "save_" + std::to_string(command.int_value) + ".sav";
        }
        FILE *file = fopen(save_path.c_str(), "wb");
        if (file == nullptr) {
          break;
        }
        /* Write header. */
        static constexpr char header[] = "UPBGE_SAVE";
        fwrite(header, 1, 10, file);
        /* Owner-scoped save: persist only the tree owner's game properties. */
        KX_GameObject *owner = command.object;
        const int32_t obj_count = owner != nullptr ? 1 : 0;
        fwrite(&obj_count, sizeof(obj_count), 1, file);
        if (owner != nullptr) {
          const std::string &name = owner->GetName();
          const int32_t name_len = int32_t(name.length());
          fwrite(&name_len, sizeof(name_len), 1, file);
          fwrite(name.c_str(), 1, name_len, file);
          const std::vector<std::string> property_names = owner->GetPropertyNames();
          const int32_t prop_count = int32_t(property_names.size());
          fwrite(&prop_count, sizeof(prop_count), 1, file);
          for (const std::string &prop_name : property_names) {
            EXP_Value *prop = owner->GetProperty(prop_name);
            if (prop == nullptr) {
              continue;
            }
            const int32_t prop_name_len = int32_t(prop_name.length());
            fwrite(&prop_name_len, sizeof(prop_name_len), 1, file);
            fwrite(prop_name.c_str(), 1, prop_name_len, file);
            /* Write type tag: 0=int, 1=float, 2=bool, 3=string. */
            uint8_t type_tag = 3;
            std::string prop_value;
            if (dynamic_cast<EXP_IntValue *>(prop)) {
              type_tag = 0;
              prop_value = std::to_string(int(prop->GetNumber()));
            }
            else if (dynamic_cast<EXP_FloatValue *>(prop)) {
              type_tag = 1;
              prop_value = std::to_string(prop->GetNumber());
            }
            else if (EXP_BoolValue *bool_val = dynamic_cast<EXP_BoolValue *>(prop)) {
              type_tag = 2;
              prop_value = bool_val->GetBool() ? "1" : "0";
            }
            else {
              prop_value = prop->GetText();
            }
            fwrite(&type_tag, sizeof(type_tag), 1, file);
            const int32_t prop_value_len = int32_t(prop_value.length());
            fwrite(&prop_value_len, sizeof(prop_value_len), 1, file);
            fwrite(prop_value.c_str(), 1, prop_value_len, file);
          }
        }
        fclose(file);
        break;
      }
      case CommandType::LoadGame: {
        KX_Scene *scene = command.object ? command.object->GetScene() : nullptr;
        if (scene == nullptr) {
          KX_KetsjiEngine *engine = KX_GetActiveEngine();
          if (engine == nullptr) {
            break;
          }
          EXP_ListValue<KX_Scene> *scenes = engine->CurrentScenes();
          if (scenes == nullptr || scenes->GetCount() == 0) {
            break;
          }
          scene = scenes->GetValue(0);
        }
        if (scene == nullptr) {
          break;
        }
        std::string load_path = command.property_name;
        if (load_path.empty()) {
          load_path = "save_" + std::to_string(command.int_value) + ".sav";
        }
        FILE *file = fopen(load_path.c_str(), "rb");
        if (file == nullptr) {
          break;
        }
        /* Read header. */
        char header[10];
        if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
            memcmp(header, "UPBGE_SAVE", sizeof(header)) != 0)
        {
          fclose(file);
          break;
        }
        /* Read object count. */
        int32_t obj_count = 0;
        if (fread(&obj_count, sizeof(obj_count), 1, file) != 1) {
          fclose(file);
          break;
        }
        for (int i = 0; i < obj_count; i++) {
          int32_t name_len = 0;
          if (fread(&name_len, sizeof(name_len), 1, file) != 1) {
            break;
          }
          std::string obj_name(name_len, '\0');
          if (fread(&obj_name[0], 1, name_len, file) != size_t(name_len)) {
            break;
          }
          KX_GameObject *obj = nullptr;
          if (command.object != nullptr &&
              (obj_name == command.object->GetName() || obj_count == 1))
          {
            obj = command.object;
          }
          if (obj == nullptr) {
            if (SCA_LogicManager *logicmgr = scene->GetLogicManager()) {
              obj = static_cast<KX_GameObject *>(logicmgr->GetGameObjectByName(obj_name));
            }
          }
          if (obj == nullptr) {
            EXP_ListValue<KX_GameObject> *obj_list = scene->GetObjectList();
            if (obj_list != nullptr) {
              for (int j = 0; j < obj_list->GetCount(); j++) {
                KX_GameObject *candidate = obj_list->GetValue(j);
                if (candidate != nullptr && candidate->GetName() == obj_name) {
                  obj = candidate;
                  break;
                }
              }
            }
          }
          int32_t prop_count = 0;
          if (fread(&prop_count, sizeof(prop_count), 1, file) != 1) {
            break;
          }
          for (int p = 0; p < prop_count; p++) {
            int32_t prop_name_len = 0;
            if (fread(&prop_name_len, sizeof(prop_name_len), 1, file) != 1) {
              break;
            }
            std::string prop_name(prop_name_len, '\0');
            if (fread(&prop_name[0], 1, prop_name_len, file) != size_t(prop_name_len)) {
              break;
            }
            /* Read type tag. */
            uint8_t type_tag = 3;
            if (fread(&type_tag, sizeof(type_tag), 1, file) != 1) {
              break;
            }
            int32_t prop_value_len = 0;
            if (fread(&prop_value_len, sizeof(prop_value_len), 1, file) != 1) {
              break;
            }
            std::string prop_value(prop_value_len, '\0');
            if (fread(&prop_value[0], 1, prop_value_len, file) != size_t(prop_value_len)) {
              break;
            }
            if (obj != nullptr) {
              EXP_Value *val = nullptr;
              switch (type_tag) {
                case 0:
                  val = new EXP_IntValue(atoi(prop_value.c_str()), prop_name);
                  break;
                case 1:
                  val = new EXP_FloatValue(atof(prop_value.c_str()), prop_name);
                  break;
                case 2:
                  val = new EXP_BoolValue(prop_value == "1", prop_name);
                  break;
                default:
                  val = new EXP_StringValue(prop_value, prop_name);
                  break;
              }
              if (val != nullptr) {
                if (EXP_Value *existing = obj->GetProperty(prop_name)) {
                  existing->SetValue(val);
                  val->Release();
                }
                else {
                  obj->SetProperty(prop_name, val);
                }
              }
            }
          }
        }
        fclose(file);
        break;
      }
      case CommandType::AlignAxisToVector: {
        if (command.object == nullptr) {
          break;
        }
        const MT_Vector3 target_vector = command.vector_value.normalized();
        const int32_t axis = command.int_value;
        const float factor = std::clamp(command.scalar_value, 0.0f, 1.0f);
        MT_Vector3 local_axis;
        switch (axis) {
          case 0:
            local_axis = MT_Vector3(1.0f, 0.0f, 0.0f);
            break;
          case 1:
            local_axis = MT_Vector3(0.0f, 1.0f, 0.0f);
            break;
          default:
            local_axis = MT_Vector3(0.0f, 0.0f, 1.0f);
            break;
        }
        const MT_Vector3 cross = local_axis.cross(target_vector);
        const float dot = local_axis.dot(target_vector);
        if (cross.length2() < 1e-10f) {
          if (dot < 0.0f) {
            MT_Vector3 perp = (fabs(local_axis.x()) < 0.9f) ?
                                  MT_Vector3(1.0f, 0.0f, 0.0f) :
                                  MT_Vector3(0.0f, 1.0f, 0.0f);
            MT_Vector3 rot_axis = local_axis.cross(perp).normalized();
            MT_Quaternion q(rot_axis, MT_Scalar(M_PI * factor));
            command.object->NodeSetLocalOrientation(
                MT_Matrix3x3(q) * command.object->NodeGetLocalOrientation());
          }
          break;
        }
        MT_Vector3 rot_axis = cross.normalized();
        float angle = acos(std::clamp(dot, -1.0f, 1.0f)) * factor;
        MT_Quaternion q(rot_axis, MT_Scalar(angle));
        command.object->NodeSetLocalOrientation(
            MT_Matrix3x3(q) * command.object->NodeGetLocalOrientation());
        break;
      }
      case CommandType::RotateToward: {
        if (command.object == nullptr) {
          break;
        }
        const MT_Vector3 position = command.object->NodeGetWorldPosition();
        MT_Vector3 to_target = command.vector_value - position;
        if (to_target.length2() < 1e-10f) {
          break;
        }
        to_target.normalize();

        const int32_t rot_axis_index = std::clamp(command.int_value, 0, 2);
        const int32_t front_axis_index = std::clamp(command.secondary_int_value, 0, 5);
        const float factor = std::clamp(command.scalar_value, 0.0f, 1.0f);

        const MT_Matrix3x3 orientation = command.object->NodeGetWorldOrientation();
        MT_Vector3 local_rot_axis(0.0f, 0.0f, 0.0f);
        local_rot_axis[rot_axis_index] = 1.0f;
        const MT_Vector3 rot_axis_world = (orientation * local_rot_axis).normalized();

        const int front_axis = front_axis_index % 3;
        const bool front_negative = front_axis_index >= 3;
        MT_Vector3 local_front_axis(0.0f, 0.0f, 0.0f);
        local_front_axis[front_axis] = front_negative ? -1.0f : 1.0f;
        MT_Vector3 front_world = orientation * local_front_axis;

        auto project_on_plane = [](const MT_Vector3 &vector, const MT_Vector3 &normal) {
          return vector - normal * vector.dot(normal);
        };

        MT_Vector3 desired_dir = project_on_plane(to_target, rot_axis_world);
        MT_Vector3 front_dir = project_on_plane(front_world, rot_axis_world);
        if (desired_dir.length2() < 1e-10f || front_dir.length2() < 1e-10f) {
          break;
        }
        desired_dir.normalize();
        front_dir.normalize();

        const MT_Vector3 cross = front_dir.cross(desired_dir);
        const float dot = front_dir.dot(desired_dir);
        if (cross.length2() < 1e-10f) {
          if (dot < 0.0f) {
            MT_Vector3 perp = (fabs(rot_axis_world.x()) < 0.9f) ?
                                  MT_Vector3(1.0f, 0.0f, 0.0f) :
                                  MT_Vector3(0.0f, 1.0f, 0.0f);
            MT_Vector3 flip_axis = rot_axis_world.cross(perp).normalized();
            MT_Quaternion q(flip_axis, MT_Scalar(M_PI * factor));
            command.object->NodeSetLocalOrientation(
                MT_Matrix3x3(q) * command.object->NodeGetLocalOrientation());
          }
          break;
        }

        float angle = acos(std::clamp(dot, -1.0f, 1.0f));
        if (cross.dot(rot_axis_world) < 0.0f) {
          angle = -angle;
        }
        angle *= factor;
        MT_Quaternion q(rot_axis_world, MT_Scalar(angle));
        command.object->NodeSetLocalOrientation(
            MT_Matrix3x3(q) * command.object->NodeGetLocalOrientation());
        break;
      }
      case CommandType::ReplaceMesh: {
        if (command.object == nullptr || command.runtime_tree == nullptr) {
          break;
        }
        KX_GameObject *mesh_source = reinterpret_cast<KX_GameObject *>(command.runtime_tree);
        RAS_MeshObject *mesh = mesh_source->GetMesh(0);
        KX_Scene *scene = command.object->GetScene();
        if (mesh != nullptr && scene != nullptr) {
          scene->ReplaceMesh(command.object, mesh, true, false);
        }
        break;
      }
      case CommandType::CopyProperty: {
        if (command.object == nullptr || command.runtime_tree == nullptr) {
          break;
        }
        KX_GameObject *source = command.object;
        KX_GameObject *target = reinterpret_cast<KX_GameObject *>(command.runtime_tree);
        EXP_Value *prop = source->GetProperty(command.property_name);
        if (prop != nullptr) {
          target->SetProperty(command.property_name, prop->GetReplica());
        }
        break;
      }
      case CommandType::SetBonePoseLocation: {
        if (command.object == nullptr || command.property_name.empty()) {
          break;
        }
        blender::Object *ob = command.object->GetBlenderObject();
        if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr) {
          break;
        }
        blender::bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose,
                                                                command.property_name.c_str());
        if (pchan == nullptr) {
          break;
        }
        float location[3];
        if (!ResolveBonePoseLocation(command.object,
                                     pchan,
                                     command.vector_value,
                                     NormalizeBonePoseLocationSpace(command.int_value),
                                     command.secondary_int_value != 0,
                                     location))
        {
          break;
        }
        pchan->loc[0] = location[0];
        pchan->loc[1] = location[1];
        pchan->loc[2] = location[2];
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          if (blender::bContext *context = engine->GetContext()) {
            if (blender::Depsgraph *depsgraph = CTX_data_depsgraph_pointer(context)) {
              if (blender::Scene *scene = CTX_data_scene(context)) {
                BKE_pose_where_is(depsgraph, scene, ob);
              }
            }
          }
        }
        break;
      }
      case CommandType::SetBonePoseTransform: {
        if (command.object == nullptr || command.property_name.empty()) {
          break;
        }
        blender::Object *ob = command.object->GetBlenderObject();
        if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr) {
          break;
        }
        blender::bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose,
                                                                command.property_name.c_str());
        if (pchan == nullptr) {
          break;
        }
        SetPoseChannelTransform(command.object,
                                ob,
                                pchan,
                                command.vector_value,
                                command.secondary_vector_value,
                                NormalizeBonePoseLocationSpace(command.int_value),
                                command.secondary_int_value != 0);
        break;
      }
      case CommandType::SetBonePoseRotation: {
        if (command.object == nullptr || command.property_name.empty()) {
          break;
        }
        blender::Object *ob = command.object->GetBlenderObject();
        if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr) {
          break;
        }
        blender::bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose,
                                                                command.property_name.c_str());
        if (pchan == nullptr) {
          break;
        }
        float center[3] = {0.0f, 0.0f, 0.0f};
        const bool use_center = command.secondary_int_value != 0;
        if (use_center) {
          if (pchan->bone == nullptr) {
            break;
          }
          UpdateArmatureObjectPose(command.object);
          PoseChannelCenter(*pchan, center);
        }

        const LN_BonePoseRotationSpace rotation_space = NormalizeBonePoseRotationSpace(
            command.int_value);
        if (!use_center && rotation_space != LN_BonePoseRotationSpace::PoseChannel) {
          UpdateArmatureObjectPose(command.object);
        }
        if (!SetPoseChannelRotation(command.object,
                                    ob,
                                    pchan,
                                    command.vector_value,
                                    rotation_space))
        {
          break;
        }

        if (use_center && !SetPoseChannelCenter(command.object, pchan, center)) {
          break;
        }
        UpdateArmatureObjectPose(command.object);
        break;
      }
      case CommandType::SetBonePoseScale: {
        if (command.object == nullptr || command.property_name.empty()) {
          break;
        }
        blender::Object *ob = command.object->GetBlenderObject();
        if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr) {
          break;
        }
        blender::bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose,
                                                                command.property_name.c_str());
        if (pchan == nullptr) {
          break;
        }
        const MT_Vector3 channel_scale = ObjectAxesScaleToPoseChannelScale(*pchan,
                                                                           command.vector_value);
        pchan->scale[0] = float(channel_scale.x());
        pchan->scale[1] = float(channel_scale.y());
        pchan->scale[2] = float(channel_scale.z());
        UpdateArmatureObjectPose(command.object);
        break;
      }
      case CommandType::SetBoneAttribute: {
        blender::bPoseChannel *pchan = FindPoseChannel(command.object, command.property_name);
        if (pchan == nullptr) {
          break;
        }
        blender::Bone *bone = pchan->bone;
        if (bone == nullptr) {
          break;
        }
        bool changed = false;
        switch (command.int_value) {
          case 2: {
            static constexpr blender::eBone_InheritScaleMode scale_modes[] = {
                blender::BONE_INHERIT_SCALE_NONE,
                blender::BONE_INHERIT_SCALE_FULL,
                blender::BONE_INHERIT_SCALE_NONE_LEGACY,
                blender::BONE_INHERIT_SCALE_FIX_SHEAR,
                blender::BONE_INHERIT_SCALE_ALIGNED,
                blender::BONE_INHERIT_SCALE_AVERAGE,
            };
            const int32_t mode = std::clamp(command.secondary_int_value,
                                            0,
                                            int32_t(std::size(scale_modes)) - 1);
            bone->inherit_scale_mode = scale_modes[mode];
            changed = true;
            break;
          }
          case 13:
          case 15:
          case 16:
          case 17:
          case 18: {
            bool enabled = false;
            if (!LogicValueAsBool(command.property_value, enabled)) {
              break;
            }
            if (command.int_value == 13) {
              changed = SetBoneFlag(*bone, blender::BONE_HINGE, !enabled);
            }
            else if (command.int_value == 15) {
              changed = SetBoneFlag(*bone, blender::BONE_NO_DEFORM, !enabled);
            }
            else if (command.int_value == 16) {
              changed = SetBoneFlag(*bone, blender::BONE_NO_LOCAL_LOCATION, !enabled);
            }
            else if (command.int_value == 17) {
              changed = SetBoneFlag(*bone, blender::BONE_RELATIVE_PARENTING, enabled);
            }
            else {
              if (enabled) {
                bone->bbone_flag |= blender::BBONE_SCALE_EASING;
              }
              else {
                bone->bbone_flag &= ~blender::BBONE_SCALE_EASING;
              }
              changed = true;
            }
            break;
          }
          default:
            break;
        }
        if (changed) {
          UpdateArmatureObjectPose(command.object);
        }
        break;
      }
      case CommandType::SetBoneConstraintInfluence: {
        if (command.object == nullptr || command.property_name.empty() ||
            command.secondary_property_name.empty())
        {
          break;
        }
        blender::Object *ob = command.object->GetBlenderObject();
        if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr) {
          break;
        }
        blender::bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose,
                                                                command.property_name.c_str());
        if (pchan == nullptr) {
          break;
        }
        blender::bConstraint *constraint = nullptr;
        for (blender::bConstraint *con = static_cast<blender::bConstraint *>(pchan->constraints.first);
             con != nullptr;
             con = con->next)
        {
          if (STREQ(con->name, command.secondary_property_name.c_str())) {
            constraint = con;
            break;
          }
        }
        if (constraint == nullptr) {
          break;
        }
        constraint->enforce = std::clamp(command.scalar_value, 0.0f, 1.0f);
        UpdateArmatureObjectPose(command.object);
        break;
      }
      case CommandType::SetBoneConstraintTarget: {
        blender::bConstraint *constraint = FindPoseConstraint(
            command.object, command.property_name, command.secondary_property_name);
        if (constraint == nullptr || command.runtime_tree == nullptr) {
          break;
        }
        KX_GameObject *target_object = ResolveObjectForCommand(command.runtime_tree,
                                                               command.property_value);
        blender::Object *target = target_object ? target_object->GetBlenderObject() : nullptr;

        blender::ListBaseT<blender::bConstraintTarget> targets = {nullptr, nullptr};
        if (!BKE_constraint_targets_get(constraint, &targets)) {
          break;
        }
        for (blender::bConstraintTarget *ct = static_cast<blender::bConstraintTarget *>(
                 targets.first);
             ct != nullptr;
             ct = ct->next)
        {
          ct->tar = target;
        }
        BKE_constraint_targets_flush(constraint, &targets, false);
        UpdateArmatureObjectPose(command.object);
        break;
      }
      case CommandType::SetBoneConstraintAttribute: {
        blender::bConstraint *constraint = FindPoseConstraint(
            command.object, command.property_name, command.secondary_property_name);
        if (constraint == nullptr) {
          break;
        }
        const std::string &attribute = command.tertiary_property_name;
        if (attribute == "influence" || attribute == "enforce") {
          float influence = 0.0f;
          if (LogicValueAsFloat(command.property_value, influence)) {
            constraint->enforce = std::clamp(influence, 0.0f, 1.0f);
            UpdateArmatureObjectPose(command.object);
          }
          break;
        }
        blender::Object *ob = command.object ? command.object->GetBlenderObject() : nullptr;
        if (ob == nullptr) {
          break;
        }
        blender::bContext *ctx = nullptr;
        CurrentMain(&ctx);
        blender::PointerRNA target_ptr = blender::RNA_pointer_create_discrete(
            &ob->id, blender::RNA_Constraint, constraint);
        blender::PropertyRNA *prop = blender::RNA_struct_find_property(
            &target_ptr, attribute.c_str());
        if (SetRnaPropertyValue(ctx, command.runtime_tree, target_ptr, prop, command.property_value)) {
          blender::RNA_property_update(ctx, &target_ptr, prop);
          UpdateArmatureObjectPose(command.object);
        }
        break;
      }
      case CommandType::SetMaterialSlot: {
        if (command.object == nullptr || command.int_value < 0) {
          break;
        }
        blender::Object *ob = command.object->GetBlenderObject();
        if (ob == nullptr) {
          break;
        }
        blender::bContext *ctx = nullptr;
        blender::Main *bmain = CurrentMain(&ctx);
        if (bmain == nullptr) {
          break;
        }
        blender::Material *material = ResolveMaterialForCommand(
            bmain, command.runtime_tree, command.runtime_ref, command.property_name);
        if (material == nullptr) {
          break;
        }
        const short material_slot = short(command.int_value + 1);
        if (command.int_value < ob->totcol &&
            BKE_object_material_get(ob, material_slot) == material)
        {
          break;
        }
        BKE_object_material_assign(bmain,
                                   ob,
                                   material,
                                   material_slot,
                                   blender::BKE_MAT_ASSIGN_OBJECT);
        ApplyRuntimeMaterialSlot(command.object, material, command.int_value);
        if (KX_Scene *scene = command.object->GetScene()) {
          const bool overlay_only = (ob->gameflag & blender::OB_OVERLAY_COLLECTION) != 0;
          scene->AppendToIdsToUpdate(&ob->id, blender::ID_RECALC_GEOMETRY, overlay_only);
          scene->AppendToIdsToUpdate(&material->id, blender::ID_RECALC_SHADING, overlay_only);
        }
        break;
      }
      case CommandType::SetMaterialParameter: {
        apply_material_parameter_command(command);
        break;
      }
      case CommandType::SetMaterialNodeSocketValue: {
        apply_material_socket_value_command(command);
        break;
      }
      case CommandType::SetGeometryNodesInput: {
        apply_geometry_nodes_input_command(command);
        break;
      }
      case CommandType::SetGeometryNodeSocketValue: {
        apply_geometry_node_socket_value_command(command);
        break;
      }
      case CommandType::SetCompositorNodeSocketValue: {
        apply_compositor_node_socket_value_command(command);
        break;
      }
      case CommandType::MakeNodeTreeUnique: {
        apply_make_node_tree_unique_command(command);
        break;
      }
      case CommandType::SetNodeMute: {
        apply_set_node_mute_command(command);
        break;
      }
      case CommandType::EnableDisableModifier: {
        apply_enable_disable_modifier_command(command);
        break;
      }
    }
  };

  auto command_streams_sorted = [&]() {
    return m_commandStreamsSorted;
  };

  auto typed_stream_stats = [&]() {
    CommandStreamStats stats;
    stats.legacy_command_count = uint32_t(m_commands.size());
    stats.typed_transform_count = uint32_t(m_transformCommands.size());
    stats.typed_velocity_count = uint32_t(m_velocityCommands.size());
    stats.typed_relative_vector_count = uint32_t(m_relativeVectorCommands.size());
    stats.typed_motion_count = uint32_t(m_motionCommands.size());
    stats.typed_property_count = uint32_t(m_propertyCommands.size());
    stats.typed_event_count = uint32_t(m_eventCommands.size());
    stats.typed_audio_count = uint32_t(m_audioCommands.size());
    stats.typed_lifecycle_count = uint32_t(m_lifecycleCommands.size());
    stats.typed_object_service_count = uint32_t(m_objectServiceCommands.size());
    stats.typed_runtime_service_count = uint32_t(m_runtimeServiceCommands.size());
    stats.typed_animation_count = uint32_t(m_animationCommands.size());
    stats.typed_armature_count = uint32_t(m_armatureCommands.size());
    stats.typed_material_count = uint32_t(m_materialCommands.size());
    stats.typed_physics_count = uint32_t(m_physicsCommands.size());
    return stats;
  };

  auto non_empty_stream_count = [&]() {
    uint32_t count = 0;
    auto count_stream = [&count](const size_t size) {
      if (size != 0) {
        count++;
      }
    };
    count_stream(m_commands.size());
    count_stream(m_transformCommands.size());
    count_stream(m_velocityCommands.size());
    count_stream(m_relativeVectorCommands.size());
    count_stream(m_motionCommands.size());
    count_stream(m_propertyCommands.size());
    count_stream(m_eventCommands.size());
    count_stream(m_audioCommands.size());
    count_stream(m_lifecycleCommands.size());
    count_stream(m_objectServiceCommands.size());
    count_stream(m_runtimeServiceCommands.size());
    count_stream(m_animationCommands.size());
    count_stream(m_armatureCommands.size());
    count_stream(m_materialCommands.size());
    count_stream(m_physicsCommands.size());
    return count;
  };

  auto flush_typed_vector_command = [&](const TypedVectorCommand &typed_command) {
    KX_GameObject *object = typed_command.object;
    if (object_removed_during_flush(object)) {
      return;
    }
    if (object == nullptr) {
      Command command;
      command.type = typed_command.type;
      command.sort_key = typed_command.header.sort_key;
      command.record_sequence = typed_command.header.record_sequence;
      command.source_ref_index = typed_command.header.source_ref_index;
      warn_command_failure_once(command, "missing target object");
      return;
    }

    switch (typed_command.type) {
      case CommandType::SetWorldPosition:
        object->NodeSetWorldPosition(typed_command.value);
        object->NodeUpdateGS(0.0);
        break;
      case CommandType::SetLocalPosition:
        object->NodeSetLocalPosition(typed_command.value);
        object->NodeUpdateGS(0.0);
        break;
      case CommandType::SetWorldOrientation:
        object->NodeSetGlobalOrientation(MT_Matrix3x3(typed_command.value));
        object->NodeUpdateGS(0.0);
        break;
      case CommandType::SetLocalOrientation:
        object->NodeSetLocalOrientation(MT_Matrix3x3(typed_command.value));
        object->NodeUpdateGS(0.0);
        break;
      case CommandType::SetWorldScale:
        object->NodeSetWorldScale(typed_command.value);
        object->NodeUpdateGS(0.0);
        break;
      case CommandType::SetLocalScale:
        object->NodeSetLocalScale(typed_command.value);
        object->NodeUpdateGS(0.0);
        break;
      case CommandType::SetLinearVelocity:
        object->setLinearVelocity(typed_command.value, false);
        break;
      case CommandType::SetLocalLinearVelocity:
        object->setLinearVelocity(typed_command.value, true);
        break;
      case CommandType::SetAngularVelocity:
        object->setAngularVelocity(typed_command.value, false);
        break;
      case CommandType::SetLocalAngularVelocity:
        object->setAngularVelocity(typed_command.value, true);
        break;
      case CommandType::ApplyMovement:
        object->ApplyMovement(typed_command.value, typed_command.bool_value);
        break;
      case CommandType::ApplyRotation:
        object->ApplyRotation(typed_command.value, typed_command.bool_value);
        break;
      case CommandType::ApplyForce:
        object->ApplyForce(typed_command.value, typed_command.bool_value);
        break;
      case CommandType::ApplyTorque:
        object->ApplyTorque(typed_command.value, typed_command.bool_value);
        break;
      default: {
        Command command;
        command.type = typed_command.type;
        command.object = object;
        command.vector_value = typed_command.value;
        command.bool_value = typed_command.bool_value;
        command.sort_key = typed_command.header.sort_key;
        command.record_sequence = typed_command.header.record_sequence;
        command.source_ref_index = typed_command.header.source_ref_index;
        flush_command(command);
        break;
      }
    }
  };

  auto flush_typed_property_command = [&](const TypedPropertyCommand &typed_command) {
    auto make_warning_command = [&]() {
      Command command;
      command.type = typed_command.type;
      command.object = typed_command.object;
      command.runtime_tree = typed_command.runtime_tree;
      command.object_handle = typed_command.object_handle;
      command.game_property_id = typed_command.game_property_id;
      command.property_name_id = typed_command.property_name_id;
      command.property_name_ptr = typed_command.property_name_ptr;
      command.property_ref_index = typed_command.property_ref_index;
      command.property_name = typed_command.property_name;
      command.property_value = typed_command.value;
      command.sort_key = typed_command.header.sort_key;
      command.record_sequence = typed_command.header.record_sequence;
      command.source_ref_index = typed_command.header.source_ref_index;
      return command;
    };

    if (typed_command.type == CommandType::SetTreeProperty) {
      if (typed_command.runtime_tree != nullptr) {
        typed_command.runtime_tree->SetTreePropertyValue(typed_command.property_ref_index,
                                                         typed_command.value);
      }
      else {
        warn_command_failure_once(make_warning_command(),
                                  "missing runtime tree for tree-property write");
      }
      return;
    }

    KX_GameObject *object = typed_command.object;
    if (object_removed_during_flush(object)) {
      return;
    }
    if (object == nullptr) {
      warn_command_failure_once(make_warning_command(), "missing target object");
      return;
    }

    if (!typed_command.game_property_id.IsValid() && m_logicManager != nullptr &&
        !typed_command.property_name.empty())
    {
      m_logicManager->InternGamePropertyName(typed_command.property_name);
    }

    const std::string *property_name = nullptr;
    if (typed_command.property_name_ptr != nullptr && !typed_command.property_name_ptr->empty()) {
      property_name = typed_command.property_name_ptr;
    }
    if (property_name == nullptr && typed_command.game_property_id.IsValid() &&
        m_logicManager != nullptr)
    {
      const std::string &value = m_logicManager->DebugName(typed_command.game_property_id);
      if (!value.empty()) {
        property_name = &value;
      }
    }
    if (property_name == nullptr && typed_command.runtime_tree != nullptr &&
        typed_command.property_ref_index != LN_INVALID_INDEX)
    {
      const std::shared_ptr<const LN_Program> program = typed_command.runtime_tree->GetProgram();
      if (program != nullptr) {
        const std::vector<LN_GamePropertyRef> &property_refs = program->GetGamePropertyRefs();
        if (typed_command.property_ref_index < property_refs.size()) {
          property_name = &property_refs[typed_command.property_ref_index].name;
        }
      }
    }
    if (property_name == nullptr && !typed_command.property_name.empty()) {
      property_name = &typed_command.property_name;
    }
    if (property_name == nullptr || property_name->empty()) {
      return;
    }

    if (!apply_game_property_value(object, *property_name, typed_command.value)) {
      warn_command_failure_once(make_warning_command(), "unsupported game-property payload type");
    }
  };

  auto flush_typed_event_command = [&](const TypedEventCommand &typed_command) {
    if (m_logicManager == nullptr) {
      return;
    }

    LN_EventSubjectId event_subject_id = typed_command.event_subject_id;
    if (!event_subject_id.IsValid() && !typed_command.dynamic_subject.empty()) {
      event_subject_id = m_logicManager->InternEventSubject(typed_command.dynamic_subject);
    }

    const std::string *subject = nullptr;
    if (event_subject_id.IsValid()) {
      const std::string &value = m_logicManager->DebugName(event_subject_id);
      if (!value.empty()) {
        subject = &value;
      }
    }
    if (subject == nullptr && typed_command.subject_name_id.IsValid() &&
        typed_command.runtime_tree != nullptr)
    {
      const std::shared_ptr<const LN_Program> program = typed_command.runtime_tree->GetProgram();
      if (program != nullptr) {
        const std::string &value = program->GetString(typed_command.subject_name_id);
        if (!value.empty()) {
          subject = &value;
        }
      }
    }
    if (subject == nullptr && !typed_command.dynamic_subject.empty()) {
      subject = &typed_command.dynamic_subject;
    }
    if (subject == nullptr || subject->empty()) {
      return;
    }

    LN_EventEntry event;
    event.subject_id = event_subject_id;
    event.messenger_handle = typed_command.messenger_handle.IsValid() ?
                                 typed_command.messenger_handle :
                                 m_logicManager->MakeObjectHandle(
                                     typed_command.messenger,
                                     typed_command.messenger ? typed_command.messenger->GetName() :
                                                               std::string());
    event.target_handle = typed_command.target_handle.IsValid() ?
                              typed_command.target_handle :
                              m_logicManager->MakeObjectHandle(
                                  typed_command.target,
                                  typed_command.target ? typed_command.target->GetName() :
                                                         std::string());
    event.subject = *subject;
    event.content = typed_command.content;
    event.messenger = typed_command.messenger;
    event.target = typed_command.target;
    m_logicManager->PushEvent(std::move(event));
  };

  auto flush_typed_audio_command = [&](const TypedAudioCommand &typed_command) {
    std::string sound_name = typed_command.sound_name;
    if (sound_name.empty() && typed_command.sound_id.IsValid() && m_logicManager != nullptr) {
      sound_name = m_logicManager->DebugName(typed_command.sound_id);
    }

    switch (typed_command.type) {
      case CommandType::PlaySound:
      case CommandType::PlaySound3D: {
#ifdef WITH_AUDASPACE
        if (sound_name.empty()) {
          break;
        }
        KX_KetsjiEngine *engine = KX_GetActiveEngine();
        blender::bContext *ctx = engine ? engine->GetContext() : nullptr;
        blender::Main *bmain = ctx ? CTX_data_main(ctx) : nullptr;
        if (bmain == nullptr) {
          bmain = G_MAIN;
        }
        if (bmain == nullptr) {
          break;
        }
        auto device = aud::DeviceManager::getDevice();
        if (!device) {
          break;
        }
        try {
          AUD_Sound snd = logic_native_sound_resolve(bmain, sound_name);
          if (!snd) {
            break;
          }
          logic_native_sound_handles_stop_by_name(sound_name);
          AUD_Handle handle = device->play(snd, false);
          if (handle) {
            auto h3d = std::dynamic_pointer_cast<aud::I3DHandle>(handle);
            if (h3d) {
              h3d->setRelative(true);
              if (typed_command.type == CommandType::PlaySound3D) {
                logic_native_sound_apply_3d_from_speaker(h3d, typed_command.speaker);
              }
            }
            handle->setVolume(std::max(typed_command.volume, 0.0f));
            handle->setPitch(std::max(typed_command.pitch, 0.0001f));
            if (typed_command.loop) {
              handle->setLoopCount(-1);
            }
            logic_native_sound_handles_register(sound_name, std::move(handle));
          }
        }
        catch (const aud::Exception &) {
        }
#endif
        break;
      }
      case CommandType::StopSound:
#ifdef WITH_AUDASPACE
        if (!sound_name.empty()) {
          logic_native_sound_handles_stop_by_name(sound_name);
        }
#endif
        break;
      case CommandType::PauseSound:
#ifdef WITH_AUDASPACE
        if (!sound_name.empty()) {
          logic_native_sound_handles_pause_by_name(sound_name);
        }
#endif
        break;
      case CommandType::ResumeSound:
#ifdef WITH_AUDASPACE
        if (!sound_name.empty()) {
          logic_native_sound_handles_resume_by_name(sound_name);
        }
#endif
        break;
      case CommandType::StopAllSounds:
#ifdef WITH_AUDASPACE
        logic_native_sound_handles_stop_all();
        g_logic_native_sound_cache.clear();
        if (auto device = aud::DeviceManager::getDevice()) {
          device->stopAll();
        }
#endif
        break;
      default: {
        Command command;
        command.type = typed_command.type;
        command.object = typed_command.speaker;
        command.property_name = sound_name;
        command.vector_value = MT_Vector3(typed_command.volume, typed_command.pitch, 0.0f);
        command.bool_value = typed_command.loop;
        command.sort_key = typed_command.header.sort_key;
        command.record_sequence = typed_command.header.record_sequence;
        command.source_ref_index = typed_command.header.source_ref_index;
        flush_command(command);
        break;
      }
    }
  };

  auto make_lifecycle_command = [](const TypedLifecycleCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.runtime_tree = typed_command.runtime_tree;
    command.runtime_ref = typed_command.runtime_ref;
    command.property_name = typed_command.name;
    command.scalar_value = typed_command.scalar_value;
    command.property_ref_index = typed_command.property_ref_index;
    command.bool_value = typed_command.bool_value;
    command.secondary_bool_value = typed_command.secondary_bool_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    return command;
  };

  auto make_object_service_command = [](const TypedObjectServiceCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.vector_value = typed_command.vector_value;
    command.color_value = typed_command.color_value;
    command.scalar_value = typed_command.scalar_value;
    command.bool_value = typed_command.bool_value;
    command.secondary_bool_value = typed_command.secondary_bool_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    return command;
  };

  auto flush_typed_object_service_command = [&](const TypedObjectServiceCommand &typed_command) {
    KX_GameObject *object = typed_command.object;
    if (object_removed_during_flush(object)) {
      return;
    }
    if (object == nullptr) {
      warn_command_failure_once(make_object_service_command(typed_command),
                                "missing target object");
      return;
    }

    switch (typed_command.type) {
      case CommandType::SetVisibility:
        object->SetVisible(typed_command.bool_value, typed_command.secondary_bool_value);
        break;
      case CommandType::SetObjectColor:
        object->SetObjectColor(typed_command.color_value);
        break;
      case CommandType::MakeLightUnique:
        EnsureLightDataUnique(object);
        break;
      case CommandType::SetLightColor:
        if (KX_LightObject *light_object = dynamic_cast<KX_LightObject *>(object)) {
          if (blender::Light *light = light_object->GetLight()) {
            light->r = typed_command.color_value.x();
            light->g = typed_command.color_value.y();
            light->b = typed_command.color_value.z();
            NotifyLightUpdate(light);
          }
        }
        break;
      case CommandType::SetLightPower:
        if (KX_LightObject *light_object = dynamic_cast<KX_LightObject *>(object)) {
          if (blender::Light *light = light_object->GetLight()) {
            light->energy = std::max(typed_command.scalar_value, 0.0f);
            NotifyLightUpdate(light);
          }
        }
        break;
      case CommandType::SetLightShadow:
        if (KX_LightObject *light_object = dynamic_cast<KX_LightObject *>(object)) {
          if (blender::Light *light = light_object->GetLight()) {
            if (typed_command.bool_value) {
              light->mode |= blender::LA_SHADOW;
            }
            else {
              light->mode &= ~blender::LA_SHADOW;
            }
            NotifyLightUpdate(light);
          }
        }
        break;
      case CommandType::SetGravity:
        if (KX_Scene *scene = object->GetScene()) {
          scene->SetGravity(typed_command.vector_value);
        }
        break;
      case CommandType::SetTimeScale:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          engine->SetTimeScale(typed_command.scalar_value);
        }
        break;
      case CommandType::SetActiveCamera:
        if (KX_Camera *camera = dynamic_cast<KX_Camera *>(object)) {
          if (KX_Scene *scene = camera->GetScene()) {
            scene->SetActiveCamera(camera);
          }
        }
        break;
      case CommandType::SetCameraFov:
        if (KX_Camera *camera = dynamic_cast<KX_Camera *>(object)) {
          constexpr float radians_per_degree = 0.01745329251994329577f;
          const float fov_radians = std::max(typed_command.scalar_value, 0.001f) *
                                    radians_per_degree;
          RAS_CameraData *camera_data = camera->GetCameraData();
          const float width = camera->GetSensorWidth();
          camera_data->m_lens = width / (2.0f * std::tan(0.5f * fov_radians));
          camera->InvalidateProjectionMatrix();
        }
        break;
      case CommandType::SetCameraOrthoScale:
        if (KX_Camera *camera = dynamic_cast<KX_Camera *>(object)) {
          camera->GetCameraData()->m_scale = std::max(typed_command.scalar_value, 0.001f);
          camera->InvalidateProjectionMatrix();
        }
        break;
      default:
        flush_command(make_object_service_command(typed_command));
        break;
    }
  };

  auto make_runtime_service_command = [](const TypedRuntimeServiceCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.int_value = typed_command.int_value;
    command.secondary_int_value = typed_command.secondary_int_value;
    command.property_ref_index = typed_command.uint_value;
    command.scalar_value = typed_command.scalar_value;
    command.vector_value = MT_Vector3(typed_command.secondary_scalar_value, 0.0f, 0.0f);
    command.bool_value = typed_command.bool_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    return command;
  };

  auto flush_typed_runtime_service_command = [&](const TypedRuntimeServiceCommand &typed_command) {
    switch (typed_command.type) {
      case CommandType::SetCursorVisibility:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          if (RAS_ICanvas *canvas = engine->GetCanvas()) {
            canvas->SetMouseState(typed_command.bool_value ? RAS_ICanvas::MOUSE_NORMAL :
                                                               RAS_ICanvas::MOUSE_INVISIBLE);
          }
        }
        break;
      case CommandType::SetCursorPosition:
        logic_node_set_cursor_position(KX_GetActiveEngine(),
                                       typed_command.int_value,
                                       typed_command.secondary_int_value);
        break;
      case CommandType::SetGamepadVibration:
        if (typed_command.int_value >= 0 && typed_command.int_value < JOYINDEX_MAX) {
          if (DEV_Joystick *joystick = DEV_Joystick::GetInstance(
                  short(typed_command.int_value)))
          {
            joystick->RumblePlay(typed_command.scalar_value,
                                 typed_command.secondary_scalar_value,
                                 typed_command.uint_value);
          }
        }
        break;
      case CommandType::SetWindowSize:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          if (RAS_ICanvas *canvas = engine->GetCanvas()) {
            canvas->ResizeWindow(typed_command.int_value,
                                 typed_command.secondary_int_value);
          }
        }
        break;
      case CommandType::SetFullscreen:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          if (RAS_ICanvas *canvas = engine->GetCanvas()) {
            canvas->SetFullScreen(typed_command.bool_value);
          }
        }
        break;
      case CommandType::SetVSync:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          if (RAS_ICanvas *canvas = engine->GetCanvas()) {
            canvas->SetSwapInterval(CanvasSwapIntervalFromVSyncMode(typed_command.int_value));
          }
        }
        break;
      case CommandType::SetShowFramerate:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          engine->SetFlag(KX_KetsjiEngine::SHOW_FRAMERATE, typed_command.bool_value);
        }
        break;
      case CommandType::SetShowProfile:
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          engine->SetFlag(KX_KetsjiEngine::SHOW_PROFILE, typed_command.bool_value);
        }
        break;
      default:
        flush_command(make_runtime_service_command(typed_command));
        break;
    }
  };

  auto make_animation_command = [](const TypedAnimationCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.property_name = typed_command.action_name;
    command.vector_value = MT_Vector3(typed_command.start_frame,
                                      typed_command.end_frame,
                                      typed_command.blendin);
    command.secondary_vector_value = MT_Vector3(typed_command.layer_weight,
                                                typed_command.playback_speed,
                                                0.0f);
    command.scalar_value = typed_command.frame;
    command.int_value = typed_command.layer;
    command.secondary_int_value = typed_command.priority;
    command.animation_flags = typed_command.animation_flags;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    return command;
  };

  auto flush_typed_animation_command = [&](const TypedAnimationCommand &typed_command) {
    KX_GameObject *object = typed_command.object;
    if (object_removed_during_flush(object)) {
      return;
    }
    if (object == nullptr) {
      warn_command_failure_once(make_animation_command(typed_command), "missing target object");
      return;
    }

    switch (typed_command.type) {
      case CommandType::PlayAction: {
        if (typed_command.action_name.empty()) {
          break;
        }
        const int play_mode = int(typed_command.animation_flags & 0xFFu);
        const int blend_mode = int((typed_command.animation_flags >> 8u) & 0xFFu);
        const int ipo_flags = int((typed_command.animation_flags >> 16u) & 0xFFFFu);
        object->PlayAction(typed_command.action_name,
                           typed_command.start_frame,
                           typed_command.end_frame,
                           short(typed_command.layer),
                           short(typed_command.priority),
                           typed_command.blendin,
                           short(play_mode),
                           typed_command.layer_weight,
                           short(ipo_flags),
                           typed_command.playback_speed,
                           short(blend_mode));
        break;
      }
      case CommandType::StopAction:
        object->StopAction(short(typed_command.layer));
        break;
      case CommandType::SetActionFrame:
        object->SetActionFrame(short(typed_command.layer), typed_command.frame);
        break;
      default:
        flush_command(make_animation_command(typed_command));
        break;
    }
  };

  auto make_material_command = [](const TypedMaterialCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.runtime_tree = typed_command.runtime_tree;
    command.runtime_ref = typed_command.runtime_ref;
    command.property_name = typed_command.material_name;
    command.secondary_property_name = typed_command.node_name;
    command.tertiary_property_name = typed_command.internal_name;
    command.quaternary_property_name = typed_command.attribute_name;
    command.property_value = typed_command.value;
    command.scalar_value = typed_command.scalar_value;
    command.int_value = typed_command.int_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    return command;
  };

  auto make_armature_command = [](const TypedArmatureCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.runtime_tree = typed_command.runtime_tree;
    command.property_name = typed_command.bone_name;
    command.secondary_property_name = typed_command.constraint_name;
    command.tertiary_property_name = typed_command.attribute_name;
    command.vector_value = typed_command.vector_value;
    command.secondary_vector_value = typed_command.secondary_vector_value;
    command.property_value = typed_command.value;
    command.scalar_value = typed_command.scalar_value;
    command.int_value = typed_command.int_value;
    command.secondary_int_value = typed_command.secondary_int_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    return command;
  };

  auto flush_typed_armature_command = [&](const TypedArmatureCommand &typed_command) {
    KX_GameObject *object = typed_command.object;
    if (object_removed_during_flush(object)) {
      return;
    }
    flush_command(make_armature_command(typed_command));
  };

  auto flush_typed_material_command = [&](const TypedMaterialCommand &typed_command) {
    KX_GameObject *object = typed_command.object;
    if (object_removed_during_flush(object)) {
      return;
    }
    flush_command(make_material_command(typed_command));
  };

  auto make_physics_command = [](const TypedPhysicsCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.vector_value = typed_command.vector_value;
    command.secondary_vector_value = typed_command.secondary_vector_value;
    command.scalar_value = typed_command.scalar_value;
    command.int_value = typed_command.int_value;
    command.secondary_int_value = typed_command.secondary_int_value;
    command.bool_value = typed_command.bool_value;
    command.secondary_bool_value = typed_command.secondary_bool_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    return command;
  };

  auto flush_typed_physics_command = [&](const TypedPhysicsCommand &typed_command) {
    KX_GameObject *object = typed_command.object;
    if (object_removed_during_flush(object)) {
      return;
    }
    if (object == nullptr) {
      warn_command_failure_once(make_physics_command(typed_command), "missing target object");
      return;
    }

    switch (typed_command.type) {
      case CommandType::ApplyImpulse:
        if (PHY_IPhysicsController *physics_controller = object->GetPhysicsController()) {
          physics_controller->ApplyImpulse(typed_command.secondary_vector_value,
                                           typed_command.vector_value,
                                           false);
        }
        break;
      case CommandType::SetCollisionGroup:
        object->SetCollisionGroup(static_cast<unsigned short>(typed_command.int_value));
        break;
      case CommandType::SetPhysics:
        if (typed_command.bool_value) {
          object->RestorePhysics(false);
        }
        else {
          object->SuspendPhysics(typed_command.secondary_bool_value, false);
        }
        break;
      case CommandType::SetDynamics:
        if (PHY_IPhysicsController *physics_controller = object->GetPhysicsController()) {
          if (typed_command.int_value != int(PHY_DynamicsMode::Dynamic) || !object->GetParent())
          {
            physics_controller->SetDynamicsMode(
                static_cast<PHY_DynamicsMode>(typed_command.int_value),
                typed_command.bool_value);
          }
        }
        break;
      case CommandType::RebuildCollisionShape:
        if (PHY_IPhysicsController *physics_controller = object->GetPhysicsController()) {
          physics_controller->ReinstancePhysicsShape(object, object->GetMesh(0), false, true);
        }
        break;
      case CommandType::SetRigidBodyAttribute:
        if (PHY_IPhysicsController *physics_controller = object->GetPhysicsController()) {
          apply_rigid_body_attribute(
              *physics_controller,
              static_cast<LN_RigidBodyAttribute>(typed_command.int_value),
              typed_command.vector_value,
              typed_command.secondary_vector_value,
              typed_command.scalar_value,
              typed_command.bool_value,
              typed_command.secondary_bool_value);
        }
        break;
      case CommandType::CharacterJump:
        if (KX_Scene *scene = object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(object)) {
              character->Jump();
            }
          }
        }
        break;
      case CommandType::SetCharacterGravity:
        if (KX_Scene *scene = object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(object)) {
              character->SetGravity(typed_command.vector_value);
            }
          }
        }
        break;
      case CommandType::SetCharacterJumpSpeed:
        if (KX_Scene *scene = object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(object)) {
              character->SetJumpSpeed(std::max(typed_command.scalar_value, 0.0f));
            }
          }
        }
        break;
      case CommandType::SetCharacterMaxJumps:
        if (KX_Scene *scene = object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(object)) {
              character->SetMaxJumps(std::max(typed_command.int_value, 0));
            }
          }
        }
        break;
      case CommandType::SetCharacterWalkDirection:
        if (KX_Scene *scene = object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(object)) {
              MT_Vector3 walk_direction = typed_command.vector_value;
              if (typed_command.bool_value) {
                walk_direction = object->NodeGetWorldOrientation() * walk_direction;
              }
              character->SetWalkDirection(walk_direction);
            }
          }
        }
        break;
      case CommandType::SetCharacterVelocity:
        if (KX_Scene *scene = object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_ICharacter *character = environment->GetCharacterController(object)) {
              character->SetVelocity(typed_command.vector_value,
                                     std::max(typed_command.scalar_value, 0.0f),
                                     typed_command.bool_value);
            }
          }
        }
        break;
      case CommandType::VehicleControl:
        if (KX_Scene *scene = object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_IVehicle *vehicle = environment->GetVehicleConstraint(
                    object->GetPhysicsController()))
            {
              ApplyVehicleActuatorControl(object,
                                          vehicle,
                                          typed_command.vector_value,
                                          typed_command.scalar_value);
            }
          }
        }
        break;
      case CommandType::VehicleApplyEngineForce:
      case CommandType::VehicleApplyBraking:
      case CommandType::VehicleApplySteering:
      case CommandType::SetVehicleSuspensionCompression:
      case CommandType::SetVehicleSuspensionStiffness:
      case CommandType::SetVehicleSuspensionDamping:
      case CommandType::SetVehicleWheelFriction:
        if (KX_Scene *scene = object->GetScene()) {
          if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
            if (PHY_IVehicle *vehicle = environment->GetVehicleConstraint(
                    object->GetPhysicsController()))
            {
              Object *chassis_object = object->GetBlenderObject();
              const GameVehicleSettings *chassis_settings = chassis_object ?
                                                                 chassis_object->vehicle :
                                                                 nullptr;
              const auto selected_wheels = SelectVehicleWheels(
                  object,
                  vehicle,
                  VehicleAxisFromInt(typed_command.secondary_int_value),
                  typed_command.int_value);
              for (const SelectedVehicleWheel &wheel : selected_wheels) {
                switch (typed_command.type) {
                  case CommandType::VehicleApplyEngineForce: {
                    const float configured_force = ConfiguredWheelEngineForce(chassis_settings,
                                                                             wheel.settings);
                    const float applied_force = configured_force > 1.0e-4f ?
                                                    typed_command.scalar_value *
                                                        configured_force :
                                                    typed_command.scalar_value;
                    vehicle->ApplyEngineForce(applied_force, wheel.index);
                    break;
                  }
                  case CommandType::VehicleApplyBraking: {
                    const float configured_brake = ConfiguredWheelBrakeTorque(chassis_settings,
                                                                              wheel.settings);
                    const float applied_brake = configured_brake > 1.0e-4f ?
                                                    std::max(typed_command.scalar_value, 0.0f) *
                                                        configured_brake :
                                                    std::max(typed_command.scalar_value, 0.0f);
                    vehicle->ApplyBraking(applied_brake, wheel.index);
                    break;
                  }
                  case CommandType::VehicleApplySteering: {
                    const float configured_steering = ConfiguredWheelSteeringAngle(wheel.settings);
                    if (configured_steering > 1.0e-4f) {
                      vehicle->SetSteeringValue(typed_command.scalar_value * configured_steering,
                                                wheel.index);
                    }
                    break;
                  }
                  case CommandType::SetVehicleSuspensionCompression:
                    vehicle->SetSuspensionCompression(std::max(typed_command.scalar_value, 0.0f),
                                                      wheel.index);
                    break;
                  case CommandType::SetVehicleSuspensionStiffness:
                    vehicle->SetSuspensionStiffness(std::max(typed_command.scalar_value, 0.0f),
                                                    wheel.index);
                    break;
                  case CommandType::SetVehicleSuspensionDamping:
                    vehicle->SetSuspensionDamping(std::max(typed_command.scalar_value, 0.0f),
                                                  wheel.index);
                    break;
                  case CommandType::SetVehicleWheelFriction:
                    vehicle->SetWheelFriction(std::max(typed_command.scalar_value, 0.0f),
                                              wheel.index);
                    break;
                  default:
                    break;
                }
              }
            }
          }
        }
        break;
      default:
        flush_command(make_physics_command(typed_command));
        break;
    }
  };

  auto make_motion_command = [](const TypedMotionCommand &typed_command) {
    Command command;
    command.type = typed_command.type;
    command.object = typed_command.object;
    command.runtime_tree = reinterpret_cast<LN_RuntimeTree *>(typed_command.target_object);
    command.vector_value = typed_command.vector_value;
    command.secondary_vector_value = typed_command.secondary_vector_value;
    command.scalar_value = typed_command.scalar_value;
    command.int_value = typed_command.int_value;
    command.secondary_int_value = typed_command.secondary_int_value;
    command.bool_value = typed_command.bool_value;
    command.secondary_bool_value = typed_command.secondary_bool_value;
    command.sort_key = typed_command.header.sort_key;
    command.record_sequence = typed_command.header.record_sequence;
    command.source_ref_index = typed_command.header.source_ref_index;
    return command;
  };

  auto flush_typed_motion_command = [&](const TypedMotionCommand &typed_command) {
    KX_GameObject *object = typed_command.object;
    if (object_removed_during_flush(object)) {
      return;
    }
    if (object == nullptr) {
      warn_command_failure_once(make_motion_command(typed_command), "missing target object");
      return;
    }

    switch (typed_command.type) {
      case CommandType::MoveToward: {
        const float dt = std::max(typed_command.secondary_vector_value.y(), 0.0f);
        const MT_Vector3 current = object->NodeGetWorldPosition();
        const MT_Vector3 target = typed_command.vector_value;
        const MT_Vector3 direction = target - current;
        const float distance = direction.length();
        const float stop_distance = typed_command.secondary_vector_value.x();
        if (distance <= stop_distance) {
          break;
        }
        const float speed = typed_command.scalar_value;
        if (typed_command.bool_value) {
          MT_Vector3 velocity_direction = direction;
          velocity_direction.z() = 0.0f;
          if (velocity_direction.length2() <= MT_EPSILON * MT_EPSILON) {
            break;
          }
          velocity_direction.normalize();
          MT_Vector3 velocity = velocity_direction * speed;
          velocity.z() = object->GetLinearVelocity(false).z();
          object->setLinearVelocity(velocity, false);
          break;
        }
        const float step = std::min(speed * (typed_command.secondary_bool_value ? dt : 1.0f),
                                    distance - stop_distance);
        const MT_Vector3 movement = direction.normalized() * step;
        object->ApplyMovement(movement, false);
        break;
      }
      case CommandType::SlowFollow: {
        KX_GameObject *follow_target = typed_command.target_object;
        if (follow_target == nullptr || object_removed_during_flush(follow_target)) {
          warn_command_failure_once(make_motion_command(typed_command),
                                    "missing follow target object");
          break;
        }
        const MT_Vector3 current = object->NodeGetWorldPosition();
        const MT_Vector3 target_pos = follow_target->NodeGetWorldPosition();
        const float factor = std::clamp(typed_command.scalar_value, 0.0f, 1.0f);
        const MT_Vector3 new_pos = current.lerp(target_pos, factor);
        object->NodeSetWorldPosition(new_pos);
        break;
      }
      case CommandType::AlignAxisToVector: {
        const MT_Vector3 target_vector = typed_command.vector_value.normalized();
        const int32_t axis = typed_command.int_value;
        const float factor = std::clamp(typed_command.scalar_value, 0.0f, 1.0f);
        MT_Vector3 local_axis;
        switch (axis) {
          case 0:
            local_axis = MT_Vector3(1.0f, 0.0f, 0.0f);
            break;
          case 1:
            local_axis = MT_Vector3(0.0f, 1.0f, 0.0f);
            break;
          default:
            local_axis = MT_Vector3(0.0f, 0.0f, 1.0f);
            break;
        }
        const MT_Vector3 cross = local_axis.cross(target_vector);
        const float dot = local_axis.dot(target_vector);
        if (cross.length2() < 1e-10f) {
          if (dot < 0.0f) {
            MT_Vector3 perp = (fabs(local_axis.x()) < 0.9f) ?
                                  MT_Vector3(1.0f, 0.0f, 0.0f) :
                                  MT_Vector3(0.0f, 1.0f, 0.0f);
            MT_Vector3 rot_axis = local_axis.cross(perp).normalized();
            MT_Quaternion q(rot_axis, MT_Scalar(M_PI * factor));
            object->NodeSetLocalOrientation(MT_Matrix3x3(q) * object->NodeGetLocalOrientation());
          }
          break;
        }
        MT_Vector3 rot_axis = cross.normalized();
        const float angle = acos(std::clamp(dot, -1.0f, 1.0f)) * factor;
        MT_Quaternion q(rot_axis, MT_Scalar(angle));
        object->NodeSetLocalOrientation(MT_Matrix3x3(q) * object->NodeGetLocalOrientation());
        break;
      }
      case CommandType::RotateToward: {
        const MT_Vector3 position = object->NodeGetWorldPosition();
        MT_Vector3 to_target = typed_command.vector_value - position;
        if (to_target.length2() < 1e-10f) {
          break;
        }
        to_target.normalize();

        const int32_t rot_axis_index = std::clamp(typed_command.int_value, 0, 2);
        const int32_t front_axis_index = std::clamp(typed_command.secondary_int_value, 0, 5);
        const float factor = std::clamp(typed_command.scalar_value, 0.0f, 1.0f);

        const MT_Matrix3x3 orientation = object->NodeGetWorldOrientation();
        MT_Vector3 local_rot_axis(0.0f, 0.0f, 0.0f);
        local_rot_axis[rot_axis_index] = 1.0f;
        const MT_Vector3 rot_axis_world = (orientation * local_rot_axis).normalized();

        const int front_axis = front_axis_index % 3;
        const bool front_negative = front_axis_index >= 3;
        MT_Vector3 local_front_axis(0.0f, 0.0f, 0.0f);
        local_front_axis[front_axis] = front_negative ? -1.0f : 1.0f;
        const MT_Vector3 front_world = orientation * local_front_axis;

        auto project_on_plane = [](const MT_Vector3 &vector, const MT_Vector3 &normal) {
          return vector - normal * vector.dot(normal);
        };

        MT_Vector3 desired_dir = project_on_plane(to_target, rot_axis_world);
        MT_Vector3 front_dir = project_on_plane(front_world, rot_axis_world);
        if (desired_dir.length2() < 1e-10f || front_dir.length2() < 1e-10f) {
          break;
        }
        desired_dir.normalize();
        front_dir.normalize();

        const MT_Vector3 cross = front_dir.cross(desired_dir);
        const float dot = front_dir.dot(desired_dir);
        if (cross.length2() < 1e-10f) {
          if (dot < 0.0f) {
            MT_Vector3 perp = (fabs(rot_axis_world.x()) < 0.9f) ?
                                  MT_Vector3(1.0f, 0.0f, 0.0f) :
                                  MT_Vector3(0.0f, 1.0f, 0.0f);
            MT_Vector3 flip_axis = rot_axis_world.cross(perp).normalized();
            MT_Quaternion q(flip_axis, MT_Scalar(M_PI * factor));
            object->NodeSetLocalOrientation(MT_Matrix3x3(q) *
                                            object->NodeGetLocalOrientation());
          }
          break;
        }

        float angle = acos(std::clamp(dot, -1.0f, 1.0f));
        if (cross.dot(rot_axis_world) < 0.0f) {
          angle = -angle;
        }
        angle *= factor;
        MT_Quaternion q(rot_axis_world, MT_Scalar(angle));
        object->NodeSetLocalOrientation(MT_Matrix3x3(q) * object->NodeGetLocalOrientation());
        break;
      }
      default:
        flush_command(make_motion_command(typed_command));
        break;
    }
  };

  auto flush_typed_lifecycle_command = [&](const TypedLifecycleCommand &typed_command) {
    KX_GameObject *object = typed_command.object;
    if (object_removed_during_flush(object)) {
      return;
    }
    if (object == nullptr) {
      warn_command_failure_once(make_lifecycle_command(typed_command), "missing target object");
      return;
    }

    switch (typed_command.type) {
      case CommandType::AddObject: {
        KX_Scene *scene = object->GetScene();
        if (scene == nullptr) {
          break;
        }
        const AddObjectSourceLookup &source_lookup = lookup_add_object_source(
            scene, typed_command.name);
        KX_GameObject *source = source_lookup.inactive_source;
        if (source == nullptr) {
          if (source_lookup.active_source != nullptr) {
            warn_add_object_source_not_inactive(make_lifecycle_command(typed_command),
                                               scene,
                                               source_lookup.active_source,
                                               typed_command.name);
          }
          break;
        }

        const float scene_lifespan = add_object_life_seconds_to_scene_lifespan(
            typed_command.scalar_value, typed_command.bool_value);
        KX_GameObject *replica = typed_command.bool_value ?
                                     scene->AddFullCopyObject(source, object, scene_lifespan) :
                                     scene->AddReplicaObject(source, object, scene_lifespan);
        if (replica != nullptr && !typed_command.bool_value) {
          replica->Release();
        }
        break;
      }
      case CommandType::AddObjectFromRef: {
        LN_Value added_object;
        Command command = make_lifecycle_command(typed_command);
        ExecuteAddObjectFromRefCommand(command, added_object);
        if (typed_command.runtime_tree != nullptr &&
            typed_command.property_ref_index != LN_INVALID_INDEX)
        {
          typed_command.runtime_tree->SetTreePropertyValue(typed_command.property_ref_index,
                                                           added_object);
        }
        break;
      }
      case CommandType::SetParent: {
        KX_Scene *scene = object->GetScene();
        if (scene == nullptr || scene->GetObjectList() == nullptr) {
          break;
        }
        KX_GameObject *parent = lookup_active_object_by_name(scene, typed_command.name);
        if (parent != nullptr && parent != object) {
          object->SetParent(parent, typed_command.bool_value, typed_command.secondary_bool_value);
        }
        break;
      }
      case CommandType::SetParentFromRef: {
        if (typed_command.runtime_tree == nullptr) {
          break;
        }
        KX_GameObject *parent = typed_command.runtime_tree->ResolveObjectRef(
            typed_command.runtime_ref);
        if (parent != nullptr && parent != object) {
          object->SetParent(parent, typed_command.bool_value, typed_command.secondary_bool_value);
        }
        break;
      }
      case CommandType::RemoveParent:
        object->RemoveParent();
        break;
      case CommandType::RemoveObject:
        if (object->GetScene() != nullptr) {
          object->GetScene()->DelayedRemoveObject(object);
        }
        break;
      case CommandType::ReplaceMesh: {
        if (typed_command.runtime_tree == nullptr) {
          break;
        }
        KX_GameObject *mesh_source = reinterpret_cast<KX_GameObject *>(
            typed_command.runtime_tree);
        RAS_MeshObject *mesh = mesh_source->GetMesh(0);
        KX_Scene *scene = object->GetScene();
        if (mesh != nullptr && scene != nullptr) {
          scene->ReplaceMesh(object, mesh, true, false);
        }
        break;
      }
      case CommandType::CopyProperty: {
        if (typed_command.runtime_tree == nullptr || typed_command.name.empty()) {
          break;
        }
        KX_GameObject *target = reinterpret_cast<KX_GameObject *>(typed_command.runtime_tree);
        EXP_Value *prop = object->GetProperty(typed_command.name);
        if (prop != nullptr) {
          target->SetProperty(typed_command.name, prop->GetReplica());
        }
        break;
      }
      default:
        flush_command(make_lifecycle_command(typed_command));
        break;
    }
  };

  auto flush_direct_ordered_streams = [&]() {
    struct Cursor {
      enum class Kind : uint8_t {
        Legacy = 0,
        Transform,
        Velocity,
        RelativeVector,
        Motion,
        Property,
        Event,
        Audio,
        Lifecycle,
        ObjectService,
        RuntimeService,
        Animation,
        Armature,
        Material,
        Physics,
      };
      Kind kind = Kind::Legacy;
      size_t index = 0;
      size_t size = 0;
      uint64_t sort_key = 0;
      uint64_t record_sequence = 0;
    };

    std::array<Cursor, 15> cursors{};
    size_t cursor_count = 0;
    size_t total_direct_count = 0;
    auto append_cursor = [&](const Cursor::Kind kind, const size_t size) {
      if (size == 0) {
        return;
      }
      Cursor &cursor = cursors[cursor_count++];
      cursor.kind = kind;
      cursor.index = 0;
      cursor.size = size;
      total_direct_count += size;
    };

    append_cursor(Cursor::Kind::Legacy, m_commands.size());
    append_cursor(Cursor::Kind::Transform, m_transformCommands.size());
    append_cursor(Cursor::Kind::Velocity, m_velocityCommands.size());
    append_cursor(Cursor::Kind::RelativeVector, m_relativeVectorCommands.size());
    append_cursor(Cursor::Kind::Motion, m_motionCommands.size());
    append_cursor(Cursor::Kind::Property, m_propertyCommands.size());
    append_cursor(Cursor::Kind::Event, m_eventCommands.size());
    append_cursor(Cursor::Kind::Audio, m_audioCommands.size());
    append_cursor(Cursor::Kind::Lifecycle, m_lifecycleCommands.size());
    append_cursor(Cursor::Kind::ObjectService, m_objectServiceCommands.size());
    append_cursor(Cursor::Kind::RuntimeService, m_runtimeServiceCommands.size());
    append_cursor(Cursor::Kind::Animation, m_animationCommands.size());
    append_cursor(Cursor::Kind::Armature, m_armatureCommands.size());
    append_cursor(Cursor::Kind::Material, m_materialCommands.size());
    append_cursor(Cursor::Kind::Physics, m_physicsCommands.size());

    auto refresh_cursor = [&](Cursor &cursor) {
      if (cursor.index >= cursor.size) {
        return;
      }
      switch (cursor.kind) {
        case Cursor::Kind::Legacy:
          cursor.sort_key = m_commands[cursor.index].sort_key;
          cursor.record_sequence = m_commands[cursor.index].record_sequence;
          break;
        case Cursor::Kind::Transform:
          cursor.sort_key = m_transformCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_transformCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Velocity:
          cursor.sort_key = m_velocityCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_velocityCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::RelativeVector:
          cursor.sort_key = m_relativeVectorCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_relativeVectorCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Motion:
          cursor.sort_key = m_motionCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_motionCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Property:
          cursor.sort_key = m_propertyCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_propertyCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Event:
          cursor.sort_key = m_eventCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_eventCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Audio:
          cursor.sort_key = m_audioCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_audioCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Lifecycle:
          cursor.sort_key = m_lifecycleCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_lifecycleCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::ObjectService:
          cursor.sort_key = m_objectServiceCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_objectServiceCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::RuntimeService:
          cursor.sort_key = m_runtimeServiceCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_runtimeServiceCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Animation:
          cursor.sort_key = m_animationCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_animationCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Armature:
          cursor.sort_key = m_armatureCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_armatureCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Material:
          cursor.sort_key = m_materialCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_materialCommands[cursor.index].header.record_sequence;
          break;
        case Cursor::Kind::Physics:
          cursor.sort_key = m_physicsCommands[cursor.index].header.sort_key;
          cursor.record_sequence = m_physicsCommands[cursor.index].header.record_sequence;
          break;
      }
    };

    auto flush_cursor_command = [&](const Cursor::Kind kind, const size_t index) {
      switch (kind) {
        case Cursor::Kind::Legacy:
          flush_command(m_commands[index]);
          break;
        case Cursor::Kind::Transform:
          flush_typed_vector_command(m_transformCommands[index]);
          break;
        case Cursor::Kind::Velocity:
          flush_typed_vector_command(m_velocityCommands[index]);
          break;
        case Cursor::Kind::RelativeVector:
          flush_typed_vector_command(m_relativeVectorCommands[index]);
          break;
        case Cursor::Kind::Motion:
          flush_typed_motion_command(m_motionCommands[index]);
          break;
        case Cursor::Kind::Property:
          flush_typed_property_command(m_propertyCommands[index]);
          break;
        case Cursor::Kind::Event:
          flush_typed_event_command(m_eventCommands[index]);
          break;
        case Cursor::Kind::Audio:
          flush_typed_audio_command(m_audioCommands[index]);
          break;
        case Cursor::Kind::Lifecycle:
          flush_typed_lifecycle_command(m_lifecycleCommands[index]);
          break;
        case Cursor::Kind::ObjectService:
          flush_typed_object_service_command(m_objectServiceCommands[index]);
          break;
        case Cursor::Kind::RuntimeService:
          flush_typed_runtime_service_command(m_runtimeServiceCommands[index]);
          break;
        case Cursor::Kind::Animation:
          flush_typed_animation_command(m_animationCommands[index]);
          break;
        case Cursor::Kind::Armature:
          flush_typed_armature_command(m_armatureCommands[index]);
          break;
        case Cursor::Kind::Material:
          flush_typed_material_command(m_materialCommands[index]);
          break;
        case Cursor::Kind::Physics:
          flush_typed_physics_command(m_physicsCommands[index]);
          break;
      }
    };

    if (cursor_count == 0) {
      return;
    }
    if (cursor_count == 1) {
      const Cursor &cursor = cursors[0];
      switch (cursor.kind) {
        case Cursor::Kind::Legacy:
          for (const Command &command : m_commands) {
            flush_command(command);
          }
          break;
        case Cursor::Kind::Transform:
          for (const TypedVectorCommand &command : m_transformCommands) {
            flush_typed_vector_command(command);
          }
          break;
        case Cursor::Kind::Velocity:
          for (const TypedVectorCommand &command : m_velocityCommands) {
            flush_typed_vector_command(command);
          }
          break;
        case Cursor::Kind::RelativeVector:
          for (const TypedVectorCommand &command : m_relativeVectorCommands) {
            flush_typed_vector_command(command);
          }
          break;
        case Cursor::Kind::Motion:
          for (const TypedMotionCommand &command : m_motionCommands) {
            flush_typed_motion_command(command);
          }
          break;
        case Cursor::Kind::Property:
          for (const TypedPropertyCommand &command : m_propertyCommands) {
            flush_typed_property_command(command);
          }
          break;
        case Cursor::Kind::Event:
          for (const TypedEventCommand &command : m_eventCommands) {
            flush_typed_event_command(command);
          }
          break;
        case Cursor::Kind::Audio:
          for (const TypedAudioCommand &command : m_audioCommands) {
            flush_typed_audio_command(command);
          }
          break;
        case Cursor::Kind::Lifecycle:
          for (const TypedLifecycleCommand &command : m_lifecycleCommands) {
            flush_typed_lifecycle_command(command);
          }
          break;
        case Cursor::Kind::ObjectService:
          for (const TypedObjectServiceCommand &command : m_objectServiceCommands) {
            flush_typed_object_service_command(command);
          }
          break;
        case Cursor::Kind::RuntimeService:
          for (const TypedRuntimeServiceCommand &command : m_runtimeServiceCommands) {
            flush_typed_runtime_service_command(command);
          }
          break;
        case Cursor::Kind::Animation:
          for (const TypedAnimationCommand &command : m_animationCommands) {
            flush_typed_animation_command(command);
          }
          break;
        case Cursor::Kind::Armature:
          for (const TypedArmatureCommand &command : m_armatureCommands) {
            flush_typed_armature_command(command);
          }
          break;
        case Cursor::Kind::Material:
          for (const TypedMaterialCommand &command : m_materialCommands) {
            flush_typed_material_command(command);
          }
          break;
        case Cursor::Kind::Physics:
          for (const TypedPhysicsCommand &command : m_physicsCommands) {
            flush_typed_physics_command(command);
          }
          break;
      }
      return;
    }

    for (size_t cursor_index = 0; cursor_index < cursor_count; cursor_index++) {
      refresh_cursor(cursors[cursor_index]);
    }

    size_t flushed_count = 0;
    while (flushed_count < total_direct_count) {
      Cursor *selected = nullptr;
      for (size_t cursor_index = 0; cursor_index < cursor_count; cursor_index++) {
        Cursor &cursor = cursors[cursor_index];
        if (cursor.index >= cursor.size) {
          continue;
        }
        if (selected == nullptr ||
            logic_node_command_order_less(cursor.sort_key,
                                          cursor.record_sequence,
                                          selected->sort_key,
                                          selected->record_sequence))
        {
          selected = &cursor;
        }
      }
      if (selected == nullptr) {
        break;
      }

      flush_cursor_command(selected->kind, selected->index);
      selected->index++;
      refresh_cursor(*selected);
      flushed_count++;
    }
  };

  const uint32_t coalescible_command_count = CountCoalescibleCommands();
  const size_t total_command_count = Size();
  const bool enable_flush_coalescing =
      m_typedCommandStreamsEnabled &&
      coalescible_command_count != 0u &&
      (coalescible_command_count >= k_logic_node_min_coalescible_commands_for_runtime_flush ||
       coalescible_command_count == total_command_count);
  const bool direct_ordered_flush =
      m_typedCommandStreamsEnabled &&
      !enable_flush_coalescing &&
      command_streams_sorted();

  if (direct_ordered_flush) {
    CommandStreamStats stats = typed_stream_stats();
    stats.direct_ordered_stream_count = non_empty_stream_count();
    stats.direct_ordered_command_count = uint32_t(total_command_count);
    m_lastCommandStreamStats = stats;
    flush_direct_ordered_streams();
  }
  else {
    CommandStreamStats planner_stats;
    m_commands = BuildPlannedCommands(enable_flush_coalescing, true, &planner_stats);
    m_lastCommandStreamStats = planner_stats;
    for (const Command &command : m_commands) {
      flush_command(command);
    }
  }

  flush_material_parameter_object_updates();

  m_isFlushing = false;
  m_removedObjectsDuringFlush.clear();
  m_commands.clear();
  ClearTypedStreams();
}

void LN_CommandBuffer::Clear()
{
  AssertMainThread();
  BLI_assert_msg(!m_isRecording, "Logic Nodes command buffer cannot clear while recording");
  BLI_assert_msg(!m_isFlushing, "Logic Nodes command buffer cannot clear while flushing");

  m_commands.clear();
  ClearTypedStreams();
  m_removedObjectsDuringFlush.clear();
  m_lastCommandStreamStats = CommandStreamStats();
}

void LN_CommandBuffer::ClearMaterialParameterDefaultState()
{
  AssertMainThread();
  BLI_assert_msg(!m_isRecording,
                 "Logic Nodes command buffer cannot clear material parameter defaults while "
                 "recording");
  BLI_assert_msg(!m_isFlushing,
                 "Logic Nodes command buffer cannot clear material parameter defaults while "
                 "flushing");

  m_initializedMaterialParameterDefaults.clear();
}

void LN_CommandBuffer::AssertMainThread() const
{
  if (m_allowWorkerRecording) {
    return;
  }

  if (m_mainThreadId == std::thread::id()) {
    return;
  }

  BLI_assert(std::this_thread::get_id() == m_mainThreadId);
}
