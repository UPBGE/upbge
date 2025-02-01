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
Classes and methods to compute the difference between a BpyDataProxy and the bpy.data collections.

It computes datablock additions, removals and renames.
This module was written before the proxy system implements differential synchronization (Proxy.diff() and Proxy.apply())
and its functionality should move into BpyDataProxy

See synchronization.md
"""
from __future__ import annotations

import logging
from typing import List, Dict, Tuple, TYPE_CHECKING

import bpy
import bpy.types as T  # noqa

from mixer.blender_data.datablock_proxy import DatablockProxy
from mixer.blender_data.filter import SynchronizedProperties, skip_bpy_data_item
from mixer.blender_data.proxy import ensure_uuid
from mixer.blender_data.library_proxies import DatablockLinkProxy

if TYPE_CHECKING:
    from mixer.blender_data.bpy_data_proxy import BpyDataProxy
    from mixer.blender_data.datablock_collection_proxy import DatablockCollectionProxy

ItemsRemoved = List[DatablockProxy]
ItemsRenamed = List[Tuple[DatablockProxy, str]]
"""(proxy, old_name)"""

ItemsAdded = List[Tuple[T.ID, str]]
"""datablock : collection_name"""

logger = logging.getLogger(__name__)
Uuid = str
BpyDataCollectionName = str


class BpyDataCollectionDiff:
    """
    Diff between Blender state and proxy state for a bpy.data collection.
    """

    def __init__(self):
        self._items_added: ItemsAdded = {}
        self._items_removed: ItemsRemoved = []
        self._items_renamed: ItemsRenamed = []

    @property
    def items_added(self):
        return self._items_added

    @property
    def items_removed(self):
        return self._items_removed

    @property
    def items_renamed(self):
        return self._items_renamed

    def diff(
        self, proxy: DatablockCollectionProxy, collection_name: str, synchronized_properties: SynchronizedProperties
    ):
        self._items_added.clear()
        self._items_removed.clear()
        self._items_renamed.clear()

        # Proxies for received image datablocks that failed to load because of a locally misconfigured shared folders do
        # not have a datablock (they have one when loading the .blend file). Do not consider the proxies without
        # datablock otherwise they would be found as deleted and removals would be sent to peers that may have
        # them.
        proxies = {
            datablock_proxy.mixer_uuid: datablock_proxy
            for datablock_proxy in proxy._data.values()
            if datablock_proxy.has_datablock
        }
        bl_collection = getattr(bpy.data, collection_name)

        # (item name, collection name)
        blender_items: Dict[Uuid, Tuple[T.ID, str]] = {}
        conflicts: List[T.ID] = []
        for datablock in bl_collection.values():
            if skip_bpy_data_item(collection_name, datablock):
                continue

            uuid = datablock.mixer_uuid
            if uuid in blender_items.keys():
                conflicts.append(datablock)
            else:
                ensure_uuid(datablock)
                if datablock.mixer_uuid in blender_items.keys():
                    logger.error(f"Duplicate uuid found for {datablock}")
                    continue

                blender_items[datablock.mixer_uuid] = (datablock, collection_name)

        for second_datablock in conflicts:
            first_datablock = blender_items[second_datablock.mixer_uuid][0]
            if first_datablock.library is None:
                if second_datablock.library is None:
                    # local/local : assume second is the new conflicting, from a copy paste
                    second_datablock.mixer_uuid = ""
                    ensure_uuid(second_datablock)
                    blender_items[second_datablock.mixer_uuid] = (second_datablock, collection_name)
                else:
                    # local/linked: first is made_local from linked second
                    first_datablock.mixer_uuid = ""
                    ensure_uuid(first_datablock)
                    blender_items[first_datablock.mixer_uuid] = (first_datablock, collection_name)
            else:
                if second_datablock.library is not None:
                    # linked/local: breaks the assumption that local are listed before linked. Strange.
                    # could do as local.linked if we were sure that is doe"s not have another weird cause
                    logger.error(
                        f"Unexpected link datablock {first_datablock} listed before local {second_datablock} ..."
                    )
                    logger.error(f"... {second_datablock} ignored")
                else:
                    # linked/linked: Conflicts between two linked. One of:
                    # - a library contains uuids and is indirectly linked more than once
                    # - a self link
                    # Probably need to locally reset both uuids, keeping the link target uuid for direct link datablock
                    logger.error(f"Linked datablock with duplicate uuids {first_datablock} {second_datablock}...")
                    logger.error("... unsupported")

        proxy_uuids = set(proxies.keys())
        blender_uuids = set(blender_items.keys())

        # Ignore linked datablocks to find renamed datablocks, as they cannot be renamed locally
        renamed_uuids = {
            uuid
            for uuid in blender_uuids & proxy_uuids
            if not isinstance(proxies[uuid], DatablockLinkProxy)
            and proxies[uuid].data("name") != blender_items[uuid][0].name
        }
        added_uuids = blender_uuids - proxy_uuids - renamed_uuids
        removed_uuids = proxy_uuids - blender_uuids - renamed_uuids

        # this finds standalone datablock, link datablocks and override datablocks
        self._items_added = [(blender_items[uuid][0], blender_items[uuid][1]) for uuid in added_uuids]
        self._items_removed = [proxies[uuid] for uuid in removed_uuids]

        # TODO LIB
        self._items_renamed = [(proxies[uuid], blender_items[uuid][0].name) for uuid in renamed_uuids]

    def empty(self):
        return not (self._items_added or self._items_removed or self._items_renamed)


class BpyBlendDiff:
    """
    Diff for the whole bpy.data
    """

    def __init__(self):
        self._collection_deltas: List[Tuple[BpyDataCollectionName, BpyDataCollectionDiff]] = []
        """A list of deltas per bpy.data collection. Use a list because if will be sorted later"""

    @property
    def collection_deltas(self):
        return self._collection_deltas

    def diff(self, blend_proxy: BpyDataProxy, synchronized_properties: SynchronizedProperties):
        self._collection_deltas.clear()

        for collection_name, _ in synchronized_properties.properties(bpy_type=T.BlendData):
            if collection_name not in blend_proxy._data:
                continue
            delta = BpyDataCollectionDiff()
            delta.diff(blend_proxy._data[collection_name], collection_name, synchronized_properties)
            if not delta.empty():
                self._collection_deltas.append((collection_name, delta))

        # Before this change:
        # Only datablocks handled by the generic synchronization system get a uuid.
        # Datablocks of unhandled types get no uuid and DatablockRefProxy references to them are incorrect.
        # What is more, this means trouble for tests since datablocks of unhandled types are assigned
        # a uuid during the message grabbing, which means that they get different uuids on both ends.
        for collection_name in synchronized_properties.unhandled_bpy_data_collection_names:
            collection = getattr(bpy.data, collection_name)
            for datablock in collection.values():
                ensure_uuid(datablock)
