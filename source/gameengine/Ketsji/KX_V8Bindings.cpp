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

/** \file KX_V8Bindings.cpp
 *  \ingroup ketsji
 */

#ifdef WITH_JAVASCRIPT

#  include "v8_include.h"

#  include "KX_V8Bindings.h"
#  include "KX_Camera.h"
#  include "KX_GameObject.h"
#  include "KX_RayCast.h"
#  include "KX_Scene.h"
#  include "EXP_ListValue.h"
#  include "SCA_IController.h"
#  include "SCA_ILogicBrick.h"
#  include "SCA_LogicManager.h"
#  include "SCA_ISensor.h"
#  include "SCA_IActuator.h"
#  include "SCA_JavaScriptController.h"
#  include "SCA_IInputDevice.h"
#  include "SCA_InputEvent.h"
#  include "SCA_KeyboardSensor.h"
#  include "KX_Globals.h"
#  include "KX_KetsjiEngine.h"
#  include "KX_MotionState.h"
#  include "MT_Vector3.h"
#  include "MT_Matrix3x3.h"
#  include "MT_Quaternion.h"
#  include "PHY_DynamicTypes.h"
#  include "PHY_IPhysicsEnvironment.h"
#  include "PHY_IVehicle.h"
#  include "PHY_ICharacter.h"

using namespace v8;

// --- Helpers ---
bool KX_V8Bindings::Vec3FromArray(Isolate *isolate, Local<Value> val, MT_Vector3 &out)
{
  if (!val->IsArray()) {
    return false;
  }
  Local<Array> arr = val.As<Array>();
  if (arr->Length() < 3) {
    return false;
  }
  Local<Context> ctx = isolate->GetCurrentContext();
  double x = arr->Get(ctx, 0).ToLocalChecked()->NumberValue(ctx).FromMaybe(0.0);
  double y = arr->Get(ctx, 1).ToLocalChecked()->NumberValue(ctx).FromMaybe(0.0);
  double z = arr->Get(ctx, 2).ToLocalChecked()->NumberValue(ctx).FromMaybe(0.0);
  out = MT_Vector3(MT_Scalar(x), MT_Scalar(y), MT_Scalar(z));
  return true;
}

Local<Array> KX_V8Bindings::ArrayFromVec3(Isolate *isolate, const MT_Vector3 &v)
{
  Local<Context> context = isolate->GetCurrentContext();
  Local<Array> arr = Array::New(isolate, 3);
  arr->Set(context, 0, Number::New(isolate, v[0])).Check();
  arr->Set(context, 1, Number::New(isolate, v[1])).Check();
  arr->Set(context, 2, Number::New(isolate, v[2])).Check();
  return arr;
}

bool KX_V8Bindings::ResolveVectorOrGameObject(Isolate *isolate,
                                              Local<Context> context,
                                              Local<Value> val,
                                              KX_GameObject *fallbackObj,
                                              MT_Vector3 &out)
{
  if (val.IsEmpty() || val->IsUndefined() || val->IsNull()) {
    if (fallbackObj) {
      out = fallbackObj->NodeGetWorldPosition();
      return true;
    }
    return false;
  }
  if (Vec3FromArray(isolate, val, out)) {
    return true;
  }
  if (val->IsObject()) {
    Local<Object> obj = val.As<Object>();
    Local<Value> tag = obj->Get(context, String::NewFromUtf8Literal(isolate, "__bgeType"))
                           .ToLocalChecked();
    if (tag->StrictEquals(String::NewFromUtf8Literal(isolate, "GameObject"))) {
      KX_GameObject *go = GetGameObjectFromWrapper(obj);
      if (go) {
        out = go->NodeGetWorldPosition();
        return true;
      }
    }
  }
  return false;
}

void KX_V8Bindings::InitializeBindings(Local<Context> context)
{
  Context::Scope context_scope(context);

  SetupBGENamespace(context);
  SetupLogicObject(context);
  SetupConstraints(context);
}

void KX_V8Bindings::SetupBGENamespace(Local<Context> context)
{
  Isolate *isolate = context->GetIsolate();
  Local<Object> global = context->Global();

  Local<ObjectTemplate> bge_template = ObjectTemplate::New(isolate);
  Local<Object> bge_obj = bge_template->NewInstance(context).ToLocalChecked();

  // bge.events: key and input-state constants for Keyboard sensor scripts
  Local<Object> events_obj = ObjectTemplate::New(isolate)->NewInstance(context).ToLocalChecked();
  events_obj->Set(context, String::NewFromUtf8Literal(isolate, "WKEY"),
                  Integer::New(isolate, SCA_IInputDevice::WKEY)).Check();
  events_obj->Set(context, String::NewFromUtf8Literal(isolate, "SKEY"),
                  Integer::New(isolate, SCA_IInputDevice::SKEY)).Check();
  events_obj->Set(context, String::NewFromUtf8Literal(isolate, "AKEY"),
                  Integer::New(isolate, SCA_IInputDevice::AKEY)).Check();
  events_obj->Set(context, String::NewFromUtf8Literal(isolate, "DKEY"),
                  Integer::New(isolate, SCA_IInputDevice::DKEY)).Check();
  events_obj->Set(context, String::NewFromUtf8Literal(isolate, "ACTIVE"),
                  Integer::New(isolate, SCA_InputEvent::ACTIVE)).Check();
  events_obj->Set(context, String::NewFromUtf8Literal(isolate, "JUSTACTIVATED"),
                  Integer::New(isolate, SCA_InputEvent::JUSTACTIVATED)).Check();
  events_obj->Set(context, String::NewFromUtf8Literal(isolate, "JUSTRELEASED"),
                  Integer::New(isolate, SCA_InputEvent::JUSTRELEASED)).Check();
  bge_obj->Set(context, String::NewFromUtf8Literal(isolate, "events"), events_obj).Check();

  global->Set(context, String::NewFromUtf8Literal(isolate, "bge"), bge_obj).Check();
}

void KX_V8Bindings::SetupConstraints(Local<Context> context)
{
  Isolate *isolate = context->GetIsolate();
  Local<Object> global = context->Global();
  Local<Object> bge_obj = global->Get(context, String::NewFromUtf8Literal(isolate, "bge"))
                              .ToLocalChecked()
                              .As<Object>();

  Local<ObjectTemplate> constraints_template = ObjectTemplate::New(isolate);
  constraints_template->Set(String::NewFromUtf8Literal(isolate, "setGravity"),
                            FunctionTemplate::New(isolate, ConstraintsSetGravity));
  constraints_template->Set(String::NewFromUtf8Literal(isolate, "getVehicleConstraint"),
                            FunctionTemplate::New(isolate, ConstraintsGetVehicleConstraint));
  constraints_template->Set(String::NewFromUtf8Literal(isolate, "createVehicle"),
                            FunctionTemplate::New(isolate, ConstraintsCreateVehicle));
  constraints_template->Set(String::NewFromUtf8Literal(isolate, "getCharacter"),
                            FunctionTemplate::New(isolate, ConstraintsGetCharacter));

  Local<Object> constraints_obj = constraints_template->NewInstance(context).ToLocalChecked();
  bge_obj->Set(context, String::NewFromUtf8Literal(isolate, "constraints"), constraints_obj)
      .Check();
}

void KX_V8Bindings::ConstraintsSetGravity(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  extern SCA_JavaScriptController *g_currentJavaScriptController;
  if (!g_currentJavaScriptController || args.Length() < 3) {
    return;
  }
  KX_Scene *scene = g_currentJavaScriptController->GetScene();
  if (!scene) {
    return;
  }
  PHY_IPhysicsEnvironment *pe = scene->GetPhysicsEnvironment();
  if (!pe) {
    return;
  }
  MT_Vector3 g;
  if (Vec3FromArray(isolate, args[0], g)) {
    pe->SetGravity(float(g.x()), float(g.y()), float(g.z()));
    return;
  }
  double x = args[0]->NumberValue(context).FromMaybe(0.0);
  double y = args[1]->NumberValue(context).FromMaybe(0.0);
  double z = args[2]->NumberValue(context).FromMaybe(0.0);
  pe->SetGravity(float(x), float(y), float(z));
}

