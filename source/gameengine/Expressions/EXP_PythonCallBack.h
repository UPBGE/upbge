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
 * Contributor(s): Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file EXP_PythonCallBack.h
 *  \ingroup expressions
 */

#ifndef __EXP_PYTHON_CALLBACK_H__
#define __EXP_PYTHON_CALLBACK_H__

#include "EXP_Python.h"

/** Check and call a python callable object.
 * \param value Callable object candidate.
 * \param arglist The first item in the tuple to execute callbacks (can be nullptr for no arguments).
 * \param minargcount The minimum of arguments possible.
 * \param maxargcount The maximum of arguments possible.
 */
void EXP_RunPythonCallback(PyObject *value, PyObject **arglist, unsigned int minargcount, unsigned int maxargcount);

/** Execute each functions with at least one argument
 * \param functionlist The python list which contains callbacks.
 * \param arglist The first item in the tuple to execute callbacks (can be nullptr for no arguments).
 * \param minargcount The minimum of quantity of arguments possible.
 * \param maxargcount The maximum of quantity of arguments possible.
 */
void EXP_RunPythonCallBackList(PyObject *functionlist, PyObject **arglist, unsigned int minargcount, unsigned int maxargcount);

#endif // __EXP_PYTHON_CALLBACK_H__
