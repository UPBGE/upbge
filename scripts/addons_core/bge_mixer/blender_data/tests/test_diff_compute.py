from pathlib import Path

import unittest

import bpy

from mixer.blender_data.aos_proxy import AosProxy
from mixer.blender_data.aos_soa_proxy import SoaElement
from mixer.blender_data.bpy_data_proxy import BpyDataProxy
from mixer.blender_data.datablock_proxy import DatablockProxy
from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy
from mixer.blender_data.datablock_collection_proxy import DatablockRefCollectionProxy
from mixer.blender_data.proxy import DeltaAddition, DeltaDeletion, DeltaReplace, DeltaUpdate
from mixer.blender_data.diff import BpyBlendDiff
from mixer.blender_data.struct_proxy import StructProxy

from mixer.blender_data.filter import test_properties


class DifferentialCompute(unittest.TestCase):
    def setUp(self):
        this_folder = Path(__file__).parent
        test_blend_file = str(this_folder / "empty.blend")
        file = test_blend_file
        bpy.ops.wm.open_mainfile(filepath=file)
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        self.scenes_property = bpy.data.bl_rna.properties["scenes"]

    @property
    def scene_proxy(self):
        return self.proxy.data("scenes").search_one("Scene")

    @property
    def scene(self):
        return bpy.data.scenes["Scene"]

    def generate_all_uuids(self):
        # as a side effect, BpyBlendDiff generates the uuids
        _ = BpyBlendDiff()
        _.diff(self.proxy, test_properties)


class Datablock(DifferentialCompute):
    def test_datablock_builtin(self):
        # test_diff_compute.Datablock.test_datablock_builtin
        expected_float = 0.5
        self.scene.audio_volume = expected_float
        diff = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())

        # there is a pending issue with use_curve_mapping. it is filtered on proxy load, but not during diff
        # and is sone struct it has a value despite use_curve_mapping = False
        # TODO do not use curve_mapping. We wand the documents to be as close as possible
        # self.assertSetEqual(set(diff.value._data.keys()), {"audio_volume"})

        delta = diff.value._data["audio_volume"]
        self.assertIsInstance(delta, DeltaUpdate)
        value = delta.value
        self.assertIsInstance(value, float)
        self.assertEqual(value, expected_float)

    def test_datablock_struct_builtin(self):
        expected_bool = not self.scene.eevee.use_bloom
        self.scene.eevee.use_bloom = expected_bool
        diff = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())

        # there is a pending issue with use_curve_mapping. it is filtered on proxy load, but not during diff
        # and is sone struct it has a value despite use_curve_mapping = False
        # TODO do not use curve_mapping. We wand the documents to be as close as possible
        # self.assertSetEqual(set(diff.value._data.keys()), {"eevee"})

        delta_eevee = diff.value._data["eevee"]
        self.assertIsInstance(delta_eevee, DeltaUpdate)
        self.assertIsInstance(delta_eevee.value, StructProxy)
        self.assertSetEqual(set(delta_eevee.value._data.keys()), {"use_bloom"})
        delta_use_bloom = delta_eevee.value._data["use_bloom"]
        self.assertEqual(delta_use_bloom.value, expected_bool)


