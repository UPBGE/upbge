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
This module provides an implementation for the a proxy to the whole Blender data state, i.e the relevant members
of bpy.data.

See synchronization.md
"""
from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, field
from itertools import islice
import logging
import sys
from typing import Any, Callable, Dict, List, Optional, Set, Tuple, TYPE_CHECKING, Union
import pathlib

import bpy
import bpy.types as T  # noqa

from mixer.blender_data.bpy_data import collections_names
from mixer.blender_data.changeset import Changeset, RenameChangeset
from mixer.blender_data.datablock_collection_proxy import DatablockCollectionProxy
from mixer.blender_data.datablock_proxy import DatablockProxy
from mixer.blender_data.diff import BpyBlendDiff
from mixer.blender_data.filter import SynchronizedProperties, safe_depsgraph_updates, safe_properties
from mixer.blender_data.proxy import (
    DeltaReplace,
    DeltaUpdate,
    Proxy,
    MaxDepthExceeded,
    UnresolvedRefs,
    Uuid,
)

if TYPE_CHECKING:
    from mixer.blender_data.changeset import Removal
    from mixer.blender_data.library_proxies import LibraryProxy
    from mixer.blender_data.types import Path, SoaMember

logger = logging.getLogger(__name__)


class ProxyState:
    """
    State of a BpyDataProxy
    """

    def __init__(self):
        self.proxies: Dict[Uuid, DatablockProxy] = {}
        """Known proxies"""

        self._datablocks: Dict[Uuid, T.ID] = {}
        """Known datablocks"""

        self.objects: Dict[Uuid, Set[Uuid]] = defaultdict(set)
        """Object.data uuid : (set of uuids of Object using object.data). Mostly used for shape keys"""

        self.unresolved_refs: UnresolvedRefs = UnresolvedRefs()

        self.unregistered_libraries: Set[LibraryProxy] = set()
        """indirect libraries that were received but not yet registered because no datablock they provide were processed
        yet"""

        self.shared_folders: List[pathlib.Path] = []

    def register_object(self, datablock: T.Object):
        if datablock.data is not None:
            data_uuid = datablock.data.mixer_uuid
            object_uuid = datablock.mixer_uuid
            self.objects[data_uuid].add(object_uuid)

    def datablock(self, uuid: Uuid) -> Optional[T.ID]:
        datablock = self._datablocks.get(uuid)
        if datablock:
            try:
                _ = datablock.name
            except ReferenceError as e:
                logger.error(f"datablock {uuid} access exception {e!r}")
                # TODO returning a deleted datablock is dangerous, but returning None erases the difference between no
                # datablock and deleted datablock which causes some tests to fail. It would be better to return a
                # "LocallyDeleted" object. Also, remove_datablock should probably assign a tombstone instead of deleting
                # the entry. This requires reviewing all callers

                # datablock = None
        return datablock

    def add_datablock(self, uuid: Uuid, datablock: T.ID):
        assert self.datablock(uuid) in [datablock, None]
        self._datablocks[uuid] = datablock

    def remove_datablock(self, uuid: Uuid):
        del self._datablocks[uuid]

    def objects_using_data(self, data_datablock: T.ID) -> List[T.ID]:
        objects = [self.datablock(uuid) for uuid in self.objects[data_datablock.mixer_uuid]]
        return [object for object in objects if object is not None]


class VisitState:
    """
    Keeps track of relevent state during the proxy structure hierarchy visit.

    VisitState contains intra datablock or inter datablock state.

    Intra datablock state is local to a datablock. It is used to keek track of the datablock being processed or
    when the settign of a block attribute influences the processing of other attributes in a different structure.
    This state is reset when a new datablock is entered.

    Inter datablock state is global to the set of datablocks visited during an update. It is used when the state of one
    datablock influences the processing of another datablock.
    """

    _MAX_DEPTH = 30
    """Maximum nesting level, to guard against unfiltered circular references."""

    class CurrentDatablockContext:
        """Context manager to keep track of the current standalone datablock"""

        def __init__(self, visit_state: VisitState, proxy: DatablockProxy, datablock: T.ID):
            self._visit_state = visit_state
            self._is_embedded_data = datablock.is_embedded_data
            self._datablock_string = repr(datablock)
            self._proxy = proxy

        def __enter__(self):
            self._visit_state.send_nodetree_links = False
            if not self._is_embedded_data:
                self._visit_state.datablock_proxy = self._proxy
                self._visit_state.datablock_string = self._datablock_string

        def __exit__(self, exc_type, exc_value, traceback):
            self._visit_state.send_nodetree_links = False
            if not self._is_embedded_data:
                self._visit_state.datablock_proxy = None
                self._visit_state.datablock_string = None

    Path = List[Union[str, int]]
    """The current visit path relative to the datablock, for instance in a GreasePencil datablock
    ("layers", "MyLayer", "frames", 0, "strokes", 0, "points").
    Used to identify SoaElement buffer updates.
    Equivalent to a RNA path, parsed with indexed instead of names.
    Local state
    """

    def __init__(self):

        self.datablock_proxy: Optional[DatablockProxy] = None
        """The standalone datablock proxy being visited.

        Local state
        """

        self._attribute_path: List[Tuple[T.bpy_struct, Union[int, str]]] = []
        """The sequence of attributes and identifiers to the current property starting from the current datablock.
            [
                (bpy.data.objects[0], "modifiers")
                (bpy.data.objects[0].modifiers, 0)
                ...
            ]

        Note that the first item is the parent attribute of the current attribute and the second element is the
        identifier of the current attribute in its parent.

        Used to :
        - identify SoaElement buffer updates : for instance ["layers", "fills", "frames", 0, "strokes", 1, "points", 0]
        - guard against circular references,
        - keep track of where we come from

        Local state
        """

        self.dirty_vertex_groups: Set[Uuid] = set()
        """Uuids of the Mesh datablocks whose vertex_groups data has been updated since last loaded
        into their MeshProxy.

        Global state
        """

        self.send_nodetree_links: bool = False
        """NodeTree.nodes has been modified in a way that requires NodeTree.links to be resent.

        Intra datablock state
        """

        self.datablock_string: Optional[str] = None
        """"Current datablock display string, for logging"""

    def enter_datablock(self, proxy: DatablockProxy, datablock: T.ID) -> VisitState.CurrentDatablockContext:
        return VisitState.CurrentDatablockContext(self, proxy, datablock)

    def display_path(self) -> str:
        """Path to the attribute currently visited (e.g. "bpy.data.objects['Cube'].modifiers.0.name"), for logging"""
        if self._attribute_path:
            component = "." + ".".join([str(x[1]) for x in self._attribute_path])
        else:
            component = ""

        return str(self.datablock_string) + component

    def push(self, attribute: T.bpy_struct, key: Union[int, str]):
        if len(self._attribute_path) > self._MAX_DEPTH:
            raise MaxDepthExceeded(self.display_path())

        self._attribute_path.append((attribute, key))

    def pop(self):
        self._attribute_path.pop()

    def attribute(self, i: int) -> T.bpy_struct:
        """The i-th attribute visited, starting from the datablock.

        More useful from the end, the attribute at -1 being the one that contains the attribute being processed.
        """
        return self._attribute_path[i][0]

    def path(self) -> Tuple[Union[int, str], ...]:
        """The path of the attribute being processed.

        In load(), save(), diff(), apply() the value when visiting the first modifier of bpy.data.objects[0] is
            [
                (bpy.data.objects[0], "modifiers")
                (bpy.data.objects[0].modifiers, 0)
            ]
        """
        return tuple(item[1] for item in self._attribute_path)


