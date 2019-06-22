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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file CM_Message.h
 *  \ingroup common
 */

#ifndef __CM_MESSAGE_H__
#define __CM_MESSAGE_H__

#include <iostream>
#include <string>

std::ostream& _CM_PrefixWarning(std::ostream& stream);
std::ostream& _CM_PrefixError(std::ostream& stream);
std::ostream& _CM_PrefixDebug(std::ostream& stream);

#ifdef WITH_PYTHON

std::ostream& _CM_PythonPrefix(std::ostream& stream);

class _CM_PythonAttributPrefix
{
private:
	std::string m_className;
	std::string m_attributName;

public:
	_CM_PythonAttributPrefix(std::string className, std::string attributName);

	friend std::ostream& operator<<(std::ostream& stream, const _CM_PythonAttributPrefix& prefix);
};

std::ostream& operator<<(std::ostream& stream, const _CM_PythonAttributPrefix& prefix);

class _CM_PythonFunctionPrefix
{
private:
	std::string m_className;
	std::string m_attributName;

public:
	_CM_PythonFunctionPrefix(std::string className, std::string attributName);

	friend std::ostream& operator<<(std::ostream& stream, const _CM_PythonFunctionPrefix& prefix);
};

std::ostream& operator<<(std::ostream& stream, const _CM_PythonFunctionPrefix& prefix);

#endif  // WITH_PYTHON

class _CM_FunctionPrefix
{
private:
	std::string m_functionName;

public:
	_CM_FunctionPrefix(std::string functionName);

	friend std::ostream& operator<<(std::ostream& stream, const _CM_FunctionPrefix& prefix);
};

std::ostream& operator<<(std::ostream& stream, const _CM_FunctionPrefix& prefix);

#define CM_Message(msg) std::cout << msg << std::endl;

/** Format message:
 * Warning: msg
 */
#define CM_Warning(msg) std::cout << _CM_PrefixWarning << msg << std::endl;

/** Format message:
 * Error: msg
 */
#define CM_Error(msg) std::cout << _CM_PrefixError << msg << std::endl;

/** Format message:
 * Debug: msg
 */
#define CM_Debug(msg) std::cout << _CM_PrefixDebug << msg << std::endl;


#ifdef _MSC_VER
#  define CM_FunctionName __FUNCSIG__
#else
#  define CM_FunctionName __PRETTY_FUNCTION__
#endif

/** Format message:
 * Warning: class::function(...) msg
 */
#define CM_FunctionWarning(msg) std::cout << _CM_PrefixWarning << _CM_FunctionPrefix(CM_FunctionName) << msg << std::endl;

/** Format message:
 * Error: class::function(...) msg
 */
#define CM_FunctionError(msg) std::cout << _CM_PrefixError << _CM_FunctionPrefix(CM_FunctionName) << msg << std::endl;

/** Format message:
 * Debug: class::function(...) msg
 */
#define CM_FunctionDebug(msg) std::cout << _CM_PrefixDebug << _CM_FunctionPrefix(CM_FunctionName) << msg << std::endl;


#ifdef WITH_PYTHON

/** Format message:
 * prefix: script(line), msg
 */
#define _CM_PythonMsg(prefix, msg) std::cout << prefix << _CM_PythonPrefix << msg << std::endl;

/** Format message:
 * Warning: script(line), msg
 */
#define CM_PythonWarning(msg) _CM_PythonMsg(_CM_PrefixWarning, msg)

/** Format message:
 * Error: script(line), msg
 */
#define CM_PythonError(msg) _CM_PythonMsg(_CM_PrefixError, msg)


/** Format message:
 * prefix: script(line), class.attribut, msg
 */
#define _CM_PythonAttributMsg(prefix, class, attribut, msg) \
	std::cout << prefix << _CM_PythonPrefix << _CM_PythonAttributPrefix(class, attribut) << msg << std::endl;

/** Format message:
 * Warning: script(line), class.attribut, msg
 */
#define CM_PythonAttributWarning(class, attribut, msg) _CM_PythonAttributMsg(_CM_PrefixWarning, class, attribut, msg)

/** Format message:
 * Error: script(line), class.attribut, msg
 */
#define CM_PythonAttributError(class, attribut, msg) _CM_PythonAttributMsg(_CM_PrefixError, class, attribut, msg)


/** Format message:
 * prefix: script(line), class.function(...), msg
 */
#define _CM_PythonFunctionMsg(prefix, class, function, msg) \
	std::cout << prefix << _CM_PythonPrefix << _CM_PythonFunctionPrefix(class, function) << msg << std::endl;

/** Format message:
 * Warning: script(line), class.function(...), msg
 */
#define CM_PythonFunctionWarning(class, function, msg) _CM_PythonFunctionMsg(_CM_PrefixWarning, class, function, msg)

/** Format message:
 * Error: script(line), class.function(...), msg
 */
#define CM_PythonFunctionError(class, function, msg) _CM_PythonFunctionMsg(_CM_PrefixError, class, function, msg)

#endif  // WITH_PYTHON

#endif  // __CM_MESSAGE_H__
