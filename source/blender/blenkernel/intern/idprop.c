/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "BLI_endian_switch.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_global.h"
#include "BKE_idprop.h"
#include "BKE_lib_id.h"

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

#include "BLO_read_write.h"

#include "BLI_strict_flags.h"

/* IDPropertyTemplate is a union in DNA_ID.h */

/**
 * if the new is 'IDP_ARRAY_REALLOC_LIMIT' items less,
 * than #IDProperty.totallen, reallocate anyway.
 */
#define IDP_ARRAY_REALLOC_LIMIT 200

static CLG_LogRef LOG = {"bke.idprop"};

/* Local size table. */
static size_t idp_size_table[] = {
    1, /*strings*/
    sizeof(int),
    sizeof(float),
    sizeof(float[3]),  /* Vector type, deprecated. */
    sizeof(float[16]), /* Matrix type, deprecated. */
    0,                 /* Arrays don't have a fixed size. */
    sizeof(ListBase),  /* Group type. */
    sizeof(void *),
    sizeof(double),
};

/* -------------------------------------------------------------------- */
/** \name Array Functions (IDP Array API)
 * \{ */

#define GETPROP(prop, i) &(IDP_IDPArray(prop)[i])

/* --------- property array type -------------*/

IDProperty *IDP_NewIDPArray(const char *name)
{
  IDProperty *prop = MEM_callocN(sizeof(IDProperty), "IDProperty prop array");
  prop->type = IDP_IDPARRAY;
  prop->len = 0;
  BLI_strncpy(prop->name, name, MAX_IDPROP_NAME);

  return prop;
}

IDProperty *IDP_CopyIDPArray(const IDProperty *array, const int flag)
{
  /* don't use MEM_dupallocN because this may be part of an array */
  BLI_assert(array->type == IDP_IDPARRAY);

  IDProperty *narray = MEM_mallocN(sizeof(IDProperty), __func__);
  *narray = *array;

  narray->data.pointer = MEM_dupallocN(array->data.pointer);
  for (int i = 0; i < narray->len; i++) {
    /* OK, the copy functions always allocate a new structure,
     * which doesn't work here.  instead, simply copy the
     * contents of the new structure into the array cell,
     * then free it.  this makes for more maintainable
     * code than simply re-implementing the copy functions
     * in this loop. */
    IDProperty *tmp = IDP_CopyProperty_ex(GETPROP(narray, i), flag);
    memcpy(GETPROP(narray, i), tmp, sizeof(IDProperty));
    MEM_freeN(tmp);
  }

  return narray;
}

static void IDP_FreeIDPArray(IDProperty *prop, const bool do_id_user)
{
  BLI_assert(prop->type == IDP_IDPARRAY);

  for (int i = 0; i < prop->len; i++) {
    IDP_FreePropertyContent_ex(GETPROP(prop, i), do_id_user);
  }

  if (prop->data.pointer) {
    MEM_freeN(prop->data.pointer);
  }
}

void IDP_SetIndexArray(IDProperty *prop, int index, IDProperty *item)
{
  BLI_assert(prop->type == IDP_IDPARRAY);

  if (index >= prop->len || index < 0) {
    return;
  }

  IDProperty *old = GETPROP(prop, index);
  if (item != old) {
    IDP_FreePropertyContent(old);

    memcpy(old, item, sizeof(IDProperty));
  }
}

IDProperty *IDP_GetIndexArray(IDProperty *prop, int index)
{
  BLI_assert(prop->type == IDP_IDPARRAY);

  return GETPROP(prop, index);
}

void IDP_AppendArray(IDProperty *prop, IDProperty *item)
{
  BLI_assert(prop->type == IDP_IDPARRAY);

  IDP_ResizeIDPArray(prop, prop->len + 1);
  IDP_SetIndexArray(prop, prop->len - 1, item);
}

void IDP_ResizeIDPArray(IDProperty *prop, int newlen)
{
  BLI_assert(prop->type == IDP_IDPARRAY);

  /* first check if the array buffer size has room */
  if (newlen <= prop->totallen) {
    if (newlen < prop->len && prop->totallen - newlen < IDP_ARRAY_REALLOC_LIMIT) {
      for (int i = newlen; i < prop->len; i++) {
        IDP_FreePropertyContent(GETPROP(prop, i));
      }

      prop->len = newlen;
      return;
    }
    if (newlen >= prop->len) {
      prop->len = newlen;
      return;
    }
  }

  /* free trailing items */
  if (newlen < prop->len) {
    /* newlen is smaller */
    for (int i = newlen; i < prop->len; i++) {
      IDP_FreePropertyContent(GETPROP(prop, i));
    }
  }

  /* NOTE: This code comes from python, here's the corresponding comment. */
  /* This over-allocates proportional to the list size, making room
   * for additional growth. The over-allocation is mild, but is
   * enough to give linear-time amortized behavior over a long
   * sequence of appends() in the presence of a poorly-performing
   * system realloc().
   * The growth pattern is:  0, 4, 8, 16, 25, 35, 46, 58, 72, 88, ...
   */
  int newsize = newlen;
  newsize = (newsize >> 3) + (newsize < 9 ? 3 : 6) + newsize;
  prop->data.pointer = MEM_recallocN(prop->data.pointer, sizeof(IDProperty) * (size_t)newsize);
  prop->len = newlen;
  prop->totallen = newsize;
}

/* ----------- Numerical Array Type ----------- */
static void idp_resize_group_array(IDProperty *prop, int newlen, void *newarr)
{
  if (prop->subtype != IDP_GROUP) {
    return;
  }

  if (newlen >= prop->len) {
    /* bigger */
    IDProperty **array = newarr;
    IDPropertyTemplate val;

    for (int a = prop->len; a < newlen; a++) {
      val.i = 0; /* silence MSVC warning about uninitialized var when debugging */
      array[a] = IDP_New(IDP_GROUP, &val, "IDP_ResizeArray group");
    }
  }
  else {
    /* smaller */
    IDProperty **array = prop->data.pointer;

    for (int a = newlen; a < prop->len; a++) {
      IDP_FreeProperty(array[a]);
    }
  }
}

