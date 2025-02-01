from pathlib import Path

import unittest

import bpy

from mixer.blender_data.bpy_data_proxy import BpyDataProxy
from mixer.blender_data.datablock_proxy import DatablockProxy
from mixer.blender_data.diff import BpyBlendDiff
from mixer.blender_data.filter import test_properties


class DifferentialApply(unittest.TestCase):
    def setUp(self):
        this_folder = Path(__file__).parent
        test_blend_file = str(this_folder / "empty.blend")
        file = test_blend_file
        bpy.ops.wm.open_mainfile(filepath=file)
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        self.scene_proxy: DatablockProxy = self.proxy.data("scenes").search_one("Scene")
        self.scene = bpy.data.scenes["Scene"]
        self.scenes_property = bpy.data.bl_rna.properties["scenes"]

    def generate_all_uuids(self):
        # as a side effect, BpyBlendDiff generates the uuids
        _ = BpyBlendDiff()
        _.diff(self.proxy, test_properties)


class Datablock(DifferentialApply):
    def test_builtin(self):
        # a python builtin in a dataclock
        # Scene.audio_volume

        # test_diff_apply.Datablock.test_builtin

        self.scene.audio_volume = 0.5
        delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())
        # the diff has audio_volume, updated to 0.5

        # rollback to anything else
        self.scene.audio_volume = 0.0

        # apply the diff
        scene = bpy.data.scenes[self.scene.name]
        self.scene_proxy.apply(scene, bpy.data.scenes, self.scene.name, delta, self.proxy.context())
        self.assertEqual(self.scene.audio_volume, 0.5)

    def test_struct_builtin(self):
        # a python builtin a a struct inside a datablock
        # Scene.eevee.use_bloom

        # test_diff_apply.Datablock.test_struct_builtin

        self.scene.eevee.use_bloom = False
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        self.scene_proxy: DatablockProxy = self.proxy.data("scenes").search_one("Scene")
        self.scene.eevee.use_bloom = True

        delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())
        # diff is -> True

        # reset
        self.scene.eevee.use_bloom = False

        # apply the diff
        scene = bpy.data.scenes[self.scene.name]
        self.scene_proxy.apply(scene, bpy.data.scenes, self.scene.name, delta, self.proxy.context())
        self.assertEqual(self.scene.eevee.use_bloom, True)


class StructDatablockRef(DifferentialApply):
    # datablock reference in a struct
    # Scene.world

    def test_add(self):
        # set reference from None to a valid datablock
        # test_diff_apply.StructDatablockRef.test_add

        # create first so that is is correctly registered (bpt_data.diff would register it, not scene_proxy.diff)
        world = bpy.data.worlds.new("W")
        self.scene.world = None
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        # Loaded proxy contains scene.world = None
        self.scene_proxy: DatablockProxy = self.proxy.data("scenes").search_one("Scene")

        self.scene.world = world
        self.generate_all_uuids()
        delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())
        # Diff contains set scene.proxy to world

        self.scene.world = None

        # apply the diff
        scene = bpy.data.scenes[self.scene.name]
        self.scene_proxy.apply(scene, bpy.data.scenes, self.scene.name, delta, self.proxy.context())
        self.assertEqual(self.scene.world, world)

    def test_update(self):
        # set reference from None to a valid datablock
        # test_diff_apply.StructDatablockRef.test_update
        world1 = bpy.data.worlds.new("W1")
        world2 = bpy.data.worlds.new("W2")
        self.scene.world = world1
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        self.scene_proxy: DatablockProxy = self.proxy.data("scenes").search_one("Scene")

        self.scene.world = world2
        self.generate_all_uuids()
        delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())
        # diff -> world2

        # reset
        self.scene.world = world1

        # apply the diff
        scene = bpy.data.scenes[self.scene.name]
        self.scene_proxy.apply(scene, bpy.data.scenes, self.scene.name, delta, self.proxy.context())
        self.assertEqual(self.scene.world, world2)

    def test_remove(self):
        # apply sets reference from a valid datablock to None
        # test_diff_apply.StructDatablockRef.test_remove
        world = bpy.data.worlds.new("W")
        self.scene.world = world
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        # Loaded proxy contains scene.world = world
        self.scene_proxy: DatablockProxy = self.proxy.data("scenes").search_one("Scene")

        self.scene.world = None
        self.generate_all_uuids()
        delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())
        # Delta contains set scene.proxy to none

        self.scene.world = world

        # apply the diff
        scene = bpy.data.scenes[self.scene.name]
        self.scene_proxy.apply(scene, bpy.data.scenes, self.scene.name, delta, self.proxy.context())
        self.assertEqual(self.scene.world, None)


