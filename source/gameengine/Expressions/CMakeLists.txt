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
	../Common
	../SceneGraph
	../../blender/blenlib
	../../../intern/guardedalloc
	../../../intern/termcolor
)

set(INC_SYS
	../../../intern/debugbreak
	../../../intern/mathfu
	${BOOST_INCLUDE_DIR}
)

set(SRC
	intern/BaseListValue.cpp
	intern/BaseListWrapper.cpp
	intern/BaseMapValue.cpp
	intern/PropBool.cpp
	intern/PropFloat.cpp
	intern/PropInt.cpp
	intern/PyObjectPlus.cpp
	intern/PropString.cpp
	intern/Value.cpp

	EXP_BaseListValue.h
	EXP_BaseListWrapper.h
	EXP_BaseMapValue.h
	EXP_ListValue.h
	EXP_ListWrapper.h
	EXP_MapValue.h
	EXP_PropBool.h
	EXP_PropFloat.h
	EXP_PropInt.h
	EXP_PyObjectPlus.h
	EXP_Python.h
	EXP_PropString.h
	EXP_Value.h
)

if(WITH_PYTHON)
	list(APPEND SRC
		intern/PythonCallBack.cpp
		intern/PropPython.cpp

		EXP_PythonCallBack.h
		EXP_PropPython.h
	)
endif()

blender_add_lib(ge_logic_expressions "${SRC}" "${INC}" "${INC_SYS}")
