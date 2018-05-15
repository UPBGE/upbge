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

/** \file gameengine/Common/CM_Message.cpp
 *  \ingroup common
 */

#include "CM_Message.h"

#include "BLI_path_util.h"

#include "termcolor.hpp"

#ifdef WITH_PYTHON

#  include "EXP_Python.h"
extern "C" {
#  include "py_capi_utils.h" // for PyC_FileAndNum only
}

#endif  // WITH_PYTHON

std::ostream& _CM_PrefixWarning(std::ostream& stream)
{
	stream << termcolor::yellow << termcolor::bold << "Warning" << termcolor::reset << ": ";
	return stream;
}

std::ostream& _CM_PrefixError(std::ostream& stream)
{
	stream << termcolor::red << termcolor::bold << "Error" << termcolor::reset << ": ";
	return stream;
}

std::ostream& _CM_PrefixDebug(std::ostream& stream)
{
	stream << termcolor::bold << "Debug" << termcolor::reset << ": ";
	return stream;
}

#ifdef WITH_PYTHON

std::ostream& _CM_PythonPrefix(std::ostream& stream)
{
	int line;
	const char *path;
	char file[FILE_MAX];
	PyC_FileAndNum(&path, &line);
	if (!path) {
		return stream;
	}

	BLI_split_file_part(path, file, sizeof(file));

	stream << termcolor::bold << file << termcolor::reset << "(" << termcolor::bold << line << termcolor::reset << "), ";
	return stream;
}

_CM_PythonAttributPrefix::_CM_PythonAttributPrefix(std::string className, std::string attributName)
	:m_className(className),
	m_attributName(attributName)
{
}

std::ostream& operator<<(std::ostream& stream, const _CM_PythonAttributPrefix& prefix)
{
	stream << termcolor::green << prefix.m_className << termcolor::reset << "." << termcolor::green
	       << termcolor::bold << prefix.m_attributName << termcolor::reset << ", ";
	return stream;
}

_CM_PythonFunctionPrefix::_CM_PythonFunctionPrefix(std::string className, std::string attributName)
	:m_className(className),
	m_attributName(attributName)
{
}

std::ostream& operator<<(std::ostream& stream, const _CM_PythonFunctionPrefix& prefix)
{
	stream << termcolor::green << prefix.m_className << termcolor::reset << "." << termcolor::green
	       << termcolor::bold << prefix.m_attributName << termcolor::reset << "(...), ";
	return stream;
}

#endif  // WITH_PYTHON

_CM_FunctionPrefix::_CM_FunctionPrefix(std::string functionName)
	:m_functionName(functionName)
{
}

std::ostream& operator<<(std::ostream& stream, const _CM_FunctionPrefix& prefix)
{
	const std::string& functionName = prefix.m_functionName;
	const size_t colons = functionName.find("::");
	const size_t begin = functionName.substr(0, colons).rfind(" ") + 1;
	const size_t end = functionName.rfind("(") - begin;

	stream << termcolor::bold << functionName.substr(begin, end) << termcolor::reset << "(...), ";
	return stream;
}