class Collection(DifferentialApply):
    # test_differential.Collection

    def test_datablock_collection(self):
        # Scene.collection.objects
        # A collection of references to standalone datablocks
        # tests DatablockCollectionProxy.apply()

        # test_diff_apply.Collection.test_datablock_collection
        for i in range(2):
            empty = bpy.data.objects.new(f"Unchanged{i}", None)
            self.scene.collection.objects.link(empty)
        for i in range(2):
            empty = bpy.data.objects.new(f"Deleted{i}", None)
            self.scene.collection.objects.link(empty)
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        self.scene_proxy = self.proxy.data("scenes").search_one("Scene")
        self.scene = bpy.data.scenes["Scene"]
        for i in range(2):
            empty = bpy.data.objects.new(f"Added{i}", None)
            self.scene.collection.objects.link(empty)
        for i in range(2):
            empty = bpy.data.objects[f"Deleted{i}"]
            self.scene.collection.objects.unlink(empty)

        self.generate_all_uuids()

        scene_delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())
        # delta contains(deleted1, deleted 2, added1, added2)

        # reset
        for i in range(2):
            empty = bpy.data.objects[f"Deleted{i}"]
            self.scene.collection.objects.link(empty)
        for i in range(2):
            empty = bpy.data.objects[f"Added{i}"]
            self.scene.collection.objects.unlink(empty)

        # required because the Added{i} were created after proxy load and are not known by the proxy
        # at this time. IRL the depsgraph handler uses BpyBendDiff to find datablock additions,
        # then BpyDataProxy.update()
        self.proxy.load(test_properties)

        scene = bpy.data.scenes[self.scene.name]
        self.scene_proxy.apply(scene, bpy.data.scenes, self.scene.name, scene_delta, self.proxy.context())

        self.assertIn("Unchanged0", self.scene.collection.objects)
        self.assertIn("Unchanged1", self.scene.collection.objects)
        self.assertIn("Added0", self.scene.collection.objects)
        self.assertIn("Added1", self.scene.collection.objects)
        self.assertNotIn("Deleted0", self.scene.collection.objects)
        self.assertNotIn("Deleted1", self.scene.collection.objects)

    def test_key_str(self):
        # Scene.render.views
        # A bpy_prop_collection with string keys
        # tests StructCollectionProxy.apply()

        # test_diff_apply.Collection.test_key_str

        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        self.scene_proxy = self.proxy.data("scenes").search_one("Scene")
        self.scene = bpy.data.scenes["Scene"]

        view_right = self.scene.render.views["right"]
        self.scene.render.views.remove(view_right)

        view = self.scene.render.views.new("New")

        view = self.scene.render.views["left"]
        view_left_suffix_bak = view.file_suffix
        view.file_suffix = "new_suffix"

        self.generate_all_uuids()

        scene_delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())

        # reset to initial state
        views = bpy.data.scenes["Scene"].render.views
        view_right = views.new("right")

        views["left"].file_suffix = view_left_suffix_bak

        view_new = views["New"]
        views.remove(view_new)

        scene = bpy.data.scenes[self.scene.name]
        self.scene_proxy.apply(scene, bpy.data.scenes, self.scene.name, scene_delta, self.proxy.context())
        self.assertIn("New", views)
        self.assertIn("left", views)
        self.assertEqual(views["left"].file_suffix, "new_suffix")
        self.assertNotIn("right", views)

    @unittest.skip("Not implemented: addition in array")
    def test_key_int(self):
        # Scene.view_settings.curve_mapping.curves
        # A bpy_prop_collection with string keys

        # test_diff_apply.Collection.test_key_int
        self.scene.view_settings.use_curve_mapping = True

        points0 = self.scene.view_settings.curve_mapping.curves[0].points
        points0.new(0.5, 0.5)

        points1 = self.scene.view_settings.curve_mapping.curves[1].points

        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        self.scene_proxy = self.proxy.data("scenes").search_one("Scene")
        self.scene = bpy.data.scenes["Scene"]

        points0.remove(points0[1])
        points1.new(2.0, 2.0)

        self.generate_all_uuids()

        scene_delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())
        # the delta contains :
        #   curves[0]: Deletion of element 1
        #   curves[1]: Addition of element 2

        # reset state
        points0.new(0.5, 0.5)
        points1.remove(points1[2])

        scene = bpy.data.scenes[self.scene.name]
        self.scene_proxy.apply(scene, bpy.data.scenes, self.scene.name, scene_delta, self.proxy.context())
        self.assertEqual(len(points0), 2)
        self.assertEqual(list(points0[0].location), [0.0, 0.0])
        self.assertEqual(list(points0[1].location), [1.0, 1.0])

        self.assertEqual(len(points1), 3)
        self.assertEqual(list(points1[0].location), [0.0, 0.0])
        self.assertEqual(list(points1[1].location), [1.0, 1.0])
        self.assertEqual(list(points1[2].location), [2.0, 2.0])


class Aos(DifferentialApply):
    # test_diff_compute.Aos

    # @unittest.skip("AttributeError: 'CollectionObjects' object has no attribute 'fixed_type'")
    def test_modify_value(self):
        # modify a vertex coordinate in a mesh

        # test_diff_apply.Aos.test_modify_value

        mesh = bpy.data.meshes.new("Mesh")
        mesh.vertices.add(4)
        for i in [0, 1, 2, 3]:
            v = 10 * i
            mesh.vertices[i].co = [v, v + 1, v + 2]

        expected_vertices = [list(vertex.co) for vertex in mesh.vertices]

        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        mesh_proxy = self.proxy.data("meshes").search_one("Mesh")
        mesh = bpy.data.meshes["Mesh"]

        modified_vertex = (-1.0, -2.0, -3.0)
        mesh.vertices[0].co = modified_vertex

        self.generate_all_uuids()

        mesh_delta = mesh_proxy.diff(mesh, mesh.name, None, self.proxy.context())

        # reset mesh state
        mesh.vertices[0].co = (0.0, 1.0, 2.0)

        mesh_proxy.apply(bpy.data.meshes[mesh.name], bpy.data.meshes, mesh.name, mesh_delta, self.proxy.context())

        vertices = [list(vertex.co) for vertex in mesh.vertices]
        self.assertEqual(vertices, expected_vertices)
