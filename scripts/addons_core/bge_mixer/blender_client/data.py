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

"""
Send BLENDER_DATA messages, receive and process them
"""
from __future__ import annotations

import itertools
import logging
import traceback
from typing import List, TYPE_CHECKING

import bpy

from mixer.blender_data.json_codec import Codec, DecodeError, EncodeError
from mixer.blender_data.messages import (
    BlenderDataMessage,
    BlenderMediaMessage,
    BlenderRemoveMessage,
    BlenderRenamesMessage,
)
from mixer.broadcaster.common import Command, MessageType
from mixer.local_data import get_local_or_create_cache_file
from mixer.share_data import share_data

if TYPE_CHECKING:
    from mixer.blender_data.changeset import CreationChangeset, RemovalChangeset, UpdateChangeset, RenameChangeset
    from mixer.blender_data.datablock_proxy import DatablockProxy
    from mixer.blender_data.proxy import Delta, Uuid
    from mixer.blender_data.types import Soa


logger = logging.getLogger(__name__)


def send_media_creations(proxy: DatablockProxy):
    bytes_ = BlenderMediaMessage.encode(proxy)
    if bytes_ and proxy._media:
        logger.info("send_media_creations %s: %d bytes", proxy._media[0], len(bytes_))
        command = Command(MessageType.BLENDER_DATA_MEDIA, bytes_, 0)
        share_data.client.add_command(command)


def build_data_media(buffer: bytes):
    # TODO save to resolved path.
    # The packed data with be saved to file, not a problem
    message = BlenderMediaMessage()
    message.decode(buffer)
    logger.info("build_data_media %s: %d bytes", message.path, len(message.bytes_))
    # TODO this does not overwrite outdated local files
    get_local_or_create_cache_file(message.path, message.bytes_)


def send_data_creations(proxies: CreationChangeset):
    if share_data.use_vrtist_protocol():
        return

    codec = Codec()

    for datablock_proxy in proxies:
        logger.info("%s %s", "send_data_create", datablock_proxy)
        send_media_creations(datablock_proxy)
        try:
            encoded_proxy = codec.encode(datablock_proxy)
        except EncodeError as e:
            logger.error(f"send_data_create: encode exception for {datablock_proxy}")
            logger.error(f"... {e!r}")
            return
        except Exception:
            logger.error(f"send_data_create: encode exception for {datablock_proxy}")
            for line in traceback.format_exc().splitlines():
                logger.error(line)
            return

        buffer = BlenderDataMessage.encode(datablock_proxy, encoded_proxy)
        command = Command(MessageType.BLENDER_DATA_CREATE, buffer, 0)
        share_data.client.add_command(command)


def send_data_updates(updates: UpdateChangeset):
    if share_data.use_vrtist_protocol():
        return

    codec = Codec()
    for update in updates:
        logger.debug("%s %s", "send_data_update", update)

        try:
            encoded_update = codec.encode(update)
        except Exception:
            logger.error(f"send_data_update: encode exception for {update}")
            for line in traceback.format_exc().splitlines():
                logger.error(line)
            continue

        buffer = BlenderDataMessage.encode(update.value, encoded_update)
        command = Command(MessageType.BLENDER_DATA_UPDATE, buffer, 0)
        share_data.client.add_command(command)


def build_data_create(buffer):
    if share_data.use_vrtist_protocol():
        return

    share_data.set_dirty()
    rename_changeset = None
    codec = Codec()
    try:
        message = BlenderDataMessage()
        message.decode(buffer)
        datablock_proxy = codec.decode(message.proxy_string)
        logger.info("%s %s", "build_data_create", datablock_proxy)
        datablock_proxy.arrays = message.arrays
        _, rename_changeset = share_data.bpy_data_proxy.create_datablock(datablock_proxy)
        _build_soas(datablock_proxy.mixer_uuid, message.soas)
    except DecodeError as e:
        logger.error(f"Decode error for {str(e.args[1])[:100]} ...")
        logger.error("... possible version mismatch")
        return
    except Exception:
        logger.error("Exception during build_data_create")
        for line in traceback.format_exc().splitlines():
            logger.error(line)
        logger.error(buffer[0:200])
        logger.error("...")
        logger.error(buffer[-200:0])
        logger.error("ignored")
        return

    if rename_changeset:
        send_data_renames(rename_changeset)


