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
  ../Expressions
  ../GameLogic
  ../Ketsji
  ../SceneGraph
  ../../blender/python/generic
  ../../../intern/termcolor
)

set(INC_SYS
  ../../../intern/moto/include
)

set(SRC
  CM_Clock.cpp
  CM_Message.cpp
  CM_Thread.cpp
  CM_Utils.cpp

  CM_Clock.h
  CM_Format.h
  CM_List.h
  CM_Message.h
  CM_RefCount.h
  CM_Thread.h
  CM_Utils.h
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::intern::guardedalloc
)

if(MSVC)
	add_definitions(
		/DNOMINMAX
	)
endif()

blender_add_lib(ge_common "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
