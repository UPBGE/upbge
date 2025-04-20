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

list(APPEND LIB
  ge_player
  bf_asset_system
  bf_intern_guardedalloc
  PRIVATE bf::blenlib
  bf_intern_memutil
  bf_dna_blenlib
  bf_blenloader
)

# Compatibility with Windows 7
if(WIN32 AND WITH_WINDOWS_7)
  list(INSERT LIB 0 ${BCOMPAT7_LIBRARIES})
endif()

# MSVC 2010 gives linking errors with the manifest
if(WIN32 AND NOT UNIX)
    add_definitions(
    -DBLEN_VER_RC_STR="${BLENDER_VERSION}"
    -DBLEN_VER_RC_1=${BLENDER_VERSION_MAJOR}
    -DBLEN_VER_RC_2=${BLENDER_VERSION_MINOR}
    -DBLEN_VER_RC_3=${BLENDER_VERSION_PATCH}
    -DBLEN_VER_RC_4=0
  )


  list(APPEND SRC
    ${CMAKE_SOURCE_DIR}/release/windows/icons/winblender.rc
    ${CMAKE_BINARY_DIR}/blender.exe.manifest
  )
endif()

if(WITH_BUILDINFO)
  add_definitions(-DWITH_BUILDINFO)
  # --------------------------------------------------------------------------
  # These defines could all be moved into the header below

  # Write strings into a separate header since we can escape C-strings
  # in a way that's not practical when passing defines.
  set(BUILD_PLATFORM "${CMAKE_SYSTEM_NAME}")
  set(BUILD_TYPE "${CMAKE_BUILD_TYPE}")
  set(BUILD_CFLAGS "${CMAKE_C_FLAGS}")
  set(BUILD_CXXFLAGS "${CMAKE_CXX_FLAGS}")
  set(BUILD_LINKFLAGS "${PLATFORM_LINKFLAGS}")
  set(BUILD_SYSTEM "CMake")

  if(WITH_COMPILER_SHORT_FILE_MACRO)
    # It's not necessary to include path information
    # about the system building Blender in the executable.
    string(REPLACE "${PLATFORM_CFLAGS_FMACRO_PREFIX_MAP}" " " BUILD_CFLAGS "${BUILD_CFLAGS}")
    string(REPLACE "${PLATFORM_CFLAGS_FMACRO_PREFIX_MAP}" " " BUILD_CXXFLAGS "${BUILD_CXXFLAGS}")
  endif()

  # Use `configure_file` instead of definitions since properly
  # escaping the multiple command line arguments which themselves
  # contain strings and spaces becomes overly error-prone & complicated.
  configure_file(
    "${CMAKE_SOURCE_DIR}/build_files/cmake/buildinfo_static.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/buildinfo_static.h"
    ESCAPE_QUOTES
    @ONLY
  )

  unset(BUILD_PLATFORM)
  unset(BUILD_TYPE)
  unset(BUILD_CFLAGS)
  unset(BUILD_CXXFLAGS)
  unset(BUILD_LINKFLAGS)
  unset(BUILD_SYSTEM)

  # --------------------------------------------------------------------------
  # write header for values that change each build
  # note, generaed file is in build dir's source/creator
  #       except when used as an include path.

  add_definitions(-DWITH_BUILDINFO_HEADER)

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

  # From the cmake documentation "If the output of the custom command is not actually created as a
  # file on disk it should be marked with the SYMBOLIC source file property."
  #
  # Not doing this leads to build warnings for the not generated file on windows when using msbuild
  SET_SOURCE_FILES_PROPERTIES(${buildinfo_h_fake} PROPERTIES SYMBOLIC TRUE)

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
  add_library(blenderplayer MODULE ${SRC})
  set_target_properties(
    blenderplayer
    PROPERTIES
      PREFIX ""
      OUTPUT_NAME bpy
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin  # only needed on windows
  )

  if(APPLE)
    set_target_properties(blenderplayer PROPERTIES MACOSX_BUNDLE TRUE)
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
endif()

if(WITH_BUILDINFO)
  # explicitly say that the executable depends on the buildinfo
  add_dependencies(blenderplayer buildinfo_player)
endif()