class StructDatablockRef(DifferentialCompute):
    # datablock reference in a struct
    # Scene.world
    def test_add(self):
        # set reference from NOne to a valid datablock
        # test_diff_compute.StructDatablockRef.test_add
        self.scene.world = None
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        world = bpy.data.worlds.new("W")
        self.scene.world = world
        self.generate_all_uuids()
        scene_delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())
        self.assertIsInstance(scene_delta, DeltaUpdate)
        world_delta = scene_delta.value.data("world", resolve_delta=False)
        self.assertIsInstance(world_delta, DeltaReplace)
        world_update = world_delta.value
        self.assertIsInstance(world_update, DatablockRefProxy)
        self.assertEqual(world_update._datablock_uuid, world.mixer_uuid)

    def test_update(self):
        # set reference from None to a valid datablock
        # test_diff_compute.StructDatablockRef.test_update
        world1 = bpy.data.worlds.new("W1")
        world2 = bpy.data.worlds.new("W2")
        self.scene.world = world1
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        self.scene.world = world2
        self.generate_all_uuids()
        scene_delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())
        self.assertIsInstance(scene_delta, DeltaUpdate)
        world_delta = scene_delta.value.data("world", resolve_delta=False)
        self.assertIsInstance(world_delta, DeltaReplace)
        world_update = world_delta.value
        self.assertIsInstance(world_update, DatablockRefProxy)
        self.assertEqual(world_update._datablock_uuid, world2.mixer_uuid)

    def test_remove(self):
        # set reference from a valid datablock to None
        # test_diff_compute.StructDatablockRef.test_remove
        world1 = bpy.data.worlds.new("W1")
        self.scene.world = world1
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        self.scene.world = None
        self.generate_all_uuids()
        # delta contains valid ref to None
        scene_delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())
        self.assertIsInstance(scene_delta, DeltaUpdate)
        world_delta = scene_delta.value.data("world", resolve_delta=False)
        self.assertIsInstance(world_delta, DeltaReplace)
        world_update = world_delta.value
        self.assertIsInstance(world_update, DatablockRefProxy)
        self.assertFalse(world_update)


class Collection(DifferentialCompute):
    # test_diff_compute.Collection

    # @unittest.skip("AttributeError: 'CollectionObjects' object has no attribute 'fixed_type'")
    def test_datablock_collection(self):
        # Scene.collection.objects
        # A collection of references to standalone datablocks

        # test_diff_compute.Collection.test_datablock_collection
        for i in range(2):
            name = f"Unchanged{i}"
            empty = bpy.data.objects.new(name, None)
            self.scene.collection.objects.link(empty)
        for i in range(2):
            name = f"Deleted{i}"
            empty = bpy.data.objects.new(name, None)
            self.scene.collection.objects.link(empty)

        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        for i in range(2):
            name = f"Added{i}"
            empty = bpy.data.objects.new(name, None)
            self.scene.collection.objects.link(empty)
        for i in range(2):
            bpy.data.objects.remove(bpy.data.objects[f"Deleted{i}"])

        self.generate_all_uuids()

        scene_delta = self.scene_proxy.diff(self.scene, self.scene.name, self.scenes_property, self.proxy.context())

        self.assertIsInstance(scene_delta, DeltaUpdate)
        scene_update = scene_delta.value
        self.assertIsInstance(scene_update, DatablockProxy)

        collection_delta = scene_update.data("collection", resolve_delta=False)
        self.assertIsInstance(scene_delta, DeltaUpdate)
        collection_update = collection_delta.value
        self.assertIsInstance(collection_update, StructProxy)

        objects_delta = collection_update.data("objects", resolve_delta=False)
        self.assertIsInstance(objects_delta, DeltaUpdate)
        objects_update = objects_delta.value
        self.assertIsInstance(objects_update, DatablockRefCollectionProxy)

        deltas = {delta.value._initial_name: delta for delta in objects_update._data.values()}
        proxies = {name: delta.value for name, delta in deltas.items()}
        for name in ("Added0", "Added1"):
            self.assertIsInstance(deltas[name], DeltaAddition)
            self.assertIsInstance(proxies[name], DatablockRefProxy)

        for name in ("Deleted0", "Deleted1"):
            self.assertIsInstance(deltas[name], DeltaDeletion)
            self.assertIsInstance(proxies[name], DatablockRefProxy)

    def test_bpy_collection(self):
        # bpy.data.collections[x].objects
        # A collection of references to standalone datablocks

        # test_diff_compute.Collection.test_bpy_collection
        collection = bpy.data.collections.new("Collection")
        for i in range(2):
            empty = bpy.data.objects.new(f"Unchanged{i}", None)
            collection.objects.link(empty)
        for i in range(2):
            empty = bpy.data.objects.new(f"Unlinked{i}", None)
            collection.objects.link(empty)
        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        self.collection_proxy = self.proxy.data("collections").search_one("Collection")
        self.collection = bpy.data.collections["Collection"]
        for i in range(2):
            empty = bpy.data.objects.new(f"Added{i}", None)
            collection.objects.link(empty)
        for i in range(2):
            collection.objects.unlink(bpy.data.objects[f"Unlinked{i}"])

        self.generate_all_uuids()
        collections_property = bpy.data.bl_rna.properties["scenes"]

        collection_delta = self.collection_proxy.diff(
            self.collection, self.collection.name, collections_property, self.proxy.context()
        )

        self.assertIsInstance(collection_delta, DeltaUpdate)
        collection_update = collection_delta.value
        self.assertIsInstance(collection_update, DatablockProxy)

        objects_delta = collection_update.data("objects", resolve_delta=False)
        self.assertIsInstance(objects_delta, DeltaUpdate)
        objects_update = objects_delta.value
        self.assertIsInstance(objects_update, DatablockRefCollectionProxy)

        #  test_diff_compute.Collection.test_bpy_collection
        deltas = {delta.value._initial_name: delta for delta in objects_update._data.values()}
        proxies = {name: delta.value for name, delta in deltas.items()}
        for name in ("Added0", "Added1"):
            self.assertIsInstance(deltas[name], DeltaAddition)
            self.assertIsInstance(proxies[name], DatablockRefProxy)

        for name in ("Unlinked0", "Unlinked1"):
            self.assertIsInstance(deltas[name], DeltaDeletion)
            self.assertIsInstance(proxies[name], DatablockRefProxy)