void KX_V8Bindings::ConstraintsGetVehicleConstraint(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  extern SCA_JavaScriptController *g_currentJavaScriptController;
  if (!g_currentJavaScriptController || args.Length() < 1) {
    args.GetReturnValue().SetNull();
    return;
  }
  KX_Scene *scene = g_currentJavaScriptController->GetScene();
  if (!scene) {
    args.GetReturnValue().SetNull();
    return;
  }
  PHY_IPhysicsEnvironment *pe = scene->GetPhysicsEnvironment();
  if (!pe) {
    args.GetReturnValue().SetNull();
    return;
  }
  int cid = (int)args[0]->IntegerValue(context).FromMaybe(0);
  PHY_IVehicle *v = pe->GetVehicleConstraint(cid);
  if (v) {
    args.GetReturnValue().Set(CreateVehicleWrapper(isolate, v));
  }
  else {
    args.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::ConstraintsCreateVehicle(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  extern SCA_JavaScriptController *g_currentJavaScriptController;
  if (!g_currentJavaScriptController || args.Length() < 1) {
    args.GetReturnValue().SetNull();
    return;
  }
  KX_Scene *scene = g_currentJavaScriptController->GetScene();
  if (!scene) {
    args.GetReturnValue().SetNull();
    return;
  }
  PHY_IPhysicsEnvironment *pe = scene->GetPhysicsEnvironment();
  if (!pe) {
    args.GetReturnValue().SetNull();
    return;
  }
  if (!args[0]->IsObject()) {
    args.GetReturnValue().SetNull();
    return;
  }
  KX_GameObject *chassis = GetGameObjectFromWrapper(args[0].As<Object>());
  if (!chassis) {
    args.GetReturnValue().SetNull();
    return;
  }
  PHY_IPhysicsController *phys = chassis->GetPhysicsController();
  if (!phys) {
    args.GetReturnValue().SetNull();
    return;
  }
  PHY_IVehicle *v = pe->CreateVehicle(phys);
  if (v) {
    args.GetReturnValue().Set(CreateVehicleWrapper(isolate, v));
  }
  else {
    args.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::ConstraintsGetCharacter(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  extern SCA_JavaScriptController *g_currentJavaScriptController;
  if (!g_currentJavaScriptController || args.Length() < 1) {
    args.GetReturnValue().SetNull();
    return;
  }
  KX_Scene *scene = g_currentJavaScriptController->GetScene();
  if (!scene) {
    args.GetReturnValue().SetNull();
    return;
  }
  PHY_IPhysicsEnvironment *pe = scene->GetPhysicsEnvironment();
  if (!pe) {
    args.GetReturnValue().SetNull();
    return;
  }
  if (!args[0]->IsObject()) {
    args.GetReturnValue().SetNull();
    return;
  }
  KX_GameObject *obj = GetGameObjectFromWrapper(args[0].As<Object>());
  if (!obj) {
    args.GetReturnValue().SetNull();
    return;
  }
  PHY_ICharacter *c = pe->GetCharacterController(obj);
  if (c) {
    args.GetReturnValue().Set(CreateCharacterWrapper(isolate, c));
  }
  else {
    args.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::SetupLogicObject(Local<Context> context)
{
  Isolate *isolate = context->GetIsolate();
  Local<Object> global = context->Global();
  Local<Object> bge_obj = global->Get(context, String::NewFromUtf8Literal(isolate, "bge"))
                              .ToLocalChecked()
                              .As<Object>();

  Local<ObjectTemplate> logic_template = ObjectTemplate::New(isolate);
  
  // Add functions to logic object
  logic_template->Set(String::NewFromUtf8Literal(isolate, "getCurrentController"),
                      FunctionTemplate::New(isolate, GetCurrentController));
  logic_template->Set(String::NewFromUtf8Literal(isolate, "getCurrentScene"),
                      FunctionTemplate::New(isolate, GetCurrentScene));
  logic_template->Set(String::NewFromUtf8Literal(isolate, "getCurrentControllerObject"),
                      FunctionTemplate::New(isolate, GetCurrentControllerObject));

  Local<Object> logic_obj = logic_template->NewInstance(context).ToLocalChecked();
  bge_obj->Set(context, String::NewFromUtf8Literal(isolate, "logic"), logic_obj).Check();
}

void KX_V8Bindings::GetCurrentController(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  
  extern SCA_JavaScriptController *g_currentJavaScriptController;
  if (g_currentJavaScriptController) {
    Local<Object> controller_obj = CreateControllerWrapper(isolate, g_currentJavaScriptController);
    args.GetReturnValue().Set(controller_obj);
  }
  else {
    args.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::GetCurrentScene(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  
  extern SCA_JavaScriptController *g_currentJavaScriptController;
  if (g_currentJavaScriptController) {
    KX_Scene *scene = g_currentJavaScriptController->GetScene();
    if (scene) {
      Local<Object> scene_obj = CreateSceneWrapper(isolate, scene);
      args.GetReturnValue().Set(scene_obj);
    }
    else {
      args.GetReturnValue().SetNull();
    }
  }
  else {
    args.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::GetCurrentControllerObject(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  
  extern SCA_JavaScriptController *g_currentJavaScriptController;
  if (g_currentJavaScriptController) {
    KX_GameObject *obj = static_cast<KX_GameObject *>(g_currentJavaScriptController->GetParent());
    if (obj) {
      Local<Object> obj_wrapper = CreateGameObjectWrapper(isolate, obj);
      args.GetReturnValue().Set(obj_wrapper);
    }
    else {
      args.GetReturnValue().SetNull();
    }
  }
  else {
    args.GetReturnValue().SetNull();
  }
}

Local<Object> KX_V8Bindings::CreateGameObjectWrapper(Isolate *isolate, KX_GameObject *obj)
{
  EscapableHandleScope handle_scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();

  Local<ObjectTemplate> obj_template = ObjectTemplate::New(isolate);
  obj_template->SetInternalFieldCount(1);  // Store pointer to C++ object

  // Add property accessors (SetNativeDataProperty: getter-only)
  obj_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "name"), GameObjectGetName);
  obj_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "position"), GameObjectGetPosition);
  obj_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "rotation"), GameObjectGetRotation);
  obj_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "scale"), GameObjectGetScale);
  obj_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "has_physics"), GameObjectGetHasPhysics);
  obj_template->Set(String::NewFromUtf8Literal(isolate, "setPosition"),
                    FunctionTemplate::New(isolate, GameObjectSetPosition));
  obj_template->Set(String::NewFromUtf8Literal(isolate, "setRotation"),
                    FunctionTemplate::New(isolate, GameObjectSetRotation));
  obj_template->Set(String::NewFromUtf8Literal(isolate, "setScale"),
                    FunctionTemplate::New(isolate, GameObjectSetScale));
  obj_template->Set(String::NewFromUtf8Literal(isolate, "applyForce"),
                    FunctionTemplate::New(isolate, GameObjectApplyForce));
  obj_template->Set(String::NewFromUtf8Literal(isolate, "getVelocity"),
                    FunctionTemplate::New(isolate, GameObjectGetVelocity));
  obj_template->Set(String::NewFromUtf8Literal(isolate, "getLinearVelocity"),
                    FunctionTemplate::New(isolate, GameObjectGetLinearVelocity));
  obj_template->Set(String::NewFromUtf8Literal(isolate, "setLinearVelocity"),
                    FunctionTemplate::New(isolate, GameObjectSetLinearVelocity));
  obj_template->Set(String::NewFromUtf8Literal(isolate, "getAngularVelocity"),
                    FunctionTemplate::New(isolate, GameObjectGetAngularVelocity));
  obj_template->Set(String::NewFromUtf8Literal(isolate, "setAngularVelocity"),
                    FunctionTemplate::New(isolate, GameObjectSetAngularVelocity));
  obj_template->Set(String::NewFromUtf8Literal(isolate, "rayCast"),
                    FunctionTemplate::New(isolate, GameObjectRayCast));
  obj_template->Set(String::NewFromUtf8Literal(isolate, "rayCastTo"),
                    FunctionTemplate::New(isolate, GameObjectRayCastTo));

  Local<Object> wrapper = obj_template->NewInstance(context).ToLocalChecked();
  wrapper->SetInternalField(0, External::New(isolate, obj));
  wrapper->Set(context, String::NewFromUtf8Literal(isolate, "__bgeType"),
               String::NewFromUtf8Literal(isolate, "GameObject")).Check();

  return handle_scope.Escape(wrapper);
}

