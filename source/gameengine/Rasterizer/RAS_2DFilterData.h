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
* Contributor(s): Pierluigi Grassi, Porteries Tristan.
*
* ***** END GPL LICENSE BLOCK *****
*/

#ifndef __RAS_2DFILTERDATA__
#define __RAS_2DFILTERDATA__

#include <vector>
#include <string>

class EXP_Dictionary;

/** This type is used to pack data received from a 2D Filter actuator and send it to
the RAS_2DFilterManager::AddFilter method, because the number of parameters needed by
a custom filter may become quite large and a function with a thousands paramters doesn't look
very good. So it's purely for readability.
*/
class RAS_2DFilterData
{
public:
	RAS_2DFilterData();
	virtual ~RAS_2DFilterData();

	/// The names of the properties of the game object that the shader may want to use as uniforms.
	std::vector<std::string> propertyNames;
	/// The KX_GameObject (or something else?) that provides the values for the uniforms named above.
	EXP_Dictionary *gameObject;
	/// Enable/Disable mipmap in for rendered texture.
	bool mipmap;
	/// Should be a SCA_2DFilterActuator.FILTER_MODE value.
	int filterMode;
	/// In the original design this was bot the pass index and the unique identifier of the filter in the filter manager.
	unsigned int filterPassIndex;
	/// This is the shader program source code IF the filter is not a predefined one.
	std::string shaderText;
};

#endif // __RAS_2DFILTERDATA__
