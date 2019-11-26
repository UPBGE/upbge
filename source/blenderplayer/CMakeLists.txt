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

# message(STATUS "Configuring blenderplayer")

setup_libdirs()


list(APPEND LIB
  ge_player
  blenkernel_blc
  bf_intern_guardedalloc
  bf_blenlib
  bf_intern_memutil
  bf_dna_blenlib
  bf_blenloader
)

if(WIN32 AND NOT UNIX)
	string(SUBSTRING ${BLENDER_VERSION} 0 1 bver1)
	string(SUBSTRING ${BLENDER_VERSION} 2 1 bver2)
	string(SUBSTRING ${BLENDER_VERSION} 3 1 bver3)
	add_definitions(
		-DBLEN_VER_RC_STR=${BLENDER_VERSION}
		-DBLEN_VER_RC_1=${bver1}
		-DBLEN_VER_RC_2=${bver2}
		-DBLEN_VER_RC_3=${bver3}
		-DBLEN_VER_RC_4=0
	)

	add_executable(
			blenderplayer ${EXETYPE}
			bad_level_call_stubs/stubs.c
			${CMAKE_SOURCE_DIR}/release/windows/icons/winblender.rc)
	WINDOWS_SIGN_TARGET(blenderplayer)
	install(TARGETS blenderplayer
			COMPONENT Blenderplayer
			DESTINATION ".")

add_cc_flags_custom_test(blenderplayer)

elseif(APPLE)
	add_executable(blenderplayer ${EXETYPE} bad_level_call_stubs/stubs.c)
	# setup Info.plist
	execute_process(COMMAND date "+%Y-%m-%d" OUTPUT_VARIABLE BLENDER_DATE OUTPUT_STRIP_TRAILING_WHITESPACE)
	set(PLAYER_SOURCEDIR ${CMAKE_SOURCE_DIR}/release/darwin/blenderplayer.app)
	set(PLAYER_SOURCEINFO ${PLAYER_SOURCEDIR}/Contents/Info.plist)
	set_target_properties(blenderplayer PROPERTIES
		MACOSX_BUNDLE_INFO_PLIST ${PLAYER_SOURCEINFO}
		MACOSX_BUNDLE_SHORT_VERSION_STRING "${BLENDER_VERSION}${BLENDER_VERSION_CHAR}"
		MACOSX_BUNDLE_LONG_VERSION_STRING "${BLENDER_VERSION}${BLENDER_VERSION_CHAR} ${BLENDER_DATE}")

else()
	add_executable(blenderplayer bad_level_call_stubs/stubs.c)

	if(NOT WITH_INSTALL_PORTABLE)
		install(
			TARGETS blenderplayer
			DESTINATION bin
		)
	else()
		install(
			TARGETS blenderplayer
			DESTINATION "."
		)
	endif()
endif()

add_dependencies(blenderplayer makesdna)

target_link_libraries(blenderplayer ${LIB})
unset(LIB)

if(WITH_PLAYER)
	add_subdirectory(bad_level_call_stubs)
endif()

setup_liblinks(blenderplayer)