Local<Object> KX_V8Bindings::CreateSceneWrapper(Isolate *isolate, KX_Scene *scene)
{
  EscapableHandleScope handle_scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();

  Local<ObjectTemplate> scene_template = ObjectTemplate::New(isolate);
  scene_template->SetInternalFieldCount(1);
  scene_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "objects"),
                                        SceneGetObjects);
  scene_template->Set(String::NewFromUtf8Literal(isolate, "get"),
                      FunctionTemplate::New(isolate, SceneGet));
  scene_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "activeCamera"),
                                        SceneGetActiveCamera, SceneSetActiveCamera);
  scene_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "gravity"),
                                        SceneGetGravity, SceneSetGravity);

  Local<Object> wrapper = scene_template->NewInstance(context).ToLocalChecked();
  wrapper->SetInternalField(0, External::New(isolate, scene));

  return handle_scope.Escape(wrapper);
}

void KX_V8Bindings::SceneGetObjects(Local<Name> property,
                                    const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = info.Holder();
  KX_Scene *scene = GetSceneFromWrapper(self);
  if (!scene) {
    info.GetReturnValue().Set(Array::New(isolate));
    return;
  }
  EXP_ListValue<KX_GameObject> *list = scene->GetObjectList();
  if (!list) {
    info.GetReturnValue().Set(Array::New(isolate));
    return;
  }
  int n = list->GetCount();
  Local<Array> arr = Array::New(isolate, n);
  for (int i = 0; i < n; i++) {
    arr->Set(context, i, CreateGameObjectWrapper(isolate, list->GetValue(i))).Check();
  }
  info.GetReturnValue().Set(arr);
}

void KX_V8Bindings::SceneGet(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = args.This();
  KX_Scene *scene = GetSceneFromWrapper(self);
  if (!scene || args.Length() < 1 || !args[0]->IsString()) {
    args.GetReturnValue().SetNull();
    return;
  }
  String::Utf8Value nameStr(isolate, args[0]);
  if (!*nameStr) {
    args.GetReturnValue().SetNull();
    return;
  }
  std::string name(*nameStr);
  EXP_ListValue<KX_GameObject> *list = scene->GetObjectList();
  if (!list) {
    args.GetReturnValue().SetNull();
    return;
  }
  KX_GameObject *obj = list->FindValue(name);
  if (obj) {
    args.GetReturnValue().Set(CreateGameObjectWrapper(isolate, obj));
  }
  else {
    args.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::SceneGetActiveCamera(Local<Name> property,
                                         const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Object> self = info.Holder();
  KX_Scene *scene = GetSceneFromWrapper(self);
  if (!scene) {
    info.GetReturnValue().SetNull();
    return;
  }
  KX_Camera *cam = scene->GetActiveCamera();
  if (cam) {
    info.GetReturnValue().Set(CreateGameObjectWrapper(isolate, cam));
  }
  else {
    info.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::SceneSetActiveCamera(Local<Name> property,
                                         Local<Value> value,
                                         const PropertyCallbackInfo<void> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Object> self = info.Holder();
  KX_Scene *scene = GetSceneFromWrapper(self);
  if (!scene || value.IsEmpty() || !value->IsObject()) {
    return;
  }
  KX_GameObject *go = GetGameObjectFromWrapper(value.As<Object>());
  if (!go) {
    return;
  }
  KX_Camera *cam = dynamic_cast<KX_Camera *>(go);
  if (cam) {
    scene->SetActiveCamera(cam);
  }
}

void KX_V8Bindings::SceneGetGravity(Local<Name> property,
                                    const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Object> self = info.Holder();
  KX_Scene *scene = GetSceneFromWrapper(self);
  if (!scene) {
    info.GetReturnValue().SetNull();
    return;
  }
  MT_Vector3 g = scene->GetGravity();
  info.GetReturnValue().Set(ArrayFromVec3(isolate, g));
}

void KX_V8Bindings::SceneSetGravity(Local<Name> property,
                                    Local<Value> value,
                                    const PropertyCallbackInfo<void> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Object> self = info.Holder();
  KX_Scene *scene = GetSceneFromWrapper(self);
  if (!scene || value.IsEmpty()) {
    return;
  }
  MT_Vector3 g;
  if (Vec3FromArray(isolate, value, g)) {
    scene->SetGravity(g);
  }
}

Local<Object> KX_V8Bindings::CreateControllerWrapper(Isolate *isolate, SCA_IController *controller)
{
  EscapableHandleScope handle_scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();

  Local<ObjectTemplate> controller_template = ObjectTemplate::New(isolate);
  controller_template->SetInternalFieldCount(1);
  controller_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "owner"),
                                            ControllerGetOwner);
  controller_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "sensors"),
                                            ControllerGetSensors);
  controller_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "actuators"),
                                            ControllerGetActuators);
  controller_template->Set(String::NewFromUtf8Literal(isolate, "activate"),
                           FunctionTemplate::New(isolate, ControllerActivate));
  controller_template->Set(String::NewFromUtf8Literal(isolate, "deactivate"),
                           FunctionTemplate::New(isolate, ControllerDeactivate));

  Local<Object> wrapper = controller_template->NewInstance(context).ToLocalChecked();
  wrapper->SetInternalField(0, External::New(isolate, controller));

  return handle_scope.Escape(wrapper);
}

Local<Object> KX_V8Bindings::CreateSensorWrapper(Isolate *isolate, SCA_ISensor *sensor)
{
  EscapableHandleScope handle_scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();

  Local<ObjectTemplate> sensor_template = ObjectTemplate::New(isolate);
  sensor_template->SetInternalFieldCount(1);
  sensor_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "positive"),
                                         SensorGetPositive);
  sensor_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "events"),
                                         SensorGetEvents);

  Local<Object> wrapper = sensor_template->NewInstance(context).ToLocalChecked();
  wrapper->SetInternalField(0, External::New(isolate, sensor));

  return handle_scope.Escape(wrapper);
}

Local<Object> KX_V8Bindings::CreateActuatorWrapper(Isolate *isolate, SCA_IActuator *actuator)
{
  EscapableHandleScope handle_scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();

  Local<ObjectTemplate> actuator_template = ObjectTemplate::New(isolate);
  actuator_template->SetInternalFieldCount(1);
  actuator_template->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "name"),
                                           ActuatorGetName);

  Local<Object> wrapper = actuator_template->NewInstance(context).ToLocalChecked();
  wrapper->SetInternalField(0, External::New(isolate, actuator));

  return handle_scope.Escape(wrapper);
}