def _build_soas(uuid: Uuid, soas: List[Soa]):
    update = False
    try:
        for soa in soas:
            update |= share_data.bpy_data_proxy.update_soa(uuid, soa.path, soa.members)
    except Exception:
        # Partial update of arrays may cause data length mismatch between array elements (co, normals, ...)
        logger.error(f"Exception during update_soa for {uuid} {soa.path}")
        logger.error(" -------  This may cause a Blender crash ---------")
        for line in traceback.format_exc().splitlines():
            logger.error(line)

    if update:
        # TODO this is skipped if the proxy update call is retained until returning into OBJECT mode
        try:
            # needed for Grease Pencil.
            bpy.context.view_layer.update()
        except Exception:
            pass


def build_data_update(buffer: bytes):
    if share_data.use_vrtist_protocol():
        return

    share_data.set_dirty()
    codec = Codec()
    try:
        message = BlenderDataMessage()
        message.decode(buffer)
        delta: Delta = codec.decode(message.proxy_string)
        logger.debug("%s: %s", "build_data_update", delta)
        delta.value.arrays = message.arrays
        share_data.bpy_data_proxy.update_datablock(delta)

        datablock_proxy = delta.value
        if datablock_proxy is not None:
            _build_soas(datablock_proxy.mixer_uuid, message.soas)

    except DecodeError as e:
        logger.error(f"Decode error for {str(e.args[1])[:100]} . Possible causes...")
        logger.error("... user error: version mismatch")
        logger.error("... internal error: Proxy class not registered. Import it in blender_data.__init__.py")
    except Exception:
        logger.error("Exception during build_data_update")
        for line in traceback.format_exc().splitlines():
            logger.error(line)
        logger.error(f"During processing of buffer for {delta}")
        logger.error(buffer[0:200])
        logger.error("...")
        logger.error(buffer[-200:0])
        logger.error("ignored")


def send_data_removals(removals: RemovalChangeset):
    if share_data.use_vrtist_protocol():
        return

    for uuid, _, debug_info in removals:
        logger.info("send_removal: %s (%s)", uuid, debug_info)
        buffer = BlenderRemoveMessage.encode(uuid, debug_info)
        command = Command(MessageType.BLENDER_DATA_REMOVE, buffer, 0)
        share_data.client.add_command(command)


def build_data_remove(buffer):
    if share_data.use_vrtist_protocol():
        return

    message = BlenderRemoveMessage()
    message.decode(buffer)
    logger.info("build_data_remove: %s (%s)", message.uuid, message.debug_info)
    share_data.bpy_data_proxy.remove_datablock(message.uuid)

    # TODO temporary until VRtist protocol uses Blenddata instead of blender_objects & co
    share_data.set_dirty()


def send_data_renames(renames: RenameChangeset):
    if not renames:
        return
    if share_data.use_vrtist_protocol():
        return

    items = []
    for uuid, old_name, new_name, debug_info in renames:
        logger.warning("send_rename: %s (%s) into %s", uuid, debug_info, new_name)
        items.extend([uuid, old_name, new_name])

    buffer = BlenderRenamesMessage.encode(items)
    command = Command(MessageType.BLENDER_DATA_RENAME, buffer, 0)
    share_data.client.add_command(command)


def build_data_rename(buffer):
    if share_data.use_vrtist_protocol():
        return

    message = BlenderRenamesMessage()
    message.decode(buffer)
    renames = message.renames

    # (uuid1, old1, new1, uuid2, old2, new2, ...) to ((uuid1, old1, new1), (uuid2, old2, new2), ...)
    args = [iter(renames)] * 3
    # do not consume the iterator on the log loop !
    items = list(itertools.zip_longest(*args))

    for uuid, old_name, new_name in items:
        logger.info("build_data_rename: %s (%s) into %s", uuid, old_name, new_name)

    rename_changeset = share_data.bpy_data_proxy.rename_datablocks(items)

    # TODO temporary until VRtist protocol uses Blenddata instead of blender_objects & co
    share_data.set_dirty()

    if rename_changeset:
        send_data_renames(rename_changeset)
