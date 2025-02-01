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

import logging
import struct
import array
from typing import Optional

import bpy
import bmesh
from mathutils import Vector

from mixer.broadcaster import common
from mixer.blender_client import material as material_api

logger = logging.getLogger(__name__)


def decode_layer_float(elmt, layer, data, index):
    elmt[layer], index = common.decode_float(data, index)
    return index


def extract_layer_float(elmt, layer):
    return (elmt[layer],)


extract_layer_float.struct = "1f"


def decode_layer_int(elmt, layer, data, index):
    elmt[layer], index = common.decode_int(data, index)
    return index


def extract_layer_int(elmt, layer):
    return (elmt[layer],)


extract_layer_int.struct = "1i"


def decode_layer_vector(elmt, layer, data, index):
    elmt[layer], index = common.decode_vector3(data, index)
    return index


def extract_layer_vector3(elmt, layer):
    v = elmt[layer]
    return (v[0], v[1], v[2])


extract_layer_vector3.struct = "3f"


def decode_layer_color(elmt, layer, data, index):
    elmt[layer], index = common.decode_color(data, index)
    return index


def extract_layer_color(elmt, layer):
    color = elmt[layer]
    if len(color) == 3:
        return (color[0], color[1], color[2], 1.0)
    return (color[0], color[1], color[2], color[3])


extract_layer_color.struct = "4f"


def decode_layer_uv(elmt, layer, data, index):
    pin_uv, index = common.decode_bool(data, index)
    uv, index = common.decode_vector2(data, index)
    elmt[layer].pin_uv = pin_uv
    elmt[layer].uv = uv
    return index


def extract_layer_uv(elmt, layer):
    return (elmt[layer].pin_uv, *elmt[layer].uv)


extract_layer_uv.struct = "1I2f"


def decode_bmesh_layer(data, index, layer_collection, element_seq, decode_layer_value_func):
    layer_count, index = common.decode_int(data, index)
    while layer_count > len(layer_collection):
        if not layer_collection.is_singleton:
            layer_collection.new()
        else:
            layer_collection.verify()  # Will create a layer and returns it
            break  # layer_count should be one but break just in case
    for i in range(layer_count):
        layer = layer_collection[i]
        for elt in element_seq:
            index = decode_layer_value_func(elt, layer, data, index)
    return index


def encode_bmesh_layer(layer_collection, element_seq, extract_layer_tuple_func):
    buffer = []
    count = 0
    for i in range(len(layer_collection)):
        layer = layer_collection[i]
        for elt in element_seq:
            buffer.extend(extract_layer_tuple_func(elt, layer))
            count += 1

    binary_buffer = struct.pack("1I", len(layer_collection))
    if len(layer_collection) > 0:
        binary_buffer += struct.pack(extract_layer_tuple_func.struct * count, *buffer)
    return binary_buffer


# We cannot iterate directly over bm.loops, so we use a generator
def loops_iterator(bm):
    for face in bm.faces:
        for loop in face.loops:
            yield loop


def encode_baked_mesh(obj):
    """
    Bake an object as a triangle mesh and encode it.
    """
    # Bake modifiers
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj = obj.evaluated_get(depsgraph)

    # Triangulate mesh (before calculating normals)
    mesh = obj.data if obj.type == "MESH" else obj.to_mesh()
    if mesh is None:
        # This happens for empty curves
        return bytes()

    original_bm = None
    if obj.type == "MESH":
        # Mesh is restored later only if is has not been generated from a curve or something else
        original_bm = bmesh.new()
        original_bm.from_mesh(mesh)

    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()

    # Calculate normals, necessary if auto-smooth option enabled
    mesh.calc_normals()
    mesh.calc_normals_split()
    # calc_loop_triangles resets normals so... don't use it

    # get active uv layer
    uvlayer = mesh.uv_layers.active

    vertices = array.array("d", (0.0,)) * len(mesh.vertices) * 3
    mesh.vertices.foreach_get("co", vertices)

    normals = array.array("d", (0.0,)) * len(mesh.loops) * 3
    mesh.loops.foreach_get("normal", normals)

    if uvlayer:
        uvs = array.array("d", (0.0,)) * len(mesh.loops) * 2
        mesh.uv_layers[0].data.foreach_get("uv", uvs)
    else:
        uvs = []

    indices = array.array("i", (0,)) * len(mesh.loops)
    mesh.loops.foreach_get("vertex_index", indices)

    if len(obj.material_slots) <= 1:
        material_indices = []
    else:
        material_indices = array.array("i", (0,)) * len(mesh.polygons)
        mesh.polygons.foreach_get("material_index", material_indices)

    if obj.type != "MESH":
        obj.to_mesh_clear()
    else:
        original_bm.to_mesh(mesh)
        original_bm.free()

    # Vericex count + binary vertices buffer
    size = len(vertices) // 3
    binary_vertices_buffer = common.int_to_bytes(size, 4) + struct.pack(f"{len(vertices)}f", *vertices)

    # Normals count + binary normals buffer
    size = len(normals) // 3
    binary_normals_buffer = common.int_to_bytes(size, 4) + struct.pack(f"{len(normals)}f", *normals)

    # UVs count + binary uvs buffer
    size = len(uvs) // 2
    binary_uvs_buffer = common.int_to_bytes(size, 4) + struct.pack(f"{len(uvs)}f", *uvs)

    # material indices + binary material indices buffer
    size = len(material_indices)
    binary_material_indices_buffer = common.int_to_bytes(size, 4) + struct.pack(
        f"{len(material_indices)}I", *material_indices
    )

    # triangle indices count + binary triangle indices buffer
    size = len(indices) // 3
    binary_indices_buffer = common.int_to_bytes(size, 4) + struct.pack(f"{len(indices)}I", *indices)

    return (
        binary_vertices_buffer
        + binary_normals_buffer
        + binary_uvs_buffer
        + binary_material_indices_buffer
        + binary_indices_buffer
    )