Local<Object> KX_V8Bindings::CreateVehicleWrapper(Isolate *isolate, PHY_IVehicle *vehicle)
{
  EscapableHandleScope handle_scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();
  Local<ObjectTemplate> tpl = ObjectTemplate::New(isolate);
  tpl->SetInternalFieldCount(1);
  tpl->Set(String::NewFromUtf8Literal(isolate, "addWheel"),
           FunctionTemplate::New(isolate, VehicleAddWheel));
  tpl->Set(String::NewFromUtf8Literal(isolate, "getNumWheels"),
           FunctionTemplate::New(isolate, VehicleGetNumWheels));
  tpl->Set(String::NewFromUtf8Literal(isolate, "getWheelPosition"),
           FunctionTemplate::New(isolate, VehicleGetWheelPosition));
  tpl->Set(String::NewFromUtf8Literal(isolate, "getWheelRotation"),
           FunctionTemplate::New(isolate, VehicleGetWheelRotation));
  tpl->Set(String::NewFromUtf8Literal(isolate, "getWheelOrientationQuaternion"),
           FunctionTemplate::New(isolate, VehicleGetWheelOrientationQuaternion));
  tpl->Set(String::NewFromUtf8Literal(isolate, "setSteeringValue"),
           FunctionTemplate::New(isolate, VehicleSetSteeringValue));
  tpl->Set(String::NewFromUtf8Literal(isolate, "applyEngineForce"),
           FunctionTemplate::New(isolate, VehicleApplyEngineForce));
  tpl->Set(String::NewFromUtf8Literal(isolate, "applyBraking"),
           FunctionTemplate::New(isolate, VehicleApplyBraking));
  tpl->Set(String::NewFromUtf8Literal(isolate, "setTyreFriction"),
           FunctionTemplate::New(isolate, VehicleSetTyreFriction));
  tpl->Set(String::NewFromUtf8Literal(isolate, "setSuspensionStiffness"),
           FunctionTemplate::New(isolate, VehicleSetSuspensionStiffness));
  tpl->Set(String::NewFromUtf8Literal(isolate, "setSuspensionDamping"),
           FunctionTemplate::New(isolate, VehicleSetSuspensionDamping));
  tpl->Set(String::NewFromUtf8Literal(isolate, "setSuspensionCompression"),
           FunctionTemplate::New(isolate, VehicleSetSuspensionCompression));
  tpl->Set(String::NewFromUtf8Literal(isolate, "setRollInfluence"),
           FunctionTemplate::New(isolate, VehicleSetRollInfluence));
  tpl->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "constraintId"),
                             VehicleGetConstraintId);
  tpl->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "constraintType"),
                             VehicleGetConstraintType);
  tpl->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "rayMask"),
                             VehicleGetRayMask, VehicleSetRayMask);

  Local<Object> wrapper = tpl->NewInstance(context).ToLocalChecked();
  wrapper->SetInternalField(0, External::New(isolate, vehicle));
  return handle_scope.Escape(wrapper);
}

Local<Object> KX_V8Bindings::CreateCharacterWrapper(Isolate *isolate, PHY_ICharacter *character)
{
  EscapableHandleScope handle_scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();
  Local<ObjectTemplate> tpl = ObjectTemplate::New(isolate);
  tpl->SetInternalFieldCount(1);
  tpl->Set(String::NewFromUtf8Literal(isolate, "jump"), FunctionTemplate::New(isolate, CharacterJump));
  tpl->Set(String::NewFromUtf8Literal(isolate, "setVelocity"),
           FunctionTemplate::New(isolate, CharacterSetVelocity));
  tpl->Set(String::NewFromUtf8Literal(isolate, "reset"), FunctionTemplate::New(isolate, CharacterReset));
  tpl->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "onGround"),
                             CharacterGetOnGround);
  tpl->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "gravity"),
                             CharacterGetGravity, CharacterSetGravity);
  tpl->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "fallSpeed"),
                             CharacterGetFallSpeed, CharacterSetFallSpeed);
  tpl->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "maxJumps"),
                             CharacterGetMaxJumps, CharacterSetMaxJumps);
  tpl->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "maxSlope"),
                             CharacterGetMaxSlope, CharacterSetMaxSlope);
  tpl->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "jumpCount"),
                             CharacterGetJumpCount);
  tpl->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "jumpSpeed"),
                             CharacterGetJumpSpeed, CharacterSetJumpSpeed);
  tpl->SetNativeDataProperty(String::NewFromUtf8Literal(isolate, "walkDirection"),
                             CharacterGetWalkDirection, CharacterSetWalkDirection);

  Local<Object> wrapper = tpl->NewInstance(context).ToLocalChecked();
  wrapper->SetInternalField(0, External::New(isolate, character));
  return handle_scope.Escape(wrapper);
}

KX_GameObject *KX_V8Bindings::GetGameObjectFromWrapper(Local<Object> wrapper)
{
  if (wrapper->InternalFieldCount() > 0) {
    Local<External> external = wrapper->GetInternalField(0).As<External>();
    return static_cast<KX_GameObject *>(external->Value());
  }
  return nullptr;
}

KX_Scene *KX_V8Bindings::GetSceneFromWrapper(Local<Object> wrapper)
{
  if (wrapper->InternalFieldCount() > 0) {
    Local<External> external = wrapper->GetInternalField(0).As<External>();
    return static_cast<KX_Scene *>(external->Value());
  }
  return nullptr;
}

SCA_IController *KX_V8Bindings::GetControllerFromWrapper(Local<Object> wrapper)
{
  if (wrapper->InternalFieldCount() > 0) {
    Local<External> external = wrapper->GetInternalField(0).As<External>();
    return static_cast<SCA_IController *>(external->Value());
  }
  return nullptr;
}

SCA_ISensor *KX_V8Bindings::GetSensorFromWrapper(Local<Object> wrapper)
{
  if (wrapper->InternalFieldCount() > 0) {
    Local<External> external = wrapper->GetInternalField(0).As<External>();
    return static_cast<SCA_ISensor *>(external->Value());
  }
  return nullptr;
}

SCA_IActuator *KX_V8Bindings::GetActuatorFromWrapper(Local<Object> wrapper)
{
  if (wrapper->InternalFieldCount() > 0) {
    Local<External> external = wrapper->GetInternalField(0).As<External>();
    return static_cast<SCA_IActuator *>(external->Value());
  }
  return nullptr;
}

PHY_IVehicle *KX_V8Bindings::GetVehicleFromWrapper(Local<Object> wrapper)
{
  if (wrapper->InternalFieldCount() > 0) {
    Local<External> external = wrapper->GetInternalField(0).As<External>();
    return static_cast<PHY_IVehicle *>(external->Value());
  }
  return nullptr;
}

PHY_ICharacter *KX_V8Bindings::GetCharacterFromWrapper(Local<Object> wrapper)
{
  if (wrapper->InternalFieldCount() > 0) {
    Local<External> external = wrapper->GetInternalField(0).As<External>();
    return static_cast<PHY_ICharacter *>(external->Value());
  }
  return nullptr;
}

