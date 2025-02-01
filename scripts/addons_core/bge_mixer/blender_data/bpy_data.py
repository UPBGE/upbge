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

"""Helpers for bpy.data collections."""

from functools import lru_cache

import bpy


def bl_rna_to_type(bl_rna):
    return getattr(bpy.types, bl_rna.identifier)


@lru_cache(None)
def collection_name_to_type():
    """Map root collection name to object type (e.g. "objects" -> bpy.types.Object, "lights" -> bpy.types.Light, ...)"""
    return {
        p.identifier: bl_rna_to_type(p.fixed_type)
        for p in bpy.types.BlendData.bl_rna.properties
        if p.bl_rna.identifier == "CollectionProperty"
    }


@lru_cache(None)
def rna_identifier_to_collection_name():
    """Map object type name to root collection, e.g. "Object" -> "objects", "Light" -> "lights"""
    return {value.bl_rna.identifier: key for key, value in collection_name_to_type().items()}


@lru_cache(None)
def collections_types():
    """Types of datablocks in bpy.data datablock collections (e.g. bpy.types.Object, bpy.data.Light, ...)"""
    return collection_name_to_type().values()


@lru_cache(None)
def collections_names():
    """Names of the datablock collections in bpy.data (e.g. "objects", "lights", ...)"""
    return collection_name_to_type().keys()