def encode_base_mesh_geometry(mesh_data):

    # We do not synchronize "select" and "hide" state of mesh elements
    # because we consider them user specific.

    bm = bmesh.new()
    bm.from_mesh(mesh_data)

    binary_buffer = bytes()

    logger.debug("Writing %d vertices", len(bm.verts))
    bm.verts.ensure_lookup_table()

    verts_array = []
    for vert in bm.verts:
        verts_array.extend((*vert.co,))

    binary_buffer += struct.pack(f"1I{len(verts_array)}f", len(bm.verts), *verts_array)

    # Vertex layers
    # Ignored layers for now:
    # - skin (BMVertSkin)
    # - deform (BMDeformVert)
    # - paint_mask (float)
    # Other ignored layers:
    # - shape: shape keys are handled with Shape Keys at the mesh and object level
    # - float, int, string: don't really know their role
    binary_buffer += encode_bmesh_layer(bm.verts.layers.bevel_weight, bm.verts, extract_layer_float)

    logger.debug("Writing %d edges", len(bm.edges))
    bm.edges.ensure_lookup_table()

    edges_array = []
    for edge in bm.edges:
        edges_array.extend((edge.verts[0].index, edge.verts[1].index, edge.smooth, edge.seam))

    binary_buffer += struct.pack(f"1I{len(edges_array)}I", len(bm.edges), *edges_array)

    # Edge layers
    # Ignored layers for now: None
    # Other ignored layers:
    # - freestyle: of type NotImplementedType, maybe reserved for future dev
    # - float, int, string: don't really know their role
    binary_buffer += encode_bmesh_layer(bm.edges.layers.bevel_weight, bm.edges, extract_layer_float)
    binary_buffer += encode_bmesh_layer(bm.edges.layers.crease, bm.edges, extract_layer_float)

    logger.debug("Writing %d faces", len(bm.faces))
    bm.faces.ensure_lookup_table()

    faces_array = []
    for face in bm.faces:
        faces_array.extend((face.material_index, face.smooth, len(face.verts)))
        faces_array.extend((vert.index for vert in face.verts))

    binary_buffer += struct.pack(f"1I{len(faces_array)}I", len(bm.faces), *faces_array)

    # Face layers
    # Ignored layers for now: None
    # Other ignored layers:
    # - freestyle: of type NotImplementedType, maybe reserved for future dev
    # - float, int, string: don't really know their role
    binary_buffer += encode_bmesh_layer(bm.faces.layers.face_map, bm.faces, extract_layer_int)

    # Loops layers
    # A loop is an edge attached to a face (so each edge of a manifold can have 2 loops at most).
    # Ignored layers for now: None
    # Other ignored layers:
    # - float, int, string: don't really know their role
    binary_buffer += encode_bmesh_layer(bm.loops.layers.uv, loops_iterator(bm), extract_layer_uv)
    binary_buffer += encode_bmesh_layer(bm.loops.layers.color, loops_iterator(bm), extract_layer_color)

    bm.free()

    return binary_buffer


