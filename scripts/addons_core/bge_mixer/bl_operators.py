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
This module define Blender Operators types for the addon.
"""

import logging
import os
import socket
import subprocess
import time
from pathlib import Path

import bpy
from bpy_extras.io_utils import ImportHelper

import mixer
from mixer.share_data import share_data
from mixer.bl_utils import get_mixer_props, get_mixer_prefs
from mixer.broadcaster.common import RoomAttributes, ClientAttributes
from mixer.connection import (
    is_client_connected,
    connect,
    create_room,
    join_room,
    leave_current_room,
    disconnect,
    network_consumer_timer,
)

logger = logging.getLogger(__name__)


poll_is_client_connected = (lambda: is_client_connected(), "Client not connected")
poll_already_in_a_room = (lambda: not is_client_connected() or not share_data.client.current_room, "Already in a room")


class SharedFoldersAddFolderOperator(bpy.types.Operator, ImportHelper):
    bl_idname = "mixer.add_shared_folder"
    bl_label = "Add Shared Folder"
    filepath: bpy.props.StringProperty(subtype="DIR_PATH")

    def execute(self, context):

        path = self.filepath
        if not Path(path).is_dir():
            path = str(Path(path).parent)

        for item in get_mixer_prefs().shared_folders:
            if item.shared_folder == path:
                return {"FINISHED"}

        item = get_mixer_prefs().shared_folders.add()
        item.shared_folder = path
        return {"FINISHED"}

    @classmethod
    def poll_functors(cls, context):
        return [
            poll_already_in_a_room,
        ]

    @classmethod
    def poll(cls, context):
        return generic_poll(cls, context)


class SharedFoldersRemoveFolderOperator(bpy.types.Operator):
    bl_idname = "mixer.remove_shared_folder"
    bl_label = "Remove Shared Folder"

    def execute(self, context):
        props = get_mixer_props()
        get_mixer_prefs().shared_folders.remove(props.shared_folder_index)
        return {"FINISHED"}

    @classmethod
    def poll_functors(cls, context):
        return [
            poll_already_in_a_room,
        ]

    @classmethod
    def poll(cls, context):
        return generic_poll(cls, context)


def generic_poll(cls, context):
    for func, _reason in cls.poll_functors(context):
        if not func():
            return False
    return True


def generic_description(cls, context, properties):
    result = cls.__doc__
    for func, reason in cls.poll_functors(context):
        if not func():
            result += f" (Error: {reason})"
            break
    return result


class CreateRoomOperator(bpy.types.Operator):
    """Create a new room on Mixer server with the specified name"""

    bl_idname = "mixer.create_room"
    bl_label = "Create Room"
    bl_options = {"REGISTER"}

    @classmethod
    def poll_functors(cls, context):
        return [
            poll_is_client_connected,
            poll_already_in_a_room,
            (lambda: get_mixer_prefs().room != "", "Room name cannot be empty"),
            (lambda: get_mixer_prefs().room not in share_data.client.rooms_attributes, "Room already exists"),
        ]

    @classmethod
    def poll(cls, context):
        return generic_poll(cls, context)

    @classmethod
    def description(cls, context, properties):
        return generic_description(cls, context, properties)

    def execute(self, context):
        assert share_data.client.current_room is None
        if not is_client_connected():
            return {"CANCELLED"}

        mixer_prefs = get_mixer_prefs()
        room = mixer_prefs.room
        logger.warning(f"CreateRoomOperator.execute({room})")

        shared_folders = []
        for item in mixer_prefs.shared_folders:
            shared_folders.append(item.shared_folder)
        create_room(room, mixer_prefs.vrtist_protocol, shared_folders, mixer_prefs.ignore_version_check)

        return {"FINISHED"}


def get_selected_room_dict():
    room_index = get_mixer_props().room_index
    assert room_index < len(get_mixer_props().rooms)
    return share_data.client.rooms_attributes[get_mixer_props().rooms[room_index].name]


def clear_undo_history():
    # A horrible way to clear the undo stack since we can't do it normally in Blender :(
    count = bpy.context.preferences.edit.undo_steps + 1
    try:
        for _ in range(count):
            bpy.ops.ed.undo_push(message="Mixer clear history")
    except RuntimeError:
        logging.error("Clear history failed")


class JoinRoomOperator(bpy.types.Operator):
    """Join a room"""

    bl_idname = "mixer.join_room"
    bl_label = "Join Room"
    bl_options = {"REGISTER"}

    @classmethod
    def poll_functors(cls, context):
        return [
            poll_is_client_connected,
            poll_already_in_a_room,
            (lambda: get_mixer_props().room_index < len(get_mixer_props().rooms), "Invalid room selection"),
            (
                lambda: get_selected_room_dict().get(RoomAttributes.JOINABLE, False),
                "Room is not joinable, first client has not finished sending initial content.",
            ),
            (
                lambda: get_selected_room_dict().get(RoomAttributes.IGNORE_VERSION_CHECK, False)
                or get_selected_room_dict().get(RoomAttributes.BLENDER_VERSION, "") == bpy.app.version_string,
                "Room is not joinable, blender version mismatch.",
            ),
            (
                lambda: get_selected_room_dict().get(RoomAttributes.IGNORE_VERSION_CHECK, False)
                or get_selected_room_dict().get(RoomAttributes.MIXER_VERSION, "") == mixer.display_version,
                "Room is not joinable, mixer version mismatch.",
            ),
        ]

    @classmethod
    def poll(cls, context):
        return generic_poll(cls, context)

    @classmethod
    def description(cls, context, properties):
        return generic_description(cls, context, properties)

    def execute(self, context):
        assert not share_data.client.current_room
        share_data.set_dirty()

        props = get_mixer_props()
        room_index = props.room_index
        room = props.rooms[room_index].name
        logger.warning(f"JoinRoomOperator.execute({room})")
        room_attributes = get_selected_room_dict()
        logger.warning(f"Client Blender version: {room_attributes.get(RoomAttributes.BLENDER_VERSION, '')}")
        logger.warning(f"Client Mixer version: {room_attributes.get(RoomAttributes.MIXER_VERSION, '')}")

        clear_undo_history()

        mixer_prefs = get_mixer_prefs()
        shared_folders = []
        for item in mixer_prefs.shared_folders:
            shared_folders.append(item.shared_folder)
        join_room(
            room,
            not room_attributes.get(RoomAttributes.GENERIC_PROTOCOL, True),
            shared_folders,
            mixer_prefs.ignore_version_check,
        )

        return {"FINISHED"}


class DeleteRoomOperator(bpy.types.Operator):
    """Delete an empty room"""

    bl_idname = "mixer.delete_room"
    bl_label = "Delete Room"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        room_index = get_mixer_props().room_index
        return (
            is_client_connected()
            and room_index < len(get_mixer_props().rooms)
            and (get_mixer_props().rooms[room_index].users_count == 0)
        )

    def execute(self, context):
        props = get_mixer_props()
        room_index = props.room_index
        room = props.rooms[room_index].name
        share_data.client.delete_room(room)

        return {"FINISHED"}


class DownloadRoomOperator(bpy.types.Operator):
    """Download content of an empty room"""

    bl_idname = "mixer.download_room"
    bl_label = "Download Room"
    bl_options = {"REGISTER"}

    filepath: bpy.props.StringProperty(subtype="FILE_PATH")

    @classmethod
    def poll(cls, context):
        room_index = get_mixer_props().room_index
        return (
            is_client_connected()
            and room_index < len(get_mixer_props().rooms)
            and (get_mixer_props().rooms[room_index].users_count == 0)
        )

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        from mixer.broadcaster.room_bake import download_room, save_room

        prefs = get_mixer_prefs()
        props = get_mixer_props()
        room_index = props.room_index
        room = props.rooms[room_index].name
        protocol = props.rooms[room_index].protocol
        attributes, commands = download_room(
            prefs.host, prefs.port, room, bpy.app.version_string, mixer.display_version, protocol == "Generic"
        )
        save_room(attributes, commands, self.filepath)

        return {"FINISHED"}


class UploadRoomOperator(bpy.types.Operator):
    """Upload content of an empty room"""

    bl_idname = "mixer.upload_room"
    bl_label = "Upload Room"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        mixer_props = get_mixer_props()
        return (
            is_client_connected()
            and os.path.exists(mixer_props.upload_room_filepath)
            and mixer_props.upload_room_name not in share_data.client.rooms_attributes
        )

    def execute(self, context):
        from mixer.broadcaster.room_bake import load_room, upload_room

        prefs = get_mixer_prefs()
        props = get_mixer_props()

        attributes, commands = load_room(props.upload_room_filepath)
        upload_room(prefs.host, prefs.port, props.upload_room_name, attributes, commands)

        return {"FINISHED"}


class LeaveRoomOperator(bpy.types.Operator):
    """Leave the current room"""

    bl_idname = "mixer.leave_room"
    bl_label = "Leave Room"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        return is_client_connected() and share_data.client.current_room is not None

    def execute(self, context):
        from mixer.bl_panels import update_ui_lists

        leave_current_room()
        update_ui_lists()
        return {"FINISHED"}


class ConnectOperator(bpy.types.Operator):
    """Connect to the Mixer server"""

    bl_idname = "mixer.connect"
    bl_label = "Connect to server"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        return not is_client_connected()

    def execute(self, context):
        prefs = get_mixer_prefs()
        try:
            self.report({"INFO"}, f'Connecting to "{prefs.host}:{prefs.port}" ...')
            try:
                connect()
            except Exception as e:
                self.report({"ERROR"}, f"mixer.connect error : {e!r}")
                return {"CANCELLED"}

            self.report({"INFO"}, f'Connected to "{prefs.host}:{prefs.port}" ...')
        except socket.gaierror:
            msg = f'Cannot connect to "{prefs.host}": invalid host name or address'
            self.report({"ERROR"}, msg)
            return {"CANCELLED"}
        except Exception as e:
            self.report({"ERROR"}, repr(e))
            return {"CANCELLED"}

        return {"FINISHED"}


class DisconnectOperator(bpy.types.Operator):
    """Disconnect from the Mixer server"""

    bl_idname = "mixer.disconnect"
    bl_label = "Disconnect from server"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        return is_client_connected()

    def execute(self, context):
        disconnect()
        self.report({"INFO"}, "Disconnected ...")
        return {"FINISHED"}


class LaunchVRtistOperator(bpy.types.Operator):
    """Launch a VRtist instance"""

    bl_idname = "vrtist.launch"
    bl_label = "Launch VRtist"
    bl_options = {"REGISTER"}

    vrtist_process = None

    @classmethod
    def poll(cls, context):
        # Check VRtist process to auto disconnect
        if cls.vrtist_process is not None and cls.vrtist_process.poll() is not None:
            cls.vrtist_process = None
            leave_current_room()
            disconnect()

        # Manage button state
        return os.path.isfile(get_mixer_prefs().VRtist)

    def execute(self, context):
        bpy.data.window_managers["WinMan"].mixer.send_bake_meshes = True

        mixer_prefs = get_mixer_prefs()
        if not share_data.client or not share_data.client.current_room:
            timeout = 10
            try:
                connect()
            except Exception as e:
                self.report({"ERROR"}, f"vrtist.launch connect error : {e!r}")
                return {"CANCELLED"}

            # Wait for local server creation
            while timeout > 0 and not is_client_connected():
                time.sleep(0.5)
                timeout -= 0.5
            if timeout <= 0:
                self.report({"ERROR"}, "vrtist.launch connect error : unable to connect")
                return {"CANCELLED"}

            logger.warning("LaunchVRtistOperator.execute({mixer_prefs.room})")
            shared_folders = []
            for item in mixer_prefs.shared_folders:
                shared_folders.append(item.shared_folder)
            mixer_prefs.ignore_version_check = True
            join_room(mixer_prefs.room, True, shared_folders, mixer_prefs.ignore_version_check)

            # Wait for room creation/join
            timeout = 10
            while timeout > 0 and share_data.client.current_room is None:
                time.sleep(0.5)
                timeout -= 0.5
            if timeout <= 0:
                self.report({"ERROR"}, "vrtist.launch connect error : unable to join room")
                return {"CANCELLED"}

            # Wait for client id
            timeout = 10
            while timeout > 0 and share_data.client.client_id is None:
                network_consumer_timer()
                time.sleep(0.1)
                timeout -= 0.1
            if timeout <= 0:
                self.report({"ERROR"}, "vrtist.launch connect error : unable to retrieve client id")
                return {"CANCELLED"}

        color = share_data.client.clients_attributes[share_data.client.client_id].get(
            ClientAttributes.USERCOLOR, (0.0, 0.0, 0.0)
        )
        color = (int(c * 255) for c in color)
        color = "#" + "".join(f"{c:02x}" for c in color)
        name = "VR " + share_data.client.clients_attributes[share_data.client.client_id].get(
            ClientAttributes.USERNAME, "client"
        )

        args = [
            mixer_prefs.VRtist,
            "--room",
            share_data.client.current_room,
            "--hostname",
            mixer_prefs.host,
            "--port",
            str(mixer_prefs.port),
            "--master",
            str(share_data.client.client_id),
            "--usercolor",
            color,
            "--username",
            name,
            "--startScene",
            os.path.split(bpy.data.filepath)[1],
        ]
        LaunchVRtistOperator.vrtist_process = subprocess.Popen(
            args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, shell=False
        )
        return {"FINISHED"}


class ToggleBetweenMixerAndVRtistPanels(bpy.types.Operator):
    bl_idname = "mixervrtist.toggle"
    bl_label = "Mixer / VRtist Panels"
    bl_description = "Toggle Between Mixer and VRtist Panels"
    bl_options = {"INTERNAL"}

    mixer_desciption = """Toggle from VRtist to Mixer panel.
