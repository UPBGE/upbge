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
from mixer.blender_client.misc import get_or_create_object_data, get_object_path, get_or_create_path
from mixer.broadcaster import common
from mixer.broadcaster.client import Client
from mixer.share_data import share_data
import bpy

logger = logging.getLogger(__name__)


def get_camera_buffer(obj):
    cam = obj.data
    focal = cam.lens
    front_clip_plane = cam.clip_start
    far_clip_plane = cam.clip_end
    dof_enabled = cam.dof.use_dof
    aperture = cam.dof.aperture_fstop
    colimator_name = cam.dof.focus_object.name_full if cam.dof.focus_object is not None else ""
    sensor_fit_name = cam.sensor_fit
    sensor_fit = common.SensorFitMode.AUTO
    if sensor_fit_name == "AUTO":
        sensor_fit = common.SensorFitMode.AUTO
    elif sensor_fit_name == "HORIZONTAL":
        sensor_fit = common.SensorFitMode.HORIZONTAL
    elif sensor_fit_name == "VERTICAL":
        sensor_fit = common.SensorFitMode.VERTICAL
    sensor_width = cam.sensor_width
    sensor_height = cam.sensor_height

    path = get_object_path(obj)
    return (
        common.encode_string(path)
        + common.encode_string(obj.name_full)
        + common.encode_float(focal)
        + common.encode_float(front_clip_plane)
        + common.encode_float(far_clip_plane)
        + common.encode_bool(dof_enabled)
        + common.encode_float(aperture)
        + common.encode_string(colimator_name)
        + common.encode_int(sensor_fit.value)
        + common.encode_float(sensor_width)
        + common.encode_float(sensor_height)
    )


def send_camera(client: Client, obj):
    camera_buffer = get_camera_buffer(obj)
    if camera_buffer:
        client.add_command(common.Command(common.MessageType.CAMERA, camera_buffer, 0))


def build_camera(data):
    camera_path, start = common.decode_string(data, 0)
    logger.info("build_camera %s", camera_path)
    camera_name, start = common.decode_string(data, start)
    camera = get_or_create_camera(camera_name)

    camera.lens, start = common.decode_float(data, start)
    camera.clip_start, start = common.decode_float(data, start)
    camera.clip_end, start = common.decode_float(data, start)
    camera.dof.use_dof, start = common.decode_bool(data, start)
    camera.dof.aperture_fstop, start = common.decode_float(data, start)
    colimator_name, start = common.decode_string(data, start)
    sensor_fit, start = common.decode_int(data, start)
    camera.sensor_width, start = common.decode_float(data, start)
    camera.sensor_height, start = common.decode_float(data, start)

    if sensor_fit == 0:
        camera.sensor_fit = "AUTO"
    elif sensor_fit == 1:
        camera.sensor_fit = "VERTICAL"
    else:
        camera.sensor_fit = "HORIZONTAL"

    get_or_create_object_data(camera_path, camera)

    # colimator
    if len(colimator_name) > 0:
        camera.dof.use_dof = True
        camera.dof.focus_object = get_or_create_path(colimator_name)


def get_or_create_camera(camera_name):
    camera = share_data.blender_cameras.get(camera_name)
    if camera:
        return camera
    camera = bpy.data.cameras.new(camera_name)
    share_data._blender_cameras[camera.name_full] = camera
    return camera
