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
Proxy of a bpy.types.Struct collection, excluding bpy.types.ID collections that are implemented
in datablock_collection_proxy.py

See synchronization.md
"""
from __future__ import annotations

from collections import defaultdict
import logging
from typing import Callable, Dict, List, Optional, Tuple, TYPE_CHECKING, TypeVar, Union

import bpy.types as T  # noqa

from mixer.blender_data import specifics
from mixer.blender_data.attributes import apply_attribute, diff_attribute, read_attribute, write_attribute
from mixer.blender_data.json_codec import serialize
from mixer.blender_data.proxy import AddElementFailed, Delta, DeltaAddition, DeltaReplace, DeltaUpdate, Proxy
from mixer.blender_data.struct_proxy import StructProxy

if TYPE_CHECKING:
    from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy
    from mixer.blender_data.misc_proxies import NonePtrProxy
    from mixer.blender_data.proxy import Context

logger = logging.getLogger(__name__)


def _proxy_factory(attr) -> Union[DatablockRefProxy, NonePtrProxy, StructProxy]:
    if isinstance(attr, T.ID) and not attr.is_embedded_data:
        from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy

        return DatablockRefProxy()
    elif attr is None:
        from mixer.blender_data.misc_proxies import NonePtrProxy

        return NonePtrProxy()
    else:
        return StructProxy.make(attr)


T_ = TypeVar("T_")


class Resolver:
    """Helper to defer item reference resolution after the referenced item creation.

    An array element may reference an item with a larger index. As the arrays are created depth wise, the item
    with the larger index does not exist when the item with the smaller item stored a reference. This situation
    occurs when bone parenting is reversed.

    TODO use this class for DatablockRefCollectionProxy as well
    """

    def __init__(self):
        self._items: Dict[T_, List[Callable[[], None]]] = defaultdict(list)

    def __bool__(self):
        return bool(self._items)

    def append(self, key: T_, func: Callable[[], None]):
        """Add func() to be called by resolve() for item at key"""
        self._items[key].append(func)

    def resolve(self, key: T_):
        """Resolve the references to item identified by key by calling the closures registered for it."""
        try:
            funcs = self._items.pop(key)
        except IndexError:
            return
        for f in funcs:
            f()


@serialize
class StructCollectionProxy(Proxy):
    """
    Proxy to a bpy_prop_collection of non-datablock Struct.

    It can track an array (int keys) or a dictionnary(string keys). Both implementation are
    in the same class as it is not possible to know at creation time the type of an empty collection
    """

    _serialize = ("_sequence", "_diff_additions", "_diff_deletions", "_diff_updates")

    def __init__(self):
        self._diff_updates: List[Tuple[int, Delta]] = []
        self._diff_deletions: int = 0
        self._diff_additions: List[DeltaAddition] = []
        self._sequence: List[Proxy] = []
        self._resolver: Optional[Resolver] = None

    @classmethod
    def make(cls, attr_property: T.Property):
        if attr_property.srna == T.NodeLinks.bl_rna:
            from mixer.blender_data.node_proxy import NodeLinksProxy

            return NodeLinksProxy()
        return StructCollectionProxy()

    def __len__(self):
        return len(self._sequence)

    def __iter__(self):
        return iter(self._sequence)

    def __getitem__(self, i: int):
        return self._sequence[i]

    @property
    def length(self) -> int:
        return len(self._sequence)

    def register_unresolved(self, i: int, func: Callable[[], None]):
        if self._resolver is None:
            self._resolver = Resolver()
        self._resolver.append(i, func)

    def data(self, key: int, resolve_delta=True) -> Optional[Union[Delta, Proxy]]:
        """Return the data at key, which may be a struct member, a dict value or an array value,

        Args:
            key: Integer or string to be used as index or key to the data
            resolve_delta: If True, and the data is a Delta, will return the delta value
        """

        # shaky and maybe not useful
        length = self.length
        if key < length:
            delta_update = next((delta for i, delta in self._diff_updates if i == key), None)
            if delta_update is None:
                return self._sequence[key]
            if resolve_delta:
                return delta_update.value
            return delta_update
        else:
            try:
                delta_addition = self._diff_additions[key - length]
            except IndexError:
                return None
            if resolve_delta:
                return delta_addition.value
            return delta_addition

    def load(
        self,
        bl_collection: T.bpy_prop_collection,
        context: Context,
    ):
        self._sequence.clear()
        for i, v in enumerate(bl_collection.values()):
            context.visit_state.push(v, i)
            try:
                self._sequence.append(_proxy_factory(v).load(v, context))
            except Exception as e:
                logger.error(f"Exception during load at {context.visit_state.display_path()} ...")
                logger.error(f"... {e!r}")
            finally:
                context.visit_state.pop()
        return self

    def save(self, collection: T.bpy_prop_collection, parent: T.bpy_struct, key: str, context: Context):
        """
        Save this proxy into collection

        Args:
            collection: the collection into which this proxy is saved
            parent: the attribute that contains collection (e.g. a Scene instance)
            key: the name of the collection in parent (e.g "background_images")
            context: the proxy and visit state
        """
        sequence = self._sequence

        # Using clear_from ensures that sequence data is compatible with remaining elements after
        # truncate_collection. This addresses an issue with Nodes, for which the order of default nodes (material
        # output and principled in collection) may not match the order of incoming nodes. Saving node data into a
        # node of the wrong type can lead to a crash.
        clear_from = specifics.clear_from(collection, sequence, context)
        specifics.truncate_collection(collection, clear_from)

        # For collections like `IDMaterials`, the creation API (`.new(datablock_ref)`) also writes the value.
        # For collections like `Nodes`, the creation API (`.new(name)`) does not write the item value.
        # So the value must always be written for all collection types.
        collection_length = len(collection)
        for i, item_proxy in enumerate(sequence[:collection_length]):
            write_attribute(collection, i, item_proxy, context)
        for i, item_proxy in enumerate(sequence[collection_length:], collection_length):
            try:
                specifics.add_element(collection, item_proxy, i, context)
                if self._resolver:
                    self._resolver.resolve(i)
            except AddElementFailed:
                break
            # Must write at once, otherwise the default item name might conflit with a later item name
            write_attribute(collection, i, item_proxy, context)

    def apply(
        self,
        collection: T.bpy_prop_collection,
        parent: T.bpy_struct,
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender=True,
    ) -> StructCollectionProxy:
        """
        Apply delta to this proxy and optionally to the Blender attribute its manages.

        Args:
            attribute: the collection to update (e.g. a_mesh.material)
            parent: the attribute that contains attribute (e.g. a a Mesh instance)
            key: the key that identifies attribute in parent (e.g "materials")
            delta: the delta to apply
            context: proxy and visit state
            to_blender: update the managed Blender attribute in addition to this Proxy
        """
        assert isinstance(key, str)

        update = delta.value
        assert type(update) == type(self)

        if isinstance(delta, DeltaReplace):
            # The collection must be replaced as a whole
            self._sequence = update._sequence
            if to_blender:
                specifics.truncate_collection(collection, 0)
                self.save(collection, parent, key, context)
        else:
            # a sparse update
            try:
                sequence = self._sequence

                # Delete before update and process updates in reverse order to avoid spurious renames.
                # Starting with sequence A, B, C, D and delete B causes :
                # - an update for items 1 and 2 to be renamed into C and D
                # - one delete
                # If the update is processed first, Blender renames item 3 into D.001
                # If the deletes are processed first but the updates are processed in order, Blender renames item 1
                # into C.001

                delete_count = update._diff_deletions
                if delete_count > 0:
                    if to_blender:
                        specifics.truncate_collection(collection, len(collection) - delete_count)
                    del sequence[-delete_count:]

                for i, delta_update in reversed(update._diff_updates):
                    sequence[i] = apply_attribute(collection, i, sequence[i], delta_update, context, to_blender)

                for i, delta_addition in enumerate(update._diff_additions, len(sequence)):
                    if to_blender:
                        item_proxy = delta_addition.value
                        try:
                            specifics.add_element(collection, item_proxy, i, context)
                            if self._resolver:
                                self._resolver.resolve(i)
                        except AddElementFailed:
                            break
                        write_attribute(collection, i, item_proxy, context)
                    sequence.append(delta_addition.value)

            except Exception as e:
                logger.warning("apply: Exception while processing attribute ...")
                logger.warning(f"... {context.visit_state.display_path()}.{key}")
                logger.warning(f"... {e!r}")

        return self

    def diff(
        self, collection: T.bpy_prop_collection, key: Union[int, str], collection_property: T.Property, context: Context
    ) -> Optional[Union[DeltaUpdate, DeltaReplace]]:
        """
        Computes the difference between the state of an item tracked by this proxy and its Blender state.

        This proxy tracks a collection of items indexed by string (e.g Scene.render.views) or int.
        The result will be a ProxyDiff that contains a Delta item per added, deleted or updated item

        Args:
            collection; the collection that must be diffed agains this proxy
            key: the name of the collection, to record in the visit path
            collection_property; the property os collection as found in its enclosing object
        """
        sequence = self._sequence
        if len(sequence) == 0 and len(collection) == 0:
            return None

        if specifics.diff_must_replace(collection, sequence, collection_property):
            # A collection cannot be updated because either:
            # - some of its members cannot be updated :
            #   SplineBezierPoints has no API to remove points, so Curve.splines cannot be update and must be replaced
            # - updating the name of members will cause unsolicited renames.
            #   When swapping layers A and B in a GreasePencilLayers, renaming layer 0 into B cause an unsolicited
            #   rename of layer 0 into B.001
            # Send a replacement for the whole collection
            self.load(collection, context)
            return DeltaReplace(self)
        else:
            item_property = collection_property.fixed_type
            diff = self.__class__()

            # items from clear_from index cannot be updated, most often because eir type has changed (e.g
            # ObjectModifier)
            clear_from = specifics.clear_from(collection, sequence, context)

            # run a diff for the head, that can be updated in-place
            for i in range(clear_from):
                delta = diff_attribute(collection[i], i, item_property, sequence[i], context)
                if delta is not None:
                    diff._diff_updates.append((i, delta))

            if specifics.can_resize(collection, context):
                # delete the existing tail that cannot be modified
                diff._diff_deletions = len(sequence) - clear_from

                # add the new tail
                for i, item in enumerate(collection[clear_from:], clear_from):
                    value = read_attribute(item, i, item_property, collection, context)
                    diff._diff_additions.append(DeltaAddition(value))

            if diff._diff_updates or diff._diff_deletions or diff._diff_additions:
                return DeltaUpdate(diff)

        return None
