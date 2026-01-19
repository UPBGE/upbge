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

#include "KX_V8Bindings.h"

#include "KX_GameObject.h"
#include "KX_Scene.h"
#include "SCA_IController.h"
#include "SCA_ISensor.h"
#include "SCA_IActuator.h"
#include "SCA_JavaScriptController.h"
#include "MT_Vector3.h"
#include "MT_Matrix3x3.h"

#include <v8.h>

using namespace v8;

void KX_V8Bindings::InitializeBindings(Local<Context> context)
{
  Isolate *isolate = context->GetIsolate();
  Context::Scope context_scope(context);

  SetupBGENamespace(context);
  SetupLogicObject(context);
}

void KX_V8Bindings::SetupBGENamespace(Local<Context> context)
{
  Isolate *isolate = context->GetIsolate();
  Local<Object> global = context->Global();

  Local<ObjectTemplate> bge_template = ObjectTemplate::New(isolate);
  Local<Object> bge_obj = bge_template->NewInstance(context).ToLocalChecked();

  global->Set(context, String::NewFromUtf8Literal(isolate, "bge"), bge_obj).Check();
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

  // Add property accessors
  obj_template->SetAccessor(String::NewFromUtf8Literal(isolate, "name"), GameObjectGetName);
  obj_template->SetAccessor(String::NewFromUtf8Literal(isolate, "position"), GameObjectGetPosition);
  obj_template->SetAccessor(String::NewFromUtf8Literal(isolate, "rotation"), GameObjectGetRotation);
  obj_template->SetAccessor(String::NewFromUtf8Literal(isolate, "scale"), GameObjectGetScale);

  Local<Object> wrapper = obj_template->NewInstance(context).ToLocalChecked();
  wrapper->SetInternalField(0, External::New(isolate, obj));

  return handle_scope.Escape(wrapper);
}

Local<Object> KX_V8Bindings::CreateSceneWrapper(Isolate *isolate, KX_Scene *scene)
{
  EscapableHandleScope handle_scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();

  Local<ObjectTemplate> scene_template = ObjectTemplate::New(isolate);
  scene_template->SetInternalFieldCount(1);

  Local<Object> wrapper = scene_template->NewInstance(context).ToLocalChecked();
  wrapper->SetInternalField(0, External::New(isolate, scene));

  return handle_scope.Escape(wrapper);
}

Local<Object> KX_V8Bindings::CreateControllerWrapper(Isolate *isolate, SCA_IController *controller)
{
  EscapableHandleScope handle_scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();

  Local<ObjectTemplate> controller_template = ObjectTemplate::New(isolate);
  controller_template->SetInternalFieldCount(1);

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

  Local<Object> wrapper = actuator_template->NewInstance(context).ToLocalChecked();
  wrapper->SetInternalField(0, External::New(isolate, actuator));

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

void KX_V8Bindings::GameObjectGetName(Local<String> property,
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

void KX_V8Bindings::GameObjectGetPosition(Local<String> property,
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

void KX_V8Bindings::GameObjectGetRotation(Local<String> property,
                                          const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> self = info.Holder();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  
  if (obj) {
    MT_Matrix3x3 rot = obj->NodeGetWorldOrientation();
    // Return as Euler angles (simplified - could be improved)
    MT_Vector3 euler = rot.to_euler();
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

void KX_V8Bindings::GameObjectGetScale(Local<String> property,
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

#endif  // WITH_JAVASCRIPT
