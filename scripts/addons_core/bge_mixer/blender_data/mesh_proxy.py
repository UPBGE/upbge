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
Proxy for Mesh datablock

See synchronization.md
"""
from __future__ import annotations

import array
from collections import defaultdict
import logging
from typing import Dict, Optional, Tuple, TYPE_CHECKING, Union

import bpy.types as T  # noqa

from mixer.blender_data import specifics
from mixer.blender_data.attributes import apply_attribute, diff_attribute
from mixer.blender_data.datablock_proxy import DatablockProxy
from mixer.blender_data.json_codec import serialize
from mixer.blender_data.proxy import Delta, DeltaReplace, DeltaUpdate

if TYPE_CHECKING:
    from mixer.blender_data.bpy_data_proxy import Context
    from mixer.blender_data.types import ArrayGroup


DEBUG = True

logger = logging.getLogger(__name__)


_mesh_geometry_properties = {
    "edges",
    "loop_triangles",
    "loops",
    "polygons",
    "vertices",
}
"""If the size of any of these has changes clear_geomtry() is required. Is is not necessary to check for
other properties (uv_layers), as they are redundant checks"""

mesh_resend_on_clear = {
    "edges",
    "face_maps",
    "loops",
    "loop_triangles",
    "polygons",
    "vertices",
    "uv_layers",
    "vertex_colors",
}
"""if geometry needs to be cleared, these arrays must be resend, as they will need to be reloaded by the receiver"""


def update_requires_clear_geometry(incoming_update: MeshProxy, existing_proxy: MeshProxy) -> bool:
    """Determine if applying incoming_update requires to clear the geometry of existing_proxy"""
    geometry_updates = _mesh_geometry_properties & set(incoming_update._data.keys())
    for k in geometry_updates:
        existing_length = existing_proxy._data[k].length
        incoming_soa = incoming_update.data(k)
        if incoming_soa:
            incoming_length = incoming_soa.length
            if existing_length != incoming_length:
                logger.debug("apply: length mismatch %s.%s ", existing_proxy, k)
                return True
    return False


class VertexGroups:
    """Utility class to hold vertex groups data in a handy way for MeshProxy, ObjectProxy and serialization """

    def __init__(self, indices: Dict[int, array.array] = None, weights: Dict[int, array.array] = None) -> None:
        self.indices = indices if indices is not None else {}
        self.weights = weights if weights is not None else {}

    def __bool__(self):
        return bool(self.indices)

    def __eq__(self, other):
        return self.indices == other.indices and self.weights == other.weights

    def group(self, group_index: int) -> Tuple[array.array, array.array]:
        return self.indices[group_index], self.weights[group_index]

    @staticmethod
    def from_mesh(datablock: T.Mesh) -> VertexGroups:
        indices = defaultdict(list)
        weights = defaultdict(list)
        for i, vertex in enumerate(datablock.vertices):
            for element in vertex.groups:
                group_index = element.group
                indices[group_index].append(i)
                weights[group_index].append(element.weight)

        return VertexGroups(
            {g: array.array("i", list_) for g, list_ in indices.items()},
            {g: array.array("f", list_) for g, list_ in weights.items()},
        )

    @staticmethod
    def from_array_sequence(array_sequence: ArrayGroup) -> VertexGroups:
        """
        Args:
            seq: a list of tuples ((vertex_group number, "i" or "w" ), array)
        """
        vertex_groups = VertexGroups()
        for identifier, array_ in array_sequence:
            group, item_name = identifier
            if item_name == "i":
                vertex_groups.indices[group] = array_
            elif item_name == "w":
                vertex_groups.weights[group] = array_
        return vertex_groups

    def to_array_sequence(self) -> ArrayGroup:
        array_sequence = []
        array_sequence.extend([([group, "i"], array_) for group, array_ in self.indices.items()])
        array_sequence.extend([([group, "w"], array_) for group, array_ in self.weights.items()])
        return array_sequence


@serialize
class MeshProxy(DatablockProxy):
    """
    Proxy for a Mesh datablock. This specialization is required to handle geometry resize processing, that
    spans across Mesh (for clear_geometry()) and geometry arrays of structures (Mesh.vertices.add() and others)
    """

    def requires_clear_geometry(self, mesh: T.Mesh) -> bool:
        """Determines if the difference between mesh and self will require a clear_geometry() on the receiver side"""
        for k in _mesh_geometry_properties:
            soa = getattr(mesh, k)
            existing_length = len(soa)
            incoming_soa = self.data(k)
            if incoming_soa:
                incoming_length = len(incoming_soa)
                if existing_length != incoming_length:
                    logger.debug(
                        "need_clear_geometry: %s.%s (current/incoming) (%s/%s)",
                        mesh,
                        k,
                        existing_length,
                        incoming_length,
                    )
                    return True
        return False

    def load(self, datablock: T.ID, context: Context) -> MeshProxy:
        super().load(datablock, context)
        self._arrays["vertex_groups"] = VertexGroups.from_mesh(datablock).to_array_sequence()
        return self

    def _diff(
        self, struct: T.Mesh, key: str, prop: T.Property, context: Context, diff: MeshProxy
    ) -> Optional[Union[DeltaUpdate, DeltaReplace]]:

        if self.requires_clear_geometry(struct):
            # If any mesh buffer changes requires a clear geometry on the receiver, the receiver will clear all
            # buffers, including uv_layers and vertex_colors.
            # Resend everything
            diff.load(struct, context)

            # force ObjectProxy._diff to resend the Vertex groups
            context.visit_state.dirty_vertex_groups.add(struct.mixer_uuid)
            return DeltaReplace(diff)
        else:
            # vertex groups are always replaced as a whole
            mesh_vertex_groups = VertexGroups.from_mesh(struct).to_array_sequence()
            proxy_vertex_groups: ArrayGroup = self._arrays.get("vertex_groups", [])
            if mesh_vertex_groups != proxy_vertex_groups:
                diff._arrays["vertex_groups"] = mesh_vertex_groups

                # force Object update. This requires that Object updates are processed later, which seems to be
                # the order  they are listed in Depsgraph.updates
                context.visit_state.dirty_vertex_groups.add(struct.mixer_uuid)

            properties = context.synchronized_properties.properties(struct)
            properties = specifics.conditional_properties(struct, properties)
            for k, member_property in properties:
                try:
                    member = getattr(struct, k)
                except AttributeError:
                    logger.warning("diff: unknown attribute ...")
                    logger.warning(f"... {context.visit_state.display_path()}.{k}")
                    continue

                proxy_data = self._data.get(k)
                delta = diff_attribute(member, k, member_property, proxy_data, context)

                if delta is not None:
                    diff._data[k] = delta

            if len(diff._data) or len(diff._arrays):
                return DeltaUpdate(diff)

            return None

    def apply(
        self,
        attribute: T.Mesh,
        parent: T.BlendDataMeshes,
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender: bool = True,
    ) -> MeshProxy:
        """
        Apply delta to this proxy and optionally to the Blender attribute its manages.

        Args:
            attribute: the Mesh datablock to update
            parent: the attribute that contains attribute (e.g. a bpy.data.meshes)
            key: the key that identifies attribute in parent.
            delta: the delta to apply
            context: proxy and visit state
            to_blender: update the managed Blender attribute in addition to this Proxy
        """

        struct_update = delta.value

        if isinstance(delta, DeltaReplace):
            self.copy_data(struct_update)
            if to_blender:
                attribute.clear_geometry()
                # WARNING ensure that parent is not queried for key, which would fail with libraries and duplicate names
                self.save(attribute, parent, key, context)
        else:
            # vertex groups are always replaced as a whole
            vertex_groups_arrays = struct_update._arrays.get("vertex_groups", None)
            if vertex_groups_arrays is not None:
                self._arrays["vertex_groups"] = vertex_groups_arrays

            # collection resizing will be done in AosProxy.apply()

            for k, member_delta in struct_update._data.items():
                current_value = self._data.get(k)
                try:
                    self._data[k] = apply_attribute(attribute, k, current_value, member_delta, context, to_blender)
                except Exception as e:
                    logger.warning(f"Struct.apply(). Processing {member_delta}")
                    logger.warning(f"... for {attribute}.{k}")
                    logger.warning(f"... Exception: {e!r}")
                    logger.warning("... Update ignored")
                    continue

            # If a face is removed from a cube, the vertices array is unchanged but the polygon array is changed.
            # We expect to receive soa updates for arrays that have been modified, but not for unmodified arrays.
            # however unmodified arrays must be reloaded if clear_geometry was called

        return self
