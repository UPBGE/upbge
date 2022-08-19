# SPDX-License-Identifier: GPL-2.0-or-later

unpack_only(igc_vcintrinsics)
unpack_only(igc_spirv_headers)
unpack_only(igc_spirv_tools)

#
# igc_opencl_clang contains patches that need to be applied
# to external_igc_llvm and igc_spirv_translator, we unpack
# igc_opencl_clang first, then have the patch stages of
# external_igc_llvm and igc_spirv_translator apply them.
#

ExternalProject_Add(external_igc_opencl_clang
  URL file://${PACKAGE_DIR}/${IGC_OPENCL_CLANG_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${IGC_OPENCL_CLANG_HASH_TYPE}=${IGC_OPENCL_CLANG_HASH}
  PREFIX ${BUILD_DIR}/igc_opencl_clang
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
  PATCH_COMMAND ${PATCH_CMD} -p 1 -d ${BUILD_DIR}/igc_opencl_clang/src/external_igc_opencl_clang/ < ${PATCH_DIR}/igc_opencl_clang.diff
)

set(IGC_OPENCL_CLANG_PATCH_DIR ${BUILD_DIR}/igc_opencl_clang/src/external_igc_opencl_clang/patches)
set(IGC_LLVM_SOURCE_DIR ${BUILD_DIR}/igc_llvm/src/external_igc_llvm)
set(IGC_SPIRV_TRANSLATOR_SOURCE_DIR ${BUILD_DIR}/igc_spirv_translator/src/external_igc_spirv_translator)

ExternalProject_Add(external_igc_llvm
  URL file://${PACKAGE_DIR}/${IGC_LLVM_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${IGC_LLVM_HASH_TYPE}=${IGC_LLVM_HASH}
  PREFIX ${BUILD_DIR}/igc_llvm
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
  PATCH_COMMAND ${PATCH_CMD} -p 1 -d ${IGC_LLVM_SOURCE_DIR} < ${IGC_OPENCL_CLANG_PATCH_DIR}/clang/0001-OpenCL-3.0-support.patch &&
    ${PATCH_CMD} -p 1 -d ${IGC_LLVM_SOURCE_DIR} < ${IGC_OPENCL_CLANG_PATCH_DIR}/clang/0002-Remove-__IMAGE_SUPPORT__-macro-for-SPIR.patch &&
    ${PATCH_CMD} -p 1 -d ${IGC_LLVM_SOURCE_DIR} < ${IGC_OPENCL_CLANG_PATCH_DIR}/clang/0003-Avoid-calling-ParseCommandLineOptions-in-BackendUtil.patch &&
    ${PATCH_CMD} -p 1 -d ${IGC_LLVM_SOURCE_DIR} < ${IGC_OPENCL_CLANG_PATCH_DIR}/clang/0004-OpenCL-support-cl_ext_float_atomics.patch &&
    ${PATCH_CMD} -p 1 -d ${IGC_LLVM_SOURCE_DIR} < ${IGC_OPENCL_CLANG_PATCH_DIR}/clang/0005-OpenCL-Add-cl_khr_integer_dot_product.patch &&
    ${PATCH_CMD} -p 1 -d ${IGC_LLVM_SOURCE_DIR} < ${IGC_OPENCL_CLANG_PATCH_DIR}/llvm/0001-Memory-leak-fix-for-Managed-Static-Mutex.patch &&
    ${PATCH_CMD} -p 1 -d ${IGC_LLVM_SOURCE_DIR} < ${IGC_OPENCL_CLANG_PATCH_DIR}/llvm/0002-Remove-repo-name-in-LLVM-IR.patch
)
add_dependencies(
  external_igc_llvm
  external_igc_opencl_clang
)

