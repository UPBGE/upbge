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
# Contributor(s): Jacques Beaurain.
#
# ***** END GPL LICENSE BLOCK *****

set(INC
  ../include
  ../interface
  ../../makesrna

  # dna_type_offsets.h
  ${CMAKE_CURRENT_BINARY_DIR}/../../makesdna/intern
  # RNA_prototypes.h
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)

set(INC_SYS
  ${Epoxy_INCLUDE_DIRS}
)

set(SRC
  logic_buttons.cc
  logic_ops.cc
  logic_window.cc
  space_logic.cc

  logic_intern.hh
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader
  PRIVATE bf::blentranslation
  PRIVATE bf::dna
  PRIVATE bf::gpu
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::windowmanager
)

if(WITH_GAMEENGINE)
  add_definitions(-DWITH_GAMEENGINE)
endif()

add_definitions(${GL_DEFINITIONS})

if(WITH_INTERNATIONAL)
	add_definitions(-DWITH_INTERNATIONAL)
endif()

blender_add_lib(bf_editor_space_logic "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

# RNA_prototypes.h
add_dependencies(bf_editor_space_logic bf_rna)

if(MSVC AND NOT MSVC_CLANG)
  target_compile_options(bf_editor_space_logic PRIVATE /wd26495 /wd26812) # disable annoying MSVC warnings
endif()
