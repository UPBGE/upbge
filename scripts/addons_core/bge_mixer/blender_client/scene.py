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


def send_scene(client: Client, scene_name: str):
    logger.info("send_scene %s", scene_name)
    buffer = common.encode_string(scene_name)
    client.add_command(common.Command(common.MessageType.SCENE, buffer, 0))


def delete_scene(scene) -> bool:
    # Due to bug mentionned here https://developer.blender.org/T71422, deleting a scene with D.scenes.remove()
    # in a function called from a timer gives a hard crash. This is due to context.window being None.
    # To overcome this issue, we call an operator with a custom context that define window.
    # https://devtalk.blender.org/t/new-timers-have-no-context-object-why-is-that-so-cant-override-it/6802
    def window():
        for window in bpy.context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == "VIEW_3D":
                    return window

    ctx = {"window": window(), "scene": scene}
    logger.info(f"deleting scene {scene} ...")
    try:
        bpy.ops.scene.delete(ctx)
        logger.info(f"... OK. Remaining scenes: {bpy.data.scenes.keys()}")
        return True
    except RuntimeError as e:
        logger.info(f"delete_scene {scene}: exception {e!r}")
        return False


def build_scene(data):
    scene_name, _ = common.decode_string(data, 0)
    logger.warning("build_scene %s (VRtist)", scene_name)

    # remove what was previously the last scene that could not be removed
    to_remove = None
    if len(bpy.data.scenes) == 1 and bpy.data.scenes[0].name == "_mixer_to_be_removed_":
        to_remove = bpy.data.scenes[0]

    scene = share_data.blender_scenes.get(scene_name)
    if scene is None:
        scene = bpy.data.scenes.new(scene_name)
        if len(bpy.data.worlds):
            scene.world = bpy.data.worlds[0]
        share_data.blender_scenes[scene_name] = scene

    if to_remove is not None:
        delete_scene(to_remove)


def send_add_collection_to_scene(client: Client, scene_name: str, collection_name: str):
    logger.info("send_add_collection_to_scene %s <- %s", scene_name, collection_name)

    buffer = common.encode_string(scene_name) + common.encode_string(collection_name)
    client.add_command(common.Command(common.MessageType.ADD_COLLECTION_TO_SCENE, buffer, 0))


def build_collection_to_scene(data):
    scene_name, index = common.decode_string(data, 0)
    collection_name, _ = common.decode_string(data, index)

    # This message is not emitted by VRtist, only by Blender, so it is used only for Blender/Blender sync.
    # In generic mode, it conflicts with generic messages, so drop it
    if not share_data.use_vrtist_protocol():
        logger.warning("build_collection_to_scene %s <- %s", scene_name, collection_name)
        return

    logger.info("build_collection_to_scene %s <- %s", scene_name, collection_name)

    try:
        scene = share_data.blender_scenes[scene_name]
    except KeyError:
        if not share_data.use_vrtist_protocol():
            # Removed by the Blender Protocol
            logger.info(f"build_collection_to_scene(): scene not found {scene_name}. Safe in generic mode ...")
            return
        else:
            raise

    collection = share_data.blender_collections[collection_name]
    try:
        scene.collection.children.link(collection)
    except RuntimeError as e:
        if not share_data.use_vrtist_protocol():
            # Added by the Blender Protocol
            logger.info(f"build_collection_to_scene(): scene {scene_name}, collection {collection_name}...")
            logger.info("... Exception during scene.collection.children.link() ...")
            logger.info("... Safe in generic mode ...")
            logger.info(f"... {e!r}")
        else:
            raise
    share_data.update_collection_temporary_visibility(collection_name)


def send_remove_collection_from_scene(client: Client, scene_name: str, collection_name: str):
    logger.info("send_remove_collection_from_scene %s <- %s", scene_name, collection_name)

    buffer = common.encode_string(scene_name) + common.encode_string(collection_name)
    client.add_command(common.Command(common.MessageType.REMOVE_COLLECTION_FROM_SCENE, buffer, 0))


def build_remove_collection_from_scene(data):
    scene_name, index = common.decode_string(data, 0)
    collection_name, _ = common.decode_string(data, index)

    # This message is not emitted by VRtist, only by Blender, so it is used only for Blender/Blender sync.
    # In generic mode, it conflicts with generic messages, so drop it
    if not share_data.use_vrtist_protocol():
        logger.warning("build_remove_collection_from_scene  %s <- %s", scene_name, collection_name)
        return

    logger.info("build_remove_collection_from_scene %s <- %s", scene_name, collection_name)
    scene = share_data.blender_scenes[scene_name]
    collection = share_data.blender_collections.get(collection_name)
    if collection:
        # otherwise already removed by Blender protocol
        try:
            scene.collection.children.unlink(collection)
        except Exception as e:
            logger.info("build_remove_collection_from_scene: exception during unlink... ")
            logger.info(f"... {e!r} ")


def send_add_object_to_vrtist(client: Client, scene_name: str, obj_name: str):
    logger.debug("send_add_object_to_vrtist %s <- %s", scene_name, obj_name)
    buffer = common.encode_string(scene_name) + common.encode_string(obj_name)
    client.add_command(common.Command(common.MessageType.ADD_OBJECT_TO_VRTIST, buffer, 0))


def send_add_object_to_scene(client: Client, scene_name: str, obj_name: str):
    logger.info("send_add_object_to_scene %s <- %s", scene_name, obj_name)
    buffer = common.encode_string(scene_name) + common.encode_string(obj_name)
    client.add_command(common.Command(common.MessageType.ADD_OBJECT_TO_SCENE, buffer, 0))


def build_add_object_to_scene(data):
    scene_name, index = common.decode_string(data, 0)
    object_name, _ = common.decode_string(data, index)
    logger.info("build_add_object_to_scene %s <- %s", scene_name, object_name)

    try:
        scene = share_data.blender_scenes[scene_name]
    except KeyError:
        if not share_data.use_vrtist_protocol():
            # Removed by the Blender Protocol
            logger.info(f"build_collection_to_scene(): scene not found {scene_name}. Safe in generic mode ...")
            return
        else:
            raise

    # We may have received an object creation message before this collection link message
    # and object creation will have created and linked the collecetion if needed
    if scene.collection.objects.get(object_name) is None:
        object_ = share_data.blender_objects[object_name]
        scene.collection.objects.link(object_)


def send_remove_object_from_scene(client: Client, scene_name: str, object_name: str):
    logger.info("send_remove_object_from_scene %s <- %s", scene_name, object_name)
    buffer = common.encode_string(scene_name) + common.encode_string(object_name)
    client.add_command(common.Command(common.MessageType.REMOVE_OBJECT_FROM_SCENE, buffer, 0))


def build_remove_object_from_scene(data):

    # TODO ckeck if obsolete
    scene_name, index = common.decode_string(data, 0)
    object_name, _ = common.decode_string(data, index)
    logger.info("build_remove_object_from_scene %s <- %s", scene_name, object_name)
    scene = share_data.blender_scenes[scene_name]
    object_ = share_data.blender_objects.get(object_name)
    if object_:
        # otherwise already removed by Blender protocol
        try:
            scene.collection.objects.unlink(object_)
        except Exception as e:
            logger.info("build_remove_object_from_scene: exception during unlink... ")
            logger.info(f"... {e!r} ")
