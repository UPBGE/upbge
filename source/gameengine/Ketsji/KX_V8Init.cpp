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

/** \file KX_V8Init.cpp
 *  \ingroup ketsji
 */

#ifdef WITH_JAVASCRIPT

#  include "v8_include.h"

#  include "KX_V8Init.h"
#  include "KX_V8Engine.h"
#  include "KX_V8Bindings.h"
#  include "KX_KetsjiEngine.h"
#  include "CM_Message.h"
#  include "BKE_main.hh"

using namespace blender;

void initV8Engine()
{
  if (!KX_V8Engine::Initialize()) {
    CM_Error("Failed to initialize V8 JavaScript engine");
  }
}

void setupGameJavaScript(KX_KetsjiEngine *ketsjiengine, Main *blenderdata)
{
  KX_V8Engine *engine = KX_V8Engine::GetInstance();
  if (!engine) {
    CM_Error("V8 engine not initialized");
    return;
  }

  // GetDefaultContext().Get() and InitializeBindings create handles; need a HandleScope.
  v8::HandleScope handle_scope(engine->GetIsolate());
  auto context = engine->GetDefaultContext();
  KX_V8Bindings::InitializeBindings(context);
}

void exitGameJavaScript()
{
  KX_V8Engine::Shutdown();
}

#endif  // WITH_JAVASCRIPT
