# SPDX-FileCopyrightText: 2025 UPBGE
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Extract a pre-built V8 zip (v8/include, v8/lib) to HARVEST_TARGET/v8.
# Only runs on WIN32 when ${PACKAGE_DIR}/${V8_FILE} exists (include is conditional).
# Zip must have top-level directory "v8" with "include/" and "lib/" inside.
# Set V8_HASH in versions.cmake to the MD5 of your zip.

set(V8_EP_PREFIX ${BUILD_DIR}/v8)
set(V8_EP_SOURCE ${V8_EP_PREFIX}/src/external_v8)

ExternalProject_Add(external_v8
  URL file://${PACKAGE_DIR}/${V8_FILE}
  URL_HASH ${V8_HASH_TYPE}=${V8_HASH}
  DOWNLOAD_DIR ${PACKAGE_DIR}
  PREFIX ${V8_EP_PREFIX}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND
    ${CMAKE_COMMAND} -E make_directory ${HARVEST_TARGET}/v8 &&
    ${CMAKE_COMMAND} -E copy_directory ${V8_EP_SOURCE}/v8/include ${HARVEST_TARGET}/v8/include &&
    ${CMAKE_COMMAND} -E copy_directory ${V8_EP_SOURCE}/v8/lib ${HARVEST_TARGET}/v8/lib
)
