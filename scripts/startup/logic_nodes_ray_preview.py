# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Logic Nodes ray previews (2026-11-12): the rays, shape casts, throws, view cones and ranges of
the ray nodes (and the spheres of Explode or Implode Impulse and Keep Distance), drawn in the 3D
viewport before the game runs.

Each node's geometry is worked out here exactly as the game engine works it out when the game
starts (the C++ rules are named at each function: LN_RuntimeTreeKinds.inl, LN_MeshRays.cpp,
LN_AimMath.h), from the node's own values and the objects that run its tree. A linked input is
followed through a few nodes whose answer is known before the game runs (constants, Combine /
Separate XYZ, Math, Vector Math, Get Owner, Object By Name, Get Active Camera, Get Object
Attribute, Get Axis Vector, Combine Rotation); anything else is known only in the game, and the
preview marks it (grey, dashed) instead of guessing. Hits are not shown: the editor has no physics
world (collision shapes, masks, sensors, properties), only where each query goes.

What shows (2026-11-12, the user's call): mode Selected (the default) shows a ray node only while
it is selected in an open Logic Node Editor, on the selected objects running its tree (what you
edit, on what you edit it for; working on other nodes leaves the viewport clean); a tree no object
runs shows its selected ray nodes once. Mode Visualized adds the game's own view: the ray nodes
whose Visualize is on, for every object (a Visualize only the game knows counts as on). Nothing
shows while the 3D view's overlays are off.

Nothing here changes Blender's code or the game: it is the public drawing API (as the Connect
Rigid Bodies operator uses it), only in the editor. The game's drawing never calls these
callbacks, the update handler is taken out while a game plays, and blenderplayer registers
nothing. The probe ``ray_preview_parity`` of the Logic Nodes acceptance tests holds the geometry to
what the engine gives; ``tests/python/logic_nodes_ray_preview.py`` holds the rules.
"""

import math
import os

import bpy
from bpy.app.handlers import persistent
from bpy.props import BoolProperty, EnumProperty, PointerProperty
from bpy.types import Panel, PropertyGroup
from mathutils import Euler, Matrix, Vector


# --------------------------------------------------------------------------------------------
# Values that are not known, and empty Object inputs.


class _Unknown:
    """A value only the running game knows (an input linked to a game property, a sensor ...)."""

    __slots__ = ()

    def __repr__(self):
        return "UNKNOWN"


UNKNOWN = _Unknown()


class _Empty:
    """An Object input with nothing in it (most nodes then use the object running the tree)."""

    __slots__ = ()

    def __repr__(self):
        return "EMPTY"


EMPTY = _Empty()

DEFAULT_HIT_COLOR = (1.0, 0.05, 0.02, 1.0)
DEFAULT_MISS_COLOR = (1.0, 0.92, 0.05, 1.0)
SEEN_COLOR = (0.1, 1.0, 0.2, 1.0)
HIDDEN_COLOR = (1.0, 0.05, 0.02, 1.0)
START_COLOR = (0.1, 0.55, 1.0, 1.0)
CONE_COLOR = (0.6, 0.6, 0.6, 1.0)
DYNAMIC_COLOR = (0.62, 0.62, 0.68, 0.9)
REACH_COLOR = (0.2, 0.9, 0.35, 1.0)
NO_REACH_COLOR = (1.0, 0.3, 0.2, 1.0)
PLANE_COLOR = (0.4, 0.7, 1.0, 1.0)
EXPLOSION_COLOR = (1.0, 0.45, 0.1, 1.0)
# Keep Distance: its cushion (and Hold's distance) yellow, where it pulls magenta, as the game's
# Visualize draws its lines (LN_KeepDistance.cpp, draw_keep_distance).
CUSHION_COLOR = (1.0, 0.85, 0.1, 1.0)
PULL_COLOR = (0.9, 0.2, 0.9, 1.0)

# The most line segments drawn at once (dashes count one each): more are left out, and the
# panel says so.
MAX_SEGMENTS = 250000
# Raycast from Mesh casts at most this many rays (LN_RAYCAST_FROM_MESH_MAX_RAYS).
MESH_MAX_RAYS = 256
MT_EPSILON = 1.0e-6


def _finite(vector):
    return all(math.isfinite(component) for component in vector)


def _rotation_of(matrix_world):
    """The object's turn without its scale (the game's NodeGetWorldOrientation)."""
    return matrix_world.to_quaternion().to_matrix()


def _scene_gravity(scene):
    """The game's gravity: straight down, Scene > Game Physics > Gravity (KX_Scene::SetGravity)."""
    return Vector((0.0, 0.0, -float(scene.game_settings.physics_gravity)))


def logic_idname(node):
    """The node type a node works as (a merged node's option is its former node)."""
    return getattr(node, "variant_idname", "") or node.bl_idname


def _socket(sockets, identifier):
    for socket in sockets:
        if socket.identifier == identifier:
            return socket
    return None


# --------------------------------------------------------------------------------------------
# Inputs, and the nodes a linked input can be followed through.


class Evaluator:
    """Reads one node's inputs for one object running its tree (`owner`, None without one)."""

    def __init__(self, scene, depsgraph, owner):
        self.scene = scene
        self.depsgraph = depsgraph
        self.owner = owner
        self._memo = {}
        self._active = set()
        # The data-blocks read (an update of one remakes the preview), and whether an object was
        # found by name (then any object's update does).
        self.deps = set()
        self.uses_names = False
        if owner is not None:
            self.use(owner)

    def use(self, obj):
        """Note an object (and its mesh or camera) the preview reads; returns it."""
        if isinstance(obj, bpy.types.Object):
            self.deps.add(obj.as_pointer())
            data = obj.data
            if data is not None:
                self.deps.add(data.as_pointer())
        return obj

    # Inputs ---------------------------------------------------------------------------------

    def value(self, node, identifier, kind):
        """The input's value as `kind` ('FLOAT', 'INT', 'BOOL', 'VECTOR', 'ROTATION', 'STRING',
        'OBJECT', 'COLOR'), UNKNOWN when only the game knows it, EMPTY for an empty Object."""
        socket = _socket(node.inputs, identifier)
        if socket is None:
            return UNKNOWN
        source = _link_source(socket)
        value = _default_of(socket, kind) if source is None else \
            self.output(source[0], source[1], kind)
        return self.use(value) if kind == 'OBJECT' else value

    def object_or_owner(self, node, identifier):
        """An Object input that means the tree's object when it is empty."""
        value = self.value(node, identifier, 'OBJECT')
        if value is EMPTY:
            return self.owner if self.owner is not None else UNKNOWN
        return value

    def linked(self, node, identifier):
        socket = _socket(node.inputs, identifier)
        return socket is not None and _link_source(socket) is not None

    # Outputs of the nodes known before the game -----------------------------------------------

    def output(self, node, socket, kind):
        key = (node.as_pointer(), socket.identifier, kind)
        if key in self._memo:
            return self._memo[key]
        if key in self._active:
            return UNKNOWN
        self._active.add(key)
        try:
            handler = _OUTPUTS.get(logic_idname(node))
            value = handler(self, node, socket.identifier) if handler is not None else UNKNOWN
            value = _convert(value, kind)
        finally:
            self._active.discard(key)
        self._memo[key] = value
        return value

    def scene_object(self, name):
        """The object a name finds when the game starts (an object of the scene that is on)."""
        obj = self.scene.objects.get(name)
        if obj is None or not obj.visible_get():
            return None
        return obj


def _link_source(socket):
    """(node, output socket) feeding an input, through reroutes; None when nothing (or only muted
    links) is linked. As the compiler (IsUsedLink, ResolveSourceEndpoint): a muted link is no
    link; a muted node still runs (the engine reads no node's mute)."""
    for _depth in range(64):
        links = [link for link in socket.links if not link.is_muted]
        if not links:
            return None
        node = links[0].from_node
        out = links[0].from_socket
        if node.bl_idname != "NodeReroute":
            return node, out
        if not node.inputs:
            return None
        socket = node.inputs[0]
    return None


def _default_of(socket, kind):
    value = getattr(socket, "default_value", None)
    if kind == 'OBJECT':
        return value if value is not None else EMPTY
    if value is None:
        return UNKNOWN
    if kind == 'VECTOR':
        return Vector(value[:3])
    if kind == 'ROTATION':
        return Euler(value[:3], 'XYZ').to_matrix()
    if kind == 'COLOR':
        return tuple(value)
    if kind == 'FLOAT':
        return float(value)
    if kind == 'INT':
        return int(value)
    if kind == 'BOOL':
        return bool(value)
    if kind == 'STRING':
        return str(value)
    return value


def _convert(value, kind):
    """A followed output as the input's type; a type the engine would not connect: UNKNOWN."""
    if value is UNKNOWN:
        return UNKNOWN
    if kind == 'OBJECT':
        return value if value is None or isinstance(value, bpy.types.Object) else UNKNOWN
    if kind == 'VECTOR':
        return value if isinstance(value, Vector) else UNKNOWN
    if kind == 'ROTATION':
        return value if isinstance(value, Matrix) else UNKNOWN
    if kind == 'FLOAT':
        return float(value) if isinstance(value, (float, int)) and not isinstance(value, bool) \
            else UNKNOWN
    if kind == 'INT':
        return value if isinstance(value, int) and not isinstance(value, bool) else UNKNOWN
    if kind == 'BOOL':
        return value if isinstance(value, bool) else UNKNOWN
    if kind == 'STRING':
        return value if isinstance(value, str) else UNKNOWN
    return UNKNOWN


def _out_value(node, identifier):
    socket = _socket(node.outputs, identifier)
    return None if socket is None else getattr(socket, "default_value", None)


def _value_float(_ev, node, _out):
    return float(_out_value(node, "Float"))


def _value_int(_ev, node, _out):
    return int(_out_value(node, "Int"))


def _value_bool(_ev, node, _out):
    return bool(_out_value(node, "Bool"))


def _value_vector(_ev, node, _out):
    return Vector(_out_value(node, "Vector")[:3])


def _combine_xyz(ev, node, _out):
    parts = [ev.value(node, axis, 'FLOAT') for axis in ("X", "Y", "Z")]
    return UNKNOWN if UNKNOWN in parts else Vector(parts)


def _separate_xyz(ev, node, out):
    vector = ev.value(node, "Vector", 'VECTOR')
    if vector is UNKNOWN or out not in ("X", "Y", "Z"):
        return UNKNOWN
    return float(vector["XYZ".index(out)])


def _safe_div(a, b):
    return 0.0 if abs(b) <= 1.0e-20 else a / b


