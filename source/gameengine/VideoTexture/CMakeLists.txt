# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2006 Blender Foundation. All rights reserved.

set(INC
  .
  ../BlenderRoutines
  ../Common
  ../Device
  ../Expressions
  ../GameLogic
  ../Ketsji
  ../Rasterizer
  ../Rasterizer/RAS_OpenGLRasterizer
  ../SceneGraph
  ../../blender/editors/include
  ../../blender/imbuf/movie/intern
  ../../blender/makesrna
  ../../blender/python
  ../../blender/python/generic
  ../../../intern/decklink
  ../../../intern/gpudirect
  ../../../intern/atomic
)

set(INC_SYS
  ../../../intern/moto/include
  ${Epoxy_INCLUDE_DIRS}
  ${BOOST_INCLUDE_DIR}
)

add_definitions(${GL_DEFINITIONS})

set(SRC
  Exception.cpp
  FilterBase.cpp
  FilterBlueScreen.cpp
  FilterColor.cpp
  FilterNormal.cpp
  FilterSource.cpp
  ImageBase.cpp
  ImageBuff.cpp
  ImageMix.cpp
  ImageRender.cpp
  ImageViewport.cpp
  PyTypeList.cpp
  Texture.cpp
  DeckLink.cpp
  VideoBase.cpp
  VideoFFmpeg.cpp
  VideoDeckLink.cpp
  blendVideoTex.cpp

  Common.h
  Exception.h
  FilterBase.h
  FilterBlueScreen.h
  FilterColor.h
  FilterNormal.h
  FilterSource.h
  ImageBase.h
  ImageBuff.h
  ImageMix.h
  ImageRender.h
  ImageViewport.h
  PyTypeList.h
  Texture.h
  DeckLink.h
  VideoBase.h
  VideoFFmpeg.h
  VideoDeckLink.h
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blentranslation
  PRIVATE bf::bmesh
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::draw
  PRIVATE bf::gpu
  PRIVATE bf::imbuf
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::render
  ge_scenegraph
)

if(MSVC)
  add_definitions(
    /DNOMINMAX
  )
endif()

if(WITH_GAMEENGINE_DECKLINK)
  add_definitions(-DWITH_GAMEENGINE_DECKLINK)

  list(APPEND LIB
    bf_intern_decklink
  )
endif()

if(WIN32)
    list(APPEND LIB
      bf_intern_gpudirect
    )
endif()

if(WITH_CODEC_FFMPEG)
  list(APPEND INC_SYS
    ${FFMPEG_INCLUDE_DIRS}
    ${PTHREADS_INCLUDE_DIRS}
  )
  add_definitions(-DWITH_FFMPEG)
endif()

blender_add_lib(ge_videotexture "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
