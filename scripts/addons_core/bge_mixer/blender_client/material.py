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
from mixer.broadcaster import common
from mixer.broadcaster.client import Client
from mixer.share_data import share_data
from mixer.local_data import get_resolved_file_path

import bpy

logger = logging.getLogger(__name__)


def get_or_create_material(material_name):
    material = share_data.blender_materials.get(material_name)
    if material:
        material.use_nodes = True
        return material

    material = bpy.data.materials.new(name=material_name)
    share_data._blender_materials[material.name_full] = material
    material.use_nodes = True
    return material


def build_texture(principled, material, channel, is_color, data, index):
    name_or_path, index = common.decode_string(data, index)
    if len(name_or_path) == 0:
        return index

    texture_data = share_data.client.textures.get(name_or_path)
    if texture_data is None:
        logger.error("%s not registered", name_or_path)
        return index

    tex_image = None
    for link in material.node_tree.links:
        if link.to_socket == principled.inputs[channel]:
            connected_node = link.from_socket.node
            if connected_node.type == "TEX_IMAGE":
                tex_image = connected_node
                break

    if tex_image is None:
        tex_image = material.node_tree.nodes.new("ShaderNodeTexImage")

    if texture_data.packed:
        if tex_image.image and tex_image.image.packed_file:
            return index
        buffer = texture_data.data
        tex_image.image = bpy.data.images.new(name_or_path, width=texture_data.width, height=texture_data.height)
        tex_image.image.pack(data=buffer, data_len=len(buffer))
        tex_image.image.source = "FILE"
    else:
        resolved_filename = get_resolved_file_path(name_or_path)
        if tex_image.image and tex_image.image.filepath == resolved_filename:
            return index

        try:
            tex_image.image = bpy.data.images.load(resolved_filename)
        except Exception as e:
            logger.error(e)

    if not is_color:
        tex_image.image.colorspace_settings.name = "Non-Color"
    material.node_tree.links.new(principled.inputs[channel], tex_image.outputs["Color"])
    return index


def build_material(data):
    material_name_length = common.bytes_to_int(data[:4])
    start = 4
    end = start + material_name_length
    material_name = data[start:end].decode()
    start = end

    material = get_or_create_material(material_name)
    nodes = material.node_tree.nodes
    # Get a principled node
    principled = None
    if nodes:
        for n in nodes:
            if n.type == "BSDF_PRINCIPLED":
                principled = n
                break

    if not principled:
        logger.error("Cannot find Principled BSDF node")
        return

    index = start

    # Transmission ( 1 - opacity)
    transmission, index = common.decode_float(data, index)
    transmission = 1 - transmission
    principled.inputs["Transmission"].default_value = transmission
    file_name, index = common.decode_string(data, index)
    if len(file_name) > 0:
        invert = material.node_tree.nodes.new("ShaderNodeInvert")
        material.node_tree.links.new(principled.inputs["Transmission"], invert.outputs["Color"])
        tex_image = material.node_tree.nodes.new("ShaderNodeTexImage")
        try:
            tex_image.image = bpy.data.images.load(get_resolved_file_path(file_name))
            tex_image.image.colorspace_settings.name = "Non-Color"
        except Exception as e:
            logger.error("could not load file %s ...", get_resolved_file_path(file_name))
            logger.error("... %s", e)
        material.node_tree.links.new(invert.inputs["Color"], tex_image.outputs["Color"])

    # Base Color
    base_color, index = common.decode_color(data, index)
    material.diffuse_color = (base_color[0], base_color[1], base_color[2], 1)
    principled.inputs["Base Color"].default_value = material.diffuse_color
    index = build_texture(principled, material, "Base Color", True, data, index)

    # Metallic
    material.metallic, index = common.decode_float(data, index)
    principled.inputs["Metallic"].default_value = material.metallic
    index = build_texture(principled, material, "Metallic", False, data, index)

    # Roughness
    material.roughness, index = common.decode_float(data, index)
    principled.inputs["Roughness"].default_value = material.roughness
    index = build_texture(principled, material, "Roughness", False, data, index)

    # Normal
    file_name, index = common.decode_string(data, index)
    if len(file_name) > 0:
        normal_map = material.node_tree.nodes.new("ShaderNodeNormalMap")
        material.node_tree.links.new(principled.inputs["Normal"], normal_map.outputs["Normal"])
        tex_image = material.node_tree.nodes.new("ShaderNodeTexImage")
        try:
            tex_image.image = bpy.data.images.load(get_resolved_file_path(file_name))
            tex_image.image.colorspace_settings.name = "Non-Color"
        except Exception as e:
            logger.error("could not load file %s ...", get_resolved_file_path(file_name))
            logger.error("... %s", e)
        material.node_tree.links.new(normal_map.inputs["Color"], tex_image.outputs["Color"])

    # Emission
    emission, index = common.decode_color(data, index)
    principled.inputs["Emission"].default_value = emission
    index = build_texture(principled, material, "Emission", False, data, index)


def build_assign_material(data):
    index = 0
    object_name, index = common.decode_string(data, index)
    material_name, index = common.decode_string(data, index)
    mesh = share_data.blender_objects[object_name]
    material = get_or_create_material(material_name)

    # If the mesh hasn't any material slot, just append the material (this will create the material)
    if len(mesh.material_slots) == 0:
        mesh.data.materials.append(material)
    # Else assign the material to all slots
    else:
        for slot in mesh.material_slots:
            slot.material = material


