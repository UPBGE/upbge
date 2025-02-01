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
This module defines how Shot Manager messages are handled.

Shot Manager is another addon we develop and that can be controlled through Mixer.
We plan to extract this code in a plug-in system in the future to avoid polluting the core of Mixer.
"""

from enum import IntEnum

import bpy
from mixer.share_data import share_data
from mixer.shot_manager_data import Shot

import mixer.broadcaster.common as common

try:
    from shotmanager.api import shot_manager
    from shotmanager.api import shot
    from shotmanager.api import take
except Exception:
    pass


class SMAction(IntEnum):
    ADD_SHOT = 0
    DELETE_SHOT = 1
    DUPLICATE_SHOT = 2
    MOVE_SHOT = 3
    UPDATE_SHOT = 4


def get_shot_manager():
    sm_props = None
    try:
        sm_props = shot_manager.get_shot_manager(bpy.context.scene)
        shot_manager.initialize_shot_manager(sm_props)
    except Exception:
        pass
    return sm_props


def get_or_set_current_take(sm_props):
    current_take = shot_manager.get_current_take(sm_props)
    if not current_take:
        current_take = shot_manager.add_take(sm_props, at_index=-1, name="Main Take")
    return current_take


def build_shot_manager_action(data):
    sm_props = get_shot_manager()
    if sm_props is None:
        return
    get_or_set_current_take(sm_props)

    index = 0
    action, index = common.decode_int(data, index)
    shot_index, index = common.decode_int(data, index)
    # bpy.context.scene.UAS_shot_manager_props.selected_shot_index = shot_index
    bpy.context.scene.UAS_shot_manager_props.setSelectedShotByIndex(shot_index)

    # Add
    if action == SMAction.ADD_SHOT:
        shot_name, index = common.decode_string(data, index)
        start, index = common.decode_int(data, index)
        end, index = common.decode_int(data, index)
        camera_name, index = common.decode_string(data, index)
        camera = None
        if len(camera_name) > 0:
            camera = bpy.data.objects[camera_name]

        color, index = common.decode_color(data, index)

        # bpy.context.scene.UAS_shot_manager_props.get_isInitialized()
        shot_manager.add_shot(
            sm_props,
            at_index=shot_index,
            take_index=-1,
            name=shot_name,
            start=start,  # avoid using a short start value before the lenght of the handles (which is 10)
            end=end,
            camera=camera,
            color=(color[0], color[1], color[2], 1),
            enabled=True,
        )
    # Delete
    elif action == SMAction.DELETE_SHOT:
        s = shot_manager.get_shot(sm_props, shot_index)
        shot_manager.remove_shot(sm_props, s)
    # Duplicate
    elif action == SMAction.DUPLICATE_SHOT:
        s = shot_manager.get_shot(sm_props, shot_index)
        new_shot = shot_manager.copy_shot(sm_props, shot=s, at_index=shot_index + 1)
        shot_name, index = common.decode_string(data, index)
        shot.set_name(new_shot, shot_name)
    # Move
    elif action == SMAction.MOVE_SHOT:
        s = shot_manager.get_shot(sm_props, shot_index)
        offset, index = common.decode_int(data, index)
        shot_manager.move_shot_to_index(sm_props, shot=s, new_index=(shot_index + offset))
    # Update
    elif action == SMAction.UPDATE_SHOT:
        # take = bpy.context.scene.UAS_shot_manager_props.current_take_name
        start, index = common.decode_int(data, index)
        end, index = common.decode_int(data, index)
        camera, index = common.decode_string(data, index)
        color, index = common.decode_color(data, index)
        enabled, index = common.decode_int(data, index)
        s = shot_manager.get_shot(sm_props, shot_index)
        if start > -1:
            shot.set_start(s, start)
        if end > -1:
            shot.set_end(s, end)
        if len(camera) > 0:
            shot.set_camera(s, bpy.data.objects[camera])
        if enabled != -1:
            shot.set_enable_state(s, enabled)


def send_montage_mode():
    buffer = common.encode_bool(share_data.shot_manager.montage_mode)
    share_data.client.add_command(common.Command(common.MessageType.SHOT_MANAGER_MONTAGE_MODE, buffer, 0))


def check_montage_mode():
    winman = bpy.data.window_managers["WinMan"]
    if not hasattr(winman, "UAS_shot_manager_shots_play_mode"):
        return False

    montage_mode = winman.UAS_shot_manager_shots_play_mode
    if share_data.shot_manager.montage_mode is None or montage_mode != share_data.shot_manager.montage_mode:
        share_data.shot_manager.montage_mode = montage_mode
        send_montage_mode()
        return True
    return False


def send_frame():
    sm_props = get_shot_manager()
    if sm_props is None:
        return
    current_shot_index = shot_manager.get_current_shot_index(sm_props)

    if share_data.shot_manager.current_shot_index != current_shot_index:
        share_data.shot_manager.current_shot_index = current_shot_index
        buffer = common.encode_int(share_data.shot_manager.current_shot_index)
        share_data.client.add_command(common.Command(common.MessageType.SHOT_MANAGER_CURRENT_SHOT, buffer, 0))


def get_state():
    sm_props = get_shot_manager()
    if sm_props is None:
        return

    current_take = shot_manager.get_current_take(sm_props)
    if current_take is None:
        return

    share_data.shot_manager.current_take_name = take.get_name(current_take)

    share_data.shot_manager.shots = []
    for s in shot_manager.get_shots(sm_props):
        new_shot = Shot()
        new_shot.name = shot.get_name(s)
        camera = shot.get_camera(s)
        if camera:
            new_shot.camera_name = camera.name_full
        new_shot.start = shot.get_start(s)
        new_shot.end = shot.get_end(s)
        new_shot.enabled = shot.get_enable_state(s)
        share_data.shot_manager.shots.append(new_shot)


def send_scene():
    get_state()
    buffer = common.encode_int(len(share_data.shot_manager.shots))
    for s in share_data.shot_manager.shots:
        buffer += (
            common.encode_string(s.name)
            + common.encode_string(s.camera_name)
            + common.encode_int(s.start)
            + common.encode_int(s.end)
            + common.encode_bool(s.enabled)
        )
    share_data.client.add_command(common.Command(common.MessageType.SHOT_MANAGER_CONTENT, buffer, 0))


def update_scene():
    sm_props = get_shot_manager()
    if sm_props is None:
        return

    current_take = shot_manager.get_current_take(sm_props)
    if current_take is None:
        return

    current_take_name = shot_manager.get_current_take_name(sm_props)

    if current_take_name != share_data.shot_manager.current_take_name:
        send_scene()
        return

    shots = shot_manager.get_shots(sm_props)
    if len(shots) != len(share_data.shot_manager.shots):
        send_scene()
        return

    for i, s in enumerate(shots):
        prev_shot = share_data.shot_manager.shots[i]
        camera_name = ""
        camera = shot.get_camera(s)
        if camera:
            camera_name = camera.name_full
        if (
            prev_shot.name != shot.get_name(s)
            or prev_shot.camera_name != camera_name
            or prev_shot.start != shot.get_start(s)
            or prev_shot.end != shot.get_end(s)
            or prev_shot.enabled != shot.get_enable_state(s)
        ):
            send_scene()
            return