# LN_NodeValues_test's models of the engine's Math and Vector Math.
_MATH = {
    'ADD': lambda a, b: a + b,
    'SUBTRACT': lambda a, b: a - b,
    'MULTIPLY': lambda a, b: a * b,
    'DIVIDE': _safe_div,
    'POWER': lambda a, b: math.pow(a, b),
    'MINIMUM': min,
    'MAXIMUM': max,
    'ABSOLUTE': lambda a, _b: abs(a),
    'SIGN': lambda a, _b: float((a > 0.0) - (a < 0.0)),
    'ROUND': lambda a, _b: float(math.floor(a + 0.5)),
    'FLOOR': lambda a, _b: float(math.floor(a)),
    'CEIL': lambda a, _b: float(math.ceil(a)),
    'TRUNC': lambda a, _b: float(math.trunc(a)),
    'FRACT': lambda a, _b: a - math.floor(a),
    'MODULO': lambda a, b: 0.0 if abs(b) <= 1.0e-20 else math.fmod(a, b),
    'SINE': lambda a, _b: math.sin(a),
    'COSINE': lambda a, _b: math.cos(a),
    'RADIANS': lambda a, _b: a * (math.pi / 180.0),
    'DEGREES': lambda a, _b: a * (180.0 / math.pi),
}
_MATH_UNARY = {'ABSOLUTE', 'SIGN', 'ROUND', 'FLOOR', 'CEIL', 'TRUNC', 'FRACT', 'SINE', 'COSINE',
               'RADIANS', 'DEGREES'}


def _math(ev, node, _out):
    operation = getattr(node, "operation", "")
    function = _MATH.get(operation)
    if function is None:
        return UNKNOWN
    a = ev.value(node, "A", 'FLOAT')
    b = 0.0 if operation in _MATH_UNARY else ev.value(node, "B", 'FLOAT')
    if a is UNKNOWN or b is UNKNOWN:
        return UNKNOWN
    try:
        result = float(function(a, b))
    except (ValueError, OverflowError, ZeroDivisionError):
        return UNKNOWN
    return result if math.isfinite(result) else UNKNOWN


def _vector_math(ev, node, _out):
    operation = getattr(node, "operation", "")
    a = ev.value(node, "A", 'VECTOR')
    if a is UNKNOWN:
        return UNKNOWN
    if operation == 'NORMALIZE':
        length = a.length
        return Vector((0.0, 0.0, 0.0)) if length <= 1.0e-20 else a / length
    if operation == 'ABSOLUTE':
        return Vector([abs(c) for c in a])
    if operation == 'SCALE':
        scale = ev.value(node, "Scale", 'FLOAT')
        return UNKNOWN if scale is UNKNOWN else a * scale
    per_component = {
        'ADD': lambda x, y: x + y,
        'SUBTRACT': lambda x, y: x - y,
        'MULTIPLY': lambda x, y: x * y,
        'DIVIDE': _safe_div,
        'MINIMUM': min,
        'MAXIMUM': max,
    }.get(operation)
    if per_component is None:
        return UNKNOWN
    b = ev.value(node, "B", 'VECTOR')
    if b is UNKNOWN:
        return UNKNOWN
    return Vector([per_component(x, y) for x, y in zip(a, b)])


def _get_owner(ev, _node, _out):
    return ev.owner if ev.owner is not None else UNKNOWN


def _object_by_name(ev, node, _out):
    name = ev.value(node, "Name", 'STRING')
    ev.uses_names = True
    return UNKNOWN if name is UNKNOWN else ev.use(ev.scene_object(name))


def _active_camera(ev, _node, _out):
    camera = ev.scene.camera
    return ev.use(camera) if camera is not None else UNKNOWN


def _object_attribute(ev, node, out):
    obj = ev.object_or_owner(node, "Object")
    if obj is UNKNOWN or obj is None:
        return UNKNOWN
    attribute = getattr(node, "attribute_type", "")
    world = obj.matrix_world
    if out == "Vector":
        return {
            'WORLD_POSITION': lambda: world.translation.copy(),
            'LOCAL_POSITION': lambda: Vector(obj.location),
            'LOCAL_SCALE': lambda: Vector(obj.scale),
            'WORLD_SCALE': lambda: world.to_scale(),
        }.get(attribute, lambda: UNKNOWN)()
    if out == "Orientation":
        if attribute in ('WORLD_ORIENTATION', 'WORLD_TRANSFORM'):
            return _rotation_of(world)
        if attribute in ('LOCAL_ORIENTATION', 'LOCAL_TRANSFORM'):
            return _rotation_of(obj.matrix_basis)
        return UNKNOWN
    if out == "Position":
        if attribute == 'WORLD_TRANSFORM':
            return world.translation.copy()
        if attribute == 'LOCAL_TRANSFORM':
            return Vector(obj.location)
        return UNKNOWN
    if out == "Scale":
        if attribute == 'WORLD_TRANSFORM':
            return world.to_scale()
        if attribute == 'LOCAL_TRANSFORM':
            return Vector(obj.scale)
        return UNKNOWN
    return UNKNOWN


def _axis_vector(ev, node, _out):
    obj = ev.object_or_owner(node, "Object")
    if obj is UNKNOWN or obj is None:
        return UNKNOWN
    axis = getattr(node, "axis", "")
    rotation = _rotation_of(obj.matrix_world)
    column = {'LOCAL_X': 0, 'LOCAL_Y': 1, 'LOCAL_Z': 2, 'LOCAL_NEGATIVE_X': 0,
              'LOCAL_NEGATIVE_Y': 1, 'LOCAL_NEGATIVE_Z': 2}.get(axis)
    if column is None:
        return UNKNOWN
    vector = rotation.col[column].copy()
    return -vector if "NEGATIVE" in axis else vector


def _euler(ev, node, _out):
    angles = [ev.value(node, axis, 'FLOAT') for axis in ("X", "Y", "Z")]
    return UNKNOWN if UNKNOWN in angles else Euler(angles, 'XYZ').to_matrix()


_OUTPUTS = {
    "LogicNativeValueFloat": _value_float,
    "LogicNativeValueInt": _value_int,
    "LogicNativeValueBool": _value_bool,
    "LogicNativeValueVector": _value_vector,
    "LogicNativeCombineXYZ": _combine_xyz,
    "LogicNativeSeparateXYZ": _separate_xyz,
    "LogicNativeMath": _math,
    "LogicNativeVectorMath": _vector_math,
    "LogicNativeGetOwner": _get_owner,
    "LogicNativeObjectByName": _object_by_name,
    "LogicNativeGetActiveCamera": _active_camera,
    "LogicNativeGetObjectAttribute": _object_attribute,
    "LogicNativeGetAxisVector": _axis_vector,
    "LogicNativeEuler": _euler,
}


# --------------------------------------------------------------------------------------------
# What one node shows.


class Preview:
    """What one ray node shows for one object: lines (world ends, colour, dashed), where its name
    goes, and `data`: the numbers the game engine would use (the parity probe compares them).
    `dynamic` lists the inputs only the game knows."""

    __slots__ = ("tree", "node", "owner", "lines", "anchor", "data", "dynamic", "problem",
                 "deps", "uses_names")

    def __init__(self, tree, node, owner):
        self.tree = tree
        self.node = node
        self.owner = owner
        self.lines = []
        self.anchor = None
        self.data = {}
        self.dynamic = []
        self.problem = ""
        self.deps = set()
        self.uses_names = False

    def line(self, a, b, color, dashed=False):
        self.lines.append((Vector(a), Vector(b), color, dashed))

    def polyline(self, points, color, dashed=False):
        for a, b in zip(points, points[1:]):
            self.line(a, b, color, dashed)

    def cross(self, center, size, color):
        for axis in (Vector((1, 0, 0)), Vector((0, 1, 0)), Vector((0, 0, 1))):
            self.line(center - axis * size, center + axis * size, color)

    def arrow(self, a, b, color, dashed=False):
        self.line(a, b, color, dashed)
        direction = Vector(b) - Vector(a)
        length = direction.length
        if length <= MT_EPSILON:
            return
        direction /= length
        head = min(max(length * 0.06, 0.06), 0.4)
        side = _perpendicular(direction)
        up = direction.cross(side)
        for edge in (side, -side, up, -up):
            self.line(b, Vector(b) - direction * head + edge * head * 0.45, color)

    def circle(self, center, axis, radius, color, dashed=False, segments=32):
        axis = Vector(axis)
        if axis.length <= MT_EPSILON or radius <= 0.0:
            return
        axis.normalize()
        u = _perpendicular(axis)
        v = axis.cross(u)
        points = [Vector(center) + (u * math.cos(t) + v * math.sin(t)) * radius
                  for t in (2.0 * math.pi * i / segments for i in range(segments + 1))]
        self.polyline(points, color, dashed)

    def sphere(self, center, radius, color, rotation=None, dashed=False):
        rotation = rotation if rotation is not None else Matrix.Identity(3)
        for column in range(3):
            self.circle(center, rotation.col[column], radius, color, dashed)

    def box(self, center, rotation, half, color):
        corners = []
        for sx in (-1, 1):
            for sy in (-1, 1):
                for sz in (-1, 1):
                    corners.append(Vector(center) + rotation @ Vector(
                        (sx * half[0], sy * half[1], sz * half[2])))
        for i, a in enumerate(corners):
            for j in range(i + 1, 8):
                if bin(i ^ j).count("1") == 1:
                    self.line(a, corners[j], color)

    def capsule(self, center, rotation, radius, half_height, color):
        """Jolt's capsule stands along its own Z (Blender's up)."""
        axis = rotation.col[2]
        top = Vector(center) + axis * half_height
        bottom = Vector(center) - axis * half_height
        self.circle(top, axis, radius, color)
        self.circle(bottom, axis, radius, color)
        for column in (0, 1):
            side = rotation.col[column] * radius
            self.line(top + side, bottom + side, color)
            self.line(top - side, bottom - side, color)
            # The caps: half circles over each end, through its pole.
            for end, sign in ((top, 1.0), (bottom, -1.0)):
                arc = [end + side * math.cos(t) + axis * (sign * radius * math.sin(t))
                       for t in (math.pi * i / 16 for i in range(17))]
                self.polyline(arc, color)

    def unknown_at(self, position, names):
        """A grey dashed ring where the query starts (or the object), for inputs only the game
        knows."""
        self.dynamic.extend(names)
        if position is None or position is UNKNOWN:
            return
        position = Vector(position)
        self.anchor = self.anchor or position
        for axis in (Vector((0, 0, 1)), Vector((1, 0, 0))):
            self.circle(position, axis, 0.25, DYNAMIC_COLOR, dashed=True, segments=24)


def _perpendicular(direction):
    seed = Vector((0.0, 0.0, 1.0)) if abs(direction.z) < 0.9 else Vector((1.0, 0.0, 0.0))
    return seed.cross(direction).normalized()


def _color(ev, node, identifier, fallback):
    if _socket(node.inputs, identifier) is None or ev.linked(node, identifier):
        return fallback
    value = ev.value(node, identifier, 'COLOR')
    return tuple(value) if value is not UNKNOWN else fallback


def _missing(*values):
    return any(value is UNKNOWN for value in values)


def _owner_position(ev):
    return ev.owner.matrix_world.translation.copy() if ev.owner is not None else None


# --------------------------------------------------------------------------------------------
# The nodes (each as LN_RuntimeTreeKinds.inl's query kind of the same name works it out).


