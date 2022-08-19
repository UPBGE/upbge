# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2015 Blender Foundation.

# - Find OPENVDB library
# Find the native OPENVDB includes and library
# This module defines
#  OPENVDB_INCLUDE_DIRS, where to find openvdb.h, Set when
#                            OPENVDB_INCLUDE_DIR is found.
#  OPENVDB_LIBRARIES, libraries to link against to use OPENVDB.
#  OPENVDB_ROOT_DIR, The base directory to search for OPENVDB.
#                        This can also be an environment variable.
#  OPENVDB_FOUND, If false, do not try to use OPENVDB.
#
# also defined, but not for general use are
#  OPENVDB_LIBRARY, where to find the OPENVDB library.

# If OPENVDB_ROOT_DIR was defined in the environment, use it.
IF(NOT OPENVDB_ROOT_DIR AND NOT $ENV{OPENVDB_ROOT_DIR} STREQUAL "")
  SET(OPENVDB_ROOT_DIR $ENV{OPENVDB_ROOT_DIR})
ENDIF()

SET(_openvdb_SEARCH_DIRS
  ${OPENVDB_ROOT_DIR}
  /opt/lib/openvdb
)

FIND_PATH(OPENVDB_INCLUDE_DIR
  NAMES
    openvdb/openvdb.h
  HINTS
    ${_openvdb_SEARCH_DIRS}
  PATH_SUFFIXES
    include
)

FIND_LIBRARY(OPENVDB_LIBRARY
  NAMES
    openvdb
  HINTS
    ${_openvdb_SEARCH_DIRS}
  PATH_SUFFIXES
    lib64 lib
)

# handle the QUIETLY and REQUIRED arguments and set OPENVDB_FOUND to TRUE if
# all listed variables are TRUE
INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(OpenVDB DEFAULT_MSG
    OPENVDB_LIBRARY OPENVDB_INCLUDE_DIR)

IF(OPENVDB_FOUND)
  SET(OPENVDB_LIBRARIES ${OPENVDB_LIBRARY})
  SET(OPENVDB_INCLUDE_DIRS ${OPENVDB_INCLUDE_DIR})
ENDIF()

MARK_AS_ADVANCED(
  OPENVDB_INCLUDE_DIR
  OPENVDB_LIBRARY
)

UNSET(_openvdb_SEARCH_DIRS)
