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

import bpy
from bpy import data as D  # noqa
from bpy import types as T  # noqa
from mixer.blender_data.json_codec import Codec
from mixer.blender_data.bpy_data_proxy import BpyDataProxy
from mixer.blender_data.datablock_proxy import DatablockProxy
from mixer.blender_data.struct_proxy import StructProxy
from mixer.blender_data.tests.utils import register_bl_equals, test_blend_file

from mixer.blender_data.filter import safe_properties
from mixer.blender_data.diff import BpyBlendDiff


class TestWorld(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.open_mainfile(filepath=test_blend_file)
        self.bpy_data_proxy = BpyDataProxy()
        self.diff = BpyBlendDiff()
        bpy.data.worlds[0].name = "World"
        register_bl_equals(self, safe_properties)

    def test_world(self):
        # test_end_to_end.TestWorld.test_world
        world = bpy.data.worlds[0]
        world.use_nodes = True
        self.assertGreaterEqual(len(world.node_tree.nodes), 2)

        self.diff.diff(self.bpy_data_proxy, safe_properties)
        sent_ids = {}
        sent_ids.update({("worlds", world.name): world})

        changeset = self.bpy_data_proxy.update(self.diff, {}, False, safe_properties)
        updates = changeset.creations
        # avoid clash on restore
        world.name = world.name + "_bak"

        codec = Codec()
        for update in updates:
            key = (update.collection_name, update.data("name"))
            sent_id = sent_ids.get(key)
            if sent_id is None:
                continue

            # pretend it is a new one
            update._datablock_uuid += "_new"

            encoded = codec.encode(update)
            # sender side
            #######################
            # receiver side
            decoded = codec.decode(encoded)
            created, _ = self.bpy_data_proxy.create_datablock(decoded)
            self.assertEqual(created, sent_id)

    def test_non_existing(self):
        # test_end_to_end.TestWorld.test_non_existing
        world = bpy.data.worlds[0]

        self.diff.diff(self.bpy_data_proxy, safe_properties)
        sent_ids = {}
        sent_ids.update({("worlds", world.name): world})

        changeset = self.bpy_data_proxy.update(self.diff, {}, False, safe_properties)
        creations = changeset.creations
        # avoid clash on restore
        world.name = world.name + "_bak"

        codec = Codec()
        for update in creations:
            key = (update.collection_name, update.data("name"))
            sent_id = sent_ids.get(key)
            if sent_id is None:
                continue

            # pretend it is a new one
            update._datablock_uuid += "_new"

            # create a property on the send proxy and test that is does not fail on the receiver
            # property on ID
            update._data["does_not_exist_property"] = ""
            update._data["does_not_exist_struct"] = StructProxy()
            update._data["does_not_exist_ID"] = DatablockProxy()

            encoded = codec.encode(update)
            # sender side
            #######################
            # receiver side
            decoded = codec.decode(encoded)
            created, _ = self.bpy_data_proxy.create_datablock(decoded)
            self.assertEqual(created, sent_id)
