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
Script executed on the Blender command line to execute blender_data tests.
"""
import os
from pathlib import Path
import unittest
import xmlrunner

import bpy

this_folder = str(Path(__file__).parent)


def main_ci():

    # despite installing and enabling the addon in install_mixer.py, it does not remain enabled
    bpy.ops.preferences.addon_enable(module="mixer")

    os.makedirs("logs/tests", exist_ok=True)
    with open("logs/tests/blender_data.xml", "wb") as output:
        suite = unittest.defaultTestLoader.discover(this_folder)
        runner = xmlrunner.XMLTestRunner(verbosity=2, output=output)
        result = runner.run(suite)
        if not result.wasSuccessful():
            # exitcode != 0 for gitlab test runner
            raise AssertionError("Tests failed")


if __name__ == "__main__":
    main_ci()
    pass
