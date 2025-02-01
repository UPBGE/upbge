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
This module defines an API to download, upload, save and load rooms.

An known issue is that all these functions are synchronous, so we cannot inform the user of completion.
"""

from mixer.broadcaster.common import MessageType, encode_json
from mixer.broadcaster.common import Command
from mixer.broadcaster.common import ClientDisconnectedException
from mixer.broadcaster.common import read_all_messages
from mixer.broadcaster.client import Client
from typing import List, Tuple, Dict, Any
import logging

logger = logging.getLogger(__name__)


def download_room(
    host: str, port: int, room_name: str, blender_version: str, mixer_version: str, generic_protocol: bool
) -> Tuple[Dict[str, Any], List[Command]]:
    from mixer.broadcaster.common import decode_json, RoomAttributes

    logger.info("Downloading room %s", room_name)

    commands = []

    with Client(host, port) as client:
        client.join_room(room_name, blender_version, mixer_version, False, generic_protocol)

        room_attributes = None

        try:
            while room_attributes is None or len(commands) < room_attributes[RoomAttributes.COMMAND_COUNT]:
                received_commands = client.fetch_incoming_commands()

                for command in received_commands:
                    if room_attributes is None and command.type == MessageType.LIST_ROOMS:
                        rooms_attributes, _ = decode_json(command.data, 0)
                        if room_name not in rooms_attributes:
                            logger.error("Room %s does not exist on server", room_name)
                            return {}, []
                        room_attributes = rooms_attributes[room_name]
                        logger.info(
                            "Meta data received, number of commands in the room: %d",
                            room_attributes[RoomAttributes.COMMAND_COUNT],
                        )
                    elif command.type <= MessageType.COMMAND:
                        continue  # don't store server protocol commands

                    commands.append(command)
                    if room_attributes is not None:
                        logger.debug(
                            "Command %d / %d received", len(commands), room_attributes[RoomAttributes.COMMAND_COUNT]
                        )
        except ClientDisconnectedException:
            logger.error(f"Disconnected while downloading room {room_name} from {host}:{port}")
            return {}, []

        assert room_attributes is not None

        client.leave_room(room_name)

    return room_attributes, commands


def upload_room(host: str, port: int, room_name: str, room_attributes: dict, commands: List[Command]):
    """
    Upload a room to the server.
    Warning: This function is blocking, so when run from Blender the client dedicated to the user will be blocked and will accumulate lot of
    room updates that will be processed later.
    Todo: Write a non blocking version of this function to be used inside Blender, some kind of UploadClient that can exist side by side with BlenderClient.
    """
    from mixer.broadcaster.common import RoomAttributes

    with Client(host, port) as client:
        client.join_room(
            room_name,
            room_attributes.get(RoomAttributes.BLENDER_VERSION, ""),
            room_attributes.get(RoomAttributes.MIXER_VERSION, ""),
            False,
            room_attributes.get(RoomAttributes.GENERIC_PROTOCOL, True),
        )
        client.set_room_attributes(room_name, room_attributes)
        client.set_room_keep_open(room_name, True)

        for idx, c in enumerate(commands):
            logger.debug("Sending command %s (%d / %d)", c.type, idx, len(commands))
            client.send_command(c)

            # The server will send back room update messages since the room is joined.
            # Consume them to avoid a client/server deadlock on broadcaster full send socket
            read_all_messages(client.socket, timeout=0.0)

        client.send_command(Command(MessageType.CONTENT))

        client.leave_room(room_name)
        if not client.wait(MessageType.LEAVE_ROOM):
            raise ClientDisconnectedException("Client disconnected before the end of upload room")


def save_room(room_attributes: dict, commands: List[Command], file_path: str):
    with open(file_path, "wb") as f:
        f.write(encode_json(room_attributes))
        for c in commands:
            f.write(c.to_byte_buffer())


def load_room(file_path: str) -> Tuple[dict, List[Command]]:
    from mixer.broadcaster.common import bytes_to_int, int_to_message_type
    import json

    # todo factorize file reading with network reading
    room_medata = None
    commands = []
    with open(file_path, "rb") as f:
        data = f.read(4)
        string_length = bytes_to_int(data)
        attributes_string = f.read(string_length).decode()
        room_medata = json.loads(attributes_string)
        while True:
            prefix_size = 14
            msg = f.read(prefix_size)
            if not msg:
                break

            frame_size = bytes_to_int(msg[:8])
            command_id = bytes_to_int(msg[8:12])
            message_type = bytes_to_int(msg[12:])

            msg = f.read(frame_size)

            commands.append(Command(int_to_message_type(message_type), msg, command_id))

    assert room_medata is not None

    return room_medata, commands
