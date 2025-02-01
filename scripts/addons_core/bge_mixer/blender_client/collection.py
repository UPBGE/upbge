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


def send_collection(client: Client, collection: bpy.types.Collection):
    logger.info("send_collection %s", collection.name_full)
    collection_instance_offset = collection.instance_offset
    temporary_visibility = True
    layer_collection = share_data.blender_layer_collections.get(collection.name_full)
    if layer_collection:
        temporary_visibility = not layer_collection.hide_viewport

    buffer = (
        common.encode_string(collection.name_full)
        + common.encode_bool(not collection.hide_viewport)
        + common.encode_vector3(collection_instance_offset)
        + common.encode_bool(temporary_visibility)
    )
    client.add_command(common.Command(common.MessageType.COLLECTION, buffer, 0))


def build_collection(data):
    name_full, index = common.decode_string(data, 0)

    # This message is not emitted by VRtist, only by Blender, so it is used only for Blender/Blender sync.
    # In generic mode, it conflicts with generic messages, so drop it
    if not share_data.use_vrtist_protocol():
        logger.warning("build_collection %s, ignored in generic mode", name_full)
        return

    # Blender/Blender in VRtist (non generic) mode
    visible, index = common.decode_bool(data, index)
    hide_viewport = not visible
    offset, index = common.decode_vector3(data, index)
    temporary_visibility, index = common.decode_bool(data, index)

    logger.info("build_collection %s", name_full)
    collection = share_data.blender_collections.get(name_full)
    if collection is None:
        collection = bpy.data.collections.new(name_full)
        share_data.blender_collections[name_full] = collection

    collection.hide_viewport = hide_viewport
    collection.instance_offset = offset

    layer_collection = share_data.blender_layer_collections.get(name_full)
    if layer_collection:
        layer_collection.hide_viewport = not temporary_visibility
    else:
        # if the layer collection does not exists, store its state for later
        share_data.blender_collection_temporary_visibility[name_full] = temporary_visibility


def send_collection_removed(client: Client, collection_name):
    logger.info("send_collection_removed %s", collection_name)
    buffer = common.encode_string(collection_name)
    client.add_command(common.Command(common.MessageType.COLLECTION_REMOVED, buffer, 0))


def build_collection_removed(data):
    name_full, index = common.decode_string(data, 0)

    # This message is not emitted by VRtist, only by Blender, so it is used only for Blender/Blender sync.
    # In generic mode, it conflicts with generic messages, so drop it
    if not share_data.use_vrtist_protocol():
        logger.warning("build_collection_remove %s, ignore in generic mode", name_full)
        return

    # Blender/Blender in VRtist (non generic) mode
    logger.info("build_collectionRemove %s", name_full)
    collection = share_data.blender_collections.get(name_full)
    if collection:
        # otherwise already removed by Blender protocol
        try:
            del share_data.blender_collections[name_full]
            bpy.data.collections.remove(collection)
        except Exception as e:
            logger.info("build_remove_collection_from_scene: exception during unlink... ")
            logger.info(f"... {e!r} ")


def send_add_collection_to_collection(client: Client, parent_collection_name, collection_name):
    logger.info("send_add_collection_to_collection %s <- %s", parent_collection_name, collection_name)

    buffer = common.encode_string(parent_collection_name) + common.encode_string(collection_name)
    client.add_command(common.Command(common.MessageType.ADD_COLLECTION_TO_COLLECTION, buffer, 0))


def build_collection_to_collection(data):
    parent_name, index = common.decode_string(data, 0)
    child_name, _ = common.decode_string(data, index)

    # This message is not emitted by VRtist, only by Blender, so it is used only for Blender/Blender sync.
    # In generic mode, it conflicts with generic messages, so drop it
    if not share_data.use_vrtist_protocol():
        logger.warning("build_collection_to_collection %s <- %s, ignore in generic mode", parent_name, child_name)
        return

    logger.info("build_collection_to_collection %s <- %s", parent_name, child_name)
    parent = share_data.blender_collections[parent_name]

    child = share_data.blender_collections[child_name]

    try:
        parent.children.link(child)
    except RuntimeError as e:
        if not share_data.use_vrtist_protocol():
            # Added by the Blender Protocol
            logger.info(f"build_collection_to_collection(): parent {parent_name}, child {child_name}...")
            logger.info("... Exception during parent.children.link() ...")
            logger.info("... Safe in generic mode ...")
            logger.info(f"... {e!r}")
        else:
            logger.warning(f"build_collection_to_collection(): parent {parent_name}, child {child_name}...")
            logger.warning("... Exception during parent.children.link() ...")
            logger.warning(f"... {e!r}")


