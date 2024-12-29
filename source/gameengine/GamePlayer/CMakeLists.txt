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
  ..
  ../BlenderRoutines
  ../Common
  ../Converter
  ../Device
  ../Expressions
  ../GameLogic
  ../Ketsji
  ../Ketsji/KXNetwork
  ../Launcher
  ../Physics/Common
  ../Rasterizer
  ../Rasterizer/RAS_OpenGLRasterizer
  ../SceneGraph
  ../../blender
  ../../blender/editors/include
  ../../blender/makesrna
  ../../blender/python
  ../../blender/sequencer
  ../../blender/windowmanager/message_bus
  ../../../intern/memutil
  # RNA_prototypes.h
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)

set(INC_SYS
  ../../../intern/moto/include
  ${BOOST_INCLUDE_DIR}
  ${Epoxy_INCLUDE_DIRS}
)

set(SRC
  GPG_Canvas.cpp
  GPG_ghost.cpp

  GPG_Canvas.h
)

set(LIB
  PRIVATE bf::animrig
  PRIVATE bf::asset_system
  PRIVATE bf::blenfont
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader
  PRIVATE bf::blentranslation
  PRIVATE bf::bmesh
  PRIVATE bf::depsgraph
  PRIVATE bf::draw
  PRIVATE bf::dna
  PRIVATE bf::gpu
  PRIVATE bf::imbuf
  PRIVATE bf::imbuf::movie
  PRIVATE bf::intern::clog
  PRIVATE bf::intern::guardedalloc
  bf_intern_ghost
  PRIVATE bf::render
  PRIVATE bf::windowmanager
)

add_definitions(${GL_DEFINITIONS})

if(WIN32)
  list(APPEND INC ../../../intern/utfconv)
endif()

if(WITH_USD)
  list(APPEND LIB
    bf_io_usd
  )
  #add_definitions(-DWITH_USD)
endif()

if(WITH_TBB)
  add_definitions(-DWITH_TBB)

  list(APPEND INC_SYS
    ${TBB_INCLUDE_DIRS}
  )

  list(APPEND LIB
    ${TBB_LIBRARIES}
  )
endif()

if(WITH_PYTHON)
  list(APPEND INC_SYS
    ${PYTHON_INCLUDE_DIRS}
  )
endif()

if(WITH_INPUT_NDOF)
  add_definitions(-DWITH_INPUT_NDOF)
endif(WITH_INPUT_NDOF)

if(WITH_CODEC_FFMPEG)
  add_definitions(-DWITH_FFMPEG)
endif()

if(WITH_INTERNATIONAL)
  add_definitions(-DWITH_INTERNATIONAL)
endif()

if(WITH_AUDASPACE)
  list(APPEND INC_SYS
    ${AUDASPACE_C_INCLUDE_DIRS}
  )
  list(APPEND LIB
    bf_intern_audaspace

    ${AUDASPACE_C_LIBRARIES}
    ${AUDASPACE_PY_LIBRARIES}
  )
  add_definitions(-DWITH_AUDASPACE)
endif()

if(WITH_GAMEENGINE_BPPLAYER)
  list(APPEND INC
    ../../../intern/spindle
  )
  add_definitions(-DWITH_GAMEENGINE_BPPLAYER)
endif()

if(WITH_VULKAN_METAL)
  add_definitions(-DWITH_VULKAN_METAL)
endif()

if(WITH_VULKAN_BACKEND)
  add_definitions(-DWITH_VULKAN_BACKEND)
endif()

blender_add_lib_nolist(ge_player "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

# RNA_prototypes.h
add_dependencies(ge_player bf_rna)
