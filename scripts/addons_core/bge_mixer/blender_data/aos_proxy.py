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
Proxy for array of structures proxified as structure of arrays

See synchronization.md
"""
from __future__ import annotations

import logging
from typing import Dict, Iterable, Optional, Union, TYPE_CHECKING

import bpy.types as T  # noqa

from mixer.blender_data import specifics
from mixer.blender_data.json_codec import serialize
from mixer.blender_data.aos_soa_proxy import SoaElement, AosElement
from mixer.blender_data.specifics import is_soable_property
from mixer.blender_data.attributes import diff_attribute, write_attribute
from mixer.blender_data.proxy import DeltaUpdate, Proxy

if TYPE_CHECKING:
    from mixer.blender_data.proxy import Context, Delta

logger = logging.getLogger(__name__)

_unknown_type_attributes = {"__doc__", "__module__", "__slots__", "bl_rna", "rna_type"}
"""Attributes of bpy.types.UnknownType to not synchronize"""


@serialize
class AosProxy(Proxy):
    """
    Proxy to a bpy_prop_collection of structure with at least a member that can be handled
    by foreach_get()/foreach_set(), such as MeshVertices
    """

    _serialize = ("_aos_length", "_data")

    def __init__(self):
        self._data: Dict[str, Union[AosElement, SoaElement, Delta]] = {}
        self._aos_length = 0

    def __len__(self):
        return self._aos_length

    @property
    def length(self) -> int:
        return self._aos_length

    def load(self, bl_collection: T.bpy_prop_collection, bl_collection_property: T.Property, context: Context):

        # Must process the Soa elements, even if empty, because we may we called when a diff detects that
        # a replace is required (e.g. geometry vertext count change) and we must ensure that the soas are updated.
        # This will unfortunately register and transfer empty arrays.
        # TODO optimize and do not send empty arrays
        self._aos_length = len(bl_collection)

        item_bl_rna = bl_collection_property.fixed_type.bl_rna
        if bl_collection_property.fixed_type.bl_rna is T.UnknownType.bl_rna:
            # UnknownType used in ShakeKey. Contents depends on the items that has the Key (Curve, Mesh, Lattice)
            if len(self) != 0:
                item = bl_collection[0]
                names = set(dir(item)) - _unknown_type_attributes
                for attr_name in names:
                    # Since this dies no use read_attribute, pugh the current item by hand
                    context.visit_state.push(bl_collection, attr_name)
                    try:
                        self._data[attr_name] = SoaElement(attr_name).load(bl_collection, item_bl_rna, context)
                    finally:
                        context.visit_state.pop()
        else:
            for attr_name, bl_rna_property in context.synchronized_properties.properties(item_bl_rna):
                # Since this dies no use read_attribute, pugh the current item by hand
                context.visit_state.push(bl_collection, attr_name)
                try:
                    if is_soable_property(bl_rna_property):
                        # element supported by foreach_get()/foreach_set(), e.g. MeshVertices.co
                        # The collection is loaded as an array.array and encoded as a binary buffer
                        self._data[attr_name] = SoaElement(attr_name).load(bl_collection, item_bl_rna, context)
                    else:
                        # element not supported by foreach_get()/foreach_set(), e.g. BezierSplinePoint.handle_left_type,
                        # which is an enum, loaded as string
                        # The collection is loaded as a dict, encoded as such
                        self._data[attr_name] = AosElement().load(bl_collection, attr_name, item_bl_rna, context)
                finally:
                    context.visit_state.pop()

        return self

    def save(self, attribute: T.bpy_prop_collection, parent: T.bpy_struct, key: Union[int, str], context: Context):
        """
        Save this proxy into attribute.

        Args:
            attribute: a collection of bpy_struct (e.g. a_Mesh_instance.vertices)
            parent: the attribute that contains attribute (e.g. a Mesh instance)
            key: the name of the bpy_collection in parent (e.g "vertices")
            context: proxy and visit state
        """

        specifics.fit_aos(attribute, self, context)

        # nothing to do save here. The buffers that contains vertices and co are serialized apart from the json
        # that contains the Mesh members. The children of this are SoaElement and have no child.
        # They are updated directly bu SoaElement.save_array()

        for k, v in self._data.items():
            write_attribute(attribute, k, v, context)

    def apply(
        self,
        attribute: T.bpy_prop_collection,
        parent: T.bpy_struct,
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender=True,
    ) -> AosProxy:
        """
        Apply delta to this proxy and optionally to the Blender attribute its manages.

        Args:
            attribute: a collection of bpy_struct (e.g. a_Mesh_instance.vertices)
            parent: the attribute that contains attribute (e.g. a Mesh instance)
            key: the name of the bpy_collection in parent (e.g "vertices")
            delta: the delta to apply
            context: proxy and visit state
            to_blender: update the managed Blender attribute in addition to this Proxy
        """

        struct_update = delta.value

        self._aos_length = struct_update._aos_length
        specifics.fit_aos(attribute, self, context)
        for k, member_delta in struct_update._data.items():
            current_value = self.data(k)
            if current_value is not None:
                self._data[k] = current_value.apply(None, attribute, k, member_delta, to_blender)

        return self

    def diff(
        self, aos: T.bpy_prop_collection, key: Union[int, str], prop: T.Property, context: Context
    ) -> Optional[DeltaUpdate]:
        """"""

        # Create a proxy that will be populated with attributes differences, resulting in a hollow dict,
        # as opposed as the dense self
        diff = self.__class__()
        diff.init(aos)
        diff._aos_length = len(aos)

        item_bl_rna = prop.fixed_type.bl_rna
        member_names: Iterable[str] = []
        if item_bl_rna is T.UnknownType.bl_rna:
            # UnknownType used in ShapeKey. Contents depends on the items that has the Key (Curve, Mesh, Lattice)
            if len(self) != 0:
                member_names = set(dir(aos[0])) - _unknown_type_attributes
        else:
            member_names = [item[0] for item in context.synchronized_properties.properties(item_bl_rna)]

        for member_name in member_names:
            # co, normals, ...
            proxy_data = self._data.get(member_name, SoaElement(member_name))
            delta = diff_attribute(aos, member_name, item_bl_rna, proxy_data, context)
            if delta is not None:
                diff._data[member_name] = delta

        # if anything has changed, wrap the hollow proxy in a DeltaUpdate. This may be superfluous but
        # it is homogenous with additions and deletions
        if len(diff._data):
            return DeltaUpdate(diff)

        return None