def _raycast(ev, preview):
    """Raycast / Raycast All (LN_QueryExpressionKind::Raycast): Local turns Origin (and End Point's
    Destination) by the Caster's whole transform and Direction by its turn alone; an empty Caster
    in Local is the tree's object. Direction is made unit length; Max Distance is in world units."""
    node = preview.node
    color = _color(ev, node, "Miss Color", DEFAULT_MISS_COLOR)
    end_point = getattr(node, "input_mode", "") == 'END_POINT'
    local = ev.value(node, "Local", 'BOOL')
    origin = ev.value(node, "Origin", 'VECTOR')
    if end_point:
        destination = ev.value(node, "Destination", 'VECTOR')
        direction = distance = None
    else:
        direction = ev.value(node, "Direction", 'VECTOR')
        distance = ev.value(node, "Max Distance", 'FLOAT')
        destination = None
    if local is UNKNOWN:
        preview.unknown_at(_owner_position(ev), ["Local"])
        return
    if local:
        caster = ev.object_or_owner(node, "Caster")
        if caster is UNKNOWN or caster is None:
            preview.unknown_at(_owner_position(ev), ["Caster"])
            return
        transform = caster.matrix_world
        if origin is not UNKNOWN:
            origin = transform @ origin
        if end_point and destination is not UNKNOWN:
            destination = transform @ destination
        if not end_point and direction is not UNKNOWN:
            direction = _rotation_of(transform) @ direction
    if origin is UNKNOWN:
        preview.unknown_at(_owner_position(ev), ["Origin"])
        return
    preview.anchor = origin
    if end_point:
        if destination is UNKNOWN:
            preview.unknown_at(origin, ["Destination"])
            return
    else:
        if direction is UNKNOWN or distance is UNKNOWN:
            preview.unknown_at(origin, ["Direction" if direction is UNKNOWN else "Max Distance"])
            if direction is not UNKNOWN and direction.length > MT_EPSILON:
                preview.line(origin, origin + direction.normalized(), DYNAMIC_COLOR, dashed=True)
            return
        if distance <= 0.0 or direction.length <= MT_EPSILON:
            preview.problem = "no direction or distance"
            return
        direction = direction.normalized()
        destination = origin + direction * distance
    if (destination - origin).length <= MT_EPSILON:
        preview.problem = "the ray has no length"
        return
    preview.data.update(origin=origin, destination=destination,
                        direction=(destination - origin).normalized())
    preview.arrow(origin, destination, color)


def _shape_cast(ev, preview):
    """Shape Cast (All) (LN_QueryExpressionKind::ShapeCast): Local moves Origin and Destination by
    the Caster's whole transform and turns the shape by its turn; Overlap is the shape at Origin.
    Box: Half Extents; Capsule: Radius and the whole Height along its Z; Extra Radius rounds it."""
    node = preview.node
    color = _color(ev, node, "Miss Color", DEFAULT_MISS_COLOR)
    shape = getattr(node, "shape_type", 'SPHERE')
    overlap = getattr(node, "cast_mode", 'SWEEP') == 'OVERLAP'
    origin = ev.value(node, "Origin", 'VECTOR')
    destination = origin if overlap else ev.value(node, "Destination", 'VECTOR')
    rotation = ev.value(node, "Rotation", 'ROTATION') if shape != 'SPHERE' else Matrix.Identity(3)
    local = ev.value(node, "Local", 'BOOL')
    if local is UNKNOWN:
        preview.unknown_at(_owner_position(ev), ["Local"])
        return
    if local:
        caster = ev.object_or_owner(node, "Caster")
        if caster is UNKNOWN or caster is None:
            preview.unknown_at(_owner_position(ev), ["Caster"])
            return
        transform = caster.matrix_world
        if origin is not UNKNOWN:
            origin = transform @ origin
        if destination is not UNKNOWN:
            destination = transform @ destination
        if rotation is not UNKNOWN:
            rotation = _rotation_of(transform) @ rotation
    if origin is UNKNOWN:
        preview.unknown_at(_owner_position(ev), ["Origin"])
        return
    preview.anchor = origin
    extra = ev.value(node, "Extra Radius", 'FLOAT')
    radius = ev.value(node, "Radius", 'FLOAT') if shape != 'BOX' else 0.0
    half = ev.value(node, "Half Extents", 'VECTOR') if shape == 'BOX' else None
    height = ev.value(node, "Height", 'FLOAT') if shape == 'CAPSULE' else 0.0
    missing = [name for name, value in (("Destination", destination), ("Rotation", rotation),
                                        ("Extra Radius", extra), ("Radius", radius),
                                        ("Half Extents", half), ("Height", height))
               if value is UNKNOWN]
    if missing:
        preview.unknown_at(origin, missing)
        return
    if extra < 0.0 or (shape == 'BOX' and min(half) <= 0.0) or \
            (shape != 'BOX' and radius <= 0.0) or (shape == 'CAPSULE' and height <= 0.0):
        preview.problem = "the shape has no size"
        return

    def draw(center, line_color):
        if shape == 'SPHERE':
            preview.sphere(center, radius + extra, line_color, rotation)
        elif shape == 'BOX':
            preview.box(center, rotation, [h + extra for h in half], line_color)
        else:
            cylinder = height * 0.5 - radius
            if cylinder <= MT_EPSILON:
                preview.sphere(center, radius + extra, line_color, rotation)
            else:
                preview.capsule(center, rotation, radius + extra, cylinder, line_color)

    preview.data.update(origin=origin, destination=destination, rotation=rotation)
    if overlap or (destination - origin).length <= MT_EPSILON:
        draw(origin, color)
        return
    draw(origin, START_COLOR)
    preview.arrow(origin, destination, color)
    draw(destination, color)


# Raycast from Mesh ------------------------------------------------------------------------


class _MeshData:
    """One evaluated mesh's reads for the rays (kept for one rebuild of the previews)."""

    def __init__(self, mesh):
        self.mesh = mesh
        self._vertex_faces = None

    def vertex_face_counts(self):
        if self._vertex_faces is None:
            counts = [0] * len(self.mesh.vertices)
            for polygon in self.mesh.polygons:
                for vertex in set(polygon.vertices):
                    counts[vertex] += 1
            self._vertex_faces = counts
        return self._vertex_faces


def _mesh_group_members(eval_object, mesh, name, faces):
    """LN_MeshRaySelectGroup: the elements whose value is above 0, from a vertex group or a Float /
    Integer / Boolean attribute; faces of a vertex value cast when all their corners do, a face
    attribute picks faces by its own value. None: no such group (no rays). UNKNOWN: an attribute
    the preview cannot read as the engine does."""
    group = eval_object.vertex_groups.get(name)
    if group is not None:
        values = [0.0] * len(mesh.vertices)
        for vertex in mesh.vertices:
            for element in vertex.groups:
                if element.group == group.index:
                    values[vertex.index] = element.weight
    else:
        attribute = mesh.attributes.get(name)
        if attribute is None:
            return None
        if attribute.data_type not in ('FLOAT', 'INT', 'INT8', 'BOOLEAN'):
            return UNKNOWN
        if faces and attribute.domain == 'FACE':
            return [i for i, item in enumerate(attribute.data) if float(item.value) > 0.0]
        if attribute.domain != 'POINT':
            return UNKNOWN
        values = [float(item.value) for item in attribute.data]
    if not faces:
        return [i for i, value in enumerate(values) if value > 0.0]
    return [polygon.index for polygon in mesh.polygons
            if all(values[vertex] > 0.0 for vertex in polygon.vertices)]


def pick_mesh_ray_samples(count, max_rays, sample):
    """LN_PickMeshRaySamples: `min(count, max_rays)` even bins of the elements in order, each giving
    its middle element or the nearest one around it that `sample` accepts (a callable giving a
    sample or None)."""
    output_count = min(count, max_rays)
    samples = []
    if output_count <= 0:
        return samples
    stride, remainder = divmod(count, output_count)

    def boundary(index):
        return stride * index + (remainder * index) // output_count

    for output_index in range(output_count):
        begin = boundary(output_index)
        end = boundary(output_index + 1)
        center = begin + (end - begin - 1) // 2
        picked = sample(center)
        if picked is not None:
            samples.append(picked)
            continue
        for offset in range(1, end - begin):
            has_lower = offset <= center - begin
            has_upper = offset < end - center
            if has_lower:
                picked = sample(center - offset)
                if picked is not None:
                    samples.append(picked)
                    break
            if has_upper:
                picked = sample(center + offset)
                if picked is not None:
                    samples.append(picked)
                    break
            if not has_lower and not has_upper:
                break
    return samples


def _raycast_from_mesh(ev, preview):
    """Raycast from Mesh (LN_QueryExpressionKind::RaycastFromMesh, LN_MeshRays.cpp): rays from the
    Blender faces' centres along their normals, or from the vertices along Blender's vertex
    normals, of the source's evaluated mesh; at most Max Rays (1 to 256) spread evenly; turned by
    the inverse transpose of the object's matrix; Start Offset then Max Distance along each."""
    node = preview.node
    color = _color(ev, node, "Miss Color", DEFAULT_MISS_COLOR)
    source = ev.object_or_owner(node, "Source Object")
    if source is UNKNOWN or source is None:
        preview.unknown_at(_owner_position(ev), ["Source Object"])
        return
    preview.anchor = source.matrix_world.translation.copy()
    start_offset = ev.value(node, "Start Offset", 'FLOAT')
    max_distance = ev.value(node, "Max Distance", 'FLOAT')
    max_rays = ev.value(node, "Max Rays", 'INT')
    group = ev.value(node, "Vertex Group", 'STRING')
    missing = [name for name, value in (("Start Offset", start_offset),
                                        ("Max Distance", max_distance), ("Max Rays", max_rays),
                                        ("Vertex Group", group)) if value is UNKNOWN]
    if missing:
        preview.unknown_at(preview.anchor, missing)
        return
    if not math.isfinite(start_offset) or start_offset < 0.0 or not math.isfinite(max_distance) \
            or max_distance <= MT_EPSILON:
        preview.problem = "Start Offset or Max Distance out of range"
        return
    max_rays = min(max(max_rays, 1), MESH_MAX_RAYS)
    eval_object = source.evaluated_get(ev.depsgraph) if ev.depsgraph is not None else source
    if eval_object.type != 'MESH':
        preview.problem = "the source has no mesh"
        return
    mesh = eval_object.data
    faces = getattr(node, "mesh_domain", 'VERTICES') == 'FACES'
    members = None
    if group:
        members = _mesh_group_members(eval_object, mesh, group, faces)
        if members is None:
            preview.problem = "the mesh has no vertex group or attribute \"%s\"" % group
            return
        if members is UNKNOWN:
            preview.unknown_at(preview.anchor, ["Vertex Group"])
            return
    count = len(members) if members is not None else \
        (len(mesh.polygons) if faces else len(mesh.vertices))
    data = _MeshData(mesh)

    def element_of(index):
        return members[index] if members is not None else index

    def face_sample(index):
        polygon = mesh.polygons[element_of(index)]
        if polygon.loop_total < 3 or not polygon.area > 0.0:
            return None
        return polygon.center.copy(), polygon.normal.copy()

    def vertex_sample(index):
        vertex = element_of(index)
        if data.vertex_face_counts()[vertex] == 0:
            return None
        normal = mesh.vertex_normals[vertex].vector.copy()
        if normal.length <= 0.0 or not _finite(normal):
            return None
        return mesh.vertices[vertex].co.copy(), normal

    samples = pick_mesh_ray_samples(count, max_rays, face_sample if faces else vertex_sample)
    transform = source.matrix_world
    try:
        normal_matrix = transform.to_3x3().inverted().transposed()
    except ValueError:
        preview.problem = "the object is scaled to nothing"
        return
    inverted = getattr(node, "ray_direction", 'NORMALS') == 'INVERTED_NORMALS'
    rays = []
    for position, normal in samples:
        direction = normal_matrix @ normal
        if direction.length <= 0.0:
            continue
        direction.normalize()
        if inverted:
            direction = -direction
        surface = transform @ position
        origin = surface + direction * start_offset
        end = origin + direction * max_distance
        rays.append((origin, end))
        preview.line(origin, end, color)
    preview.data.update(rays=rays)
    if not rays:
        preview.problem = "no face or vertex gives a ray"