Mixer offers a collaborative real-time environment for Blender users.
\nSee the documentation for more information"""
    vrtist_desciption = """Toggle from Mixer to VRtist panel.
This panel will allow you to set up a live link between Blender and VRtist,
a VR application developed by Ubisoft Animation Studio for immersive animation direction.
\nSee the documentation for more information"""
    panel_mode: bpy.props.StringProperty(default="MIXER")

    @classmethod
    def description(cls, context, properties):
        descr = "Toggle between Mixer and VRtist panels.\n"
        if "MIXER" == properties.panel_mode:
            descr = cls.mixer_desciption
        elif "VRTIST" == properties.panel_mode:
            descr = cls.vrtist_desciption
        return descr

    def invoke(self, context, event):
        mixer_prefs = get_mixer_prefs()
        mixer_prefs.display_mixer_vrtist_panels = self.panel_mode
        return {"FINISHED"}


classes = (
    LaunchVRtistOperator,
    CreateRoomOperator,
    ConnectOperator,
    DisconnectOperator,
    JoinRoomOperator,
    DeleteRoomOperator,
    LeaveRoomOperator,
    DownloadRoomOperator,
    UploadRoomOperator,
    SharedFoldersAddFolderOperator,
    SharedFoldersRemoveFolderOperator,
    ToggleBetweenMixerAndVRtistPanels,
)

register_factory, unregister_factory = bpy.utils.register_classes_factory(classes)


def register():
    register_factory()


def unregister():
    disconnect()
    unregister_factory()
