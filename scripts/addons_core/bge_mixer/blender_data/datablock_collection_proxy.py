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
Proxies for collections of datablock (e.g. bpy.data.objects) or datablock references (e.g. Scene.objects)

See synchronization.md
"""
from __future__ import annotations

import logging
import traceback
from typing import Any, Dict, List, Optional, Tuple, TYPE_CHECKING, Union

import bpy
import bpy.types as T  # noqa

from mixer.blender_data import specifics
from mixer.blender_data.attributes import diff_attribute, read_attribute, write_attribute
from mixer.blender_data.changeset import Changeset, RenameChangeset
from mixer.blender_data.datablock_proxy import DatablockProxy
from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy
from mixer.blender_data.diff import BpyDataCollectionDiff
from mixer.blender_data.json_codec import serialize
from mixer.blender_data.proxy import Delta, DeltaUpdate, DeltaAddition, DeltaDeletion, MaxDepthExceeded
from mixer.blender_data.proxy import ensure_uuid, Proxy

if TYPE_CHECKING:
    from mixer.blender_data.proxy import Context, Uuid


logger = logging.getLogger(__name__)


@serialize
class DatablockCollectionProxy(Proxy):
    """
    Proxy to a bpy_prop_collection of standalone datablocks, i.e. of bpy.data collections

    This proxy keeps track of the state of the whole collection. The proxy contents are be instances
    of DatablockProxy.
    """

    _serialize = ("_data",)

    def __init__(self, name: str):
        self._name: str = name
        """Name of the collection in bpy.data"""

        self._data: Dict[str, DatablockProxy] = {}
        """One item per datablock. The key is the uuid, which eases rename management"""

        self._snapshot_undo_pre: Dict[str, Uuid] = {}
        """{name_full: uuid_before_undo} for all datablocks of the collection managed by this proxy at time of undo_pre"""

        self._snapshot_undo_post: Dict[str, Uuid] = {}
        """{name_full: uuid_before_undo} for datablocks with no uuid after undo"""

    def __len__(self):
        return len(self._data)

    def reload_datablocks(self, datablocks: Dict[str, T.ID]):
        """Reload datablock references after undo, fixing undone uuids"""
        collection = getattr(bpy.data, self._name)
        datablocks.update({datablock.mixer_uuid: datablock for datablock in collection if datablock.mixer_uuid != ""})
        if self._snapshot_undo_post:
            # Restore uuids for datablocks that had a uuid before undo but have none after undo. Expected to be rare.
            updates = {
                self._snapshot_undo_post[datablock.name_full]: datablock
                for datablock in collection
                if datablock.name_full in self._snapshot_undo_post
            }
            for uuid, datablock in updates.items():
                datablock.mixer_uuid = uuid
            datablocks.update(updates)

    def snapshot_undo_pre(self):
        """Record pre undo state to recover undone uuids."""
        collection = getattr(bpy.data, self._name)
        self._snapshot_undo_pre = {datablock.name_full: datablock.mixer_uuid for datablock in collection}

    def snapshot_undo_post(self) -> Optional[Tuple[str, Dict[str, Uuid]]]:
        """Compare post undo uuid state to recover undone uuids."""
        collection = getattr(bpy.data, self._name)
        undo_post = {datablock.name_full for datablock in collection if datablock.mixer_uuid == ""}
        self._snapshot_undo_post = {
            name: self._snapshot_undo_pre[name] for name in undo_post if name in self._snapshot_undo_pre
        }

        # temporary for logging
        if self._snapshot_undo_post:
            return (self._name, self._snapshot_undo_post)

        return None

    def save(self, attribute: bpy.type.Collection, parent: Any, key: str, context: Context):
        """
        OBSOLETE Save this Proxy a Blender collection that may be a collection of standalone datablocks in bpy.data
        or a collection of referenced datablocks like bpy.type.Collection.children
        """
        if not self._data:
            return

        # collection of standalone datablocks
        for k, v in self._data.items():
            write_attribute(attribute, k, v, context)

    def find(self, key: str):
        return self._data.get(key)

    def create_datablock(
        self, incoming_proxy: DatablockProxy, context: Context
    ) -> Tuple[Optional[T.ID], Optional[RenameChangeset]]:
        """Create a bpy.data datablock from a received DatablockProxy and update the proxy structures accordingly

        Args:
            incoming_proxy : this proxy contents is used to update the bpy.data collection item
        """

        datablock, renames = incoming_proxy.create_standalone_datablock(context)
        # returned datablock is None for ShapeKey and Library. Datablock creation is deferred until :
        # - ShapeKey : Object update
        # - Library : creation of a link datablock

        if incoming_proxy.collection_name == "scenes":
            logger.info(f"Creating scene '{incoming_proxy.data('name')}' uuid: '{incoming_proxy.mixer_uuid}'")

            # One existing scene from the document loaded at join time could not be removed during clear_scene_conten().
            # Remove it now
            scenes = bpy.data.scenes
            if len(scenes) == 2 and ("_mixer_to_be_removed_" in scenes):
                from mixer.blender_client.scene import delete_scene

                scene_to_remove = scenes["_mixer_to_be_removed_"]
                logger.info(
                    f"After create scene '{incoming_proxy.data('name_full')}' uuid: '{incoming_proxy.mixer_uuid}''"
                )
                logger.info(f"... delete {scene_to_remove} uuid '{scene_to_remove.mixer_uuid}'")
                delete_scene(scene_to_remove)

        uuid = incoming_proxy.mixer_uuid
        self._data[uuid] = incoming_proxy
        context.proxy_state.proxies[uuid] = incoming_proxy

        if datablock is not None:
            context.proxy_state.unresolved_refs.resolve(uuid, datablock)

        return datablock, renames

    def update_datablock(self, delta: DeltaUpdate, context: Context):
        """Update a bpy.data item from a received DatablockProxy and update the proxy state"""
        incoming_proxy = delta.value
        uuid = incoming_proxy.mixer_uuid

        proxy = context.proxy_state.proxies.get(uuid)
        if proxy is None:
            logger.error(
                f"update_datablock(): Missing proxy for bpy.data.{incoming_proxy.collection_name}[{incoming_proxy.data('name')}] uuid {uuid}"
            )
            return

        if proxy.mixer_uuid != incoming_proxy.mixer_uuid:
            logger.error(
                f"update_datablock : uuid mismatch between incoming {incoming_proxy.mixer_uuid} ({incoming_proxy}) and existing {proxy.mixer_uuid} ({proxy})"
            )
            return

        # the ID will have changed if the object has been morphed (change light type, for instance)
        existing_id = context.proxy_state.datablock(uuid)
        if existing_id is None:
            logger.warning(f"Non existent uuid {uuid} while updating {proxy.collection_name}[{proxy.data('name')}]")
            return None

        id_ = proxy.update_standalone_datablock(existing_id, delta, context)
        if existing_id != id_:
            # Not a problem for light morphing
            logger.warning(f"Update_datablock changes datablock {existing_id} to {id_}")
            context.proxy_state.add_datablock(uuid, id_)

        return id_

    def remove_datablock(self, proxy: DatablockProxy, datablock: Optional[T.ID]):
        """Remove a bpy.data collection item and update the proxy state"""
        logger.info("Perform removal for %s", proxy)
        try:
            if datablock is None:
                from mixer.blender_data.library_proxies import DatablockLinkProxy

                if not isinstance(proxy, DatablockLinkProxy):
                    logger.error(f"remove_datablock. Unexpected None datablock for uuid {proxy}")
                else:
                    # the datablock loading probably failed because the library was missing locally because of a shared
                    # folders misconfiguration bu the user
                    logger.info(f"remove_datablock(None) for unloaded {proxy}")
            else:
                try:
                    specifics.remove_datablock(proxy.collection, datablock)
                except ReferenceError as e:
                    # We probably have processed previously the deletion of a datablock referenced by Object.data (e.g.
                    # Light). On both sides it deletes the Object as well. This is a failure in properly orderring
                    # delete messages on the sender
                    logger.warning(f"Exception during remove_datablock for {proxy}")
                    logger.warning(f"... {e!r}")
        finally:
            uuid = proxy.mixer_uuid
            del self._data[uuid]

    def rename_datablock(self, proxy: DatablockProxy, new_name: str, datablock: T.ID):
        """
        Rename a bpy.data collection item and update the proxy state (receiver side)
        """
        logger.info("rename_datablock proxy %s datablock %r into %s", proxy, datablock, new_name)
        datablock.name = new_name
        proxy._data["name"] = new_name

    def update(self, diff: BpyDataCollectionDiff, context: Context) -> Changeset:
        """
        Update the proxy according to local datablock creations, removals or renames (sender side)
        """
        changeset = Changeset()

        # Sort so that the tests receive the messages in deterministic order for two reasons :
        # - The tests compare the creation message streams received from participating Blender and there
        #   is not reason why they would emit creation messages
        # - TestObjectRenameGeneric.test_update_object exhibits a random failure without the sort
        #   Scene creation messages order is then random and an update is missed when the scene that links
        #   the update object is not current. Setting PYTHONHASHSEED is not enough to get a deterministic test outcome.
        added_names = sorted(diff.items_added, key=lambda x: x[0].name_full)

        for datablock, collection_name in added_names:
            name_full = datablock.name_full
            logger.info("Perform update/creation for %s[%s]", collection_name, name_full)
            try:
                uuid = ensure_uuid(datablock)
                context.proxy_state.add_datablock(uuid, datablock)
                proxy = DatablockProxy.make(datablock).load(datablock, context)
                context.proxy_state.proxies[uuid] = proxy
                self._data[uuid] = proxy
                changeset.creations.append(proxy)
            except MaxDepthExceeded as e:
                logger.error(f"MaxDepthExceeded while loading {collection_name}[{name_full}]:")
                logger.error("... Nested attribute depth is too large: ")
                logger.error(f"... {e!r}")
            except Exception:
                logger.error(f"Exception while loading {collection_name}[{name_full}]:")
                for line in traceback.format_exc().splitlines():
                    logger.error(line)

        for proxy in diff.items_removed:
            try:
                logger.warning("Perform removal for %s", proxy)
                uuid = proxy.mixer_uuid
                changeset.removals.append((uuid, proxy.collection_name, str(proxy)))
                del self._data[uuid]
                del context.proxy_state.proxies[uuid]
                try:
                    context.proxy_state.remove_datablock(uuid)
                except KeyError:
                    logger.warning(f"remove_datablock: n,o entry for {uuid}. Assuming removed by undo")
            except Exception:
                logger.error(f"Exception during update/removed for proxy {proxy})  :")
                for line in traceback.format_exc().splitlines():
                    logger.error(line)

        #
        # Handle spontaneous renames
        #
        # - local and remote are initially synced with 2 objects with uuid/name D7/A FC/B
        # - local renames D7/A into B
        #   - D7 is actually renamed into B.001 !
        #   - we detect (D7 -> B.001)
        #   - remote proceses normally
        # - local renames D7/B.001 into B
        #   - D7 is renamed into B
        #   - FC is renamed into B.001
        #   - we detect (D7->B, FC->B.001)
        #   - local result is (D7/B, FC/B.001)
        # - local repeatedly renames the item named B.001 into B
        # - at some point on remote, the execution of a rename command will provoke a spontaneous rename,
        #   resulting in a situation where remote has FC/B.001 and D7/B.002 linked to the
        #   Master collection and also a FC/B unlinked
        #
        for proxy, new_name in diff.items_renamed:
            old_name = proxy.data("name")
            changeset.renames.append((proxy.mixer_uuid, old_name, new_name, str(proxy)))
            proxy._data["name"] = new_name

        return changeset

    def search(self, name: str) -> List[DatablockProxy]:
        """Convenience method to find proxies by name instead of uuid (for tests only)"""
        results = []
        for uuid in self._data.keys():
            proxy_or_update = self.data(uuid)
            proxy = proxy_or_update if isinstance(proxy_or_update, Proxy) else proxy_or_update.value
            if proxy.data("name") == name:
                results.append(proxy)
        return results

    def search_one(self, name: str) -> Optional[DatablockProxy]:
        """Convenience method to find a proxy by name instead of uuid (for tests only)"""
        results = self.search(name)
        return None if not results else results[0]


@serialize
class DatablockRefCollectionProxy(Proxy):
    """
    Proxy to a bpy_prop_collection of datablock references (CollectionObjects and CollectionChildren only,
    with link/unlink API
    """

    _serialize = ("_data",)

    def __init__(self):
        # One item per datablock. The key is the uuid, which eases rename management
        self._data: Dict[str, Union[DeltaAddition, DeltaDeletion, DeltaUpdate, DatablockRefProxy]] = {}

    def __len__(self):
        return len(self._data)

    def load(self, bl_collection: bpy.types.bpy_prop_collection, context: Context):  # noqa N802
        """
        Load bl_collection elements as references to bpy.data collections
        """
        for item in bl_collection:
            proxy = DatablockRefProxy()
            uuid = item.mixer_uuid
            proxy.load(item, context)
            self._data[uuid] = proxy
        return self

    def save(self, collection: T.bpy_prop_collection, parent: T.bpy_struct, key: str, context: Context):
        """
        Saves this proxy into collection

        Args:
            collection: a collection of datablock references with link/unlink interface
                (e.g a_Collection_instance.objects)
            parent: the structure that contains collection to be loaded (e.g. a Collection instance)
            key: the name of the bpy_collection (e.g "objects")
            context:
        """
        for _, ref_proxy in self._data.items():
            assert isinstance(ref_proxy, DatablockRefProxy)
            datablock = ref_proxy.target(context)
            if datablock:
                collection.link(datablock)
            else:
                logger.info(
                    f"unresolved reference {parent!r}.{key} -> {ref_proxy.display_string} {ref_proxy.mixer_uuid}"
                )
                # TODO use Resolver class instead
                add_element = collection.link
                context.proxy_state.unresolved_refs.append(
                    ref_proxy.mixer_uuid, add_element, f"{collection!r}.link({ref_proxy.display_string})"
                )

    def apply(
        self,
        collection: T.bpy_prop_collection,
        parent: T.Collection,
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender: bool = True,
    ) -> DatablockRefCollectionProxy:
        """
        Apply delta to this proxy and optionally to the Blender attribute its manages.

        Args:
            attribute: a bpy_prop_collection of datablock references with link/unlink API (e.g. a_collection.objects)
            parent: the attribute that contains attribute (e.g. a Collection instance)
            key: the name of the bpy_collection in parent (e.g "objects")
            delta: the delta to apply
            context: proxy and visit state
            to_blender: update the managed Blender attribute in addition to this Proxy
        """
        update: DatablockRefCollectionProxy = delta.value
        assert type(update) == type(self)

        # objects or children
        for k, ref_delta in update._data.items():
            try:
                if not isinstance(ref_delta, (DeltaAddition, DeltaDeletion)):
                    logger.error(f"unexpected type for delta at {collection}[{k}]: {ref_delta}. Ignored")
                    continue
                ref_update: DatablockRefProxy = ref_delta.value
                if not isinstance(ref_update, DatablockRefProxy):
                    logger.error(f"unexpected type for delta_value at {collection}[{k}]: {ref_update}. Ignored")
                    continue

                assert isinstance(ref_update, DatablockRefProxy)
                if to_blender:
                    uuid = ref_update._datablock_uuid
                    datablock = context.proxy_state.datablock(uuid)
                    if isinstance(ref_delta, DeltaAddition):
                        if datablock is not None:
                            collection.link(datablock)
                        else:
                            # unloaded datablock
                            logger.warning(
                                f"delta apply add for {parent!r}.{key}: no datablock for {ref_update.display_string} ({uuid})"
                            )

                    else:
                        if datablock is not None:
                            collection.unlink(datablock)
                        # else
                        #   we have already processed an Objet removal. Not an error

                if isinstance(ref_delta, DeltaAddition):
                    self._data[k] = ref_update
                else:
                    del self._data[k]
            except Exception as e:
                logger.warning(f"DatablockCollectionProxy.apply(). Processing {ref_delta} to_blender {to_blender}")
                logger.warning(f"... for {collection}[{k}]")
                logger.warning(f"... Exception: {e!r}")
                logger.warning("... Update ignored")
                continue

        return self

    def diff(
        self, collection: T.bpy_prop_collection, key: str, collection_property: T.Property, context: Context
    ) -> Optional[DeltaUpdate]:
        """
        Computes the difference between the state of an item tracked by this proxy and its Blender state.

        As this proxy tracks a collection, the result will be a DeltaUpdate that contains a DatablockCollectionProxy
        with an Delta item per added, deleted or update item

        Args:
            collection: the collection diff against this proxy
            collection_property: the property of collection in its enclosing object
        """

        # This method is called from the depsgraph handler. The proxy holds a representation of the Blender state
        # before the modification being processed. So the changeset is (Blender state - proxy state)

        # TODO how can this replace BpyBlendDiff ?

        diff = self.__class__()

        item_property = collection_property.fixed_type

        # keys are uuids
        # BpyDataCollectionDiff.diff() for why proxies without datablocks are ignores
        proxy_keys = {k for k, v in self._data.items() if v.target(context)}

        blender_items = {datablock.mixer_uuid: datablock for datablock in collection.values()}
        blender_keys = blender_items.keys()
        added_keys = blender_keys - proxy_keys
        deleted_keys = proxy_keys - blender_keys
        maybe_updated_keys = proxy_keys & blender_keys

        for k in added_keys:
            value = read_attribute(blender_items[k], k, item_property, collection, context)
            assert isinstance(value, (DatablockProxy, DatablockRefProxy))
            diff._data[k] = DeltaAddition(value)

        for k in deleted_keys:
            diff._data[k] = DeltaDeletion(self._data[k])

        for k in maybe_updated_keys:
            delta = diff_attribute(blender_items[k], k, item_property, self.data(k), context)
            if delta is not None:
                assert isinstance(delta, DeltaUpdate)
                diff._data[k] = delta

        if len(diff._data):
            return DeltaUpdate(diff)

        return None