ExternalProject_Add(external_igc_spirv_translator
  URL file://${PACKAGE_DIR}/${IGC_SPIRV_TRANSLATOR_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${IGC_SPIRV_TRANSLATOR_HASH_TYPE}=${IGC_SPIRV_TRANSLATOR_HASH}
  PREFIX ${BUILD_DIR}/igc_spirv_translator
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
  PATCH_COMMAND ${PATCH_CMD} -p 1 -d ${IGC_SPIRV_TRANSLATOR_SOURCE_DIR} < ${IGC_OPENCL_CLANG_PATCH_DIR}/spirv/0001-update-SPIR-V-headers-for-SPV_INTEL_split_barrier.patch &&
    ${PATCH_CMD} -p 1 -d ${IGC_SPIRV_TRANSLATOR_SOURCE_DIR} < ${IGC_OPENCL_CLANG_PATCH_DIR}/spirv/0002-Add-support-for-split-barriers-extension-SPV_INTEL_s.patch &&
    ${PATCH_CMD} -p 1 -d ${IGC_SPIRV_TRANSLATOR_SOURCE_DIR} < ${IGC_OPENCL_CLANG_PATCH_DIR}/spirv/0003-Support-cl_bf16_conversions.patch
)
add_dependencies(
  external_igc_spirv_translator
  external_igc_opencl_clang
)

if(WIN32)
  set(IGC_GENERATOR "Ninja")
  set(IGC_TARGET Windows64)
else()
  set(IGC_GENERATOR "Unix Makefiles")
  set(IGC_TARGET Linux64)
endif()

set(IGC_EXTRA_ARGS
  -DIGC_OPTION__ARCHITECTURE_TARGET=${IGC_TARGET}
  -DIGC_OPTION__ARCHITECTURE_HOST=${IGC_TARGET}
)

if(UNIX AND NOT APPLE)
  list(APPEND IGC_EXTRA_ARGS
       -DFLEX_EXECUTABLE=${LIBDIR}/flex/bin/flex
       -DFLEX_INCLUDE_DIR=${LIBDIR}/flex/include
  )
endif()

ExternalProject_Add(external_igc
  URL file://${PACKAGE_DIR}/${IGC_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${IGC_HASH_TYPE}=${IGC_HASH}
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${LIBDIR}/igc ${DEFAULT_CMAKE_FLAGS} ${IGC_EXTRA_ARGS}

  # IGC is pretty set in its way where sub projects ought to live, for some it offers
  # hooks to supply alternatives folders, other are just hardocded with no way to configure
  # we symlink everything here, since it's less work than trying to convince the cmake
  # scripts to accept alternative locations.
  #
  PATCH_COMMAND ${CMAKE_COMMAND} -E create_symlink ${BUILD_DIR}/igc_llvm/src/external_igc_llvm/ ${BUILD_DIR}/igc/src/llvm-project &&
    ${CMAKE_COMMAND} -E create_symlink ${BUILD_DIR}/igc_opencl_clang/src/external_igc_opencl_clang/ ${BUILD_DIR}/igc/src/llvm-project/llvm/projects/opencl-clang &&
    ${CMAKE_COMMAND} -E create_symlink ${BUILD_DIR}/igc_spirv_translator/src/external_igc_spirv_translator/ ${BUILD_DIR}/igc/src/llvm-project/llvm/projects/llvm-spirv &&
    ${CMAKE_COMMAND} -E create_symlink ${BUILD_DIR}/igc_spirv_tools/src/external_igc_spirv_tools/ ${BUILD_DIR}/igc/src/SPIRV-Tools &&
    ${CMAKE_COMMAND} -E create_symlink ${BUILD_DIR}/igc_spirv_headers/src/external_igc_spirv_headers/ ${BUILD_DIR}/igc/src/SPIRV-Headers &&
    ${CMAKE_COMMAND} -E create_symlink ${BUILD_DIR}/igc_vcintrinsics/src/external_igc_vcintrinsics/ ${BUILD_DIR}/igc/src/vc-intrinsics
  PREFIX ${BUILD_DIR}/igc
  INSTALL_DIR ${LIBDIR}/igc
  INSTALL_COMMAND ${CMAKE_COMMAND} --install . --strip
  CMAKE_GENERATOR ${IGC_GENERATOR}
)

add_dependencies(
  external_igc
  external_igc_vcintrinsics
  external_igc_llvm
  external_igc_opencl_clang
  external_igc_vcintrinsics
  external_igc_spirv_headers
  external_igc_spirv_tools
  external_igc_spirv_translator
)

if(UNIX AND NOT APPLE)
  add_dependencies(
    external_igc
    external_flex
  )
endif()
