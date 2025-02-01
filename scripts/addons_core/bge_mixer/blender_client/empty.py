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
from mixer.blender_client.misc import get_object_path
from mixer.broadcaster import common
from mixer.broadcaster.client import Client

logger = logging.getLogger(__name__)


def send_empty(client: Client, obj):
    path = get_object_path(obj)
    buffer = common.encode_string(path)
    if buffer:
        client.add_command(common.Command(common.MessageType.EMPTY, buffer, 0))
