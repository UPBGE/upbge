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
# Contributor(s): Tristan Porteries.
#
# ***** END GPL LICENSE BLOCK *****

set(INC
  .
  ../BlenderRoutines
  ../Common
  ../Converter
  ../Device
  ../Expressions
  ../GameLogic
  ../GamePlayer
  ../Ketsji
  ../Ketsji/KXNetwork
  ../Rasterizer
  ../SceneGraph
  ../VideoTexture
  ../../blender/blenkernel
  ../../blender/blenlib
  ../../blender/blentranslation
  ../../blender/draw/engines/eevee/
  ../../blender/draw/intern
  ../../blender/gpu
  ../../blender/makesdna
  ../../blender/makesrna
  ../../blender/render
  ../../blender/windowmanager
  ../../../intern/guardedalloc
  ../../../intern/ghost
  ../../../intern/string
  ../../../intern/termcolor
)

set(INC_SYS
  ../../../intern/moto/include
  ${BOOST_INCLUDE_DIR}
  ${GLEW_INCLUDE_PATH}
)

set(SRC
  LA_BlenderLauncher.cpp
  LA_Launcher.cpp
  LA_PlayerLauncher.cpp
  LA_SystemCommandLine.cpp
  LA_System.cpp

  LA_BlenderLauncher.h
  LA_Launcher.h
  LA_PlayerLauncher.h
  LA_SystemCommandLine.h
  LA_System.h
)

set(LIB
  bf_intern_moto
  ge_expressions
  ge_logic_bricks
  ge_msg_network
  ge_videotexture
  ge_device
  ge_blender_routines
)

if(WITH_GAMEENGINE_SECURITY)
  add_definitions(-DWITH_GAMEENGINE_SECURITY)
endif()

if(WITH_PLAYER)
    list(APPEND LIB
      ge_player
    )
endif()

if(WITH_AUDASPACE)
  list(APPEND INC_SYS
    ${AUDASPACE_C_INCLUDE_DIRS}
  )
  list(APPEND LIB
    ${AUDASPACE_C_LIBRARIES}
    ${AUDASPACE_PY_LIBRARIES}
  )
  add_definitions(-DWITH_AUDASPACE)
endif()

blender_add_lib(ge_launcher "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