class Aos(DifferentialCompute):
    # test_diff_compute.Aos

    # @unittest.skip("AttributeError: 'CollectionObjects' object has no attribute 'fixed_type'")
    def test_modify_value(self):
        # modify a vertex coordinate in a mesh

        # test_diff_compute.Aos.test_modify_value

        mesh = bpy.data.meshes.new("Mesh")
        mesh.vertices.add(4)
        for i in [0, 1, 2, 3]:
            v = 10 * i
            mesh.vertices[i].co = [v, v + 1, v + 2]

        self.proxy = BpyDataProxy()
        self.proxy.load(test_properties)
        mesh_proxy = self.proxy.data("meshes").search_one("Mesh")
        plane_mesh = bpy.data.meshes["Mesh"]

        expected_vertex = (-1.0, -2.0, -3.0)
        plane_mesh.vertices[0].co = expected_vertex
        expected_vertices = [list(vertex.co) for vertex in mesh.vertices]

        self.generate_all_uuids()

        mesh_delta = mesh_proxy.diff(plane_mesh, plane_mesh.name, None, self.proxy.context())

        self.assertIsInstance(mesh_delta, DeltaUpdate)
        mesh_update = mesh_delta.value
        self.assertIsInstance(mesh_update, DatablockProxy)

        vertices_delta = mesh_update.data("vertices", resolve_delta=False)
        self.assertIsInstance(vertices_delta, DeltaUpdate)
        vertices_update = vertices_delta.value
        self.assertIsInstance(vertices_update, AosProxy)
        self.assertTrue(vertices_update)

        co_delta = vertices_update.data("co", resolve_delta=False)
        self.assertIsInstance(co_delta, DeltaUpdate)
        co_update = co_delta.value
        self.assertIsInstance(co_update, SoaElement)

        array_ = co_update._array
        self.assertEqual(len(array_), 4 * 3)
        vertices = [[x, y, z] for x, y, z in zip(array_[0::3], array_[1::3], array_[2::3])]

        self.assertEqual(vertices, expected_vertices)
