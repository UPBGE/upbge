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
Proxy helpers Blender types that have different interfaces or requirements, but do not require their own complete
Proxy implementation.


TODO Enhance this module so that it is possible to reference types that do not exist in all Blender versions or
to control behavior with plugin data.
"""

from __future__ import annotations

import array
from functools import lru_cache
import logging
from pathlib import Path
from typing import Any, Callable, cast, Dict, ItemsView, List, Optional, Tuple, TYPE_CHECKING, Union

from mixer.blender_data.proxy import AddElementFailed, ExternalFileFailed


import bpy
import bpy.types as T  # noqa N812
import bpy.path
import mathutils

if TYPE_CHECKING:
    from mixer.blender_data.aos_proxy import AosProxy
    from mixer.blender_data.datablock_proxy import DatablockProxy
    from mixer.blender_data.proxy import Context, Proxy

logger = logging.getLogger(__name__)


# Beware that MeshVertex must be handled as SOA although "groups" is a variable length item.
# Enums are not handled by foreach_get()
@lru_cache(None)
def soable_collection_properties():
    return {
        T.GPencilStroke.bl_rna.properties["points"],
        T.GPencilStroke.bl_rna.properties["triangles"],
        T.Mesh.bl_rna.properties["edges"],
        T.Mesh.bl_rna.properties["loops"],
        T.Mesh.bl_rna.properties["loop_triangles"],
        T.Mesh.bl_rna.properties["polygons"],
        T.Mesh.bl_rna.properties["vertices"],
        T.MeshFaceMapLayer.bl_rna.properties["data"],
        T.MeshLoopColorLayer.bl_rna.properties["data"],
        T.MeshUVLoopLayer.bl_rna.properties["data"],
        T.ShapeKey.bl_rna.properties["data"],
        T.Spline.bl_rna.properties["bezier_points"],
    }


@lru_cache(None)
def _resize_geometry_types():
    return tuple(
        type(t.bl_rna)
        for t in [
            T.MeshEdges,
            T.MeshLoops,
            T.MeshLoopTriangles,
            T.MeshPolygons,
            T.MeshVertices,
        ]
    )


# in sync with soa_initializers
soable_properties = (
    T.BoolProperty,
    T.IntProperty,
    T.FloatProperty,
    mathutils.Vector,
    mathutils.Color,
    mathutils.Quaternion,
)

# in sync with soable_properties
soa_initializers: Dict[type, array.array] = {
    bool: array.array("b", [0]),
    int: array.array("i", [0]),  # has same itemsize (4) on Linux and Windows
    float: array.array("f", [0.0]),
    mathutils.Vector: array.array("f", [0.0]),
    mathutils.Color: array.array("f", [0.0]),
    mathutils.Quaternion: array.array("f", [0.0]),
}

_node_groups: Tuple[type, ...] = (T.ShaderNodeGroup, T.CompositorNodeGroup, T.TextureNodeGroup)
if bpy.app.version is not None and bpy.app.version >= (2, 92, 0):
    _node_groups = _node_groups + (T.GeometryNodeGroup,)


def dispatch_rna(no_rna_impl: Callable[..., Any]):
    """Decorator to select a function implementation according to the rna of its first argument

    See test_rna_dispatch
    """
    registry: Dict[type, Callable[..., Any]] = {}

    def register_default():
        """Registers the decorated function f as the implementaton to use if the rna of the first argument
        of f was not otherwise registered"""

        def decorator(f: Callable[..., Any]):
            registry[type(None)] = f
            return f

        return decorator

    def register(class_):
        """Registers the decorated function f as the implementaton to use if the first argument
        has the same rna as class_"""

        def decorator(f: Callable[..., Any]):
            registry[class_] = f
            return f

        return decorator

    def dispatch(class_):
        # ignore "object" parent
        for cls_ in class_.mro()[:-1]:
            # KeyError exceptions are a pain when debugging in VScode, avoid them
            func = registry.get(cls_)
            if func:
                return func

        func = registry.get(type(None))
        if func is not None:
            return func

        return no_rna_impl

    def wrapper(bpy_prop_collection: T.bpy_prop_collection, *args, **kwargs):
        """Calls the function registered for bpy_prop_collection.bl_rna"""
        rna = getattr(bpy_prop_collection, "bl_rna", None)
        if rna is None:
            func = no_rna_impl
        else:
            func = dispatch(type(rna))
        return func(bpy_prop_collection, *args, **kwargs)

    # wrapper.register = register  genarates mypy error
    setattr(wrapper, "register", register)  # noqa B010
    setattr(wrapper, "register_default", register_default)  # noqa B010
    return wrapper


def dispatch_value(default_func):
    """Decorator to select a function implementation according to the value of its fist parameter"""
    registry: Dict[type, Callable[..., Any]] = {}
    default: Callable[..., Any] = default_func

    def register(value: Any):
        """Registers the decorated function f as the implementaton to use if the first argument
        has the same value a the value parameter"""

        def decorator(f: Callable[..., Any]):
            registry[value] = f
            return f

        return decorator

    def dispatch(value: Any):
        func = registry.get(value)
        if func:
            return func

        return default

    def wrapper(value: Any, *args, **kwargs):
        """Calls the function registered for value"""
        func = dispatch(value)
        return func(value, *args, **kwargs)

    # wrapper.register = register  genarates mypy error
    setattr(wrapper, "register", register)  # noqa B010
    return wrapper


def is_soable_collection(prop):
    return prop in soable_collection_properties()


def is_soable_property(bl_rna_property):
    return isinstance(bl_rna_property, soable_properties)


@dispatch_value
def bpy_data_ctor(collection_name: str, proxy: DatablockProxy, context: Any) -> Optional[T.ID]:
    """
    Create an element in a bpy.data collection.

    Contains collection-specific code is the mathod to add an element is not new(name: str)
    """
    collection = getattr(bpy.data, collection_name)
    name = proxy.data("name")
    try:
        id_ = collection.new(name)
    except Exception as e:
        logger.error(f"Exception while calling : bpy.data.{collection_name}.new({name})")
        logger.error(f"... {e!r}")
        return None

    return id_


@bpy_data_ctor.register("fonts")  # type: ignore[no-redef]
def _(collection_name: str, proxy: DatablockProxy, context: Context) -> T.VectorFont:
    name = proxy.data("name")
    filepath = proxy.data("filepath")

    if filepath != "<builtin>":
        raise NotImplementedError(f"non builtin font: {name}")

    dummy_text = bpy.data.curves.new("_mixer_tmp_text", "FONT")
    font = dummy_text.font
    bpy.data.curves.remove(dummy_text)
    return font


@bpy_data_ctor.register("images")  # type: ignore[no-redef]
@bpy_data_ctor.register("movieclips")
@bpy_data_ctor.register("sounds")  # type: ignore[no-redef]
def _(collection_name: str, proxy: DatablockProxy, context: Context) -> Optional[T.ID]:
    collection = getattr(bpy.data, collection_name)
    media = None
    media_name = proxy.data("name")
    filepath = proxy.data("filepath")

    resolved_filepath = proxy.resolved_filepath(context)
    if resolved_filepath is None:
        return None

    packed_files = proxy.data("packed_files")
    if packed_files is not None and packed_files.length:
        if collection_name == "images":
            width, height = proxy.data("size")
            try:
                with open(resolved_filepath, "rb") as file_:
                    buffer = file_.read()
                media = collection.new(media_name, width, height)
                media.pack(data=buffer, data_len=len(buffer))
            except RuntimeError as e:
                logger.warning(
                    f'Cannot load packed file original "{filepath}"", resolved "{resolved_filepath}". Exception: '
                )
                logger.warning(f"... {e}")
                raise ExternalFileFailed from e

    else:
        try:
            media = collection.load(resolved_filepath)
            media.name = media_name
        except RuntimeError as e:
            logger.warning(f'Cannot load file original "{filepath}"", resolved "{resolved_filepath}". Exception: ')
            logger.warning(f"... {e}")
            raise ExternalFileFailed from e

    # prevent filepath to be overwritten by the incoming proxy value as it would attempt to reload the file
    # from the incoming path that may not exist
    proxy._data["filepath"] = resolved_filepath
    proxy._data["filepath_raw"] = resolved_filepath
    return media


@bpy_data_ctor.register("objects")  # type: ignore[no-redef]
def _(collection_name: str, proxy: DatablockProxy, context: Context) -> Optional[T.ID]:
    from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy
    from mixer.blender_data.misc_proxies import NonePtrProxy

    collection = getattr(bpy.data, collection_name)
    name = proxy.data("name")
    data_datablock = None
    data_proxy = proxy.data("data")
    if isinstance(data_proxy, DatablockRefProxy):
        data_datablock = data_proxy.target(context)
    elif isinstance(data_proxy, NonePtrProxy):
        data_datablock = None
    else:
        # error on the sender side
        logger.warning(f"bpy.data.objects[{name}].data proxy is a {data_proxy.__class__}.")
        logger.warning("... loaded as Empty")
        data_datablock = None

    return collection.new(name, data_datablock)


@bpy_data_ctor.register("node_groups")  # type: ignore[no-redef]
def _(collection_name: str, proxy: DatablockProxy, context: Context) -> Optional[T.ID]:
    collection = getattr(bpy.data, collection_name)
    name = proxy.data("name")
    bl_idname = proxy.data("bl_idname")
    return collection.new(name, bl_idname)


@bpy_data_ctor.register("lights")  # type: ignore[no-redef]
@bpy_data_ctor.register("textures")  # type: ignore[no-redef]
def _(collection_name: str, proxy: DatablockProxy, context: Context) -> Optional[T.ID]:
    collection = getattr(bpy.data, collection_name)
    name = proxy.data("name")
    type_ = proxy.data("type")
    return collection.new(name, type_)


_curve_ids = {
    "Curve": "CURVE",
    "SurfaceCurve": "SURFACE",
    "TextCurve": "FONT",
}


@bpy_data_ctor.register("curves")  # type: ignore[no-redef]
def _(collection_name: str, proxy: DatablockProxy, context: Context) -> Optional[T.ID]:
    collection = getattr(bpy.data, collection_name)
    name = proxy.data("name")
    curve_type = proxy._type_name
    return collection.new(name, _curve_ids[curve_type])


@bpy_data_ctor.register("shape_keys")  # type: ignore[no-redef]
def _(collection_name: str, proxy: DatablockProxy, context: Context) -> Optional[T.ID]:
    user = proxy._data["user"]
    user_proxy = context.proxy_state.proxies.get(user.mixer_uuid)
    datablock = proxy.create_shape_key_datablock(user_proxy, context)
    return datablock


def _filter_properties(properties: ItemsView, exclude_names: List[str]) -> ItemsView:
    filtered = {k: v for k, v in properties if k not in exclude_names}
    return filtered.items()


@dispatch_rna
def conditional_properties(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    """Filter properties list according to a specific property value in the same structure.

    This prevents loading values that cannot always be saved, such as Object.instance_collection
    that can only be saved when Object.data is None

    Args:
        bpy_struct: the structure
        properties: a view into a Dict[str, bpy.types.Property] to filter
    Returns:
        The filtered properties
    """
    return properties


@conditional_properties.register(T.ColorManagedViewSettings)  # type: ignore[no-redef]
def _(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    if bpy_struct.use_curve_mapping:
        return properties
    filter_props = ["curve_mapping"]
    return _filter_properties(properties, filter_props)


@conditional_properties.register(T.Object)  # type: ignore[no-redef]
def _(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    if not bpy_struct.data:
        return properties

    filter_props = ["instance_collection"]
    return _filter_properties(properties, filter_props)


@conditional_properties.register(T.Curve)  # type: ignore[no-redef]
@conditional_properties.register(T.Mesh)  # type: ignore[no-redef]
@conditional_properties.register(T.MetaBall)  # type: ignore[no-redef]
def _(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    if not bpy_struct.use_auto_texspace:
        return properties

    filter_props = ["texspace_location", "texspace_size"]
    return _filter_properties(properties, filter_props)


@conditional_properties.register(T.Node)  # type: ignore[no-redef]
def _(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    if bpy_struct.hide:
        return properties

    # not hidden: saving width_hidden is ignored
    filter_props = ["width_hidden"]

    if isinstance(bpy_struct, T.NodeReroute):
        filter_props.extend(
            [
                # cannot be set !!
                "width"
            ]
        )

    return _filter_properties(properties, filter_props)


@conditional_properties.register(T.NodeGroupInput)  # type: ignore[no-redef]
@conditional_properties.register(T.NodeGroupOutput)  # type: ignore[no-redef]
def _(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    # For nodes of type NodeGroupInput and NodeGroupOutput, do not save inputs and outputs,
    # which are created created/updated via NodeTree.inputs and NoteTree.outputs
    filter_props = ["inputs", "outputs"]
    if not bpy_struct.hide:
        # same as for Node
        filter_props.append("width_hidden")

    filtered = {k: v for k, v in properties if k not in filter_props}
    return filtered.items()


@conditional_properties.register(T.NodeSocket)  # type: ignore[no-redef]
def _(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    # keep identifier for XxxNodeGroup only
    if isinstance(bpy_struct.node, _node_groups):
        return properties

    filter_props = [
        "identifier",
        # saving bl_idname for NodeReroute (and others ?) cause havoc
        "bl_idname",
    ]

    return _filter_properties(properties, filter_props)


@conditional_properties.register(T.NodeTree)  # type: ignore[no-redef]
def _(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    if not bpy_struct.is_embedded_data:
        return properties

    filter_props = ["name"]
    return _filter_properties(properties, filter_props)


@conditional_properties.register(T.LayerCollection)  # type: ignore[no-redef]
def _(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    scene = bpy_struct.id_data
    if bpy_struct.collection != scene.collection:
        return properties

    filter_props = ["exclude"]
    return _filter_properties(properties, filter_props)


@conditional_properties.register(T.UnitSettings)  # type: ignore[no-redef]
def _(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    if bpy_struct.system != "NONE":
        return properties

    filter_props = ["length_unit", "mass_unit", "time_unit", "temperature_unit"]
    return _filter_properties(properties, filter_props)


@conditional_properties.register(T.FCurve)  # type: ignore[no-redef]
def _(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    if bpy_struct.group is not None:
        return properties

    # FCurve.group = None
    # triggers noisy message
    # ERROR: one of the ID's for the groups to assign to is invalid (ptr=0000028B55B0C038, val=0000000000000000)
    filter_props = ["group"]
    return _filter_properties(properties, filter_props)


@conditional_properties.register(T.EffectSequence)  # type: ignore[no-redef]
@conditional_properties.register(T.ImageSequence)
@conditional_properties.register(T.MaskSequence)
@conditional_properties.register(T.MetaSequence)
@conditional_properties.register(T.MovieClipSequence)
@conditional_properties.register(T.MovieSequence)
@conditional_properties.register(T.SceneSequence)
def _(bpy_struct: T.Struct, properties: ItemsView) -> ItemsView:
    if bpy.app.version >= (2, 92, 0):
        return properties
    filter_props = []
    if not bpy_struct.use_crop:
        filter_props.append("crop")
    if not bpy_struct.use_translation:
        filter_props.append("transform")

    if not filter_props:
        return properties

    return _filter_properties(properties, filter_props)


_morphable_types = (T.Light, T.Texture)
"""Datablock types that may change and need type_recast type after modification of their type attribute."""


def pre_save_datablock(proxy: DatablockProxy, target: T.ID, context: Context) -> T.ID:
    """Process attributes that must be saved first and return a possibly updated reference to the target"""

    # WARNING this is called from save() and from apply()
    # When called from save, the proxy has  all the synchronized properties
    # WHen called from apply, the proxy only contains the updated properties

    if target.library:
        return target

    #  animation_data is handled in StructProxy (parent class of DatablockProxy)

    if isinstance(target, T.Mesh):
        from mixer.blender_data.mesh_proxy import MeshProxy

        assert isinstance(proxy, MeshProxy)
        if proxy.requires_clear_geometry(target):
            target.clear_geometry()
    elif isinstance(target, T.Material):
        is_grease_pencil = proxy.data("is_grease_pencil")
        # will be None for a DeltaUpdate that does not modify "is_grease_pencil"
        if is_grease_pencil is not None:
            # Seems to be write once as no depsgraph update is fired
            if is_grease_pencil and not target.grease_pencil:
                bpy.data.materials.create_gpencil_data(target)
            elif not is_grease_pencil and target.grease_pencil:
                bpy.data.materials.remove_gpencil_data(target)
    elif isinstance(target, T.Scene):
        from mixer.blender_data.misc_proxies import NonePtrProxy

        sequence_editor = proxy.data("sequence_editor")
        if sequence_editor is not None:
            # NonePtrProxy or StructProxy
            if not isinstance(sequence_editor, NonePtrProxy) and target.sequence_editor is None:
                target.sequence_editor_create()
            elif isinstance(sequence_editor, NonePtrProxy) and target.sequence_editor is not None:
                target.sequence_editor_clear()
    elif isinstance(target, _morphable_types):
        # required first to have access to new datablock attributes
        type_ = proxy.data("type")
        if type_ is not None and type_ != target.type:
            target.type = type_
            # must reload the reference
            target = target.type_recast()
            uuid = proxy.mixer_uuid
            context.proxy_state.remove_datablock(uuid)
            context.proxy_state.add_datablock(uuid, target)
    elif isinstance(target, T.Action):
        groups = proxy.data("groups")
        if groups:
            groups.save(target.groups, target, "groups", context)

    return target


#
# add_element
#
@dispatch_rna
def add_element(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context):
    """Add an element to a bpy_prop_collection using the collection specific API.s"""

    if hasattr(collection, "add"):
        # either a bpy_prop_collection  with an rna or a bpy_prop_collection_idprop
        try:
            collection.add()
            return
        except Exception as e:
            logger.error(f"add_element: call to add() failed for {context.visit_state.display_path()} ...")
            logger.error(f"... {e!r}")
            raise AddElementFailed from None

    if not hasattr(collection, "bl_rna"):
        # a bpy.types.bpy_prop_collection, e.g Pose.bones
        # We should not even attempt to add elements in these collections since they do not allow it at all.
        # However bpy_prop_collection and collections with an rna both managed by StructCollectionProxy. We need
        # proxy update to update the contents of existing elements, but it should not attempt to add/remove elements.
        # As a consequence, for attributes that fall into this category we trigger updates with additions and
        # deletions that are meaningless. Ignore them.
        # The right design could be to have different proxies for bpy_prop_collection and bpy_struct that behave like
        # collections.
        # see Proxy construction in  read_attribute()
        return


@add_element.register_default()
def _add_element_default(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    try:
        new_or_add = collection.new
    except AttributeError:
        try:
            new_or_add = collection.add
        except AttributeError:
            logger.error(f"add_element: not implemented for {context.visit_state.display_path()} ...")
            raise AddElementFailed from None

    try:
        return new_or_add()
    except TypeError:
        try:
            key = proxy.data("name")
            return new_or_add(key)
        except Exception as e:
            logger.error(f"add_element: not implemented for {context.visit_state.display_path()} ...")
            logger.error(f"... {e!r}")
            raise AddElementFailed from None


@add_element.register(T.NodeInputs)  # type: ignore[no-redef]
@add_element.register(T.NodeOutputs)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    node = context.visit_state.attribute(-1)
    if not isinstance(node, _node_groups):
        logger.warning(f"Unexpected add node input for {node} at {context.visit_path.path()}")

    socket_type = proxy.data("bl_idname")
    name = proxy.data("name")
    return collection.new(socket_type, name)


@add_element.register(T.NodeTreeInputs)  # type: ignore[no-redef]
@add_element.register(T.NodeTreeOutputs)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    socket_type = proxy.data("bl_socket_idname")
    name = proxy.data("name")
    return collection.new(socket_type, name)


@add_element.register(T.ObjectGpencilModifiers)  # type: ignore[no-redef]
@add_element.register(T.ObjectModifiers)
@add_element.register(T.ObjectShaderFx)  # type: ignore[no-redef]
@add_element.register(T.SequenceModifiers)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    name = proxy.data("name")
    type_ = proxy.data("type")
    return collection.new(name, type_)


@add_element.register(T.CurveSplines)  # type: ignore[no-redef]
@add_element.register(T.FCurveModifiers)
@add_element.register(T.ObjectConstraints)
@add_element.register(T.PoseBoneConstraints)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    type_ = proxy.data("type")
    return collection.new(type_)


@add_element.register(T.SplinePoints)  # type: ignore[no-redef]
@add_element.register(T.SplineBezierPoints)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    return collection.add(1)


@add_element.register(T.MetaBallElements)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    type_ = proxy.data("type")
    return collection.new(type=type_)


@add_element.register(T.CurveMapPoints)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    location = proxy.data("location")
    return collection.new(location[0], location[1])


@add_element.register(T.Nodes)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    node_type = proxy.data("bl_idname")
    try:
        return collection.new(node_type)
    except RuntimeError as e:
        name = proxy.data("name")
        logger.error(f"add_element failed for node {name!r} into {context.visit_state.display_path()} ...")
        logger.error(f"... {e!r}")
        raise AddElementFailed from None


@add_element.register(T.ActionGroups)  # type: ignore[no-redef]
@add_element.register(T.FaceMaps)
@add_element.register(T.LoopColors)
@add_element.register(T.TimelineMarkers)
@add_element.register(T.UVLoopLayers)
@add_element.register(T.VertexGroups)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    name = proxy.data("name")
    return collection.new(name=name)


@add_element.register(T.ArmatureEditBones)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    name = proxy.data("name")
    return collection.new(name)


@add_element.register(T.GreasePencilLayers)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    name = proxy.data("info")
    return collection.new(name)


@add_element.register(T.GPencilFrames)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    frame_number = proxy.data("frame_number")
    return collection.new(frame_number)


@add_element.register(T.KeyingSets)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    label = proxy.data("bl_label")
    idname = proxy.data("bl_idname")
    return collection.new(name=label, idname=idname)


@add_element.register(T.KeyingSetPaths)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    # TODO current implementation fails
    # All keying sets paths have an empty name, and insertion with add() fails
    # with an empty name
    target_ref = proxy.data("id")
    if target_ref is None:
        target = None
    else:
        target = target_ref.target(context)
    data_path = proxy.data("data_path")
    index = proxy.data("array_index")
    group_method = proxy.data("group_method")
    group_name = proxy.data("group")
    return collection.add(
        target_id=target, data_path=data_path, index=index, group_method=group_method, group_name=group_name
    )


@add_element.register(T.AnimDataDrivers)  # type: ignore[no-redef]
@add_element.register(T.ActionFCurves)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    data_path = proxy.data("data_path")
    array_index = proxy.data("array_index")
    return collection.new(data_path, index=array_index)


@add_element.register(T.FCurveKeyframePoints)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    return collection.add(1)


@add_element.register(T.AttributeGroup)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    name = proxy.data("name")
    type_ = proxy.data("type")
    domain = proxy.data("domain")
    return collection.new(name, type_, domain)


_non_effect_sequences = {"IMAGE", "SOUND", "META", "SCENE", "MOVIE", "MOVIECLIP", "MASK"}


@lru_cache(None)
def _effect_sequences():
    return set(T.EffectSequence.bl_rna.properties["type"].enum_items.keys()) - _non_effect_sequences


@lru_cache(None)
def _version():
    version = bpy.app.version
    if not isinstance(version, tuple):
        return (0,)
    return version


if _version() < (2, 92, 0):
    _Sequences = T.Sequences
else:
    _Sequences = T.SequencesTopLevel


@add_element.register(_Sequences)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    type_name = proxy.data("type")
    name = proxy.data("name")
    channel = proxy.data("channel")
    frame_start = proxy.data("frame_start")
    if type_name in _effect_sequences():
        # overwritten anyway
        frame_end = frame_start + 1
        return collection.new_effect(name, type_name, channel, frame_start, frame_end=frame_end)
    if type_name == "SOUND":
        sound = proxy.data("sound")
        target = sound.target(context)
        if not target:
            logger.warning(f"missing target ID block for bpy.data.{sound.collection}[{sound.key}] ")
            return None
        filepath = target.filepath
        return collection.new_sound(name, filepath, channel, frame_start)
    if type_name == "MOVIE":
        filepath = proxy.data("filepath")
        return collection.new_movie(name, filepath, channel, frame_start)
    if type_name == "IMAGE":
        directory = proxy.data("directory")
        filename = proxy.data("elements").data(0).data("filename")
        filepath = str(Path(directory) / filename)
        return collection.new_image(name, filepath, channel, frame_start)

    logger.warning(f"Sequence type not implemented: {type_name}")
    return None


@add_element.register(T.IDMaterials)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    material_datablock = proxy.target(context)
    return collection.append(material_datablock)


@add_element.register(T.ColorRampElements)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, proxy: Proxy, index: int, context: Context) -> T.bpy_struct:
    position = proxy.data("position")
    return collection.new(position)


def fit_aos(target: T.bpy_prop_collection, proxy: AosProxy, context: Context):
    """
    Adjust the size of a bpy_prop_collection proxified as an array of structures (e.g. MeshVertices)
    """

    if not hasattr(target, "bl_rna"):
        return

    target_rna = target.bl_rna
    if isinstance(target_rna, _resize_geometry_types()):
        existing_length = len(target)
        incoming_length = len(proxy)
        if existing_length != incoming_length:
            if existing_length != 0:
                logger.error(f"resize_geometry(): size mismatch for {target}")
                logger.error(f"... existing: {existing_length} incoming {incoming_length}")
                return
            logger.debug(f"resizing geometry: add({incoming_length}) for {target}")
            target.add(incoming_length)
        return

    if isinstance(target_rna, type(T.GPencilStrokePoints.bl_rna)):
        existing_length = len(target)
        incoming_length = proxy.length
        delta = incoming_length - existing_length
        if delta > 0:
            target.add(delta)
        else:
            while delta < 0:
                target.pop()
                delta += 1
        return

    if isinstance(target_rna, type(T.SplineBezierPoints.bl_rna)):
        existing_length = len(target)
        incoming_length = len(proxy)
        delta = incoming_length - existing_length
        if delta > 0:
            target.add(delta)
        elif delta < 0:
            logger.error("Remove not implemented for type SplineBezierPoints")
        return

    logger.error(f"Not implemented fit_aos for type {target.bl_rna} for {target} ...")


#
# must_replace
#
@lru_cache(None)
def _object_material_slots_property():
    return T.Object.bl_rna.properties["material_slots"]


@lru_cache(None)
def _key_blocks_property():
    return T.Key.bl_rna.properties["key_blocks"]


@dispatch_rna
def can_resize(collection: T.bpy_prop_collection, context: Context) -> bool:
    """Returns True if the collection can safely be resized."""
    return True


@can_resize.register(T.NodeInputs)  # type: ignore[no-redef]
@can_resize.register(T.NodeOutputs)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, context: Context) -> bool:
    # in XxxNodeGroups, the number of sockets is controlled by the inner NodeGroup.inputs and NodeGroup.outputs
    # Extending the collection in XxxNodeGroup would create the socket twice (once in the XXXNodeGroup and once
    # in NodeTree.inputs or outputs).
    # The existing items in XxxNodeGroup.inputs and outputs must be saved as they contain the socket default_value
    node = context.visit_state.attribute(-2)
    return not isinstance(node, _node_groups)


@dispatch_rna
def diff_must_replace(
    collection: T.bpy_prop_collection, sequence: List[DatablockProxy], collection_property: T.Property
) -> bool:
    """
    Returns True if a diff between the proxy sequence state and the Blender collection state must force a
    full collection replacement.
    """

    if collection_property == _object_material_slots_property():
        from mixer.blender_data.datablock_ref_proxy import DatablockRefProxy

        # Object.material_slots has no bl_rna, so rely on the property to identify it
        # TODO should we change to a dispatch on the property value ?
        from mixer.blender_data.misc_proxies import NonePtrProxy

        if len(collection) != len(sequence):
            return True

        # The struct_collection update encoding is complex. It is way easier to handle a full replace in
        # ObjectProxy._update_material_slots, so replace all if anything has changed

        # As diff yields a complete DiffReplace or nothing, all the attributes are present in the proxy
        for bl_item, proxy in zip(collection, sequence):
            bl_material = bl_item.material
            material_proxy: Union[DatablockRefProxy, NonePtrProxy] = proxy.data("material")
            if (bl_material is None) != isinstance(material_proxy, NonePtrProxy):
                return True
            if bl_material is not None and bl_material.mixer_uuid != material_proxy.mixer_uuid:
                return True
            if bl_item.link != proxy.data("link"):
                return True

    elif collection_property == _key_blocks_property():
        if len(collection) != len(sequence):
            return True
        for bl_item, proxy in zip(collection, sequence):
            if bl_item.name != proxy.data("name"):
                return True

    return False


@diff_must_replace.register(T.CurveSplines)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, sequence: List[DatablockProxy], collection_property: T.Property) -> bool:
    return True


@diff_must_replace.register(T.ArmatureEditBones)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, sequence: List[DatablockProxy], collection_property: T.Property) -> bool:
    # HACK
    # Without forcing a full update, using rigify addon, to create a basic human, then use rigify button
    # causes some bones to be lost after leaving edit mode (bones number drops from 218 to 217).
    # Not cause of the lost bone problem has not yet been found.
    return True


@diff_must_replace.register(T.VertexGroups)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, sequence: List[DatablockProxy], collection_property: T.Property) -> bool:
    # Full replace if anything has changed is easier to cope with in ObjectProxy._update_vertex_groups()
    return (
        any((bl_item.name != proxy.data("name") for bl_item, proxy in zip(collection, sequence)))
        or any((bl_item.index != proxy.data("index") for bl_item, proxy in zip(collection, sequence)))
        or any((bl_item.lock_weight != proxy.data("lock_weight") for bl_item, proxy in zip(collection, sequence)))
    )


@diff_must_replace.register(T.GreasePencilLayers)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, sequence: List[DatablockProxy], collection_property: T.Property) -> bool:
    # Name mismatch (in info property). This may happen during layer swap and cause unsolicited rename
    # Easier to solve with full replace
    return any((bl_item.info != proxy.data("info") for bl_item, proxy in zip(collection, sequence)))


@diff_must_replace.register(T.ActionFCurves)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, sequence: List[DatablockProxy], collection_property: T.Property) -> bool:
    # The FCurve API has two caveats (seen in 2.83.9):
    # - it is not possible to set FCurve.group from a valid group to None (but the inverse is possible)
    # - setting group sometimes changes array_index, like in (no groups initially)
    #       >>> a=D.actions[0]
    #       >>> g=a.groups.new('plop')
    #       >>> a.fcurves[1].color[0] = 0.1
    #       >>> a.fcurves[2].color[0] = 0.2
    #       >>> [(i.data_path, i.array_index, i.color[0]) for i in a.fcurves]
    #       [('location', 0, 0.0), ('location', 1, 0.10000000149011612), ('location', 2, 0.20000000298023224)]
    #       >>>
    #       >>> a.fcurves[2].group = g
    #       >>> [(i.data_path, i.array_index, i.color[0]) for i in a.fcurves]
    #       [('location', 2, 0.20000000298023224), ('location', 0, 0.0), ('location', 1, 0.10000000149011612)]

    # So overwrite the whole Action.fcurves array as soon as any group changes

    from mixer.blender_data.misc_proxies import PtrToCollectionItemProxy

    for proxy, item in zip(sequence, collection):
        group_proxy = cast(PtrToCollectionItemProxy, proxy.data("group"))
        if item.group is None:
            if group_proxy:
                # not None -> None
                return True
        else:
            if not group_proxy:
                # None -> not None
                return True
            same_group = group_proxy == PtrToCollectionItemProxy.make(T.FCurve, "group").load(item)
            if not same_group:
                return True

    return False


#
# Clear_from
#
@dispatch_rna
def clear_from(collection: T.bpy_prop_collection, sequence: List[DatablockProxy], context: Context) -> int:
    """
    Returns the index of the first item in collection that has a type that does not match the
    coresponding item in sequence

    This is the case for items that would require to change type (e.g. ObjectModifier) but cannot be morphed. This enables the caller
    to truncate the collection at the first element that cannot be morphed in-place and re_write thj collection
    from this point on.
    """
    return min(len(sequence), len(collection))


@clear_from.register(T.ObjectModifiers)  # type: ignore[no-redef]
@clear_from.register(T.ObjectGpencilModifiers)
@clear_from.register(T.SequenceModifiers)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, sequence: List[DatablockProxy], context: Context) -> int:
    """clear_from implementation for collections of items that cannot be updated if their "type" attribute changes."""
    for i, (proxy, item) in enumerate(zip(sequence, collection)):
        if proxy.data("type") != item.type:
            return i
    return min(len(sequence), len(collection))


@clear_from.register(T.Nodes)  # type: ignore[no-redef]
def _(collection: T.bpy_prop_collection, sequence: List[DatablockProxy], context: Context) -> int:
    """clear_from implementation for collections of items that cannot be updated if their "bl_idname" attribute
    changes."""
    for i, (proxy, item) in enumerate(zip(sequence, collection)):
        if proxy.data("bl_idname") != item.bl_idname:
            # On the receiver, NodeTree.nodes will be partially cleared, which will clear some links.
            # Resending the links is the easiest way to restore them.
            # Note that Nodes items order may change after a socket default_value is updated !
            context.visit_state.send_nodetree_links = True
            return i

    return min(len(sequence), len(collection))


#
# truncate_collection
#
@dispatch_rna
def truncate_collection(collection: T.bpy_prop_collection, size: int):
    """Truncates collection to size elements by removing elements at the end."""
    return


@truncate_collection.register_default()
def _truncate_collection_remove(collection: T.bpy_prop_collection, size: int):
    try:
        while len(collection) > max(size, 0):
            collection.remove(collection[-1])
    except Exception as e:
        logger.error(f"truncate_collection {collection}: exception ...")
        logger.error(f"... {e!r}")


@truncate_collection.register(T.IDMaterials)
def _truncate_collection_pop(collection: T.bpy_prop_collection, size: int):
    while len(collection) > max(size, 0):
        collection.pop()


def remove_datablock(collection: T.bpy_prop_collection, datablock: T.ID):
    """Delete a datablock from its bpy.data collection"""
    if isinstance(datablock, T.Scene):
        from mixer.blender_client.scene import delete_scene

        delete_scene(datablock)
    elif isinstance(datablock, T.Key):
        # the doc labels it unsafe, use sparingly
        bpy.data.batch_remove([datablock])
    elif isinstance(datablock, T.Library):
        # TODO 2.91 has BlendDatalibraries.remove()
        logger.warning(f"remove_datablock({datablock}): ignored (library)")
    else:
        collection.remove(datablock)
