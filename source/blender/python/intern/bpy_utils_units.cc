/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file defines a singleton py object accessed via 'bpy.utils.units',
 * which exposes various data and functions useful in units handling.
 */

/* Future-proof, See https://docs.python.org/3/c-api/arg.html#strings-and-buffers */
#define PY_SSIZE_T_CLEAN

#include <Python.h>
#include <structmember.h>

#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "bpy_utils_units.hh"

#include "../generic/py_capi_utils.hh"
#include "../generic/python_compat.hh" /* IWYU pragma: keep. */

#include "BKE_unit.hh"

/***** C-defined systems and types *****/

static PyTypeObject BPyUnitsSystemsType;
static PyTypeObject BPyUnitsCategoriesType;

/* XXX: Maybe better as `extern` of `BKE_unit.hh` ? */
static const char *bpyunits_usystem_items[] = {
    "NONE",
    "METRIC",
    "IMPERIAL",
    nullptr,
};

static const char *bpyunits_ucategories_items[] = {
    "NONE",
    "LENGTH",
    "AREA",
    "VOLUME",
    "MASS",
    "ROTATION",
    "TIME",
    "TIME_ABSOLUTE",
    "VELOCITY",
    "ACCELERATION",
    "CAMERA",
    "POWER",
    "TEMPERATURE",
    "WAVELENGTH",
    "COLOR_TEMPERATURE",
    "FREQUENCY",
    nullptr,
};

BLI_STATIC_ASSERT(
    ARRAY_SIZE(bpyunits_ucategories_items) == B_UNIT_TYPE_TOT + 1,
    "`bpyunits_ucategories_items` should match `B_UNIT_` enum items in `BKE_units.h`")

/**
 * These fields are just empty placeholders, actual values get set in initializations functions.
 * This allows us to avoid many handwriting, and above all,
 * to keep all systems/categories definition stuff in `BKE_unit.hh`.
 */
static PyStructSequence_Field bpyunits_systems_fields[ARRAY_SIZE(bpyunits_usystem_items)];
static PyStructSequence_Field bpyunits_categories_fields[ARRAY_SIZE(bpyunits_ucategories_items)];

static PyStructSequence_Desc bpyunits_systems_desc = {
    /*name*/ "bpy.utils.units.systems",
    /*doc*/ "This named tuple contains all predefined unit systems",
    /*fields*/ bpyunits_systems_fields,
    /*n_in_sequence*/ ARRAY_SIZE(bpyunits_systems_fields) - 1,
};
static PyStructSequence_Desc bpyunits_categories_desc = {
    /*name*/ "bpy.utils.units.categories",
    /*doc*/ "This named tuple contains all predefined unit names",
    /*fields*/ bpyunits_categories_fields,
    /*n_in_sequence*/ ARRAY_SIZE(bpyunits_categories_fields) - 1,
};

/**
 * Simple utility function to initialize #PyStructSequence_Desc
 */
static PyObject *py_structseq_from_strings(PyTypeObject *py_type,
                                           PyStructSequence_Desc *py_sseq_desc,
                                           const char **str_items)
{
  PyObject *py_struct_seq;
  int pos = 0;

  const char **str_iter;
  PyStructSequence_Field *desc;

  /* Initialize array. */
  /* We really populate the contexts' fields here! */
  for (str_iter = str_items, desc = py_sseq_desc->fields; *str_iter; str_iter++, desc++) {
    desc->name = (char *)*str_iter;
    desc->doc = nullptr;
  }
  /* end sentinel */
  desc->name = desc->doc = nullptr;

  PyStructSequence_InitType(py_type, py_sseq_desc);

  /* Initialize the Python type. */
  py_struct_seq = PyStructSequence_New(py_type);
  BLI_assert(py_struct_seq != nullptr);

  for (str_iter = str_items; *str_iter; str_iter++) {
    PyStructSequence_SET_ITEM(py_struct_seq, pos++, PyUnicode_FromString(*str_iter));
  }

  return py_struct_seq;
}

static bool bpyunits_validate(const char *usys_str, const char *ucat_str, int *r_usys, int *r_ucat)
{
  *r_usys = BLI_str_index_in_array(usys_str, bpyunits_usystem_items);
  if (*r_usys < 0) {
    PyErr_Format(PyExc_ValueError, "Unknown unit system specified: %.200s.", usys_str);
    return false;
  }

  *r_ucat = BLI_str_index_in_array(ucat_str, bpyunits_ucategories_items);
  if (*r_ucat < 0) {
    PyErr_Format(PyExc_ValueError, "Unknown unit category specified: %.200s.", ucat_str);
    return false;
  }

  if (!BKE_unit_is_valid(*r_usys, *r_ucat)) {
    PyErr_Format(PyExc_ValueError,
                 "%.200s / %.200s unit system/category combination is not valid.",
                 usys_str,
                 ucat_str);
    return false;
  }

  return true;
}