def encode_base_mesh(obj):

    # Temporary for curves and other objects that support to_mesh()
    # #todo Implement correct base encoding for these objects
    mesh_data = obj.data if obj.type == "MESH" else obj.to_mesh()
    if mesh_data is None:
        # This happens for empty curves
        # This is temporary, when curves will be fully implemented we will encode something
        return bytes()

    binary_buffer = encode_base_mesh_geometry(mesh_data)

    # Shape keys
    # source https://blender.stackexchange.com/questions/111661/creating-shape-keys-using-python
    if mesh_data.shape_keys is None:
        binary_buffer += common.encode_int(0)  # Indicate 0 key blocks
    else:
        logger.debug("Writing %d shape keys", len(mesh_data.shape_keys.key_blocks))

        binary_buffer += common.encode_int(len(mesh_data.shape_keys.key_blocks))
        # Encode names
        for key_block in mesh_data.shape_keys.key_blocks:
            binary_buffer += common.encode_string(key_block.name)
        # Encode vertex group names
        for key_block in mesh_data.shape_keys.key_blocks:
            binary_buffer += common.encode_string(key_block.vertex_group)
        # Encode relative key names
        for key_block in mesh_data.shape_keys.key_blocks:
            binary_buffer += common.encode_string(key_block.relative_key.name)
        # Encode data
        shape_keys_buffer = []
        fmt_str = ""
        for key_block in mesh_data.shape_keys.key_blocks:
            shape_keys_buffer.extend(
                (key_block.mute, key_block.value, key_block.slider_min, key_block.slider_max, len(key_block.data))
            )
            fmt_str += f"1I1f1f1f1I{(3 * len(key_block.data))}f"
            for i in range(len(key_block.data)):
                shape_keys_buffer.extend(key_block.data[i].co)
        binary_buffer += struct.pack(f"{fmt_str}", *shape_keys_buffer)

        binary_buffer += common.encode_bool(mesh_data.shape_keys.use_relative)

    # Vertex Groups
    verts_per_group = {}
    for vertex_group in obj.vertex_groups:
        verts_per_group[vertex_group.index] = []
    for vert in mesh_data.vertices:
        for vg in vert.groups:
            weighted_vertices = verts_per_group.get(vg.group, None)
            if weighted_vertices:
                weighted_vertices.append((vert.index, vg.weight))

    binary_buffer += common.encode_int(len(obj.vertex_groups))
    for vertex_group in obj.vertex_groups:
        binary_buffer += common.encode_string(vertex_group.name)
        binary_buffer += common.encode_bool(vertex_group.lock_weight)
        binary_buffer += common.encode_int(len(verts_per_group[vertex_group.index]))
        for vg_elmt in verts_per_group[vertex_group.index]:
            binary_buffer += common.encode_int(vg_elmt[0])
            binary_buffer += common.encode_float(vg_elmt[1])

    # Normals
    binary_buffer += common.encode_bool(mesh_data.use_auto_smooth)
    binary_buffer += common.encode_float(mesh_data.auto_smooth_angle)
    binary_buffer += common.encode_bool(mesh_data.has_custom_normals)

    if mesh_data.has_custom_normals:
        mesh_data.calc_normals_split()  # Required otherwise all normals are (0, 0, 0)
        normals = []
        for loop in mesh_data.loops:
            normals.extend((*loop.normal,))
        binary_buffer += struct.pack(f"{len(normals)}f", *normals)

    # UV Maps
    for uv_layer in mesh_data.uv_layers:
        binary_buffer += common.encode_string(uv_layer.name)
        binary_buffer += common.encode_bool(uv_layer.active_render)

    # Vertex Colors
    for vertex_colors in mesh_data.vertex_colors:
        binary_buffer += common.encode_string(vertex_colors.name)
        binary_buffer += common.encode_bool(vertex_colors.active_render)

    if obj.type != "MESH":
        obj.to_mesh_clear()

    return binary_buffer


def encode_mesh(obj, do_encode_base_mesh, do_encode_baked_mesh):
    binary_buffer = bytes()

    if do_encode_base_mesh:
        logger.info("encode_base_mesh %s", obj.name_full)
        mesh_buffer = encode_base_mesh(obj)
        binary_buffer += common.encode_int(len(mesh_buffer))
        binary_buffer += mesh_buffer
    else:
        binary_buffer += common.encode_int(0)

    if do_encode_baked_mesh:
        logger.info("encode_baked_mesh %s", obj.name_full)
        mesh_buffer = encode_baked_mesh(obj)
        binary_buffer += common.encode_int(len(mesh_buffer))
        binary_buffer += mesh_buffer
    else:
        binary_buffer += common.encode_int(0)

    # Materials
    materials = []
    for material in obj.data.materials:
        materials.append(material.name_full if material is not None else "")
    binary_buffer += common.encode_string_array(materials)

    return binary_buffer


