/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_BinaryCrypt.h
 *  \ingroup ketsji
 *
 * Python side of UPBGE-BinaryCrypt: the Python modules and data files the tool encrypted.
 */

#pragma once

#ifdef WITH_PYTHON

#  include <Python.h>

/**
 * bge.logic.decryptFile: the bytes of the file at \a filepath (already expanded), decrypted when
 * it is an encrypted data file, as they are otherwise, so one script runs before and after the
 * export. Encrypted .blend files and Python modules are refused.
 * \return a new bytes object, or nullptr with a Python exception set.
 */
PyObject *KX_BinaryCrypt_ReadFile(const char *filepath);

/**
 * Put the finder of encrypted Python modules in \a sys_meta_path, in front of Python's path
 * finder, once. Only in a player that received a key: elsewhere no encrypted module can be opened
 * anyway, and imports stay untouched.
 */
void KX_BinaryCrypt_InstallImporter(PyObject *sys_meta_path);

#endif /* WITH_PYTHON */
