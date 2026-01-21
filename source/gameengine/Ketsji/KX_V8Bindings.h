/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2024 UPBGE Contributors
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_V8Bindings.h
 *  \ingroup ketsji
 *  \brief V8 JavaScript bindings for UPBGE objects
 */

#pragma once

#ifdef WITH_JAVASCRIPT

#include <string>

#  include <v8.h>

class KX_GameObject;
class KX_Scene;
class SCA_IController;
class SCA_ISensor;
class SCA_IActuator;
class KX_KetsjiEngine;
class PHY_IVehicle;
class PHY_ICharacter;

class KX_V8Bindings {
 public:
  // Initialize bindings - set up global objects and functions
  static void InitializeBindings(v8::Local<v8::Context> context);

  // Create JavaScript wrapper for GameObject
  static v8::Local<v8::Object> CreateGameObjectWrapper(v8::Isolate *isolate, KX_GameObject *obj);

  // Create JavaScript wrapper for Scene
  static v8::Local<v8::Object> CreateSceneWrapper(v8::Isolate *isolate, KX_Scene *scene);

  // Create JavaScript wrapper for Controller (includes "owner" -> parent KX_GameObject)
  static v8::Local<v8::Object> CreateControllerWrapper(v8::Isolate *isolate, SCA_IController *controller);

  // Create JavaScript wrapper for Sensor
  static v8::Local<v8::Object> CreateSensorWrapper(v8::Isolate *isolate, SCA_ISensor *sensor);

  // Create JavaScript wrapper for Actuator
  static v8::Local<v8::Object> CreateActuatorWrapper(v8::Isolate *isolate, SCA_IActuator *actuator);

  // Get C++ object from JavaScript wrapper
  static KX_GameObject *GetGameObjectFromWrapper(v8::Local<v8::Object> wrapper);
  static KX_Scene *GetSceneFromWrapper(v8::Local<v8::Object> wrapper);
  static SCA_IController *GetControllerFromWrapper(v8::Local<v8::Object> wrapper);
  static SCA_ISensor *GetSensorFromWrapper(v8::Local<v8::Object> wrapper);
  static SCA_IActuator *GetActuatorFromWrapper(v8::Local<v8::Object> wrapper);
  static PHY_IVehicle *GetVehicleFromWrapper(v8::Local<v8::Object> wrapper);
  static PHY_ICharacter *GetCharacterFromWrapper(v8::Local<v8::Object> wrapper);

  static v8::Local<v8::Object> CreateVehicleWrapper(v8::Isolate *isolate, PHY_IVehicle *vehicle);
  static v8::Local<v8::Object> CreateCharacterWrapper(v8::Isolate *isolate, PHY_ICharacter *character);

  // Helpers: [x,y,z] <-> MT_Vector3; resolve val as vector or GameObject position
  static bool Vec3FromArray(v8::Isolate *isolate,
                            v8::Local<v8::Value> val,
                            class MT_Vector3 &out);
  static v8::Local<v8::Array> ArrayFromVec3(v8::Isolate *isolate,
                                            const class MT_Vector3 &v);
  static bool ResolveVectorOrGameObject(v8::Isolate *isolate,
                                        v8::Local<v8::Context> context,
                                        v8::Local<v8::Value> val,
                                        KX_GameObject *fallbackObj,
                                        class MT_Vector3 &out);

 private:
  // Setup global 'bge' namespace
  static void SetupBGENamespace(v8::Local<v8::Context> context);

  // Setup 'logic' object in bge namespace
  static void SetupLogicObject(v8::Local<v8::Context> context);
  // Setup 'constraints' object in bge namespace
  static void SetupConstraints(v8::Local<v8::Context> context);