# Camera rays ------------------------------------------------------------------------------


def _camera_frame(scene, camera):
    """The camera's frame in its own space (the view the game draws with the scene's resolution),
    as left, right, top, bottom at depth z, and whether it is orthographic."""
    corners = camera.data.view_frame(scene=scene)
    top_right, bottom_right, bottom_left, _top_left = corners
    return (bottom_left.x, top_right.x, top_right.y, bottom_right.y, top_right.z,
            camera.data.type == 'ORTHO')


def camera_ray(scene, camera, aim_x, aim_y, distance):
    """LN_RuntimeTree::ProjectScreenToWorld (Camera Ray) and the pointer ray: from the camera (an
    orthographic one: from the near plane) through the frame's point (0, 0 top left, 1, 1 bottom
    right); returns origin, unit direction."""
    left, right, top, bottom, depth, ortho = _camera_frame(scene, camera)
    x = left + (right - left) * aim_x
    y = top + (bottom - top) * aim_y
    rotation = _rotation_of(camera.matrix_world)
    position = camera.matrix_world.translation.copy()
    if ortho:
        origin = position + rotation @ Vector((x, y, -camera.data.clip_start))
        direction = rotation @ Vector((0.0, 0.0, -1.0))
    else:
        origin = position
        direction = (rotation @ Vector((x, y, depth))).normalized()
    return origin, direction


def _camera_of(ev, node, identifier, use_scene_camera):
    camera = ev.value(node, identifier, 'OBJECT') if identifier else EMPTY
    if camera is EMPTY:
        camera = ev.scene.camera if use_scene_camera else None
    if camera is UNKNOWN:
        return UNKNOWN
    if camera is None or camera.type != 'CAMERA':
        return None
    return ev.use(camera)


def _mouse_ray(ev, preview):
    """Mouse Ray: the cursor is the game's, so the preview shows how far the pointer reaches from
    the camera it casts through (the scene's camera; Custom Camera: its Camera input): the four
    corners and the middle of the view, Max Distance long."""
    node = preview.node
    color = _color(ev, node, "Miss Color", DEFAULT_MISS_COLOR)
    view = getattr(node, "interaction_view", 'AUTOMATIC')
    if view == 'OVERLAY':
        preview.problem = "the overlay camera is chosen while the game runs"
        return
    camera = _camera_of(ev, node, "Camera" if view == 'CUSTOM_CAMERA' else None,
                        use_scene_camera=view != 'CUSTOM_CAMERA')
    distance = ev.value(node, "Distance", 'FLOAT')
    if camera is UNKNOWN or distance is UNKNOWN:
        preview.unknown_at(_owner_position(ev), ["Camera" if camera is UNKNOWN else "Max Distance"])
        return
    if camera is None:
        preview.problem = "no camera"
        return
    if not distance >= 0.0:
        preview.problem = "Max Distance below 0"
        return
    ends = []
    for aim in ((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)):
        origin, direction = camera_ray(ev.scene, camera, aim[0], aim[1], distance)
        ends.append(origin + direction * distance)
        preview.line(origin, ends[-1], color, dashed=True)
    preview.polyline(ends + ends[:1], color)
    origin, direction = camera_ray(ev.scene, camera, 0.5, 0.5, distance)
    preview.arrow(origin, origin + direction * distance, color)
    preview.anchor = origin
    preview.data.update(origin=origin, direction=direction, destination=origin + direction * distance)


def _camera_ray(ev, preview):
    """Camera Ray (LN_QueryExpressionKind::CameraRay): from its Camera (empty: the scene's camera)
    through Aim (0, 0 top left, 1, 1 bottom right), Max Distance long."""
    node = preview.node
    color = _color(ev, node, "Miss Color", DEFAULT_MISS_COLOR)
    camera = _camera_of(ev, node, "Camera", use_scene_camera=True)
    aim = ev.value(node, "Aim", 'VECTOR')
    distance = ev.value(node, "Distance", 'FLOAT')
    missing = [name for name, value in (("Camera", camera), ("Aim", aim),
                                        ("Max Distance", distance)) if value is UNKNOWN]
    if missing:
        preview.unknown_at(_owner_position(ev), missing)
        return
    if camera is None:
        preview.problem = "no camera"
        return
    if not distance >= 0.0:
        preview.problem = "Max Distance below 0"
        return
    origin, direction = camera_ray(ev.scene, camera, aim.x, aim.y, distance)
    destination = origin + direction * distance
    preview.anchor = origin
    preview.data.update(origin=origin, direction=direction, destination=destination)
    preview.arrow(origin, destination, color)


# Throws -----------------------------------------------------------------------------------


def projectile_points(origin, velocity, gravity, distance, resolution, max_time):
    """The Projectile Ray's path without hits (LN_QueryExpressionKind::ProjectileRay): steps of
    max(0.01, 1 - Resolution) seconds, the last one cut at Max Distance along the path or at Max
    Time; returns the points and the time at the last one."""
    distance = max(0.0, distance)
    resolution = min(max(resolution, 0.0), 0.99)
    time_step = max(0.01, 1.0 - resolution)
    total = 0.0
    time_at = 0.0
    points = [origin.copy()]
    for _step in range(2048):
        if not total < distance:
            break
        next_time = time_at + time_step
        capped = False
        if max_time > 0.0 and next_time >= max_time:
            next_time = max_time
            capped = True
        if next_time <= time_at:
            break
        target = gravity * (0.5 * next_time * next_time) + velocity * next_time + origin
        start = points[-1]
        segment = target - start
        segment_length = segment.length
        if not segment_length > 1.0e-6:
            break
        remaining = distance - total
        step_length = min(segment_length, remaining)
        share = step_length / segment_length
        end = start + segment * share if step_length < segment_length else target
        time_at = time_at + (next_time - time_at) * share
        total += step_length
        points.append(end)
        if step_length < segment_length or capped:
            break
    return points, time_at


def _projectile_ray(ev, preview):
    """Projectile Ray: Local places Origin from the Caster's position and turn (no scale) and takes
    Aim as a direction in its space; otherwise Aim is the world point it heads for. Speed Power,
    the scene's gravity (Scene > Game Physics) or its own."""
    node = preview.node
    color = _color(ev, node, "Miss Color", DEFAULT_MISS_COLOR)
    local = ev.value(node, "Local", 'BOOL')
    origin = ev.value(node, "Origin", 'VECTOR')
    aim = ev.value(node, "Aim", 'VECTOR')
    if local is UNKNOWN:
        preview.unknown_at(_owner_position(ev), ["Local"])
        return
    if local:
        caster = ev.object_or_owner(node, "Caster")
        if caster is UNKNOWN or caster is None:
            preview.unknown_at(_owner_position(ev), ["Caster"])
            return
        rotation = _rotation_of(caster.matrix_world)
        position = caster.matrix_world.translation
        if origin is not UNKNOWN:
            origin = position + rotation @ origin
        if aim is not UNKNOWN:
            aim = rotation @ aim
    elif origin is not UNKNOWN and aim is not UNKNOWN:
        aim = aim - origin
    if origin is UNKNOWN:
        preview.unknown_at(_owner_position(ev), ["Origin"])
        return
    preview.anchor = origin
    power = ev.value(node, "Power", 'FLOAT')
    distance = ev.value(node, "Distance", 'FLOAT')
    resolution = ev.value(node, "Resolution", 'FLOAT')
    radius = ev.value(node, "Radius", 'FLOAT')
    max_time = ev.value(node, "Max Time", 'FLOAT')
    custom = getattr(node, "gravity_source", 'SCENE') == 'CUSTOM'
    gravity = ev.value(node, "Gravity", 'VECTOR') if custom else _scene_gravity(ev.scene)
    missing = [name for name, value in (("Aim", aim), ("Power", power), ("Max Distance", distance),
                                        ("Resolution", resolution), ("Radius", radius),
                                        ("Max Time", max_time), ("Gravity", gravity))
               if value is UNKNOWN]
    if missing:
        preview.unknown_at(origin, missing)
        return
    if aim.length <= 0.0:
        preview.problem = "no direction"
        return
    velocity = aim.normalized() * power
    radius = max(radius, 0.0) if math.isfinite(radius) else 0.0
    max_time = max_time if math.isfinite(max_time) and max_time > 0.0 else 0.0
    points, flight_time = projectile_points(origin, velocity, gravity, distance, resolution,
                                            max_time)
    preview.data.update(points=points, flight_time=flight_time, velocity=velocity)
    preview.polyline(points, color)
    preview.cross(origin, 0.1, color)
    if radius > 0.0:
        preview.sphere(points[-1], radius, color)
    elif len(points) > 1:
        preview.cross(points[-1], 0.1, color)


def solve_arc(start, target, gravity, by_speed, value, high_arc):
    """LN_SolveArc (LN_AimMath.h), returns (velocity, flight time, can reach)."""
    none = (Vector((0.0, 0.0, 0.0)), 0.0, False)
    if not (_finite(start) and _finite(target) and _finite(gravity)) or \
            not math.isfinite(value) or value < 0.0:
        return none
    delta = target - start
    distance = delta.length
    tiny = 1.0e-6 * max(1.0, distance)
    g = gravity.length
    v = float(value)
    if g <= 1.0e-9:
        if not by_speed:
            return none
        if distance <= tiny:
            return (Vector((0.0, 0.0, 0.0)), 0.0, True)
        if v <= 0.0:
            return none
        return (delta * (v / distance), distance / v, True)
    up = gravity * (-1.0 / g)
    y = delta.dot(up)
    horizontal = delta - up * y
    x = horizontal.length
    vertical = x <= tiny
    forward = Vector((0.0, 0.0, 0.0)) if vertical else horizontal / x
    if not by_speed:
        top = max(0.0, y) + v
        rise = math.sqrt(2.0 * g * top)
        fall = top - y
        time = rise / g + math.sqrt(max(0.0, 2.0 * fall / g))
        if time <= 1.0e-12:
            return (Vector((0.0, 0.0, 0.0)), 0.0, True) if vertical else none
        return (horizontal / time + up * rise, time, True)
    if v <= 0.0:
        return (Vector((0.0, 0.0, 0.0)), 0.0, True) if distance <= tiny else none
    if vertical:
        if y >= 0.0:
            disc = v * v - 2.0 * g * y
            if disc < 0.0:
                return (up * v, 0.0, False)
            root = math.sqrt(disc)
            return (up * v, (v + root if high_arc else v - root) / g, True)
        root = math.sqrt(v * v - 2.0 * g * y)
        if high_arc:
            return (up * v, (v + root) / g, True)
        return (up * -v, (root - v) / g, True)
    v2 = v * v
    disc = v2 * v2 - g * (g * x * x + 2.0 * y * v2)
    if disc < 0.0:
        angle = 0.5 * (math.pi * 0.5 + math.atan2(y, x))
        return (forward * (v * math.cos(angle)) + up * (v * math.sin(angle)), 0.0, False)
    root = math.sqrt(disc)
    angle = math.atan((v2 + root if high_arc else v2 - root) / (g * x))
    cos_angle = math.cos(angle)
    return (forward * (v * cos_angle) + up * (v * math.sin(angle)), x / (v * cos_angle), True)


