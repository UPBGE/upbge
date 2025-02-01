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
This modules defines utilities related to Blender.
"""

from __future__ import annotations
from typing import TYPE_CHECKING
import bpy

if TYPE_CHECKING:
    from mixer.bl_properties import MixerProperties
    from mixer.bl_preferences import MixerPreferences


def get_mixer_props() -> MixerProperties:
    return bpy.context.window_manager.mixer


def get_mixer_prefs() -> MixerPreferences:
    return bpy.context.preferences.addons[__package__].preferences