void KX_V8Bindings::GameObjectGetName(Local<Name> property,
                                      const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Object> self = info.Holder();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  
  if (obj) {
    Local<String> name = String::NewFromUtf8(isolate, obj->GetName().c_str()).ToLocalChecked();
    info.GetReturnValue().Set(name);
  }
  else {
    info.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::GameObjectGetPosition(Local<Name> property,
                                          const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = info.Holder();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  
  if (obj) {
    MT_Vector3 pos = obj->NodeGetWorldPosition();
    Local<Array> pos_array = Array::New(isolate, 3);
    pos_array->Set(context, 0, Number::New(isolate, pos[0])).Check();
    pos_array->Set(context, 1, Number::New(isolate, pos[1])).Check();
    pos_array->Set(context, 2, Number::New(isolate, pos[2])).Check();
    info.GetReturnValue().Set(pos_array);
  }
  else {
    info.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::GameObjectGetRotation(Local<Name> property,
                                          const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = info.Holder();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  
  if (obj) {
    MT_Matrix3x3 rot = obj->NodeGetWorldOrientation();
    MT_Scalar yaw, pitch, roll;
    rot.getEuler(yaw, pitch, roll);
    MT_Vector3 euler(pitch, yaw, roll);
    Local<Array> rot_array = Array::New(isolate, 3);
    rot_array->Set(context, 0, Number::New(isolate, euler[0])).Check();
    rot_array->Set(context, 1, Number::New(isolate, euler[1])).Check();
    rot_array->Set(context, 2, Number::New(isolate, euler[2])).Check();
    info.GetReturnValue().Set(rot_array);
  }
  else {
    info.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::GameObjectGetScale(Local<Name> property,
                                       const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = info.Holder();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  
  if (obj) {
    MT_Vector3 scale = obj->NodeGetWorldScaling();
    Local<Array> scale_array = Array::New(isolate, 3);
    scale_array->Set(context, 0, Number::New(isolate, scale[0])).Check();
    scale_array->Set(context, 1, Number::New(isolate, scale[1])).Check();
    scale_array->Set(context, 2, Number::New(isolate, scale[2])).Check();
    info.GetReturnValue().Set(scale_array);
  }
  else {
    info.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::GameObjectSetPosition(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Object> self = args.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj || args.Length() < 3) {
    return;
  }
  Local<Context> ctx = isolate->GetCurrentContext();
  double x = args[0]->NumberValue(ctx).FromMaybe(0.0);
  double y = args[1]->NumberValue(ctx).FromMaybe(0.0);
  double z = args[2]->NumberValue(ctx).FromMaybe(0.0);
  obj->NodeSetWorldPosition(MT_Vector3(MT_Scalar(x), MT_Scalar(y), MT_Scalar(z)));
}

void KX_V8Bindings::GameObjectSetRotation(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Object> self = args.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj || args.Length() < 1) {
    return;
  }
  MT_Vector3 euler;
  if (Vec3FromArray(isolate, args[0], euler)) {
    /* from [pitch, yaw, roll] */
  }
  else if (args.Length() >= 3) {
    Local<Context> ctx = isolate->GetCurrentContext();
    euler = MT_Vector3(MT_Scalar(args[0]->NumberValue(ctx).FromMaybe(0.0)),
                       MT_Scalar(args[1]->NumberValue(ctx).FromMaybe(0.0)),
                       MT_Scalar(args[2]->NumberValue(ctx).FromMaybe(0.0)));
  }
  else {
    return;
  }
  /* getRotation exports [pitch, yaw, roll]; MT_Matrix3x3 euler ctor expects (yaw, pitch, roll) */
  MT_Matrix3x3 mat(MT_Vector3(euler[1], euler[0], euler[2]));
  obj->NodeSetGlobalOrientation(mat);
}

void KX_V8Bindings::GameObjectSetScale(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Object> self = args.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj || args.Length() < 1) {
    return;
  }
  MT_Vector3 s;
  if (Vec3FromArray(isolate, args[0], s)) {
    obj->NodeSetWorldScale(s);
    return;
  }
  if (args.Length() >= 3) {
    Local<Context> ctx = isolate->GetCurrentContext();
    double x = args[0]->NumberValue(ctx).FromMaybe(1.0);
    double y = args[1]->NumberValue(ctx).FromMaybe(1.0);
    double z = args[2]->NumberValue(ctx).FromMaybe(1.0);
    obj->NodeSetWorldScale(MT_Vector3(MT_Scalar(x), MT_Scalar(y), MT_Scalar(z)));
  }
}

void KX_V8Bindings::GameObjectApplyForce(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Object> self = args.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj || args.Length() < 1) {
    return;
  }
  MT_Vector3 force;
  if (!Vec3FromArray(isolate, args[0], force)) {
    return;
  }
  bool local = (args.Length() > 1 && args[1]->BooleanValue(isolate));
  obj->ApplyForce(force, local);
}

void KX_V8Bindings::GameObjectGetVelocity(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Object> self = args.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj) {
    return;
  }
  MT_Vector3 point(0.0f, 0.0f, 0.0f);
  if (args.Length() >= 1 && Vec3FromArray(isolate, args[0], point)) {
    /* use provided point */
  }
  MT_Vector3 v = obj->GetVelocity(point);
  args.GetReturnValue().Set(ArrayFromVec3(isolate, v));
}

void KX_V8Bindings::GameObjectGetLinearVelocity(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Object> self = args.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj) {
    return;
  }
  bool local = (args.Length() > 0 && args[0]->BooleanValue(isolate));
  MT_Vector3 v = obj->GetLinearVelocity(local);
  args.GetReturnValue().Set(ArrayFromVec3(isolate, v));
}

void KX_V8Bindings::GameObjectSetLinearVelocity(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Object> self = args.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj || args.Length() < 1) {
    return;
  }
  MT_Vector3 v;
  if (!Vec3FromArray(isolate, args[0], v)) {
    return;
  }
  bool local = (args.Length() > 1 && args[1]->BooleanValue(isolate));
  obj->setLinearVelocity(v, local);
}

void KX_V8Bindings::GameObjectGetAngularVelocity(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Object> self = args.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj) {
    return;
  }
  bool local = (args.Length() > 0 && args[0]->BooleanValue(isolate));
  MT_Vector3 v = obj->GetAngularVelocity(local);
  args.GetReturnValue().Set(ArrayFromVec3(isolate, v));
}

void KX_V8Bindings::GameObjectSetAngularVelocity(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Object> self = args.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj || args.Length() < 1) {
    return;
  }
  MT_Vector3 v;
  if (!Vec3FromArray(isolate, args[0], v)) {
    return;
  }
  bool local = (args.Length() > 1 && args[1]->BooleanValue(isolate));
  obj->setAngularVelocity(v, local);
}

void KX_V8Bindings::GameObjectGetHasPhysics(Local<Name> property,
                                            const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Object> self = info.Holder();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  info.GetReturnValue().Set(Boolean::New(isolate, obj && obj->GetPhysicsController() != nullptr));
}

static void RayCastReturnNoHit(Isolate *isolate, const FunctionCallbackInfo<Value> &args)
{
  Local<Context> ctx = isolate->GetCurrentContext();
  Local<Object> out = Object::New(isolate);
  out->Set(ctx, String::NewFromUtf8Literal(isolate, "object"), Null(isolate)).Check();
  out->Set(ctx, String::NewFromUtf8Literal(isolate, "point"), Null(isolate)).Check();
  out->Set(ctx, String::NewFromUtf8Literal(isolate, "normal"), Null(isolate)).Check();
  args.GetReturnValue().Set(out);
}