def _arc_points(start, velocity, gravity, time, segments=40):
    return [start + velocity * t + gravity * (0.5 * t * t)
            for t in (time * i / segments for i in range(segments + 1))]


def _aim_for_arc(ev, preview):
    """Aim for Arc: the throw that lands on Target (both arcs when High Arc is known only in the
    game); out of reach: the farthest throw, dashed, until it is back at the start's height."""
    node = preview.node
    start = ev.value(node, "Start", 'VECTOR')
    target = ev.value(node, "Target", 'VECTOR')
    by_speed = getattr(node, "solve_by", 'SPEED') == 'SPEED'
    custom = getattr(node, "gravity_source", 'SCENE') == 'CUSTOM'
    gravity = ev.value(node, "Gravity", 'VECTOR') if custom else _scene_gravity(ev.scene)
    amount = ev.value(node, "Speed" if by_speed else "Arc Height", 'FLOAT')
    high = ev.value(node, "High Arc", 'BOOL') if by_speed else False
    if start is UNKNOWN:
        preview.unknown_at(_owner_position(ev), ["Start"])
        return
    preview.anchor = start
    missing = [name for name, value in (("Target", target), ("Gravity", gravity),
                                        ("Speed" if by_speed else "Arc Height", amount))
               if value is UNKNOWN]
    if missing:
        preview.unknown_at(start, missing)
        return
    preview.cross(target, 0.15, START_COLOR)
    choices = [False, True] if high is UNKNOWN else [high]
    if high is UNKNOWN:
        preview.dynamic.append("High Arc")
    results = []
    for high_arc in choices:
        velocity, time, reach = solve_arc(start, target, gravity, by_speed, amount, high_arc)
        results.append((velocity, time, reach))
        if reach and time > 0.0:
            preview.polyline(_arc_points(start, velocity, gravity, time), REACH_COLOR,
                             dashed=high is UNKNOWN and high_arc)
        elif not reach and velocity.length > 0.0:
            g = gravity.length
            rise = velocity.dot(-gravity / g) if g > 0.0 else 0.0
            show = 2.0 * rise / g if g > 0.0 and rise > 0.0 else 1.0
            preview.polyline(_arc_points(start, velocity, gravity, show), NO_REACH_COLOR,
                             dashed=True)
    velocity, time, reach = results[0]
    preview.data.update(velocity=velocity, flight_time=time, can_reach=reach)


def ray_plane(origin, direction, point, normal):
    """LN_RayPlaneDistance: how far along the unit direction the ray meets the plane, -1 never."""
    if not (_finite(origin) and _finite(direction) and _finite(point) and _finite(normal)):
        return -1.0
    if direction.length <= 1.0e-12 or normal.length <= 1.0e-12:
        return -1.0
    d = direction.normalized()
    n = normal.normalized()
    facing = d.dot(n)
    if abs(facing) <= 1.0e-9:
        return -1.0
    t = -(origin - point).dot(n) / facing
    return t if t >= 0.0 and math.isfinite(t) else -1.0


