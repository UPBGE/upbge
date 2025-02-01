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
Base class and utilities for the proxy system.

See synchronization.md
"""
from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass
import logging
from typing import Any, Callable, Dict, List, Optional, Iterable, Tuple, TYPE_CHECKING, Union
from uuid import uuid4

import bpy
import bpy.types as T  # noqa

from mixer.blender_data.json_codec import serialize

if TYPE_CHECKING:
    from mixer.blender_data.bpy_data_proxy import Context
    from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy
    from mixer.blender_data.types import Path

logger = logging.getLogger(__name__)

Uuid = str


@dataclass
class UnresolvedRef:
    """A datablock reference that could not be resolved when target.init() needed to be called
    because the referenced datablock was not yet received.

    No suitable ordering can easily be provided by the sender for many reasons including Collection.children
    referencing other collections and Scene.sequencer strips that can reference other scenes
    """

    target: T.bpy_prop_collection
    proxy: DatablockRefProxy


class UnresolvedRefs:
    """For each each Struct ot collection of datablocks, a list or unresolved references
    Datablock referenced may be temporarily unresolved when referenced datablocks
    (e.g. a bpy.types.Collection) are received before the item that references it
    (e.g. bpy.types.Collection.children)
    """

    def __init__(self):
        self._refs: Dict[Uuid, List[Tuple[Callable[[T.ID], None], str]]] = defaultdict(list)

    def __bool__(self):
        return bool(self._refs)

    def append(self, dst_uuid: Uuid, src_link: Callable[[T.ID], None], display_string: str = ""):
        self._refs[dst_uuid].append((src_link, display_string))

    def resolve(self, dst_uuid: Uuid, dst_datablock: T.ID):
        if dst_uuid in self._refs:
            for src_link, display_string in self._refs[dst_uuid]:
                src_link(dst_datablock)
                logger.info(f"resolving reference to {dst_datablock!r} {dst_uuid}: {display_string}")
            del self._refs[dst_uuid]


class MaxDepthExceeded(Exception):
    """Thrown when attribute depth is too large"""

    pass


@serialize
class Delta:
    """
    A Delta records the difference between the proxy state and Blender state.

    It is created with Proxy.diff() and applied with Proxy.apply()

    TODO this was added to emphasize the existence of deltas when differential updates were implemented
    but is not strictly required. As an alternative, Deltas could be implemented as  hollow Proxy
    with and addition flag indicating that it should be processed as a set(ordinary proxy), or a
    add, remove or update operation
    """

    _serialize = ("value",)

    def __init__(self, value: Any):
        self.value = value

    def __str__(self):
        return f"<{self.__class__.__name__}({self.value})>"


@serialize(ctor_args=("value",))
class DeltaAddition(Delta):
    pass


@serialize(ctor_args=("value",))
class DeltaDeletion(Delta):
    pass


@serialize(ctor_args=("value",))
class DeltaUpdate(Delta):
    pass


@serialize(ctor_args=("value",))
class DeltaReplace(Delta):
    pass


class MixerException(Exception):
    """Base class for Mixer specific exceptions."""

    pass


class ExternalFileFailed(MixerException):
    """Datablock creation failed because of a file related issue"""

    pass


class AddElementFailed(MixerException):
    """Creation of an element into a bpy_prop_collection failed"""

    pass


class Proxy:
    """
    Base class for all proxies.
    """

    def __eq__(self, other):
        if self.__class__ is not other.__class__:
            return False
        if len(self._data) != len(other._data):
            return False

        # TODO test same keys
        # TODO test _bpy_collection
        for k, v in self._data.items():
            if k not in other._data.keys():
                return False
            if v != other._data[k]:
                return False
        return True

    def __contains__(self, value):
        return value in self._data

    def init(self, _):
        pass

    def data(self, key_or_path: Union[int, str, Iterable[Union[int, str]]], resolve_delta=True) -> Any:
        """Return the item identified by key_or_path in this proxy hierarchy.

        Args:
            key_or_path: Integer or string or sequence of integer or string to be used as index or key to the data
            resolve_delta: If True, and the data is a Delta, will return the delta value
        """

        if isinstance(key_or_path, (int, str)):
            key_or_path = (key_or_path,)

        data = self
        for item in key_or_path:
            try:
                data = data[item]
            except IndexError:
                return None
            except TypeError:
                try:
                    data = data._data[item]
                except KeyError:
                    return None

            if isinstance(data, Delta) and resolve_delta:
                data = data.value

        return data

    def save(
        self, attribute: Any, parent: Union[T.bpy_struct, T.bpy_prop_collection], key: Union[int, str], context: Context
    ):
        """Save this proxy into attribute, which is contained in parent[key] or parent.key

        The attribute parameter is mainly needed to have a uniform API while ensuring that for any datablock,
        a bpy.data collection is never searched by name, which would fail with libraries.

        Args:
            attribute: the attribute into which the proxy is saved.
            parent: the attribute that contains attribute
            key, the string or index that identifies attribute in parent
        """
        raise NotImplementedError(f"Proxy.save() for {parent}[{key}]")

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

        TODO The parameters parent and key should not be required
        Args:
            attribute: the Blender attribute to update
            parent: the attribute that contains attribute
            key: the key that identifies attribute in parent
            delta: the delta to apply
            context: proxy and visit state
            to_blender: update the managed Blender attribute in addition to this Proxy
        """
        raise NotImplementedError(f"Proxy.apply() for {parent}[{key}]")

    def diff(
        self,
        container: Union[T.bpy_prop_collection, T.Struct],
        key: Union[str, int],
        prop: T.Property,
        context: Context,
    ) -> Optional[Delta]:
        raise NotImplementedError(f"diff for {container}[{key}]")

    def find_by_path(
        self, bl_item: Union[T.bpy_struct, T.bpy_prop_collection], path: Path
    ) -> Optional[Tuple[Union[T.bpy_struct, T.bpy_prop_collection], Proxy]]:
        head, *tail = path
        if isinstance(bl_item, T.bpy_struct):
            bl = getattr(bl_item, head)  # type: ignore
        elif isinstance(bl_item, T.bpy_prop_collection):
            try:
                bl = bl_item[head]
            except (KeyError, IndexError):
                return None
        else:
            return None

        proxy = self.data(head)
        if proxy is None:
            logger.warning(f"find_by_path: No proxy for {bl_item} {path}")
            return None

        if not tail:
            return bl, proxy

        return proxy.find_by_path(bl, tail)


def ensure_uuid(item: bpy.types.ID) -> str:
    """Ensures that the item datablock has a mixer_uuid property"""
    uuid = item.get("mixer_uuid")
    if not uuid:
        uuid = str(uuid4())
        item.mixer_uuid = uuid
    return uuid
