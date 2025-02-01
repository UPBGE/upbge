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
This module defines the About panel.
"""

import bpy
from bpy.types import Operator

from mixer import display_version, about_date


class Mixer_OT_About(Operator):  # noqa 801
    bl_idname = "mixer.about"
    bl_label = "About UAS Mixer..."
    bl_description = "More information about UAS Mixer..."
    bl_options = {"INTERNAL"}

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=400)

    def draw(self, context):
        layout = self.layout
        box = layout.box()

        # Version
        ###############
        row = box.row()
        row.separator()
        row.label(
            text=f"Version: {display_version or '(Unknown version)'}   -   {about_date()}   -    Ubisoft Animation Studio"
        )

        # Authors
        ###############
        row = box.row()
        row.separator()
        row.label(text="Written by the R&D team of Ubisoft Animation Studio")

        # Purpose
        ###############
        row = box.row()
        row.label(text="Purpose:")
        row = box.row()
        row.separator()
        col = row.column()
        col.label(text="Mixer provides real time collaboration between several users of Blender,")
        col.label(text="allowing them to work on the same scene at the same time from")
        col.label(text="different computers.")

        # Dependencies
        ###############
        # row = box.row()
        # row.label(text="Dependencies:")
        # row = box.row()
        # row.separator()
        #
        # Documentation
        # ##############
        row = box.row()
        row.label(text="Documentation:")
        row.separator()
        row.operator(
            "mixer.open_documentation_url", text="Documentation, Tutorials..."
        ).path = "https://ubisoft-mixer.readthedocs.io/en/latest/"

        # Download
        # ########
        row = box.row()
        row.label(text="Downloads:")
        row.separator()
        row.operator(
            "mixer.open_documentation_url", text="Latest Release..."
        ).path = "https://github.com/ubisoft/mixer/releases/latest/"

        # Source code
        # ###########
        row = box.row()
        row.label(text="Source Code:")
        row.separator()
        row.operator(
            "mixer.open_documentation_url", text="Mixer on GitHub..."
        ).path = "https://github.com/ubisoft/mixer/"

        box.separator()

        layout.separator(factor=1)

    def execute(self, context):
        return {"FINISHED"}


_classes = (Mixer_OT_About,)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