def _ray_to_plane(ev, preview):
    """Ray to Plane: the plane (a square around Plane Point), its normal, and the ray to where it
    meets the plane (dashed when it never does)."""
    node = preview.node
    origin = ev.value(node, "Ray Origin", 'VECTOR')
    direction = ev.value(node, "Ray Direction", 'VECTOR')
    point = ev.value(node, "Plane Point", 'VECTOR')
    normal = ev.value(node, "Plane Normal", 'VECTOR')
    if point is not UNKNOWN and normal is not UNKNOWN and normal.length > 1.0e-12:
        size = 1.0
        if origin is not UNKNOWN:
            size = min(max(0.25 * (origin - point).length, 0.5), 5.0)
        unit = normal.normalized()
        u = _perpendicular(unit)
        v = unit.cross(u)
        corners = [point + (u * a + v * b) * size for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        preview.polyline(corners + corners[:1], PLANE_COLOR)
        preview.line(point - u * size, point + u * size, PLANE_COLOR, dashed=True)
        preview.line(point - v * size, point + v * size, PLANE_COLOR, dashed=True)
        preview.arrow(point, point + unit * size * 0.5, PLANE_COLOR)
    if origin is UNKNOWN:
        preview.unknown_at(point if point is not UNKNOWN else _owner_position(ev), ["Ray Origin"])
        return
    preview.anchor = origin
    missing = [name for name, value in (("Ray Direction", direction), ("Plane Point", point),
                                        ("Plane Normal", normal)) if value is UNKNOWN]
    if missing:
        preview.unknown_at(origin, missing)
        return
    t = ray_plane(origin, direction, point, normal)
    preview.data.update(distance=t)
    if t >= 0.0:
        hit = origin + direction.normalized() * t
        preview.data.update(point=hit)
        preview.arrow(origin, hit, DEFAULT_MISS_COLOR)
        preview.cross(hit, 0.12, DEFAULT_HIT_COLOR)
    elif direction.length > 1.0e-12:
        preview.line(origin, origin + direction.normalized() * 5.0, DEFAULT_MISS_COLOR,
                     dashed=True)


# Sight, sweeps, blasts --------------------------------------------------------------------


def _line_of_sight(ev, preview):
    """Line of Sight (LN_QueryExpressionKind::LineOfSight): the eye (the Eye object's whole
    transform on Eye Offset, its turn on Forward; or Eye Position and a world Forward), the view
    cone (View Angle wide, Max Distance long; all around: a sphere), and the line to the target in
    the seen colour when it is in range and in view (what may block it is known only in the game),
    else the hidden colour, faded."""
    node = preview.node
    seen_color = _color(ev, node, "Seen Color", SEEN_COLOR)
    hidden_color = _color(ev, node, "Hidden Color", HIDDEN_COLOR)
    eye_object = getattr(node, "eye_mode", 'OBJECT') == 'OBJECT'
    target_object = getattr(node, "target_mode", 'OBJECT') == 'OBJECT'
    forward = ev.value(node, "Forward", 'VECTOR')
    if eye_object:
        obj = ev.object_or_owner(node, "Eye")
        offset = ev.value(node, "Eye Offset", 'VECTOR')
        if obj is UNKNOWN or obj is None or offset is UNKNOWN:
            preview.unknown_at(_owner_position(ev), ["Eye" if offset is not UNKNOWN else
                                                     "Eye Offset"])
            return
        eye = obj.matrix_world @ offset
        if forward is not UNKNOWN:
            forward = _rotation_of(obj.matrix_world) @ forward
    else:
        eye = ev.value(node, "Eye Position", 'VECTOR')
        if eye is UNKNOWN:
            preview.unknown_at(_owner_position(ev), ["Eye Position"])
            return
    preview.anchor = eye
    max_distance = ev.value(node, "Max Distance", 'FLOAT')
    view_angle = ev.value(node, "View Angle", 'FLOAT')
    missing = [name for name, value in (("Forward", forward), ("Max Distance", max_distance),
                                        ("View Angle", view_angle)) if value is UNKNOWN]
    if missing:
        preview.unknown_at(eye, missing)
        return
    target = None
    if target_object:
        obj = ev.value(node, "Target", 'OBJECT')
        offset = ev.value(node, "Target Offset", 'VECTOR')
        if obj is UNKNOWN or offset is UNKNOWN:
            preview.dynamic.append("Target" if obj is UNKNOWN else "Target Offset")
        elif obj is not EMPTY and obj is not None:
            target = obj.matrix_world @ offset
    else:
        target = ev.value(node, "Target Position", 'VECTOR')
        if target is UNKNOWN:
            preview.dynamic.append("Target Position")
            target = None

    forward_length = forward.length
    all_around = forward_length <= MT_EPSILON or view_angle >= 2.0 * math.pi
    distance = (target - eye).length if target is not None else 0.0
    reach = max_distance if max_distance > 0.0 else max(distance, 1.0)
    if all_around:
        if max_distance > 0.0:
            preview.sphere(eye, max_distance, CONE_COLOR, dashed=True)
    else:
        axis = forward / forward_length
        side = _perpendicular(axis)
        up = axis.cross(side).normalized()
        half = min(max(view_angle * 0.5, 0.0), math.pi)
        for edge in (side, -side, up, -up):
            preview.line(eye, eye + (axis * math.cos(half) + edge * math.sin(half)) * reach,
                         CONE_COLOR)
        preview.circle(eye + axis * (math.cos(half) * reach), axis, abs(math.sin(half)) * reach,
                       CONE_COLOR)
        preview.arrow(eye, eye + axis * min(reach * 0.25, 1.0), CONE_COLOR)
    preview.cross(eye, 0.08, seen_color)
    if target is None:
        return
    angle = 0.0
    if distance > MT_EPSILON and forward_length > MT_EPSILON:
        cosine = min(max((target - eye).dot(forward) / (distance * forward_length), -1.0), 1.0)
        angle = math.degrees(math.acos(cosine))
    in_range = max_distance <= 0.0 or distance <= max_distance
    in_view = all_around or angle <= math.degrees(view_angle * 0.5) + 1.0e-4
    preview.data.update(eye=eye, target=target, distance=distance, angle=angle,
                        in_range=in_range, in_view=in_view)
    if in_range and in_view:
        preview.line(eye, target, seen_color)
    else:
        faded = (hidden_color[0], hidden_color[1], hidden_color[2], hidden_color[3] * 0.35)
        preview.line(eye, target, faded, dashed=True)
    preview.cross(target, 0.1, seen_color if in_range and in_view else hidden_color)


def _sweep_object(ev, preview):
    """Sweep Object (LN_QueryExpressionKind::SweepObject): the object's bounds where it is and at
    Destination (Local: in its own space), turned its own way or by Rotation (Local: on top of its
    own turn). The bounds stand for its collision shape."""
    node = preview.node
    color = _color(ev, node, "Miss Color", DEFAULT_MISS_COLOR)
    obj = ev.object_or_owner(node, "Object")
    if obj is UNKNOWN or obj is None:
        preview.unknown_at(_owner_position(ev), ["Object"])
        return
    position = obj.matrix_world.translation.copy()
    preview.anchor = position
    destination = ev.value(node, "Destination", 'VECTOR')
    local = ev.value(node, "Local", 'BOOL')
    custom = getattr(node, "rotation_mode", 'CURRENT') == 'CUSTOM'
    rotation = ev.value(node, "Rotation", 'ROTATION') if custom else None
    missing = [name for name, value in (("Destination", destination), ("Local", local),
                                        ("Rotation", rotation)) if value is UNKNOWN]
    if missing:
        preview.unknown_at(position, missing)
        return
    own = _rotation_of(obj.matrix_world)
    orientation = own
    if local:
        destination = obj.matrix_world @ destination
        if custom:
            orientation = own @ rotation
    elif custom:
        orientation = rotation
    eval_object = obj.evaluated_get(ev.depsgraph) if ev.depsgraph is not None else obj
    corners = [Vector(corner) for corner in eval_object.bound_box]
    low = Vector([min(c[i] for c in corners) for i in range(3)])
    high = Vector([max(c[i] for c in corners) for i in range(3)])
    scale = obj.matrix_world.to_scale()
    half = [abs((high[i] - low[i]) * 0.5 * scale[i]) for i in range(3)]
    middle = Vector([(high[i] + low[i]) * 0.5 * scale[i] for i in range(3)])
    preview.data.update(start=position, destination=destination, orientation=orientation)
    if max(half) > 0.0:
        preview.box(position + own @ middle, own, half, START_COLOR)
        preview.box(destination + orientation @ middle, orientation, half, color)
    preview.arrow(position, destination, color)


def _explosion(ev, preview):
    """Explode or Implode Impulse: its sphere, Radius around the Object (empty: the tree's
    object) or the Origin; arrows out (Explode) or in (Implode)."""
    node = preview.node
    if getattr(node, "center", 'OBJECT') == 'OBJECT':
        obj = ev.object_or_owner(node, "Object")
        if obj is UNKNOWN or obj is None:
            preview.unknown_at(_owner_position(ev), ["Object"])
            return
        center = obj.matrix_world.translation.copy()
    else:
        center = ev.value(node, "Origin", 'VECTOR')
        if center is UNKNOWN:
            preview.unknown_at(_owner_position(ev), ["Origin"])
            return
    preview.anchor = center
    radius = ev.value(node, "Radius", 'FLOAT')
    if radius is UNKNOWN:
        preview.unknown_at(center, ["Radius"])
        return
    if not radius > 0.0:
        preview.problem = "Radius 0"
        return
    preview.data.update(center=center, radius=radius)
    preview.sphere(center, radius, EXPLOSION_COLOR)
    implode = getattr(node, "direction", 'EXPLODE') == 'IMPLODE'
    for axis in (Vector((1, 0, 0)), Vector((-1, 0, 0)), Vector((0, 1, 0)), Vector((0, -1, 0)),
                 Vector((0, 0, 1))):
        a, b = center + axis * radius * 0.35, center + axis * radius * 0.75
        preview.arrow(b if implode else a, a if implode else b, EXPLOSION_COLOR)


def keep_distance_shape(obj):
    """The shape Keep Distance's Object's Shape measures from, as the converter builds the
    object's body (JoltPhysicsEnvironment's bounds rules): 'SPHERE', 'BOX', 'CAPSULE', 'CYLINDER',
    'CONE' or 'CONVEX_HULL'; None where the engine measures from the Radius sphere (Jolt's
    ProximityBatch): no body, a soft body, a triangle mesh (static-only in Jolt), an empty shape."""
    game = obj.game
    kind = game.physics_type
    if kind in ('NO_COLLISION', 'OCCLUDER', 'NAVMESH', 'SOFT_BODY'):
        return None
    if game.use_collision_bounds:
        bounds = game.collision_bounds_type
        if bounds in ('CONVEX_HULL', 'TRIANGLE_MESH') and obj.type != 'MESH':
            bounds = 'SPHERE'
    elif kind in ('CHARACTER', 'DYNAMIC', 'VEHICLE'):
        bounds = 'SPHERE'
    elif kind in ('RIGID_BODY', 'KINEMATIC'):
        bounds = 'BOX'
    else:
        bounds = 'TRIANGLE_MESH'
    return None if bounds in ('TRIANGLE_MESH', 'EMPTY') else bounds


def _keep_distance(ev, preview):
    """Keep Distance (LN_KeepDistance.cpp): what it measures from, blue (the object's own
    collision shape as the converter builds it, its bounds around the origin, which stand for a
    cylinder, cone or convex hull; or the Radius sphere: Measure From Sphere, or an object whose
    shape it cannot take), and that shape grown by Distance: yellow where it pushes (Push's
    cushion, Hold's distance kept), magenta where it pulls (Pull); Hold also grown by Reach,
    magenta: how far it pulls back (LN_KeepDistanceRange: at least Distance). A box grown is
    rounded at its edges in the game. The game's Visualize draws its lines in the same colours.
    The Mode is an input (0 Push, 1 Pull, 2 Hold; below 0 Push, above 2 Hold); one only the game
    knows draws as Hold, both ranges shown (data mode 'UNKNOWN')."""
    node = preview.node
    obj = ev.object_or_owner(node, "Object")
    if obj is UNKNOWN or obj is None:
        preview.unknown_at(_owner_position(ev), ["Object"])
        return
    center = obj.matrix_world.translation.copy()
    preview.anchor = center
    mode_value = ev.value(node, "Mode", 'INT')
    mode = 'UNKNOWN' if mode_value is UNKNOWN else \
        ('PUSH', 'PULL', 'HOLD')[min(max(int(mode_value), 0), 2)]
    distance = ev.value(node, "Distance", 'FLOAT')
    if distance is UNKNOWN:
        preview.unknown_at(center, ["Distance"])
        return
    reach = None
    if mode in ('HOLD', 'UNKNOWN'):
        reach = ev.value(node, "Reach", 'FLOAT')
        if reach is UNKNOWN:
            preview.unknown_at(center, ["Reach"])
            return
        reach = max(reach, distance)
    if not distance >= 0.0 or not (reach if reach is not None else distance) > 0.0:
        preview.problem = "Distance 0"
        return
    shells = []
    if distance > 0.0:
        shells.append((distance, PULL_COLOR if mode == 'PULL' else CUSHION_COLOR))
    if reach is not None and reach > distance:
        shells.append((reach, PULL_COLOR))
    preview.data.update(center=center, mode=mode, distance=distance,
                        range=reach if reach is not None else distance)
    if reach is not None:
        preview.data["reach"] = reach
    shape = None
    if getattr(node, "measure_from", 'OBJECT_SHAPE') == 'OBJECT_SHAPE':
        shape = keep_distance_shape(obj)
    if shape is None:
        radius = ev.value(node, "Radius", 'FLOAT')
        if radius is UNKNOWN:
            preview.unknown_at(center, ["Radius"])
            return
        radius = max(radius, 1.0e-4) if math.isfinite(radius) else 1.0e-4
        preview.data.update(shape='SPHERE', radius=radius)
        preview.sphere(center, radius, START_COLOR)
        for grow, color in shells:
            preview.sphere(center, radius + grow, color)
        return
    rotation = _rotation_of(obj.matrix_world)
    eval_object = obj.evaluated_get(ev.depsgraph) if ev.depsgraph is not None else obj
    corners = [Vector(corner) for corner in eval_object.bound_box]
    scale = obj.matrix_world.to_scale()
    half = [abs(0.5 * (max(c[i] for c in corners) - min(c[i] for c in corners)) * scale[i])
            for i in range(3)]
    preview.data.update(shape=shape, half=half)
    if shape == 'SPHERE':
        radius = max(half)
        preview.data["radius"] = radius
        preview.sphere(center, radius, START_COLOR, rotation)
        for grow, color in shells:
            preview.sphere(center, radius + grow, color, rotation)
    elif shape == 'CAPSULE':
        radius = max(half[0], half[1])
        half_height = max(half[2] - radius, 0.0)
        preview.data.update(radius=radius, half_height=half_height)
        preview.capsule(center, rotation, radius, half_height, START_COLOR)
        for grow, color in shells:
            preview.capsule(center, rotation, radius + grow, half_height, color)
    else:
        preview.box(center, rotation, half, START_COLOR)
        for grow, color in shells:
            preview.box(center, rotation, [h + grow for h in half], color)


RAY_NODES = {
    "LogicNativeRaycast": _raycast,
    "LogicNativeRaycastAll": _raycast,
    "LogicNativeShapeCast": _shape_cast,
    "LogicNativeShapeCastAll": _shape_cast,
    "LogicNativeRaycastFromMesh": _raycast_from_mesh,
    "LogicNativeMouseRay": _mouse_ray,
    "LogicNativeCameraRay": _camera_ray,
    "LogicNativeProjectileRay": _projectile_ray,
    "LogicNativeLineOfSight": _line_of_sight,
    "LogicNativeSweepObject": _sweep_object,
    "LogicNativeAimForArc": _aim_for_arc,
    "LogicNativeRayToPlane": _ray_to_plane,
    "LogicNativeExplosionImpulse": _explosion,
    "LogicNativeKeepDistance": _keep_distance,
}


def is_ray_node(node):
    return logic_idname(node) in RAY_NODES


def preview_node(scene, depsgraph, tree, node, owner):
    """One node's preview for one object running its tree (None: no object)."""
    preview = Preview(tree, node, owner)
    function = RAY_NODES.get(logic_idname(node))
    if function is None:
        return preview
    evaluator = Evaluator(scene, depsgraph, owner)
    try:
        function(evaluator, preview)
    except (TypeError, ValueError, ZeroDivisionError, OverflowError, AttributeError,
            IndexError) as error:
        preview.lines.clear()
        preview.problem = "cannot be shown: %s" % error
    preview.deps = evaluator.deps
    preview.deps.add(tree.as_pointer())
    preview.uses_names = evaluator.uses_names
    return preview


# --------------------------------------------------------------------------------------------
# Which previews are shown.


def tree_users(scene):
    """{tree: [objects]}: the logic trees the scene's shown objects run (any binding, on or off:
    a tree switched off can be started later), in the scene's object order."""
    users = {}
    for obj in scene.objects:
        game = getattr(obj, "game", None)
        if game is None or not obj.visible_get():
            continue
        bindings = getattr(game, "logic_node_bindings", None)
        if not bindings:
            continue
        for binding in bindings:
            tree = binding.tree or bpy.data.node_groups.get(binding.tree_name)
            if tree is None or tree.bl_idname != "LogicNodeTree":
                continue
            owners = users.setdefault(tree, [])
            if obj not in owners:
                owners.append(obj)
    return users


def shown_trees(window_manager):
    """The logic trees open in a Logic Node Editor of any window."""
    trees = []
    for window in window_manager.windows:
        screen = window.screen
        if screen is None:
            continue
        for area in screen.areas:
            if area.type != 'NODE_EDITOR':
                continue
            space = area.spaces.active
            tree = getattr(space, "edit_tree", None) or getattr(space, "node_tree", None)
            if tree is not None and tree.bl_idname == "LogicNodeTree" and tree not in trees:
                trees.append(tree)
    return trees


def visualize_on(scene, node, owner):
    """The node's Visualize for an object running its tree: True, False, or None when only the
    game knows it (an input linked to a game property: shown). A node without one (Aim for Arc,
    Ray to Plane): False."""
    if _socket(node.inputs, "Visualize") is None:
        return False
    value = Evaluator(scene, None, owner).value(node, "Visualize", 'BOOL')
    return None if value is UNKNOWN else bool(value)


def collect_previews(scene, depsgraph, mode, open_trees=(), selected=None):
    """The previews to draw. Mode 'SELECTED': the ray nodes selected in an open tree
    (`open_trees`), for the selected objects running it (once with no object for a tree no object
    runs). 'VISUALIZED': those, and the ray nodes whose Visualize is on, for every object running
    their tree. `selected`: whether an object counts as selected (default: its selection)."""
    return [preview_node(scene, depsgraph, tree, node, owner)
            for tree, node, owner in wanted_previews(scene, mode, open_trees, selected)]


def wanted_previews(scene, mode, open_trees=(), selected=None):
    """collect_previews' (tree, node, object) triples, in drawing order."""
    if mode == 'OFF':
        return []
    is_selected = selected if selected is not None else (lambda obj: obj.select_get())
    users = tree_users(scene)
    wanted = []
    for tree, owners in users.items():
        nodes = [node for node in tree.nodes if is_ray_node(node)]
        if not nodes:
            continue
        open_tree = tree in open_trees
        chosen = [obj for obj in owners if is_selected(obj)] if open_tree else []
        for node in nodes:
            shown = set(chosen) if open_tree and node.select else set()
            if mode == 'VISUALIZED':
                visualize = _socket(node.inputs, "Visualize")
                if visualize is not None and _link_source(visualize) is not None:
                    # Linked: it may differ per object.
                    shown.update(obj for obj in owners
                                 if visualize_on(scene, node, obj) is not False)
                elif visualize_on(scene, node, None) is not False:
                    shown.update(owners)
            # (In the scene's object order.)
            wanted.extend((tree, node, owner) for owner in owners if owner in shown)
    for tree in open_trees:
        if tree in users:
            continue
        for node in tree.nodes:
            if is_ray_node(node) and (node.select or (
                    mode == 'VISUALIZED' and visualize_on(scene, node, None) is not False)):
                wanted.append((tree, node, None))
    return wanted


def objects_to_select(scene, open_trees=(), selected=None):
    """The open trees with a selected ray node none of whose objects is selected (its preview
    waits for one: the panel says so)."""
    is_selected = selected if selected is not None else (lambda obj: obj.select_get())
    waiting = []
    for tree, owners in tree_users(scene).items():
        if tree in open_trees and not any(is_selected(obj) for obj in owners) and \
                any(node.select and is_ray_node(node) for node in tree.nodes):
            waiting.append(tree)
    return waiting


# --------------------------------------------------------------------------------------------
# Drawing. Previews are kept per (tree, node, object) with the data-blocks they read: an update
# remakes only the previews whose data changed, and the batch is joined from their arrays.


class _Entry:
    """One kept preview: its lines as arrays, its name label, what it read."""

    __slots__ = ("deps", "uses_names", "positions", "colors", "label", "dynamic", "problem",
                 "node_name")


class _State:
    entries = {}
    keys = []
    batches = None
    labels = []
    stats = {"previews": 0, "dynamic": 0, "problems": [], "capped": False}
    dirty_ids = set()
    objects_changed = False
    scene_signature = None
    signature = None
    version = 0
    in_game = False
    draw_handles = []
    timer_signature = None
    # The trees drawn (names), their last fingerprints, and the objects' trees, for the timer.
    waiting = []
    tree_names = set()
    tree_fingerprints = {}
    users_signature = None
    ticks = 0


def _dashes(a, b):
    length = (b - a).length
    if length <= MT_EPSILON:
        return [(a, b)]
    dash = max(0.12, length / 64.0)
    count = max(1, int(length / dash))
    step = (b - a) / count
    return [(a + step * i, a + step * (i + 0.55)) for i in range(count)]


def _entry_of(preview):
    import numpy as np
    positions = []
    colors = []
    for a, b, color, dashed in preview.lines:
        for p, q in (_dashes(a, b) if dashed else ((a, b),)):
            positions.append(p[:])
            positions.append(q[:])
            colors.append(color)
            colors.append(color)
    entry = _Entry()
    entry.deps = preview.deps
    entry.uses_names = preview.uses_names
    entry.positions = np.array(positions, dtype=np.float32).reshape(-1, 3)
    entry.colors = np.array(colors, dtype=np.float32).reshape(-1, 4)
    entry.label = None
    if preview.anchor is not None:
        text = preview.node.label or preview.node.name
        if preview.owner is not None:
            text += "  (%s)" % preview.owner.name
        if preview.dynamic:
            text += "  [in game: %s]" % ", ".join(sorted(set(preview.dynamic)))
        entry.label = (preview.anchor.copy(), text)
    entry.dynamic = bool(preview.dynamic)
    entry.problem = preview.problem
    entry.node_name = preview.node.name
    return entry


def _build_batches(entries):
    """The two batches (full, and faded for what objects hide) of the entries' lines."""
    import gpu
    import numpy as np
    from gpu_extras.batch import batch_for_shader
    parts = [entry for entry in entries if len(entry.positions)]
    if not parts:
        return None, False
    positions = np.concatenate([entry.positions for entry in parts])
    colors = np.concatenate([entry.colors for entry in parts])
    capped = len(positions) > MAX_SEGMENTS * 2
    if capped:
        positions = positions[:MAX_SEGMENTS * 2]
        colors = colors[:MAX_SEGMENTS * 2]
    faded = colors.copy()
    faded[:, 3] *= 0.25
    shader = gpu.shader.from_builtin('POLYLINE_FLAT_COLOR')
    solid = batch_for_shader(shader, 'LINES', {"pos": positions, "color": colors})
    shadow = batch_for_shader(shader, 'LINES', {"pos": positions, "color": faded})
    return (shader, solid, shadow), capped


def _settings(scene):
    return getattr(scene, "logic_ray_preview", None) if scene is not None else None


def _scene_signature(scene):
    """What every preview may read of the scene: a change remakes them all."""
    render = scene.render
    camera = scene.camera
    return (scene.as_pointer(), round(float(scene.game_settings.physics_gravity), 6),
            camera.as_pointer() if camera is not None else 0, render.resolution_x,
            render.resolution_y, render.pixel_aspect_x, render.pixel_aspect_y)


def _selection_signature(context, mode):
    open_trees = shown_trees(context.window_manager)
    nodes = ()
    if mode != 'OFF':
        nodes = tuple(sorted((tree.as_pointer(), node.as_pointer()) for tree in open_trees
                             for node in tree.nodes if node.select and is_ray_node(node)))
    return tuple(tree.as_pointer() for tree in open_trees), nodes


def _forget(everything=False):
    if everything:
        _State.entries.clear()
    _State.keys = []
    _State.signature = None


def _refresh(context, settings):
    """Remake the previews whose data changed, and the batch when anything did."""
    scene = context.scene
    signature = _scene_signature(scene)
    if signature != _State.scene_signature:
        _State.entries.clear()
        _State.scene_signature = signature
    if _State.dirty_ids or _State.objects_changed:
        dirty = _State.dirty_ids
        names = _State.objects_changed
        for key in [key for key, entry in _State.entries.items()
                    if entry.deps & dirty or (names and entry.uses_names)]:
            del _State.entries[key]
        _State.dirty_ids = set()
        _State.objects_changed = False
    depsgraph = context.view_layer.depsgraph if context.view_layer is not None else None
    open_trees = shown_trees(context.window_manager)
    wanted = wanted_previews(scene, settings.mode, open_trees)
    _State.waiting = [tree.name for tree in objects_to_select(scene, open_trees)]
    _State.tree_names = {tree.name_full for tree, _node, _owner in wanted}
    keys = []
    made = False
    for tree, node, owner in wanted:
        key = (tree.as_pointer(), node.as_pointer(), owner.as_pointer() if owner else 0)
        keys.append(key)
        if key not in _State.entries:
            _State.entries[key] = _entry_of(preview_node(scene, depsgraph, tree, node, owner))
            made = True
    if len(_State.entries) > 2 * len(keys) + 256:
        wanted_keys = set(keys)
        for key in [key for key in _State.entries if key not in wanted_keys]:
            del _State.entries[key]
    if not made and keys == _State.keys:
        return
    _State.keys = keys
    entries = [_State.entries[key] for key in keys]
    _State.batches, capped = _build_batches(entries)
    _State.labels = [entry.label for entry in entries if entry.label is not None]
    _State.stats = {
        "previews": len(entries),
        "dynamic": sum(1 for entry in entries if entry.dynamic),
        "problems": [(entry.node_name, entry.problem) for entry in entries if entry.problem],
        "capped": capped,
    }


def _rebuild(context, settings):
    """Every preview made anew (tests and timings)."""
    _forget(everything=True)
    _refresh(context, settings)


def _overlays_shown(context):
    """The 3D view's Overlays button: off hides the previews as it hides Blender's gizmos."""
    space = context.space_data
    overlay = getattr(space, "overlay", None)
    return overlay is None or overlay.show_overlays


def _draw_view():
    if _State.in_game:
        return
    context = bpy.context
    settings = _settings(context.scene)
    if settings is None or settings.mode == 'OFF' or not _overlays_shown(context):
        return
    selected = getattr(context, "selected_objects", None) or ()
    signature = (context.scene.as_pointer(), settings.mode, _State.version,
                 _selection_signature(context, settings.mode),
                 tuple(sorted(obj.as_pointer() for obj in selected)))
    if signature != _State.signature:
        _refresh(context, settings)
        _State.signature = signature
    if _State.batches is None:
        return
    import gpu
    shader, solid, shadow = _State.batches
    viewport = gpu.state.viewport_get()
    width = 2.0 * context.preferences.system.ui_scale
    shader.bind()
    shader.uniform_float("viewportSize", (viewport[2], viewport[3]))
    shader.uniform_float("lineWidth", width)
    gpu.state.blend_set('ALPHA')
    gpu.state.depth_mask_set(False)
    # What objects hide, faded; then what is in front, full.
    gpu.state.depth_test_set('NONE')
    shadow.draw(shader)
    gpu.state.depth_test_set('LESS_EQUAL')
    solid.draw(shader)
    gpu.state.depth_test_set('NONE')
    gpu.state.blend_set('NONE')


def _draw_labels():
    if _State.in_game or not _State.labels:
        return
    context = bpy.context
    settings = _settings(context.scene)
    if settings is None or settings.mode == 'OFF' or not settings.show_names or \
            not _overlays_shown(context):
        return
    import blf
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    region = context.region
    region_3d = context.region_data
    if region is None or region_3d is None:
        return
    scale = context.preferences.system.ui_scale
    blf.size(0, 11.0 * scale)
    blf.color(0, 1.0, 1.0, 1.0, 0.9)
    blf.enable(0, blf.SHADOW)
    blf.shadow(0, 3, 0.0, 0.0, 0.0, 0.8)
    line_height = 14.0 * scale
    placed = []
    for position, text in _State.labels[:400]:
        point = location_3d_to_region_2d(region, region_3d, position)
        if point is None:
            continue
        x = point.x + 6.0 * scale
        y = point.y + 6.0 * scale
        # Names of previews starting at one point go one under the other.
        while any(abs(x - px) < 40.0 * scale and abs(y - py) < line_height for px, py in placed):
            y -= line_height
        placed.append((x, y))
        blf.position(0, x, y, 0.0)
        blf.draw(0, text)
    blf.disable(0, blf.SHADOW)


def _redraw_3d_views():
    window_manager = getattr(bpy.context, "window_manager", None)
    if window_manager is None:
        return
    for window in window_manager.windows:
        screen = window.screen
        if screen is None:
            continue
        for area in screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()


def _changed(_self=None, _context=None):
    _State.version += 1
    _redraw_3d_views()


def _note_updates(depsgraph):
    """The data-blocks an update changed; True when a node tree is among them."""
    tree_changed = False
    for update in depsgraph.updates:
        data = update.id
        original = getattr(data, "original", None) or data
        _State.dirty_ids.add(original.as_pointer())
        if isinstance(original, bpy.types.Object):
            _State.objects_changed = True
            if original.data is not None:
                _State.dirty_ids.add(original.data.as_pointer())
        elif isinstance(original, bpy.types.NodeTree):
            tree_changed = True
    _State.version += 1
    return tree_changed


@persistent
def _on_depsgraph_update(_scene, depsgraph):
    if _note_updates(depsgraph):
        _redraw_3d_views()


@persistent
def _on_frame_change(_scene, depsgraph):
    # Animated objects are moved without an update of the graph's own.
    _forget(everything=True)
    _State.version += 1


@persistent
def _on_file_change(*_args):
    _forget(everything=True)
    _State.scene_signature = None
    _State.batches = None
    _State.labels = []
    _State.dirty_ids = set()
    _State.tree_fingerprints = {}
    _State.tree_names = set()
    _State.users_signature = None
    _State.version += 1


@persistent
def _on_game_pre(*_args):
    _State.in_game = True
    if _on_depsgraph_update in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(_on_depsgraph_update)


@persistent
def _on_game_post(*_args):
    _State.in_game = False
    if _on_depsgraph_update not in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.append(_on_depsgraph_update)
    # The game moved objects without telling the previews.
    _forget(everything=True)
    _changed()


_SETTING_NAMES = {}


def _setting_names(node):
    """A node type's own settings (its dropdowns and values beside the sockets)."""
    names = _SETTING_NAMES.get(node.bl_idname)
    if names is None:
        base = set(bpy.types.Node.bl_rna.properties.keys())
        names = [prop.identifier for prop in node.bl_rna.properties
                 if prop.identifier not in base and prop.type in ('ENUM', 'BOOLEAN', 'INT',
                                                                  'FLOAT', 'STRING', 'POINTER')]
        _SETTING_NAMES[node.bl_idname] = names
    return names


def _frozen(value):
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, bpy.types.ID):
        return value.as_pointer()
    try:
        return tuple(value)
    except TypeError:
        return repr(value)


