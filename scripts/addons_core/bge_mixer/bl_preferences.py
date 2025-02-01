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
This module defines Blender Preferences for the addon.
"""

import os
import logging
import random

import bpy

from mixer.bl_panels import draw_preferences_ui, update_panels_category
from mixer.broadcaster import common
from mixer.broadcaster.common import ClientAttributes
from mixer.os_utils import getuser
from mixer.share_data import share_data
from mixer.local_data import get_data_directory

logger = logging.getLogger(__name__)


def gen_random_color():
    r = random.random()
    g = random.random()
    b = random.random()
    return [r, g, b]


def set_log_level(self, value):
    logging.getLogger(__package__).setLevel(value)
    logger.log(value, "Logging level changed")


class SharedFolderItem(bpy.types.PropertyGroup):
    shared_folder: bpy.props.StringProperty(default="", subtype="DIR_PATH", name="Shared Folder")


class MixerPreferences(bpy.types.AddonPreferences):
    """
    Preferences class, store persistent properties and options.

    Note for developers using blender-vscode - when an addon is disabled, its preferences are erased, so you will
    loose them regularly while developing with hot-reload.
    A possible solution is to make the addon fully reloadable like described here https://developer.blender.org/T67387#982929
    and avoid using hot-reload of blender-vscode.
    A task exists to support keeping preferences of disabled add-ons: https://developer.blender.org/T71486
    """

    bl_idname = __package__

    def on_user_changed(self, context):
        client = share_data.client
        if client and client.is_connected():
            client.set_client_attributes({ClientAttributes.USERNAME: self.user})

    def on_user_color_changed(self, context):
        client = share_data.client
        if client and client.is_connected():
            client.set_client_attributes({ClientAttributes.USERCOLOR: list(self.color)})

    category: bpy.props.StringProperty(
        name="Tab Category",
        description="Choose a name for the category of the panel.",
        default=os.environ.get("MIXER_CATEGORY", "Mixer"),
        update=update_panels_category,
    )

    vrtist_category: bpy.props.StringProperty(
        name="Tab Category",
        description="VRtist Panel.",
        default=os.environ.get("VRTIST_CATEGORY", "VRtist"),
        update=update_panels_category,
    )

    display_mixer_vrtist_panels: bpy.props.EnumProperty(
        name="Display Mixer and VRtist Panels",
        description="Control which panel is displayed between Mixer and VRtist",
        items=[
            ("MIXER", "Mixer", ""),
            ("VRTIST", "VRtit", ""),
            ("MIXER_AND_VRTIST", "Mixer And VRtist", ""),
        ],
        default="MIXER",
    )

    host: bpy.props.StringProperty(
        name="Host", description="Server Host Name", default=os.environ.get("VRTIST_HOST", common.DEFAULT_HOST)
    )
    port: bpy.props.IntProperty(
        name="Port",
        description="Port to use to connect the server host",
        default=int(os.environ.get("VRTIST_PORT", common.DEFAULT_PORT)),
    )
    room: bpy.props.StringProperty(
        name="Room", description="Name of the session room", default="RM_" + os.environ.get("VRTIST_ROOM", getuser())
    )

    # User name as displayed in peers user list
    user: bpy.props.StringProperty(
        name="User Name",
        description="Name by which the other users will identify you during\na cooperative session",
        default=getuser(),
        update=on_user_changed,
    )
    color: bpy.props.FloatVectorProperty(
        name="User Color",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=1.0,
        precision=2,
        description="Color used in the viewport of the cooperative session\nto differenciate you from the other users",
        default=gen_random_color(),
        update=on_user_color_changed,
    )

    display_selected_room_properties: bpy.props.BoolProperty(
        name="Room Properties", description="Display the properties of the selected room", default=False
    )
    users_list_panel_opened: bpy.props.BoolProperty(
        name="Users List", description="Display the list of the users in the selected room", default=True
    )

    def get_log_level(self):
        return logging.getLogger(__package__).level

    log_level: bpy.props.EnumProperty(
        name="Log Level",
        description="Logging level to use",
        items=[
            ("ERROR", "Error", "", logging.ERROR),
            ("WARNING", "Warning", "", logging.WARNING),
            ("INFO", "Info", "", logging.INFO),
            ("DEBUG", "Debug", "", logging.DEBUG),
        ],
        set=set_log_level,
        get=get_log_level,
    )

    vrtist_protocol: bpy.props.BoolProperty(
        name="VRtist Protocol", default=os.environ.get("MIXER_VRTIST_PROTOCOL") == "0"
    )

    ignore_version_check: bpy.props.BoolProperty(default=False, name="Ignore Room Version Check")

    show_server_console: bpy.props.BoolProperty(name="Show Server Console", default=False)

    VRtist: bpy.props.StringProperty(
        name="VRtist", default=os.environ.get("VRTIST_EXE", "D:/unity/VRtist/Build/VRtist.exe"), subtype="FILE_PATH"
    )
    VRtist_suffix: bpy.props.StringProperty(name="VRtist_suffix", default="_VRtist")

    data_directory: bpy.props.StringProperty(
        name="Data Directory", default=os.environ.get("MIXER_DATA_DIR", get_data_directory()), subtype="FILE_PATH"
    )

    shared_folders: bpy.props.CollectionProperty(name="Shared Folders", type=SharedFolderItem)

    # Developer option to avoid sending scene content to server at the first connexion
    # Allow to quickly iterate debugging/test on large scenes with only one client in room
    # Main usage: optimization of client timers to check if updates are required
    no_send_scene_content: bpy.props.BoolProperty(name="Do Not Send Scene Content", default=False)
    no_start_server: bpy.props.BoolProperty(
        name="Do Not Start Server on Connect", default=os.environ.get("MIXER_NO_START_SERVER") is not None
    )
    send_base_meshes: bpy.props.BoolProperty(default=True)
    send_baked_meshes: bpy.props.BoolProperty(default=True)

    display_own_gizmos: bpy.props.BoolProperty(default=False, name="Display Own Gizmos")
    display_ids_gizmos: bpy.props.BoolProperty(default=False, name="Display ID Gizmos")
    display_debugging_tools: bpy.props.BoolProperty(default=False, name="Display Debugging Tools")

    display_frustums_gizmos: bpy.props.BoolProperty(default=True, name="Display Frustums Gizmos")
    display_frustums_names_gizmos: bpy.props.BoolProperty(default=True, name="Display Frustums User Names")
    display_selections_gizmos: bpy.props.BoolProperty(default=True, name="Display Selection Gizmos")
    display_selections_names_gizmos: bpy.props.BoolProperty(default=True, name="Display Selection User Names")

    commands_send_interval: bpy.props.FloatProperty(
        name="Command Send Interval",
        description="Debug tool to specify a number of seconds to wait between each command emission toward the server.",
        default=0,
    )

    def draw(self, context):
        draw_preferences_ui(self, context)


classes = (
    SharedFolderItem,
    MixerPreferences,
)

register_factory, unregister_factory = bpy.utils.register_classes_factory(classes)


def register():
    register_factory()


def unregister():
    unregister_factory()
