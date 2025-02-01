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
Proxy classes for library-related items, i.e. libraries and link datablocks

See synchronization.md
"""
from __future__ import annotations

import logging
from typing import cast, Dict, Optional, Tuple, TYPE_CHECKING, Union

import bpy
import bpy.path
import bpy.types as T  # noqa

from mixer.blender_data.proxy import Delta, DeltaUpdate, ExternalFileFailed
from mixer.blender_data.datablock_proxy import DatablockProxy
from mixer.blender_data.json_codec import serialize


if TYPE_CHECKING:
    from mixer.blender_data.proxy import Context, Uuid

logger = logging.getLogger(__name__)


@serialize
class LibraryProxy(DatablockProxy):
    """Proxy for Library datablocks."""

    def __init__(self):
        super().__init__()

        self._unregistered_datablocks: Dict[str, Uuid] = {}
        """Uuids to assign to indirect link datablocks after their creation."""

    def __eq__(self, other):
        return self is other

    def __hash__(self):
        return hash(self.mixer_uuid)

    def register_indirect(self, identifier: str, uuid: Uuid, context: Context) -> Optional[T.ID]:
        """Registers an indirect link datablock provided by the Library managed by this proxy.

        Args:
            identifier : the datablock name_full
            uuid : the uuid of the link datablock
            context : proxy and visit state

        Returns:
            The link datablock if is is already loaded or None if it has not yet been loaded (because the direct link
            datablock that references it is not yet loaded)
        """

        if identifier in self._unregistered_datablocks and self._unregistered_datablocks[identifier] != uuid:
            logger.error(f"register_indirect: in {self} ...")
            logger.error(f"... {identifier}({uuid}) already in unregistered list ...")
            logger.error(f"... with {self._unregistered_datablocks[identifier]}. Update ignored")
            return None

        library_datablock = bpy.data.libraries.get(self._data["name"])
        if library_datablock:
            # The library is already loaded. Register the linked datablock at once.
            # Registration in ProxyState.datablocks is performed by a caller during datablock creation
            # TODO/perf: users_id iterates over all items of all collections
            for linked_datablock in library_datablock.users_id:
                if repr(linked_datablock) == identifier:
                    # logger.warning(f"register indirect for {library_datablock}: {identifier} {uuid}")
                    return linked_datablock

        #   The library is not already loaded:
        #       when processing an indirect link datablock (e.g. Mesh) _before_ the first direct link datablock
        #       that references it (e.g. Object) has been processed.
        # or
        #   The library is loaded but the datablock is not yet in Library.users_id.
        #       when processing an additional indirect link datablock (e.g. an additional Mesh) _after_
        #       the library is loaded by the processing of a previous direct link datablock.
        #       This occurs when linking an additional object

        # Register the indirect datablock for update after the library is loaded.
        # logger.warning(f"register indirect: delayed for {identifier} {uuid}")
        context.proxy_state.unregistered_libraries.add(self)
        self._unregistered_datablocks[identifier] = uuid
        return None

    def create_standalone_datablock(self, context: Context):
        # No datablock is created at this point.
        # The Library datablock will be created when the linked datablock is loaded (see load_library_item)

        resolved_filepath = self.resolved_filepath(context)
        self._data["filepath"] = resolved_filepath
        self._data["filepath_raw"] = resolved_filepath
        return None, None

    def load_library_item(self, collection_name: str, datablock_name: str, context: Context) -> T.ID:
        """Load a direct link datablock.

        Args:
            collection_name: the name of the bpy.data collection ("objects", "cameras", ...) in which the datablock must
            be created.
            datablock_name: the  name of the datablock in the library
            context: proxy and visit_state
        Raises:
            ExternalFileFailed: [description]

        Returns:
            The link datablock.
        """

        library_path = self.resolved_filepath(context)
        if library_path is None:
            logger.error(f"load_library_item(): no file for {self._filepath_raw!r} ...")
            logger.error(f"... referenced by bpy.data.{collection_name}[{datablock_name}]")
            logger.error("... check Shared Folders")
            # TODO not the best exception type
            raise ExternalFileFailed

        logger.warning(f"load_library_item(): from {library_path} : {collection_name}[{datablock_name}]")

        try:
            # this creates the Library datablock on first load.
            with bpy.data.libraries.load(library_path, link=True) as (data_from, data_to):
                setattr(data_to, collection_name, [datablock_name])
        except OSError as e:
            raise ExternalFileFailed from e

        try:
            linked_datablock = getattr(data_to, collection_name)[0]
        except IndexError:
            # TODO not the best exception type
            raise ExternalFileFailed

        library_datablock = linked_datablock.library
        self.register(library_datablock, context)
        return linked_datablock

    def register(self, library_datablock: T.Library, context: Context):
        """Recursively register the Library managed by this proxy, its children and all the datablocks they provide."""

        proxy_state = context.proxy_state

        if not self._has_datablock:
            self._has_datablock = True
            # The received datablock name might not match the library name
            library_datablock.name = self.data("name")
            library_datablock.mixer_uuid = self.mixer_uuid
            proxy_state.add_datablock(self.mixer_uuid, library_datablock)

        # Register the link datablocks provided by this library

        # TODO/perf: users_id iterates over all items of all collections
        for linked_datablock in library_datablock.users_id:
            identifier = repr(linked_datablock)
            uuid = self._unregistered_datablocks.get(identifier)
            if uuid:
                # logger.warning(f"register indirect at load {identifier} {uuid}")
                linked_datablock.mixer_uuid = uuid
                proxy_state.proxies[uuid]._has_datablock = True
                proxy_state.add_datablock(uuid, linked_datablock)
                del self._unregistered_datablocks[identifier]

        if self in proxy_state.unregistered_libraries and not self._unregistered_datablocks:
            proxy_state.unregistered_libraries.remove(self)

        # Recursively register pending child libraries and their datablocks
        for unregistered_child_proxy in list(proxy_state.unregistered_libraries):
            child_name = unregistered_child_proxy.data("name")
            children = [datablock for datablock in bpy.data.libraries if datablock.name == child_name]
            if not children:
                continue

            if len(children) > 1:
                logger.warning(f"register: more than one library found with name {child_name!r} ...")
                logger.warning(f"... {children}")
                continue

            child_library = children[0]
            if child_library.parent == library_datablock:
                unregistered_child_proxy.register(child_library, context)

    def load(self, datablock: T.ID, context: Context) -> LibraryProxy:
        logger.warning(f"load(): {datablock}")
        super().load(datablock, context)
        return self

    def save(self, unused_attribute, parent: T.bpy_struct, key: Union[int, str], context: Context):
        """"""
        # Nothing to save to Blender when the LibraryProxy is received.
        # The Library datablock will be created when the linked datablock is loaded (see load_library_item)
        pass

    def apply(
        self,
        attribute: Union[T.bpy_struct, T.bpy_prop_collection],
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender: bool = True,
    ):

        """
        Apply delta to an attribute with None value.

        This is used for instance Scene.camera is None and update to hold a valid Camera reference

        Args:
            attribute: the Blender attribute to update (e.g a_scene.camera)
            parent: the attribute that contains attribute (e.g. a Scene instance)
            key: the key that identifies attribute in parent (e.g; "camera").
            delta: the delta to apply
            context: proxy and visit state
            to_blender: update attribute in addition to this Proxy
        """
        raise NotImplementedError("LibraryProxy.apply()")
        return self

    def diff(
        self,
        container: Union[T.bpy_prop_collection, T.Struct],
        key: Union[str, int],
        prop: T.Property,
        context: Context,
    ) -> Optional[DeltaUpdate]:
        raise NotImplementedError("LibraryProxy.diff()")
        return None


@serialize
class DatablockLinkProxy(DatablockProxy):
    """Proxy for direct or indirect linked datablock"""

    _serialize = DatablockProxy._serialize + ("_library_uuid", "_is_library_indirect", "_name", "_identifier")

    def __init__(self):
        super().__init__()

        self._library_uuid: Uuid = ""
        """Uuid of the library datablock"""

        self._name = ""
        """Name of the datablock in the library"""

        self._identifier = ""
        """repr() value for the datablock, used as identifier to """

        self._is_library_indirect = False

    def __str__(self) -> str:
        return f"{self.__class__.__name__} {self.mixer_uuid} for {self._identifier}"

    @property
    def is_library_indirect(self):
        return self._is_library_indirect

    def create_standalone_datablock(self, context: Context) -> Tuple[Optional[T.ID], None]:
        """Save this proxy into its target standalone datablock."""

        # Some parts must be kept in sync with DatablockProxy.create_standalone_datablock()

        from mixer.blender_data.library_proxies import LibraryProxy

        proxy_state = context.proxy_state

        library_proxy = cast(LibraryProxy, proxy_state.proxies[self._library_uuid])

        if self._is_library_indirect:
            # Indirect linked datablock are created implicitely during the load() of their parent. Keep track of
            # them in order to assign them a uuid after their creation. A uuid is required because they can be
            # referenced by non linked datablocks after load (e.g. a linked Camera referenced by the main Scene)
            uuid = self.mixer_uuid
            datablock = library_proxy.register_indirect(self._identifier, uuid, context)
            if datablock is not None:
                self._has_datablock = True
                datablock.mixer_uuid = uuid
                proxy_state.add_datablock(uuid, datablock)
                if isinstance(datablock, T.Object):
                    proxy_state.register_object(datablock)

            return datablock, None

        try:
            datablock = library_proxy.load_library_item(self._bpy_data_collection, self._name, context)
        except ExternalFileFailed:
            return None, None
        except Exception as e:
            logger.error(
                f"load_library {library_proxy.data('name')!r} failed for bpy.data.{self._bpy_data_collection}[{self._name}] ..."
            )
            logger.error(f"... {e!r}")
            return None, None

        self._has_datablock = True
        uuid = self.mixer_uuid
        datablock.mixer_uuid = uuid
        context.proxy_state.add_datablock(uuid, datablock)
        if isinstance(datablock, T.Object):
            context.proxy_state.register_object(datablock)

        return datablock, None

    def load(self, datablock: T.ID, context: Context) -> DatablockLinkProxy:
        """Load datablock into this proxy."""
        assert datablock.library is not None

        # Do not load the attributes
        self._library_uuid = datablock.library.mixer_uuid
        self._is_library_indirect = datablock.is_library_indirect
        self._name = datablock.name
        self._identifier = repr(datablock)
        self._has_datablock = True

        if isinstance(datablock, T.Object):
            context.proxy_state.register_object(datablock)

        return self

    def apply(
        self,
        attribute,
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender: bool = True,
    ) -> DatablockLinkProxy:
        """
        Apply delta to this proxy and optionally to the Blender attribute its manages.

        Args:
            attribute: the Blender attribute to update
            parent: the attribute that contains attribute
            key: the key that identifies attribute in parent
            delta: the delta to apply
            context: proxy and visit state
            to_blender: update the managed Blender attribute in addition to this Proxy
        """
        return self

    def diff(
        self, attribute, unused_key: Union[int, str], unused_prop: T.Property, unused_context: Context
    ) -> Optional[Delta]:
        """
        Computes the difference between the state of an item tracked by this proxy and its Blender state.

        Args:
            attribute: the set to update (e.g. a the "delimit" attribute of a DecimateModifier instance)
            unused_key: the key that identifies attribute in parent (e.g "delimit")
            unused_prop: the Property of attribute as found in its parent attribute
            unused_context: proxy and visit state
        """
        pass
