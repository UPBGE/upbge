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
Proxy of a reference to datablock

See synchronization.md
"""
from __future__ import annotations

import logging
from typing import Optional, TYPE_CHECKING, Union

import bpy.types as T  # noqa

from mixer.blender_data.attributes import read_attribute
from mixer.blender_data.bpy_data import rna_identifier_to_collection_name
from mixer.blender_data.json_codec import serialize
from mixer.blender_data.proxy import Delta, DeltaReplace, Proxy
from mixer.blender_data.type_helpers import bases_of

if TYPE_CHECKING:
    from mixer.blender_data.proxy import Uuid, Context
    from mixer.blender_data.misc_proxies import NonePtrProxy


logger = logging.getLogger(__name__)


@serialize
class DatablockRefProxy(Proxy):
    """
    A reference to a standalone datablock

    Examples of such references are :
    - Camera.dof.focus_object
    """

    _serialize = ("_bpy_data_collection", "_datablock_uuid", "_initial_name")

    def __init__(self):
        self._datablock_uuid: str = None
        self._bpy_data_collection: str = ""
        self._initial_name: str = None

        self._debug_name = None

    def __str__(self) -> str:
        return f"{self.__class__.__name__}({self._datablock_uuid}, bpy.data.{self._bpy_data_collection}, name at creation: {self._initial_name})"

    def __bool__(self):
        return self._datablock_uuid is not None

    @property
    def display_string(self) -> str:
        return f"bpy.data.{self._bpy_data_collection}[{self._initial_name}]"

    @property
    def mixer_uuid(self) -> Uuid:
        return self._datablock_uuid

    def load(self, datablock: T.ID, context: Context) -> DatablockRefProxy:
        """
        Load a reference to a standalone datablock into this proxy
        """
        assert not datablock.is_embedded_data

        # see HACK in target()
        # Base type closest to ID (e.g. Light for Point)
        type_ = bases_of(type(datablock).bl_rna)[-2]
        type_name = type_.bl_rna.identifier
        self._bpy_data_collection = rna_identifier_to_collection_name()[type_name]
        self._initial_name = datablock.name

        self._datablock_uuid = datablock.mixer_uuid

        self._debug_name = str(datablock)
        return self

    def save(
        self,
        unused_attribute,
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: str,
        context: Context,
    ):
        """
        Save the datablock reference tracked by this proxy into parent.key or parent[key]

        Args:
            unused_attribute:
            parent: the structure or collection that contains the target reference (e.g. a Scene instance)
            key: the name of the bpy_collection (e.g "camera")
            context: the proxy and visit state
        """
        ref_target = self.target(context)

        try:
            if isinstance(parent, T.bpy_prop_collection):
                # reference stored in a collection
                # is there a case for this is is always link() in DatablockCollectionProxy ?
                if ref_target is None:
                    context.proxy_state.unresolved_refs.append(
                        self.mixer_uuid,
                        lambda datablock: parent.__setitem__(key, datablock),
                        f"{context.visit_state.display_path()}[{key}] = {self.display_string}",
                    )
                else:
                    parent[key] = ref_target
            else:
                # reference stored in a struct (e.g. Object.parent)
                # This is what saves Camera.dof.focus_object
                if ref_target is None:
                    context.proxy_state.unresolved_refs.append(
                        self.mixer_uuid,
                        lambda datablock: setattr(parent, key, datablock),
                        f"{context.visit_state.display_path()}.{key} = {self.display_string}",
                    )
                else:
                    setattr(parent, key, ref_target)
        except AttributeError as e:
            # Most often not an error
            # - read_only property
            # - read-only attribute in corner case :
            #   - write Object.material_slots[i].material when Object.material_slots[i].link=="DATA" and the mesh is
            #     from a library
            logger.info("Save: exception during ...")
            logger.info(f"... {context.visit_state.display_path()}.{key} = {ref_target!r}...")
            logger.info(f"... {e!r}")
        except Exception as e:
            logger.warning("Save: exception during ...")
            logger.warning(f"... {context.visit_state.display_path()}.{key} = {ref_target!r}...")
            logger.warning(f"... {e!r}")

    def target(self, context: Context) -> Optional[T.ID]:
        """
        The datablock referenced by this proxy
        """
        return context.proxy_state.datablock(self._datablock_uuid)

    def apply(
        self,
        attribute,
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender: bool = True,
    ) -> Union[NonePtrProxy, DatablockRefProxy]:
        """
        Apply a delta to this proxy, which occurs when Scene.camera changes, for instance
        """
        update = delta.value
        if to_blender:
            from mixer.blender_data.misc_proxies import NonePtrProxy

            if isinstance(update, NonePtrProxy):
                value = None
            else:
                assert type(update) == type(self), "type(update) == type(self)"

                value = context.proxy_state.datablock(update._datablock_uuid)

            if isinstance(key, int):
                parent[key] = value
            else:
                setattr(parent, key, value)

        return update

    def diff(
        self, datablock: T.ID, key: Union[int, str], datablock_property: T.Property, context: Context
    ) -> Optional[DeltaReplace]:
        """
        Computes the difference between this proxy and its Blender state.
        """

        if datablock is None:
            return DeltaReplace(DatablockRefProxy())

        value = read_attribute(datablock, key, datablock_property, None, context)
        assert isinstance(value, DatablockRefProxy)
        if value._datablock_uuid != self._datablock_uuid:
            return DeltaReplace(value)
        else:
            return None
