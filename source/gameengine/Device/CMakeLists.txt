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
# Contributor(s): Tristan Porteries.
#
# ***** END GPL LICENSE BLOCK *****

set(INC
	.
	../Common
	../Expressions
	../GameLogic
	../Ketsji
	../Rasterizer
	../SceneGraph
	../../blender/blenlib
	../../../intern/ghost
	../../../intern/string
	../../../intern/termcolor
)

set(INC_SYS
	../../../intern/mathfu
)

set(SRC
	DEV_EventConsumer.cpp
	DEV_InputDevice.cpp
	DEV_Joystick.cpp
	DEV_JoystickEvents.cpp
	DEV_JoystickVibration.cpp

	DEV_EventConsumer.h
	DEV_InputDevice.h
	DEV_Joystick.h
	DEV_JoystickDefines.h
	DEV_JoystickPrivate.h
	DEV_JoystickMappingdb.h
)

if(WITH_SDL)
	list(APPEND INC_SYS
		${SDL_INCLUDE_DIR}
	)

	add_definitions(-DWITH_SDL)
endif()

if(WITH_INPUT_NDOF)
	add_definitions(-DWITH_INPUT_NDOF)
endif()

blender_add_lib(ge_device "${SRC}" "${INC}" "${INC_SYS}")
