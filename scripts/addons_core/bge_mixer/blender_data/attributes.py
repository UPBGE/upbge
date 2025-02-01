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

from __future__ import annotations

import logging
import traceback
from typing import Any, Optional, Union, TYPE_CHECKING

import bpy
import bpy.types as T  # noqa

from mixer.blender_data.proxy import Delta, DeltaUpdate, Proxy
from mixer.blender_data.specifics import is_soable_collection
from mixer.blender_data.type_helpers import is_vector, is_matrix

if TYPE_CHECKING:
    from mixer.blender_data.bpy_data_proxy import Context

logger = logging.getLogger(__name__)


MAX_DEPTH = 30

_builtin_types = (float, int, bool, str, bytes)


class _NotBuiltin(Exception):
    pass


def _read_builtin(attr):
    if isinstance(attr, _builtin_types):
        return attr

    attr_type = type(attr)
    if is_vector(attr_type):
        return list(attr)
    if is_matrix(attr_type):
        return [list(col) for col in attr.col]

    # TODO flatten
    if attr_type == T.bpy_prop_array:
        return list(attr)

    raise _NotBuiltin


def read_attribute(attr: Any, key: Union[int, str], attr_property: T.Property, parent: T.bpy_struct, context: Context):
    """
    Load a property into a python object of the appropriate type, be it a Proxy or a native python object
    """

    try:
        return _read_builtin(attr)
    except _NotBuiltin:
        pass

    if isinstance(attr, set):
        from mixer.blender_data.misc_proxies import SetProxy

        return SetProxy().load(attr)

    context.visit_state.push(attr_property, key)
    try:
        from mixer.blender_data.misc_proxies import PtrToCollectionItemProxy

        attr_type = type(attr)
        if attr_type == T.bpy_prop_collection:
            if hasattr(attr, "bl_rna") and isinstance(
                attr.bl_rna, (type(T.CollectionObjects.bl_rna), type(T.CollectionChildren.bl_rna))
            ):
                from mixer.blender_data.datablock_collection_proxy import DatablockRefCollectionProxy

                return DatablockRefCollectionProxy().load(attr, context)
            elif is_soable_collection(attr_property):
                from mixer.blender_data.aos_proxy import AosProxy

                return AosProxy().load(attr, attr_property, context)
            else:
                # This code path is taken for collections that have an rna and collections that do not
                # There should probably be different proxies for collection with and without rna.
                # See comment in add_element()
                from mixer.blender_data.struct_collection_proxy import StructCollectionProxy

                return StructCollectionProxy.make(attr_property).load(attr, context)

        # TODO merge with previous case
        if isinstance(attr_property, T.CollectionProperty):
            from mixer.blender_data.struct_collection_proxy import StructCollectionProxy

            return StructCollectionProxy().load(attr, context)

        bl_rna = attr_property.bl_rna
        if bl_rna is None:
            logger.error("read_attribute: no implementation for ...")
            logger.error(f"... {context.visit_state.display_path()}.{key} (type: {type(attr)})")
            return None

        if issubclass(attr_type, T.PropertyGroup):
            from mixer.blender_data.struct_proxy import StructProxy

            return StructProxy.make(attr).load(attr, context)

        if issubclass(attr_type, T.ID):
            if attr.is_embedded_data:
                # Embedded datablocks are loaded as StructProxy and DatablockProxy is reserved
                # for standalone datablocks
                from mixer.blender_data.struct_proxy import StructProxy

                return StructProxy.make(attr).load(attr, context)
            else:
                # Standalone databocks are loaded from DatablockCollectionProxy, so we can only encounter
                # datablock references here
                from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy

                return DatablockRefProxy().load(attr, context)

        proxy = PtrToCollectionItemProxy.make(type(parent), key)
        if proxy is not None:
            return proxy.load(attr)

        if issubclass(attr_type, T.bpy_struct):
            from mixer.blender_data.struct_proxy import StructProxy

            return StructProxy.make(attr).load(attr, context)

        if attr is None:
            from mixer.blender_data.misc_proxies import NonePtrProxy

            return NonePtrProxy()

        logger.error("read_attribute: no implementation for ...")
        logger.error(f"... {context.visit_state.display_path()}.{key} (type: {type(attr)})")
    finally:
        context.visit_state.pop()


def get_attribute_value(parent, key):
    if isinstance(key, int):
        target = parent[key]
    elif isinstance(parent, T.bpy_prop_collection):
        target = parent.get(key)
    else:
        target = getattr(parent, key, None)
    return target