void KX_V8Bindings::GameObjectRayCast(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = args.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj || args.Length() < 1) {
    RayCastReturnNoHit(isolate, args);
    return;
  }
  MT_Vector3 toPoint, fromPoint;
  if (!ResolveVectorOrGameObject(isolate, context, args[0], nullptr, toPoint)) {
    RayCastReturnNoHit(isolate, args);
    return;
  }
  if (!ResolveVectorOrGameObject(isolate, context, args.Length() > 1 ? args[1] : Local<Value>(),
                                obj, fromPoint)) {
    fromPoint = obj->NodeGetWorldPosition();
  }
  float dist = (args.Length() > 2) ? (float)args[2]->NumberValue(context).FromMaybe(0.0) : 0.0f;
  std::string propStr;
  if (args.Length() > 3 && args[3]->IsString()) {
    String::Utf8Value utf8(isolate, args[3]);
    if (*utf8) {
      propStr = *utf8;
    }
  }
  int face = (args.Length() > 4) ? (int)args[4]->IntegerValue(context).FromMaybe(0) : 0;
  int xray = (args.Length() > 5) ? (int)args[5]->IntegerValue(context).FromMaybe(0) : 0;
  unsigned int mask = (1u << 16) - 1;  /* OB_MAX_COL_MASKS */
  if (args.Length() > 6) {
    int m = (int)args[6]->IntegerValue(context).FromMaybe((int)mask);
    if (m > 0 && m <= (int)mask) {
      mask = (unsigned int)m;
    }
  }

  if (dist != 0.0f) {
    MT_Vector3 toDir = toPoint - fromPoint;
    if (MT_fuzzyZero(toDir)) {
      RayCastReturnNoHit(isolate, args);
      return;
    }
    toPoint = fromPoint + dist * toDir.safe_normalized();
  }
  else if (MT_fuzzyZero(toPoint - fromPoint)) {
    RayCastReturnNoHit(isolate, args);
    return;
  }

  PHY_IPhysicsEnvironment *pe = obj->GetScene()->GetPhysicsEnvironment();
  PHY_IPhysicsController *spc = obj->GetPhysicsController();
  KX_GameObject *parent = obj->GetParent();
  if (!spc && parent) {
    spc = parent->GetPhysicsController();
  }

  KX_GameObject::RayCastData rayData(propStr, (xray != 0), mask);
  KX_RayCast::Callback<KX_GameObject, KX_GameObject::RayCastData> callback(
      obj, spc, &rayData, (face != 0), false);

  if (KX_RayCast::RayTest(pe, fromPoint, toPoint, callback) && rayData.m_hitObject) {
    Local<Object> out = Object::New(isolate);
    out->Set(context, String::NewFromUtf8Literal(isolate, "object"),
             CreateGameObjectWrapper(isolate, rayData.m_hitObject)).Check();
    out->Set(context, String::NewFromUtf8Literal(isolate, "point"),
             ArrayFromVec3(isolate, callback.m_hitPoint)).Check();
    out->Set(context, String::NewFromUtf8Literal(isolate, "normal"),
             ArrayFromVec3(isolate, callback.m_hitNormal)).Check();
    args.GetReturnValue().Set(out);
  }
  else {
    RayCastReturnNoHit(isolate, args);
  }
}

void KX_V8Bindings::GameObjectRayCastTo(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = args.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj || args.Length() < 1) {
    RayCastReturnNoHit(isolate, args);
    return;
  }
  MT_Vector3 toPoint;
  if (!ResolveVectorOrGameObject(isolate, context, args[0], nullptr, toPoint)) {
    RayCastReturnNoHit(isolate, args);
    return;
  }
  MT_Vector3 fromPoint = obj->NodeGetWorldPosition();
  float dist = (args.Length() > 1) ? (float)args[1]->NumberValue(context).FromMaybe(0.0) : 0.0f;
  std::string propStr;
  if (args.Length() > 2 && args[2]->IsString()) {
    String::Utf8Value utf8(isolate, args[2]);
    if (*utf8) {
      propStr = *utf8;
    }
  }

  if (dist != 0.0f) {
    MT_Vector3 toDir = toPoint - fromPoint;
    if (!MT_fuzzyZero(toDir)) {
      toPoint = fromPoint + dist * toDir.safe_normalized();
    }
  }

  PHY_IPhysicsEnvironment *pe = obj->GetScene()->GetPhysicsEnvironment();
  PHY_IPhysicsController *spc = obj->GetPhysicsController();
  KX_GameObject *parent = obj->GetParent();
  if (!spc && parent) {
    spc = parent->GetPhysicsController();
  }

  KX_GameObject::RayCastData rayData(propStr, false, (1u << 16) - 1);
  KX_RayCast::Callback<KX_GameObject, KX_GameObject::RayCastData> callback(obj, spc, &rayData);

  if (KX_RayCast::RayTest(pe, fromPoint, toPoint, callback) && rayData.m_hitObject) {
    Local<Object> out = Object::New(isolate);
    out->Set(context, String::NewFromUtf8Literal(isolate, "object"),
             CreateGameObjectWrapper(isolate, rayData.m_hitObject)).Check();
    out->Set(context, String::NewFromUtf8Literal(isolate, "point"),
             ArrayFromVec3(isolate, callback.m_hitPoint)).Check();
    out->Set(context, String::NewFromUtf8Literal(isolate, "normal"),
             ArrayFromVec3(isolate, callback.m_hitNormal)).Check();
    args.GetReturnValue().Set(out);
  }
  else {
    RayCastReturnNoHit(isolate, args);
  }
}

void KX_V8Bindings::ControllerGetOwner(Local<Name> property,
                                       const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Object> self = info.Holder();
  SCA_IController *ctrl = GetControllerFromWrapper(self);
  if (!ctrl) {
    info.GetReturnValue().SetNull();
    return;
  }
  KX_GameObject *obj = static_cast<KX_GameObject *>(ctrl->GetParent());
  if (obj) {
    info.GetReturnValue().Set(CreateGameObjectWrapper(isolate, obj));
  }
  else {
    info.GetReturnValue().SetNull();
  }
}

void KX_V8Bindings::ControllerGetSensors(Local<Name> property,
                                         const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = info.Holder();
  SCA_IController *ctrl = GetControllerFromWrapper(self);
  if (!ctrl) {
    info.GetReturnValue().SetNull();
    return;
  }
  Local<Object> out = Object::New(isolate);
  for (SCA_ISensor *sensor : ctrl->GetLinkedSensors()) {
    std::string name = sensor->GetName();
    Local<String> key = String::NewFromUtf8(isolate, name.c_str()).ToLocalChecked();
    out->Set(context, key, CreateSensorWrapper(isolate, sensor)).Check();
  }
  info.GetReturnValue().Set(out);
}

void KX_V8Bindings::ControllerGetActuators(Local<Name> property,
                                           const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = info.Holder();
  SCA_IController *ctrl = GetControllerFromWrapper(self);
  if (!ctrl) {
    info.GetReturnValue().SetNull();
    return;
  }
  Local<Object> out = Object::New(isolate);
  for (SCA_IActuator *act : ctrl->GetLinkedActuators()) {
    std::string name = act->GetName();
    Local<String> key = String::NewFromUtf8(isolate, name.c_str()).ToLocalChecked();
    out->Set(context, key, CreateActuatorWrapper(isolate, act)).Check();
  }
  info.GetReturnValue().Set(out);
}

static SCA_IActuator *FindActuatorForActivate(SCA_IController *ctrl,
                                              Isolate *isolate,
                                              Local<Value> val)
{
  if (val.IsEmpty() || val->IsUndefined() || val->IsNull()) {
    return nullptr;
  }
  if (val->IsString()) {
    String::Utf8Value nameStr(isolate, val);
    if (!*nameStr) {
      return nullptr;
    }
    std::string name(*nameStr);
    for (SCA_IActuator *act : ctrl->GetLinkedActuators()) {
      if (act->GetName() == name) {
        return act;
      }
    }
    return nullptr;
  }
  if (val->IsObject()) {
    SCA_IActuator *act = KX_V8Bindings::GetActuatorFromWrapper(val.As<Object>());
    if (!act) {
      return nullptr;
    }
    for (SCA_IActuator *a : ctrl->GetLinkedActuators()) {
      if (a == act) {
        return act;
      }
    }
  }
  return nullptr;
}

void KX_V8Bindings::ControllerActivate(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Object> self = args.This();
  SCA_IController *ctrl = GetControllerFromWrapper(self);
  extern SCA_JavaScriptController *g_currentJavaScriptController;
  if (!ctrl || g_currentJavaScriptController != ctrl || args.Length() < 1) {
    return;
  }
  SCA_IActuator *act = FindActuatorForActivate(ctrl, isolate, args[0]);
  if (!act) {
    return;
  }
  SCA_LogicManager *lm = static_cast<SCA_ILogicBrick *>(ctrl)->GetLogicManager();
  if (lm) {
    lm->AddActiveActuator(act, true);
  }
}

