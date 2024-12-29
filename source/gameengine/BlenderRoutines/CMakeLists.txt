
set(INC
  .
  ../Common
  ../Converter
  ../Device
  ../Expressions
  ../GameLogic
  ../Ketsji
  ../Ketsji/KXNetwork
  ../Launcher
  ../Physics/Bullet
  ../Physics/Common
  ../Rasterizer
  ../Rasterizer/RAS_OpenGLRasterizer
  ../SceneGraph
  ../../blender
  ../../blender/editors/include
  ../../blender/makesrna
  ../../../intern/termcolor
  ../../../intern/ghost
)

set(INC_SYS
  ../../../intern/moto/include
  ${PTHREADS_INCLUDE_DIRS}
  ${BOOST_INCLUDE_DIR}
)

set(SRC
  BL_KetsjiEmbedStart.cpp
)

set(LIB
  PRIVATE bf::blenfont
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader
  PRIVATE bf::dna
  PRIVATE bf::gpu
  PRIVATE bf::imbuf
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::windowmanager
  ge_ketsji
  ge_common
  ge_launcher
)

add_definitions(${GL_DEFINITIONS})

if(WITH_AUDASPACE)
  list(APPEND INC_SYS
    ${AUDASPACE_C_INCLUDE_DIRS}
  )
  list(APPEND LIB
    ${AUDASPACE_C_LIBRARIES}
    ${AUDASPACE_PY_LIBRARIES}
  )
  add_definitions(-DWITH_AUDASPACE)
endif()

if(WITH_CODEC_FFMPEG)
  add_definitions(-DWITH_FFMPEG)
endif()

if(WITH_BULLET)
  list(APPEND INC_SYS
    ${BULLET_INCLUDE_DIRS}
  )
  add_definitions(-DWITH_BULLET)
endif()


blender_add_lib(ge_blender_routines "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