def write_attribute(
    parent: Union[T.bpy_struct, T.bpy_prop_collection], key: Union[str, int], value: Any, context: Context
):
    """
    Write value into parent.key or parent[key].

    If value is a Python builtin or the proxy of a bpy_struct, it will be saved into parent.key. If value is a proxy
    of a bpy_prop_collection, it will be saved into parent[key].

    Args:
        parent: that Blender attribute that contains the target attribute
        key: an index of member name that identifies the target attribute
        value: the proxy or python builtin value to save parent[key] or parent[key]
        context: proxy and visit state
    """
    # Like in apply_attribute parent and key are needed to specify a L-value in setattr()
    try:
        if isinstance(value, Proxy):
            attribute_value = get_attribute_value(parent, key)
            context.visit_state.push(parent, key)
            try:
                value.save(attribute_value, parent, key, context)
            except Exception as e:
                logger.error("write_attribute: exception for ...")
                logger.error(f"... attribute: {context.visit_state.display_path()}.{key}, value: {value}")
                logger.error(f" ...{e!r}")
            finally:
                context.visit_state.pop()

        else:
            assert isinstance(key, str)

            prop = parent.bl_rna.properties.get(key)
            if prop is None:
                # Don't log this, too many messages
                # f"Attempt to write to non-existent attribute {bl_instance}.{key} : skipped"
                return

            if not prop.is_readonly:
                try:
                    setattr(parent, key, value)
                except TypeError as e:
                    if value != "":
                        # common for enum that have unsupported default values, such as FFmpegSettings.ffmpeg_preset,
                        # which seems initialized at "" and triggers :
                        #   TypeError('bpy_struct: item.attr = val: enum "" not found in (\'BEST\', \'GOOD\', \'REALTIME\')')
                        logger.warning("write_attribute: exception for ...")
                        logger.warning(f"... attribute: {context.visit_state.display_path()}.{key}, value: {value}")
                        logger.warning(f" ...{e!r}")

    except (IndexError, AttributeError) as e:
        if (
            isinstance(e, AttributeError)
            and isinstance(parent, bpy.types.Collection)
            and parent.name == "Master Collection"
            and key == "name"
        ):
            pass
        else:
            logger.warning("write_attribute: exception while accessing ...")
            logger.warning(f"... attribute: {context.visit_state.display_path()}.{key}")
            logger.warning(f" ...{e!r}")

    except Exception:
        logger.warning("write_attribute: exception for ...")
        logger.warning(f"... attribute: {context.visit_state.display_path()}.{key}, value: {value}")
        for line in traceback.format_exc().splitlines():
            logger.warning(f" ... {line}")


def apply_attribute(
    parent: Union[T.bpy_struct, T.bpy_prop_collection],
    key: Union[str, int],
    current_proxy_value: Any,
    delta: Delta,
    context: Context,
    to_blender=True,
) -> Any:
    """
    Applies a delta to the Blender attribute identified by parent.key or parent[key]

    Args:
        parent: the attribute that contains the Blender attribute to update
        key: the identifier of the attribute to update inside parent
        current_value: the current proxy value
        delta: the delta to apply
        context: proxy and visit state
        to_blender: update the managed Blender attribute in addition to current_proxy_value

    Returns:
        a value to store into the updated proxy
    """

    # Like in write_attribute parent and key are needed to specify a L-value
    # assert type(delta) == DeltaUpdate

    delta_value = delta.value
    # assert proxy_value is None or type(proxy_value) == type(value)

    try:
        if isinstance(current_proxy_value, Proxy):
            attribute_value = get_attribute_value(parent, key)

            context.visit_state.push(parent, key)
            try:
                return current_proxy_value.apply(attribute_value, parent, key, delta, context, to_blender)
            finally:
                context.visit_state.pop()
        else:
            if to_blender:
                # try is less costly than fetching the property to find if the attribute is readonly
                if isinstance(key, int):
                    parent[key] = delta_value
                else:
                    try:
                        setattr(parent, key, delta_value)
                    except AttributeError as e:
                        # most likely an addon (runtime) attribute that exists on the sender but no on this
                        # receiver or a readonly attribute that should be filtered out
                        # Do not be too verbose
                        logger.info("apply_attribute: exception for ...")
                        logger.info(f"... attribute: {context.visit_state.display_path()}.{key}, value: {delta_value}")
                        logger.info(f" ...{e!r}")

            return delta_value

    except Exception as e:
        logger.warning("apply_attribute: exception for ...")
        logger.warning(f"... attribute: {context.visit_state.display_path()}.{key}, value: {delta_value}")
        logger.warning(f" ...{e!r}")


def diff_attribute(
    item: Any, key: Union[int, str], item_property: T.Property, value: Any, context: Context
) -> Optional[Delta]:
    """
    Computes a difference between a blender item and a proxy value

    Args:
        item: the blender item
        item_property: the property of item as found in its enclosing object
        value: a proxy value

    """
    try:
        if isinstance(value, Proxy):
            context.visit_state.push(item, key)
            try:
                return value.diff(item, key, item_property, context)
            finally:
                context.visit_state.pop()

        # An attribute mappable on a python builtin type
        blender_value = _read_builtin(item)
        if blender_value != value:
            return DeltaUpdate(blender_value)

    except Exception as e:
        logger.warning("diff_attribute: exception for ...")
        logger.warning(f"... attribute: {context.visit_state.display_path()}.{key}")
        logger.warning(f" ...{e!r}")
        return None

    return None
