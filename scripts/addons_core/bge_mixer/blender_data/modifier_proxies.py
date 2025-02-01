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
Proxy classes for bpy_struct that ned a custom implementation

See synchronization.md
"""
from __future__ import annotations

import logging
from typing import Any, Dict, Optional, TYPE_CHECKING, Tuple, Union

import bpy
import bpy.types as T  # noqa

from mixer.blender_data.json_codec import serialize
from mixer.blender_data.proxy import Delta, DeltaUpdate
from mixer.blender_data.struct_proxy import StructProxy

if TYPE_CHECKING:
    from mixer.blender_data.misc_proxies import NonePtrProxy
    from mixer.blender_data.proxy import Context

logger = logging.getLogger(__name__)


@serialize
class NodesModifierProxy(StructProxy):
    """Proxy for NodesModifier.

    Requires special processing for the modifier inputs. They are "custom" properties with names that
    match the node_group inputs identifiers like in e.g bpy.nodes.objects["Plane"].modifiers[0]["Input_5"].
    """

    _serialize: Tuple[str, ...] = StructProxy._serialize + ("_inputs",)

    _non_inputs = set(("_RNA_UI",))
    _non_inputs.update(T.NodesModifier.bl_rna.properties.keys())
    """Identifiers of properties that are not inputs."""

    def __init__(self):
        self._inputs: Dict[int, Any] = {}
        """{ index in geometry node group group input node : value }."""

        super().__init__()

    def _load_inputs(self, modifier: T.bpy_struct) -> Dict[int, Any]:
        """Returns a dict containing {input_index_in_node_group_inputs: value_in_nodes_modifier} """

        input_names = set(modifier.keys()) - self._non_inputs
        node_group = modifier.node_group
        if node_group is None:
            return {}

        input_indices = {tree_input.identifier: i for i, tree_input in enumerate(node_group.inputs)}

        return {input_indices[name]: modifier.get(name) for name in input_names if name in input_indices}

    def _save_inputs(self, modifier: T.bpy_struct):
        """Saves the input values into modifier."""

        node_group = modifier.node_group
        if node_group is None:
            return {}

        inputs = {i: tree_input.identifier for i, tree_input in enumerate(node_group.inputs)}
        for input_index, value in self._inputs.items():
            # default serialization transforms int keys into string
            input_name = inputs[int(input_index)]
            modifier[input_name] = value

    def load(self, modifier: T.bpy_struct, context: Context) -> StructProxy:
        # The inputs are stored as "custom properties".
        # The keys are the geometry node_group group input node outputs.
        self._inputs = self._load_inputs(modifier)
        return super().load(modifier, context)

    def save(
        self,
        modifier: T.bpy_struct,
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: Union[int, str],
        context: Context,
    ):
        # the modifier is always created with a default geometry node : remove it
        node_group = modifier.node_group
        if node_group is not None:
            if node_group.users != 1:
                logger.error(f"save(): default node group {node_group} has {node_group.users} users")
                return
            if node_group.mixer_uuid != "":
                logger.error(f"save(): default node group {node_group} has uuid {node_group.mixer_uuid}")
                return
            bpy.data.node_groups.remove(node_group)

        # update the geometry node reference before updating the input entries
        super().save(modifier, parent, key, context)
        self._save_inputs(modifier)

    def diff(self, modifier: T.bpy_struct, key: Union[int, str], prop: T.Property, context: Context) -> Optional[Delta]:

        delta = super().diff(modifier, key, prop, context)
        inputs = self._load_inputs(modifier)
        if inputs != self._inputs:
            if delta is None:
                diff = self.__class__()
                delta = DeltaUpdate(diff)
            delta.value._inputs = inputs
        return delta

    def apply(
        self,
        modifier: T.bpy_struct,
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender: bool = True,
    ) -> Union[StructProxy, NonePtrProxy]:

        super().apply(modifier, parent, key, delta, context, to_blender)
        if not isinstance(delta, DeltaUpdate):
            logger.error(f"apply(): Internal error, unexpected delta type {type(delta)}")
            return self

        delta_inputs = getattr(delta.value, "_inputs", None)
        if delta_inputs is not None or "node_group" in delta.value._data:
            # also write inputs when the node_group changes
            self._inputs = delta_inputs
            if to_blender:
                ng = modifier.node_group
                modifier.node_group = None
                modifier.node_group = ng
                self._save_inputs(modifier)
        return self
