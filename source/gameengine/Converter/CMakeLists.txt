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
	../BlenderRoutines
	../Common
	../Device
	../Expressions
	../GameLogic
	../Ketsji
	../Ketsji/KXNetwork
	../Launcher
	../Physics/Bullet
	../Physics/Dummy
	../Physics/Common
	../Rasterizer
	../Rasterizer/Node
	../Rasterizer/RAS_OpenGLRasterizer
	../SceneGraph
	../VideoTexture
	../../blender
	../../blender/blenkernel
	../../blender/blenlib
	../../blender/blenloader
	../../blender/gpu
	../../blender/ikplugin
	../../blender/imbuf
	../../blender/makesdna
	../../blender/makesrna
	../../blender/windowmanager
	../../../intern/guardedalloc
	../../../intern/termcolor
)

set(INC_SYS
	../../../intern/mathfu
	../../../extern/recastnavigation/Detour/Include
	${EIGEN3_INCLUDE_DIRS}
	${PTHREADS_INCLUDE_DIRS}
	${BOOST_INCLUDE_DIR}
)

set(SRC
	BL_ActionActuator.cpp
	BL_ArmatureActuator.cpp
	BL_ArmatureChannel.cpp
	BL_ArmatureConstraint.cpp
	BL_ArmatureObject.cpp
	BL_BlenderDataConversion.cpp
	BL_DeformableGameObject.cpp
	BL_MeshDeformer.cpp
	BL_ModifierDeformer.cpp
	BL_ShapeDeformer.cpp
	BL_SkinDeformer.cpp
	BL_BlenderConverter.cpp
	BL_BlenderScalarInterpolator.cpp
	BL_BlenderSceneConverter.cpp
	BL_ConvertActuators.cpp
	BL_ConvertControllers.cpp
	BL_ConvertProperties.cpp
	BL_ConvertSensors.cpp
	BL_IpoConvert.cpp

	BL_ActionActuator.h
	BL_ArmatureActuator.h
	BL_ArmatureChannel.h
	BL_ArmatureConstraint.h
	BL_ArmatureObject.h
	BL_BlenderDataConversion.h
	BL_DeformableGameObject.h
	BL_MeshDeformer.h
	BL_ModifierDeformer.h
	BL_ShapeDeformer.h
	BL_SkinDeformer.h
	BL_BlenderConverter.h
	BL_BlenderScalarInterpolator.h
	BL_BlenderSceneConverter.h
	BL_ConvertActuators.h
	BL_ConvertControllers.h
	BL_ConvertProperties.h
	BL_ConvertSensors.h
	BL_IpoConvert.h
)

if(WITH_BULLET)
	list(APPEND INC_SYS
		${BULLET_INCLUDE_DIRS}
	)
	add_definitions(-DWITH_BULLET)
endif()

if(WITH_AUDASPACE)
	add_definitions(${AUDASPACE_DEFINITIONS})

	list(APPEND INC_SYS
		${AUDASPACE_C_INCLUDE_DIRS}
	)
endif()

blender_add_lib(ge_converter "${SRC}" "${INC}" "${INC_SYS}")
