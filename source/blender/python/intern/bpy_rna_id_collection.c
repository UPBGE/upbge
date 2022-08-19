/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file adds some helpers related to ID/Main handling, that cannot fit well in RNA itself.
 */

#include <Python.h>
#include <stddef.h>

#include "MEM_guardedalloc.h"

#include "BLI_bitmap.h"
#include "BLI_utildefines.h"

#include "BKE_global.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_main.h"

#include "DNA_ID.h"
/* Those following are only to support hack of not listing some internal
 * 'backward' pointers in generated user_map. */
#include "DNA_key_types.h"
#include "DNA_object_types.h"

#include "WM_api.h"
#include "WM_types.h"

#include "bpy_capi_utils.h"
#include "bpy_rna_id_collection.h"

#include "../generic/py_capi_rna.h"
#include "../generic/py_capi_utils.h"
#include "../generic/python_utildefines.h"

#include "RNA_access.h"
#include "RNA_enum_types.h"
#include "RNA_types.h"

#include "bpy_rna.h"

typedef struct IDUserMapData {
  /** We loop over data-blocks that this ID points to (do build a reverse lookup table) */
  PyObject *py_id_curr;
  ID *id_curr;

  /** Filter the values we add into the set. */
  BLI_bitmap *types_bitmap;

  /** Set to fill in as we iterate. */
  PyObject *user_map;
  /** true when we're only mapping a subset of all the ID's (subset arg is passed). */
  bool is_subset;
} IDUserMapData;

static int id_code_as_index(const short idcode)
{
  return (int)*((ushort *)&idcode);
}

static bool id_check_type(const ID *id, const BLI_bitmap *types_bitmap)
{
  return BLI_BITMAP_TEST_BOOL(types_bitmap, id_code_as_index(GS(id->name)));
}

static int foreach_libblock_id_user_map_callback(LibraryIDLinkCallbackData *cb_data)
{
  ID **id_p = cb_data->id_pointer;

  if (*id_p) {
    IDUserMapData *data = cb_data->user_data;
    const int cb_flag = cb_data->cb_flag;

    if (data->types_bitmap) {
      if (!id_check_type(*id_p, data->types_bitmap)) {
        return IDWALK_RET_NOP;
      }
    }

    if (cb_flag & IDWALK_CB_LOOPBACK) {
      /* We skip loop-back pointers like Key.from here,
       * since it's some internal pointer which is not relevant info for py/API level. */
      return IDWALK_RET_NOP;
    }

    if (cb_flag & IDWALK_CB_EMBEDDED) {
      /* We skip private pointers themselves, like root node trees, we'll 'link' their own ID
       * pointers to their 'ID owner' instead. */
      return IDWALK_RET_NOP;
    }

    PyObject *key = pyrna_id_CreatePyObject(*id_p);

    PyObject *set;
    if ((set = PyDict_GetItem(data->user_map, key)) == NULL) {
      /* limit to key's added already */
      if (data->is_subset) {
        return IDWALK_RET_NOP;
      }

      set = PySet_New(NULL);
      PyDict_SetItem(data->user_map, key, set);
      Py_DECREF(set);
    }
    Py_DECREF(key);

    if (data->py_id_curr == NULL) {
      data->py_id_curr = pyrna_id_CreatePyObject(data->id_curr);
    }

    PySet_Add(set, data->py_id_curr);
  }

  return IDWALK_RET_NOP;
}

PyDoc_STRVAR(bpy_user_map_doc,
             ".. method:: user_map(subset, key_types, value_types)\n"
             "\n"
             "   Returns a mapping of all ID data-blocks in current ``bpy.data`` to a set of all "
             "datablocks using them.\n"
             "\n"
             "   For list of valid set members for key_types & value_types, see: "
             ":class:`bpy.types.KeyingSetPath.id_type`.\n"
             "\n"
             "   :arg subset: When passed, only these data-blocks and their users will be "
             "included as keys/values in the map.\n"
             "   :type subset: sequence\n"
             "   :arg key_types: Filter the keys mapped by ID types.\n"
             "   :type key_types: set of strings\n"
             "   :arg value_types: Filter the values in the set by ID types.\n"
             "   :type value_types: set of strings\n"
             "   :return: dictionary of :class:`bpy.types.ID` instances, with sets of ID's as "
             "their values.\n"
             "   :rtype: dict\n");
