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

import copy
from typing import Iterable, Set
import unittest

import bpy
from bpy import data as D  # noqa
from bpy import types as T  # noqa

from mixer.blender_data.aos_soa_proxy import SoaElement
from mixer.blender_data.bpy_data_proxy import BpyDataProxy
from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy
from mixer.blender_data.misc_proxies import NonePtrProxy
from mixer.blender_data.filter import (
    property_order,
    SynchronizedProperties,
    TypeFilterOut,
    test_properties,
    test_filter,
)
from mixer.blender_data.tests.utils import test_blend_file
from mixer.blender_data.specifics import dispatch_rna


class TestLoadProxy(unittest.TestCase):
    # test_misc.TestLoadProxy
    def setUp(self):
        file = test_blend_file
        # file = r"D:\work\data\test_files\BlenderSS 2_82.blend"
        bpy.ops.wm.open_mainfile(filepath=file)
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)

    def check(self, item, expected_elements):
        self.assertSetEqual(set(item._data.keys()), set(expected_elements))

    def expected_uuids(self, collection: bpy.types.Collection, names: Iterable[str]) -> Set[str]:
        return {collection[name].mixer_uuid for name in names}

    # @unittest.skip("")
    def test_blenddata(self):
        # test_misc.TestLoadProxy.test_blenddata
        blend_data = self.proxy._data
        expected_data = {"scenes", "collections", "objects", "materials", "lights"}
        self.assertTrue(all([e in blend_data.keys() for e in expected_data]))

        expected_uuids = self.expected_uuids(bpy.data.scenes, ["Scene_0", "Scene_1"])
        self.check(self.proxy._data["scenes"], expected_uuids)

        expected_uuids = self.expected_uuids(bpy.data.cameras, ["Camera_0", "Camera_1"])
        self.check(self.proxy._data["cameras"], expected_uuids)

        expected_uuids = self.expected_uuids(
            bpy.data.objects, ["Camera_obj_0", "Camera_obj_1", "Cone", "Cube", "Light"]
        )
        self.check(self.proxy._data["objects"], expected_uuids)

        expected_uuids = self.expected_uuids(
            bpy.data.collections, ["Collection_0_0", "Collection_0_1", "Collection_0_0_0", "Collection_1_0"]
        )
        self.check(self.proxy._data["collections"], expected_uuids)

    def test_blenddata_filtered(self):
        blend_data = self.proxy._data
        scene = blend_data["scenes"].search_one("Scene_0")._data
        self.assertTrue("eevee" in scene)

        filter_stack = copy.copy(test_filter)
        filter_stack.append({T.Scene: [TypeFilterOut(T.SceneEEVEE)]})
        proxy = BpyDataProxy()
        proxy.load(SynchronizedProperties(filter_stack, property_order))
        blend_data_ = proxy._data
        scene_ = blend_data_["scenes"].search_one("Scene_0")._data
        self.assertFalse("eevee" in scene_)

    def test_collections(self):
        # test_misc.TestLoadProxy.test_collections
        collections = self.proxy._data["collections"]
        coll_0_0 = collections.search_one("Collection_0_0")._data

        coll_0_0_children = coll_0_0["children"]

        expected_uuids = self.expected_uuids(bpy.data.collections, ["Collection_0_0_0"])
        self.check(coll_0_0_children, expected_uuids)
        for c in coll_0_0_children._data.values():
            self.assertIsInstance(c, DatablockRefProxy)

        coll_0_0_objects = coll_0_0["objects"]
        expected_uuids = self.expected_uuids(bpy.data.objects, ["Camera_obj_0", "Camera_obj_1", "Cube", "Light"])
        self.check(coll_0_0_objects, expected_uuids)
        for o in coll_0_0_objects._data.values():
            self.assertIsInstance(o, DatablockRefProxy)

    def test_camera_focus_object_idref(self):
        # test_misc.TestLoadProxy.test_camera_focus_object_idref
        cam = D.cameras["Camera_0"]
        cam.dof.focus_object = D.objects["Cube"]
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        # load into proxy
        cam_proxy = self.proxy.data("cameras").search_one("Camera_0")
        focus_object_proxy = cam_proxy.data("dof").data("focus_object")
        self.assertIsInstance(focus_object_proxy, DatablockRefProxy)
        self.assertEqual(focus_object_proxy._datablock_uuid, D.objects["Cube"].mixer_uuid)

    def test_camera_focus_object_none(self):
        # test_misc.TestLoadProxy.test_camera_focus_object_none
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        # load into proxy
        cam_proxy = self.proxy.data("cameras").search_one("Camera_0")
        focus_object_proxy = cam_proxy.data("dof").data("focus_object")
        self.assertIsInstance(focus_object_proxy, NonePtrProxy)


