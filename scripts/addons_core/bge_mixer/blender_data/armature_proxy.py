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
Proxy for Armature datablock.

See synchronization.md
"""
from __future__ import annotations

import functools
import logging
from typing import Any, Callable, List, Optional, Union, TYPE_CHECKING

import bpy
import bpy.types as T  # noqa

from mixer.blender_data.attributes import apply_attribute, diff_attribute, read_attribute, write_attribute
from mixer.blender_data.bpy_data_proxy import DeltaUpdate
from mixer.blender_data.datablock_proxy import DatablockProxy
from mixer.blender_data.json_codec import serialize

if TYPE_CHECKING:
    from mixer.blender_data.bpy_data_proxy import Context
    from mixer.blender_data.proxy import Delta
    from mixer.blender_data.struct_proxy import StructProxy


DEBUG = True

logger = logging.getLogger(__name__)


class Command:
    """Rudimentary command to handle Blender state changes."""

    def __init__(self, do: Callable[[], None], undo: Callable[[], None], text: str):
        self._do = do
        self._undo = undo
        self._text = text

    def do(self):
        # logger.info("DO      " + self._text)
        try:
            self._do()
        except Exception as e:
            logger.error("DO exception ...")
            logger.error(f"... {self._text}")
            logger.error(f"... {e!r}")

    def undo(self):
        # logger.info("UNDO    " + self._text)
        try:
            self._undo()
        except Exception as e:
            logger.error("UNDO exception ...")
            logger.error(f"... {self._text}")
            logger.error(f"... {e!r}")


class Commands:
    """Rudimentary command stack to handle Blender state changes."""

    def __init__(self, text: str = ""):
        self._text = text
        self._commands: List[Command] = []

    def append(self, command: Command):
        self._commands.append(command)

    def do(self):
        # if self._commands and self._text:
        #     logger.info("DO   -- begin " + self._text)
        for command in self._commands:
            command.do()

    def undo(self):
        for command in reversed(self._commands):
            command.undo()
        # if self._commands and self._text:
        #     logger.info("UNDO -- end    " + self._text)


def _set_active_object(obj: T.Object):
    bpy.context.view_layer.objects.active = obj


def _armature_object(armature_data: T.Armature, context: Context) -> Optional[T.Object]:
    """Returns an Object that uses datablock"""
    objects = context.proxy_state.objects_using_data(armature_data)
    if not objects:
        # Armature without Object (orphan Armature)
        # TODO ensure that the Armature data is synced after being referenced by an Object
        logger.error(f"load: no Object for {armature_data!r} at {context.visit_state.display_path()} ...")
        logger.error(".. Armature data not synchronized")
        return None

    # TODO what if the same Armature is linked to several Object datablocks
    if len(objects) > 1:
        logger.warning(f"multiple parents for {armature_data!r}")

    return objects[0]


@serialize
class ArmatureProxy(DatablockProxy):
    """Proxy for an Armature datablock.

    This specialization is required to switch between current mode and edit mode in order to read/write edit_bones.
    """

    _edit_bones_property = T.Armature.bl_rna.properties["edit_bones"]

    _require_context_state = (
        # requires EDIT mode
        "edit_bones",
        # requires bpy.context.object. State stage will be too much, never mind
        "rigify_layers",
    )
    """These members require proper state change."""

    def load(self, armature_data: T.Armature, context: Context) -> ArmatureProxy:
        proxy = super().load(armature_data, context)
        self._custom_properties.load(armature_data)
        assert proxy is self

        # Do not use _armature_object as the user Object has not yet been registered in ProxyState
        armature_objects = [object for object in bpy.data.objects if object.data is armature_data]
        if not armature_objects:
            # Armature without Object (orphan Armature)
            # TODO ensure that the Armature data is synced after being referenced by an Object
            logger.error(f"load: no Object for {armature_data!r} at {context.visit_state.display_path()} ...")
            logger.error(".. Armature data not synchronized")
            return self

        def _read_attribute():
            self._data["edit_bones"] = read_attribute(
                armature_data.edit_bones, "edit_bones", self._edit_bones_property, armature_data, context
            )

        self._access_edit_bones(armature_objects[0], _read_attribute, context)
        return self

    def _save(self, armature_data: T.ID, context: Context) -> T.ID:
        # This is called when the Armature datablock is created. However, edit_bones can only be edited after the
        # armature Object is created and in EDIT mode.
        # So skip proxies that need proper state and ObjectProxy will call update_edit_bones() later
        proxies_needing_state = {
            name: self._data.pop(name, None) for name in self._require_context_state if name in self._data
        }
        datablock = super()._save(armature_data, context)

        # restore the bypassed proxies and apply them with context change
        # WARNING: this reorders proxy items
        self._data.update(proxies_needing_state)

        return datablock

    def _diff(
        self, armature_data: T.Armature, key: str, prop: T.Property, context: Context, diff: StructProxy
    ) -> Optional[Delta]:

        delta = super()._diff(armature_data, key, prop, context, diff)

        armature_object = _armature_object(armature_data, context)
        if not armature_object:
            return delta

        def _diff_attribute():
            return diff_attribute(
                armature_data.edit_bones, "edit_bones", self._edit_bones_property, self.data("edit_bones"), context
            )

        edit_bones_delta = self._access_edit_bones(armature_object, _diff_attribute, context)

        if edit_bones_delta is not None:
            if delta is None:
                # create an empty DeltaUpdate
                diff = self.make(armature_data)
                delta = DeltaUpdate(diff)

            # attach the edit_bones delta to the armature delta
            delta.value._data["edit_bones"] = edit_bones_delta

        return delta

    def apply(
        self,
        armature_data: T.Armature,
        parent: T.BlendDataObjects,
        key: Union[int, str],
        delta: Delta,
        context: Context,
        to_blender: bool = True,
    ) -> StructProxy:
        """
        Apply delta to this proxy and optionally to the Blender attribute its manages.

        Args:
            attribute: the Object datablock to update
            parent: the attribute that contains attribute (e.g. a bpy.data.objects)
            key: the key that identifies attribute in parent.
            delta: the delta to apply
            context: proxy and visit state
            to_blender: update the managed Blender attribute in addition to this Proxy
        """
        assert isinstance(key, str)

        if not to_blender:
            return super().apply(armature_data, parent, key, delta, context, to_blender)

        update = delta.value

        # find proxies that need proper state and keep them aside for _access_edit_bones
        proxies_needing_state = {
            name: update._data.pop(name, None) for name in self._require_context_state if name in update._data
        }
        updated_proxy = super().apply(armature_data, parent, key, delta, context, to_blender)
        assert updated_proxy is self

        if not proxies_needing_state:
            # no update requires state change
            return self

        armature_object = _armature_object(armature_data, context)
        if not armature_object:
            # TOD0 how is it possible ?
            return self

        # restore the bypassed proxies and apply them with context change
        update._data.update(proxies_needing_state)

        def _apply_attribute():
            for name in proxies_needing_state.keys():
                self._data[name] = apply_attribute(
                    armature_data, name, self._data[name], update._data[name], context, to_blender
                )

        self._access_edit_bones(armature_object, _apply_attribute, context)
        return self

    @staticmethod
    def update_edit_bones(armature_object: T.Object, context: Context):
        # The Armature datablock is required to create an armature Object, so it has already been received and
        # created. When the Armature datablock was created, its edit_bones member could not be updated since it
        # requires an Object in EDIT mode to be accessible.
        # this implements the update
        assert isinstance(armature_object.data, T.Armature)

        armature_data_uuid = armature_object.data.mixer_uuid
        armature_data_proxy = context.proxy_state.proxies[armature_data_uuid]
        assert isinstance(armature_data_proxy, ArmatureProxy)

        def _write_attribute():
            for name in armature_data_proxy._require_context_state:
                write_attribute(armature_object.data, name, armature_data_proxy._data[name], context)

        armature_data_proxy._access_edit_bones(armature_object, _write_attribute, context)

    def _access_edit_bones(self, object: T.Object, access: Callable[[], Any], context: Context) -> Any:

        # The operators used here do not require a context override, and work even if there is no VIEW_3D when
        # the received update is processed

        update_state_commands = Commands("access_edit_bones")

        if object not in bpy.context.view_layer.objects.values():
            #
            # link armature Object to scene collection
            #

            # the Armature object is not linked to the view layer. Possible reasons:
            # - it is not linked in the source blender data
            # - the code path that created the Armature on the source has not yet linked it to a collection
            # - it is only linked to collections excluded from the view_layer
            # Temporarily link to the current view layer

            # TODO this code would be simpler with full Command implementations, such as
            # command = ObjectsLinkCommand(objects, object)

            objects = bpy.context.view_layer.layer_collection.collection.objects
            command = Command(
                lambda: objects.link(object),
                lambda: objects.unlink(object),
                f"temp link {object!r} to view_layer collection",
            )
            update_state_commands.append(command)

        previous_active_object = bpy.context.view_layer.objects.active

        if previous_active_object is not object:
            #
            # only one object can be in non edit mode : reset active object mode to OBJECT
            #
            if previous_active_object is not None:
                if previous_active_object.mode != "OBJECT":
                    command = Command(
                        lambda: bpy.ops.object.mode_set(mode="OBJECT"),
                        lambda: bpy.ops.object.mode_set(mode=previous_active_object.mode),
                        f"set mode to OBJECT for {previous_active_object!r}",
                    )
                    update_state_commands.append(command)

            #
            # set armature Object as active
            #
            command = Command(
                functools.partial(_set_active_object, object),
                functools.partial(_set_active_object, previous_active_object),
                f"change active_object from {previous_active_object!r} to {object!r}",
            )
            update_state_commands.append(command)

        #
        # change armature Object mode to EDIT
        #
        object_mode = object.mode
        if object_mode != "EDIT":
            # During ObjectProxy.save(), setting mode to EDIT will trigger a depsgraph update that includes
            # an Armature drivers evaluation, but we have not yet saved the bones. This triggers errors like
            #
            #   WARN (bke.anim_sys): C:\...\anim_sys.c:2991 BKE_animsys_eval_driver: invalid driver - bones["DEF-upper_arm.R.001"].bbone_easein[0]
            #
            # TODO: Avoiding the error would require to save Armature.animation_data after Armature.edit_bones
            # TODO: Find out if the error message is a problem or if the computation will anyway be correct next time
            command = Command(
                lambda: bpy.ops.object.mode_set(mode="EDIT"),
                lambda: bpy.ops.object.mode_set(mode=object_mode),
                f"set mode to 'EDIT' for {object!r}",
            )
            update_state_commands.append(command)
        edit_bones_length = 0
        try:
            update_state_commands.do()
            result = access()
            edit_bones_length = len(object.data.edit_bones)

        except Exception as e:
            logger.warning(f"_access_edit_bones: at {context.visit_state.display_path()}...")
            logger.warning(f"... {e!r}")
        else:
            return result
        finally:
            try:
                update_state_commands.undo()
                if len(object.data.bones) != edit_bones_length:
                    # some partial updates with rigify caused loss of bones after exiting EDIT mode.
                    # This was hacked around by sending full updated for bones (see diff_must_replace)
                    logger.error(
                        f"bones length doe not match: edit {edit_bones_length}, object: {len(object.data.bones)}"
                    )
            except Exception as e:
                logger.error("_access_edit_bones: cleanup exception ...")
                logger.error(f"... {e!r}")
