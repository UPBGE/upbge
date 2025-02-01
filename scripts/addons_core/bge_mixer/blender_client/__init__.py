# GPLv3 License
#
# Copyright (C) 2020 Ubisoft
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""
This package defines how we send Blender updates to the server, and how we interpret updates we receive to update
Blender's data.

These functionalities are implemented in the BlenderClient class and in submodules of the package.

Submodules with a well defined entity name (camera, collection, light, ...) handle updates for the corresponding
data type in Blender. The goal is to replace all this specific code with the submodule data.py, which use
the blender_data package to treat updates of Blender's data in a generic way.

Specific code will still be required to handle non-Blender clients. As an example, mesh.py add to the MESH
message a triangulated, with modifiers applied, of the mesh. This is for non-Blender clients. In the future we want to
move these kind of specific processes to a plug-in system.

"""
