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
This module defines global state of the addon. It is encapsulated in a ShareData instance.
"""

from collections import namedtuple
from datetime import datetime
import logging
from typing import Dict, List, Mapping, Optional, Set
from uuid import uuid4

from mixer.blender_data.bpy_data_proxy import BpyDataProxy

import bpy
import bpy.types as T  # noqa N812

from mixer.shot_manager_data import ShotManager

logger = logging.getLogger(__name__)
ObjectVisibility = namedtuple("ObjectVisibility", ["hide_viewport", "hide_select", "hide_render", "hide_get"])

ObjectConstraint = namedtuple(
    "ObjectConstraint", ["has_parent_constraint", "parent_target", "has_look_at_constraint", "look_at_target"]
)


def object_visibility(o: bpy.types.Object):
    return ObjectVisibility(o.hide_viewport, o.hide_select, o.hide_render, o.hide_get())


def get_object_constraints(o: bpy.types.Object):
    has_parent_constraint = False
    parent_target = None
    has_look_at_constraint = False
    look_at_target = None

    for constraint in o.constraints:
        if constraint.type == "CHILD_OF":
            if constraint.target is not None:
                has_parent_constraint = True
                parent_target = constraint.target
        if constraint.type == "TRACK_TO":
            if constraint.target is not None:
                has_look_at_constraint = True
                look_at_target = constraint.target

    constraints = ObjectConstraint(has_parent_constraint, parent_target, has_look_at_constraint, look_at_target)
    return constraints


class CollectionInfo:
    def __init__(
        self,
        hide_viewport: bool,
        tmp_hide_viewport: bool,
        instance_offset,
        children: List[str],
        parent: List[str],
        objects: List[str] = None,
    ):
        self.hide_viewport = hide_viewport
        self.temporary_hide_viewport = tmp_hide_viewport
        self.instance_offset = instance_offset
        self.children = children
        self.parent = parent
        self.objects = objects or []


class SceneInfo:
    def __init__(self, scene: bpy.types.Scene):
        master_collection = scene.collection
        self.children = [x.name_full for x in master_collection.children]
        self.objects = [x.name_full for x in master_collection.objects]
        if not scene.mixer_uuid:
            scene.mixer_uuid = str(uuid4())
        self.mixer_uuid = scene.mixer_uuid


class ShareData:
    """
    ShareData is the class storing the global state of the addon.
    """

    def __init__(self):
        self.run_id = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        self.session_id = 0  # For logging and debug
        self.client = None

        self.local_server_process = None
        self.selected_objects_names = []

        self.pending_test_update = False

        self.clear_room_data()
        self.shot_manager = ShotManager()

    def clear_room_data(self):
        self.objects_added: Set(str) = set()
        self.objects_removed: Set(str) = set()
        self.collections_added: Set(str) = set()
        self.collections_removed: Set(str) = set()
        self.scenes_added: List[str] = []

        # key : collection name
        self.objects_added_to_collection: Mapping(str, str) = {}
        self.objects_removed_from_collection: Mapping(str, str) = {}
        self.collections_added_to_collection: Set(str, str) = set()
        self.collections_removed_from_collection: Set(str, str) = set()

        # key : scene name
        self.objects_added_to_scene: Mapping(str, str) = {}
        self.objects_removed_from_scene: Mapping(str, str) = {}
        self.collections_added_to_scene: Set(str, str) = set()
        self.collections_removed_from_scene: Set(str, str) = set()

        # All non master collections
        self.collections_info: Mapping[str, CollectionInfo] = {}

        # Master collections
        self.scenes_info: Mapping[str, SceneInfo] = {}

        self.objects_reparented = set()
        self.objects_parents = {}
        self.objects_renamed = {}
        self.objects_transformed = set()
        self.objects_transforms = {}
        self.objects_visibility_changed: Set[str] = set()
        self.objects_visibility: Mapping[str, ObjectVisibility] = {}
        self.objects_constraints_added: Set[str] = set()
        self.objects_constraints_removed: Set[str] = set()
        self.objects_constraints: Mapping[str, ObjectConstraint] = {}

        self.old_objects: Mapping[str, bpy.types.Object] = {}

        # {object_path: [collection_name]}
        self.restore_to_collections: Mapping[str, List[str]] = {}

        self._blender_objects = {}
        self.blender_objects_dirty = True

        self._blender_materials = {}
        self.blender_materials_dirty = True

        self._blender_meshes = {}
        self.blender_meshes_dirty = True

        self._blender_grease_pencils = {}
        self.blender_grease_pencils_dirty = True

        self._blender_cameras = {}
        self.blender_cameras_dirty = True

        self._blender_lights = {}
        self.blender_lights_dirty = True
        self._blender_collections: Mapping[str, bpy.types.Collection] = {}
        self.blender_collections_dirty = True

        # maps collection name to layerCollection
        self._blender_layer_collections: Mapping[str, bpy.types.LayerCollection] = {}
        self.blender_layer_collections_dirty = True
        # the layer collection is created when the collection is added to scene
        # keep track of the temporary visibility to apply when the layer collection will be created
        self.blender_collection_temporary_visibility: Mapping[str, bool] = {}

        self.pending_parenting = set()

        self.current_camera: str = ""
        self.start_frame = 0
        self.end_frame = 0

        self.bpy_data_proxy: Optional[BpyDataProxy] = None

    def leave_current_room(self):
        if self.client is not None:
            self.client.leave_room(share_data.client.current_room)
        self.clear_room_data()

        self._blender_scenes: Mapping[str, bpy.types.Scene] = {}
        self.blender_scenes_dirty = True

    def clear_before_state(self):
        # These objects contain the "before" state when entering the update_post handler
        # They must be empty before the first update so that the whole scene is sent
        self.old_objects = {}
        self.collections_info = {}
        self.scenes_info = {}

    def set_dirty(self):
        logging.debug("share_data.set_dirty")
        self.blender_objects_dirty = True
        self.blender_materials_dirty = True
        self.blender_meshes_dirty = True
        self.blender_grease_pencils_dirty = True
        self.blender_cameras_dirty = True
        self.blender_lights_dirty = True
        self.blender_collections_dirty = True
        self.blender_layer_collections_dirty = True
        self.blender_scenes_dirty = True

    def get_blender_property(self, property, property_dirty, elems):
        if not property_dirty:
            return property
        property = {x.name_full: x for x in elems}
        property_dirty = False
        return property

    @property
    def blender_objects(self):
        if not self.blender_objects_dirty:
            return self._blender_objects
        logger.debug("Updating blender_objects")
        self._blender_objects = {x.name_full: x for x in bpy.data.objects}
        self.blender_objects_dirty = False
        return self._blender_objects

    @property
    def blender_materials(self):
        if not self.blender_materials_dirty:
            return self._blender_materials
        self._blender_materials = {x.name_full: x for x in bpy.data.materials}
        self.blender_materials_dirty = False
        return self._blender_materials

    @property
    def blender_meshes(self):
        if not self.blender_meshes_dirty:
            return self._blender_meshes
        self._blender_meshes = {x.name_full: x for x in bpy.data.meshes}
        self.blender_meshes_dirty = False
        return self._blender_meshes

    @property
    def blender_grease_pencils(self):
        if not self.blender_grease_pencils_dirty:
            return self._blender_grease_pencils
        self._blender_grease_pencils = {x.name_full: x for x in bpy.data.grease_pencils}
        self.blender_grease_pencils_dirty = False
        return self._blender_grease_pencils

    @property
    def blender_cameras(self):
        if not self.blender_cameras_dirty:
            return self._blender_cameras
        self._blender_cameras = {x.name_full: x for x in bpy.data.cameras}
        self.blender_cameras_dirty = False
        return self._blender_cameras

    @property
    def blender_lights(self):
        if not self.blender_lights_dirty:
            return self._blender_lights
        self._blender_lights = {x.name_full: x for x in bpy.data.lights}
        self.blender_lights_dirty = False
        return self._blender_lights

    @property
    def blender_collections(self):
        if not self.blender_collections_dirty:
            return self._blender_collections
        self._blender_collections = {x.name_full: x for x in bpy.data.collections}
        self.blender_collections_dirty = False
        return self._blender_collections

    def recurs_blender_layer_collections(self, layer_collection):
        self._blender_layer_collections[layer_collection.collection.name_full] = layer_collection
        for child_collection in layer_collection.children:
            self.recurs_blender_layer_collections(child_collection)

    @property
    def blender_layer_collections(self):
        if not self.blender_layer_collections_dirty:
            return self._blender_layer_collections
        self._blender_layer_collections.clear()
        layer = bpy.context.view_layer
        layer_collections = layer.layer_collection.children
        for collection in layer_collections:
            self.recurs_blender_layer_collections(collection)
        self.blender_layer_collections_dirty = False
        return self._blender_layer_collections

    @property
    def blender_scenes(self):
        if not self.blender_scenes_dirty:
            return self._blender_scenes
        logger.debug("Updating blender_scenes")
        self._blender_scenes = {x.name_full: x for x in bpy.data.scenes}
        self.blender_scenes_dirty = False
        return self._blender_scenes

    def clear_changed_frame_related_lists(self):
        self.objects_transformed.clear()

    def clear_lists(self):
        """
        Clear the lists that record change between previous and current state
        """
        self.scenes_added.clear()

        self.collections_added.clear()
        self.collections_removed.clear()

        self.collections_added_to_collection.clear()
        self.collections_removed_from_collection.clear()
        self.objects_added_to_collection.clear()
        self.objects_removed_from_collection.clear()

        self.objects_added_to_scene.clear()
        self.objects_removed_from_scene.clear()
        self.collections_added_to_scene.clear()
        self.collections_removed_from_scene.clear()

        self.objects_reparented.clear()
        self.objects_renamed.clear()
        self.objects_visibility_changed.clear()
        self.objects_constraints_added.clear()
        self.objects_constraints_removed.clear()
        self.clear_changed_frame_related_lists()

    def update_scenes_info(self):
        logging.debug("update_scenes_info")
        self.scenes_info = {scene.name_full: SceneInfo(scene) for scene in self.blender_scenes.values()}

    # apply temporary visibility to layerCollection
    def update_collection_temporary_visibility(self, collection_name):
        bpy.context.view_layer.update()
        temporary_visible = self.blender_collection_temporary_visibility.get(collection_name)
        if temporary_visible is not None:
            share_data.blender_layer_collections_dirty = True
            layer_collection = self.blender_layer_collections.get(collection_name)
            if layer_collection:
                layer_collection.hide_viewport = not temporary_visible
            del self.blender_collection_temporary_visibility[collection_name]

    def update_collections_info(self):
        self.collections_info = {}

        # All non master collections
        for collection in self.blender_collections.values():
            if not self.collections_info.get(collection.name_full):
                layer_collection = self.blender_layer_collections.get(collection.name_full)
                temporary_hidden = False
                if layer_collection:
                    temporary_hidden = layer_collection.hide_viewport
                collection_info = CollectionInfo(
                    collection.hide_viewport,
                    temporary_hidden,
                    collection.instance_offset,
                    [x.name_full for x in collection.children],
                    None,
                )
                self.collections_info[collection.name_full] = collection_info
            for child in collection.children:
                temporary_hidden = False
                child_layer_collection = self.blender_layer_collections.get(child.name_full)
                if child_layer_collection:
                    temporary_hidden = child_layer_collection.hide_viewport
                collection_info = CollectionInfo(
                    child.hide_viewport,
                    temporary_hidden,
                    child.instance_offset,
                    [x.name_full for x in child.children],
                    collection.name_full,
                )
                self.collections_info[child.name_full] = collection_info

        # Store non master collections objects
        for collection in self.blender_collections.values():
            self.collections_info[collection.name_full].objects = [x.name_full for x in collection.objects]

    def update_objects_info(self):
        self.old_objects = self.blender_objects

        self.objects_transforms = {}
        for obj in self.blender_objects.values():
            self.objects_transforms[obj.name_full] = obj.matrix_local.copy()

    def sanitize_blender_ids(self, id_dict: Dict[str, T.ID], is_dirty: bool) -> Dict[str, T.ID]:
        if is_dirty:
            # avoid useless warnings if we are to rebuild anyway
            return id_dict

        # todo investigate this
        # the classic error is ReferenceError: StructRNA of type Object has been removed
        # I think we should remove the lazy update of dicts, because references become stale, we don't know why
        # in the meantime we need this to ensure we avoid crashes in production
        sanitized = {}
        for _key, value in id_dict.items():
            try:
                sanitized[
                    value.name_full
                ] = value  # the access value.name_full should trigger the error if the ID is invalid
            except ReferenceError as e:
                logger.error(f"{e!r}")
        return sanitized

    def update_current_data(self):
        self._blender_objects = self.sanitize_blender_ids(self._blender_objects, self.blender_objects_dirty)

        self.update_scenes_info()
        self.update_collections_info()
        self.update_objects_info()
        self.objects_visibility = {x.name_full: object_visibility(x) for x in self.blender_objects.values()}
        self.objects_constraints = {x.name_full: get_object_constraints(x) for x in self.blender_objects.values()}
        self.objects_parents = {
            x.name_full: x.parent.name_full if x.parent is not None else "" for x in self.blender_objects.values()
        }

    def init_protocol(self, vrtist_protocol: bool, shared_folders: List):
        if not vrtist_protocol:
            logger.warning("Generic protocol sync is ON")
            self.bpy_data_proxy = BpyDataProxy()
            if shared_folders is not None:
                logger.warning("Setting shared folders: " + str(shared_folders))
            else:
                logger.warning("No shared folder set")
            self.bpy_data_proxy.set_shared_folders(shared_folders)
        else:
            logger.warning("VRtist protocol sync is ON")
            if self.bpy_data_proxy:
                self.bpy_data_proxy = None

    def use_vrtist_protocol(self):
        return self.bpy_data_proxy is None

    def receive_sanity_check(self):
        if self.bpy_data_proxy:
            self.bpy_data_proxy.sanity_check()


share_data = ShareData()  # Instance storing addon state, is used by most of the sub-modules.
