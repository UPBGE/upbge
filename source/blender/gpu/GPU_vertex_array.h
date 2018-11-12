/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Jorge Bernal
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file GPU_vertex_array.h
 *  \ingroup gpu
 */

#ifndef __GPU_VERTEX_ARRAY_H__
#define __GPU_VERTEX_ARRAY_H__

#ifdef __cplusplus
extern "C" {
#endif

/* GPU Vertex Array
 * - only to make accesible VAOs for MacOS when we are in compatibility profile */

void GPU_create_vertex_arrays(int n, unsigned int *arrays);
void GPU_bind_vertex_array(unsigned int array);
void GPU_delete_vertex_arrays(int n, const unsigned int *arrays);


#ifdef __cplusplus
}
#endif

#endif  /* __GPU_VERTEX_ARRAY_H__ */
