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
Utility proxy classes

See synchronization.md
"""
from __future__ import annotations

import logging
from typing import Any, Optional, TYPE_CHECKING, List, Set, Tuple, Union

import bpy.types as T  # noqa

from mixer.blender_data.attributes import read_attribute, write_attribute
from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy
from mixer.blender_data.json_codec import serialize
from mixer.blender_data.proxy import Delta, DeltaReplace, DeltaUpdate, Proxy

if TYPE_CHECKING:
    from mixer.blender_data.proxy import Context
    from mixer.blender_data.struct_collection_proxy import StructCollectionProxy

logger = logging.getLogger(__name__)


@serialize
class NonePtrProxy(Proxy):
    """Proxy for a None PointerProperty value.

    This is used for pointers to standalone datablocks (e.g. Scene.camera, AnimData.action), pointers to embedded
    datablocks (e.g. Scene.node_tree) and pointers to other structs (Scene.sequence_editor). A null DatablockRefProxy
    can also be used for null pointers to standalone datablocks if its is clearly known that the target is a
    standalone reference. Usually, this is not known in readçattribute and a NonePtrProxy is created.

    Note: when setting a PointerProperty from None to a valid reference, apply_attributs requires that the managed
    value implements apply().
    """

    def __bool__(self):
        return False

    def target(self, context: Context) -> None:
        return None

    @property
    def mixer_uuid(self) -> str:
        return "00000000-0000-0000-0000-000000000000"

    def load(self, *_):
        return self

    def save(self, unused_attribute, parent: T.bpy_struct, key: Union[int, str], context: Context):
        """Save None into parent.key or parent[key]"""

        if isinstance(key, int):
            parent[key] = None
        else:
            try:
                setattr(parent, key, None)
            except AttributeError as e:
                # Motsly errors like
                #   AttributeError: bpy_struct: attribute "node_tree" from "Material" is read-only
                # Avoiding them would require filtering attributes on save in order not to set
                # Material.node_tree if Material.use_nodes is False
                if logger.isEnabledFor(logging.DEBUG):
                    logger.debug("NonePtrProxy.save(): exception for attribute ...")
                    logger.debug(f"... {context.visit_state.display_path()}.{key}...")
                    logger.debug(f"... {e!r}")

    def apply(
        self,
        attribute: Union[T.bpy_struct, T.bpy_prop_collection],
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender: bool = True,
    ) -> Union[DatablockRefProxy, NonePtrProxy]:
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
        replace = delta.value

        if to_blender:
            if isinstance(replace, DatablockRefProxy):
                datablock = context.proxy_state.datablock(replace._datablock_uuid)
                if isinstance(key, int):
                    parent[key] = datablock
                else:
                    setattr(parent, key, datablock)

            else:
                # This branch is taken when animation_data or node_tree instance animation_data are set from None to a
                # valid value, after animation_data_create() has been called or use_nodes is set to True
                write_attribute(parent, key, replace, context)

        return replace

    def diff(
        self,
        container: Union[T.bpy_prop_collection, T.Struct],
        key: Union[str, int],
        prop: T.Property,
        context: Context,
    ) -> Optional[Delta]:
        attr = read_attribute(container, key, prop, None, context)
        if isinstance(attr, NonePtrProxy):
            return None
        return DeltaReplace(attr)


@serialize
class SetProxy(Proxy):
    """Proxy for sets of primitive types

    Found in DecimateModifier.delimit
    """

    _serialize = ("_items",)

    def __init__(self):
        self._items: List[Any] = []

    @property
    def items(self):
        return self._items

    @items.setter
    def items(self, value):
        self._items = list(value)
        self._items.sort()

    def load(self, attribute: Set[Any]) -> SetProxy:
        """
        Load the attribute Blender struct into this proxy

        Args:
            attribute: the Blender set to load into this proxy
        """
        self.items = attribute
        return self

    def save(
        self,
        attribute: Set[Any],
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: Union[int, str],
        context: Context,
    ):
        """Save this proxy into attribute, which is contained in parent[key] or parent.key

        Args:
            attribute: the attribute into which the proxy is saved.
            parent: the attribute that contains attribute
            key: the string or index that identifies attribute in parent
        """
        try:
            if isinstance(key, int):
                parent[key] = set(self.items)
            else:
                setattr(parent, key, set(self.items))
        except Exception as e:
            logger.error("SetProxy.save(): exception for attribute ...")
            logger.error(f"... {context.visit_state.display_path()}.{key}...")
            logger.error(f"... {e!r}")

    def apply(
        self,
        attribute: Any,
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender: bool = True,
    ) -> Proxy:
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
        assert isinstance(delta, DeltaReplace)
        self.items = delta.value.items
        if to_blender:
            self.save(attribute, parent, key, context)
        return self

    def diff(
        self, attribute: Set[Any], unused_key: Union[int, str], unused_prop: T.Property, unused_context: Context
    ) -> Optional[Delta]:
        """
        Computes the difference between the state of an item tracked by this proxy and its Blender state.

        Args:
            attribute: the set to update (e.g. a the "delimit" attribute of a DecimateModifier instance)
            unused_key: the key that identifies attribute in parent (e.g "delimit")
            unused_prop: the Property of attribute as found in its parent attribute
            unused_context: proxy and visit state
        """
        if set(self.items) == attribute:
            return None

        new_set = SetProxy()
        new_set.items = attribute
        return DeltaReplace(new_set)


@serialize
class CustomPropertiesProxy:
    """Proxy-like for datablock Custom Properties"""

    _serialize = ("_dict", "_rna_ui")

    def __init__(self):
        self._dict = {}
        """Custom properties and their values"""

        self._rna_ui = {}
        """_RNA_UI dictionnary"""

    def _user_keys(self, datablock: T.ID):
        keys = set(datablock.keys())
        rna_ui = datablock.get("_RNA_UI", None)
        keys -= {"_RNA_UI"}
        keys -= set(datablock.bl_rna.properties.keys())
        return keys, rna_ui

    def load(self, datablock: T.ID):
        """Load the custom properties of datablock, skipping API defined properties"""
        keys, rna_ui = self._user_keys(datablock)
        # This only load custom properties with a UI
        if rna_ui is None:
            self._dict.clear()
            self._rna_ui.clear()
            return self

        self._rna_ui = rna_ui.to_dict()
        self._dict = {name: datablock.get(name) for name in keys}

    def save(self, datablock: T.ID):
        """Overwrite all the custom properties in datablock, including the UI"""
        if self._rna_ui:
            datablock["_RNA_UI"] = self._rna_ui
        else:
            try:
                del datablock["_RNA_UI"]
            except KeyError:
                pass

        current_keys, _ = self._user_keys(datablock)
        remove = current_keys - set(self._dict.keys())
        for key in remove:
            del datablock[key]
        for key, value in self._dict.items():
            datablock[key] = value

    def diff(self, datablock: T.ID) -> Optional[CustomPropertiesProxy]:
        current = CustomPropertiesProxy()
        current.load(datablock)
        if self._dict == current._dict and self._rna_ui == current._rna_ui:
            return None

        return current

    def apply(self, datablock: T.ID, update: Optional[CustomPropertiesProxy], to_blender: bool):
        if update is None:
            return

        self._rna_ui = update._rna_ui
        self._dict = update._dict
        if to_blender:
            self.save(datablock)


@serialize
class PtrToCollectionItemProxy(Proxy):
    """Proxy for an attribute that contains a pointer into a bpy_prop_collection in the same embeddded datablock.

    For instance, ShapeKey.relative_key is a pointer to a Key.key_blocks element.
    """

    _serialize = ("_path", "_index")

    _ctors = {
        (T.ShapeKey, "relative_key"): ("key_blocks",),
        (T.FCurve, "group"): ("groups",),
        (T.EditBone, "parent"): ("edit_bones",),
        (T.EditBone, "bbone_custom_handle_start"): ("edit_bones",),
        (T.EditBone, "bbone_custom_handle_end"): ("edit_bones",),
        (T.PoseBone, "bone_group"): ("pose", "bone_groups"),
    }
    """{ struct member: path to the enclosing datablock collection}.

    For instance PoseBone.bone_group is a pointer to a pose.bone_groups element in the same datablock.
    """

    @classmethod
    def make(cls, attr_type: type, key: str) -> Optional[PtrToCollectionItemProxy]:
        try:
            collection_path = cls._ctors[(attr_type, key)]
        except KeyError:
            return None
        return cls(collection_path)

    def __init__(self, path: Tuple[Union[int, str], ...] = ()):
        self._path = path
        """Path of the collection that contains the pointed to item in the enclosing standalone datablock."""

        self._index: int = -1
        """Index in the collection identified by _path, -1 if ot present"""

    def __bool__(self):
        return self._index != -1

    def __eq__(self, other):
        return (self._path, self._index) == (other._path, other._index)

    def _collection(self, datablock: T.ID) -> T.bpy_prop_collection:
        """Returns the bpy_prop_collection that contains the pointees referenced by the attribute managed by this proxy
        (e.g. returns Key.key_blocks, if this proxy manages Skape_key.relative_key)."""
        collection = datablock
        for item in self._path:
            if isinstance(item, str):
                collection = getattr(collection, item)
            else:
                collection = collection[item]

        return collection

    def _collection_proxy(self, datablock: T.ID, context: Context) -> StructCollectionProxy:
        """Returns the StructCollectionProxy that is expected to contain the pointee."""
        datablock_proxy = context.proxy_state.proxies[datablock.mixer_uuid]
        collection_proxy = datablock_proxy.data(self._path)
        return collection_proxy

    def _compute_index(self, attribute: T.bpy_struct):
        """Returns the index in the pointee bpy_prop_collection (e.g Key.key_blocks) that contains the item referenced
        by the attribute managed by this proxy (e.g ShapeKey.relative_key)."""
        if attribute is None:
            return -1
        collection = self._collection(attribute.id_data)
        for index, item in enumerate(collection):
            if item == attribute:
                return index
        return -1

    def load(self, attribute: T.bpy_struct) -> PtrToCollectionItemProxy:
        """
        Load the pointer member (e.g relative_key) of the attribute managed by this proxy (e.g. a ShapeKey in
        Key.key_blocks).

        Args:
            attribute: the struct that contains the pointer
        """
        self._index = self._compute_index(attribute)
        return self

    def save(
        self,
        attribute: T.bpy_struct,
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: Union[int, str],
        context: Context,
    ):
        """Save this proxy into attribute, which is contained in parent[key] or parent.key

        Args:
            attribute: the attribute into which the proxy is saved.
            parent: the attribute that contains attribute
            key: the string or index that identifies attribute in parent
            context: proxy and visit state
        """

        if self._index == -1:
            pointee = None
        else:
            collection = self._collection(parent.id_data)
            try:
                pointee = collection[self._index]
            except IndexError:
                collection_proxy = self._collection_proxy(parent.id_data, context)
                collection_proxy.register_unresolved(
                    self._index, lambda: write_attribute(parent, key, collection[self._index], context)
                )

                # TODO Fails if an array member references an element not yet created, like bones with parenting reversed
                # Could be solved with a delayed reference resolution:
                # - keep a reference to the collection proxy
                # - store the assignment closure in the collection proxy
                # - when the collection proxy creates the item, call the closure
                logger.error("save(): Unimplemented: reference an item not yet created ...")
                logger.error(f"... {parent!r}.{key}")
                logger.error(f"... references {collection!r}[{self._index}]")
            else:
                write_attribute(parent, key, pointee, context)

    def apply(
        self,
        attribute: Any,
        parent: Union[T.bpy_struct, T.bpy_prop_collection],
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender: bool = True,
    ) -> Proxy:
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
        self._index = delta.value._index
        if to_blender:
            self.save(attribute, parent, key, context)
        return self

    def diff(
        self, attribute: T.bpy_struct, unused_key: str, unused_prop: T.Property, unused_context: Context
    ) -> Optional[Delta]:
        """
        Computes the difference between the state of an item tracked by this proxy and its Blender state.

        Args:
            attribute: the attribute (e.g a ShapeKey) that contains the member managed by the proxy  (e.g. relative_key)
            unused_key: the name of the attribute member that is managed by this proxy (e.g. relative_key)
            unused_prop: the Property of attribute as found in its parent attribute
            unused_context: proxy and visit state
        """
        index = self._compute_index(attribute)
        if index == self._index:
            return None

        update = PtrToCollectionItemProxy()
        update._index = index
        return DeltaUpdate(update)
