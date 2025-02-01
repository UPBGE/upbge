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

import struct

from mixer.blender_client.misc import get_or_create_object_data, get_object_path
from mixer.broadcaster import common
from mixer.broadcaster.client import Client
from mixer.share_data import share_data
import bpy


def send_grease_pencil_stroke(stroke):
    buffer = common.encode_int(stroke.material_index)
    buffer += common.encode_int(stroke.line_width)

    points = list()

    for point in stroke.points:
        points.extend(point.co)
        points.append(point.pressure)
        points.append(point.strength)

    binary_points_buffer = common.int_to_bytes(len(stroke.points), 4) + struct.pack(f"{len(points)}f", *points)
    buffer += binary_points_buffer
    return buffer


def send_grease_pencil_frame(frame):
    buffer = common.encode_int(frame.frame_number)
    buffer += common.encode_int(len(frame.strokes))
    for stroke in frame.strokes:
        buffer += send_grease_pencil_stroke(stroke)
    return buffer


def send_grease_pencil_layer(layer, name):
    buffer = common.encode_string(name)
    buffer += common.encode_bool(layer.hide)
    buffer += common.encode_int(len(layer.frames))
    for frame in layer.frames:
        buffer += send_grease_pencil_frame(frame)
    return buffer


def send_grease_pencil_time_offset(client: Client, obj):
    grease_pencil = obj.data
    buffer = common.encode_string(grease_pencil.name_full)

    for modifier in obj.grease_pencil_modifiers:
        if modifier.type != "GP_TIME":
            continue
        offset = modifier.offset
        scale = modifier.frame_scale
        custom_range = modifier.use_custom_frame_range
        frame_start = modifier.frame_start
        frame_end = modifier.frame_end
        buffer += (
            common.encode_int(offset)
            + common.encode_float(scale)
            + common.encode_bool(custom_range)
            + common.encode_int(frame_start)
            + common.encode_int(frame_end)
        )
        client.add_command(common.Command(common.MessageType.GREASE_PENCIL_TIME_OFFSET, buffer, 0))
        break


def send_grease_pencil_mesh(client: Client, obj):
    grease_pencil = obj.data
    buffer = common.encode_string(grease_pencil.name_full)

    buffer += common.encode_int(len(grease_pencil.materials))
    for material in grease_pencil.materials:
        if not material:
            material_name = "Default"
        else:
            material_name = material.name_full
        buffer += common.encode_string(material_name)

    buffer += common.encode_int(len(grease_pencil.layers))
    for name, layer in grease_pencil.layers.items():
        buffer += send_grease_pencil_layer(layer, name)

    client.add_command(common.Command(common.MessageType.GREASE_PENCIL_MESH, buffer, 0))

    send_grease_pencil_time_offset(client, obj)


def send_grease_pencil_material(client: Client, material):
    gp_material = material.grease_pencil
    stroke_enable = gp_material.show_stroke
    stroke_mode = gp_material.mode
    stroke_style = gp_material.stroke_style
    stroke_color = gp_material.color
    stroke_overlap = gp_material.use_overlap_strokes
    fill_enable = gp_material.show_fill
    fill_style = gp_material.fill_style
    fill_color = gp_material.fill_color
    gp_material_buffer = common.encode_string(material.name_full)
    gp_material_buffer += common.encode_bool(stroke_enable)
    gp_material_buffer += common.encode_string(stroke_mode)
    gp_material_buffer += common.encode_string(stroke_style)
    gp_material_buffer += common.encode_color(stroke_color)
    gp_material_buffer += common.encode_bool(stroke_overlap)
    gp_material_buffer += common.encode_bool(fill_enable)
    gp_material_buffer += common.encode_string(fill_style)
    gp_material_buffer += common.encode_color(fill_color)
    client.add_command(common.Command(common.MessageType.GREASE_PENCIL_MATERIAL, gp_material_buffer, 0))


def send_grease_pencil_connection(client: Client, obj):
    buffer = common.encode_string(get_object_path(obj))
    buffer += common.encode_string(obj.data.name_full)
    client.add_command(common.Command(common.MessageType.GREASE_PENCIL_CONNECTION, buffer, 0))


