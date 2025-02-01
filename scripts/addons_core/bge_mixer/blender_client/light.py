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
from mixer.blender_client.misc import get_or_create_object_data, get_object_path
from mixer.broadcaster import common
from mixer.broadcaster.client import Client
from mixer.share_data import share_data
import bpy

logger = logging.getLogger(__name__)


def get_light_buffer(obj):
    light = obj.data
    light_type_name = light.type
    light_type = common.LightType.SUN
    if light_type_name == "POINT":
        light_type = common.LightType.POINT
    elif light_type_name == "SPOT":
        light_type = common.LightType.SPOT
    elif light_type_name == "SUN":
        light_type = common.LightType.SUN
    elif light_type_name == "AREA":
        light_type = common.LightType.AREA
    else:
        return None
    color = light.color
    power = light.energy
    if bpy.context.scene.render.engine == "CYCLES":
        shadow = light.cycles.cast_shadow
    else:
        shadow = light.use_shadow

    spot_blend = 10.0
    spot_size = 0.0
    if light_type == common.LightType.SPOT:
        spot_size = light.spot_size
        spot_blend = light.spot_blend

    return (
        common.encode_string(get_object_path(obj))
        + common.encode_string(light.name_full)
        + common.encode_int(light_type.value)
        + common.encode_int(shadow)
        + common.encode_color(color)
        + common.encode_float(power)
        + common.encode_float(spot_size)
        + common.encode_float(spot_blend)
    )


def send_light(client: Client, obj):
    logger.info("send_light %s", obj.name_full)
    light_buffer = get_light_buffer(obj)
    if light_buffer:
        client.add_command(common.Command(common.MessageType.LIGHT, light_buffer, 0))


def build_light(data):
    light_path, start = common.decode_string(data, 0)
    light_name, start = common.decode_string(data, start)
    logger.info("build_light %s", light_path)
    light_type, start = common.decode_int(data, start)
    blighttype = "POINT"
    if light_type == common.LightType.SUN.value:
        blighttype = "SUN"
    elif light_type == common.LightType.POINT.value:
        blighttype = "POINT"
    elif light_type == common.LightType.AREA.value:
        blighttype = "AREA"
    else:
        blighttype = "SPOT"

    light = get_or_create_light(light_name, blighttype)

    shadow, start = common.decode_int(data, start)
    if shadow != 0:
        light.use_shadow = True
    else:
        light.use_shadow = False

    color, start = common.decode_color(data, start)
    light.color = (color[0], color[1], color[2])
    light.energy, start = common.decode_float(data, start)
    if light_type == common.LightType.SPOT.value:
        light.spot_size, start = common.decode_float(data, start)
        light.spot_blend, start = common.decode_float(data, start)

    get_or_create_object_data(light_path, light)


def get_or_create_light(light_name, light_type):
    light = share_data.blender_lights.get(light_name)
    if light and light.type != light_type:
        light.type = light_type
        share_data.blender_lights_dirty = True
        light = share_data.blender_lights.get(light_name)

    if light:
        return light
    light = bpy.data.lights.new(light_name, type=light_type)
    share_data._blender_lights[light.name_full] = light
    return light
