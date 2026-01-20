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

 private:
  // Setup global 'bge' namespace
  static void SetupBGENamespace(v8::Local<v8::Context> context);

  // Setup 'logic' object in bge namespace
  static void SetupLogicObject(v8::Local<v8::Context> context);

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

  static void ControllerGetOwner(v8::Local<v8::Name> property,
                                 const v8::PropertyCallbackInfo<v8::Value> &info);
};

#endif  // WITH_JAVASCRIPT