static PyObject *bpy_user_map(PyObject *UNUSED(self), PyObject *args, PyObject *kwds)
{
#if 0 /* If someone knows how to get a proper 'self' in that case... */
  BPy_StructRNA *pyrna = (BPy_StructRNA *)self;
  Main *bmain = pyrna->ptr.data;
#else
  Main *bmain = G_MAIN; /* XXX Ugly, but should work! */
#endif
  ListBase *lb;
  ID *id;

  PyObject *subset = NULL;

  PyObject *key_types = NULL;
  PyObject *val_types = NULL;
  BLI_bitmap *key_types_bitmap = NULL;
  BLI_bitmap *val_types_bitmap = NULL;

  PyObject *ret = NULL;

  IDUserMapData data_cb = {NULL};

  static const char *_keywords[] = {"subset", "key_types", "value_types", NULL};
  static _PyArg_Parser _parser = {
      "|$" /* Optional keyword only arguments. */
      "O"  /* `subset` */
      "O!" /* `key_types` */
      "O!" /* `value_types` */
      ":user_map",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(
          args, kwds, &_parser, &subset, &PySet_Type, &key_types, &PySet_Type, &val_types)) {
    return NULL;
  }

  if (key_types) {
    key_types_bitmap = pyrna_enum_bitmap_from_set(
        rna_enum_id_type_items, key_types, sizeof(short), true, USHRT_MAX, "key types");
    if (key_types_bitmap == NULL) {
      goto error;
    }
  }

  if (val_types) {
    val_types_bitmap = pyrna_enum_bitmap_from_set(
        rna_enum_id_type_items, val_types, sizeof(short), true, USHRT_MAX, "value types");
    if (val_types_bitmap == NULL) {
      goto error;
    }
  }

  if (subset) {
    PyObject *subset_fast = PySequence_Fast(subset, "user_map");
    if (subset_fast == NULL) {
      goto error;
    }

    PyObject **subset_array = PySequence_Fast_ITEMS(subset_fast);
    Py_ssize_t subset_len = PySequence_Fast_GET_SIZE(subset_fast);

    data_cb.user_map = _PyDict_NewPresized(subset_len);
    data_cb.is_subset = true;
    for (; subset_len; subset_array++, subset_len--) {
      PyObject *set = PySet_New(NULL);
      PyDict_SetItem(data_cb.user_map, *subset_array, set);
      Py_DECREF(set);
    }
    Py_DECREF(subset_fast);
  }
  else {
    data_cb.user_map = PyDict_New();
  }

  data_cb.types_bitmap = key_types_bitmap;

  FOREACH_MAIN_LISTBASE_BEGIN (bmain, lb) {
    FOREACH_MAIN_LISTBASE_ID_BEGIN (lb, id) {
      /* We cannot skip here in case we have some filter on key types... */
      if (key_types_bitmap == NULL && val_types_bitmap != NULL) {
        if (!id_check_type(id, val_types_bitmap)) {
          break;
        }
      }

      if (!data_cb.is_subset &&
          /* We do not want to pre-add keys of filtered out types. */
          (key_types_bitmap == NULL || id_check_type(id, key_types_bitmap)) &&
          /* We do not want to pre-add keys when we have filter on value types,
           * but not on key types. */
          (val_types_bitmap == NULL || key_types_bitmap != NULL)) {
        PyObject *key = pyrna_id_CreatePyObject(id);
        PyObject *set;

        /* We have to insert the key now,
         * otherwise ID unused would be missing from final dict... */
        if ((set = PyDict_GetItem(data_cb.user_map, key)) == NULL) {
          set = PySet_New(NULL);
          PyDict_SetItem(data_cb.user_map, key, set);
          Py_DECREF(set);
        }
        Py_DECREF(key);
      }

      if (val_types_bitmap != NULL && !id_check_type(id, val_types_bitmap)) {
        continue;
      }

      data_cb.id_curr = id;
      BKE_library_foreach_ID_link(
          NULL, id, foreach_libblock_id_user_map_callback, &data_cb, IDWALK_CB_NOP);

      if (data_cb.py_id_curr) {
        Py_DECREF(data_cb.py_id_curr);
        data_cb.py_id_curr = NULL;
      }
    }
    FOREACH_MAIN_LISTBASE_ID_END;
  }
  FOREACH_MAIN_LISTBASE_ID_END;

  ret = data_cb.user_map;

error:
  if (key_types_bitmap != NULL) {
    MEM_freeN(key_types_bitmap);
  }

  if (val_types_bitmap != NULL) {
    MEM_freeN(val_types_bitmap);
  }

  return ret;
}

PyDoc_STRVAR(bpy_batch_remove_doc,
             ".. method:: batch_remove(ids)\n"
             "\n"
             "   Remove (delete) several IDs at once.\n"
             "\n"
             "   WARNING: Considered experimental feature currently.\n"
             "\n"
             "   Note that this function is quicker than individual calls to :func:`remove()` "
             "(from :class:`bpy.types.BlendData`\n"
             "   ID collections), but less safe/versatile (it can break Blender, e.g. by removing "
             "all scenes...).\n"
             "\n"
             "   :arg ids: Iterables of IDs (types can be mixed).\n"
             "   :type subset: sequence\n");
