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
Utility functions that may require os/platform specific adjustments
"""

import getpass
import os
import platform
import subprocess
from pathlib import Path
import sys
from typing import List

import mixer

import bpy
import addon_utils


def getuser() -> str:
    try:
        return getpass.getuser()
    except Exception:
        return os.getlogin()


def tech_infos() -> List[str]:
    lines = [
        f"Platform : {platform.platform()}",
        f"Blender  : {bpy.app.version_string}",
    ]

    date = getattr(mixer, "version_date", None)
    date_string = f"({date})" if date is not None else ""
    lines.append(f"Mixer    : {mixer.display_version} {date_string}")
    return lines


def addon_infos() -> List[str]:
    lines = []
    for bl_module in addon_utils.modules():
        name = bl_module.bl_info["name"]
        module = sys.modules.get(bl_module.__name__)
        enabled = module is not None and getattr(module, "__addon_enabled__", False)
        if enabled:
            version = bl_module.bl_info.get("version", (-1, -1, -1))
            lines.append(f"Addon    : {name} {version}")

    return lines


def open_folder(path):
    """
    Open a path or an URL with the application specified by the os
    """
    if sys.platform == "darwin":
        subprocess.check_call(["open", "--", path])
    elif sys.platform == "linux":
        subprocess.check_call(["xdg-open", path])
    elif sys.platform == "win32":
        subprocess.Popen(f'explorer "{Path(path)}"')