@dataclass
class Context:
    proxy_state: ProxyState
    """Proxy system state"""

    synchronized_properties: SynchronizedProperties
    """Controls what properties are synchronized"""

    visit_state: VisitState = field(default_factory=VisitState)
    """Current datablock operation state"""


_creation_order = {
    # Libraries are needed to create all linked datablocks
    "libraries": -10,
    # before materials
    "node_groups": -10,
    # before curves
    "fonts": -5,
    # anything else: 0
    "collections": 10,
    # Scene after Collection. Scene.collection must be up to date before Scene.view_layers can be saved
    "scenes": 20,
    # Object.data is required to create Object
    "objects": 30,
    # Key creation require Object API
    "shape_keys": 40,
}


def _creation_order_predicate(item: Tuple[str, Any]) -> int:
    # item (bpy.data collection name, delta)
    return _creation_order.get(item[0], 0)


_updates_order = {
    # before Mesh for shape keys
    T.Key: 5,
    # before Object for vertex_groups
    T.Mesh: 10,
    # before Object. Object.bones update require Armature bones to be updated first
    T.Armature: 12,
    # Before Scene since LayerCollection.children are available when the collection is created
    T.Collection: 15
    # anything else last
}


def _updates_order_predicate(datablock: T.ID) -> int:
    return _updates_order.get(type(datablock), sys.maxsize)


