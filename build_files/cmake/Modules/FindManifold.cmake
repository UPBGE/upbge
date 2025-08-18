# - Find Manifold library
# Find the native Manifold includes and library
# This module defines
#  MANIFOLD_INCLUDE_DIRS, where to find manifold/manifold.h, Set when
#                         MANIFOLD_INCLUDE_DIR is found.
#  MANIFOLD_LIBRARIES, libraries to link against to use Manifold.
#  MANIFOLD_ROOT_DIR, The base directory to search for Manifold.
#                     This can also be an environment variable.
#  MANIFOLD_FOUND, If false, do not try to use Manifold.
#
# also defined, but not for general use are
#  MANIFOLD_LIBRARY, where to find the Manifold library.

# If MANIFOLD_ROOT_DIR was defined in the environment, use it.
if(NOT MANIFOLD_ROOT_DIR AND NOT $ENV{MANIFOLD_ROOT_DIR} STREQUAL "")
  set(MANIFOLD_ROOT_DIR $ENV{MANIFOLD_ROOT_DIR})
endif()

set(_manifold_SEARCH_DIRS
  ${MANIFOLD_ROOT_DIR}
  /opt/lib/manifold
  /usr/local
  /usr
)

find_path(MANIFOLD_INCLUDE_DIR
  NAMES
    manifold/manifold.h
  HINTS
    ${_manifold_SEARCH_DIRS}
  PATH_SUFFIXES
    include
)

find_library(MANIFOLD_LIBRARY
  NAMES
    manifold
  HINTS
    ${_manifold_SEARCH_DIRS}
  PATH_SUFFIXES
    lib64 lib
)

# handle the QUIETLY and REQUIRED arguments and set MANIFOLD_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(Manifold DEFAULT_MSG
    MANIFOLD_LIBRARY MANIFOLD_INCLUDE_DIR)

if(MANIFOLD_FOUND)
  set(MANIFOLD_LIBRARIES ${MANIFOLD_LIBRARY})
  set(MANIFOLD_INCLUDE_DIRS ${MANIFOLD_INCLUDE_DIR})
endif()

mark_as_advanced(
  MANIFOLD_INCLUDE_DIR
  MANIFOLD_LIBRARY
)

unset(_manifold_SEARCH_DIRS)