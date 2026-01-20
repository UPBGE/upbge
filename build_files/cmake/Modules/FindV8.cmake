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
# ***** END GPL LICENSE BLOCK *****

# Find V8 JavaScript Engine
# This module defines:
#  V8_FOUND - System has V8
#  V8_INCLUDE_DIRS - The V8 include directories
#  V8_LIBRARIES - The libraries needed to use V8

# Try to find V8 in common locations (NuGet, vcpkg, LIBDIR/v8, extern)
find_path(V8_INCLUDE_DIR
  NAMES v8.h
  PATHS
    ${V8_ROOT}/include
    ${V8_ROOT}/include/v8
    ${CMAKE_SOURCE_DIR}/extern/v8/include
    ${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/include
    ${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/include/v8
    /usr/include
    /usr/local/include
    /opt/v8/include
)

find_library(V8_BASE_LIBRARY
  NAMES v8_base v8 v8.dll
  PATHS
    ${V8_ROOT}/lib
    ${CMAKE_SOURCE_DIR}/extern/v8/lib
    ${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/lib
    /usr/lib
    /usr/local/lib
    /opt/v8/lib
)

find_library(V8_PLATFORM_LIBRARY
  NAMES v8_platform v8
  PATHS
    ${V8_ROOT}/lib
    ${CMAKE_SOURCE_DIR}/extern/v8/lib
    ${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/lib
    /usr/lib
    /usr/local/lib
    /opt/v8/lib
)

find_library(V8_INITIALIZATION_LIBRARY
  NAMES v8_initialization v8
  PATHS
    ${V8_ROOT}/lib
    ${CMAKE_SOURCE_DIR}/extern/v8/lib
    /usr/lib
    /usr/local/lib
    /opt/v8/lib
)

# On Windows, V8 might be in a different structure (v8_libplatform or monolithic v8)
if(WIN32)
  find_library(V8_LIBPLATFORM_LIBRARY
    NAMES v8_libplatform v8_libplatform.dll v8
    PATHS
      ${V8_ROOT}/lib
      ${CMAKE_SOURCE_DIR}/extern/v8/lib
      ${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/lib
  )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(V8
  FOUND_VAR V8_FOUND
  REQUIRED_VARS V8_INCLUDE_DIR V8_BASE_LIBRARY
)

if(V8_FOUND)
  set(V8_INCLUDE_DIRS ${V8_INCLUDE_DIR})
  # If v8.h was in include/v8/, also add parent so libplatform and others are found
  get_filename_component(V8_LAST_DIR "${V8_INCLUDE_DIR}" NAME)
  if(V8_LAST_DIR STREQUAL "v8")
    get_filename_component(V8_INCLUDE_PARENT ${V8_INCLUDE_DIR} DIRECTORY)
    list(APPEND V8_INCLUDE_DIRS ${V8_INCLUDE_PARENT})
    list(REMOVE_DUPLICATES V8_INCLUDE_DIRS)
  endif()

  # Collect all V8 libraries
  set(V8_LIBRARIES
    ${V8_BASE_LIBRARY}
  )
  
  if(V8_PLATFORM_LIBRARY)
    list(APPEND V8_LIBRARIES ${V8_PLATFORM_LIBRARY})
  endif()
  
  if(V8_INITIALIZATION_LIBRARY)
    list(APPEND V8_LIBRARIES ${V8_INITIALIZATION_LIBRARY})
  endif()
  
  if(V8_LIBPLATFORM_LIBRARY)
    list(APPEND V8_LIBRARIES ${V8_LIBPLATFORM_LIBRARY})
  endif()
  
  # Remove duplicates
  list(REMOVE_DUPLICATES V8_LIBRARIES)
endif()

mark_as_advanced(
  V8_INCLUDE_DIR
  V8_BASE_LIBRARY
  V8_PLATFORM_LIBRARY
  V8_INITIALIZATION_LIBRARY
  V8_LIBPLATFORM_LIBRARY
)