PyDoc_STRVAR(
    /* Wrap. */
    bpyunits_to_value_doc,
    ".. method:: to_value(unit_system, unit_category, str_input, str_ref_unit=None)\n"
    "\n"
    "   Convert a given input string into a float value.\n"
    "\n"
    "   :arg unit_system: The unit system, from :attr:`bpy.utils.units.systems`.\n"
    "   :type unit_system: str\n"
    "   :arg unit_category: The category of data we are converting (length, area, rotation, "
    "etc.),\n"
    "      from :attr:`bpy.utils.units.categories`.\n"
    "   :type unit_category: str\n"
    "   :arg str_input: The string to convert to a float value.\n"
    "   :type str_input: str\n"
    "   :arg str_ref_unit: A reference string from which to extract a default unit, if none is "
    "found in ``str_input``.\n"
    "   :type str_ref_unit: str | None\n"
    "   :return: The converted/interpreted value.\n"
    "   :rtype: float\n"
    "   :raises ValueError: if conversion fails to generate a valid Python float value.\n");
static PyObject *bpyunits_to_value(PyObject * /*self*/, PyObject *args, PyObject *kw)
{
  char *usys_str = nullptr, *ucat_str = nullptr, *inpt = nullptr, *uref = nullptr;
  const float scale = 1.0f;

  char *str;
  Py_ssize_t str_len;
  double result;
  int usys, ucat;
  PyObject *ret;

  static const char *_keywords[] = {
      "unit_system",
      "unit_category",
      "str_input",
      "str_ref_unit",
      nullptr,
  };
  static _PyArg_Parser _parser = {
      PY_ARG_PARSER_HEAD_COMPAT()
      "s"  /* `unit_system` */
      "s"  /* `unit_category` */
      "s#" /* `str_input` */
      "|$" /* Optional keyword only arguments. */
      "z"  /* `str_ref_unit` */
      ":to_value",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(
          args, kw, &_parser, &usys_str, &ucat_str, &inpt, &str_len, &uref))
  {
    return nullptr;
  }

  if (!bpyunits_validate(usys_str, ucat_str, &usys, &ucat)) {
    return nullptr;
  }

  const size_t str_maxncpy = str_len * 2 + 64;
  str = static_cast<char *>(PyMem_MALLOC(sizeof(*str) * str_maxncpy));
  BLI_strncpy(str, inpt, str_maxncpy);

  BKE_unit_replace_string(str, int(str_maxncpy), uref, scale, usys, ucat);

  if (!PyC_RunString_AsNumber(nullptr, str, "<bpy_units_api>", &result)) {
    if (PyErr_Occurred()) {
      PyErr_Print();
    }

    PyErr_Format(
        PyExc_ValueError, "'%.200s' (converted as '%s') could not be evaluated.", inpt, str);
    ret = nullptr;
  }
  else {
    ret = PyFloat_FromDouble(result);
  }

  PyMem_FREE(str);
  return ret;
}

PyDoc_STRVAR(
    /* Wrap. */
    bpyunits_to_string_doc,
    ".. method:: to_string(unit_system, unit_category, value, precision=3, "
    "split_unit=False, compatible_unit=False)\n"
    "\n"
    "   Convert a given input float value into a string with units.\n"
    "\n"
    "   :arg unit_system: The unit system, from :attr:`bpy.utils.units.systems`.\n"
    "   :type unit_system: str\n"
    "   :arg unit_category: The category of data we are converting (length, area, "
    "rotation, etc.),\n"
    "      from :attr:`bpy.utils.units.categories`.\n"
    "   :type unit_category: str\n"
    "   :arg value: The value to convert to a string.\n"
    "   :type value: float\n"
    "   :arg precision: Number of digits after the comma.\n"
    "   :type precision: int\n"
    "   :arg split_unit: Whether to use several units if needed (1m1cm), or always only "
    "one (1.01m).\n"
    "   :type split_unit: bool\n"
    "   :arg compatible_unit: Whether to use keyboard-friendly units (1m2) or nicer "
    "UTF8 ones (1m²).\n"
    "   :type compatible_unit: bool\n"
    "   :return: The converted string.\n"
    "   :rtype: str\n"
    "   :raises ValueError: if conversion fails to generate a valid Python string.\n");