void KX_V8Bindings::ControllerDeactivate(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Object> self = args.This();
  SCA_IController *ctrl = GetControllerFromWrapper(self);
  extern SCA_JavaScriptController *g_currentJavaScriptController;
  if (!ctrl || g_currentJavaScriptController != ctrl || args.Length() < 1) {
    return;
  }
  SCA_IActuator *act = FindActuatorForActivate(ctrl, isolate, args[0]);
  if (!act) {
    return;
  }
  SCA_LogicManager *lm = static_cast<SCA_ILogicBrick *>(ctrl)->GetLogicManager();
  if (lm) {
    lm->AddActiveActuator(act, false);
  }
}

void KX_V8Bindings::SensorGetPositive(Local<Name> property,
                                      const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Object> self = info.Holder();
  SCA_ISensor *sensor = GetSensorFromWrapper(self);
  if (sensor) {
    info.GetReturnValue().Set(Boolean::New(isolate, sensor->IsPositiveTrigger()));
  }
  else {
    info.GetReturnValue().Set(Boolean::New(isolate, false));
  }
}

void KX_V8Bindings::SensorGetEvents(Local<Name> property,
                                    const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = info.Holder();
  SCA_ISensor *sensor = GetSensorFromWrapper(self);
  SCA_KeyboardSensor *kb = dynamic_cast<SCA_KeyboardSensor *>(sensor);
  if (!kb) {
    info.GetReturnValue().Set(Array::New(isolate));
    return;
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  if (!engine) {
    info.GetReturnValue().Set(Array::New(isolate));
    return;
  }
  SCA_IInputDevice *inputdev = engine->GetInputDevice();
  if (!inputdev) {
    info.GetReturnValue().Set(Array::New(isolate));
    return;
  }
  Local<Array> arr = Array::New(isolate);
  uint32_t idx = 0;
  for (int i = SCA_IInputDevice::BEGINKEY; i <= SCA_IInputDevice::ENDKEY; i++) {
    SCA_InputEvent &input = inputdev->GetInput(
        static_cast<SCA_IInputDevice::SCA_EnumInputs>(i));
    int ev = SCA_InputEvent::NONE;
    if (!input.m_queue.empty()) {
      ev = input.m_queue.back();
    }
    else if (!input.m_status.empty()) {
      ev = input.m_status.back();
    }
    if (ev != SCA_InputEvent::NONE) {
      Local<Array> pair = Array::New(isolate, 2);
      pair->Set(context, 0, Integer::New(isolate, i)).Check();
      pair->Set(context, 1, Integer::New(isolate, ev)).Check();
      arr->Set(context, idx++, pair).Check();
    }
  }
  info.GetReturnValue().Set(arr);
}

void KX_V8Bindings::ActuatorGetName(Local<Name> property,
                                    const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Object> self = info.Holder();
  SCA_IActuator *act = GetActuatorFromWrapper(self);
  if (act) {
    info.GetReturnValue().Set(
        String::NewFromUtf8(isolate, act->GetName().c_str()).ToLocalChecked());
  }
  else {
    info.GetReturnValue().SetNull();
  }
}

/* --- Vehicle wrapper --- */
void KX_V8Bindings::VehicleAddWheel(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = args.This();
  PHY_IVehicle *v = GetVehicleFromWrapper(self);
  if (!v || args.Length() < 7) {
    return;
  }
  if (!args[0]->IsObject()) {
    return;
  }
  KX_GameObject *wheelOb = GetGameObjectFromWrapper(args[0].As<Object>());
  if (!wheelOb || !wheelOb->GetSGNode()) {
    return;
  }
  MT_Vector3 attachPos, attachDir, attachAxle;
  if (!Vec3FromArray(isolate, args[1], attachPos) || !Vec3FromArray(isolate, args[2], attachDir) ||
      !Vec3FromArray(isolate, args[3], attachAxle)) {
    return;
  }
  attachAxle = -attachAxle;  /* Bullet axle winding convention */
  float sus = (float)args[4]->NumberValue(context).FromMaybe(0.5f);
  float radius = (float)args[5]->NumberValue(context).FromMaybe(0.5f);
  bool hasSteering = args[6]->BooleanValue(isolate);
  if (radius <= 0.0f) {
    return;
  }
  PHY_IMotionState *ms = new KX_MotionState(wheelOb->GetSGNode());
  v->AddWheel(ms, attachPos, attachDir, attachAxle, sus, radius, hasSteering);
}

void KX_V8Bindings::VehicleGetNumWheels(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  PHY_IVehicle *v = GetVehicleFromWrapper(args.This().As<Object>());
  args.GetReturnValue().Set(Integer::New(isolate, v ? v->GetNumWheels() : 0));
}

void KX_V8Bindings::VehicleGetWheelPosition(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  PHY_IVehicle *v = GetVehicleFromWrapper(args.This().As<Object>());
  if (!v || args.Length() < 1) {
    return;
  }
  int i = (int)args[0]->IntegerValue(context).FromMaybe(0);
  if (i < 0 || i >= v->GetNumWheels()) {
    return;
  }
  args.GetReturnValue().Set(ArrayFromVec3(isolate, v->GetWheelPosition(i)));
}

void KX_V8Bindings::VehicleGetWheelRotation(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  PHY_IVehicle *v = GetVehicleFromWrapper(args.This().As<Object>());
  if (!v || args.Length() < 1) {
    return;
  }
  int i = (int)args[0]->IntegerValue(context).FromMaybe(0);
  if (i < 0 || i >= v->GetNumWheels()) {
    return;
  }
  args.GetReturnValue().Set(Number::New(isolate, v->GetWheelRotation(i)));
}

void KX_V8Bindings::VehicleGetWheelOrientationQuaternion(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  PHY_IVehicle *v = GetVehicleFromWrapper(args.This().As<Object>());
  if (!v || args.Length() < 1) {
    return;
  }
  int i = (int)args[0]->IntegerValue(context).FromMaybe(0);
  if (i < 0 || i >= v->GetNumWheels()) {
    return;
  }
  MT_Quaternion q = v->GetWheelOrientationQuaternion(i);
  Local<Array> arr = Array::New(isolate, 4);
  arr->Set(context, 0, Number::New(isolate, q[0])).Check();
  arr->Set(context, 1, Number::New(isolate, q[1])).Check();
  arr->Set(context, 2, Number::New(isolate, q[2])).Check();
  arr->Set(context, 3, Number::New(isolate, q[3])).Check();
  args.GetReturnValue().Set(arr);
}

static void VehicleWheelMethod2F(const FunctionCallbackInfo<Value> &args,
                                 void (PHY_IVehicle::*method)(float, int))
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  PHY_IVehicle *v = KX_V8Bindings::GetVehicleFromWrapper(args.This().As<Object>());
  if (!v || args.Length() < 2) {
    return;
  }
  float f = (float)args[0]->NumberValue(context).FromMaybe(0.0);
  int i = (int)args[1]->IntegerValue(context).FromMaybe(0);
  if (i >= 0 && i < v->GetNumWheels()) {
    (v->*method)(f, i);
  }
}

