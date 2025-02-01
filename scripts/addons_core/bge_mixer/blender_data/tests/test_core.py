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


from mixer.blender_data.type_helpers import is_pointer_to
from mixer.blender_data.bpy_data_proxy import BpyDataProxy
from mixer.blender_data.filter import test_properties
from mixer.blender_data.struct_proxy import StructProxy
from mixer.blender_data.tests.utils import equals, register_bl_equals, test_blend_file


# @unittest.skip('')
class TestCore(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.open_mainfile(filepath=test_blend_file)
        register_bl_equals(self, test_properties)
        self._proxy = BpyDataProxy()
        self._context = self._proxy.context()

    def test_issubclass(self):

        # Warning T.bpy_struct is not T.Struct !!
        self.assertTrue(issubclass(T.ID, T.bpy_struct))
        self.assertFalse(issubclass(T.bpy_struct, T.ID))

        self.assertTrue(issubclass(T.StringProperty, T.StringProperty))
        self.assertTrue(issubclass(T.StringProperty, T.Property))
        self.assertTrue(issubclass(T.StringProperty, T.bpy_struct))
        self.assertFalse(issubclass(T.StringProperty, T.ID))

        self.assertTrue(issubclass(T.ShaderNodeTree, T.NodeTree))
        self.assertTrue(issubclass(T.ShaderNodeTree, T.ID))
        self.assertTrue(issubclass(T.ShaderNodeTree, T.bpy_struct))
        self.assertTrue(isinstance(T.ShaderNodeTree.bl_rna, T.NodeTree))
        self.assertTrue(isinstance(T.ShaderNodeTree.bl_rna, T.ID))
        self.assertTrue(isinstance(T.ShaderNodeTree.bl_rna, T.bpy_struct))

        self.assertTrue(issubclass(T.Camera, T.Camera))
        self.assertTrue(issubclass(T.Camera, T.ID))
        self.assertTrue(isinstance(T.Camera.bl_rna, T.Camera))
        self.assertTrue(isinstance(T.Camera.bl_rna, T.ID))

    def test_invariants(self):
        s = D.scenes[0]

        #
        # same bl_rna in type and property
        self.assertTrue(isinstance(s, T.Scene))
        self.assertIs(T.Scene.bl_rna, s.bl_rna)

        #
        # Pointers
        self.assertTrue(isinstance(s.eevee, T.SceneEEVEE))
        self.assertFalse(isinstance(s.eevee, T.PointerProperty))
        self.assertIsNot(T.Scene.bl_rna.properties["eevee"].bl_rna, s.eevee.bl_rna)
        self.assertIs(T.Scene.bl_rna.properties["eevee"].bl_rna, T.PointerProperty.bl_rna)
        self.assertIs(T.Scene.bl_rna.properties["eevee"].fixed_type.bl_rna, T.SceneEEVEE.bl_rna)
        # readonly pointer with readwrite pointee :
        self.assertTrue(T.Scene.bl_rna.properties["eevee"].is_readonly)
        s.eevee.use_volumetric_shadows = not s.eevee.use_volumetric_shadows
        # readwrite pointer :
        self.assertFalse(T.Scene.bl_rna.properties["camera"].is_readonly)

        #
        # Collection element type
        # The type of a collection element : Scene.objects is a T.Object
        objects_rna_property = T.Scene.bl_rna.properties["objects"]
        self.assertNotEqual(objects_rna_property.fixed_type, T.Object)
        self.assertIs(objects_rna_property.fixed_type.bl_rna, T.Object.bl_rna)
        self.assertIs(T.Mesh.bl_rna.properties["vertices"].srna.bl_rna, T.MeshVertices.bl_rna)

    def test_types_grease_pencil(self):
        # Grease pencil elements
        triangles = T.GPencilStroke.bl_rna.properties["triangles"]
        self.assertIs(triangles.bl_rna, T.CollectionProperty.bl_rna)
        self.assertIs(triangles.fixed_type.bl_rna, T.GPencilTriangle.bl_rna)

    def test_check_types(self):
        # check our own assertions about types
        for name in dir(bpy.types):
            item = getattr(bpy.types, name)
            try:
                rna = item.bl_rna
            except AttributeError:
                continue

            for prop in rna.properties.values():
                # All ID are behind pointers or in collections
                self.assertFalse(isinstance(prop.bl_rna, T.ID))

    def test_pointer_class(self):
        eevee = T.Scene.bl_rna.properties["eevee"]
        self.assertTrue(is_pointer_to(eevee, T.SceneEEVEE))

        collection = T.Scene.bl_rna.properties["collection"]
        self.assertTrue(is_pointer_to(collection, T.Collection))
        node_tree = T.World.bl_rna.properties["node_tree"]
        self.assertTrue(is_pointer_to(node_tree, T.NodeTree))
        self.assertFalse(is_pointer_to(node_tree, T.ShaderNodeTree))

        camera = T.Scene.bl_rna.properties["camera"]
        self.assertTrue(is_pointer_to(camera, T.Object))

        data = T.Object.bl_rna.properties["data"]
        self.assertTrue(is_pointer_to(data, T.ID))

    def test_scene_viewlayer_layercollection_is_master(self):
        s = D.scenes["Scene_0"]
        master_coll = s.collection
        for vl in s.view_layers:
            self.assertIs(vl.layer_collection.collection, master_coll)

    def test_skip_ShaderNodeTree(self):  # noqa N802
        world = D.worlds["World"]
        proxy = StructProxy().load(world, self._context)
        self.assertTrue("color" in proxy._data)
        # self.assertFalse("node_tree" in proxy._data)

    def test_equals(self):
        self.assertTrue(equals(D, D))
        self.assertTrue(equals(D.objects[0], D.objects[0]))
        self.assertFalse(equals(D.objects[0], D.objects[1]))

    def test_equality_func(self):
        self.assertEqual(D.objects[0], D.objects[0])
        self.assertNotEqual(D.objects[0], D.objects[1])
        self.assertEqual(D.objects, D.objects)
        self.assertEqual(D, D)
