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
Defines the classes and configuration that controls the data synchronizations, i.e. which types and type members
should be synchronized.

This module could be enhanced to provide multiple SynchronizedProperties to that different data is synchronized at different times
according to user preferences.

see synchronization.md
"""
from __future__ import annotations

from functools import lru_cache
import logging
import sys
from typing import Any, Dict, ItemsView, Iterable, List, Optional, Set, Union

from bpy import types as T  # noqa

from mixer.blender_data.type_helpers import is_pointer_to
from mixer.blender_data.bpy_data import collections_names

DEBUG = True
logger = logging.getLogger(__name__)


def _skip_scene(item):
    return item.name == "_mixer_to_be_removed_"


def _skip_image(item):
    return item.source == "VIEWER"


def _skip_skape_key(item):
    # shape keys are not linkable, they can only be linked indirectly via a Mesh or other
    return item.library is not None


_skip = {"scenes": _skip_scene, "images": _skip_image, "shape_keys": _skip_skape_key}


def skip_bpy_data_item(collection_name, item):
    # Never want to consider these as updated, created, removed, ...
    try:
        skip = _skip[collection_name]
    except KeyError:
        return False
    else:
        return skip(item)


class Filter:
    def apply(self, properties):
        return properties, ""

    def is_active(self):
        return True


class TypeFilter(Filter):
    """
    Filter on type or Pointer to type.

    T.SceneEEVEE wil match D.scenes[0].eevee although the later is a T.PointerProperty
    """

    def __init__(self, types: Union[Any, Iterable[Any]]):
        self._types = types if isinstance(types, Iterable) else [types]

    # use cached_property in 3.8
    @property  # type: ignore
    @lru_cache()
    def _rnas(self):
        return [t.bl_rna for t in self._types]

    def matches(self, bl_rna_property):
        return bl_rna_property.bl_rna in self._rnas or any([is_pointer_to(bl_rna_property, t) for t in self._rnas])


class TypeFilterIn(TypeFilter):
    def apply(self, properties):
        return [p for p in properties if self.matches(p)], ""


class TypeFilterOut(TypeFilter):
    def apply(self, properties):
        return [p for p in properties if not self.matches(p)], ""


class NameFilter(Filter):
    def __init__(self, names: List[str]):
        self._names = names

    def check_unknown(self, properties):
        if not DEBUG:
            return None
        identifiers = [p.identifier for p in properties]
        local_exclusions = set(self._names) - set(_exclude_names)
        unknowns = [repr(name) for name in local_exclusions if name not in identifiers]
        if unknowns:
            return f"Unknown properties: {', '.join(unknowns)}. Check spelling"
        return ""


class NameFilterOut(NameFilter):
    def apply(self, properties):
        return [p for p in properties if p.identifier not in self._names], self.check_unknown(properties)


class NameFilterIn(NameFilter):
    def apply(self, properties):
        return [p for p in properties if p.identifier in self._names], self.check_unknown(properties)


# true class with isactive()
FilterSet = Dict[Optional[Any], Iterable[Filter]]


def bases(bl_rna):
    b = bl_rna
    while b is not None:
        yield b
        b = None if b.base is None else b.base.bl_rna
    yield None


class FilterStack:
    def __init__(self):
        self._filter_sets: List[FilterSet] = []

    @lru_cache()
    def _filter_stack(self):
        filters = []
        for filter_set in self._filter_sets:
            filters.append({None if k is None else k.bl_rna: v for k, v in filter_set.items()})
        return filters

    def apply(self, bl_rna: T.bpy_struct) -> List[T.Property]:
        properties = list(bl_rna.properties)
        for class_ in bases(bl_rna):
            bl_rna = None if class_ is None else class_.bl_rna
            for filter_set in self._filter_stack():
                filters = filter_set.get(bl_rna, [])
                for filter_ in filters:
                    properties, error = filter_.apply(properties)
                    if error:
                        logger.error(
                            f"Error while applying filter {filter_.__class__.__name__!r} on {bl_rna.identifier!r} ..."
                        )
                        logger.error(f"... {error}")
        return properties

    def append(self, filter_set: FilterSet):
        self._filter_sets.append(filter_set)


BlRna = Any
PropertyName = str
Property = Any
Properties = Dict[PropertyName, Property]
PropertiesOrder = Dict[T.bpy_struct, Set[str]]
"""type: {properties to deliver first}"""


class SynchronizedProperties:
    """
    Keeps track of properties to synchronize for all types.

    Only one SynchronizedProperties is currently use, but using several contexts could let the user control what is synchronized.

    TODO Removing a plugin may cause a failure because the plugin properties are loaded in SynchronizedProperties
    and never unloaded
    """

    def __init__(self, filter_stack, properties_order: PropertiesOrder):
        self._properties: Dict[BlRna, Properties] = {}
        self._filter_stack: FilterStack = filter_stack
        self._unhandled_bpy_data_collection_names: Optional[List[str]] = None
        self._properties_order = properties_order

    @lru_cache()
    def _order(self):
        return {k.bl_rna: v for k, v in self._properties_order.items()}

    def _sort(self, bl_rna, properties: List[T.Property]):

        for class_ in bases(bl_rna):
            bl_rna = None if class_ is None else class_.bl_rna
            ordered_identifiers = self._order().get(bl_rna)
            if ordered_identifiers is not None:
                break

        if not ordered_identifiers:
            return properties

        identifier_order = {identifier: i for i, identifier in enumerate(ordered_identifiers)}

        def predicate(prop: T.Property):
            return identifier_order.get(prop.identifier, sys.maxsize)

        return sorted(properties, key=predicate)

    def properties(self, bl_rna_property: T.Property = None, bpy_type=None) -> ItemsView:
        """
        Return the properties to synchronize for bpy_type
        """
        if (bl_rna_property is None) and (bpy_type is None):
            return {}.items()
        if (bl_rna_property is not None) and (bpy_type is not None):
            raise ValueError("Exactly one of bl_rna and bpy_type must be provided")
        if bl_rna_property is not None:
            bl_rna = bl_rna_property.bl_rna
        elif bpy_type is not None:
            bl_rna = bpy_type.bl_rna
        bl_rna_properties = self._properties.get(bl_rna)
        if bl_rna_properties is None:
            filtered_properties = self._filter_stack.apply(bl_rna)
            sorted_properties = self._sort(bl_rna, filtered_properties)
            bl_rna_properties = {p.identifier: p for p in sorted_properties}
            self._properties[bl_rna] = bl_rna_properties
        return bl_rna_properties.items()

    @property
    def unhandled_bpy_data_collection_names(self) -> List[str]:
        """
        Returns the list of bpy.data collection names not handled (synchronized) by this context
        """
        if self._unhandled_bpy_data_collection_names is None:
            handled = {item[0] for item in self.properties(bpy_type=T.BlendData)}
            self._unhandled_bpy_data_collection_names = list(collections_names() - handled)

        return self._unhandled_bpy_data_collection_names


test_filter = FilterStack()

blenddata_exclude = [
    # "brushes" generates harmless warnings when EnumProperty properties are initialized with a value not in the enum
    "brushes",
    # we do not need those
    "screens",
    "window_managers",
    "workspaces",
]
"""Members of bpy.data that will be totally excluded from synchronization.

