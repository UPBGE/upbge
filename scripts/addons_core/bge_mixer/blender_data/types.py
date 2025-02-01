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
Simple types used by the addon and the tests.

Must not include bpy
"""
from __future__ import annotations

import array
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Tuple, Union


SoaMember = Tuple[str, array.array]
"""Member of a structure of array from a Blender array of structure like MeshVertices
- Name of the structure member, e.g, "co" or "normal"
- Data to be loaded with foreach_set()
"""

Path = Iterable[Union[str, int]]
"""A data path starting from the datablock e.g. ["curves", 0, "bezier_points"]"""


@dataclass
class Soa:
    """A structure of array, loaded from a Blender array of structure like MeshVertices"""

    path: Path
    """a data path to the array"""

    members: List[SoaMember]


ArrayGroup = List[Tuple[Any, array.array]]
"""A logical group of related arrays, like vertex groups.

The first item is an identifier for the DatablockProxy that uses the ArrayGroup. see MeshProxy.py:VertexGroups.
Json serialization converts tuples into lists"""

ArrayGroups = Dict[str, ArrayGroup]
"""ArrayGroups contain arrays that must be serialized in binary format, mainly because of their size, but could
otherwise be stored in Proxy._data

TODO use ArrayGroups for Proxy._media and Proxy._soas
"""
