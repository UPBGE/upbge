# MIT License
#
# Copyright (c) 2020 Ubisoft
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""
This module defines types and utilities used by client and server code.
"""

import array
from enum import IntEnum
from typing import Dict, Mapping, Any, Optional, List, Tuple
import select
import struct
import json
import logging

from mixer.broadcaster.socket import Socket

DEFAULT_HOST = "localhost"
DEFAULT_PORT = 12800

logger = logging.getLogger(__name__)


class MessageType(IntEnum):
    """
    Each message has a integer code to identify it.

    A known issue of this strategy is that it is difficult to sync the code of different kind of clients (blender, vrtist)
    according to changes here. This will be adressed in the future by improving the protocol to include the notion
    of client types.

    Documentation to update if you change this: doc/protocol.md
    """

    JOIN_ROOM = 1
    LEAVE_ROOM = 3
    LIST_ROOMS = 4
    CONTENT = 5  # Server: ask client to send initial room content; Client: notify server content has been sent
    CLEAR_CONTENT = 6  # Server: ask client to clear its own content before room content is sent
    DELETE_ROOM = 7

    # All joined clients for all rooms
    SET_CLIENT_NAME = 11  # Deprecated
    SEND_ERROR = 12

    # All all joined and un joined clients
    LIST_CLIENTS = 14
    SET_CLIENT_CUSTOM_ATTRIBUTES = 15
    SET_ROOM_CUSTOM_ATTRIBUTES = 16
    SET_ROOM_KEEP_OPEN = 17
    CLIENT_ID = 18  # Client: ask the server to send the unique id string for him; Server: send the unique id string of the client

    CLIENT_UPDATE = 19  # Server: Notify that data of a client have changed
    ROOM_UPDATE = 20  # Server: Notify that data of a room have changed
    ROOM_DELETED = 21  # Server: Notify a room was deleted

    CLIENT_DISCONNECTED = 22  # Server: Notify a client has diconnected

    COMMAND = 100
    DELETE = 101
    CAMERA = 102
    LIGHT = 103
    _MESHCONNECTION_DEPRECATED = 104
    RENAME = 105
    DUPLICATE = 106
    SEND_TO_TRASH = 107
    RESTORE_FROM_TRASH = 108
    TEXTURE = 109

    ADD_COLLECTION_TO_COLLECTION = 110
    REMOVE_COLLECTION_FROM_COLLECTION = 111
    ADD_OBJECT_TO_COLLECTION = 112
    REMOVE_OBJECT_FROM_COLLECTION = 113

    ADD_OBJECT_TO_SCENE = 114
    ADD_COLLECTION_TO_SCENE = 115

    INSTANCE_COLLECTION = 116
    COLLECTION = 117
    COLLECTION_REMOVED = 118
    SET_SCENE = 119
    GREASE_PENCIL_MESH = 120
    GREASE_PENCIL_MATERIAL = 121
    GREASE_PENCIL_CONNECTION = 122
    GREASE_PENCIL_TIME_OFFSET = 123
    FRAME_START_END = 124
    ANIMATION = 125

    REMOVE_OBJECT_FROM_SCENE = 126
    REMOVE_COLLECTION_FROM_SCENE = 127

    SCENE = 128
    _SCENE_REMOVED_DEPRECATED = 129

    ADD_OBJECT_TO_VRTIST = 130
    OBJECT_VISIBILITY = 131

    # Start / End a group of command. Allows to inform clients that they must process multiple commands
    # before giving back control to they users.
    GROUP_BEGIN = 132
    GROUP_END = 133

    _SCENE_RENAMED_DEPRECATED = 134

    ADD_KEYFRAME = 135
    REMOVE_KEYFRAME = 136
    MOVE_KEYFRAME = 137
    QUERY_CURRENT_FRAME = 138
    QUERY_ANIMATION_DATA = 139

    BLENDER_DATA_UPDATE = 140
    _CAMERA_ATTRIBUTES = 141
    _LIGHT_ATTRIBUTES = 142

    BLENDER_DATA_REMOVE = 143
    BLENDER_DATA_RENAME = 144

    CLEAR_ANIMATIONS = 145
    CURRENT_CAMERA = 146
    SHOT_MANAGER_MONTAGE_MODE = 147
    SHOT_MANAGER_CONTENT = 148
    SHOT_MANAGER_CURRENT_SHOT = 149
    SHOT_MANAGER_ACTION = 150

    BLENDER_DATA_CREATE = 151
    BLENDER_DATA_MEDIA = 152
    EMPTY = 153
    ADD_CONSTRAINT = 154
    REMOVE_CONSTRAINT = 155
    ASSET_BANK = 156
    SAVE = 157

    OPTIMIZED_COMMANDS = 200
    TRANSFORM = 201
    MESH = 202
    MATERIAL = 203
    ASSIGN_MATERIAL = 204
    FRAME = 205
    PLAY = 206
    PAUSE = 207
    WORLD_SKY = 208
    END_OPTIMIZED_COMMANDS = 999

    CLIENT_ID_WRAPPER = 1000


class LightType(IntEnum):
    SPOT = 0  # directly mapped from Unity enum
    SUN = 1
    POINT = 2
    AREA = 3


class SensorFitMode(IntEnum):
    AUTO = 0
    VERTICAL = 1
    HORIZONTAL = 2


class ClientAttributes:
    """
    Attributes associated with a client by the server.
    First part is defined by the server, second part is generic and sent by clients to be forwarded to others.
    Clients are free to define custom attributes they need, but some standard names are provided here to ease sync
    between clients of different kind.

    Documentation to update if you change this: doc/protocol.md
    """

    ID = "id"  # Sent by server only, type = str, the id of the client which is unique for each connected client
    IP = "ip"  # Sent by server only, type = str
    PORT = "port"  # Sent by server only, type = int
    ROOM = "room"  # Sent by server only, type = str

    # Client to server attributes, not used by the server but clients are encouraged to use these keys for the same semantic
    USERNAME = "user_name"  # type = str
    USERCOLOR = "user_color"  # type = float3 (as list)
    USERSCENES = "user_scenes"  # type = dict(str, dict) key = Scene name_full, value = a dictionnary for scene attributes relative to the user
    USERSCENES_FRAME = "frame"  # type = int, can be a field in a user_scenes dict
    USERSCENES_SELECTED_OBJECTS = "selected_objects"  # type = list[string], can be a field in a user_scenes dict
    USERSCENES_VIEWS = (
        "views"  # type dict(str, dict), can be a field in a user_scenes dict; keys are unique ids for the views
    )
    USERSCENES_VIEWS_EYE = "eye"  # type = float3 (as list)
    USERSCENES_VIEWS_TARGET = "target"  # type = float3 (as list)
    USERSCENES_VIEWS_SCREEN_CORNERS = (
        "screen_corners"  # type = list[float3], 4 elements, bottom_left, bottom_right, top_right, top_left
    )
    USERMODE = "user_mode"  # type = str


class RoomAttributes:
    """
    Attributes associated with a room by the server.
    First part is defined by the server, second part is generic and sent by clients to be forwarded to others.
    Clients are free to define custom attributes they need, but some standard names are provided here to ease sync
    between clients of different kind.

    Documentation to update if you change this: doc/protocol.md
    """

    NAME = "name"  # Sent by server only, type = str, the name of the room which is unique for each room
    BLENDER_VERSION = (
        "blender_version"  # Sent by server only, type = str, the version of blender which created the room
    )
    MIXER_VERSION = "mixer_version"  # Sent by server only, type = str, the version of mixer which created the room
    IGNORE_VERSION_CHECK = (
        "ignore_version_check"  # Sent by server only, type = str, to ignore blender and mixer versions
    )
    GENERIC_PROTOCOL = "generic_protocol"  # Sent by server only, type = bool
    KEEP_OPEN = (
        "keep_open"  # Sent by server only, type = bool, indicate if the room should be kept open after all clients left
    )
    COMMAND_COUNT = "command_count"  # Sent by server only, type = bool, indicate how many commands the room contains
    BYTE_SIZE = "byte_size"  # Sent by server only, type = int, indicate the size in byte of the room
    JOINABLE = "joinable"  # Sent by server only, type = bool, indicate if the room is joinable


class ClientDisconnectedException(Exception):
    """When a client is disconnected and we try to read from it."""


def int_to_bytes(value, size=8):
    return value.to_bytes(size, byteorder="little")


def bytes_to_int(value):
    return int.from_bytes(value, "little")


def int_to_message_type(value):
    return MessageType(value)


def encode_bool(value):
    if value:
        return int_to_bytes(1, 4)
    else:
        return int_to_bytes(0, 4)


def decode_bool(data, index):
    value = bytes_to_int(data[index : index + 4])
    if value == 1:
        return True, index + 4
    else:
        return False, index + 4


def encode_string(value):
    encoded_value = value.encode()
    return int_to_bytes(len(encoded_value), 4) + encoded_value


def decode_string(data, index):
    string_length = bytes_to_int(data[index : index + 4])
    start = index + 4
    end = start + string_length
    value = data[start:end].decode()
    return value, end


def encode_json(value: dict):
    return encode_string(json.dumps(value))


def decode_json(data, index):
    value, end = decode_string(data, index)
    return json.loads(value), end


def encode_float(value):
    return struct.pack("f", value)


def decode_float(data, index):
    return struct.unpack("f", data[index : index + 4])[0], index + 4


def encode_int(value):
    return struct.pack("i", value)


def decode_int(data, index):
    return struct.unpack("i", data[index : index + 4])[0], index + 4


def encode_vector2(value):
    return struct.pack("2f", *(value.x, value.y))


def decode_vector2(data, index):
    return struct.unpack("2f", data[index : index + 2 * 4]), index + 2 * 4


def encode_vector3(value):
    return struct.pack("3f", *(value.x, value.y, value.z))


def decode_vector3(data, index):
    return struct.unpack("3f", data[index : index + 3 * 4]), index + 3 * 4


def encode_vector4(value):
    return struct.pack("4f", *(value[0], value[1], value[2], value[3]))


def decode_vector4(data, index):
    return struct.unpack("4f", data[index : index + 4 * 4]), index + 4 * 4


def encode_matrix(value):
    return (
        encode_vector4(value.col[0])
        + encode_vector4(value.col[1])
        + encode_vector4(value.col[2])
        + encode_vector4(value.col[3])
    )


def decode_matrix(data, index):
    c0, index = decode_vector4(data, index)
    c1, index = decode_vector4(data, index)
    c2, index = decode_vector4(data, index)
    c3, index = decode_vector4(data, index)
    return (c0, c1, c2, c3), index


def encode_color(value):
    if len(value) == 3:
        return struct.pack("4f", *(value[0], value[1], value[2], 1.0))
    else:
        return struct.pack("4f", *(value[0], value[1], value[2], value[3]))


def decode_color(data, index):
    return struct.unpack("4f", data[index : index + 4 * 4]), index + 4 * 4


def encode_quaternion(value):
    return struct.pack("4f", *(value.w, value.x, value.y, value.z))


def decode_quaternion(data, index):
    return struct.unpack("4f", data[index : index + 4 * 4]), index + 4 * 4


def encode_string_array(values):
    buffer = encode_int(len(values))
    for item in values:
        buffer += encode_string(item)
    return buffer


def decode_string_array(data, index):
    count = bytes_to_int(data[index : index + 4])
    index = index + 4
    values = []
    for _ in range(count):
        string, index = decode_string(data, index)
        values.append(string)
    return values, index


def decode_array(data, index, schema, inc):
    count = bytes_to_int(data[index : index + 4])
    start = index + 4
    end = start
    values = []
    for _ in range(count):
        end = start + inc
        values.append(struct.unpack(schema, data[start:end]))
        start = end
    return values, end


def decode_float_array(data, index):
    count = bytes_to_int(data[index : index + 4])
    start = index + 4
    values = []
    end = start
    for _ in range(count):
        end = start + 4
        values.extend(struct.unpack("f", data[start:end]))
        start = end
    return values, end


def decode_int_array(data, index):
    count = bytes_to_int(data[index : index + 4])
    start = index + 4
    values = []
    end = start
    for _ in range(count):
        end = start + 4
        values.extend(struct.unpack("I", data[start:end]))
        start = end
    return values, end


def decode_int2_array(data, index):
    return decode_array(data, index, "2I", 2 * 4)


def decode_int3_array(data, index):
    return decode_array(data, index, "3I", 3 * 4)


def decode_vector3_array(data, index):
    return decode_array(data, index, "3f", 3 * 4)


def decode_vector2_array(data, index):
    return decode_array(data, index, "2f", 2 * 4)


def encode_py_array(data: array.array) -> bytes:
    typecode = data.typecode
    count = data.buffer_info()[1]
    byte_count = count * data.itemsize
    buffer = encode_string(typecode) + encode_int(byte_count) + data.tobytes()
    return buffer


def decode_py_array(buffer: bytes, index: int) -> Tuple[array.array, int]:
    typecode, index = decode_string(buffer, index)
    byte_count, index = decode_int(buffer, index)
    array_ = array.array(typecode, b"")
    slice_ = buffer[index : index + byte_count]
    array_.frombytes(slice_)
    return array_, index + byte_count


class Command:
    _id = 100

    def __init__(self, command_type: MessageType, data=b"", command_id=0):
        self.data = data or b""
        self.type = command_type
        self.id = command_id
        if command_id == 0:
            self.id = Command._id
            Command._id += 1

    def byte_size(self):
        return 8 + 4 + 2 + len(self.data)

    def to_byte_buffer(self):
        size = int_to_bytes(len(self.data), 8)
        command_id = int_to_bytes(self.id, 4)
        mtype = int_to_bytes(self.type.value, 2)

        return size + command_id + mtype + self.data


class CommandFormatter:
    def format_clients(self, clients):
        s = ""
        for c in clients:
            s += f'   - {c[ClientAttributes.IP]}:{c[ClientAttributes.PORT]} name = "{c[ClientAttributes.USERNAME]}" room = "{c[ClientAttributes.ROOM]}"\n'
        return s

    def format(self, command: Command):

        s = f"={command.type.name}: "

        if command.type == MessageType.LIST_ROOMS:
            rooms, _ = decode_string_array(command.data, 0)
            s += "LIST_ROOMS: "
            if len(rooms) == 0:
                s += "  No rooms"
            else:
                s += f" {len(rooms)} room(s) : {rooms}"
        elif command.type == MessageType.LIST_CLIENTS:
            clients, _ = decode_json(command.data, 0)
            if len(clients) == 0:
                s += "  No clients\n"
            else:
                s += f"  {len(clients)} client(s):\n"
                s += self.format_clients(clients)
        elif command.type == MessageType.SEND_ERROR:
            s += f"ERROR: {decode_string(command.data, 0)[0]}\n"
        else:
            pass

        return s


def recv(socket: Socket, size: int):
    """
    Try to read size bytes from the socket.
    Raise ClientDisconnectedException if the socket is disconnected.
    """
    result = b""
    while size != 0:
        r, _, _ = select.select([socket], [], [], 0.1)
        if len(r) > 0:
            try:
                tmp = socket.recv(size)
            except (ConnectionAbortedError, ConnectionResetError) as e:
                logger.warning(e)
                raise ClientDisconnectedException()

            if len(tmp) == 0:
                raise ClientDisconnectedException()

            result += tmp
            size -= len(tmp)
    return result


def read_message(socket: Socket, timeout: Optional[float] = None) -> Optional[Command]:
    """
    Try to read a full message from the socket.
    Raise ClientDisconnectedException if the socket is disconnected.
    Return None if no message is waiting on the socket.
    """
    if not socket:
        logger.warning("read_message called with no socket")
        return None

    select_timeout = timeout if timeout is not None else 0.0001
    r, _, _ = select.select([socket._socket], [], [], select_timeout)
    if len(r) == 0:
        return None

    try:
        prefix_size = 14
        msg = recv(socket, prefix_size)

        frame_size = bytes_to_int(msg[:8])
        command_id = bytes_to_int(msg[8:12])
        message_type = bytes_to_int(msg[12:])

        msg = recv(socket, frame_size)

        return Command(int_to_message_type(message_type), msg, command_id)

    except ClientDisconnectedException:
        raise
    except Exception as e:
        logger.error(e, exc_info=True)
        raise


def read_all_messages(socket: Socket, timeout: Optional[float] = None) -> List[Command]:
    """
    Try to read all messages waiting on the socket.
    Raise ClientDisconnectedException if the socket is disconnected.
    Return empty list if no message is waiting on the socket.
    """
    received_commands: List[Command] = []
    while True:
        command = read_message(socket, timeout=timeout)
        if command is None:
            break
        received_commands.append(command)
    return received_commands


def write_message(sock: Optional[Socket], command: Command):
    if not sock:
        logger.warning("write_message called with no socket")
        return

    buffer = command.to_byte_buffer()

    try:
        _, w, _ = select.select([], [sock._socket], [])
        if sock.sendall(buffer) is not None:
            raise ClientDisconnectedException()
    except (ConnectionAbortedError, ConnectionResetError) as e:
        logger.warning(e)
        raise ClientDisconnectedException()


def make_set_room_attributes_command(room_name: str, attributes: dict):
    return Command(MessageType.SET_ROOM_CUSTOM_ATTRIBUTES, encode_string(room_name) + encode_json(attributes))


def update_attributes_and_get_diff(current: Dict[str, Any], updates: Mapping[str, Any]) -> Dict[str, Any]:
    diff = {}
    for key, value in updates.items():
        if key not in current or current[key] != value:
            current[key] = value
            diff[key] = value
    return diff


def update_named_attributes_and_get_diff(
    current: Dict[str, Dict[str, Any]], updates: Mapping[str, Dict[str, Any]]
) -> Dict[str, Dict[str, Any]]:
    diff = {}
    for name, attrs_updates in updates.items():
        if name not in current:
            current[name] = attrs_updates
            diff[name] = attrs_updates
        else:
            diff[name] = update_attributes_and_get_diff(current[name], attrs_updates)
    return diff


def update_named_attributes(current: Dict[str, Dict[str, Any]], updates: Mapping[str, Dict[str, Any]]):
    for name, attrs_updates in updates.items():
        if name not in current:
            current[name] = attrs_updates
        else:
            attrs = current[name]
            for attr_name, attr_value in attrs_updates.items():
                attrs[attr_name] = attr_value
