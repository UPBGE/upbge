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
This module define how the addon connect and interact with the server, for the Mixer protocol.
It updates the addon state according to this connection.
"""

import logging

import bpy
import mixer
from mixer.bl_utils import get_mixer_prefs
from mixer.share_data import share_data
from mixer.broadcaster.common import ClientAttributes, ClientDisconnectedException
import subprocess
import time
from pathlib import Path

from mixer.draw_handlers import remove_draw_handlers
from mixer.blender_client.client import SendSceneContentFailed, BlenderClient
from mixer.handlers import HandlerManager
from mixer.os_utils import addon_infos, tech_infos


logger = logging.getLogger(__name__)


def set_client_attributes():
    prefs = get_mixer_prefs()
    username = prefs.user
    usercolor = prefs.color
    share_data.client.set_client_attributes(
        {ClientAttributes.USERNAME: username, ClientAttributes.USERCOLOR: list(usercolor)}
    )


def create_room(room_name: str, vrtist_protocol: bool = False, shared_folders=None, ignore_version_check: bool = False):
    if ignore_version_check:
        logger.warning("Ignoring version check")
    join_room(room_name, vrtist_protocol, shared_folders, ignore_version_check)


def join_room(room_name: str, vrtist_protocol: bool = False, shared_folders=None, ignore_version_check: bool = False):
    prefs = get_mixer_prefs()
    logger.warning(f"join: room: {room_name}, user: {prefs.user}")

    for line in tech_infos():
        logger.warning(line)

    for line in addon_infos():
        logger.info(line)

    assert share_data.client.current_room is None
    share_data.session_id += 1
    # todo tech debt -> current_room should be set when JOIN_ROOM is received
    # todo _joining_room_name should be set in client timer
    share_data.client.current_room = room_name
    share_data.client._joining_room_name = room_name
    set_client_attributes()
    blender_version = bpy.app.version_string
    mixer_version = mixer.display_version
    share_data.client.join_room(room_name, blender_version, mixer_version, ignore_version_check, not vrtist_protocol)

    if shared_folders is None:
        shared_folders = []
    share_data.init_protocol(vrtist_protocol, shared_folders)
    share_data.pending_test_update = False

    # join a room <==> want to track local changes
    HandlerManager.set_handlers(True)


def leave_current_room():
    logger.info("leave_current_room")

    if share_data.client and share_data.client.current_room:
        share_data.leave_current_room()
        HandlerManager.set_handlers(False)

    share_data.clear_before_state()


def is_joined():
    connected = share_data.client is not None and share_data.client.is_connected()
    return connected and share_data.client.current_room


def wait_for_server(host, port):
    attempts = 0
    max_attempts = 10
    while not create_main_client(host, port) and attempts < max_attempts:
        attempts += 1
        time.sleep(0.2)
    return attempts < max_attempts


def start_local_server():
    import mixer

    dir_path = Path(mixer.__file__).parent.parent  # broadcaster is submodule of mixer

    if get_mixer_prefs().show_server_console:
        args = {"creationflags": subprocess.CREATE_NEW_CONSOLE}
    else:
        args = {}

    share_data.local_server_process = subprocess.Popen(
        [bpy.app.binary_path_python, "-m", "mixer.broadcaster.apps.server", "--port", str(get_mixer_prefs().port)],
        cwd=dir_path,
        shell=False,
        **args,
    )


def is_localhost(host):
    # does not catch local address
    return host == "localhost" or host == "127.0.0.1"


def connect():
    prefs = get_mixer_prefs()
    logger.info(f"connect to {prefs.host}:{prefs.port}")
    if share_data.client is not None:
        # a server shutdown was not processed
        logger.debug("connect: share_data.client is not None")
        share_data.client = None

    if not create_main_client(prefs.host, prefs.port):
        if is_localhost(prefs.host):
            if prefs.no_start_server:
                raise RuntimeError(
                    f"Cannot connect to existing server at {prefs.host}:{prefs.port} and MIXER_NO_START_SERVER environment variable exists"
                )
            start_local_server()
            if not wait_for_server(prefs.host, prefs.port):
                raise RuntimeError("Unable to start local server")
        else:
            raise RuntimeError(f"Unable to connect to remote server {prefs.host}:{prefs.port}")

    assert is_client_connected()

    set_client_attributes()
    HandlerManager._set_connection_handler(True)


def disconnect():
    from mixer.bl_panels import update_ui_lists

    logger.info("disconnect")

    leave_current_room()

    remove_draw_handlers()

    if bpy.app.timers.is_registered(network_consumer_timer):
        bpy.app.timers.unregister(network_consumer_timer)

    # the socket has already been disconnected
    if share_data.client is not None:
        if share_data.client.is_connected():
            share_data.client.disconnect()
        share_data.client = None

    update_ui_lists()
    HandlerManager._set_connection_handler(False)


def is_client_connected():
    return share_data.client is not None and share_data.client.is_connected()


def network_consumer_timer():
    if not share_data.client.is_connected():
        error_msg = "Timer still registered but client disconnected."
        logger.error(error_msg)
        # Returning None from a timer unregister it
        return None

    # Encapsulate call to share_data.client.network_consumer because
    # if we register it directly, then bpy.app.timers.is_registered(share_data.client.network_consumer)
    # return False...
    # However, with a simple function bpy.app.timers.is_registered works.
    try:
        share_data.client.network_consumer()
    except (ClientDisconnectedException, SendSceneContentFailed) as e:
        logger.warning(e)
        share_data.client = None
        disconnect()
        return None
    except Exception as e:
        logger.error(f"{e!r}", stack_info=True)

    # Run every 1 / 100 seconds
    return 0.01


def create_main_client(host: str, port: int):
    if share_data.client is not None:
        # a server shutdown was not processed
        logger.debug("create_main_client: share_data.client is not None")
        share_data.client = None

    client = BlenderClient(host, port)
    client.connect()
    if not client.is_connected():
        return False

    share_data.client = client
    if not bpy.app.timers.is_registered(network_consumer_timer):
        bpy.app.timers.register(network_consumer_timer)

    return True
