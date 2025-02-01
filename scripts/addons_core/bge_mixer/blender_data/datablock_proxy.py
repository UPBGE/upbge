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
Proxy of a datablock

See synchronization.md
"""
from __future__ import annotations

from collections import defaultdict
import logging
from typing import Dict, List, Optional, Tuple, TYPE_CHECKING, Union
import pathlib

import bpy
import bpy.types as T  # noqa

from mixer.blender_data import specifics
from mixer.blender_data.bpy_data import rna_identifier_to_collection_name

from mixer.blender_data.attributes import read_attribute, write_attribute
from mixer.blender_data.json_codec import serialize
from mixer.blender_data.proxy import Delta, DeltaReplace, DeltaUpdate, ExternalFileFailed, Uuid
from mixer.blender_data.misc_proxies import CustomPropertiesProxy
from mixer.blender_data.struct_proxy import StructProxy
from mixer.blender_data.type_helpers import sub_id_type
from mixer.local_data import get_resolved_file_path, get_source_file_path

if TYPE_CHECKING:
    from mixer.blender_data.aos_soa_proxy import SoaElement
    from mixer.blender_data.bpy_data_proxy import RenameChangeset, Context
    from mixer.blender_data.types import ArrayGroups, Path, SoaMember


DEBUG = True

logger = logging.getLogger(__name__)


@serialize
class DatablockProxy(StructProxy):
    """
    Proxy to a standalone datablock (e.g. bpy.data.cameras['Camera']).
    """

    _serialize: Tuple[str, ...] = (
        "_bpy_data_collection",
        "_data",
        "_datablock_uuid",
        "_custom_properties",
        "_is_in_shared_folder",
        "_filepath_raw",
        "_type_name",
    )

    def __init__(self):
        # deserialization call the ctor with no arguments

        super().__init__()

        self._bpy_data_collection: str = ""
        """name of the bpy.data collection this datablock belongs to, None if embedded in another datablock"""

        self._datablock_uuid: Uuid = ""

        self._custom_properties = CustomPropertiesProxy()

        self._soas: Dict[Path, List[Tuple[str, SoaElement]]] = defaultdict(list)
        """e.g. {
            ("vertices"): [("co", co_soa), ("normals", normals_soa)]
            ("edges"): ...
        }
        Serialized as array"""

        # TODO move into _arrays
        self._media: Optional[Tuple[str, bytes]] = None
        """Media file data.
        Serialized as array"""
        self._is_in_shared_folder: Optional[bool] = None
        self._filepath_raw: Optional[str] = None

        self._arrays: ArrayGroups = {}
        """arrays that must not be serialized as json because of their size.
        Serialized as array"""

        self._has_datablock = False
        """False for local (non link) datablocks received if they cannot be created (file error for images), for a
        library before the first link datalock is loaded, for an indirect link datablock before the direct link
        datablock is loaded. Always True on the sender side."""

        self._type_name: str = ""
        """The type name in the bpy.type module, e.g. Object, TextCurve"""

    def copy_data(self, other: DatablockProxy):
        super().copy_data(other)
        self._soas = other._soas
        self._media = other._media
        self._arrays = other._arrays

    def clear_data(self):
        super().clear_data()
        self._soas.clear()
        self._arrays.clear()
        if self._media:
            self._media.clear()

    @property
    def arrays(self):
        return self._arrays

    @arrays.setter
    def arrays(self, arrays: ArrayGroups):
        self._arrays = arrays

    @property
    def has_datablock(self):
        return self._has_datablock

    @classmethod
    def make(cls, datablock: T.ID) -> DatablockProxy:
        proxy: DatablockProxy
        if datablock.library:
            from mixer.blender_data.library_proxies import DatablockLinkProxy

            proxy = DatablockLinkProxy()

        elif isinstance(datablock, T.Armature):
            from mixer.blender_data.armature_proxy import ArmatureProxy

            proxy = ArmatureProxy()
        elif isinstance(datablock, T.Library):
            from mixer.blender_data.library_proxies import LibraryProxy

            proxy = LibraryProxy()
        elif isinstance(datablock, T.Object):
            from mixer.blender_data.object_proxy import ObjectProxy

            proxy = ObjectProxy()
        elif isinstance(datablock, T.Mesh):
            from mixer.blender_data.mesh_proxy import MeshProxy

            proxy = MeshProxy()

        elif isinstance(datablock, T.Key):
            from mixer.blender_data.shape_key_proxy import ShapeKeyProxy

            proxy = ShapeKeyProxy()
        else:
            proxy = DatablockProxy()

        type_name = sub_id_type(type(datablock)).bl_rna.identifier
        proxy._bpy_data_collection = rna_identifier_to_collection_name()[type_name]
        proxy._datablock_uuid = datablock.mixer_uuid
        return proxy

    @property
    def mixer_uuid(self) -> str:
        return self._datablock_uuid

    def rename(self, new_name: str):
        self._data["name"] = new_name

    def __str__(self) -> str:
        return f"{self.__class__.__name__} {self.mixer_uuid} for bpy.data.{self.collection_name}[{self.data('name')}]"

    def load(
        self,
        datablock: T.ID,
        context: Context,
    ) -> DatablockProxy:
        """Load a datablock into this proxy.

        Args:
            datablock: the embedded datablock to load into this proxy
            context: visit and proxy state

        Returns:
            this DatablockProxy
        """

        self.clear_data()
        self._type_name = type(datablock).__name__

        self._has_datablock = True
        if isinstance(datablock, T.Object):
            context.proxy_state.register_object(datablock)

        properties = context.synchronized_properties.properties(datablock)
        # this assumes that specifics.py apply only to ID, not Struct
        properties = specifics.conditional_properties(datablock, properties)
        with context.visit_state.enter_datablock(self, datablock):
            for name, bl_rna_property in properties:
                attr = getattr(datablock, name)
                attr_value = read_attribute(attr, name, bl_rna_property, datablock, context)
                # Also write None values to reset attributes like Camera.dof.focus_object
                # TODO for scene, test difference, only send update if dirty as continuous updates to scene
                # master collection will conflicting writes with Master Collection
                self._data[name] = attr_value

        self.attach_filepath_raw(datablock)
        self.attach_media_descriptor(datablock, context)
        self._custom_properties.load(datablock)
        return self

    def attach_filepath_raw(self, datablock: T.ID):
        if hasattr(datablock, "filepath"):
            filepath = datablock.filepath
            if len(filepath) == 0:
                return
            if filepath[0] == "<" and filepath[-1] == ">":
                # various builtin names, like "<builtin>" for VectorFont, or "<startup.blend>" for Library
                return

            try:
                path_string = get_source_file_path(bpy.path.abspath(filepath))
            except OSError as e:
                logger.warning(f"{datablock!r}: invalid file path {filepath} ...")
                logger.warning(f"... {e!r}")
                return

            path = pathlib.Path(path_string)
            if not path.exists():
                logger.warning(f"{datablock!r}: file with computed source path does not exist ...")
                logger.warning(f"... filepath: '{filepath}'")
                logger.warning(f"... abspath:  '{bpy.path.abspath(filepath)}'")
                logger.warning(f"... source:   '{path_string}'")
            self._filepath_raw = str(path)

    def matches_shared_folder(self, filepath_str: str, context: Context):
        filepath = pathlib.Path(filepath_str)
        for shared_folder in context.proxy_state.shared_folders:
            try:
                relative_path = filepath.relative_to(shared_folder)
            except ValueError:
                continue
            return str(relative_path)
        return None

    def resolve_shared_folder_file(self, relative_path: str, context: Context):
        resolved_path = None
        for shared_folder in context.proxy_state.shared_folders:
            shared_folder_file = shared_folder / relative_path
            if shared_folder_file.is_file():
                if resolved_path is None:
                    resolved_path = str(shared_folder_file)
                else:
                    logger.error("Unable to resolve shared_folder file: multiple matches found")
                    resolved_path = None
                    break
        return resolved_path

    def resolved_filepath(self, context: Context) -> Optional[str]:
        """Returns the local filepath for the datablock.

        This references a file in "shared files" or a temporary file"""
        if self._filepath_raw is None:
            return None

        if not self._is_in_shared_folder:
            resolved_filepath = get_resolved_file_path(self._filepath_raw)
            logger.info(f"resolved_filepath: for {self} not in shared folder ...")
            logger.info(f"... resolve {self._filepath_raw!r} to {resolved_filepath}")
        else:
            resolved_filepath = self.resolve_shared_folder_file(self._filepath_raw, context)
            if resolved_filepath is None:
                logger.info(f'"{self._filepath_raw}" not in shared_folder')
            logger.info(f"resolved_filepath: for {self} in shared folder ...")
            logger.info(f"... resolve {self._filepath_raw!r} to {resolved_filepath}")
        return resolved_filepath

    def attach_media_descriptor(self, datablock: T.ID, context: Context):
        # if Image, Sound, Library, MovieClip, Text, VectorFont, Volume
        # create a self._media with the data to be sent
        # - filepath
        # - reference to the packed data if packed
        #
        #
        if hasattr(datablock, "packed_file"):
            self._is_in_shared_folder = False
            if self._filepath_raw is None:
                return

            packed_file = datablock.packed_file
            data = None
            if packed_file is not None:
                data = packed_file.data
                self._media = (get_source_file_path(self._filepath_raw), data)
                return

            relative_to_shared_folder_path = self.matches_shared_folder(self._filepath_raw, context)
            if relative_to_shared_folder_path is not None:
                self._filepath_raw = relative_to_shared_folder_path
                self._is_in_shared_folder = True
                self._media = None
                return

            path = get_source_file_path(self._filepath_raw)
            try:
                abspath = bpy.path.abspath(path)
                with open(abspath, "rb") as data_file:
                    data = data_file.read()
            except Exception as e:
                logger.error(f"Error while loading {abspath!r} ...")
                logger.error(f"... for {datablock!r}. Check shared folders ...")
                logger.error(f"... {e!r}")
                self._media = None
            else:
                self._media = (path, data)

    @property
    def collection_name(self) -> str:
        """
        The name of the bpy.data collection that contains the proxified datablock, empty string if the
        proxified datablock is embedded
        """
        return self._bpy_data_collection

    @property
    def collection(self) -> T.bpy_prop_collection:
        return getattr(bpy.data, self.collection_name)

    def target(self, context: Context) -> T.ID:
        """Returns the datablock proxified by this proxy"""
        return context.proxy_state.datablock(self.mixer_uuid)

    def create_standalone_datablock(self, context: Context) -> Tuple[Optional[T.ID], Optional[RenameChangeset]]:
        """
        Save this proxy into its target standalone datablock
        """
        if self.target(context):
            logger.warning(f"create_standalone_datablock: datablock already registered : {self}")
            logger.warning("... update ignored")
            return None, None
        renames: RenameChangeset = []
        incoming_name = self.data("name")

        # Detect a conflicting creation by look for a datablock with the wanted name.
        # Ignore the duplicate name if it is from a library, it is not a name clash
        existing_datablock = self.collection.get(incoming_name)
        if existing_datablock and not existing_datablock.library:
            # The correct assertion is
            #   assert existing_datablock.mixer_uuid
            # but it causes a visible failure in a test that formerly failed silently (internal issue #396)
            if not existing_datablock.mixer_uuid:
                # A datablock created by VRtist command in the same command batch
                # Not an error, we will make it ours by adding the uuid and registering it

                # TODO this branch should be obsolete as VRtist commands are no more processed in generic mode
                logger.info(f"create_standalone_datablock for {self} found existing datablock from VRtist")
                datablock = existing_datablock
            else:
                if existing_datablock.mixer_uuid != self.mixer_uuid:
                    # TODO LIB

                    # local has a datablock with the same name as remote wants to create, but a different uuid.
                    # It is a simultaneous creation : rename local's datablock. Remote will do the same thing on its side
                    # and we will end up will all renamed datablocks
                    unique_name = f"{existing_datablock.name}_{existing_datablock.mixer_uuid}"
                    logger.warning(
                        f"create_standalone_datablock: Creation name conflict. Renamed existing bpy.data.{self.collection_name}[{existing_datablock.name}] into {unique_name}"
                    )

                    # Rename local's and issue a rename command
                    renames.append(
                        (
                            existing_datablock.mixer_uuid,
                            existing_datablock.name,
                            unique_name,
                            f"Conflict bpy.data.{self.collection_name}[{self.data('name')}] into {unique_name}",
                        )
                    )
                    existing_datablock.name = unique_name

                    datablock = specifics.bpy_data_ctor(self.collection_name, self, context)
                else:
                    # a creation for a datablock that we already have. This should not happen
                    logger.error(f"create_standalone_datablock: unregistered uuid for {self}")
                    logger.error("... update ignored")
                    return None, None
        else:
            try:
                datablock = specifics.bpy_data_ctor(self.collection_name, self, context)
            except ExternalFileFailed:
                return None, None

        if datablock is None:
            if self.collection_name != "shape_keys":
                logger.warning(f"Cannot create bpy.data.{self.collection_name}[{self.data('name')}]")
            return None, None

        if DEBUG:
            # TODO LIB
            # Detect a failure to avoid spontaneous renames ??
            name = self.data("name")
            if self.collection.get(name).name != datablock.name:
                logger.error(f"Name mismatch after creation of bpy.data.{self.collection_name}[{name}] ")

        self._has_datablock = True
        uuid = self.mixer_uuid
        datablock.mixer_uuid = uuid
        context.proxy_state.add_datablock(uuid, datablock)
        if isinstance(datablock, T.Object):
            context.proxy_state.register_object(datablock)

        datablock = self._save(datablock, context)
        return datablock, renames

    def _save(self, datablock: T.ID, context: Context) -> T.ID:
        datablock = self._pre_save(datablock, context)
        if datablock is None:
            logger.warning(f"DatablockProxy.update_standalone_datablock() {self} pre_save returns None")
            return None, None

        with context.visit_state.enter_datablock(self, datablock):
            for k, v in self._data.items():
                write_attribute(datablock, k, v, context)

        self._custom_properties.save(datablock)
        return datablock

    def update_standalone_datablock(self, datablock: T.ID, delta: Delta, context: Context) -> T.ID:
        """
        Update this proxy and datablock according to delta
        """
        datablock = delta.value._pre_save(datablock, context)
        if datablock is None:
            logger.warning(f"DatablockProxy.update_standalone_datablock() {self} pre_save returns None")
            return None

        with context.visit_state.enter_datablock(self, datablock):
            self.apply(datablock, self.collection, datablock.name, delta, context)

        return datablock

    def save(self, attribute: T.ID, unused_parent: T.bpy_struct, unused_key: Union[int, str], context: Context) -> T.ID:
        """
        Save this proxy into an embedded datablock

        Args:
            attribute: the datablock into which this proxy is saved
            unused_parent: the struct that contains the embedded datablock (e.g. a Scene)
            unused_key: the member name of the datablock in parent (e.g. node_tree)
            context: proxy and visit state

        Returns:
            The saved datablock
        """

        datablock = self._pre_save(attribute, context)
        if datablock is None:
            logger.error(f"DatablockProxy.save() get None after _pre_save({attribute})")
            return None

        with context.visit_state.enter_datablock(self, datablock):
            for k, v in self._data.items():
                write_attribute(datablock, k, v, context)

        return datablock

    def apply(
        self,
        attribute: T.ID,
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender: bool = True,
    ) -> StructProxy:
        """
        Apply delta to this proxy and optionally to the Blender attribute its manages.

        Args:
            attribute: the struct to update (e.g. a Material instance)
            parent: the attribute that contains attribute (e.g. bpy.data.materials)
            key: the key that identifies attribute in parent (e.g "Material")
            delta: the delta to apply
            context: proxy and visit state
            to_blender: update the managed Blender attribute in addition to this Proxy
        """
        custom_properties_update = delta.value._custom_properties
        if custom_properties_update is not None:
            self._custom_properties = custom_properties_update
            if to_blender:
                custom_properties_update.save(attribute)

        return super().apply(attribute, parent, key, delta, context, to_blender)

    def apply_to_proxy(
        self,
        attribute: T.ID,
        delta: DeltaUpdate,
        context: Context,
    ):
        """
        Apply delta to this proxy without updating the value of the Blender attribute it manages.

        This method is used in the depsgraph update callback, after the Blender attribute value has been updated by
        the user.

        Args:
            attribute: the datablock that is managed by this proxy
            delta: the delta to apply
            context: proxy and visit state
        """
        collection = getattr(bpy.data, self.collection_name)
        self.apply(attribute, collection, attribute.name, delta, context, to_blender=False)

    def update_soa(self, bl_item, path: Path, soa_members: List[SoaMember]) -> bool:

        r = self.find_by_path(bl_item, path)
        if r is None:
            logger.error(f"update_soa: {path} not found in {bl_item}")
            return False
        container, container_proxy = r
        for soa_member in soa_members:
            soa_proxy = container_proxy.data(soa_member[0])
            soa_proxy.save_array(container, soa_member[0], soa_member[1])

        # HACK force updates : unsure what is strictly required
        # specifying refresh is not compatible with Grease Pencil and causes a crash
        update = False
        if isinstance(bl_item, T.Mesh):
            bl_item.update()
        elif isinstance(bl_item, T.Curve):
            bl_item.twist_mode = bl_item.twist_mode
        elif isinstance(bl_item, T.GreasePencil):
            bl_item.update_tag()
            update = True

        return update

    def diff(self, attribute: T.ID, key: Union[int, str], prop: T.Property, context: Context) -> Optional[Delta]:
        """
        Computes the difference between the state of an item tracked by this proxy and its Blender state.

        Args:
            attribute: the datablock to update (e.g. a Material instance)
            key: the key that identifies attribute in parent (e.g "Material")
            prop: the Property of struct as found in its enclosing object
            context: proxy and visit state
        """

        # Create a proxy that will be populated with attributes differences.
        diff = DatablockProxy.make(attribute)

        with context.visit_state.enter_datablock(diff, attribute):
            delta = self._diff(attribute, key, prop, context, diff)

        # compute the custom properties update
        if not isinstance(delta, DeltaReplace):
            custom_properties_update = self._custom_properties.diff(attribute)
            if custom_properties_update is not None:
                if delta is None:
                    # regular diff had found no delta: create one
                    delta = DeltaUpdate(diff)
                diff._custom_properties = custom_properties_update

        return delta

    def _pre_save(self, target: T.bpy_struct, context: Context) -> T.ID:
        return specifics.pre_save_datablock(self, target, context)
