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
This module defines Blender Property types for the addon.
"""

import logging

import bpy

from mixer import display_version
from mixer.broadcaster.common import RoomAttributes
from mixer.os_utils import getuser
from mixer.share_data import share_data

logger = logging.getLogger(__name__)


class RoomItem(bpy.types.PropertyGroup):
    def has_warnings(self):
        return bpy.app.version_string != self.blender_version or display_version != self.mixer_version

    def get_room_blender_version(self):
        if (
            share_data.client is not None
            and self.name in share_data.client.rooms_attributes
            and "blender_version" in share_data.client.rooms_attributes[self.name]
        ):
            return share_data.client.rooms_attributes[self.name]["blender_version"]
        return ""

    def get_room_mixer_version(self):
        if (
            share_data.client is not None
            and self.name in share_data.client.rooms_attributes
            and "mixer_version" in share_data.client.rooms_attributes[self.name]
        ):
            return share_data.client.rooms_attributes[self.name]["mixer_version"]
        return ""

    def is_ignore_version_check(self):
        if (
            share_data.client is not None
            and self.name in share_data.client.rooms_attributes
            and "ignore_version_check" in share_data.client.rooms_attributes[self.name]
        ):
            return share_data.client.rooms_attributes[self.name]["ignore_version_check"]
        return False

    def get_protocol(self):
        if (
            share_data.client is not None
            and self.name in share_data.client.rooms_attributes
            and "generic_protocol" in share_data.client.rooms_attributes[self.name]
        ):
            return "Generic" if share_data.client.rooms_attributes[self.name]["generic_protocol"] else "VRtist"
        return ""

    def is_kept_open(self):
        if (
            share_data.client is not None
            and self.name in share_data.client.rooms_attributes
            and RoomAttributes.KEEP_OPEN in share_data.client.rooms_attributes[self.name]
        ):
            return share_data.client.rooms_attributes[self.name][RoomAttributes.KEEP_OPEN]
        return False

    def on_keep_open_changed(self, value):
        share_data.client.set_room_keep_open(self.name, value)
        return None

    def get_command_count(self):
        if (
            share_data.client is not None
            and self.name in share_data.client.rooms_attributes
            and RoomAttributes.COMMAND_COUNT in share_data.client.rooms_attributes[self.name]
        ):
            return share_data.client.rooms_attributes[self.name][RoomAttributes.COMMAND_COUNT]
        return 0

    def get_mega_byte_size(self):
        if (
            share_data.client is not None
            and self.name in share_data.client.rooms_attributes
            and RoomAttributes.BYTE_SIZE in share_data.client.rooms_attributes[self.name]
        ):
            return share_data.client.rooms_attributes[self.name][RoomAttributes.BYTE_SIZE] * 1e-6
        return 0

    def is_joinable(self):
        if (
            share_data.client is not None
            and self.name in share_data.client.rooms_attributes
            and RoomAttributes.JOINABLE in share_data.client.rooms_attributes[self.name]
        ):
            return share_data.client.rooms_attributes[self.name][RoomAttributes.JOINABLE]
        return False

    name: bpy.props.StringProperty(name="Name", description="Room name")
    blender_version: bpy.props.StringProperty(
        name="Blender Version",
        description="Version of the Blender instance from which the room has been created",
        get=get_room_blender_version,
    )
    mixer_version: bpy.props.StringProperty(
        name="Mixer Version",
        description="Version of the Mixer instance from which the room has been created",
        get=get_room_mixer_version,
    )
    ignore_version_check: bpy.props.BoolProperty(name="Ignore Version Check", get=is_ignore_version_check)
    users_count: bpy.props.IntProperty(name="Users Count")
    protocol: bpy.props.StringProperty(name="Protocol", get=get_protocol)
    keep_open: bpy.props.BoolProperty(
        name="Keep Open",
        description="Indicate if the room should be kept on the server when no more client is inside",
        default=False,
        get=is_kept_open,
        set=on_keep_open_changed,
    )
    command_count: bpy.props.IntProperty(name="Command Count", get=get_command_count)
    mega_byte_size: bpy.props.FloatProperty(name="Mega Byte Size", get=get_mega_byte_size)
    joinable: bpy.props.BoolProperty(name="Joinable", get=is_joinable)


class UserWindowItem(bpy.types.PropertyGroup):
    scene: bpy.props.StringProperty(name="Scene")
    view_layer: bpy.props.StringProperty(name="View Layer")
    screen: bpy.props.StringProperty(name="Screen")
    areas_3d_count: bpy.props.IntProperty(name="3D Areas Count")


class UserSceneItem(bpy.types.PropertyGroup):
    scene: bpy.props.StringProperty(name="Scene")
    frame: bpy.props.IntProperty(name="Frame")


class UserItem(bpy.types.PropertyGroup):
    is_me: bpy.props.BoolProperty(name="Is Me")
    name: bpy.props.StringProperty(name="Name")
    ip: bpy.props.StringProperty(name="IP")
    port: bpy.props.IntProperty(name="Port")
    ip_port: bpy.props.StringProperty(name="IP:Port")
    room: bpy.props.StringProperty(name="Room")
    internal_color: bpy.props.FloatVectorProperty(
        name="Color",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=1.0,
        precision=2,
    )
    mode: bpy.props.StringProperty(name="User Mode")

    def _get_color(self):
        return self.internal_color

    def _set_color(self, value):
        self["color"] = self.internal_color

    color: bpy.props.FloatVectorProperty(
        name="User Color",
        description="Color associated to the user in the 3D viewport.\nCan only be modified by the user himself",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=1.0,
        precision=2,
        get=_get_color,
        set=_set_color,
    )

    windows: bpy.props.CollectionProperty(name="Windows", type=UserWindowItem)
    selected_window_index: bpy.props.IntProperty(name="Window Index")
    scenes: bpy.props.CollectionProperty(name="Scenes", type=UserSceneItem)


class SharedFolderItem(bpy.types.PropertyGroup):
    shared_folder: bpy.props.StringProperty(default="", subtype="DIR_PATH", name="Shared Folder")


class MixerProperties(bpy.types.PropertyGroup):
    """
    Main Property class, registered on the WindowManager.
    Store non-persistent options and data to be displayed on the UI.
    """

    rooms: bpy.props.CollectionProperty(name="Rooms", type=RoomItem)
    room_index: bpy.props.IntProperty()  # index in the list of rooms

    # user list of the selected or connected room, according to status
    users: bpy.props.CollectionProperty(name="Users", type=UserItem)
    user_index: bpy.props.IntProperty()  # index in the list of users

    display_shared_folders_options: bpy.props.BoolProperty(default=False)
    display_gizmos_options: bpy.props.BoolProperty(default=True)
    display_advanced_options: bpy.props.BoolProperty(default=False)
    display_developer_options: bpy.props.BoolProperty(default=False)
    display_rooms: bpy.props.BoolProperty(default=True)
    display_rooms_details: bpy.props.BoolProperty(
        default=False, name="Display Rooms Details in the Server Rooms List Panel"
    )
    display_users: bpy.props.BoolProperty(default=True)

    display_users_filter: bpy.props.EnumProperty(
        name="Display Users Filter",
        description="Display users filter",
        items=[
            ("all", "All", "", 0),
            ("current_room", "Current Room", "", 1),
            ("selected_room", "Selected Room", "", 2),
            ("no_room", "No Room", "", 3),
        ],
        default="all",
    )
    display_users_details: bpy.props.BoolProperty(default=False, name="Display Users Details")

    shared_folders: bpy.props.CollectionProperty(name="Shared Folders", type=SharedFolderItem)
    shared_folder_index: bpy.props.IntProperty()

    display_advanced_room_control: bpy.props.BoolProperty(default=False)
    upload_room_name: bpy.props.StringProperty(default=f"{getuser()}_uploaded_room", name="Upload Room Name")

    internal_upload_room_filepath: bpy.props.StringProperty(
        default="",
        subtype="FILE_PATH",
        name="Upload Room File",
    )

    def _get_room_filepath(self):
        return bpy.path.abspath(self.internal_upload_room_filepath)

    upload_room_filepath: bpy.props.StringProperty(
        default="",
        subtype="FILE_PATH",
        name="Upload Room File",
        get=_get_room_filepath,
    )

    joining_percentage: bpy.props.FloatProperty(default=0, name="Joining Percentage")


classes = (
    RoomItem,
    UserWindowItem,
    UserSceneItem,
    UserItem,
    SharedFolderItem,
    MixerProperties,
)

register_factory, unregister_factory = bpy.utils.register_classes_factory(classes)


def register():
    register_factory()
    bpy.types.WindowManager.mixer = bpy.props.PointerProperty(type=MixerProperties)


def unregister():
    del bpy.types.WindowManager.mixer
    unregister_factory()
