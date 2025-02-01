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


class ShotManager:
    def __init__(self):
        self.current_take_name = ""
        self.current_shot_index = -1
        self.montage_mode = None
        self.shots = []


class Shot:
    def __init__(self):
        self.name = ""
        self.camera_name = ""
        self.start = 0
        self.end = 0
        self.enabled = True