def get_material_buffer(client: Client, material):
    name = material.name_full
    buffer = common.encode_string(name)
    principled = None
    diffuse = None
    # Get the nodes in the node tree
    if material.node_tree:
        nodes = material.node_tree.nodes
        # Get a principled node
        if nodes:
            for n in nodes:
                if n.type == "BSDF_PRINCIPLED":
                    principled = n
                    break
                if n.type == "BSDF_DIFFUSE":
                    diffuse = n
        # principled = next(n for n in nodes if n.type == 'BSDF_PRINCIPLED')
    if principled is None and diffuse is None:
        base_color = (0.8, 0.8, 0.8)
        metallic = 0.0
        roughness = 0.5
        opacity = 1.0
        emission_color = (0.0, 0.0, 0.0)
        buffer += common.encode_float(opacity) + common.encode_string("")
        buffer += common.encode_color(base_color) + common.encode_string("")
        buffer += common.encode_float(metallic) + common.encode_string("")
        buffer += common.encode_float(roughness) + common.encode_string("")
        buffer += common.encode_string("")
        buffer += common.encode_color(emission_color) + common.encode_string("")
        return buffer
    elif diffuse:
        opacity = 1.0
        opacity_texture = None
        metallic = 0.0
        metallic_texture = None
        emission = (0.0, 0.0, 0.0)
        emission_texture = None

        # Get the slot for 'base color'
        # Or principled.inputs[0]
        base_color = (1.0, 1.0, 1.0)
        base_color_texture = None
        base_color_input = diffuse.inputs.get("Color")
        # Get its default value (not the value from a possible link)
        if base_color_input:
            base_color = base_color_input.default_value
            base_color_texture = client.get_texture(base_color_input)

        roughness = 1.0
        roughness_texture = None
        roughness_input = diffuse.inputs.get("Roughness")
        if roughness_input:
            roughness_texture = client.get_texture(roughness_input)
            if len(roughness_input.links) == 0:
                roughness = roughness_input.default_value

        normal_texture = None
        norma_input = diffuse.inputs.get("Normal")
        if norma_input:
            if len(norma_input.links) == 1:
                normal_map = norma_input.links[0].from_node
                if "Color" in normal_map.inputs:
                    color_input = normal_map.inputs["Color"]
                    normal_texture = client.get_texture(color_input)

    else:
        opacity = 1.0
        opacity_texture = None
        opacity_input = principled.inputs.get("Transmission")
        if opacity_input:
            if len(opacity_input.links) == 1:
                invert = opacity_input.links[0].from_node
                if "Color" in invert.inputs:
                    color_input = invert.inputs["Color"]
                    opacity_texture = client.get_texture(color_input)
            else:
                opacity = 1.0 - opacity_input.default_value

        # Get the slot for 'base color'
        # Or principled.inputs[0]
        base_color = (1.0, 1.0, 1.0)
        base_color_texture = None
        base_color_input = principled.inputs.get("Base Color")
        # Get its default value (not the value from a possible link)
        if base_color_input:
            base_color = base_color_input.default_value
            base_color_texture = client.get_texture(base_color_input)

        metallic = 0.0
        metallic_texture = None
        metallic_input = principled.inputs.get("Metallic")
        if metallic_input:
            metallic_texture = client.get_texture(metallic_input)
            if len(metallic_input.links) == 0:
                metallic = metallic_input.default_value

        roughness = 1.0
        roughness_texture = None
        roughness_input = principled.inputs.get("Roughness")
        if roughness_input:
            roughness_texture = client.get_texture(roughness_input)
            if len(roughness_input.links) == 0:
                roughness = roughness_input.default_value

        normal_texture = None
        norma_input = principled.inputs.get("Normal")
        if norma_input:
            if len(norma_input.links) == 1:
                normal_map = norma_input.links[0].from_node
                if "Color" in normal_map.inputs:
                    color_input = normal_map.inputs["Color"]
                    normal_texture = client.get_texture(color_input)

        emission = (0.0, 0.0, 0.0)
        emission_texture = None
        emission_input = principled.inputs.get("Emission")
        if emission_input:
            # Get its default value (not the value from a possible link)
            emission = emission_input.default_value
            emission_texture = client.get_texture(emission_input)

    buffer += common.encode_float(opacity)
    if opacity_texture:
        buffer += common.encode_string(opacity_texture)
    else:
        buffer += common.encode_string("")
    buffer += common.encode_color(base_color)
    if base_color_texture:
        buffer += common.encode_string(base_color_texture)
    else:
        buffer += common.encode_string("")

    buffer += common.encode_float(metallic)
    if metallic_texture:
        buffer += common.encode_string(metallic_texture)
    else:
        buffer += common.encode_string("")

    buffer += common.encode_float(roughness)
    if roughness_texture:
        buffer += common.encode_string(roughness_texture)
    else:
        buffer += common.encode_string("")

    if normal_texture:
        buffer += common.encode_string(normal_texture)
    else:
        buffer += common.encode_string("")

    buffer += common.encode_color(emission)
    if emission_texture:
        buffer += common.encode_string(emission_texture)
    else:
        buffer += common.encode_string("")

    return buffer
