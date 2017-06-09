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

set(INC
	.
	../BlenderRoutines
	../Common
	../Device
	../Expressions
	../GameLogic
	../Ketsji
	../Rasterizer
	../Rasterizer/Node
	../Rasterizer/RAS_OpenGLRasterizer
	../SceneGraph
	../../blender/blenkernel
	../../blender/blenlib
	../../blender/editors/include
	../../blender/gpu
	../../blender/imbuf
	../../blender/makesdna
	../../blender/python
	../../blender/python/generic
	../../../intern/ffmpeg
	../../../intern/glew-mx
	../../../intern/guardedalloc
	../../../intern/decklink
	../../../intern/gpudirect
	../../../intern/atomic
)

set(INC_SYS
	../../../intern/mathfu
	${GLEW_INCLUDE_PATH}
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

if(WITH_CODEC_FFMPEG)
	list(APPEND INC_SYS
		${FFMPEG_INCLUDE_DIRS}
		${PTHREADS_INCLUDE_DIRS}
	)
	add_definitions(-DWITH_FFMPEG)

	remove_strict_flags_file(
		VideoFFmpeg.cpp
		VideoDeckLink
		DeckLink
	)
endif()

if (WITH_GAMEENGINE_GPU_SYNC)
	add_definitions(-DWITH_GAMEENGINE_GPU_SYNC)
endif()

if(WITH_GAMEENGINE_DECKLINK)
	add_definitions(-DWITH_GAMEENGINE_DECKLINK)
endif()

blender_add_lib(ge_videotex "${SRC}" "${INC}" "${INC_SYS}")
