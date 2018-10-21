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
	..
	../Node
	# XXX Remove these <<<
	../../BlenderRoutines
	../../Common
	../../Converter
	../../Expressions
	../../GameLogic
	../../Ketsji
	../../Physics/Common
	# >>>
	../../SceneGraph
	../../../blender/blenfont
	../../../blender/blenkernel
	../../../blender/blenlib
	../../../blender/gpu
	../../../blender/makesdna
	../../../../intern/glew-mx
	../../../../intern/guardedalloc
)

set(INC_SYS
	../../../intern/debugbreak
	../../../../intern/mathfu
	${GLEW_INCLUDE_PATH}
	${BOOST_INCLUDE_DIR}
)

set(SRC
	RAS_OpenGLDebugDraw.cpp
	RAS_OpenGLLight.cpp
	RAS_OpenGLPixelBuffer.cpp
	RAS_OpenGLQuery.cpp
	RAS_OpenGLSync.cpp
	RAS_OpenGLRasterizer.cpp
	RAS_StorageVao.cpp
	RAS_StorageVbo.cpp

	RAS_OpenGLDebugDraw.h
	RAS_OpenGLLight.h
	RAS_OpenGLPixelBuffer.h
	RAS_OpenGLQuery.h
	RAS_OpenGLSync.h
	RAS_OpenGLRasterizer.h
	RAS_StorageVao.h
	RAS_StorageVbo.h
)

add_definitions(${GL_DEFINITIONS})

blender_add_lib(ge_oglrasterizer "${SRC}" "${INC}" "${INC_SYS}")