void KX_V8Bindings::VehicleSetSteeringValue(const FunctionCallbackInfo<Value> &args)
{
  VehicleWheelMethod2F(args, &PHY_IVehicle::SetSteeringValue);
}
void KX_V8Bindings::VehicleApplyEngineForce(const FunctionCallbackInfo<Value> &args)
{
  VehicleWheelMethod2F(args, &PHY_IVehicle::ApplyEngineForce);
}
void KX_V8Bindings::VehicleApplyBraking(const FunctionCallbackInfo<Value> &args)
{
  VehicleWheelMethod2F(args, &PHY_IVehicle::ApplyBraking);
}
void KX_V8Bindings::VehicleSetTyreFriction(const FunctionCallbackInfo<Value> &args)
{
  VehicleWheelMethod2F(args, &PHY_IVehicle::SetWheelFriction);
}
void KX_V8Bindings::VehicleSetSuspensionStiffness(const FunctionCallbackInfo<Value> &args)
{
  VehicleWheelMethod2F(args, &PHY_IVehicle::SetSuspensionStiffness);
}
void KX_V8Bindings::VehicleSetSuspensionDamping(const FunctionCallbackInfo<Value> &args)
{
  VehicleWheelMethod2F(args, &PHY_IVehicle::SetSuspensionDamping);
}
void KX_V8Bindings::VehicleSetSuspensionCompression(const FunctionCallbackInfo<Value> &args)
{
  VehicleWheelMethod2F(args, &PHY_IVehicle::SetSuspensionCompression);
}
void KX_V8Bindings::VehicleSetRollInfluence(const FunctionCallbackInfo<Value> &args)
{
  VehicleWheelMethod2F(args, &PHY_IVehicle::SetRollInfluence);
}

void KX_V8Bindings::VehicleGetConstraintId(Local<Name> property,
                                           const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_IVehicle *v = GetVehicleFromWrapper(info.Holder().As<Object>());
  info.GetReturnValue().Set(Integer::New(isolate, v ? v->GetUserConstraintId() : 0));
}
void KX_V8Bindings::VehicleGetConstraintType(Local<Name> property,
                                             const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_IVehicle *v = GetVehicleFromWrapper(info.Holder().As<Object>());
  info.GetReturnValue().Set(Integer::New(isolate, v ? v->GetUserConstraintType() : 0));
}
void KX_V8Bindings::VehicleGetRayMask(Local<Name> property,
                                      const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_IVehicle *v = GetVehicleFromWrapper(info.Holder().As<Object>());
  info.GetReturnValue().Set(Integer::New(isolate, v ? v->GetRayCastMask() : 0));
}
void KX_V8Bindings::VehicleSetRayMask(Local<Name> property,
                                      Local<Value> value,
                                      const PropertyCallbackInfo<void> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  PHY_IVehicle *v = GetVehicleFromWrapper(info.Holder().As<Object>());
  if (v && !value.IsEmpty()) {
    int m = (int)value->IntegerValue(context).FromMaybe(0);
    v->SetRayCastMask((short)m);
  }
}

/* --- Character wrapper --- */
void KX_V8Bindings::CharacterJump(const FunctionCallbackInfo<Value> &args)
{
  PHY_ICharacter *c = GetCharacterFromWrapper(args.This().As<Object>());
  if (c) {
    c->Jump();
  }
}
void KX_V8Bindings::CharacterSetVelocity(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  PHY_ICharacter *c = GetCharacterFromWrapper(args.This().As<Object>());
  if (!c || args.Length() < 1) {
    return;
  }
  MT_Vector3 vel;
  if (!Vec3FromArray(isolate, args[0], vel)) {
    return;
  }
  float time = (args.Length() > 1) ? (float)args[1]->NumberValue(context).FromMaybe(0.0f) : 0.0f;
  bool local = (args.Length() > 2) && args[2]->BooleanValue(isolate);
  c->SetVelocity(vel, time, local);
}
void KX_V8Bindings::CharacterReset(const FunctionCallbackInfo<Value> &args)
{
  PHY_ICharacter *c = GetCharacterFromWrapper(args.This().As<Object>());
  if (c) {
    c->Reset();
  }
}
void KX_V8Bindings::CharacterGetOnGround(Local<Name> property,
                                         const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  info.GetReturnValue().Set(Boolean::New(isolate, c && c->OnGround()));
}
void KX_V8Bindings::CharacterGetGravity(Local<Name> property,
                                        const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  if (c) {
    info.GetReturnValue().Set(ArrayFromVec3(isolate, c->GetGravity()));
  }
  else {
    info.GetReturnValue().SetNull();
  }
}
void KX_V8Bindings::CharacterSetGravity(Local<Name> property,
                                        Local<Value> value,
                                        const PropertyCallbackInfo<void> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  if (c && !value.IsEmpty()) {
    MT_Vector3 g;
    if (Vec3FromArray(isolate, value, g)) {
      c->SetGravity(g);
    }
  }
}
void KX_V8Bindings::CharacterGetFallSpeed(Local<Name> property,
                                          const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  info.GetReturnValue().Set(Number::New(isolate, c ? c->GetFallSpeed() : 0.0f));
}
void KX_V8Bindings::CharacterSetFallSpeed(Local<Name> property,
                                          Local<Value> value,
                                          const PropertyCallbackInfo<void> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  if (c && !value.IsEmpty()) {
    float f = (float)value->NumberValue(context).FromMaybe(0.0);
    if (f >= 0.0f) {
      c->SetFallSpeed(f);
    }
  }
}
void KX_V8Bindings::CharacterGetMaxJumps(Local<Name> property,
                                         const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  info.GetReturnValue().Set(Integer::New(isolate, c ? c->GetMaxJumps() : 0));
}
void KX_V8Bindings::CharacterSetMaxJumps(Local<Name> property,
                                         Local<Value> value,
                                         const PropertyCallbackInfo<void> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  if (c && !value.IsEmpty()) {
    int i = (int)value->IntegerValue(context).FromMaybe(0);
    if (i >= 0) {
      c->SetMaxJumps((unsigned char)i);
    }
  }
}
void KX_V8Bindings::CharacterGetMaxSlope(Local<Name> property,
                                         const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  info.GetReturnValue().Set(Number::New(isolate, c ? c->GetMaxSlope() : 0.0f));
}
void KX_V8Bindings::CharacterSetMaxSlope(Local<Name> property,
                                         Local<Value> value,
                                         const PropertyCallbackInfo<void> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  if (c && !value.IsEmpty()) {
    float f = (float)value->NumberValue(context).FromMaybe(0.0);
    c->SetMaxSlope(f);
  }
}
void KX_V8Bindings::CharacterGetJumpCount(Local<Name> property,
                                          const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  info.GetReturnValue().Set(Integer::New(isolate, c ? c->GetJumpCount() : 0));
}
void KX_V8Bindings::CharacterGetJumpSpeed(Local<Name> property,
                                          const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  info.GetReturnValue().Set(Number::New(isolate, c ? c->GetJumpSpeed() : 0.0f));
}
void KX_V8Bindings::CharacterSetJumpSpeed(Local<Name> property,
                                          Local<Value> value,
                                          const PropertyCallbackInfo<void> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  if (c && !value.IsEmpty()) {
    float f = (float)value->NumberValue(context).FromMaybe(0.0);
    c->SetJumpSpeed(f);
  }
}
void KX_V8Bindings::CharacterGetWalkDirection(Local<Name> property,
                                              const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  if (c) {
    info.GetReturnValue().Set(ArrayFromVec3(isolate, c->GetWalkDirection()));
  }
  else {
    info.GetReturnValue().SetNull();
  }
}
void KX_V8Bindings::CharacterSetWalkDirection(Local<Name> property,
                                              Local<Value> value,
                                              const PropertyCallbackInfo<void> &info)
{
  Isolate *isolate = info.GetIsolate();
  PHY_ICharacter *c = GetCharacterFromWrapper(info.Holder().As<Object>());
  if (c && !value.IsEmpty()) {
    MT_Vector3 d;
    if (Vec3FromArray(isolate, value, d)) {
      c->SetWalkDirection(d);
    }
  }
}

#endif  // WITH_JAVASCRIPT