def tree_fingerprint(tree):
    """What the previews read of a tree: its ray nodes and the nodes feeding them (their
    settings, the values of their unlinked inputs, their outputs' own values, where their links
    come from). Logic trees are no part of Blender's update graph: editing them sends no update,
    so the timer compares this."""
    parts = [len(tree.nodes), len(tree.links)]
    seen = set()
    stack = [node for node in tree.nodes if is_ray_node(node)]
    while stack:
        node = stack.pop()
        pointer = node.as_pointer()
        if pointer in seen:
            continue
        seen.add(pointer)
        parts.append(pointer)
        parts.extend(_frozen(getattr(node, name, None)) for name in _setting_names(node))
        for socket in node.inputs:
            source = _link_source(socket)
            if source is None:
                parts.append(_frozen(getattr(socket, "default_value", None)))
            else:
                parts.append((source[0].as_pointer(), source[1].identifier))
                stack.append(source[0])
        if not node.inputs:
            parts.extend(_frozen(getattr(socket, "default_value", None)) for socket in node.outputs)
    return hash(tuple(parts))


def _users_signature(scene):
    return tuple((obj.as_pointer(), tuple(binding.tree_name for binding in
                                          obj.game.logic_node_bindings))
                 for obj in scene.objects
                 if getattr(obj, "game", None) is not None and obj.game.logic_node_bindings)


