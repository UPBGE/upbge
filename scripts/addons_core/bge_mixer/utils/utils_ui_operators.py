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
This module define Blender Operators types used in the ui of the addon.
"""

import os
from pathlib import Path
import subprocess


from mixer.os_utils import open_folder
import bpy
from bpy.types import Operator
from bpy.props import StringProperty


class Mixer_OT_Open_Documentation_Url(Operator):  # noqa 801
    bl_idname = "mixer.open_documentation_url"
    bl_label = "Open Documentation Web Page"
    bl_description = "Open web page.\nShift + Click: Copy the URL into the clipboard"

    path: StringProperty()

    def invoke(self, context, event):
        if event.shift:
            # copy path to clipboard
            cmd = "echo " + (self.path).strip() + "|clip"
            subprocess.check_call(cmd, shell=True)
        else:
            open_folder(self.path)

        return {"FINISHED"}


class Mixer_OT_Open_Explorer(Operator):  # noqa 801
    bl_idname = "mixer.open_explorer"
    bl_label = "Open Explorer"
    bl_description = "Open an Explorer window located at the directory containing the log files.\nShift + Click: Copy the path into the clipboard"

    path: StringProperty()

    def invoke(self, context, event):
        abs_path = bpy.path.abspath(self.path)
        head, tail = os.path.split(abs_path)
        abs_path = head + os.sep

        if event.shift:
            # copy path to clipboard
            cmd = "echo " + (abs_path).strip() + "|clip"
            subprocess.check_call(cmd, shell=True)

        else:
            if Path(abs_path).exists():
                abs_path = abs_path.replace(os.sep, "/")
                open_folder(abs_path)

            else:
                print(f"Open Explorer failed: Path not found: {Path(abs_path)}")

        return {"FINISHED"}


_classes = (
    Mixer_OT_Open_Documentation_Url,
    Mixer_OT_Open_Explorer,
)


def register():

    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
