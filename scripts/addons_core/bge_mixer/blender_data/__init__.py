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
"""Package for Blender generic synchronization."""

# import triggers Proxy classes registration into json_codec
try:
    import mixer.blender_data.aos_proxy
    import mixer.blender_data.aos_soa_proxy
    import mixer.blender_data.bpy_data_proxy
    import mixer.blender_data.datablock_collection_proxy
    import mixer.blender_data.datablock_proxy
    import mixer.blender_data.datablock_ref_proxy
    import mixer.blender_data.library_proxies
    import mixer.blender_data.mesh_proxy
    import mixer.blender_data.modifier_proxies
    import mixer.blender_data.misc_proxies
    import mixer.blender_data.node_proxy
    import mixer.blender_data.object_proxy
    import mixer.blender_data.shape_key_proxy
    import mixer.blender_data.armature_proxy
    import mixer.blender_data.struct_collection_proxy
    import mixer.blender_data.struct_proxy  # noqa: 401
except (ImportError, AttributeError):
    # Import is not possible when not run within Blender (unittest)
    pass


def register():
    import bpy
    from mixer.blender_data.bpy_data import collections_types

    for type_ in collections_types():
        type_.mixer_uuid = bpy.props.StringProperty(default="")


def unregister():
    pass
