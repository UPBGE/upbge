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
Proxy classes and utilities to load array of structures into structures of array
and thus benefit from the performance of foreach_get() and foreach_set() and from the buffer compacity.

This is used for so called soable types, like MeshVertices (array of MeshVertex), SplineBezierPoints

See synchronization.md
"""
from __future__ import annotations

import array
import logging
from typing import List, Dict, Optional, Tuple, TYPE_CHECKING

import bpy
import bpy.types as T  # noqa

from mixer.blender_data.attributes import read_attribute, write_attribute
from mixer.blender_data.json_codec import serialize
from mixer.blender_data.proxy import DeltaUpdate, Proxy
from mixer.blender_data.specifics import soa_initializers
from mixer.blender_data.type_helpers import is_vector

if TYPE_CHECKING:
    from mixer.blender_data.proxy import Context


logger = logging.getLogger(__name__)


def soa_initializer(attr_type, length):
    # According to bpy_rna.c:foreach_getset() and rna_access.c:rna_raw_access() implementations,
    # some cases are implemented as memcpy (buffer interface) or array iteration (sequences),
    # with more subcases that require reallocation when the buffer type is not suitable,
    # TODO try to be smart
    element_init = soa_initializers[attr_type]
    if isinstance(element_init, array.array):
        return array.array(element_init.typecode, element_init.tolist() * length)
    elif isinstance(element_init, list):
        return element_init * length


@serialize
class AosElement(Proxy):
    """
    Proxy for a member of an soable collection that is not supported by foreach_get()/foreach_set(),
    like SplineBezierPoints[x].handle_left_type
    """

    _serialize = ("_data",)

    def __init__(self):
        self._data: Dict[int, List] = {}

    def load(
        self,
        bl_collection: bpy.types.bpy_prop_collection,
        attr_name: str,
        attr_property: T.Property,
        context: Context,
    ):
        """
        - bl_collection: a collection of structure, e.g. a SplineBezierPoints instance
        - attr_name: the name of the member to load, such as "handle_left_type"
        """

        for index, item in enumerate(bl_collection):
            self._data[index] = read_attribute(getattr(item, attr_name), index, attr_property, bl_collection, context)

        try:
            if not isinstance(self._data[0], str):
                logger.error(f"unsupported type for {bl_collection}[{attr_name}]: {type(self._data[0])}")
        except KeyError:
            pass

        return self

    def save(self, unused_attribute, parent: bpy.types.bpy_prop_collection, key: str, context: Context):
        """Saves this proxy into all parent[i].key

        Args:
            unused_attribute:
            parent: collection of structure (e.g. a SplineBezierPoints instance)
            key: the name of the structure member (e.g "handle_left_type")
        """
        for index, item in self._data.items():
            # serialization turns all dict keys to strings
            write_attribute(parent[int(index)], key, item, context)


@serialize(ctor_args=("_member_name",))
class SoaElement(Proxy):
    """
    A structure member inside a bpy_prop_collection loaded as a structure of array element

    For instance, Mesh.vertices[].co is loaded as an SoaElement of Mesh.vertices. Its _data is an array
    """

    _serialize = ("_member_name",)

    def __init__(self, member_name: str):
        self._array = array.array("b", [])
        self._member_name = member_name

    def array_attr(self, aos: T.bpy_prop_collection, bl_rna: T.bpy_struct) -> Tuple[int, type]:
        prototype_item = getattr(aos[0], self._member_name)
        member_type = type(prototype_item)

        if is_vector(member_type):
            array_size = len(aos) * len(prototype_item)
        elif member_type is T.bpy_prop_array:
            member_type = type(prototype_item[0])
            if isinstance(bl_rna, T.MeshPolygon) and self._member_name == "vertices":
                # polygon sizes can differ
                array_size = sum((len(polygon.vertices) for polygon in aos))
            else:
                array_size = len(aos) * len(prototype_item)
        else:
            array_size = len(aos)

        return array_size, member_type

    def load(self, aos: bpy.types.bpy_prop_collection, bl_rna: T.bpy_struct, context: Context):
        """
        Args:
            aos : The array or structures collection that contains this member (e.g.  a_mesh.vertices, a_mesh.edges, ...)
            member_name : The name of this aos member (e.g, "co", "normal", ...)
            prototype_item : an element of parent collection
        """
        if len(aos) == 0:
            self._array = array.array("b", [])
        else:
            array_size, member_type = self.array_attr(aos, bl_rna)
            typecode = soa_initializers[member_type].typecode
            buffer = self._array
            if buffer is None or buffer.buffer_info()[1] != array_size or buffer.typecode != typecode:
                self._array = soa_initializer(member_type, array_size)

            # if foreach_get() raises "RuntimeError: internal error setting the array"
            # it means that the array is ill-formed.
            # Check rna_access.c:rna_raw_access()
            aos.foreach_get(self._member_name, self._array)
        self._attach(context)
        return self

    def _attach(self, context: Context):
        """Attach the buffer to the DatablockProxy or DeltaUpdate"""
        # Store the buffer information at the root of the datablock so that it is easy to find it for serialization
        visit_state = context.visit_state

        # path to the bl_collection that contains the element managed by this proxy,
        # e.g ("vertices") if this proxy manages a Mesh.vertices element
        parent_path = visit_state.path()[:-1]
        root = visit_state.datablock_proxy
        root._soas[parent_path].append((self._member_name, self))

    def save(self, unused_attribute, unused_parent, key: str, context: Context):
        """Saves he name of the structure member managed by the proxy. Saving the values occurs in save_array()

        Args:
            key: the name of the structure member (e.g "co")
        """
        assert self._member_name == key

    def save_array(self, aos: T.bpy_prop_collection, member_name, array_: array.array):
        assert member_name == self._member_name
        if logger.isEnabledFor(logging.DEBUG):
            message = f"save_array {aos}.{member_name}"
            if self._array is not None:
                message += f" proxy ({len(self._array)} {self._array.typecode})"
            message += f" incoming ({len(array_)} {array_.typecode})"
            message += f" blender_length ({len(aos)})"
            logger.debug(message)

        self._array = array_
        try:
            aos.foreach_set(member_name, array_)
        except RuntimeError as e:
            logger.error(f"saving soa {aos!r}[].{member_name} failed")
            logger.error(f"... member size: {len(aos)}, array: ('{array_.typecode}', {len(array_)})")
            logger.error(f"... exception {e!r}")

    def apply(
        self,
        unused_attribute,
        unused_parent: T.bpy_prop_collection,
        unused_key: str,
        delta: DeltaUpdate,
        context: Context,
        to_blender=True,
    ) -> SoaElement:

        """
        Apply delta to this proxy and optionally to the Blender attribute its manages.

        Args:
            delta: the delta to apply
            context: proxy and visit state
            to_blender: update the managed Blender attribute in addition to this Proxy
        """
        update = delta.value
        if update is None:
            return self
        self._array = update._array
        if self._member_name != update._member_name:
            logger.error(f"apply: self._member_name != update._member_name {self._member_name} {update._member_name}")
            return self
        return self

    def diff(self, aos: T.bpy_prop_collection, key: str, prop: T.Property, context: Context) -> Optional[DeltaUpdate]:
        if len(aos) == 0:
            return None

        array_size, member_type = self.array_attr(aos, prop.bl_rna)
        typecode = self._array.typecode
        tmp_array = array.array(typecode, soa_initializer(member_type, array_size))
        if logger.isEnabledFor(logging.DEBUG):
            message = (
                f"diff {aos}.{self._member_name} proxy({len(self._array)} {typecode}) blender'{len(aos)} {member_type}'"
            )
            logger.debug(message)

        try:
            aos.foreach_get(self._member_name, tmp_array)
        except RuntimeError as e:
            logger.error(f"diff soa {aos}.{self._member_name} failed")
            logger.error(f"... member size: {len(aos)}, tmp_array: ('{tmp_array.typecode}', {len(tmp_array)})")
            logger.error(f"... exception {e!r}")

        if self._array == tmp_array:
            return None

        diff = self.__class__(self._member_name)
        diff._array = tmp_array
        diff._attach(context)
        return DeltaUpdate(diff)
