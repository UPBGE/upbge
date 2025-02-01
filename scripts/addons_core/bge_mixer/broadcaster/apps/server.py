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
Server application.

Can be launched from command line, or from the Mixer addon in Blender.
"""

from __future__ import annotations

import logging
import argparse
import select
import threading
import time
import socket
import queue
from typing import List, Mapping, Dict, Optional, Any

from mixer.broadcaster.cli_utils import init_logging, add_logging_cli_args
import mixer.broadcaster.common as common
from mixer.broadcaster.common import update_attributes_and_get_diff
from mixer.broadcaster.socket import Socket

SHUTDOWN = False

logger = logging.getLogger() if __name__ == "__main__" else logging.getLogger(__name__)
_log_server_updates: bool = False

# If more that this number of commands need to be broadcaster to a joining
# client, then release the room mutex while broadcasting
MAX_BROADCAST_COMMAND_COUNT = 64


class Connection:
    """ Represent a connection with a client """

    def __init__(self, server: Server, sock: Socket, address):
        self.socket: Socket = sock
        self.address = address
        self.room: Optional[Room] = None

        self.unique_id = f"{address[0]}:{address[1]}"

        self.custom_attributes: Dict[str, Any] = {}  # custom attributes are used between clients, but not by the server

        self._command_queue: queue.Queue = queue.Queue()  # Pending commands to send to the client
        self._server = server

        self.thread: threading.Thread = threading.Thread(None, self.run)

    def start(self):
        self.thread.start()

    def client_attributes(self) -> Dict[str, Any]:
        return {
            **self.custom_attributes,
            common.ClientAttributes.ID: f"{self.unique_id}",
            common.ClientAttributes.IP: self.address[0],
            common.ClientAttributes.PORT: self.address[1],
            common.ClientAttributes.ROOM: self.room.name if self.room is not None else None,
        }

    def broadcast_error(self, command: common.Command):
        self._server.broadcast_to_all_clients(command)

    def run(self):
        def _send_error(s: str):
            logger.error("Sending error %s", s)
            self.send_command(common.Command(common.MessageType.SEND_ERROR, common.encode_string(s)))

        def _join_room(command: common.Command):
            if self.room is not None:
                _send_error(f"Received join_room but room {self.room.name} is already joined")
                return
            room_name, index = common.decode_string(command.data, 0)
            blender_version, index = common.decode_string(command.data, index)
            mixer_version, index = common.decode_string(command.data, index)
            ignore_version_check, index = common.decode_bool(command.data, index)
            generic_protocol, _ = common.decode_bool(command.data, index)
            try:
                self._server.join_room(
                    self, room_name, blender_version, mixer_version, ignore_version_check, generic_protocol
                )
            except Exception as e:
                _send_error(f"{e!r}")

        def _leave_room(command: common.Command):
            if self.room is None:
                _send_error("Received leave_room but no room is joined")
                return
            _ = command.data.decode()  # todo remove room_name from protocol
            self._server.leave_room(self)
            self.send_command(common.Command(common.MessageType.LEAVE_ROOM))

        def _list_rooms(command: common.Command):
            self.send_command(self._server.get_list_rooms_command())

        def _delete_room(command: common.Command):
            self._server.delete_room(command.data.decode())

        def _set_custom_attributes(custom_attributes: Mapping[str, Any]):
            diff = update_attributes_and_get_diff(self.custom_attributes, custom_attributes)
            self._server.broadcast_client_update(self, diff)

        def _set_client_name(command: common.Command):
            _set_custom_attributes({common.ClientAttributes.USERNAME: command.data.decode()})

        def _list_clients(command: common.Command):
            self.send_command(self._server.get_list_clients_command())

        def _set_client_custom_attributes(command: common.Command):
            _set_custom_attributes(common.decode_json(command.data, 0)[0])

        def _set_room_custom_attributes(command: common.Command):
            room_name, offset = common.decode_string(command.data, 0)
            custom_attributes, _ = common.decode_json(command.data, offset)
            self._server.set_room_custom_attributes(room_name, custom_attributes)

        def _set_room_keep_open(command: common.Command):
            room_name, offset = common.decode_string(command.data, 0)
            value, _ = common.decode_bool(command.data, offset)
            self._server.set_room_keep_open(room_name, value)

        def _client_id(command: common.Command):
            self.send_command(
                common.Command(common.MessageType.CLIENT_ID, f"{self.address[0]}:{self.address[1]}".encode("utf8"))
            )

        def _content(command: common.Command):
            if self.room is None:
                _send_error("Unjoined client trying to set room joinable")
                return
            if self.room.joinable:
                _send_error(f"Trying to set joinable room {self.room.name} which is already joinable")
                return
            self.room.joinable = True
            self._server.broadcast_room_update(self.room, {common.RoomAttributes.JOINABLE: True})

        command_handlers = {
            common.MessageType.JOIN_ROOM: _join_room,
            common.MessageType.LEAVE_ROOM: _leave_room,
            common.MessageType.LIST_ROOMS: _list_rooms,
            common.MessageType.DELETE_ROOM: _delete_room,
            common.MessageType.SEND_ERROR: lambda x: self.broadcast_error(x),
            common.MessageType.SET_ROOM_CUSTOM_ATTRIBUTES: _set_room_custom_attributes,
            common.MessageType.SET_ROOM_KEEP_OPEN: _set_room_keep_open,
            common.MessageType.LIST_CLIENTS: _list_clients,
            common.MessageType.SET_CLIENT_NAME: _set_client_name,
            common.MessageType.SET_CLIENT_CUSTOM_ATTRIBUTES: _set_client_custom_attributes,
            common.MessageType.CLIENT_ID: _client_id,
            common.MessageType.CONTENT: _content,
        }

        def _handle_incoming_commands():
            received_commands = common.read_all_messages(self.socket)
            count = len(received_commands)
            if count > 0:
                # upstream
                time.sleep(self.latency)
                logger.debug("Received from %s - %d commands ", self.unique_id, count)

            for command in received_commands:
                if _log_server_updates or command.type not in (common.MessageType.SET_CLIENT_CUSTOM_ATTRIBUTES,):
                    logger.debug("Received from %s - %s", self.unique_id, command.type)

                if command.type in command_handlers:
                    command_handlers[command.type](command)
                elif command.type.value > common.MessageType.COMMAND.value:
                    if self.room is not None:
                        self.room.add_command(command, self)
                    else:
                        logger.warning(
                            "%s:%s - %s received but no room was joined",
                            self.address[0],
                            self.address[1],
                            command.type,
                        )
                else:
                    logger.error("Command %s received but no handler for it on server", command.type)

        def _handle_outgoing_commands():
            self.fetch_outgoing_commands()

        global SHUTDOWN
        while not SHUTDOWN:
            try:
                _handle_incoming_commands()
                _handle_outgoing_commands()
            except common.ClientDisconnectedException:
                break
            except Exception:
                logger.exception("Exception during command processing. Disconnecting")
                logger.error(f"Disconnecting {self.custom_attributes.get(common.ClientAttributes.USERNAME, 'Unknown')}")
                break

        self._server.handle_client_disconnect(self)

    def fetch_outgoing_commands(self):
        while True:
            try:
                command = self._command_queue.get_nowait()
            except queue.Empty:
                break

            self.send_command(command)
            self._command_queue.task_done()

    def add_command(self, command: common.Command):
        """
        Add command to be consumed later. Meant to be used by other threads.
        """
        self._command_queue.put(command)

    def send_command(self, command: common.Command):
        """
        Directly send a command to the socket. Meant to be used by this thread.
        """
        assert threading.current_thread() is self.thread
        if _log_server_updates or command.type not in (
            common.MessageType.CLIENT_UPDATE,
            common.MessageType.ROOM_UPDATE,
        ):
            logger.debug("Sending to %s:%s - %s", self.address[0], self.address[1], command.type)
        common.write_message(self.socket, command)


class Room:
    """
    Room class is responsible for:
    - handling its list of clients (as Connection instances)
    - keep a list of commands, to be dispatched to new clients
    - dispatch added commands to clients already in the room
    """

    def __init__(
        self,
        server: Server,
        room_name: str,
        blender_version: str,
        mixer_version: str,
        ignore_version_check: bool,
        generic_protocol: bool,
        creator: Connection,
    ):
        self.name = room_name
        self.blender_version = blender_version
        self.mixer_version = mixer_version
        self.ignore_version_check = ignore_version_check
        self.generic_protocol = generic_protocol
        self.keep_open = False  # Should the room remain open when no more clients are inside ?
        self.byte_size = 0
        self.joinable = False  # A room becomes joinable when its first client has send all the initial content

        self.custom_attributes: Dict[str, Any] = {}  # custom attributes are used between clients, but not by the server

        self._commands: List[common.Command] = []

        self._commands_mutex: threading.RLock = threading.RLock()
        self._connections: List[Connection] = [creator]

        self.join_count: int = 0
        # this is used to ensure a room cannot be deleted while clients are joining (creator is not considered to be joining)
        # Server is responsible of increasing / decreasing join_count, with mutex protection

        creator.room = self
        creator.send_command(common.Command(common.MessageType.JOIN_ROOM, common.encode_string(self.name)))
        creator.send_command(
            common.Command(common.MessageType.CONTENT)
        )  # self.joinable will be set to true by creator later

    def client_count(self):
        return len(self._connections) + self.join_count

    def command_count(self):
        return len(self._commands)

    def add_client(self, connection: Connection):
        logger.info(f"Add Client {connection.unique_id} to Room {self.name}")

        connection.send_command(common.Command(common.MessageType.CLEAR_CONTENT))  # todo temporary size stored here

        offset = 0

        def _try_finish_sync():
            connection.fetch_outgoing_commands()
            with self._commands_mutex:
                # from here no one can add commands anymore to self._commands (clients can still join and read previous commands)
                command_count = self.command_count()
                if command_count - offset > MAX_BROADCAST_COMMAND_COUNT:
                    return False  # while still more than MAX_BROADCAST_COMMAND_COUNT commands to broadcast, release the mutex

                # now is time to synchronize all room participants: broadcast remaining commands to new client
                for i in range(offset, command_count):
                    command = self._commands[i]
                    connection.add_command(command)

                # now he's part of the room, let him/her know
                self._connections.append(connection)
                connection.room = self
                connection.add_command(common.Command(common.MessageType.JOIN_ROOM, common.encode_string(self.name)))
                return True

        while True:
            if _try_finish_sync():
                break  # all done
            # broadcast commands that were added since last check
            command_count = self.command_count()
            for i in range(offset, command_count):
                command = self._commands[i]  # atomic wrt. the GIL
                connection.add_command(command)
            offset = command_count

    def remove_client(self, connection: Connection):
        logger.info("Remove Client % s from Room % s", connection.address, self.name)
        self._connections.remove(connection)

    def attributes_dict(self):
        return {
            **self.custom_attributes,
            common.RoomAttributes.KEEP_OPEN: self.keep_open,
            common.RoomAttributes.BLENDER_VERSION: self.blender_version,
            common.RoomAttributes.MIXER_VERSION: self.mixer_version,
            common.RoomAttributes.IGNORE_VERSION_CHECK: self.ignore_version_check,
            common.RoomAttributes.GENERIC_PROTOCOL: self.generic_protocol,
            common.RoomAttributes.COMMAND_COUNT: self.command_count(),
            common.RoomAttributes.BYTE_SIZE: self.byte_size,
            common.RoomAttributes.JOINABLE: self.joinable,
        }

    def add_command(self, command, sender: Connection):
        def merge_command():
            """
            Add the command to the room list, possibly merge with the previous command.
            """
            command_type = command.type
            if (
                common.MessageType.OPTIMIZED_COMMANDS.value
                < command_type.value
                < common.MessageType.END_OPTIMIZED_COMMANDS.value
            ):
                command_path = common.decode_string(command.data, 0)[0]
                if self.command_count() > 0:
                    stored_command = self._commands[-1]
                    if (
                        command_type == stored_command.type
                        and command_path == common.decode_string(stored_command.data, 0)[0]
                    ):
                        self._commands.pop()
                        self.byte_size -= stored_command.byte_size()
            if (
                command_type != common.MessageType.CLIENT_ID_WRAPPER
                and command_type != common.MessageType.FRAME
                and command_type != common.MessageType.QUERY_ANIMATION_DATA
            ):
                self._commands.append(command)
                self.byte_size += command.byte_size()

        with self._commands_mutex:
            current_byte_size = self.byte_size
            current_command_count = self.command_count()
            merge_command()

            room_update = {}
            if self.byte_size != current_byte_size:
                room_update[common.RoomAttributes.BYTE_SIZE] = self.byte_size
            if current_command_count != self.command_count():
                room_update[common.RoomAttributes.COMMAND_COUNT] = self.command_count()

            sender._server.broadcast_room_update(self, room_update)

            for connection in self._connections:
                if connection != sender:
                    connection.add_command(command)


class Server:
    def __init__(self):
        self._rooms: Dict[str, Room] = {}
        self._connections: Dict[str, Connection] = {}
        self._mutex = threading.RLock()
        self.latency: float = 0.0  # seconds
        self.bandwidth: float = 0.0  # MBps

    def delete_room(self, room_name: str):
        with self._mutex:
            if room_name not in self._rooms:
                logger.warning("Room %s does not exist.", room_name)
                return
            if self._rooms[room_name].client_count() > 0:
                logger.warning("Room %s is not empty.", room_name)
                return

            del self._rooms[room_name]
            logger.info(f"Room {room_name} deleted")

            self.broadcast_to_all_clients(
                common.Command(common.MessageType.ROOM_DELETED, common.encode_string(room_name))
            )

    def join_room(
        self,
        connection: Connection,
        room_name: str,
        blender_version: str,
        mixer_version: str,
        ignore_version_check: bool,
        generic_protocol: bool,
    ):
        assert connection.room is None

        def _create_room():
            logger.info(f"Room {room_name} does not exist. Creating it.")
            room = Room(
                self, room_name, blender_version, mixer_version, ignore_version_check, generic_protocol, connection
            )
            self._rooms[room_name] = room
            # room is now visible to others, but not joinable until the client has sent CONTENT
            logger.info(
                f"Room {room_name} added with blender version {blender_version} and mixer version {mixer_version} (ignore version check: {ignore_version_check})"
            )

            self.broadcast_room_update(room, room.attributes_dict())  # Inform new room
            self.broadcast_client_update(connection, {common.ClientAttributes.ROOM: connection.room.name})

        with self._mutex:
            room = self._rooms.get(room_name)
            if room is None:
                _create_room()
                return

            if not room.joinable:
                raise Exception(f"Room {room_name} not joinable yet.")

            if not room.ignore_version_check:
                if room.blender_version != blender_version:
                    raise Exception(
                        f"Blender version mismatch with room {room_name}: client version is {blender_version}, room version is {room.blender_version}"
                    )

                if room.mixer_version != mixer_version:
                    raise Exception(
                        f"Mixer version mismatch with room {room_name}: client version is {mixer_version}, room version is {room.mixer_version}"
                    )

            # Do this before releasing the global mutex
            # Ensure the room will not be deleted because it now has at least one client
            room.join_count += 1

        room.add_client(connection)
        # this call can take a while because history broadcasting occurs, so the mutex is released here

        # from here client is in the room list, we can decrease join_count
        with self._mutex:
            room.join_count -= 1

        assert connection.room is not None
        self.broadcast_client_update(connection, {common.ClientAttributes.ROOM: connection.room.name})

    def leave_room(self, connection: Connection):
        assert connection.room is not None
        with self._mutex:
            room = self._rooms.get(connection.room.name)
            if room is None:
                raise ValueError(f"Room not found {connection.room.name})")
            room.remove_client(connection)
            connection.room = None
            self.broadcast_client_update(connection, {common.ClientAttributes.ROOM: None})

            if room.client_count() == 0 and not room.keep_open:
                logger.info('No more clients in room "%s" and not keep_open', room.name)
                self.delete_room(room.name)
            else:
                logger.info(f"Connections left in room {room.name}: {room.client_count()}.")

    def broadcast_to_all_clients(self, command: common.Command):
        with self._mutex:
            for connection in self._connections.values():
                connection.add_command(command)

    def broadcast_client_update(self, connection: Connection, attributes: Dict[str, Any]):
        if attributes == {}:
            return

        self.broadcast_to_all_clients(
            common.Command(common.MessageType.CLIENT_UPDATE, common.encode_json({connection.unique_id: attributes}))
        )

    def broadcast_room_update(self, room: Room, attributes: Dict[str, Any]):
        if attributes == {}:
            return

        self.broadcast_to_all_clients(
            common.Command(
                common.MessageType.ROOM_UPDATE,
                common.encode_json({room.name: attributes}),
            )
        )

    def set_room_custom_attributes(self, room_name: str, custom_attributes: Mapping[str, Any]):
        with self._mutex:
            if room_name not in self._rooms:
                logger.warning("Room %s does not exist.", room_name)
                return

            diff = update_attributes_and_get_diff(self._rooms[room_name].custom_attributes, custom_attributes)
            self.broadcast_room_update(self._rooms[room_name], diff)

    def set_room_keep_open(self, room_name: str, value: bool):
        with self._mutex:
            if room_name not in self._rooms:
                logger.warning("Room %s does not exist.", room_name)
                return
            room = self._rooms[room_name]
            if room.keep_open != value:
                room.keep_open = value
                self.broadcast_room_update(room, {common.RoomAttributes.KEEP_OPEN: room.keep_open})

    def get_list_rooms_command(self) -> common.Command:
        with self._mutex:
            result_dict = {room_name: value.attributes_dict() for room_name, value in self._rooms.items()}
            return common.Command(common.MessageType.LIST_ROOMS, common.encode_json(result_dict))

    def get_list_clients_command(self) -> common.Command:
        with self._mutex:
            result_dict = {cid: c.client_attributes() for cid, c in self._connections.items()}
            return common.Command(common.MessageType.LIST_CLIENTS, common.encode_json(result_dict))

    def handle_client_disconnect(self, connection: Connection):
        # First remove connection from server state, to avoid further broadcasting tentatives
        with self._mutex:
            del self._connections[connection.unique_id]

        # Clean leaving of the room
        if connection.room is not None:
            self.leave_room(connection)

        try:
            connection.socket.close()
        except Exception as e:
            logger.warning(e)
        logger.info("%s closed", connection.address)

        self.broadcast_to_all_clients(
            common.Command(common.MessageType.CLIENT_DISCONNECTED, common.encode_string(connection.unique_id))
        )

    def run(self, port):
        global SHUTDOWN
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        binding_host = ""
        sock.bind((binding_host, port))
        sock.setblocking(0)
        sock.listen(1000)

        logger.info("Listening on port % s", port)
        while True:
            try:
                timeout = 0.1  # Check for a new client every 10th of a second
                readable, _, _ = select.select([sock], [], [], timeout)
                if len(readable) > 0:
                    client_socket, client_address = sock.accept()
                    client_socket = Socket(client_socket)
                    client_socket.set_bandwidth(self.bandwidth, self.bandwidth)

                    connection = Connection(self, client_socket, client_address)
                    connection.latency = self.latency
                    with self._mutex:
                        self._connections[connection.unique_id] = connection
                    connection.start()
                    logger.info(f"New connection from {client_address}")
                    self.broadcast_client_update(connection, connection.client_attributes())
            except KeyboardInterrupt:
                break

        logger.info("Shutting down server")
        SHUTDOWN = True
        sock.close()


def main():
    global _log_server_updates
    args, args_parser = parse_cli_args()
    init_logging(args)
    if args.latency > 0.0:
        logger.warning(f"Latency set to {args.latency} ms")
    if args.bandwidth > 0.0:
        logger.warning(f"Bandwidth limited to {args.bandwidth} Mbps")
    _log_server_updates = args.log_server_updates

    server = Server()
    server.latency = args.latency / 1000.0
    server.bandwidth = args.bandwidth
    server.run(args.port)


def parse_cli_args():
    parser = argparse.ArgumentParser(description="Start broadcasting server for Mixer")
    add_logging_cli_args(parser)
    parser.add_argument("--port", type=int, default=common.DEFAULT_PORT)
    parser.add_argument("--log-server-updates", action="store_true")
    parser.add_argument(
        "--bandwidth", type=float, default=0.0, help="simulate bandwidth limitation (megabytes per second)"
    )
    parser.add_argument("--latency", type=float, default=0.0, help="simulate network latency (in milliseconds)")
    return parser.parse_args(), parser


if __name__ == "__main__":
    main()
