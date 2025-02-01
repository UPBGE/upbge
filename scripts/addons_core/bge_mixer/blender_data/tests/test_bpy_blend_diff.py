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

import unittest

from bpy import data as D  # noqa
from bpy import types as T  # noqa

from mixer.blender_data.bpy_data_proxy import BpyDataProxy
from mixer.blender_data.diff import BpyBlendDiff
from mixer.blender_data.filter import test_properties


def sort_renamed_item(x):
    return x[1]


class TestDiff(unittest.TestCase):
    def setUp(self):
        for w in D.worlds:
            D.worlds.remove(w)
        self.proxy = BpyDataProxy()

    def test_create(self):
        # test_diff.TestDiff.test_create
        self.proxy.load(test_properties)
        new_worlds = ["W0", "W1"]
        new_worlds.sort()
        for w in new_worlds:
            D.worlds.new(w)
        diff = BpyBlendDiff()
        diff.diff(self.proxy, test_properties)
        for collection_name, delta in diff.collection_deltas:
            self.assertEqual(0, len(delta.items_removed), f"removed count mismatch for {collection_name}")
            self.assertEqual(0, len(delta.items_renamed), f"renamed count mismatch for {collection_name}")
            if collection_name == "worlds":
                self.assertEqual(len(new_worlds), len(delta.items_added), f"added count mismatch for {collection_name}")
                found = [datablock.name for datablock, _ in delta.items_added]
                found.sort()
                self.assertEqual(new_worlds, found, f"added count mismatch for {collection_name}")
            else:
                self.assertEqual(0, len(delta.items_added), f"added count mismatch for {collection_name}")

    def test_remove(self):
        # test_diff.TestDiff.test_create
        new_worlds = ["W0", "W1", "W2"]
        new_worlds.sort()
        for w in new_worlds:
            D.worlds.new(w)

        self.proxy.load(test_properties)

        removed = ["W0", "W1"]
        removed.sort()
        for w in removed:
            D.worlds.remove(D.worlds[w])

        diff = BpyBlendDiff()
        diff.diff(self.proxy, test_properties)
        for name, delta in diff.collection_deltas:
            self.assertEqual(0, len(delta.items_added), f"added count mismatch for {name}")
            self.assertEqual(0, len(delta.items_renamed), f"renamed count mismatch for {name}")
            if name == "worlds":
                self.assertEqual(len(removed), len(delta.items_removed), f"removed count mismatch for {name}")
                items_removed = [proxy.data("name") for proxy in delta.items_removed]
                items_removed.sort()
                self.assertEqual(removed, items_removed, f"removed count mismatch for {name}")
            else:
                self.assertEqual(0, len(delta.items_added), f"added count mismatch for {name}")

    def test_rename(self):
        # test_diff.TestDiff.test_create
        new_worlds = ["W0", "W1", "W2"]
        new_worlds.sort()
        for w in new_worlds:
            D.worlds.new(w)

        self.proxy.load(test_properties)

        renamed = [("W0", "W00"), ("W2", "W22")]
        renamed.sort(key=sort_renamed_item)
        for old_name, new_name in renamed:
            D.worlds[old_name].name = new_name

        diff = BpyBlendDiff()
        diff.diff(self.proxy, test_properties)
        for name, delta in diff.collection_deltas:
            self.assertEqual(0, len(delta.items_added), f"added count mismatch for {name}")
            self.assertEqual(0, len(delta.items_removed), f"removed count mismatch for {name}")
            if name == "worlds":
                self.assertEqual(len(renamed), len(delta.items_renamed), f"renamed count mismatch for {name}")
                items_renamed = list(delta.items_renamed)
                items_renamed.sort(key=sort_renamed_item)
                items_renamed = [(proxy.data("name"), new_name) for proxy, new_name in items_renamed]
                self.assertEqual(renamed, items_renamed, f"removed count mismatch for {name}")
            else:
                self.assertEqual(0, len(delta.items_added), f"added count mismatch for {name}")

    def test_create_delete_rename(self):
        # test_diff.TestDiff.test_create
        new_worlds = ["W0", "W1", "W2", "W4"]
        new_worlds.sort()
        for w in new_worlds:
            D.worlds.new(w)

        self.proxy.load(test_properties)

        renamed = [("W0", "W00"), ("W2", "W22"), ("W4", "W44")]
        renamed.sort(key=sort_renamed_item)
        for old_name, new_name in renamed:
            D.worlds[old_name].name = new_name

        added = ["W0", "W5"]
        added.sort()
        for w in added:
            D.worlds.new(w)

        removed = ["W1", "W00"]
        removed.sort()
        for w in removed:
            D.worlds.remove(D.worlds[w])

        diff = BpyBlendDiff()
        diff.diff(self.proxy, test_properties)
        for name, delta in diff.collection_deltas:
            if name == "worlds":
                items_added = [datablock.name for datablock, _ in delta.items_added]
                items_added.sort()
                self.assertEqual(items_added, ["W0", "W5"], f"added count mismatch for {name}")

                items_renamed = delta.items_renamed
                items_renamed.sort(key=sort_renamed_item)
                items_renamed = [(proxy.data("name"), new_name) for proxy, new_name in items_renamed]
                self.assertEqual(items_renamed, [("W2", "W22"), ("W4", "W44")], f"renamed count mismatch for {name}")

                items_removed = [proxy.data("name") for proxy in delta.items_removed]
                items_removed.sort()
                self.assertEqual(items_removed, ["W0", "W1"], f"removed count mismatch for {name}")
            else:
                self.assertEqual(0, len(delta.items_renamed), f"renamed count mismatch for {name}")
                self.assertEqual(0, len(delta.items_removed), f"removed count mismatch for {name}")
                self.assertEqual(0, len(delta.items_added), f"added count mismatch for {name}")
