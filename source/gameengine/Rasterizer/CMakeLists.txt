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
	Node
	RAS_OpenGLRasterizer
	../Common
	../Expressions
	../Ketsji
	../Physics/Common
	../SceneGraph
	../GameLogic
	../../blender/makesdna
	../../blender/blenfont
	../../blender/blenlib
	../../blender/blenkernel
	../../blender/gpu
	../../blender/imbuf
	../../../intern/glew-mx
	../../../intern/guardedalloc
	../../../intern/termcolor
)

set(INC_SYS
	../../../intern/debugbreak
	../../../intern/mathfu
	${GLEW_INCLUDE_PATH}
	${PYTHON_INCLUDE_DIRS}
	${BOOST_INCLUDE_DIR}
)

set(SRC
	RAS_2DFilter.cpp
	RAS_2DFilterData.cpp
	RAS_2DFilterManager.cpp
	RAS_2DFilterOffScreen.cpp
	RAS_AttributeArray.cpp
	RAS_AttributeArrayStorage.cpp
	RAS_BatchDisplayArray.cpp
	RAS_BatchGroup.cpp
	RAS_BoundingBox.cpp
	RAS_BoundingBoxManager.cpp
	RAS_BucketManager.cpp
	RAS_DebugDraw.cpp
	RAS_Deformer.cpp
	RAS_DisplayArray.cpp
	RAS_DisplayArrayBucket.cpp
	RAS_DisplayArrayStorage.cpp
	RAS_FramingManager.cpp
	RAS_ICanvas.cpp
	RAS_InstancingBuffer.cpp
	RAS_IPolygonMaterial.cpp
	RAS_Rasterizer.cpp
	RAS_MaterialBucket.cpp
	RAS_MeshMaterial.cpp
	RAS_Mesh.cpp
	RAS_MeshSlot.cpp
	RAS_MeshUser.cpp
	RAS_OffScreen.cpp
	RAS_Query.cpp
	RAS_Shader.cpp
	RAS_SortedMeshSlot.cpp
	RAS_Texture.cpp
	RAS_TextureRenderer.cpp
	RAS_TextUser.cpp
	RAS_VertexInfo.cpp

	RAS_2DFilterData.h
	RAS_2DFilter.h
	RAS_2DFilterManager.h
	RAS_2DFilterOffScreen.h
	RAS_AttributeArray.h
	RAS_AttributeArrayStorage.h
	RAS_BatchDisplayArray.h
	RAS_BatchGroup.h
	RAS_BoundingBox.h
	RAS_BoundingBoxManager.h
	RAS_BucketManager.h
	RAS_CameraData.h
	RAS_DebugDraw.h
	RAS_Deformer.h
	RAS_DisplayArray.h
	RAS_DisplayArrayBucket.h
	RAS_DisplayArrayStorage.h
	RAS_FramingManager.h
	RAS_ICanvas.h
	RAS_IPolygonMaterial.h
	RAS_Rasterizer.h
	RAS_ILightObject.h
	RAS_InstancingBuffer.h
	RAS_ISync.h
	RAS_MaterialBucket.h
	RAS_MeshMaterial.h
	RAS_Mesh.h
	RAS_MeshSlot.h
	RAS_MeshUser.h
	RAS_OffScreen.h
	RAS_Query.h
	RAS_Rect.h
	RAS_Shader.h
	RAS_SortedMeshSlot.h
	RAS_Texture.h
	RAS_TextureRenderer.h
	RAS_TextUser.h
	RAS_VertexInfo.h
)

data_to_c_simple(RAS_OpenGLFilters/RAS_Blur2DFilter.glsl SRC)
data_to_c_simple(RAS_OpenGLFilters/RAS_Dilation2DFilter.glsl SRC)
data_to_c_simple(RAS_OpenGLFilters/RAS_Erosion2DFilter.glsl SRC)
data_to_c_simple(RAS_OpenGLFilters/RAS_GrayScale2DFilter.glsl SRC)
data_to_c_simple(RAS_OpenGLFilters/RAS_Invert2DFilter.glsl SRC)
data_to_c_simple(RAS_OpenGLFilters/RAS_Laplacian2DFilter.glsl SRC)
data_to_c_simple(RAS_OpenGLFilters/RAS_Prewitt2DFilter.glsl SRC)
data_to_c_simple(RAS_OpenGLFilters/RAS_Sepia2DFilter.glsl SRC)
data_to_c_simple(RAS_OpenGLFilters/RAS_Sharpen2DFilter.glsl SRC)
data_to_c_simple(RAS_OpenGLFilters/RAS_Sobel2DFilter.glsl SRC)
data_to_c_simple(RAS_OpenGLFilters/RAS_VertexShader2DFilter.glsl SRC)

add_definitions(${GL_DEFINITIONS})

# ----------------------------------------------------------------------------------------------------------------
# workaround to increase the number of sections for object file format in MSVC 2015 compiler when we DEBUG
if(MSVC14)
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /bigobj /Zi /Od /Ob0")
endif()

blender_add_lib(ge_rasterizer "${SRC}" "${INC}" "${INC_SYS}")