static PyObject *bpyunits_to_string(PyObject * /*self*/, PyObject *args, PyObject *kw)
{
  char *usys_str = nullptr, *ucat_str = nullptr;
  double value = 0.0;
  int precision = 3;
  bool split_unit = false, compatible_unit = false;

  int usys, ucat;

  static const char *_keywords[] = {
      "unit_system",
      "unit_category",
      "value",
      "precision",
      "split_unit",
      "compatible_unit",
      nullptr,
  };
  static _PyArg_Parser _parser = {
      PY_ARG_PARSER_HEAD_COMPAT()
      "s"  /* `unit_system` */
      "s"  /* `unit_category` */
      "d"  /* `value` */
      "|$" /* Optional keyword only arguments. */
      "i"  /* `precision` */
      "O&" /* `split_unit` */
      "O&" /* `compatible_unit` */
      ":to_string",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kw,
                                        &_parser,
                                        &usys_str,
                                        &ucat_str,
                                        &value,
                                        &precision,
                                        PyC_ParseBool,
                                        &split_unit,
                                        PyC_ParseBool,
                                        &compatible_unit))
  {
    return nullptr;
  }

  if (!bpyunits_validate(usys_str, ucat_str, &usys, &ucat)) {
    return nullptr;
  }

  {
    /* Maximum expected length of string result:
     * - Number itself: precision + decimal dot + up to four 'above dot' digits.
     * - Unit: up to ten chars
     *   (six currently, let's be conservative, also because we use some UTF8 chars).
     * This can be repeated twice (e.g. `1m20cm`), and we add ten more spare chars
     * (spaces, trailing '\0'...).
     * So in practice, 64 should be more than enough.
     */
    char buf1[64], buf2[64];
    const char *str;
    PyObject *result;

    BKE_unit_value_as_string_adaptive(
        buf1, sizeof(buf1), value, precision, usys, ucat, split_unit, false);

    if (compatible_unit) {
      BKE_unit_name_to_alt(buf2, sizeof(buf2), buf1, usys, ucat);
      str = buf2;
    }
    else {
      str = buf1;
    }

    result = PyUnicode_FromString(str);

    return result;
  }
}

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wcast-function-type"
#  else
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-function-type"
#  endif
#endif

static PyMethodDef bpyunits_methods[] = {
    {"to_value",
     (PyCFunction)bpyunits_to_value,
     METH_VARARGS | METH_KEYWORDS,
     bpyunits_to_value_doc},
    {"to_string",
     (PyCFunction)bpyunits_to_string,
     METH_VARARGS | METH_KEYWORDS,
     bpyunits_to_string_doc},
    {nullptr, nullptr, 0, nullptr},
};

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic pop
#  else
#    pragma GCC diagnostic pop
#  endif
#endif

PyDoc_STRVAR(
    /* Wrap. */
    bpyunits_doc,
    "This module contains some data/methods regarding units handling.");

static PyModuleDef bpyunits_module = {
    /*m_base*/ PyModuleDef_HEAD_INIT,
    /*m_name*/ "bpy.utils.units",
    /*m_doc*/ bpyunits_doc,
    /*m_size*/ -1, /* multiple "initialization" just copies the module dict. */
    /*m_methods*/ bpyunits_methods,
    /*m_slots*/ nullptr,
    /*m_traverse*/ nullptr,
    /*m_clear*/ nullptr,
    /*m_free*/ nullptr,
};

PyObject *BPY_utils_units()
{
  PyObject *submodule, *item;

  submodule = PyModule_Create(&bpyunits_module);
  PyDict_SetItemString(PyImport_GetModuleDict(), bpyunits_module.m_name, submodule);

  /* Finalize our unit systems and types structseq definitions! */

  /* bpy.utils.units.system */
  item = py_structseq_from_strings(
      &BPyUnitsSystemsType, &bpyunits_systems_desc, bpyunits_usystem_items);
  PyModule_AddObject(submodule, "systems", item); /* steals ref */

  /* bpy.utils.units.categories */
  item = py_structseq_from_strings(
      &BPyUnitsCategoriesType, &bpyunits_categories_desc, bpyunits_ucategories_items);
  PyModule_AddObject(submodule, "categories", item); /* steals ref */

  return submodule;
}