Do not exclude collections that may be a target of Object.data. It we did so, an Object.data member
would be loaded ad a DatablockProxy instead of a DatablockRefProxy
"""

_exclude_names = [
    # Related to the UI
    "active_index",
    # found in Viewlayer
    "depsgraph",
    "is_editmode",
    "is_embedded_data",
    "is_evaluated",
    "is_library_indirect",
    "library",
    "mixer_uuid",
    "name_full",
    "original",
    "override_library",
    "preview",
    "rna_type",
    "select",
    "tag",
    "type_info",
    "users",
    "use_fake_user",
]
"""Names of properties that are always excluded"""

default_exclusions: FilterSet = {
    None: [
        # Temporary: parent and child are involved in circular reference
        TypeFilterOut(T.PoseBone),
        NameFilterOut(_exclude_names),
    ],
    T.Action: [
        NameFilterOut(
            # Read only
            ["frame_range"]
        )
    ],
    T.ActionGroup: [
        NameFilterOut(
            [
                # a view into FCurve.group
                "channels",
                # UI
                "show_expanded",
                "show_expanded_graph",
            ]
        )
    ],
    T.Armature: [
        NameFilterOut(
            [
                # A non editable view of edit_bones
                "bones",
                # hardcoded in ArmatureProxy
                "edit_bones",
            ]
        )
    ],
    T.AttributeGroup: [
        NameFilterOut(
            [
                # UI
                "active",
            ]
        )
    ],
    T.BezierSplinePoint: [
        NameFilterOut(
            [
                "select_control_point",
                "select_left_handle",
                "select_right_handle",
            ]
        )
    ],
    T.BlendData: [NameFilterOut(blenddata_exclude), TypeFilterIn(T.CollectionProperty)],  # selected collections
    T.Bone: [
        NameFilterOut(
            [
                "parent",
            ]
        )
    ],
    T.Collection: [NameFilterOut(["all_objects"])],
    T.CompositorNodeRLayers: [NameFilterOut(["scene"])],
    T.Curve: [NameFilterOut(["shape_keys"])],
    T.DecimateModifier: [NameFilterOut(["face_count"])],
    T.EditBone: [
        NameFilterOut(
            [
                # UI
                "select",
                "select_head",
                "select_tail",
            ]
        )
    ],
    T.FaceMap: [NameFilterOut(["index"])],
    T.Keyframe: [
        NameFilterOut(
            [
                # UI
                "select_control_point",
                "select_right_handle",
                "select_left_handle",
            ]
        )
    ],
    T.Image: [
        NameFilterOut(
            [
                "is_float",  # and others
                # is packed_files[0]
                "packed_file",
                "pixels",
                "bindcode",
                "has_data",
                "depth",
                "channels",
            ]
        ),
    ],
    T.GreasePencil: [
        # Temporary while we use VRtist message for meshes. Handle the datablock for uuid
        # but do not synchronize its contents
    ],
    T.GPencilLayer: [
        NameFilterOut(
            [
                "active_frame",
                # see internal issue #341
                "parent_type",
            ]
        )
    ],
    T.GPencilStroke: [
        NameFilterOut(
            [
                # Fails comparison in tests. Result Ok without. Seems computed
                "triangles",
                # readonly
                "bound_box_min",
                "bound_box_max",
            ]
        )
    ],
    T.Key: [
        NameFilterOut(
            [
                # is always the first key_blocks item
                "reference_key"
            ]
        )
    ],
    T.LayerCollection: [
        NameFilterOut(
            [
                # UI related
                "hide_viewport",
                # readonly, computed
                "is_visible",
                # A reference to the wrapped Collection
                "collection",
            ]
        )
    ],
    T.Library: [
        NameFilterOut(
            [
                "parent",
                "version",
                # "users_id",
            ]
        )
    ],
    T.MaterialSlot: [
        NameFilterOut(
            [
                # read only
                "name"
            ]
        )
    ],
    T.Mesh: [
        # Temporary while we use VRtist message for meshes. Handle the datablock for uuid
        # but do not synchronize its contents
        # NameFilterIn("name")
        NameFilterOut(
            [
                # views into uv_layers controlled by uv_layer_xxx_index
                "uv_layer_clone",
                "uv_layer_stencil",
                # readonly
                "total_vert_sel",
                "total_edge_sel",
                "total_face_sel",
                "shape_keys",
                # do not know how to update this, probably by vertices count
                "vertex_paint_masks",
            ]
        )
    ],
    T.MeshLoopColorLayer: [NameFilterOut(["active"])],
    T.MeshPolygon: [NameFilterOut(["area", "center", "normal"])],
    T.MeshUVLoopLayer: [NameFilterOut(["active", "active_clone"])],
    T.MeshVertex: [
        NameFilterOut(
            [
                # MeshVertex.groups is updated via Object.vertex_groups
                "groups",
            ]
        )
    ],
    T.Modifier: [
        # UI
        NameFilterOut(["is_active"])
    ],
    T.Node: [
        NameFilterOut(
            [
                "internal_links",
                "type",
                # cannot be written: set by shader editor
                "dimensions",
            ]
        )
    ],
    T.NodeLink: [
        # see NodeLinkProxy
        NameFilterOut(["is_hidden"])
    ],
    T.NodeSocket: [
        NameFilterOut(
            [
                # XxxNodeGroup save will modify other XxxShaderNode socker identifier !
                "identifier",
                # readonly
                "is_linked",
                "is_output",
                "label",
                "node",
                "type",
            ]
        )
    ],
    T.NodeSocketInterface: [
        # Currently synchronize builtin shading node sockets only, so assume these attributes are
        # managed only at the Node creation
        NameFilterOut(["identifier", "is_output", "type"])
    ],
    T.NodeTree: [
        NameFilterOut(
            [
                # read only
                "view_center",
                "type",
            ]
        )
    ],
    T.Object: [
        NameFilterOut(
            [
                # bounding box, will be computed
                "dimensions",
                "bound_box",
                "mode",
                # read_only
                "is_instancer",
                "is_from_instancer",
                "is_from_set",
                # UI only, define the target of operators
                "active_material",
                "active_material_index",
                "active_shape_key",
                "active_shape_key_index",
                # TODO temporary, has a seed member that makes some tests fail
                "field",
                # TODO
                "particle_systems",
                # unsupported
                "motion_path",
                "proxy_collection",
                "proxy",
                "soft_body",
                "rigid_body",
                "rigid_body_constraint",
                "image_user",
            ]
        )
    ],
    T.PackedFile: [
        # send by a BLENDER_DATA_MEDIA command, not serialized with proxies
        NameFilterOut(["data"])
    ],
    T.PointCache: [
        NameFilterOut(
            [
                # read_only
                "info",
                "is_backed",
                "is_baking",
                "is_frame_skip",
                "is_outdated",
            ]
        )
    ],
    T.PoseBone: [
        NameFilterOut(
            [
                # views into Armature.bones items
                "bone",
                "child",
                "parent",
                # computed from TRS, and vice versa
                "matrix",
                "matrix_basis",
                # readonly
                "head",
                "is_in_ik_chain",
                "length",
                "matrix_channel",
                "tail",
            ]
        )
    ],
    T.RenderSettings: [
        NameFilterOut(
            [
                # just a view of "right" and "left" from RenderSettings.views
                "stereo_views",
                # Causes error in pass_filter, maybe not useful
                "bake",
                # Engine type (Eevee, Cycle...)
                "engine",
            ]
        )
    ],
    T.Scene: [
        NameFilterOut(
            [
                # each user should be able to view and render through the scene camera she chooses
                "camera",
                # Let each participant play his own time
                "frame_current",
                "frame_current_final",
                "frame_float",
                # messy in tests because setting either may reset the other to frame_start or frame_end
                # would require
                "frame_preview_start",
                "frame_preview_end",
                # just a view into the scene objects
                "objects",
                # Not required and messy: plenty of uninitialized enums, several settings, like "scuplt" are None and
                # it is unclear how to do it.
                "tool_settings",
                # Probably per user setting. Causes a readonly error for StudioLight.spherical_harmonics_coefficients
                "display",
                # TODO temporary, not implemented
                "node_tree",
                "rigidbody_world",
                # TODO
                # a view into builtin U keying_sets ?
                "keying_sets_all",
                # ui: scene transform mode
                "transform_orientation_slots",
                # ui: user settings for viewport content manipulation
                "tool_settings",
                # ui: 3D cursor
                "cursor",
            ]
        ),
    ],
    T.SceneEEVEE: [
        NameFilterOut(
            [
                # Readonly, not meaningful
                "gi_cache_info"
            ]
        )
    ],
    T.SequenceEditor: [NameFilterOut(["active_strip", "sequences_all"])],
    T.ShaderNodeTree: [
        NameFilterOut(
            [
                # UI
                "active_input",
            ]
        )
    ],
    T.ShapeKey: [
        NameFilterOut(
            [
                "frame",
            ]
        )
    ],
    T.Spline: [
        NameFilterOut(
            [
                # FIXME Not always writable. Nurbs only ?
                "order_u",
                "order_v",
                # readonly
                "point_count_u",
                "point_count_v",
            ]
        )
    ],
    T.ViewLayer: [
        # Not useful. Requires array insertion (to do shortly)
        NameFilterOut(["freestyle_settings"]),
        # A view into ViewLayer objects
        NameFilterOut(["objects"]),
        NameFilterOut(["active_layer_collection"]),
    ],
}
"""
Per-type property exclusions
"""


property_order: PropertiesOrder = {
    # Match closest parent type (e.g. NodeTree for ShaderNodeTree)
    T.Action: {
        # before fcurves
        "groups",
    },
    T.ColorManagedViewSettings: {
        "use_curve_mapping",
    },
    T.DriverTarget: {
        # before id
        "id_type",
    },
    T.Material: {
        "use_nodes",
    },
    T.NodeTree: {
        # creating inputs and output create sockets on input and output nodes
        # https://blender.stackexchange.com/questions/184447/how-to-update-nodesocketstring-value-inside-a-node-group-in-a-attribute-node-usi
        "inputs",
        "outputs",
        # must exist before links are saved
        "nodes",
    },
    T.PoseBone: {
        "matrix",
    },
    T.Pose: {
        # before bones
        "bone_groups",
    },
    T.Scene: {
        # Required to save view_layers
        # LayerCollection.children is a view into the corresponding Collection with additional visibility
        # information and it is not possible to add/remove items from it. Saving Scene.collection before
        # Scene.view_layers ensures that LayerCollection.children items are present when Scene.view_layers
        # is saved
        "collection",
        "use_nodes",
    },
    T.World: {
        "use_nodes",
    },
}
"""Properties to deliver first because their value enables the possibility to write other attributes."""


test_filter.append(default_exclusions)
test_properties = SynchronizedProperties(test_filter, property_order)
"""For tests"""

safe_depsgraph_updates = (
    T.Action,
    T.Armature,
    T.Camera,
    T.Collection,
    T.Curve,
    # no updates, builtin font only
    # T.VectorFont,
    T.Image,
    T.GreasePencil,
    T.Key,
    T.Library,
    T.Light,
    T.Material,
    T.Mesh,
    T.MetaBall,
    T.MovieClip,
    T.NodeTree,
    T.Object,
    T.Scene,
    T.Sound,
    T.Texture,
    T.World,
)
"""
Datablock with a type in this list will be processed by the generic synchronization of depsgraph updates.

Add new datablock type in this list to synchronize its updates as detect by depsgraph updates.
See synchronization.md
"""

safe_filter = FilterStack()
safe_blenddata_collections = [
    "actions",
    "armatures",
    "cameras",
    "collections",
    "curves",
    "fonts",
    "grease_pencils",
    "images",
    "libraries",
    "lights",
    "materials",
    "meshes",
    "metaballs",
    "movieclips",
    "node_groups",
    "objects",
    "scenes",
    "shape_keys",
    "sounds",
    "textures",
    "worlds",
]
"""
The bpy.data collections in this list are checked for creation/removal and rename by BpyBlendDiff

Add a new collection to this list to synchronize creation, remova and rename events.
"""

safe_blenddata: FilterSet = {T.BlendData: [NameFilterIn(safe_blenddata_collections)]}
safe_filter.append(default_exclusions)
safe_filter.append(safe_blenddata)
safe_properties = SynchronizedProperties(safe_filter, property_order)
"""
The default context used for synchronization, that provides per-type lists of properties to synchronize
"""
