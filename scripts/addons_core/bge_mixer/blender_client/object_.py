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
import bpy

from mixer.broadcaster import common
from mixer.broadcaster.client import Client
from mixer.share_data import share_data

logger = logging.getLogger(__name__)


def send_object_visibility(client: Client, object_: bpy.types.Object):
    logger.debug("send_object_visibility %s", object_.name_full)
    buffer = (
        common.encode_string(object_.name_full)
        + common.encode_bool(object_.hide_viewport)
        + common.encode_bool(object_.hide_select)
        + common.encode_bool(object_.hide_render)
        + common.encode_bool(object_.hide_get())
    )
    client.add_command(common.Command(common.MessageType.OBJECT_VISIBILITY, buffer, 0))


def build_object_visibility(data):
    name_full, index = common.decode_string(data, 0)
    hide_viewport, index = common.decode_bool(data, index)
    hide_select, index = common.decode_bool(data, index)
    hide_render, index = common.decode_bool(data, index)
    hide_get, index = common.decode_bool(data, index)

    logger.debug("build_object_visibility %s", name_full)
    object_ = share_data.blender_objects.get(name_full)
    if object_ is None:
        logger.warning("build_object_visibility %s : object not found", name_full)
        return
    object_.hide_viewport = hide_viewport
    object_.hide_select = hide_select
    object_.hide_render = hide_render
    object_.hide_set(hide_get)