# Apple treats both executables in a self-contained manner.
# Therefore we have to copy all the resources for the blenderplayer as well
if(APPLE)

  set(PLAYER_TARGETDIR_VER Blenderplayer.app/Contents/Resources/${BLENDER_VERSION})
  set(PLAYER_TARGETDIR_LIB Blenderplayer.app/Contents/Resources/lib)
  set(PLAYER_TARGETDIR_TEXT Blenderplayer.app/Contents/Resources/text)

  # Skip relinking on cpack / install
  set_target_properties(blenderplayer PROPERTIES BUILD_WITH_INSTALL_RPATH true)

  set(PLAYER_TEXT_FILES
    ${CMAKE_SOURCE_DIR}/release/text/copyright.txt
    # generate this file
    # ${CMAKE_SOURCE_DIR}/release/text/readme.html
  )

  # -----------------------------------------------------------------------------
  # Install Targets (Generic, All Platforms)
  # important to make a clean  install each time, else old scripts get loaded.
  install(
    CODE
    "file(REMOVE_RECURSE ${PLAYER_TARGETDIR_VER})"
  )

  if(WITH_PYTHON)
    # install(CODE "message(\"copying blender scripts...\")")

    # exclude addons_contrib if release
    if("${BLENDER_VERSION_CYCLE}" STREQUAL "release" OR
       "${BLENDER_VERSION_CYCLE}" STREQUAL "rc")
      set(ADDON_EXCLUDE_CONDITIONAL "addons_contrib/*")
    else()
      set(ADDON_EXCLUDE_CONDITIONAL "_addons_contrib/*")  # dummy, wont do anything
    endif()

    # do not install freestyle dir if disabled
    if(NOT WITH_FREESTYLE)
      set(FREESTYLE_EXCLUDE_CONDITIONAL "freestyle/*")
    else()
      set(FREESTYLE_EXCLUDE_CONDITIONAL "_freestyle/*")  # dummy, wont do anything
    endif()

    install(
      DIRECTORY ${CMAKE_SOURCE_DIR}/scripts
      DESTINATION ${PLAYER_TARGETDIR_VER}
      PATTERN ".git" EXCLUDE
      PATTERN ".gitignore" EXCLUDE
      PATTERN ".arcconfig" EXCLUDE
      PATTERN "__pycache__" EXCLUDE
      PATTERN "site" EXCLUDE
      PATTERN "${ADDON_EXCLUDE_CONDITIONAL}" EXCLUDE
      PATTERN "${FREESTYLE_EXCLUDE_CONDITIONAL}" EXCLUDE
    )

    unset(ADDON_EXCLUDE_CONDITIONAL)
    unset(FREESTYLE_EXCLUDE_CONDITIONAL)
  endif()

  # fonts
  install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/release/datafiles/fonts
    DESTINATION ${PLAYER_TARGETDIR_VER}/datafiles
  )

  # color management
  if(WITH_OPENCOLORIO)
    install(
      DIRECTORY ${CMAKE_SOURCE_DIR}/release/datafiles/colormanagement
      DESTINATION ${PLAYER_TARGETDIR_VER}/datafiles
    )
  endif()

  # game controller data base
  if(WITH_GAMEENGINE AND WITH_SDL)
    install(
      DIRECTORY ${CMAKE_SOURCE_DIR}/release/datafiles/gamecontroller
      DESTINATION ${PLAYER_TARGETDIR_VER}/datafiles
    )
  endif()

  if(NOT WITH_PYTHON_MODULE)
    # Uppercase name for app bundle
    set_target_properties(blenderplayer PROPERTIES OUTPUT_NAME Blenderplayer)
  endif()

  # handy install macro to exclude files, we use \$ escape for the "to"
  # argument when calling so ${BUILD_TYPE} does not get expanded
  macro(install_dir from to)
    install(
      DIRECTORY ${from}
      DESTINATION ${to}
      PATTERN ".git" EXCLUDE
      PATTERN ".svn" EXCLUDE
      PATTERN "*.pyc" EXCLUDE
      PATTERN "*.pyo" EXCLUDE
      PATTERN "*.orig" EXCLUDE
      PATTERN "*.rej" EXCLUDE
      PATTERN "__pycache__" EXCLUDE
      PATTERN "__MACOSX" EXCLUDE
      PATTERN ".DS_Store" EXCLUDE
      PATTERN "config-${PYTHON_VERSION}/*.a" EXCLUDE  # static lib
      PATTERN "lib2to3" EXCLUDE                   # ./lib2to3
      PATTERN "tkinter" EXCLUDE                   # ./tkinter
      PATTERN "lib-dynload/_tkinter.*" EXCLUDE    # ./lib-dynload/_tkinter.co
      PATTERN "idlelib" EXCLUDE                   # ./idlelib
      PATTERN "test" EXCLUDE                      # ./test
      PATTERN "turtledemo" EXCLUDE                # ./turtledemo
      PATTERN "turtle.py" EXCLUDE                 # ./turtle.py
      PATTERN "wininst*.exe" EXCLUDE              # from distutils, avoid malware false positive
    )
  endmacro()

  set(OSX_APP_PLAYER_SOURCEDIR ${CMAKE_SOURCE_DIR}/release/darwin/Blenderplayer.app)

  # setup Info.plist
  execute_process(COMMAND date "+%Y-%m-%d"
                  OUTPUT_VARIABLE BLENDER_DATE
                  OUTPUT_STRIP_TRAILING_WHITESPACE)

  set_target_properties(blenderplayer PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${OSX_APP_PLAYER_SOURCEDIR}/Contents/Info.plist
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${BLENDER_VERSION}${BLENDER_VERSION_CHAR}"
    MACOSX_BUNDLE_LONG_VERSION_STRING "${BLENDER_VERSION}${BLENDER_VERSION_CHAR} ${BLENDER_DATE}")

  # Gather the date in finder-style
  execute_process(COMMAND date "+%m/%d/%Y/%H:%M"
  OUTPUT_VARIABLE SETFILE_DATE
  OUTPUT_STRIP_TRAILING_WHITESPACE)

  # Give the bundle actual creation/modification date
  #
  # Note that the directory might not yet exist, which happens when CMAKE is first run.
  if(NOT EXISTS ${EXECUTABLE_OUTPUT_PATH}/Blenderplayer.app)
    file(MAKE_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}/Blenderplayer.app)
  endif()
  execute_process(
    COMMAND SetFile -d ${SETFILE_DATE} -m ${SETFILE_DATE} ${EXECUTABLE_OUTPUT_PATH}/Blenderplayer.app
  )

  install(
    TARGETS blenderplayer
    DESTINATION "."
  )

  install(
    FILES ${OSX_APP_PLAYER_SOURCEDIR}/Contents/PkgInfo
    DESTINATION Blenderplayer.app/Contents
  )

  install_dir(
    ${OSX_APP_PLAYER_SOURCEDIR}/Contents/Resources
    Blenderplayer.app/Contents/
  )

  if(PLATFORM_BUNDLED_LIBRARIES AND PLAYER_TARGETDIR_LIB)
    install(
      FILES ${PLATFORM_BUNDLED_LIBRARIES}
      DESTINATION ${PLAYER_TARGETDIR_LIB}
    )
  endif()

  if(WITH_VULKAN_BACKEND)
    install(
      FILES ${VULKAN_LIBRARY} ${MOLTENVK_LIBRARY}
      DESTINATION ${PLAYER_TARGETDIR_LIB}
    )
  endif()

  # python
  if(WITH_PYTHON AND NOT WITH_PYTHON_MODULE AND NOT WITH_PYTHON_FRAMEWORK)
    # Copy the python libs into the install directory
    install_dir(
      ${PYTHON_LIBPATH}/python${PYTHON_VERSION}
      ${PLAYER_TARGETDIR_VER}/python/lib
    )

    # Install Python executable.
    install(
      PROGRAMS ${PYTHON_EXECUTABLE}
      DESTINATION ${PLAYER_TARGETDIR_VER}/python/bin
    )

    # Needed for distutils/pip
    # get the last part of the include dir, will be 'python{version}{abiflag}',
    get_filename_component(_py_inc_suffix ${PYTHON_INCLUDE_DIR} NAME)
    install(
      FILES ${PYTHON_INCLUDE_DIR}/pyconfig.h
      DESTINATION ${PLAYER_TARGETDIR_VER}/python/include/${_py_inc_suffix}
    )
    unset(_py_inc_suffix)
  endif()

  if(WITH_DRACO)
    install(
      PROGRAMS $<TARGET_FILE:extern_draco>
      DESTINATION ${PLAYER_TARGETDIR_VER}/python/lib/python${PYTHON_VERSION}/site-packages
    )
  endif()

  # -----------------------------------------------------------------------------
  # Generic Install, for all targets

  if(DEFINED PLAYER_TARGETDIR_TEXT)

    configure_file(
      ${CMAKE_SOURCE_DIR}/release/text/readme.html
      ${CMAKE_BINARY_DIR}/release/text/readme.html
      @ONLY
    )
    list(APPEND PLAYER_TEXT_FILES
      ${CMAKE_BINARY_DIR}/release/text/readme.html
    )

    install(
      FILES ${PLAYER_TEXT_FILES}
      DESTINATION "${PLAYER_TARGETDIR_TEXT}"
    )

    install(
      DIRECTORY ${CMAKE_SOURCE_DIR}/release/license
      DESTINATION "${PLAYER_TARGETDIR_TEXT}"
  )
  endif()

  # install more files specified elsewhere
  delayed_do_install(${PLAYER_TARGETDIR_VER})

  unset(PLAYER_TEXT_FILES)
  unset(PLAYER_TARGETDIR_TEXT)

  # -----------------------------------------------------------------------------
  # Studio Lights
  install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/release/datafiles/studiolights
    DESTINATION ${PLAYER_TARGETDIR_VER}/datafiles
  )
endif()

# -----------------------------------------------------------------------------
# Setup link libraries

add_dependencies(blenderplayer makesdna)
target_link_libraries(blenderplayer PRIVATE ${LIB})
unset(LIB)

setup_platform_linker_flags(blenderplayer)
setup_platform_linker_libs(blenderplayer)

if(DEFINED PLATFORM_SYMBOLS_MAP)
  set_target_properties(blenderplayer PROPERTIES LINK_DEPENDS ${PLATFORM_SYMBOLS_MAP})
endif()

blender_target_include_dirs(blenderplayer ${INC})

# Always install MaterialX files regardless if Blender itself uses them, the
# bundled Python module still needs it.
if(WITH_MATERIALX AND LIBDIR AND PLAYER_TARGETDIR_LIB)
  install(
    DIRECTORY ${LIBDIR}/materialx/libraries
    DESTINATION "${PLAYER_TARGETDIR_LIB}/materialx"
  )
endif()
