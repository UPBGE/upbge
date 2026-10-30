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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */
#pragma once

/** \file BKE_property.h
 *  \ingroup bke
 */

#include "DNA_property_types.h"

namespace blender {

struct bProperty;
struct Object;

void BKE_bproperty_free(struct bProperty *prop);
void BKE_bproperty_free_list(struct ListBase *lb);
struct bProperty *BKE_bproperty_copy(const struct bProperty *prop);
void BKE_bproperty_copy_list(struct ListBase *lbn, const struct ListBase *lbo);
void BKE_bproperty_init(struct bProperty *prop);
struct bProperty *BKE_bproperty_new(int type);
/** The list holding \a prop: the object's properties, or the items or columns of \a r_owner (a
 * Choice, List or Table property, or a Table row). */
ListBase *BKE_bproperty_owner_list(Object *ob, const bProperty *prop, bProperty **r_owner);
/** Adds a Choice option, List item, Table row, or with \a column a Table column, and selects it. */
bProperty *BKE_bproperty_item_add(bProperty *prop, bool column);
bool BKE_bproperty_item_remove(bProperty *prop, bool column, int index);
/** Moves the item or column at \a index by \a step (-1 up, 1 down) and selects it. */
bool BKE_bproperty_item_move(bProperty *prop, bool column, int index, int step);
/** Changes a List's item type; existing items restart at the new type's default. */
void BKE_bproperty_list_set_item_type(bProperty *prop, int type);
/** Gives every Table row one cell per column, of that column's type. */
void BKE_bproperty_table_sync(bProperty *table);
void BKE_bproperty_unique(struct bProperty *first, struct bProperty *prop, int force);
struct bProperty *BKE_bproperty_object_get(struct Object *ob, const char *name);
void BKE_bproperty_object_set(struct Object *ob, struct bProperty *propc);
// int               BKE_bproperty_cmp(struct bProperty *prop, const char *str);
void BKE_bproperty_set(struct bProperty *prop, const char *str);
void BKE_bproperty_add(struct bProperty *prop, const char *str);
/* should really be called '_get_valstr()' or '_as_string()' */
void BKE_bproperty_set_valstr(struct bProperty *prop, char str[MAX_PROPSTRING]);

}  // namespace blender