  // bge.constraints callbacks
  static void ConstraintsSetGravity(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void ConstraintsGetVehicleConstraint(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void ConstraintsCreateVehicle(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void ConstraintsGetCharacter(const v8::FunctionCallbackInfo<v8::Value> &args);

  // Callback functions for JavaScript API
  static void GetCurrentController(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void GetCurrentScene(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void GetCurrentControllerObject(const v8::FunctionCallbackInfo<v8::Value> &args);

  // GameObject property getters (AccessorNameGetterCallback: Local<Name>, PropertyCallbackInfo<Value>)
  static void GameObjectGetName(v8::Local<v8::Name> property,
                                const v8::PropertyCallbackInfo<v8::Value> &info);
  static void GameObjectGetPosition(v8::Local<v8::Name> property,
                                    const v8::PropertyCallbackInfo<v8::Value> &info);
  static void GameObjectGetRotation(v8::Local<v8::Name> property,
                                    const v8::PropertyCallbackInfo<v8::Value> &info);
  static void GameObjectGetScale(v8::Local<v8::Name> property,
                                 const v8::PropertyCallbackInfo<v8::Value> &info);

  static void GameObjectSetPosition(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void GameObjectSetRotation(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void GameObjectSetScale(const v8::FunctionCallbackInfo<v8::Value> &args);

  static void GameObjectApplyForce(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void GameObjectGetVelocity(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void GameObjectGetLinearVelocity(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void GameObjectSetLinearVelocity(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void GameObjectGetAngularVelocity(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void GameObjectSetAngularVelocity(const v8::FunctionCallbackInfo<v8::Value> &args);

  static void GameObjectGetHasPhysics(v8::Local<v8::Name> property,
                                      const v8::PropertyCallbackInfo<v8::Value> &info);

  static void GameObjectRayCast(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void GameObjectRayCastTo(const v8::FunctionCallbackInfo<v8::Value> &args);

  static void SceneGetObjects(v8::Local<v8::Name> property,
                              const v8::PropertyCallbackInfo<v8::Value> &info);
  static void SceneGet(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void SceneGetActiveCamera(v8::Local<v8::Name> property,
                                   const v8::PropertyCallbackInfo<v8::Value> &info);
  static void SceneSetActiveCamera(v8::Local<v8::Name> property,
                                   v8::Local<v8::Value> value,
                                   const v8::PropertyCallbackInfo<void> &info);
  static void SceneGetGravity(v8::Local<v8::Name> property,
                              const v8::PropertyCallbackInfo<v8::Value> &info);
  static void SceneSetGravity(v8::Local<v8::Name> property,
                              v8::Local<v8::Value> value,
                              const v8::PropertyCallbackInfo<void> &info);

  static void ControllerGetOwner(v8::Local<v8::Name> property,
                                 const v8::PropertyCallbackInfo<v8::Value> &info);
  static void ControllerGetSensors(v8::Local<v8::Name> property,
                                   const v8::PropertyCallbackInfo<v8::Value> &info);
  static void ControllerGetActuators(v8::Local<v8::Name> property,
                                     const v8::PropertyCallbackInfo<v8::Value> &info);
  static void ControllerActivate(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void ControllerDeactivate(const v8::FunctionCallbackInfo<v8::Value> &args);

  static void SensorGetPositive(v8::Local<v8::Name> property,
                                const v8::PropertyCallbackInfo<v8::Value> &info);
  static void SensorGetEvents(v8::Local<v8::Name> property,
                              const v8::PropertyCallbackInfo<v8::Value> &info);

  static void ActuatorGetName(v8::Local<v8::Name> property,
                              const v8::PropertyCallbackInfo<v8::Value> &info);

  // Vehicle wrapper
  static void VehicleAddWheel(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleGetNumWheels(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleGetWheelPosition(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleGetWheelRotation(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleGetWheelOrientationQuaternion(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleSetSteeringValue(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleApplyEngineForce(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleApplyBraking(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleSetTyreFriction(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleSetSuspensionStiffness(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleSetSuspensionDamping(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleSetSuspensionCompression(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleSetRollInfluence(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void VehicleGetConstraintId(v8::Local<v8::Name> property,
                                     const v8::PropertyCallbackInfo<v8::Value> &info);
  static void VehicleGetConstraintType(v8::Local<v8::Name> property,
                                       const v8::PropertyCallbackInfo<v8::Value> &info);
  static void VehicleGetRayMask(v8::Local<v8::Name> property,
                                const v8::PropertyCallbackInfo<v8::Value> &info);
  static void VehicleSetRayMask(v8::Local<v8::Name> property,
                                v8::Local<v8::Value> value,
                                const v8::PropertyCallbackInfo<void> &info);

  // Character wrapper
  static void CharacterJump(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void CharacterSetVelocity(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void CharacterReset(const v8::FunctionCallbackInfo<v8::Value> &args);
  static void CharacterGetOnGround(v8::Local<v8::Name> property,
                                   const v8::PropertyCallbackInfo<v8::Value> &info);
  static void CharacterGetGravity(v8::Local<v8::Name> property,
                                  const v8::PropertyCallbackInfo<v8::Value> &info);
  static void CharacterSetGravity(v8::Local<v8::Name> property,
                                  v8::Local<v8::Value> value,
                                  const v8::PropertyCallbackInfo<void> &info);
  static void CharacterGetFallSpeed(v8::Local<v8::Name> property,
                                    const v8::PropertyCallbackInfo<v8::Value> &info);
  static void CharacterSetFallSpeed(v8::Local<v8::Name> property,
                                    v8::Local<v8::Value> value,
                                    const v8::PropertyCallbackInfo<void> &info);
  static void CharacterGetMaxJumps(v8::Local<v8::Name> property,
                                   const v8::PropertyCallbackInfo<v8::Value> &info);
  static void CharacterSetMaxJumps(v8::Local<v8::Name> property,
                                   v8::Local<v8::Value> value,
                                   const v8::PropertyCallbackInfo<void> &info);
  static void CharacterGetMaxSlope(v8::Local<v8::Name> property,
                                   const v8::PropertyCallbackInfo<v8::Value> &info);
  static void CharacterSetMaxSlope(v8::Local<v8::Name> property,
                                   v8::Local<v8::Value> value,
                                   const v8::PropertyCallbackInfo<void> &info);
  static void CharacterGetJumpCount(v8::Local<v8::Name> property,
                                    const v8::PropertyCallbackInfo<v8::Value> &info);
  static void CharacterGetJumpSpeed(v8::Local<v8::Name> property,
                                    const v8::PropertyCallbackInfo<v8::Value> &info);
  static void CharacterSetJumpSpeed(v8::Local<v8::Name> property,
                                    v8::Local<v8::Value> value,
                                    const v8::PropertyCallbackInfo<void> &info);
  static void CharacterGetWalkDirection(v8::Local<v8::Name> property,
                                        const v8::PropertyCallbackInfo<v8::Value> &info);
  static void CharacterSetWalkDirection(v8::Local<v8::Name> property,
                                        v8::Local<v8::Value> value,
                                        const v8::PropertyCallbackInfo<void> &info);
};

#endif  // WITH_JAVASCRIPT
