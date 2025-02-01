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
This module defines the Preference menu of VRtist.
"""

import bpy
from bpy.types import Menu


#############
# Preferences
#############


class VRTIST_MT_prefs_main_menu(Menu):  # noqa 801
    bl_idname = "VRTIST_MT_prefs_main_menu"
    bl_label = "VRtist Settings"

    def draw(self, context):
        layout = self.layout

        row = layout.row(align=True)
        row.operator("preferences.addon_show", text="Add-on Preferences...").module = "mixer"

        row = layout.row(align=True)
        row.operator("mixer.open_documentation_url", text="Documentation").path = "https://github.com/ubisoft/vrtist"

        row = layout.row(align=True)
        row.operator("vrtist.about", text="About...")

        layout.separator()
        row = layout.row(align=True)

        from mixer import icons

        icon = icons.icons_col["Mixer_32"]
        row.operator("mixervrtist.toggle", text="Switch to Mixer Panel", icon_value=icon.icon_id).panel_mode = "MIXER"


_classes = (VRTIST_MT_prefs_main_menu,)


def register():

    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
