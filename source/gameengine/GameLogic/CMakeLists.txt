# ***** BEGIN GPL LICENSE BLOCK *****
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# The Original Code is Copyright (C) 2006, Blender Foundation
# All rights reserved.
#
# The Original Code is: all of this file.
#
# Contributor(s): Jacques Beaurain.
#
# ***** END GPL LICENSE BLOCK *****

set(INC
  .
  ../Common
  ../Converter
  ../Device
  ../Expressions
  ../Ketsji
  ../Ketsji/KXNetwork
  ../Physics/Common
  ../Rasterizer
  ../SceneGraph
  ../../blender/blenkernel
  ../../blender/python/generic
  ../../../intern/termcolor
  ../../../intern/ghost
)

set(INC_SYS
  ../../../intern/moto/include
        ../../../extern/recastnavigation/Recast/Include
        ../../../extern/recastnavigation/Detour/Include
  ${BOOST_INCLUDE_DIR}
)

set(SRC
  SCA_2DFilterActuator.cpp
  SCA_ActionActuator.cpp
  SCA_AddObjectActuator.cpp
  SCA_ANDController.cpp
  SCA_ActuatorEventManager.cpp
  SCA_ActuatorSensor.cpp
  SCA_AlwaysSensor.cpp
  SCA_ArmatureSensor.cpp
  SCA_BasicEventManager.cpp
  SCA_CameraActuator.cpp
  SCA_CollectionActuator.cpp
  SCA_CollisionSensor.cpp
  SCA_ConstraintActuator.cpp
  SCA_DelaySensor.cpp
  SCA_DynamicActuator.cpp
  SCA_EndObjectActuator.cpp
  SCA_EventManager.cpp
  SCA_ExpressionController.cpp
  SCA_GameActuator.cpp
  SCA_IActuator.cpp
  SCA_IController.cpp
  SCA_IInputDevice.cpp
  SCA_ILogicBrick.cpp
  SCA_InputEvent.cpp
  SCA_IObject.cpp
  SCA_IScene.cpp
  SCA_ISensor.cpp
  SCA_JoystickManager.cpp
  SCA_JoystickSensor.cpp
  SCA_KeyboardManager.cpp
  SCA_KeyboardSensor.cpp
  SCA_LogicManager.cpp
  SCA_MouseActuator.cpp
  SCA_MouseFocusSensor.cpp
  SCA_MouseManager.cpp
  SCA_MouseSensor.cpp
  SCA_MovementSensor.cpp
  SCA_NANDController.cpp
  SCA_NearSensor.cpp
  SCA_NetworkMessageActuator.cpp
  SCA_NetworkMessageSensor.cpp
  SCA_NORController.cpp
  SCA_ObjectActuator.cpp
  SCA_ORController.cpp
  SCA_ParentActuator.cpp
  SCA_PropertyActuator.cpp
  SCA_PropertySensor.cpp
  SCA_PythonController.cpp
  SCA_PythonJoystick.cpp
  SCA_PythonKeyboard.cpp
  SCA_PythonMouse.cpp
  SCA_RadarSensor.cpp
  SCA_RandomActuator.cpp
  SCA_RandomNumberGenerator.cpp
  SCA_RandomSensor.cpp
  SCA_RaySensor.cpp
  SCA_ReplaceMeshActuator.cpp
  SCA_SceneActuator.cpp
  SCA_SoundActuator.cpp
  SCA_StateActuator.cpp
  SCA_SteeringActuator.cpp
  SCA_TimeEventManager.cpp
  SCA_TrackToActuator.cpp
  SCA_VibrationActuator.cpp
  SCA_VisibilityActuator.cpp
  SCA_XNORController.cpp
  SCA_XORController.cpp

  SCA_2DFilterActuator.h
  SCA_ActionActuator.h
  SCA_AddObjectActuator.h
  SCA_ANDController.h
  SCA_ActuatorEventManager.h
  SCA_ActuatorSensor.h
  SCA_AlwaysSensor.h
  SCA_ArmatureSensor.h
  SCA_BasicEventManager.h
  SCA_CameraActuator.h
  SCA_CollectionActuator.h
  SCA_CollisionSensor.h
  SCA_ConstraintActuator.h
  SCA_DelaySensor.h
  SCA_DynamicActuator.h
  SCA_EndObjectActuator.h
  SCA_EventManager.h
  SCA_ExpressionController.h
  SCA_GameActuator.h
  SCA_IActuator.h
  SCA_IController.h
  SCA_IInputDevice.h
  SCA_ILogicBrick.h
  SCA_InputEvent.h
  SCA_IObject.h
  SCA_IScene.h
  SCA_ISensor.h
  SCA_JoystickManager.h
  SCA_JoystickSensor.h
  SCA_KeyboardManager.h
  SCA_KeyboardSensor.h
  SCA_LogicManager.h
  SCA_MouseActuator.h
  SCA_MouseFocusSensor.h
  SCA_MouseManager.h
  SCA_MouseSensor.h
  SCA_MovementSensor.h
  SCA_NANDController.h
  SCA_NearSensor.h
  SCA_NetworkMessageActuator.h
  SCA_NetworkMessageSensor.h
  SCA_NORController.h
  SCA_ObjectActuator.h
  SCA_ORController.h
  SCA_ParentActuator.h
  SCA_PropertyActuator.h
  SCA_PropertySensor.h
  SCA_PythonController.h
  SCA_PythonJoystick.h
  SCA_PythonKeyboard.h
  SCA_PythonMouse.h
  SCA_RadarSensor.h
  SCA_RandomActuator.h
  SCA_RandomNumberGenerator.h
  SCA_RandomSensor.h
  SCA_RaySensor.h
  SCA_ReplaceMeshActuator.h
  SCA_SceneActuator.h
  SCA_SoundActuator.h
  SCA_StateActuator.h
  SCA_SteeringActuator.h
  SCA_TimeEventManager.h
  SCA_TrackToActuator.h
  SCA_VibrationActuator.h
  SCA_VisibilityActuator.h
  SCA_XNORController.h
  SCA_XORController.h
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::dna
  PRIVATE bf::intern::guardedalloc
  ge_expressions
)

if(WITH_AUDASPACE)
  list(APPEND INC_SYS
    ${AUDASPACE_C_INCLUDE_DIRS}
    ${AUDASPACE_PY_INCLUDE_DIRS}
  )
  list(APPEND LIB
    ${AUDASPACE_C_LIBRARIES}
    ${AUDASPACE_PY_LIBRARIES}
  )
  add_definitions(-DWITH_AUDASPACE)
endif()

if(WITH_SDL)
  list(APPEND INC_SYS
    ${SDL_INCLUDE_DIR}
  )
  add_definitions(-DWITH_SDL)

  if(WITH_GHOST_SDL)
    add_definitions(-DWITH_GHOST_SDL)
  endif()
endif()

blender_add_lib(ge_logic_bricks "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