def send_remove_collection_from_collection(client: Client, parent_collection_name, collection_name):
    logger.info("send_remove_collection_from_collection %s <- %s", parent_collection_name, collection_name)

    buffer = common.encode_string(parent_collection_name) + common.encode_string(collection_name)
    client.add_command(common.Command(common.MessageType.REMOVE_COLLECTION_FROM_COLLECTION, buffer, 0))


def build_remove_collection_from_collection(data):
    parent_name, index = common.decode_string(data, 0)
    child_name, _ = common.decode_string(data, index)

    # This message is not emitted by VRtist, only by Blender, so it is used only for Blender/Blender sync.
    # In generic mode, it conflicts with generic messages, so drop it
    if not share_data.use_vrtist_protocol():
        logger.warning(
            "build_remove_collection_from_collection %s <- %s, ignore in generic mode", parent_name, child_name
        )
        return

    logger.info("build_remove_collection_from_collection %s <- %s", parent_name, child_name)

    parent = share_data.blender_collections[parent_name]
    child = share_data.blender_collections[child_name]
    parent.children.unlink(child)


def send_add_object_to_collection(client: Client, collection_name, obj_name):
    logger.info("send_add_object_to_collection %s <- %s", collection_name, obj_name)
    buffer = common.encode_string(collection_name) + common.encode_string(obj_name)
    client.add_command(common.Command(common.MessageType.ADD_OBJECT_TO_COLLECTION, buffer, 0))


def build_add_object_to_collection(data):
    collection_name, index = common.decode_string(data, 0)
    object_name, _ = common.decode_string(data, index)

    # This message is not emitted by VRtist, only by Blender, so it is used only for Blender/Blender sync.
    # In generic mode, it conflicts with generic messages, so drop it
    if not share_data.use_vrtist_protocol():
        logger.warning("build_add_object_to_collection %s <- %s, ignore in generic mode", collection_name, object_name)
        return
    logger.info("build_add_object_to_collection %s <- %s", collection_name, object_name)

    collection = share_data.blender_collections[collection_name]

    # We may have received an object creation message before this collection link message
    # and object creation will have created and linked the collection if needed
    if collection.objects.get(object_name) is None:
        object_ = share_data.blender_objects[object_name]
        collection.objects.link(object_)


def send_remove_object_from_collection(client: Client, collection_name, obj_name):
    logger.info("send_remove_object_from_collection %s <- %s", collection_name, obj_name)
    buffer = common.encode_string(collection_name) + common.encode_string(obj_name)
    client.add_command(common.Command(common.MessageType.REMOVE_OBJECT_FROM_COLLECTION, buffer, 0))


def build_remove_object_from_collection(data):
    collection_name, index = common.decode_string(data, 0)
    object_name, _ = common.decode_string(data, index)

    # This message is not emitted by VRtist, only by Blender, so it is used only for Blender/Blender sync.
    # In generic mode, it conflicts with generic messages, so drop it
    if not share_data.use_vrtist_protocol():
        logger.warning(
            "build_remove_object_from_collection %s <- %s, ignore in generic mode", collection_name, object_name
        )
        return

    logger.info("build_remove_object_from_collection %s <- %s", collection_name, object_name)

    collection = share_data.blender_collections[collection_name]
    object_ = share_data.blender_objects.get(object_name)
    if object_:
        # otherwise already removed by Blender protocol
        try:
            collection.objects.unlink(object_)
        except Exception as e:
            logger.info("build_remove_object_from_collection: exception during unlink... ")
            logger.info(f"... {e!r} ")


def send_collection_instance(client: Client, obj):
    if not obj.instance_collection:
        return
    instance_name = obj.name_full
    instanciated_collection = obj.instance_collection.name_full
    buffer = common.encode_string(instance_name) + common.encode_string(instanciated_collection)
    client.add_command(common.Command(common.MessageType.INSTANCE_COLLECTION, buffer, 0))


def build_collection_instance(data):
    instance_name, index = common.decode_string(data, 0)
    instantiated_name, _ = common.decode_string(data, index)

    # This message is not emitted by VRtist, only by Blender, so it is used only for Blender/Blender sync.
    # In generic mode, it conflicts with generic messages, so drop it
    if not share_data.use_vrtist_protocol():
        logger.warning("build_collection_instance %s <- %s, ignore in generic mode", instantiated_name, instance_name)
        return

    logger.info("build_collection_instance %s from %s", instantiated_name, instance_name)

    instantiated = share_data.blender_collections[instantiated_name]

    instance = bpy.data.objects.new(name=instance_name, object_data=None)
    instance.instance_collection = instantiated
    instance.instance_type = "COLLECTION"

    share_data.blender_objects[instance_name] = instance
