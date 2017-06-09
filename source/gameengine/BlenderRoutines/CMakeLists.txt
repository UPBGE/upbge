
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
	../../blender/blenfont
	../../blender/blenkernel
	../../blender/blenlib
	../../blender/blenloader
	../../blender/editors/include
	../../blender/gpu
	../../blender/imbuf
	../../blender/makesdna
	../../blender/makesrna
	../../blender/windowmanager
	../../../intern/guardedalloc
	../../../intern/string
	../../../intern/termcolor
	../../../intern/ghost
)

set(INC_SYS
	../../../intern/mathfu
	${PTHREADS_INCLUDE_DIRS}
	${BOOST_INCLUDE_DIR}
)

set(SRC
	BL_KetsjiEmbedStart.cpp
	KX_BlenderCanvas.cpp

	KX_BlenderCanvas.h
)

add_definitions(${GL_DEFINITIONS})

if(WITH_AUDASPACE)
	add_definitions(${AUDASPACE_DEFINITIONS})

	list(APPEND INC_SYS
		${AUDASPACE_C_INCLUDE_DIRS}
	)
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


blender_add_lib(ge_blen_routines "${SRC}" "${INC}" "${INC_SYS}")
