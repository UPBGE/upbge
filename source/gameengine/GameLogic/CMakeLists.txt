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
	../Device
	../Expressions
	../Rasterizer
	../SceneGraph
	../../blender/blenlib
	../../blender/python/generic
	../../../intern/termcolor
	../../../intern/ghost
)

set(INC_SYS
	../../../intern/debugbreak
	../../../intern/mathfu
	${BOOST_INCLUDE_DIR}
)

set(SRC
	SCA_IInputDevice.cpp
	SCA_InputEvent.cpp
	SCA_PythonJoystick.cpp
	SCA_PythonKeyboard.cpp
	SCA_PythonMouse.cpp

	SCA_IInputDevice.h
	SCA_InputEvent.h
	SCA_PythonJoystick.h
	SCA_PythonKeyboard.h
	SCA_PythonMouse.h
)

if(WITH_SDL)
	list(APPEND INC_SYS
		${SDL_INCLUDE_DIR}
	)

	add_definitions(-DWITH_SDL)

	if(WITH_GHOST_SDL)
		add_definitions(-DWITH_GHOST_SDL)
	endif()
	if(WITH_SDL_DYNLOAD)
		add_definitions(-DWITH_SDL_DYNLOAD)
	endif()
endif()

blender_add_lib(ge_logic "${SRC}" "${INC}" "${INC_SYS}")