def build_grease_pencil_connection(data):
    path, start = common.decode_string(data, 0)
    grease_pencil_name, start = common.decode_string(data, start)
    gp = share_data.blender_grease_pencils[grease_pencil_name]
    get_or_create_object_data(path, gp)


def decode_grease_pencil_stroke(grease_pencil_frame, stroke_index, data, index):
    material_index, index = common.decode_int(data, index)
    line_width, index = common.decode_int(data, index)
    points, index = common.decode_array(data, index, "5f", 5 * 4)

    if stroke_index >= len(grease_pencil_frame.strokes):
        stroke = grease_pencil_frame.strokes.new()
    else:
        stroke = grease_pencil_frame.strokes[stroke_index]

    stroke.material_index = material_index
    stroke.line_width = line_width

    p = stroke.points
    if len(points) > len(p):
        p.add(len(points) - len(p))
    if len(points) < len(p):
        max_index = len(points) - 1
        for _i in range(max_index, len(p)):
            p.pop(max_index)

    for i in range(len(p)):
        point = points[i]
        p[i].co = (point[0], point[1], point[2])
        p[i].pressure = point[3]
        p[i].strength = point[4]
    return index


def decode_grease_pencil_frame(grease_pencil_layer, data, index):
    grease_pencil_frame, index = common.decode_int(data, index)
    frame = None
    for f in grease_pencil_layer.frames:
        if f.frame_number == grease_pencil_frame:
            frame = f
            break
    if not frame:
        frame = grease_pencil_layer.frames.new(grease_pencil_frame)
    stroke_count, index = common.decode_int(data, index)
    for stroke_index in range(stroke_count):
        index = decode_grease_pencil_stroke(frame, stroke_index, data, index)
    return index


def decode_grease_pencil_layer(grease_pencil, data, index):
    grease_pencil_layer_name, index = common.decode_string(data, index)
    layer = grease_pencil.get(grease_pencil_layer_name)
    if not layer:
        layer = grease_pencil.layers.new(grease_pencil_layer_name)
    layer.hide, index = common.decode_bool(data, index)
    frame_count, index = common.decode_int(data, index)
    for _ in range(frame_count):
        index = decode_grease_pencil_frame(layer, data, index)
    return index


def build_grease_pencil_mesh(data):
    grease_pencil_name, index = common.decode_string(data, 0)

    grease_pencil = share_data.blender_grease_pencils.get(grease_pencil_name)
    if not grease_pencil:
        grease_pencil = bpy.data.grease_pencils.new(grease_pencil_name)
        share_data._blender_grease_pencils[grease_pencil.name_full] = grease_pencil

    grease_pencil.materials.clear()
    material_count, index = common.decode_int(data, index)
    for _ in range(material_count):
        material_name, index = common.decode_string(data, index)
        material = share_data.blender_materials.get(material_name)
        grease_pencil.materials.append(material)

    layer_count, index = common.decode_int(data, index)
    for _ in range(layer_count):
        index = decode_grease_pencil_layer(grease_pencil, data, index)


def build_grease_pencil_material(data):
    grease_pencil_material_name, start = common.decode_string(data, 0)
    material = share_data.blender_materials.get(grease_pencil_material_name)
    if not material:
        material = bpy.data.materials.new(grease_pencil_material_name)
        share_data._blender_materials[material.name_full] = material
    if not material.grease_pencil:
        bpy.data.materials.create_gpencil_data(material)

    gp_material = material.grease_pencil
    gp_material.show_stroke, start = common.decode_bool(data, start)
    gp_material.mode, start = common.decode_string(data, start)
    gp_material.stroke_style, start = common.decode_string(data, start)
    gp_material.color, start = common.decode_color(data, start)
    gp_material.use_overlap_strokes, start = common.decode_bool(data, start)
    gp_material.show_fill, start = common.decode_bool(data, start)
    gp_material.fill_style, start = common.decode_string(data, start)
    gp_material.fill_color, start = common.decode_color(data, start)


def build_grease_pencil(data):
    object_path, start = common.decode_string(data, 0)
    grease_pencil_name, start = common.decode_string(data, start)
    grease_pencil = share_data.blender_grease_pencils.get(grease_pencil_name)
    if not grease_pencil:
        grease_pencil = bpy.data.grease_pencils.new(grease_pencil_name)
        get_or_create_object_data(object_path, grease_pencil)