_removal_order = {
    # remove Object before its data otherwise data is removed at the time the Object is removed
    # and the data removal fails
    "objects": 10,
    # anything else last
}


def _remove_order_predicate(removal: Removal) -> int:
    return _removal_order.get(removal[1], sys.maxsize)


def retain(arg):
    """Decorator that delays BypDataProxy methods calls while not in OBJECT mode.

    Used to defer the execution of received update until mode is back to OBJECT. This makes is possible to use undo
    while not in OBJECT mode.
    """

    def retain_(f):
        def wrapper(*args, **kwargs):
            def func():
                return f(*args, **kwargs)

            if bpy.context.mode != "OBJECT":
                bpy_data_proxy = args[0]
                bpy_data_proxy._delayed_remote_updates.append(func)
                return arg
            else:
                return func()

        return wrapper

    return retain_


class BpyDataProxy(Proxy):
    """Proxy to bpy.data collections

    This proxy contains a DatablockCollection proxy per synchronized bpy.data collection
    """

    def __init__(self, *args, **kwargs):

        self.state: ProxyState = ProxyState()

        self._data = {name: DatablockCollectionProxy(name) for name in collections_names()}

        self._delayed_local_updates: Set[Uuid] = set()
        """Local datablock updates retained until returning to Object mode.
        This avoids transmitting edit mode updates in real time"""

        self._delayed_remote_updates: List[Callable[[], None]] = []
        """Remote datablock updates retained until returning to Object mode."""

    def clear(self):
        self._data.clear()
        self.state.proxies.clear()
        self.state._datablocks.clear()

    def reload_datablocks(self):
        datablocks = self.state._datablocks
        datablocks.clear()

        for collection_proxy in self._data.values():
            collection_proxy.reload_datablocks(datablocks)

        try:
            del datablocks[""]
        except KeyError:
            pass

    def snapshot_undo_pre(self):
        """Record pre undo state to recover undone uuids."""
        for collection_proxy in self._data.values():
            collection_proxy.snapshot_undo_pre()

    def snapshot_undo_post(self):
        """Compare post undo uuid state to recover undone uuids."""
        all_undone: List[Tuple[str, Dict[str, Uuid]]] = []
        for collection_proxy in self._data.values():
            undone = collection_proxy.snapshot_undo_post()
            if undone:
                all_undone.append(undone)

        # only for logging
        return all_undone

    def context(self, synchronized_properties: SynchronizedProperties = safe_properties) -> Context:
        return Context(self.state, synchronized_properties)

    def set_shared_folders(self, shared_folders: List):
        normalized_folders = []
        for folder in shared_folders:
            normalized_folders.append(pathlib.Path(folder))
        self.state.shared_folders = normalized_folders

    def load(self, synchronized_properties: SynchronizedProperties):
        """FOR TESTS ONLY Load the current scene into this proxy

        Only used for test. The initial load is performed by update()
        """
        diff = BpyBlendDiff()
        diff.diff(self, synchronized_properties)
        self.update(diff, set(), False, synchronized_properties)
        return self

    def find(self, collection_name: str, key: str) -> Optional[DatablockProxy]:
        # TODO not used ?
        if not self._data:
            return None
        collection_proxy = self._data.get(collection_name)
        if collection_proxy is None:
            return None
        return collection_proxy.find(key)

    def update(
        self,
        diff: BpyBlendDiff,
        updates: Set[Uuid],
        process_delayed_updates: bool,
        synchronized_properties: SynchronizedProperties = safe_properties,
    ) -> Changeset:
        """
        Process local changes, i.e. created, removed and renames datablocks as well as depsgraph updates.

        This updates the local proxy state and return a Changeset to send to the server. This method is also
        used to send the initial scene contents, which is seen as datablock creations.

        Args:
            update: the updates datablock from the last depsgraph_update handler call
            process_delayed_updates: the updates that were delayed from previous depsgraph handler call
            (mainly because not in edit mode) must now be procesed
        """
        # Update the bpy.data collections status and get the list of newly created bpy.data entries.
        # Updated proxies will contain the IDs to send as an initial transfer.
        # There is no difference between a creation and a subsequent update
        changeset: Changeset = Changeset()

        # Contains the bpy_data_proxy state (known proxies and datablocks), as well as visit_state that contains
        # shared state between updated datablock proxies
        context = self.context(synchronized_properties)

        deltas = sorted(diff.collection_deltas, key=_creation_order_predicate)
        for delta_name, delta in deltas:
            collection_changeset = self._data[delta_name].update(delta, context)
            changeset.creations.extend(collection_changeset.creations)
            changeset.removals.extend(collection_changeset.removals)
            changeset.renames.extend(collection_changeset.renames)

        # Everything is sorted with Object last, but the removals need to be sorted the other way round,
        # otherwise the receiver might get a Mesh remove (that removes the Object as well), then an Object remove
        # message for a non existent objjet that triggers a noisy warning, otherwise useful
        changeset.removals = sorted(changeset.removals, key=_remove_order_predicate)

        all_updates = updates
        if process_delayed_updates:
            for f in self._delayed_remote_updates:
                f()
            self._delayed_remote_updates.clear()
            all_updates |= {self.state.datablock(uuid) for uuid in self._delayed_local_updates}
            self._delayed_local_updates.clear()

        sorted_updates = sorted(all_updates, key=_updates_order_predicate)

        for datablock in sorted_updates:
            if not isinstance(datablock, safe_depsgraph_updates):
                logger.info("depsgraph update: ignoring untracked type %r", datablock)
                continue
            if isinstance(datablock, T.Scene) and datablock.name == "_mixer_to_be_removed_":
                logger.error(f"Skipping scene {datablock.name} uuid: '{datablock.mixer_uuid}'")
                continue
            proxy = self.state.proxies.get(datablock.mixer_uuid)
            if proxy is None:
                # Not an error for embedded IDs.
                if not datablock.is_embedded_data:
                    logger.warning(f"depsgraph update for {datablock!r} : no proxy and not datablock.is_embedded_data")
                else:
                    # For instance Scene.node_tree is not a reference to a bpy.data collection element
                    # but a "pointer" to a NodeTree owned by Scene. In such a case, the update list contains
                    # scene.node_tree, then scene. We can ignore the scene.node_tree update since the
                    # processing of scene will process scene.node_tree.
                    # However, it is not obvious to detect the safe cases and remove the message in such cases
                    logger.info("depsgraph update: Ignoring embedded %r", datablock)
                continue
            delta = proxy.diff(datablock, datablock.name, None, context)
            if delta:
                logger.info("depsgraph update: update %r", datablock)
                # TODO add an apply mode to diff instead to avoid two traversals ?
                proxy.apply_to_proxy(datablock, delta, context)
                changeset.updates.append(delta)
            else:
                logger.debug("depsgraph update: ignore empty delta %r", datablock)

        return changeset

    @retain(
        (None, None),
    )
    def create_datablock(
        self, incoming_proxy: DatablockProxy, synchronized_properties: SynchronizedProperties = safe_properties
    ) -> Tuple[Optional[T.ID], Optional[RenameChangeset]]:
        """
        Process a received datablock creation command, creating the datablock and updating the proxy state
        """
        bpy_data_collection_proxy = self._data.get(incoming_proxy.collection_name)
        if bpy_data_collection_proxy is None:
            logger.error(f"create_datablock: no bpy_data_collection_proxy with name {incoming_proxy.collection_name} ")
            return None, None

        context = self.context(synchronized_properties)
        return bpy_data_collection_proxy.create_datablock(incoming_proxy, context)

    @retain(None)
    def update_datablock(self, update: DeltaUpdate, synchronized_properties: SynchronizedProperties = safe_properties):
        """
        Process a received datablock update command, updating the datablock and the proxy state
        """
        assert isinstance(update, (DeltaUpdate, DeltaReplace))
        incoming_proxy: DatablockProxy = update.value
        bpy_data_collection_proxy = self._data.get(incoming_proxy.collection_name)
        if bpy_data_collection_proxy is None:
            logger.warning(
                f"update_datablock: no bpy_data_collection_proxy with name {incoming_proxy.collection_name} "
            )
            return None

        context = self.context(synchronized_properties)
        bpy_data_collection_proxy.update_datablock(update, context)

    @retain(None)
    def remove_datablock(self, uuid: str):
        """
        Process a received datablock removal command, removing the datablock and updating the proxy state
        """
        proxy = self.state.proxies.get(uuid)
        if proxy is None:
            logger.error(f"remove_datablock(): no proxy for {uuid}")
            return

        bpy_data_collection_proxy = self._data.get(proxy.collection_name)
        if bpy_data_collection_proxy is None:
            logger.warning(f"remove_datablock: no bpy_data_collection_proxy with name {proxy.collection_name} ")
            return None

        datablock = self.state.datablock(uuid)

        if isinstance(datablock, T.Object) and datablock.data is not None:
            data_uuid = datablock.data.mixer_uuid
        else:
            data_uuid = None

        bpy_data_collection_proxy.remove_datablock(proxy, datablock)

        if data_uuid is not None:
            # removed an Object
            self.state.objects[data_uuid].remove(uuid)
        else:
            try:
                # maybe removed an Object.data pointee
                del self.state.objects[uuid]
            except KeyError:
                pass
        del self.state.proxies[uuid]
        del self.state._datablocks[uuid]

    @retain([])
    def rename_datablocks(self, items: List[Tuple[str, str, str]]) -> RenameChangeset:
        """
        Process a received datablock rename command, renaming the datablocks and updating the proxy state.
        (receiver side)
        """
        rename_changeset_to_send: RenameChangeset = []
        renames = []
        for uuid, old_name, new_name in items:
            proxy = self.state.proxies.get(uuid)
            if proxy is None:
                logger.error(f"rename_datablocks(): no proxy for {uuid} (debug info)")
                return []

            bpy_data_collection_proxy = self._data.get(proxy.collection_name)
            if bpy_data_collection_proxy is None:
                logger.warning(f"rename_datablock: no bpy_data_collection_proxy with name {proxy.collection_name} ")
                continue

            datablock = self.state.datablock(uuid)
            tmp_name = f"_mixer_tmp_{uuid}"
            if datablock.name != new_name and datablock.name != old_name:
                # local receives a rename, but its datablock name does not match the remote datablock name before
                # the rename. This means that one of these happened:
                # - local has renamed the datablock and remote will receive the rename command later on
                # - local has processed a rename command that remote had not yet processed, but will process later on
                # ensure that everyone renames its datablock with the **same** name
                new_name = new_name = f"_mixer_rename_conflict_{uuid}"
                logger.warning(f"rename_datablocks: conflict for existing {datablock}")
                logger.warning(f'... incoming old name "{old_name}" new name "{new_name}"')
                logger.warning(f"... using {new_name}")

                # Strangely, for collections not everyone always detect a conflict, so rename for everyone
                rename_changeset_to_send.append(
                    (
                        datablock.mixer_uuid,
                        datablock.name,
                        new_name,
                        f"Conflict bpy.data.{proxy.collection_name}[{datablock.name}] into {new_name}",
                    )
                )

            renames.append([bpy_data_collection_proxy, proxy, old_name, tmp_name, new_name, datablock])

        # The rename process is handled in two phases to avoid spontaneous renames from Blender
        # see DatablockCollectionProxy.update() for explanation
        for bpy_data_collection_proxy, proxy, _, tmp_name, _, datablock in renames:
            bpy_data_collection_proxy.rename_datablock(proxy, tmp_name, datablock)

        for bpy_data_collection_proxy, proxy, _, _, new_name, datablock in renames:
            bpy_data_collection_proxy.rename_datablock(proxy, new_name, datablock)

        return rename_changeset_to_send

    def diff(self, synchronized_properties: SynchronizedProperties) -> Optional[BpyDataProxy]:
        """Currently for tests only"""
        diff = self.__class__()
        context = self.context(synchronized_properties)
        for name, proxy in self._data.items():
            collection = getattr(bpy.data, name, None)
            if collection is None:
                logger.warning(f"Unknown, collection bpy.data.{name}")
                continue
            collection_property = bpy.data.bl_rna.properties.get(name)
            delta = proxy.diff(collection, collection_property, context)
            if delta is not None:
                diff._data[name] = diff
        if len(diff._data):
            return diff
        return None

    @retain(False)
    def update_soa(self, uuid: Uuid, path: Path, soa_members: List[SoaMember]) -> bool:
        """Update the arrays if the proxy identified by uuid.

        Returns:
            True if the view layer should be updated
        """
        datablock_proxy = self.state.proxies[uuid]
        datablock = self.state.datablock(uuid)
        return datablock_proxy.update_soa(datablock, path, soa_members)

    def append_delayed_updates(self, delayed_updates: Set[T.ID]):
        self._delayed_local_updates |= {update.mixer_uuid for update in delayed_updates}

    def sanity_check(self):
        state = self.state
        datablock_keys = set(state._datablocks.keys())
        proxy_keys = set(state.proxies.keys())
        if datablock_keys != proxy_keys:
            logger.error(f"sanity_check: {len(datablock_keys ^ proxy_keys)} different keys for datablocks and proxies")

        # avoid logging large number of error messages with very large scenes
        max_items = 5
        unregistered_libraries = state.unregistered_libraries
        unregistered_libraries_count = len(unregistered_libraries)
        if unregistered_libraries:
            logger.warning(f"sanity_check: {unregistered_libraries_count} unregistered libraries ...")
            for lib in islice(unregistered_libraries, max_items):
                logger.warning(f"... {lib}. Library file may be missing.")
            if len(unregistered_libraries) > max_items:
                logger.warning(f"... {unregistered_libraries_count - max_items} more.")

        none_datablocks = [k for k, v in state._datablocks.items() if v is None and not state.proxies[k].has_datablock]
        if none_datablocks:
            logger.warning("sanity_check: no datablock for ...")
            for uuid in none_datablocks[:max_items]:
                logger.warning(f"... {state.proxies[uuid]}.")
            hidden_count = len(none_datablocks) - max_items
            if hidden_count > 0:
                logger.warning(f"... {hidden_count} more.")
            logger.warning("... check for missing libraries or other files")
            logger.warning("... datablocks referencing broken files may be removed from peers !")

        # given that none_datablocks are missing because of missing files, so it not an error per se that references to
        # the are left unresolved
        unresolved_uuids = set(state.unresolved_refs._refs.keys()) - set(none_datablocks)
        if unresolved_uuids:
            logger.warning("sanity_check: unresolved_refs not empty ...")
            for uuid in islice(unresolved_uuids, max_items):
                logger.warning(f"... {uuid} : {state.unresolved_refs._refs[uuid][0][1]}")
            hidden_count = len(unresolved_uuids) > max_items
            if hidden_count > 0:
                logger.warning(f"... {hidden_count} more.")
            logger.warning("... check for unsupported datablock types")
