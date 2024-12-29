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
  RAS_OpenGLRasterizer
  ../Common
  ../Expressions
  ../Ketsji
  ../Physics/Common
  ../SceneGraph
  ../GameLogic
  ../../blender/blenkernel
  ../../blender/draw/intern
  ../../blender/makesrna
  ../../../intern/termcolor
)

set(INC_SYS
  ../../../intern/moto/include
  ${Epoxy_INCLUDE_DIRS}
  ${BOOST_INCLUDE_DIR}
)

if(WITH_PYTHON)
  list(APPEND INC_SYS
    ${PYTHON_INCLUDE_DIRS}
  )
endif()

set(SRC
  RAS_2DFilter.cpp
  RAS_2DFilterData.cpp
  RAS_2DFilterManager.cpp
  RAS_2DFilterFrameBuffer.cpp
  RAS_BucketManager.cpp
  RAS_DebugDraw.cpp
  RAS_DisplayArrayBucket.cpp
  RAS_FrameBuffer.cpp
  RAS_FramingManager.cpp
  RAS_ICanvas.cpp
  RAS_IDisplayArray.cpp
  RAS_IPolygonMaterial.cpp
  RAS_Rasterizer.cpp
  RAS_IVertex.cpp
  RAS_MaterialBucket.cpp
  RAS_MeshMaterial.cpp
  RAS_MeshObject.cpp
  RAS_Polygon.cpp
  RAS_Shader.cpp
  RAS_Texture.cpp

  RAS_2DFilterData.h
  RAS_2DFilter.h
  RAS_2DFilterManager.h
  RAS_2DFilterFrameBuffer.h
  RAS_BucketManager.h
  RAS_CameraData.h
  RAS_DebugDraw.h
  RAS_DisplayArray.h
  RAS_DisplayArrayBucket.h
  RAS_FrameBuffer.h
  RAS_FramingManager.h
  RAS_ICanvas.h
  RAS_IDisplayArray.h
  RAS_IPolygonMaterial.h
  RAS_Rasterizer.h
  RAS_IVertex.h
  RAS_MaterialBucket.h
  RAS_MaterialShader.h
  RAS_MeshMaterial.h
  RAS_MeshObject.h
  RAS_Polygon.h
  RAS_Rect.h
  RAS_Shader.h
  RAS_Texture.h
  RAS_Vertex.h
)

set(LIB
  PRIVATE bf::blenfont
  PRIVATE bf::blenlib
  PRIVATE bf::blentranslation
  PRIVATE bf::bmesh
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::gpu
  PRIVATE bf::imbuf
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::render
  bf_intern_moto
  ge_rasterizer_opengl
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

blender_add_lib(ge_rasterizer "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
