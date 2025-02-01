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
Helper function fo encode and decode BLENDER_DATA_* messages
"""
import dataclasses
import importlib
from typing import Dict

from mixer.broadcaster import common


class Message:
    # Base class for encodable/decodable messages
    pass


class Matrix:
    pass


class Color:
    pass


MessageTypes = Dict[common.MessageType, type]

# The message types registered by "components" (VRtist, Blender protocol)
registered_message_types: MessageTypes = {}


# TODO extend
codec_functions = {
    float: (common.encode_float, common.decode_float),
    int: (common.encode_int, common.decode_int),
    str: (common.encode_string, common.decode_string),
    Color: (common.encode_color, common.decode_color),
    Matrix: (common.encode_matrix, common.decode_matrix),
}


def decode_as(message_type: common.MessageType, buffer: bytes) -> Message:
    """
    Decode buffer as message_type. Returns None is mesage_type is not registered
    """
    index = 0
    args = []
    message_class = registered_message_types.get(message_type)
    if message_class is None:
        raise NotImplementedError(f"No encode/decode function for {message_type}")

    if hasattr(message_class, "decode"):
        message = message_class()
        message.decode(buffer)
        return message
    else:
        fields = (f.type for f in dataclasses.fields(message_class))
        for type_ in fields:
            if type_ not in codec_functions:
                raise NotImplementedError(f"No codec_func for {type_}")
            decode = codec_functions[type_][1]
            decoded, index = decode(buffer, index)
            args.append(decoded)
        return message_class(*args)


def decode(command: common.Command) -> Message:
    return decode_as(command.type, command.data)


def encode(message: Message) -> bytes:
    # not tested, actually
    raise NotImplementedError("encode")
    buffer = b""
    fields = ((f.name, f.type) for f in dataclasses.fields(message))
    for (
        name,
        type_,
    ) in fields:
        if type_ not in codec_functions:
            raise NotImplementedError(f"No codec_func for {type_}")
        encode = codec_functions[type_][0]

        attr = getattr(message, name)
        # TODO need something smarter that multiple reallocations ?
        buffer += encode(type_)(attr)
    return buffer


def is_registered(message_type: common.MessageType) -> bool:
    return message_type in registered_message_types.keys()


def register_message_types(types_dict: MessageTypes):
    registered_message_types.update(types_dict)


def unregister_message_types(types_dict: MessageTypes):
    for t in types_dict:
        if t in registered_message_types:
            del registered_message_types[t]


# works around a circular dependency problem, a a tiny step towards  splitting VRtist and Blender protocols
_packages = ["mixer.blender_client", "mixer.blender_data"]


def register():
    for p in _packages:
        mod_name = p + ".codec"
        mod = importlib.import_module(mod_name)
        mod.register()


def unregister():
    for p in _packages:
        mod_name = p + ".codec"
        mod = importlib.import_module(mod_name)
        mod.unregister()
