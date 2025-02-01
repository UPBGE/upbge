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

from enum import IntEnum
import bpy
import logging
from mixer.broadcaster import common
from mixer.share_data import share_data
from mixer.broadcaster.client import Client

logger = logging.getLogger(__name__)


class ConstraintType(IntEnum):
    PARENT = 0
    LOOK_AT = 1


def build_add_constraint(data):
    index = 0
    constraint_type, index = common.decode_int(data, index)
    object_name, index = common.decode_string(data, index)
    target_name, index = common.decode_string(data, index)

    ob = share_data.blender_objects[object_name]
    target = share_data.blender_objects[target_name]

    if constraint_type == ConstraintType.PARENT:
        add_parent_constraint(ob, target)
    elif constraint_type == ConstraintType.LOOK_AT:
        add_lookat_constraint(ob, target)
    else:
        logger.warning(f"Unknown constraint {constraint_type}")


def add_parent_constraint(ob, target):
    constraint = get_or_create_constraint(ob, "CHILD_OF")
    constraint.target = target
    constraint.set_inverse_pending = True
    constraint.use_scale_x = False
    constraint.use_scale_y = False
    constraint.use_scale_z = False


def add_lookat_constraint(ob, target):
    constraint = get_or_create_constraint(ob, "TRACK_TO")
    constraint.target = target


def build_remove_constraint(data):
    index = 0
    constraint_type, index = common.decode_int(data, index)
    object_name, index = common.decode_string(data, index)
    ob = share_data.blender_objects[object_name]

    constraint = None
    if constraint_type == ConstraintType.PARENT:
        constraint = get_constraint(ob, "CHILD_OF")
    elif constraint_type == ConstraintType.LOOK_AT:
        constraint = get_constraint(ob, "TRACK_TO")

    if constraint is not None:
        ob.constraints.remove(constraint)


def get_constraint(ob, constraint_type: str):
    for constraint in ob.constraints:
        if constraint.type == constraint_type:
            return constraint
    return None


def get_or_create_constraint(ob, constraint_type: str):
    for constraint in ob.constraints:
        if constraint.type == constraint_type:
            return constraint
    constraint = ob.constraints.new(type=constraint_type)
    return constraint


def send_add_constraint(client: Client, object_: bpy.types.Object, constraint_type: ConstraintType, target: str):
    buffer = common.encode_int(constraint_type) + common.encode_string(object_.name_full) + common.encode_string(target)
    client.add_command(common.Command(common.MessageType.ADD_CONSTRAINT, buffer, 0))


def send_remove_constraints(client: Client, object_: bpy.types.Object, constraint_type: ConstraintType):
    buffer = common.encode_int(constraint_type) + common.encode_string(object_.name_full)
    client.add_command(common.Command(common.MessageType.REMOVE_CONSTRAINT, buffer, 0))
