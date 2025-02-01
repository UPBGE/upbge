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

from mixer.blender_data.bpy_data_proxy import BpyDataProxy
from mixer.blender_data.datablock_proxy import DatablockProxy
from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy
from mixer.blender_data.filter import test_properties
from mixer.blender_data.json_codec import Codec
from mixer.blender_data.tests.utils import register_bl_equals, test_blend_file


class TestCodec(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.open_mainfile(filepath=test_blend_file)
        self.proxy = BpyDataProxy()
        register_bl_equals(self, test_properties)

    def test_camera(self):
        # test_codec.TestCodec.test_camera

        # prepare camera
        transmit_name = "transmit_camera"
        cam_sent = D.cameras["Camera_0"]

        cam_sent.dof.focus_object = D.objects["Cube"]

        # load into proxy
        self.proxy.load(test_properties)

        # patch the name so that it does not get mixed up as we restore later in the same scene
        cam_proxy_sent = self.proxy.data("cameras").search_one("Camera_0")
        cam_proxy_sent._data["name"] = transmit_name
        self.assertIsInstance(cam_proxy_sent, DatablockProxy)

        # encode
        codec = Codec()
        message = codec.encode(cam_proxy_sent)

        #
        # transmit
        #

        # decode into proxy
        cam_proxy_received = codec.decode(message)

        focus_object_proxy = cam_proxy_received.data("dof").data("focus_object")
        self.assertIsInstance(focus_object_proxy, DatablockRefProxy)
        self.assertEqual(focus_object_proxy._datablock_uuid, cam_sent.dof.focus_object.mixer_uuid)

        # save into blender
        cam_proxy_received._datablock_uuid = "__" + cam_proxy_received._datablock_uuid
        cam_received, _ = cam_proxy_received.create_standalone_datablock(self.proxy.context())

        self.assertEqual(cam_sent, cam_received)
        pass

    # TODO Generic test with randomized samples of all IDs ?
