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
Encoding end decoding of BLENDER_DATA messages used by the full Blender protocol.

This module addresses the lower level encoding/decoding (from/to the wire), using encode_*() and decode_*()
shared with the VRtist protocol.

"""
from __future__ import annotations

import json
import logging
import traceback
from typing import List, Optional, Tuple, TYPE_CHECKING

from mixer.blender_data.types import ArrayGroup, ArrayGroups, Path, Soa

from mixer.broadcaster.common import (
    decode_int,
    decode_py_array,
    decode_string,
    decode_string_array,
    encode_int,
    encode_py_array,
    encode_string,
    encode_string_array,
)

if TYPE_CHECKING:
    from mixer.blender_data.datablock_proxy import DatablockProxy

logger = logging.getLogger(__name__)


def soa_buffers(datablock_proxy: Optional[DatablockProxy]) -> List[bytes]:
    if datablock_proxy is None:
        # empty update, should not happen
        return [encode_int(0)]

    # Layout is
    #   number of AosProxy: 2
    #       soa path in datablock : ("vertices")
    #       number of SoaElement : 2
    #           element name: "co"
    #           array
    #           element name: "normals"
    #           array
    #       soa path in datablock : ("edges")
    #       number of SoaElement : 1
    #           element name: "vertices"
    #           array

    items: List[bytes] = []
    items.append(encode_int(len(datablock_proxy._soas)))
    for path, soa_proxies in datablock_proxy._soas.items():
        path_string = json.dumps(path)
        items.append(encode_string(path_string))
        items.append(encode_int(len(soa_proxies)))
        for element_name, soa_element in soa_proxies:
            if soa_element._array is not None:
                items.append(encode_string(element_name))
                items.append(encode_py_array(soa_element._array))
    return items


_typecodes = {int: "i", float: "f"}


def encode_arrays(datablock_proxy: DatablockProxy) -> List[bytes]:
    if not hasattr(datablock_proxy, "_arrays"):
        return [encode_int(0)]

    items = []
    items.append(encode_int(len(datablock_proxy._arrays)))
    for array_group_name, arrays in datablock_proxy._arrays.items():
        # for vertex groups, _arrays layout is
        # { "vertex_groups: [
        #       ([0, "i"], indices_array_of_vertex_group_0),
        #       ([0, "w"], weights_array_of_vertex_group_0),
        #       ...
        # ]}
        items.append(encode_string(array_group_name))
        items.append(encode_int(len(arrays)))
        for key, array_ in arrays:
            key_string = json.dumps(key)
            items.append(encode_string(key_string))
            items.append(encode_py_array(array_))
    return items


def decode_arrays(buffer: bytes, index) -> Tuple[ArrayGroups, int]:
    array_group_count, index = decode_int(buffer, index)
    if array_group_count == 0:
        return {}, index

    array_groups: ArrayGroups = {}
    for _groups_index in range(array_group_count):
        array_group_name, index = decode_string(buffer, index)
        array_group_length, index = decode_int(buffer, index)
        array_group: ArrayGroup = []
        for _array_index in range(array_group_length):
            key_string, index = decode_string(buffer, index)
            key = json.loads(key_string)
            array_, index = decode_py_array(buffer, index)
            array_group.append(
                (key, array_),
            )
        array_groups[array_group_name] = array_group

    return array_groups, index


def _decode_soas(buffer: bytes, index: int) -> Tuple[List[Soa], int]:
    path: Path = []
    name = "unknown"
    soas: List[Soa] = []
    try:
        # see soa_buffers()
        aos_count, index = decode_int(buffer, index)
        for _ in range(aos_count):
            path_string, index = decode_string(buffer, index)
            path = json.loads(path_string)

            logger.info("%s: %s ", "build_soa", path)

            element_count, index = decode_int(buffer, index)
            members = []
            for _ in range(element_count):
                name, index = decode_string(buffer, index)
                array_, index = decode_py_array(buffer, index)
                members.append(
                    (name, array_),
                )
            soas.append(
                Soa(path, members),
            )
    except Exception:
        logger.error(f"Exception while decoding for {path} {name}")
        for line in traceback.format_exc().splitlines():
            logger.error(line)
        logger.error("ignored")
        raise

    return soas, index


class BlenderDataMessage:
    def __init__(self):
        self.proxy_string: str = ""
        self.soas: List[Soa] = []
        self.arrays: ArrayGroups = {}

    def __lt__(self, other):
        # for sorting by the tests
        return self.proxy_string < other.proxy_string

    def decode(self, buffer: bytes) -> int:
        self.proxy_string, index = decode_string(buffer, 0)
        self.soas, index = _decode_soas(buffer, index)
        self.arrays, index = decode_arrays(buffer, index)
        return index

    @staticmethod
    def encode(datablock_proxy: DatablockProxy, encoded_proxy: str) -> bytes:
        items = []
        items.append(encode_string(encoded_proxy))
        items.extend(soa_buffers(datablock_proxy))
        items.extend(encode_arrays(datablock_proxy))
        return b"".join(items)


class BlenderRemoveMessage:
    def __init__(self):
        self.uuid: str = ""
        self.debug_info: str = ""

    def __lt__(self, other):
        # for sorting by the tests
        return self.uuid < other.uuid

    def decode(self, buffer: bytes):
        self.uuid, index = decode_string(buffer, 0)
        self.debug_info, index = decode_string(buffer, index)

    @staticmethod
    def encode(uuid: str, debug_info: str) -> bytes:
        return encode_string(uuid) + encode_string(debug_info)


class BlenderRenamesMessage:
    def __init__(self):
        self.renames: List[str] = []

    def decode(self, buffer: bytes):
        self.renames, _ = decode_string_array(buffer, 0)

    @staticmethod
    def encode(renames: List[str]) -> bytes:
        return encode_string_array(renames)


class BlenderMediaMessage:
    def __init__(self):
        self.path: str = ""
        self.bytes_: bytes = b""

    def __lt__(self, other):
        # for sorting by the tests
        return self.path < other.path

    def decode(self, buffer: bytes) -> int:
        self.path, index = decode_string(buffer, 0)
        self.bytes_ = buffer[index:]
        return len(buffer)

    @staticmethod
    def encode(datablock_proxy: DatablockProxy) -> bytes:
        media_desc = getattr(datablock_proxy, "_media", None)
        if media_desc is None:
            return b""

        path, bytes_ = media_desc
        items = [encode_string(path), bytes_]
        return b"".join(items)
