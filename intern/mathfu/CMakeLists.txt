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
# The Original Code is Copyright (C) 2015, Blender Foundation
# All rights reserved.
#
# The Original Code is: all of this file.
#
# Contributor(s): Kevin Dietrich.
#
# ***** END GPL LICENSE BLOCK *****

set(INC
	.
	vectorial/include
)

set(INC_SYS
)

set(SRC
	mathfu.h
	mathfu/constants.h
	mathfu/frustum.h
	mathfu/glsl_mapping.h
	mathfu/hlsl_mapping.h
	mathfu/io.h
	mathfu/matrix.h
	mathfu/matrix_4x4.h
	mathfu/quaternion.h
	mathfu/rect.h
	mathfu/utilities.h
	mathfu/vector.h

	vectorial/config.h
	vectorial/mat4f.h
	vectorial/simd2f.h
	vectorial/simd2f_common.h
	vectorial/simd2f_neon.h
	vectorial/simd4f.h
	vectorial/simd4f_common.h
	vectorial/simd4f_gnu.h
	vectorial/simd4f_neon.h
	vectorial/simd4f_scalar.h
	vectorial/simd4f_sse.h
	vectorial/simd4x4f.h
	vectorial/simd4x4f_gnu.h
	vectorial/simd4x4f_neon.h
	vectorial/simd4x4f_scalar.h
	vectorial/simd4x4f_sse.h
	vectorial/vec2f.h
	vectorial/vec3f.h
	vectorial/vec4f.h
	vectorial/vec_convert.h
	vectorial/vectorial.h
)

blender_add_lib(bf_intern_mathfu "${SRC}" "${INC}" "${INC_SYS}")