def decode_baked_mesh(obj: Optional[bpy.types.Object], data, index):
    # Note: Blender should not load a baked mesh but we have this function to debug the encoding part
    # and as an example for implementations that load baked meshes
    byte_size, index = common.decode_int(data, index)
    if byte_size == 0:
        return index

    positions, index = common.decode_vector3_array(data, index)
    normals, index = common.decode_vector3_array(data, index)
    uvs, index = common.decode_vector2_array(data, index)
    material_indices, index = common.decode_int_array(data, index)
    triangles, index = common.decode_int3_array(data, index)

    if obj is not None:
        bm = bmesh.new()
        for i in range(len(positions)):
            bm.verts.new(positions[i])
            # according to https://blender.stackexchange.com/questions/49357/bmesh-how-can-i-import-custom-vertex-normals
            # normals are not working for bmesh...
            # vertex.normal = normals[i]
        bm.verts.ensure_lookup_table()

        uv_layer = None
        if len(uvs) > 0:
            uv_layer = bm.loops.layers.uv.new()

        multi_material = False
        if len(material_indices) > 1:
            multi_material = True

        current_uv_index = 0
        for i in range(len(triangles)):
            triangle = triangles[i]
            i1 = triangle[0]
            i2 = triangle[1]
            i3 = triangle[2]
            try:
                face = bm.faces.new((bm.verts[i1], bm.verts[i2], bm.verts[i3]))
                if multi_material:
                    face.material_index = material_indices[i]
                else:
                    face.material_index = 0
                if uv_layer:
                    face.loops[0][uv_layer].uv = uvs[current_uv_index]
                    face.loops[1][uv_layer].uv = uvs[current_uv_index + 1]
                    face.loops[2][uv_layer].uv = uvs[current_uv_index + 2]
                    current_uv_index = current_uv_index + 3
            except Exception:
                pass

        me = obj.data

        bm.to_mesh(me)
        bm.free()

        # hack ! Since bmesh cannot be used to set custom normals
        me.normals_split_custom_set(normals)
        me.use_auto_smooth = True

    return index


