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
Type related utilities
"""
import functools
from typing import Any, List, Type

import bpy.types as T  # noqa N812
import mathutils

vector_types = {mathutils.Vector, mathutils.Color, mathutils.Quaternion, mathutils.Euler}


def is_vector(type_: Type):
    return type_ in vector_types


def is_matrix(type_: Type):
    return type_ is mathutils.Matrix


def is_pointer(property_) -> bool:
    return property_.bl_rna is T.PointerProperty.bl_rna


def bases_of(rna_property: T.Property) -> List[Any]:
    """
    Including the current type and None as root
    """
    bases = []
    base = rna_property
    while base is not None:
        bases.append(base)
        base = None if base.base is None else base.base.bl_rna
    return bases


def is_instance(rna_property: T.Property, base: T.Property) -> bool:
    return base in bases_of(rna_property)


def is_pointer_to(rna_property: T.Property, base: Any) -> bool:
    return is_pointer(rna_property) and is_instance(rna_property.fixed_type, base.bl_rna)


@functools.lru_cache(maxsize=None)
def sub_id_type(type_):
    """Returns the base closest to ID (e.g Light for PointLight)"""
    sub_id_list = [t for t in type_.mro() if issubclass(t, T.ID) and t != T.ID]
    if sub_id_list:
        return sub_id_list[-1]
    return None