void IDP_ResizeArray(IDProperty *prop, int newlen)
{
  const bool is_grow = newlen >= prop->len;

  /* first check if the array buffer size has room */
  if (newlen <= prop->totallen && prop->totallen - newlen < IDP_ARRAY_REALLOC_LIMIT) {
    idp_resize_group_array(prop, newlen, prop->data.pointer);
    prop->len = newlen;
    return;
  }

  /* NOTE: This code comes from python, here's the corresponding comment. */
  /* This over-allocates proportional to the list size, making room
   * for additional growth.  The over-allocation is mild, but is
   * enough to give linear-time amortized behavior over a long
   * sequence of appends() in the presence of a poorly-performing
   * system realloc().
   * The growth pattern is:  0, 4, 8, 16, 25, 35, 46, 58, 72, 88, ...
   */
  int newsize = newlen;
  newsize = (newsize >> 3) + (newsize < 9 ? 3 : 6) + newsize;

  if (is_grow == false) {
    idp_resize_group_array(prop, newlen, prop->data.pointer);
  }

  prop->data.pointer = MEM_recallocN(prop->data.pointer,
                                     idp_size_table[(int)prop->subtype] * (size_t)newsize);

  if (is_grow == true) {
    idp_resize_group_array(prop, newlen, prop->data.pointer);
  }

  prop->len = newlen;
  prop->totallen = newsize;
}

void IDP_FreeArray(IDProperty *prop)
{
  if (prop->data.pointer) {
    idp_resize_group_array(prop, 0, NULL);
    MEM_freeN(prop->data.pointer);
  }
}

IDPropertyUIData *IDP_ui_data_copy(const IDProperty *prop)
{
  IDPropertyUIData *dst_ui_data = MEM_dupallocN(prop->ui_data);

  /* Copy extra type specific data. */
  switch (IDP_ui_data_type(prop)) {
    case IDP_UI_DATA_TYPE_STRING: {
      const IDPropertyUIDataString *src = (const IDPropertyUIDataString *)prop->ui_data;
      IDPropertyUIDataString *dst = (IDPropertyUIDataString *)dst_ui_data;
      dst->default_value = MEM_dupallocN(src->default_value);
      break;
    }
    case IDP_UI_DATA_TYPE_ID: {
      break;
    }
    case IDP_UI_DATA_TYPE_INT: {
      const IDPropertyUIDataInt *src = (const IDPropertyUIDataInt *)prop->ui_data;
      IDPropertyUIDataInt *dst = (IDPropertyUIDataInt *)dst_ui_data;
      dst->default_array = MEM_dupallocN(src->default_array);
      break;
    }
    case IDP_UI_DATA_TYPE_FLOAT: {
      const IDPropertyUIDataFloat *src = (const IDPropertyUIDataFloat *)prop->ui_data;
      IDPropertyUIDataFloat *dst = (IDPropertyUIDataFloat *)dst_ui_data;
      dst->default_array = MEM_dupallocN(src->default_array);
      break;
    }
    case IDP_UI_DATA_TYPE_UNSUPPORTED: {
      break;
    }
  }

  dst_ui_data->description = MEM_dupallocN(prop->ui_data->description);

  return dst_ui_data;
}

static IDProperty *idp_generic_copy(const IDProperty *prop, const int UNUSED(flag))
{
  IDProperty *newp = MEM_callocN(sizeof(IDProperty), __func__);

  BLI_strncpy(newp->name, prop->name, MAX_IDPROP_NAME);
  newp->type = prop->type;
  newp->flag = prop->flag;
  newp->data.val = prop->data.val;
  newp->data.val2 = prop->data.val2;

  if (prop->ui_data != NULL) {
    newp->ui_data = IDP_ui_data_copy(prop);
  }

  return newp;
}

