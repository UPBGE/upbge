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
from mixer.blender_data.attributes import write_attribute
from mixer.blender_data.bpy_data_proxy import BpyDataProxy
from mixer.blender_data.datablock_proxy import DatablockProxy
from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy
from mixer.blender_data.tests.utils import register_bl_equals, test_blend_file

from mixer.blender_data.filter import test_properties
from mathutils import Matrix, Vector

synchronized_properties = test_properties


class TestWriteAttribute(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.open_mainfile(filepath=test_blend_file)

        # otherwise the loaded scene  way have curves despite use_curve_mapping==False and
        # the new one will not have curves and will not receive them as they are not send
        # use_curve_mapping == False
        D.scenes["Scene_0"].view_settings.use_curve_mapping = True

        self.proxy = BpyDataProxy()
        self.proxy.load(synchronized_properties)
        register_bl_equals(self, synchronized_properties)

    def test_write_simple_types(self):
        scene = D.scenes[0]
        object_ = D.objects[0]
        # matrix = [10.0, 20.0, 30.0, 40.0, 11.0, 21.0, 31.0, 41.0, 12.0, 22.0, 32.0, 42.0, 14.0, 24.0, 34.0, 44]
        matrix2 = [[10.0, 20.0, 30.0, 40], [11.0, 21.0, 31.0, 41], [12.0, 22.0, 32.0, 42], [14.0, 24.0, 34.0, 44]]
        values = [
            # (scene, "name", "Plop"),
            (scene, "frame_current", 99),
            (scene, "use_gravity", False),
            (scene, "gravity", [-1, -2, -3]),
            (scene, "gravity", Vector([-10, -20, -30])),
            (scene, "sync_mode", "FRAME_DROP"),
            # (object_, "matrix_world", matrix),
            (object_, "matrix_world", Matrix(matrix2)),
        ]
        for bl_instance, name, value in values:
            write_attribute(bl_instance, name, value, self.proxy.context())
            stored_value = getattr(bl_instance, name)
            stored_type = type(stored_value)
            self.assertEqual(stored_type(value), stored_value)

    def test_write_bpy_struct_scene_eevee(self):
        scene = D.scenes[0]
        eevee_proxy = self.proxy._data["scenes"].search_one("Scene_0")._data["eevee"]
        eevee_proxy._data["gi_cubemap_resolution"] = "64"
        eevee_proxy.save(scene.eevee, scene, "eevee", self.proxy.context())
        self.assertEqual("64", scene.eevee.gi_cubemap_resolution)

    def test_write_bpy_property_group_scene_cycles(self):
        # Not very useful it derives from struct
        scene = D.scenes[0]
        cycles_proxy = self.proxy._data["scenes"].search_one("Scene_0")._data["cycles"]
        cycles_proxy._data["shading_system"] = True
        cycles_proxy.save(scene.cycles, scene, "cycles", self.proxy.context())
        self.assertEqual(True, scene.cycles.shading_system)

    @unittest.skip("Mesh currently restricted to Mesh.name")
    def test_write_array_of_struct_with_vec(self):
        # self.addTypeEqualityFunc(D.bpy_struct, bl_equalityfunc)
        cube = D.meshes["Cube"]
        vertices_proxy = self.proxy._data["meshes"].search_one("Cube")._data["vertices"]

        # loaded as SOA into array.array
        co_proxy = vertices_proxy._data["co"]._data

        # first vertex
        co_proxy[0] *= 2
        co_proxy[1] *= 2
        co_proxy[2] *= 2

        vertices_proxy.save(cube.vertices, cube, "vertices", self.proxy.context())
        self.assertListEqual(list(cube.vertices[0].co[0:3]), co_proxy[0:3].tolist())

    # explicit test per data type , including addition in collections

    def test_write_datablock_light(self):
        # Write a whole scene datablock
        light_name = "Light"
        light = D.lights[light_name]
        light_proxy = self.proxy.data("lights").search_one(light_name)

        light.name = "light_bak"
        light_bak = D.lights["light_bak"]

        light_proxy._datablock_uuid = "__" + light_proxy._datablock_uuid
        datablock, _ = light_proxy.create_standalone_datablock(self.proxy.context())
        self.assertEqual(datablock, light_bak)

    def test_write_datablock_world(self):
        # Write a whole scene datablock
        world_name = "World"
        world = D.worlds[world_name]
        world_proxy = self.proxy.data("worlds").search_one(world_name)

        world.name = "world_bak"
        world_bak = D.worlds["world_bak"]

        world = D.worlds.new(world_name)

        world_proxy._datablock_uuid = "__" + world_proxy._datablock_uuid
        datablock, _ = world_proxy.create_standalone_datablock(self.proxy.context())

        self.assertEqual(datablock, world_bak)

    def test_write_array_curvemap(self):
        bpy.ops.wm.open_mainfile(filepath=test_blend_file)

        light_name = "Light"
        light = D.lights["Light"]
        points = [(0.111, 0.222), (0.333, 0.444)]
        curve0 = light.falloff_curve.curves[0]
        for i, point in enumerate(points):
            curve0.points[i].location = point

        self.proxy = BpyDataProxy()
        self.proxy.load(synchronized_properties)

        light.name = "light_bak"
        light_bak = D.lights["light_bak"]
        light = None

        light_proxy = self.proxy.data("lights").search_one(light_name)
        light_proxy._datablock_uuid = "__" + light_proxy._datablock_uuid
        light, _ = light_proxy.create_standalone_datablock(self.proxy.context())
        curve = light.falloff_curve.curves[0]
        for i, point in enumerate(points):
            for clone, expected in zip(curve.points[i].location, point):
                self.assertAlmostEqual(clone, expected)

        self.assertEqual(D.lights[light_name], light_bak)

    def test_array_curvemap_shrink(self):
        bpy.ops.wm.open_mainfile(filepath=test_blend_file)

        light_name = "Light"
        light = D.lights["Light"]
        src_points = [(0.666, 0.777), (0.888, 0.999)]
        curve0 = light.falloff_curve.curves[0]
        for i, point in enumerate(src_points):
            curve0.points[i].location = point

        self.proxy = BpyDataProxy()
        self.proxy.load(synchronized_properties)

        light.name = "light_bak"
        light = None

        light_proxy = self.proxy.data("lights").search_one(light_name)
        light_proxy._datablock_uuid = "__" + light_proxy._datablock_uuid
        light, _ = light_proxy.create_standalone_datablock(self.proxy.context())

        dst_curve = light.falloff_curve.curves[0]
        self.assertEqual(len(src_points), len(dst_curve.points))

        # extend the dst curvemap to 3 points
        dst_points = [(0.111, 0.222), (0.333, 0.444), (0.555, 0.666)]
        curve0 = light.falloff_curve.curves[0]
        curve0.points.new(*dst_points[2])
        for i, point in enumerate(dst_points):
            curve0.points[i].location = point
        self.assertEqual(len(dst_points), len(dst_curve.points))

        # restore again, save needs to shrink
        light_proxy._datablock_uuid = "__" + light_proxy._datablock_uuid
        light, _ = light_proxy.create_standalone_datablock(self.proxy.context())

        dst_curve = light.falloff_curve.curves[0]
        self.assertEqual(len(src_points), len(dst_curve.points))
        for i, point in enumerate(src_points):
            for dst, expected in zip(dst_curve.points[i].location, point):
                self.assertAlmostEqual(dst, expected)

    def test_array_curvemap_extend(self):
        # test_write.TestWriteAttribute.test_array_curvemap_extend
        bpy.ops.wm.open_mainfile(filepath=test_blend_file)

        light_name = "Light"
        light = D.lights["Light"]
        # extend the source curvemap to 3 points
        src_points = [(0.111, 0.222), (0.333, 0.444), (0.555, 0.666)]
        curve0 = light.falloff_curve.curves[0]
        curve0.points.new(*src_points[2])
        for i, point in enumerate(src_points):
            curve0.points[i].location = point

        self.proxy = BpyDataProxy()
        self.proxy.load(synchronized_properties)

        light.name = "light_bak"

        # the dst curvemap has 2 points by default
        # save() needs to extend
        light_proxy = self.proxy.data("lights").search_one(light_name)
        light_proxy._datablock_uuid = "__" + light_proxy._datablock_uuid
        light, _ = light_proxy.create_standalone_datablock(self.proxy.context())

        dst_curve = light.falloff_curve.curves[0]
        self.assertEqual(len(src_points), len(dst_curve.points))
        for i, point in enumerate(src_points):
            for dst, expected in zip(dst_curve.points[i].location, point):
                self.assertAlmostEqual(dst, expected)

    def test_write_datablock_scene(self):
        # Write a whole scene datablock
        scene_name = "Scene_0"
        scene = D.scenes[scene_name]

        # HACK
        # frame_current is 99 in the blend file and 1 on a freshly created scene
        # this avoids committing the blend file with an updated Scene.frame_current after Scene.frame_current is
        # filtered out
        scene.frame_current = 1

        scene_proxy = self.proxy.data("scenes").search_one(scene_name)
        self.assertIsInstance(scene_proxy, DatablockProxy)

        scene.name = "scene_bak"
        scene_bak = D.scenes["scene_bak"]

        scene_proxy._datablock_uuid = "__" + scene_proxy._datablock_uuid
        datablock, _ = scene_proxy.create_standalone_datablock(self.proxy.context())
        self.assertEqual(datablock, scene_bak)

    def test_write_datablock_reference_scene_world(self):
        # just write the Scene.world attribute
        scene_name = "Scene_0"
        scene = D.scenes[scene_name]
        expected_world = scene.world
        assert expected_world is not None

        world_ref_proxy = self.proxy.data("scenes").search_one(scene_name).data("world")
        self.assertIsInstance(world_ref_proxy, DatablockRefProxy)

        scene.world = None
        assert scene.world != expected_world

        world_ref_proxy.save(scene.world, scene, "world", self.proxy.context())
        self.assertEqual(scene.world, expected_world)

    def test_write_datablock_with_reference_camera_dof_target(self):
        # Write the whole camera datablock, including its reference to dof target
        # test_write.TestWriteAttribute.test_write_datablock_with_reference_camera_dof_target
        camera_name = "Camera_0"
        camera = D.cameras[camera_name]

        # setup the scene and reload
        focus_object = D.objects["Cube"]
        camera.dof.focus_object = focus_object
        self.proxy = BpyDataProxy()
        self.proxy.load(synchronized_properties)

        camera.name = "camera_bak"

        camera_proxy = self.proxy.data("cameras").search_one(camera_name)
        camera_proxy._datablock_uuid = "__" + camera_proxy._datablock_uuid
        datablock, _ = camera_proxy.create_standalone_datablock(self.proxy.context())
        self.assertEqual(datablock.dof.focus_object, focus_object)
