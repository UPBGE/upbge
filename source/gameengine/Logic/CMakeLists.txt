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
# Contributor(s): Tristan Porteries.
#
# ***** END GPL LICENSE BLOCK *****

set(INC
	.
	../Common
	../Converter
	../Expressions
	../GameLogic
	../Ketsji
	../Rasterizer
	../SceneGraph
	../../blender/blenlib
	../../blender/python/generic
	../../../intern/guardedalloc
	../../../intern/string
	../../../intern/termcolor
)

set(INC_SYS
	../../../intern/debugbreak
	../../../intern/mathfu
)

set(SRC
	LOG_INode.cpp
	LOG_FunctionNode.cpp
	LOG_FunctionSocket.cpp
	LOG_INodeSocket.cpp
	LOG_Node.cpp
	LOG_Object.cpp
	LOG_Tree.cpp
	LOG_ValueSocket.cpp

	LOG_INode.h
	LOG_FunctionNode.h
	LOG_FunctionSocket.h
	LOG_INodeSocket.h
	LOG_Node.h
	LOG_Object.h
	LOG_Tree.h
	LOG_ValueSocket.h
)

blender_add_lib(ge_logic "${SRC}" "${INC}" "${INC_SYS}")