def decode_base_mesh(client, obj: bpy.types.Object, mesh: bpy.types.Mesh, data, index):
    bm = bmesh.new()

    position_count, index = common.decode_int(data, index)
    logger.debug("Reading %d vertices", position_count)

    for _pos_idx in range(position_count):
        co, index = common.decode_vector3(data, index)
        bm.verts.new(co)

    bm.verts.ensure_lookup_table()

    index = decode_bmesh_layer(data, index, bm.verts.layers.bevel_weight, bm.verts, decode_layer_float)

    edge_count, index = common.decode_int(data, index)
    logger.debug("Reading %d edges", edge_count)

    edges_data = struct.unpack(f"{edge_count * 4}I", data[index : index + edge_count * 4 * 4])
    index += edge_count * 4 * 4

    for edge_idx in range(edge_count):
        v1 = edges_data[edge_idx * 4]
        v2 = edges_data[edge_idx * 4 + 1]
        edge = bm.edges.new((bm.verts[v1], bm.verts[v2]))
        edge.smooth = bool(edges_data[edge_idx * 4 + 2])
        edge.seam = bool(edges_data[edge_idx * 4 + 3])

    index = decode_bmesh_layer(data, index, bm.edges.layers.bevel_weight, bm.edges, decode_layer_float)
    index = decode_bmesh_layer(data, index, bm.edges.layers.crease, bm.edges, decode_layer_float)

    face_count, index = common.decode_int(data, index)
    logger.debug("Reading %d faces", face_count)

    for _face_idx in range(face_count):
        material_idx, index = common.decode_int(data, index)
        smooth, index = common.decode_bool(data, index)
        vert_count, index = common.decode_int(data, index)
        face_vertices = struct.unpack(f"{vert_count}I", data[index : index + vert_count * 4])
        index += vert_count * 4
        verts = [bm.verts[i] for i in face_vertices]
        face = bm.faces.new(verts)
        face.material_index = material_idx
        face.smooth = smooth

    index = decode_bmesh_layer(data, index, bm.faces.layers.face_map, bm.faces, decode_layer_int)

    index = decode_bmesh_layer(data, index, bm.loops.layers.uv, loops_iterator(bm), decode_layer_uv)
    index = decode_bmesh_layer(data, index, bm.loops.layers.color, loops_iterator(bm), decode_layer_color)

    bm.normal_update()
    bm.to_mesh(mesh)
    bm.free()

    # Load shape keys
    shape_keys_count, index = common.decode_int(data, index)
    obj.shape_key_clear()
    if shape_keys_count > 0:
        logger.debug("Loading %d shape keys", shape_keys_count)
        shapes_keys_list = []
        for _i in range(shape_keys_count):
            shape_key_name, index = common.decode_string(data, index)
            shapes_keys_list.append(obj.shape_key_add(name=shape_key_name))
        for i in range(shape_keys_count):
            shapes_keys_list[i].vertex_group, index = common.decode_string(data, index)
        for i in range(shape_keys_count):
            relative_key_name, index = common.decode_string(data, index)
            shapes_keys_list[i].relative_key = obj.data.shape_keys.key_blocks[relative_key_name]

        for i in range(shape_keys_count):
            shape_key = shapes_keys_list[i]
            shape_key.mute, index = common.decode_bool(data, index)
            shape_key.value, index = common.decode_float(data, index)
            shape_key.slider_min, index = common.decode_float(data, index)
            shape_key.slider_max, index = common.decode_float(data, index)
            shape_key_data_size, index = common.decode_int(data, index)
            for i in range(shape_key_data_size):
                shape_key.data[i].co = Vector(struct.unpack("3f", data[index : index + 3 * 4]))
                index += 3 * 4
        obj.data.shape_keys.use_relative, index = common.decode_bool(data, index)

    # Vertex Groups
    vg_count, index = common.decode_int(data, index)
    obj.vertex_groups.clear()
    for _i in range(vg_count):
        vg_name, index = common.decode_string(data, index)
        vertex_group = obj.vertex_groups.new(name=vg_name)
        vertex_group.lock_weight, index = common.decode_bool(data, index)
        vg_size, index = common.decode_int(data, index)
        for _elmt_idx in range(vg_size):
            vert_idx, index = common.decode_int(data, index)
            weight, index = common.decode_float(data, index)
            vertex_group.add([vert_idx], weight, "REPLACE")

    # Normals
    mesh.use_auto_smooth, index = common.decode_bool(data, index)
    mesh.auto_smooth_angle, index = common.decode_float(data, index)

    has_custom_normal, index = common.decode_bool(data, index)

    if has_custom_normal:
        normals = []
        for _loop in mesh.loops:
            normal, index = common.decode_vector3(data, index)
            normals.append(normal)
        mesh.normals_split_custom_set(normals)

    # UV Maps and Vertex Colors are added automatically based on layers in the bmesh
    # We just need to update their name and active_render state:

    # UV Maps
    for uv_layer in mesh.uv_layers:
        uv_layer.name, index = common.decode_string(data, index)
        uv_layer.active_render, index = common.decode_bool(data, index)

    # Vertex Colors
    for vertex_colors in mesh.vertex_colors:
        vertex_colors.name, index = common.decode_string(data, index)
        vertex_colors.active_render, index = common.decode_bool(data, index)

    return index


def decode_mesh(client, obj, data, index):
    assert obj.data

    # Clear materials before building faces because it erase material idx of faces
    obj.data.materials.clear()

    byte_size, index = common.decode_int(data, index)
    if byte_size == 0:
        # No base mesh, lets read the baked mesh
        index = decode_baked_mesh(obj, data, index)
    else:
        index = decode_base_mesh(client, obj, obj.data, data, index)
        # Skip the baked mesh (its size is encoded here)
        baked_mesh_byte_size, index = common.decode_int(data, index)
        index += baked_mesh_byte_size

    # Materials
    material_names, index = common.decode_string_array(data, index)
    for material_name in material_names:
        material = material_api.get_or_create_material(material_name) if material_name != "" else None
        obj.data.materials.append(material)

    return index


def decode_mesh_generic(client, mesh: bpy.types.Mesh, data, index):
    tmp_obj = None
    try:
        tmp_obj = bpy.data.objects.new("_mixer_tmp_", mesh)
        byte_size, index = common.decode_int(data, index)
        if byte_size == 0:
            # No base mesh, lets read the baked mesh
            index = decode_baked_mesh(tmp_obj, data, index)
        else:
            index = decode_base_mesh(client, tmp_obj, mesh, data, index)
            # Skip the baked mesh (its size is encoded here)
            baked_mesh_byte_size, index = common.decode_int(data, index)
            index += baked_mesh_byte_size
    finally:
        if tmp_obj:
            bpy.data.objects.remove(tmp_obj)

    return index