class TestAosSoa(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.open_mainfile(filepath=test_blend_file)

    def test_all_soa_grease_pencil(self):

        # test_misc.TestAosSoa.test_all_soa_grease_pencil
        import array

        bpy.ops.object.gpencil_add(type="STROKE")
        proxy = BpyDataProxy()
        proxy.load(test_properties)
        gp = proxy.data("grease_pencils").search_one("Stroke")
        gp_layer_lines = gp.data("layers").data(1)
        gp_points = gp_layer_lines.data("frames").data(0).data("strokes").data(0).data("points")._data
        expected = (
            ("co", array.array, "f"),
            ("pressure", array.array, "f"),
            ("strength", array.array, "f"),
            ("uv_factor", array.array, "f"),
            ("uv_rotation", array.array, "f"),
        )
        for name, type_, element_type in expected:
            self.assertIn("co", gp_points)
            item = gp_points[name]
            self.assertIsInstance(item, SoaElement)
            self.assertIsInstance(item._array, type_)
            if type_ is array.array:
                self.assertEqual(item._array.typecode, element_type)
            else:
                self.assertIsInstance(item._array[0], element_type)

        self.assertEqual(len(gp_points["pressure"]._array), len(gp_points["strength"]._array))
        self.assertEqual(3 * len(gp_points["pressure"]._array), len(gp_points["co"]._array))

        # Test path resolution for soa buffers
        stroke = bpy.data.grease_pencils["Stroke"]
        path = next(iter(gp._soas))

        points_attribute, points_gp_points = gp.find_by_path(stroke, path)
        self.assertEqual(points_attribute, stroke.layers["Lines"].frames[0].strokes[0].points)
        self.assertIs(points_gp_points._data, gp_points)


class TestFunctionDispatch(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.open_mainfile(filepath=test_blend_file)
        self.cube = bpy.data.objects["Cube"]

    def test_rna_dispatch(self):
        # test_misc.TestFunctionDispatch.test_rna_dispatch

        @dispatch_rna
        def f(collection: T.bpy_prop_collection, arg):
            return "f_no_rna", arg

        @f.register_default()
        def _(collection: T.bpy_prop_collection, arg):
            return "f_default", arg

        @f.register(T.ObjectModifiers)
        @f.register(T.ObjectGpencilModifiers)
        def _(collection: T.bpy_prop_collection, arg):
            return "f_modifiers", arg

        @f.register(T.ObjectConstraints)
        def _(collection: T.bpy_prop_collection, arg):
            return "f_constraints", arg

        @dispatch_rna
        def g(collection: T.bpy_prop_collection, arg):
            return "g_no_rna", arg

        @g.register(T.ObjectModifiers)
        @g.register(T.ObjectGpencilModifiers)
        def _(collection: T.bpy_prop_collection, arg):
            return "g_modifiers", arg

        @g.register(T.ObjectConstraints)
        def _(collection: T.bpy_prop_collection, arg):
            return "g_constraints", arg

        self.assertEqual(f(self.cube.modifiers, 1), ("f_modifiers", 1))
        self.assertEqual(f(self.cube.grease_pencil_modifiers, 2), ("f_modifiers", 2))
        self.assertEqual(f(self.cube.constraints, 3), ("f_constraints", 3))
        self.assertEqual(f(self.cube.name, 4), ("f_no_rna", 4))
        self.assertEqual(f(self.cube.material_slots, 5), ("f_no_rna", 5))
        self.assertEqual(f(self.cube.particle_systems, 6), ("f_default", 6))

        self.assertEqual(g(self.cube.modifiers, 1), ("g_modifiers", 1))
        self.assertEqual(g(self.cube.grease_pencil_modifiers, 2), ("g_modifiers", 2))
        self.assertEqual(g(self.cube.constraints, 3), ("g_constraints", 3))
        self.assertEqual(g(self.cube.name, 4), ("g_no_rna", 4))
        self.assertEqual(g(self.cube.material_slots, 5), ("g_no_rna", 5))
        self.assertEqual(g(self.cube.particle_systems, 6), ("g_no_rna", 6))
