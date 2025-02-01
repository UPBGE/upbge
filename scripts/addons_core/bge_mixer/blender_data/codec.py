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
Define the mapping between message types and message classes.

Mostly used by tests to ease message decoding.
"""
from mixer import codec
from mixer.broadcaster.common import MessageType
from mixer.blender_data import messages

message_types = {
    MessageType.BLENDER_DATA_CREATE: messages.BlenderDataMessage,
    MessageType.BLENDER_DATA_REMOVE: messages.BlenderRemoveMessage,
    MessageType.BLENDER_DATA_RENAME: messages.BlenderRenamesMessage,
    MessageType.BLENDER_DATA_MEDIA: messages.BlenderMediaMessage,
}


def register():
    codec.register_message_types(message_types)


def unregister():
    codec.unregister_message_types(message_types)