static PyObject *bpy_batch_remove(PyObject *UNUSED(self), PyObject *args, PyObject *kwds)
{
#if 0 /* If someone knows how to get a proper 'self' in that case... */
  BPy_StructRNA *pyrna = (BPy_StructRNA *)self;
  Main *bmain = pyrna->ptr.data;
#else
  Main *bmain = G_MAIN; /* XXX Ugly, but should work! */
#endif

  PyObject *ids = NULL;

  PyObject *ret = NULL;

  static const char *_keywords[] = {"ids", NULL};
  static _PyArg_Parser _parser = {
      "O" /* `ids` */
      ":batch_remove",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args, kwds, &_parser, &ids)) {
    return ret;
  }

  if (ids) {
    BKE_main_id_tag_all(bmain, LIB_TAG_DOIT, false);

    PyObject *ids_fast = PySequence_Fast(ids, "batch_remove");
    if (ids_fast == NULL) {
      goto error;
    }

    PyObject **ids_array = PySequence_Fast_ITEMS(ids_fast);
    Py_ssize_t ids_len = PySequence_Fast_GET_SIZE(ids_fast);

    for (; ids_len; ids_array++, ids_len--) {
      ID *id;
      if (!pyrna_id_FromPyObject(*ids_array, &id)) {
        PyErr_Format(
            PyExc_TypeError, "Expected an ID type, not %.200s", Py_TYPE(*ids_array)->tp_name);
        Py_DECREF(ids_fast);
        goto error;
      }

      id->tag |= LIB_TAG_DOIT;
    }
    Py_DECREF(ids_fast);

    BKE_id_multi_tagged_delete(bmain);
    /* Force full redraw, mandatory to avoid crashes when running this from UI... */
    WM_main_add_notifier(NC_WINDOW, NULL);
  }
  else {
    goto error;
  }

  Py_INCREF(Py_None);
  ret = Py_None;

error:
  return ret;
}

PyDoc_STRVAR(bpy_orphans_purge_doc,
             ".. method:: orphans_purge()\n"
             "\n"
             "   Remove (delete) all IDs with no user.\n"
             "\n"
             "   :arg do_local_ids: Include unused local IDs in the deletion, defaults to True\n"
             "   :type do_local_ids: bool, optional\n"
             "   :arg do_linked_ids: Include unused linked IDs in the deletion, defaults to True\n"
             "   :type do_linked_ids: bool, optional\n"
             "   :arg do_recursive: Recursively check for unused IDs, ensuring no orphaned one "
             "remain after a single run of that function, defaults to False\n"
             "   :type do_recursive: bool, optional\n"
             "   :return: The number of deleted IDs.\n");
static PyObject *bpy_orphans_purge(PyObject *UNUSED(self), PyObject *args, PyObject *kwds)
{
#if 0 /* If someone knows how to get a proper 'self' in that case... */
  BPy_StructRNA *pyrna = (BPy_StructRNA *)self;
  Main *bmain = pyrna->ptr.data;
#else
  Main *bmain = G_MAIN; /* XXX Ugly, but should work! */
#endif

  int num_tagged[INDEX_ID_MAX] = {0};

  bool do_local_ids = true;
  bool do_linked_ids = true;
  bool do_recursive_cleanup = false;

  static const char *_keywords[] = {"do_local_ids", "do_linked_ids", "do_recursive", NULL};
  static _PyArg_Parser _parser = {
      "|"  /* Optional arguments. */
      "O&" /* `do_local_ids` */
      "O&" /* `do_linked_ids` */
      "O&" /* `do_recursive` */
      ":orphans_purge",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kwds,
                                        &_parser,
                                        PyC_ParseBool,
                                        &do_local_ids,
                                        PyC_ParseBool,
                                        &do_linked_ids,
                                        PyC_ParseBool,
                                        &do_recursive_cleanup)) {
    return NULL;
  }

  /* Tag all IDs to delete. */
  BKE_lib_query_unused_ids_tag(
      bmain, LIB_TAG_DOIT, do_local_ids, do_linked_ids, do_recursive_cleanup, num_tagged);

  if (num_tagged[INDEX_ID_NULL] == 0) {
    return PyLong_FromSize_t(0);
  }

  const size_t num_datablocks_deleted = BKE_id_multi_tagged_delete(bmain);
  /* Force full redraw, mandatory to avoid crashes when running this from UI... */
  WM_main_add_notifier(NC_WINDOW, NULL);

  return PyLong_FromSize_t(num_datablocks_deleted);
}

PyMethodDef BPY_rna_id_collection_user_map_method_def = {
    "user_map",
    (PyCFunction)bpy_user_map,
    METH_STATIC | METH_VARARGS | METH_KEYWORDS,
    bpy_user_map_doc,
};
PyMethodDef BPY_rna_id_collection_batch_remove_method_def = {
    "batch_remove",
    (PyCFunction)bpy_batch_remove,
    METH_STATIC | METH_VARARGS | METH_KEYWORDS,
    bpy_batch_remove_doc,
};
PyMethodDef BPY_rna_id_collection_orphans_purge_method_def = {
    "orphans_purge",
    (PyCFunction)bpy_orphans_purge,
    METH_STATIC | METH_VARARGS | METH_KEYWORDS,
    bpy_orphans_purge_doc,
};
