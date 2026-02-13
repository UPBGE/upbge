# SPDX-FileCopyrightText: 2025 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Jolt Physics integration via FetchContent.
# Downloads and builds Jolt Physics from source as part of UPBGE's build process.

include(FetchContent)

# Pin to a specific stable release for reproducible builds.
set(JOLT_PHYSICS_VERSION "v5.5.0" CACHE STRING "Jolt Physics version tag to fetch")

FetchContent_Declare(
  JoltPhysics
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG        ${JOLT_PHYSICS_VERSION}
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  SOURCE_SUBDIR  Build
)

# Jolt build configuration options — set before FetchContent_MakeAvailable
# so that Jolt's CMake picks them up.

# Use 32-bit object layers for more collision groups.
set(OBJECT_LAYER_BITS 32 CACHE STRING "Number of bits in Jolt ObjectLayer" FORCE)

# Enable Jolt ObjectStream for physics state serialization support.
# This compiles the ObjectStream class and Jolt's custom RTTI attribute
# information needed for serializing physics state.
set(ENABLE_OBJECT_STREAM ON CACHE BOOL "Enable Jolt ObjectStream for serialization" FORCE)

# Enable C++ RTTI so that Jolt emits typeinfo for its classes.
# Required for subclassing JPH::GroupFilter (used for constraint
# "Disable Collisions" filtering).
set(CPP_RTTI_ENABLED ON CACHE BOOL "Enable C++ RTTI in Jolt" FORCE)

# Disable Jolt's own install targets — UPBGE handles installation.
set(ENABLE_INSTALL OFF CACHE BOOL "Disable Jolt install targets" FORCE)

# Disable Jolt samples/tests/tools — only build the library.
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)

# --- Performance-critical settings ---

# Cross-platform determinism disables FMA and uses slower math paths.
# Not needed for a game engine — leave OFF for maximum SIMD performance.
set(CROSS_PLATFORM_DETERMINISTIC OFF CACHE BOOL "Disable cross-platform determinism for max performance" FORCE)

# Debug renderer adds overhead even when not drawing anything.
# Jolt docs: "enabling this reduces the performance of the library even if
# you're not drawing anything."
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "Disable Jolt debug renderer for performance" FORCE)

# Profiler adds timing instrumentation throughout the physics step.
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "Disable Jolt profiler for performance" FORCE)

# LTO enables cross-translation-unit inlining, critical for Jolt's
# virtual function calls in the collision pipeline.
set(INTERPROCEDURAL_OPTIMIZATION ON CACHE BOOL "Enable Jolt IPO/LTO" FORCE)

# Let Jolt use its own default CMAKE_CXX_FLAGS instead of overriding.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "Don't let Jolt override CXX flags" FORCE)

FetchContent_MakeAvailable(JoltPhysics)

# The Jolt library target is named "Jolt" after FetchContent.
# Create an alias that UPBGE's build system can reference consistently.
if(TARGET Jolt)
  # Expose include directory for consumers.
  # Jolt's headers are under the source root in the "Jolt/" subdirectory.
  # FetchContent places the source at ${joltphysics_SOURCE_DIR}.
  set(JOLT_INCLUDE_DIRS "${joltphysics_SOURCE_DIR}" CACHE PATH "Jolt Physics include directory" FORCE)
else()
  message(FATAL_ERROR "Jolt Physics target 'Jolt' not found after FetchContent_MakeAvailable")
endif()