def _watch():
    """Four times a second: the changes no update of Blender's reports. The selected ray nodes,
    an edited tree (its fingerprint: a Visualize switched too), the scene's gravity, camera and resolution, and
    (every 2 seconds) which trees the objects run: redraw, remaking what changed."""
    try:
        context = bpy.context
        if _State.in_game or context.window_manager is None:
            return 0.25
        _State.ticks += 1
        changed = False
        for window in context.window_manager.windows:
            scene = window.scene
            settings = _settings(scene)
            if settings is None or settings.mode == 'OFF':
                continue
            signature = _selection_signature(context, settings.mode)
            if signature != _State.timer_signature:
                _State.timer_signature = signature
                changed = True
            if _State.scene_signature is not None and \
                    _scene_signature(scene) != _State.scene_signature:
                changed = True
            for name in list(_State.tree_names) + [tree.name_full for tree in
                                                   shown_trees(context.window_manager)]:
                tree = bpy.data.node_groups.get(name)
                if tree is None or tree.bl_idname != "LogicNodeTree":
                    continue
                fingerprint = tree_fingerprint(tree)
                known = _State.tree_fingerprints.get(name)
                _State.tree_fingerprints[name] = fingerprint
                if known is not None and known != fingerprint:
                    _State.dirty_ids.add(tree.as_pointer())
                    changed = True
            if _State.ticks % 8 == 0:
                users = _users_signature(scene)
                if _State.users_signature is not None and users != _State.users_signature:
                    changed = True
                _State.users_signature = users
            break
        if changed:
            _State.version += 1
            _redraw_3d_views()
    except (ReferenceError, AttributeError, RuntimeError):
        pass
    return 0.25


# --------------------------------------------------------------------------------------------
# Settings and panels.


class LogicRayPreviewSettings(PropertyGroup):
    mode: EnumProperty(
        name="Ray Previews",
        description="Draw the ray nodes' rays, casts, throws and view cones in the 3D viewport "
                    "before the game runs (not while the viewport's overlays are off)",
        items=(
            ('OFF', "Off", "Draw no ray previews in the viewport", 0),
            ('SELECTED', "Selected",
             "The ray nodes selected in an open Logic Node Editor, on the selected objects "
             "running their tree", 1),
            ('VISUALIZED', "Visualized",
             "Also the ray nodes whose Visualize is on, for every object running their tree, as "
             "the game draws them", 3),
        ),
        default='SELECTED',
        update=_changed,
    )
    show_names: BoolProperty(
        name="Names",
        description="Write each preview's node name (and its object) next to it, and which inputs "
                    "only the game knows",
        default=False,
        update=_changed,
    )


def _draw_settings(layout, context):
    settings = _settings(context.scene)
    if settings is None:
        return
    row = layout.row(align=True)
    row.prop(settings, "mode", expand=True)
    layout.prop(settings, "show_names")
    if settings.mode == 'OFF':
        return
    stats = _State.stats
    col = layout.column(align=True)
    col.label(text="The ray nodes selected in the editor,")
    col.label(text="on the selected objects running them")
    if settings.mode == 'VISUALIZED':
        col.label(text="and those with Visualize on, everywhere")
    for name in _State.waiting[:3]:
        col.label(text="Select an object running \"%s\"" % name, icon='RESTRICT_SELECT_OFF')
    col.separator()
    col.label(text="Shown: {:d}".format(stats["previews"]), icon='INFO')
    if stats["dynamic"]:
        col.label(text="{:d} known only in the game (grey dashed)".format(stats["dynamic"]))
    for node_name, problem in stats["problems"][:4]:
        col.label(text="{:s}: {:s}".format(node_name, problem), icon='ERROR')
    if stats["capped"]:
        col.label(text="Too many lines: some are left out", icon='ERROR')
    col.label(text="Hits are shown only in the game", icon='BLANK1')


class NODE_PT_logic_nodes_ray_preview(Panel):
    bl_label = "Ray Previews"
    bl_space_type = 'NODE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Dashboard"

    @classmethod
    def poll(cls, context):
        space = context.space_data
        tree = getattr(space, "edit_tree", None) or getattr(space, "node_tree", None)
        return tree is not None and tree.bl_idname == "LogicNodeTree"

    def draw(self, context):
        _draw_settings(self.layout, context)


class VIEW3D_PT_logic_nodes_ray_preview(Panel):
    bl_label = "Logic Ray Previews"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "View"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return any(tree.bl_idname == "LogicNodeTree" for tree in bpy.data.node_groups)

    def draw(self, context):
        _draw_settings(self.layout, context)


classes = (
    LogicRayPreviewSettings,
    NODE_PT_logic_nodes_ray_preview,
    VIEW3D_PT_logic_nodes_ray_preview,
)


def _is_player():
    return os.path.basename(bpy.app.binary_path or "").lower().startswith("blenderplayer")


_HANDLERS = (
    ("depsgraph_update_post", _on_depsgraph_update),
    ("frame_change_post", _on_frame_change),
    ("load_post", _on_file_change),
    ("undo_post", _on_file_change),
    ("redo_post", _on_file_change),
    ("game_pre", _on_game_pre),
    ("game_post", _on_game_post),
)


def register():
    if _is_player() or not hasattr(bpy.types, "LogicNodeTree"):
        return
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.logic_ray_preview = PointerProperty(type=LogicRayPreviewSettings)
    for name, function in _HANDLERS:
        handlers = getattr(bpy.app.handlers, name, None)
        if handlers is not None and function not in handlers:
            handlers.append(function)
    _State.draw_handles = [
        bpy.types.SpaceView3D.draw_handler_add(_draw_view, (), 'WINDOW', 'POST_VIEW'),
        bpy.types.SpaceView3D.draw_handler_add(_draw_labels, (), 'WINDOW', 'POST_PIXEL'),
    ]
    if not bpy.app.timers.is_registered(_watch):
        bpy.app.timers.register(_watch, first_interval=0.5, persistent=True)


def unregister():
    if not _State.draw_handles:
        return
    if bpy.app.timers.is_registered(_watch):
        bpy.app.timers.unregister(_watch)
    for handle in _State.draw_handles:
        bpy.types.SpaceView3D.draw_handler_remove(handle, 'WINDOW')
    _State.draw_handles = []
    for name, function in _HANDLERS:
        handlers = getattr(bpy.app.handlers, name, None)
        if handlers is not None and function in handlers:
            handlers.remove(function)
    del bpy.types.Scene.logic_ray_preview
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
