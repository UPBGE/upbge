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
  # XXX Remove these <<<
  ../../BlenderRoutines
  ../../Common
  ../../Expressions
  ../../GameLogic
  ../../Ketsji
  ../../Physics/Common
  # >>>
  ../../SceneGraph
  ../../../blender/blenfont
  ../../../blender/blenkernel
  ../../../blender/blenlib
  ../../../blender/blentranslation
  ../../../blender/draw/intern
  ../../../blender/draw/engines/eevee/
  ../../../blender/gpu
  ../../../blender/makesdna
  ../../../blender/makesrna
  ../../../blender/render
  ../../../blender/render/intern
  ../../../../intern/glew-mx
  ../../../../intern/guardedalloc
)

set(INC_SYS
  ../../../../intern/moto/include
  ${GLEW_INCLUDE_PATH}
  ${BOOST_INCLUDE_DIR}
)

set(SRC
  RAS_OpenGLDebugDraw.cpp
  RAS_OpenGLRasterizer.cpp

  RAS_OpenGLDebugDraw.h
  RAS_OpenGLRasterizer.h
)

set(LIB
)

add_definitions(${GL_DEFINITIONS})

blender_add_lib(ge_rasterizer_opengl "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