static IDProperty *IDP_CopyArray(const IDProperty *prop, const int flag)
{
  IDProperty *newp = idp_generic_copy(prop, flag);

  if (prop->data.pointer) {
    newp->data.pointer = MEM_dupallocN(prop->data.pointer);

    if (prop->type == IDP_GROUP) {
      IDProperty **array = newp->data.pointer;
      int a;

      for (a = 0; a < prop->len; a++) {
        array[a] = IDP_CopyProperty_ex(array[a], flag);
      }
    }
  }
  newp->len = prop->len;
  newp->subtype = prop->subtype;
  newp->totallen = prop->totallen;

  return newp;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name String Functions (IDProperty String API)
 * \{ */

IDProperty *IDP_NewString(const char *st, const char *name, int maxlen)
{
  IDProperty *prop = MEM_callocN(sizeof(IDProperty), "IDProperty string");

  if (st == NULL) {
    prop->data.pointer = MEM_mallocN(DEFAULT_ALLOC_FOR_NULL_STRINGS, "id property string 1");
    *IDP_String(prop) = '\0';
    prop->totallen = DEFAULT_ALLOC_FOR_NULL_STRINGS;
    prop->len = 1; /* NULL string, has len of 1 to account for null byte. */
  }
  else {
    /* include null terminator '\0' */
    int stlen = (int)strlen(st) + 1;

    if (maxlen > 0 && maxlen < stlen) {
      stlen = maxlen;
    }

    prop->data.pointer = MEM_mallocN((size_t)stlen, "id property string 2");
    prop->len = prop->totallen = stlen;
    BLI_strncpy(prop->data.pointer, st, (size_t)stlen);
  }

  prop->type = IDP_STRING;
  BLI_strncpy(prop->name, name, MAX_IDPROP_NAME);

  return prop;
}

static IDProperty *IDP_CopyString(const IDProperty *prop, const int flag)
{
  BLI_assert(prop->type == IDP_STRING);
  IDProperty *newp = idp_generic_copy(prop, flag);

  if (prop->data.pointer) {
    newp->data.pointer = MEM_dupallocN(prop->data.pointer);
  }
  newp->len = prop->len;
  newp->subtype = prop->subtype;
  newp->totallen = prop->totallen;

  return newp;
}

void IDP_AssignString(IDProperty *prop, const char *st, int maxlen)
{
  BLI_assert(prop->type == IDP_STRING);
  int stlen = (int)strlen(st);
  if (maxlen > 0 && maxlen < stlen) {
    stlen = maxlen;
  }

  if (prop->subtype == IDP_STRING_SUB_BYTE) {
    IDP_ResizeArray(prop, stlen);
    memcpy(prop->data.pointer, st, (size_t)stlen);
  }
  else {
    stlen++;
    IDP_ResizeArray(prop, stlen);
    BLI_strncpy(prop->data.pointer, st, (size_t)stlen);
  }
}

void IDP_ConcatStringC(IDProperty *prop, const char *st)
{
  BLI_assert(prop->type == IDP_STRING);

  int newlen = prop->len + (int)strlen(st);
  /* We have to remember that prop->len includes the null byte for strings.
   * so there's no need to add +1 to the resize function. */
  IDP_ResizeArray(prop, newlen);
  strcat(prop->data.pointer, st);
}

void IDP_ConcatString(IDProperty *str1, IDProperty *append)
{
  BLI_assert(append->type == IDP_STRING);

  /* Since ->len for strings includes the NULL byte, we have to subtract one or
   * we'll get an extra null byte after each concatenation operation. */
  int newlen = str1->len + append->len - 1;
  IDP_ResizeArray(str1, newlen);
  strcat(str1->data.pointer, append->data.pointer);
}

void IDP_FreeString(IDProperty *prop)
{
  BLI_assert(prop->type == IDP_STRING);

  if (prop->data.pointer) {
    MEM_freeN(prop->data.pointer);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID Type (IDProperty ID API)
 * \{ */

static IDProperty *IDP_CopyID(const IDProperty *prop, const int flag)
{
  BLI_assert(prop->type == IDP_ID);
  IDProperty *newp = idp_generic_copy(prop, flag);

  newp->data.pointer = prop->data.pointer;
  if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
    id_us_plus(IDP_Id(newp));
  }

  return newp;
}

void IDP_AssignID(IDProperty *prop, ID *id, const int flag)
{
  BLI_assert(prop->type == IDP_ID);

  if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0 && IDP_Id(prop) != NULL) {
    id_us_min(IDP_Id(prop));
  }

  prop->data.pointer = id;

  if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
    id_us_plus(IDP_Id(prop));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Group Functions (IDProperty Group API)
 * \{ */

/**
 * Checks if a property with the same name as prop exists, and if so replaces it.
 */
static IDProperty *IDP_CopyGroup(const IDProperty *prop, const int flag)
{
  BLI_assert(prop->type == IDP_GROUP);
  IDProperty *newp = idp_generic_copy(prop, flag);
  newp->len = prop->len;
  newp->subtype = prop->subtype;

  LISTBASE_FOREACH (IDProperty *, link, &prop->data.group) {
    BLI_addtail(&newp->data.group, IDP_CopyProperty_ex(link, flag));
  }

  return newp;
}

void IDP_SyncGroupValues(IDProperty *dest, const IDProperty *src)
{
  BLI_assert(dest->type == IDP_GROUP);
  BLI_assert(src->type == IDP_GROUP);

  LISTBASE_FOREACH (IDProperty *, prop, &src->data.group) {
    IDProperty *other = BLI_findstring(&dest->data.group, prop->name, offsetof(IDProperty, name));
    if (other && prop->type == other->type) {
      switch (prop->type) {
        case IDP_INT:
        case IDP_FLOAT:
        case IDP_DOUBLE:
          other->data = prop->data;
          break;
        case IDP_GROUP:
          IDP_SyncGroupValues(other, prop);
          break;
        default: {
          BLI_insertlinkreplace(&dest->data.group, other, IDP_CopyProperty(prop));
          IDP_FreeProperty(other);
          break;
        }
      }
    }
  }
}

void IDP_SyncGroupTypes(IDProperty *dest, const IDProperty *src, const bool do_arraylen)
{
  LISTBASE_FOREACH_MUTABLE (IDProperty *, prop_dst, &dest->data.group) {
    const IDProperty *prop_src = IDP_GetPropertyFromGroup((IDProperty *)src, prop_dst->name);
    if (prop_src != NULL) {
      /* check of we should replace? */
      if ((prop_dst->type != prop_src->type || prop_dst->subtype != prop_src->subtype) ||
          (do_arraylen && ELEM(prop_dst->type, IDP_ARRAY, IDP_IDPARRAY) &&
           (prop_src->len != prop_dst->len))) {
        BLI_insertlinkreplace(&dest->data.group, prop_dst, IDP_CopyProperty(prop_src));
        IDP_FreeProperty(prop_dst);
      }
      else if (prop_dst->type == IDP_GROUP) {
        IDP_SyncGroupTypes(prop_dst, prop_src, do_arraylen);
      }
    }
    else {
      IDP_FreeFromGroup(dest, prop_dst);
    }
  }
}

void IDP_ReplaceGroupInGroup(IDProperty *dest, const IDProperty *src)
{
  BLI_assert(dest->type == IDP_GROUP);
  BLI_assert(src->type == IDP_GROUP);

  LISTBASE_FOREACH (IDProperty *, prop, &src->data.group) {
    IDProperty *loop;
    for (loop = dest->data.group.first; loop; loop = loop->next) {
      if (STREQ(loop->name, prop->name)) {
        BLI_insertlinkreplace(&dest->data.group, loop, IDP_CopyProperty(prop));
        IDP_FreeProperty(loop);
        break;
      }
    }

    /* only add at end if not added yet */
    if (loop == NULL) {
      IDProperty *copy = IDP_CopyProperty(prop);
      dest->len++;
      BLI_addtail(&dest->data.group, copy);
    }
  }
}

void IDP_ReplaceInGroup_ex(IDProperty *group, IDProperty *prop, IDProperty *prop_exist)
{
  BLI_assert(group->type == IDP_GROUP);
  BLI_assert(prop_exist == IDP_GetPropertyFromGroup(group, prop->name));

  if (prop_exist != NULL) {
    BLI_insertlinkreplace(&group->data.group, prop_exist, prop);
    IDP_FreeProperty(prop_exist);
  }
  else {
    group->len++;
    BLI_addtail(&group->data.group, prop);
  }
}

void IDP_ReplaceInGroup(IDProperty *group, IDProperty *prop)
{
  IDProperty *prop_exist = IDP_GetPropertyFromGroup(group, prop->name);

  IDP_ReplaceInGroup_ex(group, prop, prop_exist);
}

void IDP_MergeGroup_ex(IDProperty *dest,
                       const IDProperty *src,
                       const bool do_overwrite,
                       const int flag)
{
  BLI_assert(dest->type == IDP_GROUP);
  BLI_assert(src->type == IDP_GROUP);

  if (do_overwrite) {
    LISTBASE_FOREACH (IDProperty *, prop, &src->data.group) {
      if (prop->type == IDP_GROUP) {
        IDProperty *prop_exist = IDP_GetPropertyFromGroup(dest, prop->name);

        if (prop_exist != NULL) {
          IDP_MergeGroup_ex(prop_exist, prop, do_overwrite, flag);
          continue;
        }
      }

      IDProperty *copy = IDP_CopyProperty_ex(prop, flag);
      IDP_ReplaceInGroup(dest, copy);
    }
  }
  else {
    LISTBASE_FOREACH (IDProperty *, prop, &src->data.group) {
      IDProperty *prop_exist = IDP_GetPropertyFromGroup(dest, prop->name);
      if (prop_exist != NULL) {
        if (prop->type == IDP_GROUP) {
          IDP_MergeGroup_ex(prop_exist, prop, do_overwrite, flag);
          continue;
        }
      }
      else {
        IDProperty *copy = IDP_CopyProperty_ex(prop, flag);
        dest->len++;
        BLI_addtail(&dest->data.group, copy);
      }
    }
  }
}

void IDP_MergeGroup(IDProperty *dest, const IDProperty *src, const bool do_overwrite)
{
  IDP_MergeGroup_ex(dest, src, do_overwrite, 0);
}

bool IDP_AddToGroup(IDProperty *group, IDProperty *prop)
{
  BLI_assert(group->type == IDP_GROUP);

  if (IDP_GetPropertyFromGroup(group, prop->name) == NULL) {
    group->len++;
    BLI_addtail(&group->data.group, prop);
    return true;
  }

  return false;
}

bool IDP_InsertToGroup(IDProperty *group, IDProperty *previous, IDProperty *pnew)
{
  BLI_assert(group->type == IDP_GROUP);

  if (IDP_GetPropertyFromGroup(group, pnew->name) == NULL) {
    group->len++;
    BLI_insertlinkafter(&group->data.group, previous, pnew);
    return true;
  }

  return false;
}

void IDP_RemoveFromGroup(IDProperty *group, IDProperty *prop)
{
  BLI_assert(group->type == IDP_GROUP);
  BLI_assert(BLI_findindex(&group->data.group, prop) != -1);

  group->len--;
  BLI_remlink(&group->data.group, prop);
}

void IDP_FreeFromGroup(IDProperty *group, IDProperty *prop)
{
  IDP_RemoveFromGroup(group, prop);
  IDP_FreeProperty(prop);
}

IDProperty *IDP_GetPropertyFromGroup(const IDProperty *prop, const char *name)
{
  BLI_assert(prop->type == IDP_GROUP);

  return (IDProperty *)BLI_findstring(&prop->data.group, name, offsetof(IDProperty, name));
}
IDProperty *IDP_GetPropertyTypeFromGroup(const IDProperty *prop, const char *name, const char type)
{
  IDProperty *idprop = IDP_GetPropertyFromGroup(prop, name);
  return (idprop && idprop->type == type) ? idprop : NULL;
}

/* Ok, the way things work, Groups free the ID Property structs of their children.
 * This is because all ID Property freeing functions free only direct data (not the ID Property
 * struct itself), but for Groups the child properties *are* considered
 * direct data. */
static void IDP_FreeGroup(IDProperty *prop, const bool do_id_user)
{
  BLI_assert(prop->type == IDP_GROUP);

  LISTBASE_FOREACH (IDProperty *, loop, &prop->data.group) {
    IDP_FreePropertyContent_ex(loop, do_id_user);
  }
  BLI_freelistN(&prop->data.group);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main Functions  (IDProperty Main API)
 * \{ */

int IDP_coerce_to_int_or_zero(const IDProperty *prop)
{
  switch (prop->type) {
    case IDP_INT:
      return IDP_Int(prop);
    case IDP_DOUBLE:
      return (int)IDP_Double(prop);
    case IDP_FLOAT:
      return (int)IDP_Float(prop);
    default:
      return 0;
  }
}

double IDP_coerce_to_double_or_zero(const IDProperty *prop)
{
  switch (prop->type) {
    case IDP_DOUBLE:
      return IDP_Double(prop);
    case IDP_FLOAT:
      return (double)IDP_Float(prop);
    case IDP_INT:
      return (double)IDP_Int(prop);
    default:
      return 0.0;
  }
}

float IDP_coerce_to_float_or_zero(const IDProperty *prop)
{
  switch (prop->type) {
    case IDP_FLOAT:
      return IDP_Float(prop);
    case IDP_DOUBLE:
      return (float)IDP_Double(prop);
    case IDP_INT:
      return (float)IDP_Int(prop);
    default:
      return 0.0f;
  }
}

IDProperty *IDP_CopyProperty_ex(const IDProperty *prop, const int flag)
{
  switch (prop->type) {
    case IDP_GROUP:
      return IDP_CopyGroup(prop, flag);
    case IDP_STRING:
      return IDP_CopyString(prop, flag);
    case IDP_ID:
      return IDP_CopyID(prop, flag);
    case IDP_ARRAY:
      return IDP_CopyArray(prop, flag);
    case IDP_IDPARRAY:
      return IDP_CopyIDPArray(prop, flag);
    default:
      return idp_generic_copy(prop, flag);
  }
}

IDProperty *IDP_CopyProperty(const IDProperty *prop)
{
  return IDP_CopyProperty_ex(prop, 0);
}

void IDP_CopyPropertyContent(IDProperty *dst, IDProperty *src)
{
  IDProperty *idprop_tmp = IDP_CopyProperty(src);
  idprop_tmp->prev = dst->prev;
  idprop_tmp->next = dst->next;
  SWAP(IDProperty, *dst, *idprop_tmp);
  IDP_FreeProperty(idprop_tmp);
}

IDProperty *IDP_GetProperties(ID *id, const bool create_if_needed)
{
  if (id->properties) {
    return id->properties;
  }

  if (create_if_needed) {
    id->properties = MEM_callocN(sizeof(IDProperty), "IDProperty");
    id->properties->type = IDP_GROUP;
    /* NOTE(@campbellbarton): Don't overwrite the data's name and type
     * some functions might need this if they
     * don't have a real ID, should be named elsewhere. */
    // strcpy(id->name, "top_level_group");
  }
  return id->properties;
}

bool IDP_EqualsProperties_ex(IDProperty *prop1, IDProperty *prop2, const bool is_strict)
{
  if (prop1 == NULL && prop2 == NULL) {
    return true;
  }
  if (prop1 == NULL || prop2 == NULL) {
    return is_strict ? false : true;
  }
  if (prop1->type != prop2->type) {
    return false;
  }

  switch (prop1->type) {
    case IDP_INT:
      return (IDP_Int(prop1) == IDP_Int(prop2));
    case IDP_FLOAT:
#if !defined(NDEBUG) && defined(WITH_PYTHON)
    {
      float p1 = IDP_Float(prop1);
      float p2 = IDP_Float(prop2);
      if ((p1 != p2) && ((fabsf(p1 - p2) / max_ff(p1, p2)) < 0.001f)) {
        printf(
            "WARNING: Comparing two float properties that have nearly the same value (%f vs. "
            "%f)\n",
            p1,
            p2);
        printf("    p1: ");
        IDP_print(prop1);
        printf("    p2: ");
        IDP_print(prop2);
      }
    }
#endif
      return (IDP_Float(prop1) == IDP_Float(prop2));
    case IDP_DOUBLE:
      return (IDP_Double(prop1) == IDP_Double(prop2));
    case IDP_STRING: {
      return (((prop1->len == prop2->len) &&
               STREQLEN(IDP_String(prop1), IDP_String(prop2), (size_t)prop1->len)));
    }
    case IDP_ARRAY:
      if (prop1->len == prop2->len && prop1->subtype == prop2->subtype) {
        return (memcmp(IDP_Array(prop1),
                       IDP_Array(prop2),
                       idp_size_table[(int)prop1->subtype] * (size_t)prop1->len) == 0);
      }
      return false;
    case IDP_GROUP: {
      if (is_strict && prop1->len != prop2->len) {
        return false;
      }

      LISTBASE_FOREACH (IDProperty *, link1, &prop1->data.group) {
        IDProperty *link2 = IDP_GetPropertyFromGroup(prop2, link1->name);

        if (!IDP_EqualsProperties_ex(link1, link2, is_strict)) {
          return false;
        }
      }

      return true;
    }
    case IDP_IDPARRAY: {
      IDProperty *array1 = IDP_IDPArray(prop1);
      IDProperty *array2 = IDP_IDPArray(prop2);

      if (prop1->len != prop2->len) {
        return false;
      }

      for (int i = 0; i < prop1->len; i++) {
        if (!IDP_EqualsProperties_ex(&array1[i], &array2[i], is_strict)) {
          return false;
        }
      }
      return true;
    }
    case IDP_ID:
      return (IDP_Id(prop1) == IDP_Id(prop2));
    default:
      BLI_assert_unreachable();
      break;
  }

  return true;
}

bool IDP_EqualsProperties(IDProperty *prop1, IDProperty *prop2)
{
  return IDP_EqualsProperties_ex(prop1, prop2, true);
}

IDProperty *IDP_New(const char type, const IDPropertyTemplate *val, const char *name)
{
  IDProperty *prop = NULL;

  switch (type) {
    case IDP_INT:
      prop = MEM_callocN(sizeof(IDProperty), "IDProperty int");
      prop->data.val = val->i;
      break;
    case IDP_FLOAT:
      prop = MEM_callocN(sizeof(IDProperty), "IDProperty float");
      *(float *)&prop->data.val = val->f;
      break;
    case IDP_DOUBLE:
      prop = MEM_callocN(sizeof(IDProperty), "IDProperty double");
      *(double *)&prop->data.val = val->d;
      break;
    case IDP_ARRAY: {
      /* for now, we only support float and int and double arrays */
      if (ELEM(val->array.type, IDP_FLOAT, IDP_INT, IDP_DOUBLE, IDP_GROUP)) {
        prop = MEM_callocN(sizeof(IDProperty), "IDProperty array");
        prop->subtype = val->array.type;
        if (val->array.len) {
          prop->data.pointer = MEM_callocN(
              idp_size_table[val->array.type] * (size_t)val->array.len, "id property array");
        }
        prop->len = prop->totallen = val->array.len;
        break;
      }
      CLOG_ERROR(&LOG, "bad array type.");
      return NULL;
    }
    case IDP_STRING: {
      const char *st = val->string.str;

      prop = MEM_callocN(sizeof(IDProperty), "IDProperty string");
      if (val->string.subtype == IDP_STRING_SUB_BYTE) {
        /* NOTE: Intentionally not null terminated. */
        if (st == NULL) {
          prop->data.pointer = MEM_mallocN(DEFAULT_ALLOC_FOR_NULL_STRINGS, "id property string 1");
          *IDP_String(prop) = '\0';
          prop->totallen = DEFAULT_ALLOC_FOR_NULL_STRINGS;
          prop->len = 0;
        }
        else {
          prop->data.pointer = MEM_mallocN((size_t)val->string.len, "id property string 2");
          prop->len = prop->totallen = val->string.len;
          memcpy(prop->data.pointer, st, (size_t)val->string.len);
        }
        prop->subtype = IDP_STRING_SUB_BYTE;
      }
      else {
        if (st == NULL || val->string.len <= 1) {
          prop->data.pointer = MEM_mallocN(DEFAULT_ALLOC_FOR_NULL_STRINGS, "id property string 1");
          *IDP_String(prop) = '\0';
          prop->totallen = DEFAULT_ALLOC_FOR_NULL_STRINGS;
          /* NULL string, has len of 1 to account for null byte. */
          prop->len = 1;
        }
        else {
          BLI_assert((int)val->string.len <= (int)strlen(st) + 1);
          prop->data.pointer = MEM_mallocN((size_t)val->string.len, "id property string 3");
          memcpy(prop->data.pointer, st, (size_t)val->string.len - 1);
          IDP_String(prop)[val->string.len - 1] = '\0';
          prop->len = prop->totallen = val->string.len;
        }
        prop->subtype = IDP_STRING_SUB_UTF8;
      }
      break;
    }
    case IDP_GROUP: {
      /* Values are set properly by calloc. */
      prop = MEM_callocN(sizeof(IDProperty), "IDProperty group");
      break;
    }
    case IDP_ID: {
      prop = MEM_callocN(sizeof(IDProperty), "IDProperty datablock");
      prop->data.pointer = (void *)val->id;
      prop->type = IDP_ID;
      id_us_plus(IDP_Id(prop));
      break;
    }
    default: {
      prop = MEM_callocN(sizeof(IDProperty), "IDProperty array");
      break;
    }
  }

  prop->type = type;
  BLI_strncpy(prop->name, name, MAX_IDPROP_NAME);

  return prop;
}

void IDP_ui_data_free_unique_contents(IDPropertyUIData *ui_data,
                                      const eIDPropertyUIDataType type,
                                      const IDPropertyUIData *other)
{
  if (ui_data->description != other->description) {
    MEM_SAFE_FREE(ui_data->description);
  }

  switch (type) {
    case IDP_UI_DATA_TYPE_STRING: {
      const IDPropertyUIDataString *other_string = (const IDPropertyUIDataString *)other;
      IDPropertyUIDataString *ui_data_string = (IDPropertyUIDataString *)ui_data;
      if (ui_data_string->default_value != other_string->default_value) {
        MEM_SAFE_FREE(ui_data_string->default_value);
      }
      break;
    }
    case IDP_UI_DATA_TYPE_ID: {
      break;
    }
    case IDP_UI_DATA_TYPE_INT: {
      const IDPropertyUIDataInt *other_int = (const IDPropertyUIDataInt *)other;
      IDPropertyUIDataInt *ui_data_int = (IDPropertyUIDataInt *)ui_data;
      if (ui_data_int->default_array != other_int->default_array) {
        MEM_SAFE_FREE(ui_data_int->default_array);
      }
      break;
    }
    case IDP_UI_DATA_TYPE_FLOAT: {
      const IDPropertyUIDataFloat *other_float = (const IDPropertyUIDataFloat *)other;
      IDPropertyUIDataFloat *ui_data_float = (IDPropertyUIDataFloat *)ui_data;
      if (ui_data_float->default_array != other_float->default_array) {
        MEM_SAFE_FREE(ui_data_float->default_array);
      }
      break;
    }
    case IDP_UI_DATA_TYPE_UNSUPPORTED: {
      break;
    }
  }
}

void IDP_ui_data_free(IDProperty *prop)
{
  switch (IDP_ui_data_type(prop)) {
    case IDP_UI_DATA_TYPE_STRING: {
      IDPropertyUIDataString *ui_data_string = (IDPropertyUIDataString *)prop->ui_data;
      MEM_SAFE_FREE(ui_data_string->default_value);
      break;
    }
    case IDP_UI_DATA_TYPE_ID: {
      break;
    }
    case IDP_UI_DATA_TYPE_INT: {
      IDPropertyUIDataInt *ui_data_int = (IDPropertyUIDataInt *)prop->ui_data;
      MEM_SAFE_FREE(ui_data_int->default_array);
      break;
    }
    case IDP_UI_DATA_TYPE_FLOAT: {
      IDPropertyUIDataFloat *ui_data_float = (IDPropertyUIDataFloat *)prop->ui_data;
      MEM_SAFE_FREE(ui_data_float->default_array);
      break;
    }
    case IDP_UI_DATA_TYPE_UNSUPPORTED: {
      break;
    }
  }

  MEM_SAFE_FREE(prop->ui_data->description);

  MEM_freeN(prop->ui_data);
  prop->ui_data = NULL;
}

void IDP_FreePropertyContent_ex(IDProperty *prop, const bool do_id_user)
{
  switch (prop->type) {
    case IDP_ARRAY:
      IDP_FreeArray(prop);
      break;
    case IDP_STRING:
      IDP_FreeString(prop);
      break;
    case IDP_GROUP:
      IDP_FreeGroup(prop, do_id_user);
      break;
    case IDP_IDPARRAY:
      IDP_FreeIDPArray(prop, do_id_user);
      break;
    case IDP_ID:
      if (do_id_user) {
        id_us_min(IDP_Id(prop));
      }
      break;
  }

  if (prop->ui_data != NULL) {
    IDP_ui_data_free(prop);
  }
}

void IDP_FreePropertyContent(IDProperty *prop)
{
  IDP_FreePropertyContent_ex(prop, true);
}

void IDP_FreeProperty_ex(IDProperty *prop, const bool do_id_user)
{
  IDP_FreePropertyContent_ex(prop, do_id_user);
  MEM_freeN(prop);
}

void IDP_FreeProperty(IDProperty *prop)
{
  IDP_FreePropertyContent(prop);
  MEM_freeN(prop);
}

void IDP_ClearProperty(IDProperty *prop)
{
  IDP_FreePropertyContent(prop);
  prop->data.pointer = NULL;
  prop->len = prop->totallen = 0;
}

void IDP_Reset(IDProperty *prop, const IDProperty *reference)
{
  if (prop == NULL) {
    return;
  }
  IDP_ClearProperty(prop);
  if (reference != NULL) {
    IDP_MergeGroup(prop, reference, true);
  }
}

void IDP_foreach_property(IDProperty *id_property_root,
                          const int type_filter,
                          IDPForeachPropertyCallback callback,
                          void *user_data)
{
  if (!id_property_root) {
    return;
  }

  if (type_filter == 0 || (1 << id_property_root->type) & type_filter) {
    callback(id_property_root, user_data);
  }

  /* Recursive call into container types of ID properties. */
  switch (id_property_root->type) {
    case IDP_GROUP: {
      LISTBASE_FOREACH (IDProperty *, loop, &id_property_root->data.group) {
        IDP_foreach_property(loop, type_filter, callback, user_data);
      }
      break;
    }
    case IDP_IDPARRAY: {
      IDProperty *loop = IDP_Array(id_property_root);
      for (int i = 0; i < id_property_root->len; i++) {
        IDP_foreach_property(&loop[i], type_filter, callback, user_data);
      }
      break;
    }
    default:
      break; /* Nothing to do here with other types of IDProperties... */
  }
}

void IDP_WriteProperty_OnlyData(const IDProperty *prop, BlendWriter *writer);

static void write_ui_data(const IDProperty *prop, BlendWriter *writer)
{
  IDPropertyUIData *ui_data = prop->ui_data;

  BLO_write_string(writer, ui_data->description);

  switch (IDP_ui_data_type(prop)) {
    case IDP_UI_DATA_TYPE_STRING: {
      IDPropertyUIDataString *ui_data_string = (IDPropertyUIDataString *)ui_data;
      BLO_write_string(writer, ui_data_string->default_value);
      BLO_write_struct(writer, IDPropertyUIDataString, ui_data);
      break;
    }
    case IDP_UI_DATA_TYPE_ID: {
      BLO_write_struct(writer, IDPropertyUIDataID, ui_data);
      break;
    }
    case IDP_UI_DATA_TYPE_INT: {
      IDPropertyUIDataInt *ui_data_int = (IDPropertyUIDataInt *)ui_data;
      if (prop->type == IDP_ARRAY) {
        BLO_write_int32_array(
            writer, (uint)ui_data_int->default_array_len, (int32_t *)ui_data_int->default_array);
      }
      BLO_write_struct(writer, IDPropertyUIDataInt, ui_data);
      break;
    }
    case IDP_UI_DATA_TYPE_FLOAT: {
      IDPropertyUIDataFloat *ui_data_float = (IDPropertyUIDataFloat *)ui_data;
      if (prop->type == IDP_ARRAY) {
        BLO_write_double_array(
            writer, (uint)ui_data_float->default_array_len, ui_data_float->default_array);
      }
      BLO_write_struct(writer, IDPropertyUIDataFloat, ui_data);
      break;
    }
    case IDP_UI_DATA_TYPE_UNSUPPORTED: {
      BLI_assert_unreachable();
      break;
    }
  }
}

static void IDP_WriteArray(const IDProperty *prop, BlendWriter *writer)
{
  /* Remember to set #IDProperty.totallen to len in the linking code! */
  if (prop->data.pointer) {
    BLO_write_raw(writer, MEM_allocN_len(prop->data.pointer), prop->data.pointer);

    if (prop->subtype == IDP_GROUP) {
      IDProperty **array = prop->data.pointer;
      int a;

      for (a = 0; a < prop->len; a++) {
        IDP_BlendWrite(writer, array[a]);
      }
    }
  }
}

static void IDP_WriteIDPArray(const IDProperty *prop, BlendWriter *writer)
{
  /* Remember to set #IDProperty.totallen to len in the linking code! */
  if (prop->data.pointer) {
    const IDProperty *array = prop->data.pointer;

    BLO_write_struct_array(writer, IDProperty, prop->len, array);

    for (int a = 0; a < prop->len; a++) {
      IDP_WriteProperty_OnlyData(&array[a], writer);
    }
  }
}

static void IDP_WriteString(const IDProperty *prop, BlendWriter *writer)
{
  /* Remember to set #IDProperty.totallen to len in the linking code! */
  BLO_write_raw(writer, (size_t)prop->len, prop->data.pointer);
}

static void IDP_WriteGroup(const IDProperty *prop, BlendWriter *writer)
{
  LISTBASE_FOREACH (IDProperty *, loop, &prop->data.group) {
    IDP_BlendWrite(writer, loop);
  }
}

/* Functions to read/write ID Properties */
void IDP_WriteProperty_OnlyData(const IDProperty *prop, BlendWriter *writer)
{
  switch (prop->type) {
    case IDP_GROUP:
      IDP_WriteGroup(prop, writer);
      break;
    case IDP_STRING:
      IDP_WriteString(prop, writer);
      break;
    case IDP_ARRAY:
      IDP_WriteArray(prop, writer);
      break;
    case IDP_IDPARRAY:
      IDP_WriteIDPArray(prop, writer);
      break;
  }
  if (prop->ui_data != NULL) {
    write_ui_data(prop, writer);
  }
}

void IDP_BlendWrite(BlendWriter *writer, const IDProperty *prop)
{
  BLO_write_struct(writer, IDProperty, prop);
  IDP_WriteProperty_OnlyData(prop, writer);
}

static void IDP_DirectLinkProperty(IDProperty *prop, BlendDataReader *reader);

static void read_ui_data(IDProperty *prop, BlendDataReader *reader)
{
  BLO_read_data_address(reader, &prop->ui_data);
  BLO_read_data_address(reader, &prop->ui_data->description);

  switch (IDP_ui_data_type(prop)) {
    case IDP_UI_DATA_TYPE_STRING: {
      IDPropertyUIDataString *ui_data_string = (IDPropertyUIDataString *)prop->ui_data;
      BLO_read_data_address(reader, &ui_data_string->default_value);
      break;
    }
    case IDP_UI_DATA_TYPE_ID: {
      break;
    }
    case IDP_UI_DATA_TYPE_INT: {
      IDPropertyUIDataInt *ui_data_int = (IDPropertyUIDataInt *)prop->ui_data;
      if (prop->type == IDP_ARRAY) {
        BLO_read_int32_array(
            reader, ui_data_int->default_array_len, (int **)&ui_data_int->default_array);
      }
      break;
    }
    case IDP_UI_DATA_TYPE_FLOAT: {
      IDPropertyUIDataFloat *ui_data_float = (IDPropertyUIDataFloat *)prop->ui_data;
      if (prop->type == IDP_ARRAY) {
        BLO_read_double_array(
            reader, ui_data_float->default_array_len, (double **)&ui_data_float->default_array);
      }
      break;
    }
    case IDP_UI_DATA_TYPE_UNSUPPORTED: {
      BLI_assert_unreachable();
      break;
    }
  }
}

static void IDP_DirectLinkIDPArray(IDProperty *prop, BlendDataReader *reader)
{
  /* since we didn't save the extra buffer, set totallen to len */
  prop->totallen = prop->len;
  BLO_read_data_address(reader, &prop->data.pointer);

  IDProperty *array = (IDProperty *)prop->data.pointer;

  /* NOTE:, idp-arrays didn't exist in 2.4x, so the pointer will be cleared
   * there's not really anything we can do to correct this, at least don't crash */
  if (array == NULL) {
    prop->len = 0;
    prop->totallen = 0;
  }

  for (int i = 0; i < prop->len; i++) {
    IDP_DirectLinkProperty(&array[i], reader);
  }
}

static void IDP_DirectLinkArray(IDProperty *prop, BlendDataReader *reader)
{
  /* since we didn't save the extra buffer, set totallen to len */
  prop->totallen = prop->len;

  if (prop->subtype == IDP_GROUP) {
    BLO_read_pointer_array(reader, &prop->data.pointer);
    IDProperty **array = prop->data.pointer;

    for (int i = 0; i < prop->len; i++) {
      IDP_DirectLinkProperty(array[i], reader);
    }
  }
  else if (prop->subtype == IDP_DOUBLE) {
    BLO_read_double_array(reader, prop->len, (double **)&prop->data.pointer);
  }
  else {
    /* also used for floats */
    BLO_read_int32_array(reader, prop->len, (int **)&prop->data.pointer);
  }
}

static void IDP_DirectLinkString(IDProperty *prop, BlendDataReader *reader)
{
  /* Since we didn't save the extra string buffer, set totallen to len. */
  prop->totallen = prop->len;
  BLO_read_data_address(reader, &prop->data.pointer);
}

static void IDP_DirectLinkGroup(IDProperty *prop, BlendDataReader *reader)
{
  ListBase *lb = &prop->data.group;

  BLO_read_list(reader, lb);

  /* Link child id properties now. */
  LISTBASE_FOREACH (IDProperty *, loop, &prop->data.group) {
    IDP_DirectLinkProperty(loop, reader);
  }
}

static void IDP_DirectLinkProperty(IDProperty *prop, BlendDataReader *reader)
{
  switch (prop->type) {
    case IDP_GROUP:
      IDP_DirectLinkGroup(prop, reader);
      break;
    case IDP_STRING:
      IDP_DirectLinkString(prop, reader);
      break;
    case IDP_ARRAY:
      IDP_DirectLinkArray(prop, reader);
      break;
    case IDP_IDPARRAY:
      IDP_DirectLinkIDPArray(prop, reader);
      break;
    case IDP_DOUBLE:
      /* Workaround for doubles.
       * They are stored in the same field as `int val, val2` in the #IDPropertyData struct,
       * they have to deal with endianness specifically.
       *
       * In theory, val and val2 would've already been swapped
       * if switch_endian is true, so we have to first un-swap
       * them then re-swap them as a single 64-bit entity. */
      if (BLO_read_requires_endian_switch(reader)) {
        BLI_endian_switch_int32(&prop->data.val);
        BLI_endian_switch_int32(&prop->data.val2);
        BLI_endian_switch_int64((int64_t *)&prop->data.val);
      }
      break;
    case IDP_INT:
    case IDP_FLOAT:
    case IDP_ID:
      break; /* Nothing special to do here. */
    default:
      /* Unknown IDP type, nuke it (we cannot handle unknown types everywhere in code,
       * IDP are way too polymorphic to do it safely. */
      printf(
          "%s: found unknown IDProperty type %d, reset to Integer one !\n", __func__, prop->type);
      /* NOTE: we do not attempt to free unknown prop, we have no way to know how to do that! */
      prop->type = IDP_INT;
      prop->subtype = 0;
      IDP_Int(prop) = 0;
  }

  if (prop->ui_data != NULL) {
    read_ui_data(prop, reader);
  }
}

void IDP_BlendReadData_impl(BlendDataReader *reader, IDProperty **prop, const char *caller_func_id)
{
  if (*prop) {
    if ((*prop)->type == IDP_GROUP) {
      IDP_DirectLinkGroup(*prop, reader);
    }
    else {
      /* corrupt file! */
      printf("%s: found non group data, freeing type %d!\n", caller_func_id, (*prop)->type);
      /* don't risk id, data's likely corrupt. */
      // IDP_FreePropertyContent(*prop);
      *prop = NULL;
    }
  }
}

void IDP_BlendReadLib(BlendLibReader *reader, Library *lib, IDProperty *prop)
{
  if (!prop) {
    return;
  }

  switch (prop->type) {
    case IDP_ID: /* PointerProperty */
    {
      void *newaddr = BLO_read_get_new_id_address(reader, lib, IDP_Id(prop));
      if (IDP_Id(prop) && !newaddr && G.debug) {
        printf("Error while loading \"%s\". Data not found in file!\n", prop->name);
      }
      prop->data.pointer = newaddr;
      break;
    }
    case IDP_IDPARRAY: /* CollectionProperty */
    {
      IDProperty *idp_array = IDP_IDPArray(prop);
      for (int i = 0; i < prop->len; i++) {
        IDP_BlendReadLib(reader, lib, &(idp_array[i]));
      }
      break;
    }
    case IDP_GROUP: /* PointerProperty */
    {
      LISTBASE_FOREACH (IDProperty *, loop, &prop->data.group) {
        IDP_BlendReadLib(reader, lib, loop);
      }
      break;
    }
    default:
      break; /* Nothing to do for other IDProps. */
  }
}

void IDP_BlendReadExpand(struct BlendExpander *expander, IDProperty *prop)
{
  if (!prop) {
    return;
  }

  switch (prop->type) {
    case IDP_ID:
      BLO_expand(expander, IDP_Id(prop));
      break;
    case IDP_IDPARRAY: {
      IDProperty *idp_array = IDP_IDPArray(prop);
      for (int i = 0; i < prop->len; i++) {
        IDP_BlendReadExpand(expander, &idp_array[i]);
      }
      break;
    }
    case IDP_GROUP:
      LISTBASE_FOREACH (IDProperty *, loop, &prop->data.group) {
        IDP_BlendReadExpand(expander, loop);
      }
      break;
  }
}

eIDPropertyUIDataType IDP_ui_data_type(const IDProperty *prop)
{
  if (prop->type == IDP_STRING) {
    return IDP_UI_DATA_TYPE_STRING;
  }
  if (prop->type == IDP_ID) {
    return IDP_UI_DATA_TYPE_ID;
  }
  if (prop->type == IDP_INT || (prop->type == IDP_ARRAY && prop->subtype == IDP_INT)) {
    return IDP_UI_DATA_TYPE_INT;
  }
  if (ELEM(prop->type, IDP_FLOAT, IDP_DOUBLE) ||
      (prop->type == IDP_ARRAY && ELEM(prop->subtype, IDP_FLOAT, IDP_DOUBLE))) {
    return IDP_UI_DATA_TYPE_FLOAT;
  }
  return IDP_UI_DATA_TYPE_UNSUPPORTED;
}

bool IDP_ui_data_supported(const IDProperty *prop)
{
  return IDP_ui_data_type(prop) != IDP_UI_DATA_TYPE_UNSUPPORTED;
}

IDPropertyUIData *IDP_ui_data_ensure(IDProperty *prop)
{
  if (prop->ui_data != NULL) {
    return prop->ui_data;
  }

  switch (IDP_ui_data_type(prop)) {
    case IDP_UI_DATA_TYPE_STRING: {
      prop->ui_data = MEM_callocN(sizeof(IDPropertyUIDataString), __func__);
      break;
    }
    case IDP_UI_DATA_TYPE_ID: {
      IDPropertyUIDataID *ui_data = MEM_callocN(sizeof(IDPropertyUIDataID), __func__);
      prop->ui_data = (IDPropertyUIData *)ui_data;
      break;
    }
    case IDP_UI_DATA_TYPE_INT: {
      IDPropertyUIDataInt *ui_data = MEM_callocN(sizeof(IDPropertyUIDataInt), __func__);
      ui_data->min = INT_MIN;
      ui_data->max = INT_MAX;
      ui_data->soft_min = INT_MIN;
      ui_data->soft_max = INT_MAX;
      ui_data->step = 1;
      prop->ui_data = (IDPropertyUIData *)ui_data;
      break;
    }
    case IDP_UI_DATA_TYPE_FLOAT: {
      IDPropertyUIDataFloat *ui_data = MEM_callocN(sizeof(IDPropertyUIDataFloat), __func__);
      ui_data->min = -FLT_MAX;
      ui_data->max = FLT_MAX;
      ui_data->soft_min = -FLT_MAX;
      ui_data->soft_max = FLT_MAX;
      ui_data->step = 1.0f;
      ui_data->precision = 3;
      prop->ui_data = (IDPropertyUIData *)ui_data;
      break;
    }
    case IDP_UI_DATA_TYPE_UNSUPPORTED: {
      /* UI data not supported for remaining types, this shouldn't be called in those cases. */
      BLI_assert_unreachable();
      break;
    }
  }

  return prop->ui_data;
}

/** \} */
