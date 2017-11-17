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
	KXNetwork
	../BlenderRoutines
	../Common
	../Converter
	../Device
	../Expressions
	../GameLogic
	../Launcher
	../Physics/Common
	../Rasterizer
	../Rasterizer/Node
	../Rasterizer/RAS_OpenGLRasterizer
	../SceneGraph
	../VideoTexture
	../../blender
	../../blender/blenfont
	../../blender/blenkernel
	../../blender/blenlib
	../../blender/blenloader
	../../blender/gpu
	../../blender/imbuf
	../../blender/makesdna
	../../blender/makesrna
	../../blender/python
	../../blender/python/generic
	../../blender/python/mathutils
	../../blender/python/intern
	../../../intern/glew-mx
	../../../intern/guardedalloc
)

set(INC_SYS
	../../../intern/debugbreak
	../../../intern/mathfu
	../../../extern/recastnavigation/Recast/Include
	../../../extern/recastnavigation/Detour/Include
	${PTHREADS_INCLUDE_DIRS}
	${GLEW_INCLUDE_PATH}
	${BOOST_INCLUDE_DIR}
)

set(SRC
	BL_Action.cpp
	BL_ActionManager.cpp
	BL_BlenderShader.cpp
	BL_Shader.cpp
	BL_Texture.cpp
	KX_2DFilter.cpp
	KX_2DFilterManager.cpp
	KX_2DFilterOffScreen.cpp
	KX_ArmatureSensor.cpp
	KX_BatchGroup.cpp
	KX_BlenderMaterial.cpp
	KX_BoundingBox.cpp
	KX_Camera.cpp
	KX_CameraActuator.cpp
	KX_CameraIpoSGController.cpp
	KX_CharacterWrapper.cpp
	KX_CollisionEventManager.cpp
	KX_CollisionSensor.cpp
	KX_ConstraintActuator.cpp
	KX_ConstraintWrapper.cpp
	KX_CubeMap.cpp
	KX_CullingHandler.cpp
	KX_EmptyObject.cpp
	KX_FontObject.cpp
	KX_GameActuator.cpp
	KX_GameObject.cpp
	KX_Globals.cpp
	KX_IpoController.cpp
	KX_KetsjiEngine.cpp
	KX_LibLoadStatus.cpp
	KX_LightObject.cpp
	KX_LightIpoSGController.cpp
	KX_LodLevel.cpp
	KX_LodManager.cpp
	KX_MaterialIpoController.cpp
	KX_MeshBuilder.cpp
	KX_Mesh.cpp
	KX_MotionState.cpp
	KX_MouseActuator.cpp
	KX_MouseFocusSensor.cpp
	KX_MovementSensor.cpp
	KX_NavMeshObject.cpp
	KX_NearSensor.cpp
	KX_ObColorIpoSGController.cpp
	KX_ObjectActuator.cpp
	KX_ObstacleSimulation.cpp
	KX_ParentActuator.cpp
	KX_PlanarMap.cpp
	KX_PolyProxy.cpp
	KX_PyConstraintBinding.cpp
	KX_PyMath.cpp
	KX_PythonComponent.cpp
	KX_PythonComponentManager.cpp
	KX_PythonInit.cpp
	KX_PythonInitTypes.cpp
	KX_PythonMain.cpp
	KX_RadarSensor.cpp
	KX_RayCast.cpp
	KX_RaySensor.cpp
	KX_AddObjectActuator.cpp
	KX_DynamicActuator.cpp
	KX_EndObjectActuator.cpp
	KX_ReplaceMeshActuator.cpp
	KX_BoneParentNodeRelationship.cpp
	KX_NodeRelationships.cpp
	KX_Scene.cpp
	KX_SceneActuator.cpp
	KX_SoftBodyDeformer.cpp
	KX_SoundActuator.cpp
	KX_StateActuator.cpp
	KX_SteeringActuator.cpp
	KX_TextMaterial.cpp
	KX_TextureRenderer.cpp
	KX_TextureRendererManager.cpp
	KX_TimeCategoryLogger.cpp
	KX_TimeLogger.cpp
	KX_TrackToActuator.cpp
	KX_VehicleWrapper.cpp
	KX_VertexProxy.cpp
	KX_VisibilityActuator.cpp
	KX_WorldInfo.cpp
	KX_WorldIpoController.cpp
	KX_CollisionContactPoints.cpp

	BL_Action.h
	BL_ActionManager.h
	BL_BlenderShader.h
	BL_Shader.h
	BL_Texture.h
	KX_2DFilter.h
	KX_2DFilterManager.h
	KX_2DFilterOffScreen.h
	KX_ArmatureSensor.h
	KX_BatchGroup.h
	KX_BlenderMaterial.h
	KX_BoundingBox.h
	KX_Camera.h
	KX_CameraActuator.h
	KX_CameraIpoSGController.h
	KX_CharacterWrapper.h
	KX_ClientObjectInfo.h
	KX_ConstraintActuator.h
	KX_ConstraintWrapper.h
	KX_CubeMap.h
	KX_CullingHandler.h
	KX_EmptyObject.h
	KX_FontObject.h
	KX_GameActuator.h
	KX_GameObject.h
	KX_Globals.h
	KX_IpoTransform.h
	KX_IpoController.h
	KX_KetsjiEngine.h
	KX_LibLoadStatus.h
	KX_LightObject.h
	KX_LightIpoSGController.h
	KX_LodLevel.h
	KX_LodManager.h
	KX_MaterialIpoController.h
	KX_MeshBuilder.h
	KX_Mesh.h
	KX_MotionState.h
	KX_MouseActuator.h
	KX_MouseFocusSensor.h
	KX_MovementSensor.h
	KX_NavMeshObject.h
	KX_NearSensor.h
	KX_ObColorIpoSGController.h
	KX_ObjectActuator.h
	KX_ObstacleSimulation.h
	KX_ParentActuator.h
	KX_PhysicsEngineEnums.h
	KX_PlanarMap.h
	KX_PolyProxy.h
	KX_PyConstraintBinding.h
	KX_PyMath.h
	KX_PythonComponent.h
	KX_PythonComponentManager.h
	KX_PythonInit.h
	KX_PythonInitTypes.h
	KX_PythonMain.h
	KX_RadarSensor.h
	KX_RayCast.h
	KX_RaySensor.h
	KX_AddObjectActuator.h
	KX_DynamicActuator.h
	KX_EndObjectActuator.h
	KX_ReplaceMeshActuator.h
	KX_BoneParentNodeRelationship.h
	KX_NodeRelationships.h
	KX_Scene.h
	KX_SceneActuator.h
	KX_SoftBodyDeformer.h
	KX_SoundActuator.h
	KX_StateActuator.h
	KX_SteeringActuator.h
	KX_TextMaterial.h
	KX_TextureRenderer.h
	KX_TextureRendererManager.h
	KX_TimeCategoryLogger.h
	KX_TimeLogger.h
	KX_CollisionEventManager.h
	KX_CollisionSensor.h
	KX_TrackToActuator.h
	KX_VehicleWrapper.h
	KX_VertexProxy.h
	KX_VisibilityActuator.h
	KX_WorldInfo.h
	KX_WorldIpoController.h
	KX_CollisionContactPoints.h
)

add_definitions(${GL_DEFINITIONS})

if(WITH_IMAGE_DDS)
	add_definitions(-DWITH_DDS)
endif()

if(WITH_SDL)
	list(APPEND INC_SYS
		${SDL_INCLUDE_DIR}
	)
	add_definitions(-DWITH_SDL)
endif()

if(WITH_CODEC_FFMPEG)
	add_definitions(-DWITH_FFMPEG)
endif()

if(WITH_AUDASPACE)
	add_definitions(${AUDASPACE_DEFINITIONS})

	list(APPEND INC_SYS
		${AUDASPACE_C_INCLUDE_DIRS}
		${AUDASPACE_PY_INCLUDE_DIRS}
	)
endif()

if(WITH_BULLET)
	list(APPEND INC
		../Physics/Bullet
	)
	list(APPEND INC_SYS
		${BULLET_INCLUDE_DIRS}
	)
	add_definitions(-DWITH_BULLET)
endif()

blender_add_lib(ge_logic_ketsji "${SRC}" "${INC}" "${INC_SYS}")
