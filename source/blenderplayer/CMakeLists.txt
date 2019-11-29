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


if(WITH_PLAYER)
        add_subdirectory(bad_level_call_stubs)
endif()

list(APPEND LIB
  ge_player
  blenkernel_blc
  bf_intern_guardedalloc
  bf_blenlib
  bf_intern_memutil
  bf_dna_blenlib
  bf_blenloader
)

if(WITH_BUILDINFO)
  add_definitions(-DWITH_BUILDINFO)
  # --------------------------------------------------------------------------
  # These defines could all be moved into the header below
  string(REPLACE " " "\ " BUILDINFO_CFLAGS "${CMAKE_C_FLAGS}")
  string(REPLACE " " "\ " BUILDINFO_CXXFLAGS "${CMAKE_CXX_FLAGS}")
  string(REPLACE " " "\ " BUILDINFO_LINKFLAGS "${PLATFORM_LINKFLAGS}")
  add_definitions(
    # # define in header now, else these get out of date on rebuilds.
    # -DBUILD_DATE="${BUILD_DATE}"
    # -DBUILD_TIME="${BUILD_TIME}"
    # -DBUILD_COMMIT_TIMESTAMP="${BUILD_COMMIT_TIMESTAMP}"
    # -DBUILD_COMMIT_TIME="${BUILD_COMMIT_TIME}"
    # -DBUILD_COMMIT_DATE="${BUILD_COMMIT_DATE}"
    # -DBUILD_HASH="${BUILD_HASH}"
    # -DBUILD_BRANCH="${BUILD_BRANCH}"
    -DWITH_BUILDINFO_HEADER # alternative to lines above
    -DBUILD_PLATFORM="${CMAKE_SYSTEM_NAME}"
    -DBUILD_TYPE="${CMAKE_BUILD_TYPE}"
    -DBUILD_CFLAGS="${BUILDINFO_CFLAGS}"
    -DBUILD_CXXFLAGS="${BUILDINFO_CXXFLAGS}"
    -DBUILD_LINKFLAGS="${BUILDINFO_LINKFLAGS}"
    -DBUILD_SYSTEM="CMake"
  )

  # --------------------------------------------------------------------------
  # write header for values that change each build
  # note, generaed file is in build dir's source/creator
  #       except when used as an include path.

  # include the output directory, where the buildinfo.h file is generated
  include_directories(${CMAKE_CURRENT_BINARY_DIR})


  # XXX, ${buildinfo_h_fake} is used here,
  # because we rely on that file being detected as missing
  # every build so that the real header "buildinfo.h" is updated.
  #
  # Keep this until we find a better way to resolve!

  set(buildinfo_h_real "${CMAKE_CURRENT_BINARY_DIR}/buildinfo.h")
  set(buildinfo_h_fake "${CMAKE_CURRENT_BINARY_DIR}/buildinfo.h_fake")

  if(EXISTS ${buildinfo_h_fake})
    message(FATAL_ERROR "File \"${buildinfo_h_fake}\" found, this should never be created, remove!")
  endif()

  # a custom target that is always built
  add_custom_target(buildinfo_player ALL
    DEPENDS ${buildinfo_h_fake})

  # creates buildinfo.h using cmake script
  add_custom_command(
    OUTPUT
      ${buildinfo_h_fake}  # ensure we always run
      ${buildinfo_h_real}
    COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    # overrides only used when non-empty strings
    -DBUILD_DATE=${BUILDINFO_OVERRIDE_DATE}
    -DBUILD_TIME=${BUILDINFO_OVERRIDE_TIME}
    -P ${CMAKE_SOURCE_DIR}/build_files/cmake/buildinfo.cmake)

  # buildinfo.h is a generated file
  set_source_files_properties(
    ${buildinfo_h_real}
    PROPERTIES GENERATED TRUE
    HEADER_FILE_ONLY TRUE)

  unset(buildinfo_h_real)
  unset(buildinfo_h_fake)

  # add deps below, after adding blender
  # -------------- done with header values.

  list(APPEND SRC
    ../creator/buildinfo.c
  )

  # make an object library so can load with it in tests
  add_library(buildinfoobj_player OBJECT ../creator/buildinfo.c)
  add_dependencies(buildinfoobj_player buildinfo_player)
endif()

add_cc_flags_custom_test(blenderplayer)

# message(STATUS "Configuring blender")
if(WITH_PYTHON_MODULE)
  add_definitions(-DWITH_PYTHON_MODULE)

  # creates ./bin/bpy.so which can be imported as a python module.
  #
  # note that 'SHARED' works on Linux and Windows,
  # but not OSX which _must_ be 'MODULE'
  add_library(blender MODULE ${SRC})
  set_target_properties(
    blenderplayer
    PROPERTIES
      PREFIX ""
      OUTPUT_NAME bpy
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin  # only needed on windows
  )

  if(APPLE)
    set_target_properties(
      blender
      PROPERTIES
        MACOSX_BUNDLE TRUE
        LINK_FLAGS_RELEASE "${PLATFORM_LINKFLAGS}"
        LINK_FLAGS_DEBUG "${PLATFORM_LINKFLAGS_DEBUG}"
    )
  endif()

  if(WIN32)
    # python modules use this
    set_target_properties(
      blenderplayer
      PROPERTIES
      SUFFIX ".pyd"
    )
  endif()

else()
  add_executable(blenderplayer ${EXETYPE} ${SRC})
  WINDOWS_SIGN_TARGET(blenderplayer)
endif()

if(WITH_BUILDINFO)
  # explicitly say that the executable depends on the buildinfo
  add_dependencies(blenderplayer buildinfo_player)
endif()

add_dependencies(blenderplayer makesdna)

target_link_libraries(blenderplayer ${LIB})
unset(LIB)

setup_liblinks(blenderplayer)
