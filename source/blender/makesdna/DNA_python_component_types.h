/**
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
 * Contributor(s): Mitchell Stokes, Diego Lopes, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifndef __DNA_COMPONENT_TYPES_H__
#define __DNA_COMPONENT_TYPES_H__

#include "DNA_listBase.h"

typedef struct PythonComponentProperty {
	struct PythonComponentProperty *next, *prev;
	char name[64];
	short type;
	short boolval;
	int intval;
	float floatval;
	char strval[64];
	int itemval;
	float vec[4];
	ListBase enumval;
} PythonComponentProperty;

typedef struct PythonComponent {
	struct PythonComponent *next, *prev;
	ListBase properties;
	char name[64];
	char module[64];
	int flag;
	int pad;
} PythonComponent;


/* PythonComponentProperty.type */
#define CPROP_TYPE_INT         0
#define CPROP_TYPE_FLOAT       1
#define CPROP_TYPE_STRING      2
#define CPROP_TYPE_BOOLEAN     3
#define CPROP_TYPE_SET         4
#define CPROP_TYPE_VEC2        5
#define CPROP_TYPE_VEC3        6
#define CPROP_TYPE_VEC4        7

enum {
	COMPONENT_SHOW = (1 << 0)
};

#endif /* __DNA_COMPONENT_TYPES_H__ */
