# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Add > Game Navigation Examples (2026-11-20, the navigation ease-of-use plan's item 14): ready NPC behaviours
and level pieces, built into the open scene.

An NPC template makes a Logic Node tree (titled frames, the nodes' own names) and binds it to
the selected objects, or to a sample NPC it makes at the 3D cursor when none is selected. An
object template (Ladder, Door, Lift) makes its objects at the 3D cursor, their Physics >
Navigation set. Nothing else of the scene changes: its Navigation Mesh settings, the selected
objects' physics and their other trees stay as they are.
"""

import math

import bpy
from mathutils import Matrix, Vector

# identifier, menu text, icon, description, asks for a target, makes objects (no NPC)
TEMPLATES = (
    ('PATROL', "Patrol Guard", 'TRACKING',
     "Walks round four patrol points again and again (Follow Path); move the points where "
     "it should go", False, False),
    ('CHASER', "Chaser", 'FORWARD',
     "Runs after a target; several chasers surround it, each at its own place round it "
     "(Navigate Pursue, Avoidance Polite)", True, False),
    ('HORDE', "Horde Member", 'COMMUNITY',
     "Presses on a target, pushing through the others: zombies, a mob at a barricade "
     "(Navigate Pursue, Avoidance Dense)", True, False),
    ('WANDER', "Wandering Animal", 'GHOST_ENABLED',
     "Walks to random places round where it started (Navigate Wander)", False, False),
    ('COMPANION', "Companion", 'USER',
     "Follows a target, stopping 2.5 m from it, running to catch up when left behind "
     "(Navigate Go To)", True, False),
    ('FLYER', "Flying Creature", 'ORIENTATION_GIMBAL',
     "Flies to random places in the air round where it started, round walls and over roofs "
     "(Navigate Wander, Movement Fly); with a deck in reach it can land on ships too", False,
     False),
    ('COVER', "Cover Soldier", 'MOD_MASK',
     "Every second goes to the nearest place hidden from an enemy, with cover close by "
     "(Navmesh Query: Find Spot, then Navigate)", True, False),
    ('LADDER', "Ladder", 'SORTSIZE',
     "A ladder NPCs climb, its foot and top ends found on the floors (Physics > Navigation > "
     "Link, kind Ladder)", False, True),
    ('DOOR', "Door", 'HIDE_OFF',
     "A door NPCs wait at while it is closed (Link, kind Door, When Closed Wait); its 'open' "
     "property opens it (O does in the sample)", False, True),
    ('LIFT', "Lift", 'TRIA_UP',
     "A lift cab going up and down with stops; NPCs ride it between the floors (Physics > "
     "Navigation > Vehicle, a link at each floor)", False, True),
)
TEMPLATE_IDS = tuple(t[0] for t in TEMPLATES)
NEEDS_TARGET = {t[0] for t in TEMPLATES if t[4]}
MAKES_OBJECTS = {t[0] for t in TEMPLATES if t[5]}
# What a template made carries (tests and later tools find its parts by it).
MARK = "navigation_template"


class TemplateError(Exception):
    pass


# ---------------------------------------------------------------------------------------------
# A Logic Node tree built section by section (each a titled frame), laid out left to right.

_OPTIONS = None


def _new_node(nodes, idname):
    """A node by idname; a former node's idname (an option of a merged node) gives the merged
    node with that option."""
    global _OPTIONS
    if _OPTIONS is None:
        _OPTIONS = {}
        scratch = bpy.data.node_groups.new("__logic_options", "LogicNodeTree")
        try:
            for name in dir(bpy.types):
                rna = getattr(getattr(bpy.types, name, None), "bl_rna", None)
                if not name.startswith("LogicNative") or rna is None or \
                        "variant_idname" not in rna.properties:
                    continue
                node = scratch.nodes.new(name)
                for item in rna.properties["variant"].enum_items:
                    node.variant = item.identifier
                    _OPTIONS[node.variant_idname] = (name, item.identifier)
        finally:
            bpy.data.node_groups.remove(scratch)
    option = _OPTIONS.get(idname)
    if option is None:
        return nodes.new(idname)
    node = nodes.new(option[0])
    node.variant = option[1]
    return node


def _socket(sockets, identifier, node):
    for s in sockets:
        if s.identifier == identifier:
            return s
    raise TemplateError("%s has no socket %r" % (node.bl_idname, identifier))


class _Tree:
    def __init__(self, name):
        self.tree = bpy.data.node_groups.new(name, "LogicNodeTree")
        self.tree[MARK] = name
        self.sections = []
        self.links = []

    def section(self, title):
        self.sections.append((title, []))

    def node(self, idname, inputs=None, **props):
        node = _new_node(self.tree.nodes, idname)
        for key, value in props.items():
            setattr(node, key, value)
        for key, value in (inputs or {}).items():
            if isinstance(value, bpy.types.NodeSocket):
                self.link(value, node, key)
            else:
                self.set(node, key, value)
        self.sections[-1][1].append(node)
        return node

    def set(self, node, identifier, value):
        s = _socket(node.inputs, identifier, node)
        if not hasattr(s, "default_value"):
            # (A value socket holding no value of its own, Print's Message: a text node.)
            text = _new_node(self.tree.nodes, "LogicNativeValueString")
            text.outputs[0].default_value = str(value)
            self.sections[-1][1].append(text)
            self.link(text.outputs[0], node, identifier)
            return
        s.default_value = tuple(value) if isinstance(value, (tuple, list, Vector)) else value

    def out(self, node, identifier):
        return _socket(node.outputs, identifier, node)

    def link(self, output, node, identifier):
        self.tree.links.new(output, _socket(node.inputs, identifier, node))
        self.links.append((output.node, node))

    def step(self, interval=1, spread=False):
        n = self.node("LogicNativeOnFixedUpdate", step_interval=interval, use_step_spread=spread)
        return self.out(n, "Out")

    def position(self, obj=None):
        n = self.node("LogicNativeGetObjectAttribute", attribute_type='WORLD_POSITION')
        if obj is not None:
            self.set(n, "Object", obj)
        return self.out(n, "Vector")

    def finish(self):
        """Frames per section; nodes in columns by their links."""
        nodes = [n for _, ns in self.sections for n in ns]
        before = {n: set() for n in nodes}
        for a, b in self.links:
            if a in before and b in before and a is not b:
                before[b].add(a)
        depth = {}

        def depth_of(n, seen=()):
            if n not in depth:
                depth[n] = 0 if n in seen else max(
                    (depth_of(p, seen + (n,)) + 1 for p in before[n]), default=0)
            return depth[n]

        def height(n):
            shown = len([s for s in n.inputs if s.enabled and not s.hide]) + \
                len([s for s in n.outputs if s.enabled and not s.hide])
            return 90 + 23 * shown + (110 if n.bl_idname == "LogicNativeNavigate" else 0)

        y_top = 0.0
        for title, ns in self.sections:
            if not ns:
                continue
            frame = self.tree.nodes.new("NodeFrame")
            frame.label = title
            frame.label_size = 20
            frame.shrink = True
            base = min(depth_of(n) for n in ns)
            columns = {}
            for n in ns:
                columns.setdefault(depth_of(n) - base, []).append(n)
            tallest = 0.0
            for column, members in columns.items():
                y = y_top - 50.0
                for n in members:
                    n.location = (column * 290.0, y)
                    y -= height(n) + 25.0
                tallest = max(tallest, y_top - y)
            for n in ns:
                n.parent = frame
            y_top -= tallest + 140.0
        return self.tree

    def bind(self, objects):
        for obj in objects:
            binding = obj.game.logic_node_binding_new()
            binding.tree = self.tree
            binding.tree_name = self.tree.name
            binding.enabled = True


# ---------------------------------------------------------------------------------------------
# Objects

def _material(name, rgb):
    mat = bpy.data.materials.get(name)
    if mat is None:
        mat = bpy.data.materials.new(name)
        mat.diffuse_color = (*rgb, 1.0)
    return mat


def _mesh(name, verts, faces, rgb):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    mesh.materials.append(_material(name, rgb))
    return mesh


def _box(name, lo, hi, rgb):
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    verts = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
             (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    faces = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    return _mesh(name, verts, faces, rgb)


def _pawn(name, rgb, width=0.5, height=1.8):
    """An eight-sided body standing on its origin, a nose toward +Y (the way it walks)."""
    r = width * 0.5
    verts, faces = [], []
    for z in (0.0, height):
        for k in range(8):
            a = 2.0 * math.pi * k / 8.0
            verts.append((r * math.cos(a), r * math.sin(a), z))
    faces.append(tuple(range(7, -1, -1)))
    faces.append(tuple(range(8, 16)))
    for k in range(8):
        n = (k + 1) % 8
        faces.append((k, n, n + 8, k + 8))
    base = len(verts)
    nose = ((-0.08, r - 0.02, height * 0.78), (0.08, r - 0.02, height * 0.78),
            (0.0, r + 0.18, height * 0.82), (0.0, r - 0.02, height * 0.9))
    verts.extend(nose)
    faces.extend([(base, base + 2, base + 1), (base, base + 3, base + 2),
                  (base + 1, base + 2, base + 3), (base, base + 1, base + 3)])
    return _mesh(name, verts, faces, rgb)


def _bird(name, rgb, span=0.8):
    """Flat wings and a beak toward +Y, its middle on its origin."""
    s = span * 0.5
    verts = [(-s, -0.1, 0.0), (0.0, 0.3, 0.05), (s, -0.1, 0.0), (0.0, -0.3, 0.0),
             (0.0, 0.0, -0.08)]
    faces = [(0, 3, 1), (1, 3, 2), (0, 1, 4), (1, 2, 4), (2, 3, 4), (3, 0, 4)]
    return _mesh(name, verts, faces, rgb)


def _object(context, name, data, location, physics='NO_COLLISION', parent=None):
    ob = bpy.data.objects.new(name, data)
    collection = context.collection or context.scene.collection
    collection.objects.link(ob)
    ob.location = location
    if parent is not None:
        # (Kept where it is: the parent's own transform, its matrix_world is not evaluated yet
        # for an object made a moment ago; the template's parents have no parent.)
        ob.parent = parent
        ob.matrix_parent_inverse = parent.matrix_basis.inverted()
    if data is not None:
        ob.game.physics_type = physics
    ob[MARK] = name
    return ob


def _empty(context, name, location, parent=None, display='CIRCLE', size=0.4):
    ob = _object(context, name, None, location, parent=parent)
    ob.empty_display_type = display
    ob.empty_display_size = size
    return ob


def _game_property(context, ob, name, kind, value):
    layer = context.view_layer
    active = layer.objects.active
    layer.objects.active = ob
    try:
        with context.temp_override(object=ob, active_object=ob):
            bpy.ops.object.game_property_new(type=kind, name=name)
    finally:
        layer.objects.active = active
    prop = ob.game.properties[name]
    prop.value = value
    return prop


def _link(start, end, kind, both_ways=True, open_at_start=True, when_closed=None,
          duration=None, reach=None):
    g = start.game
    g.nav_link_end = end
    g.nav_link_kind = kind
    g.use_nav_link_both_ways = both_ways
    g.use_nav_link_open = open_at_start
    if when_closed is not None:
        g.nav_link_when_closed = when_closed
    if duration is not None:
        g.nav_link_duration = duration
    if reach is not None:
        g.nav_link_reach = reach


# ---------------------------------------------------------------------------------------------
# The templates

def _npcs(context, label, rgb, flyer=False):
    """The selected objects that can run a tree (meshes, Empties, armatures...), else a sample
    NPC made at the 3D cursor."""
    chosen = [ob for ob in context.selected_objects if ob.type not in {'CAMERA', 'LIGHT'}]
    if chosen:
        return chosen, []
    cursor = context.scene.cursor.location.copy()
    if flyer:
        sample = _object(context, label, _bird(label, rgb), cursor + Vector((0.0, 0.0, 3.0)))
    else:
        sample = _object(context, label, _pawn(label, rgb), cursor, physics='CHARACTER')
    return [sample], [sample]


def _navigate(t, mode, **inputs):
    values = {"Lin Speed": 3.0, "Reach Threshold": 1.0, "Rot Speed": 8.0}
    values.update(inputs)
    return t.node("LogicNativeNavigate", mode=mode, inputs=values)


def _patrol(context, target):
    npcs, made = _npcs(context, "Guard", (0.85, 0.25, 0.2))
    # (A sample made just now has no evaluated matrix yet.)
    home = (npcs[0].location if made else npcs[0].matrix_world.translation).copy()
    points = []
    for i, (dx, dy) in enumerate(((4.0, 0.0), (4.0, 6.0), (-4.0, 6.0), (-4.0, 0.0))):
        p = _empty(context, "Patrol Point %d" % (i + 1), home + Vector((dx, dy, 0.0)),
                   display='SINGLE_ARROW', size=0.6)
        points.append(p)
    made.extend(points)
    t = _Tree("Patrol Guard")
    t.section("Patrol: round the points, again and again (Follow Path, Loop; move the points)")
    route = t.node("LogicNativeListFromItems", mode='VECTOR')
    for p in reversed(points):
        t.link(t.position(p), route, "Vectors")
    follow = t.node("LogicNativeFollowPath", way='NAVMESH',
                    inputs={"Loop": True, "Continue": True, "Lin Speed": 1.8,
                            "Reach Threshold": 0.6, "Rot Speed": 6.0})
    t.link(t.out(route, "Vectors"), follow, "Path Points")
    t.link(t.step(), follow, "Flow")
    t.section("Each point reached: what the guard does there goes here")
    say = t.node("LogicNativePrint", inputs={"Message": "patrol point reached"})
    t.link(t.out(follow, "Point Reached"), say, "Flow")
    t.finish()
    t.bind(npcs)
    return t.tree, made


def _pursuer(context, target, dense):
    label = "Zombie" if dense else "Chaser"
    npcs, made = _npcs(context, label, (0.45, 0.65, 0.3) if dense else (0.9, 0.45, 0.15))
    t = _Tree("Horde Member" if dense else "Chaser")
    if dense:
        t.section("Press on the target, pushing through the others (Pursue, Avoidance 2 Dense)")
    else:
        t.section("After the target; several surround it, each at its own place round it "
                  "(Pursue, Avoidance 1 Polite)")
    nav = _navigate(t, 'PURSUE', **{"Target Object": target,
                                    "Lin Speed": 3.0 if dense else 4.0,
                                    "Reach Threshold": 1.0 if dense else 1.2,
                                    "Avoidance": 2 if dense else 1})
    t.link(t.step(), nav, "Flow")
    t.section("Reached it: the attack goes here")
    say = t.node("LogicNativePrint", inputs={"Message": "reached the target"})
    t.link(t.out(nav, "When Reached"), say, "Flow")
    t.finish()
    t.bind(npcs)
    return t.tree, made


def _wanderer(context, target, flyer):
    npcs, made = _npcs(context, "Bird" if flyer else "Animal",
                       (0.95, 0.95, 0.9) if flyer else (0.6, 0.45, 0.3), flyer=flyer)
    t = _Tree("Flying Creature" if flyer else "Wandering Animal")
    t.section("Home: where it is when the game starts (a tree property)")
    keep = t.node("LogicNativeSetTreeProperty", inputs={"Property": "home"})
    t.link(t.out(t.node("LogicNativeOnInit"), "Out"), keep, "Flow")
    t.link(t.position(), keep, "Value")
    if flyer:
        t.section("Fly about: random places in the air within 10 m of home (Wander, Fly)")
    else:
        t.section("Wander: random places within 8 m of home, at a stroll (Wander)")
    home = t.node("LogicNativeGetTreeProperty", inputs={"Property": "home"})
    nav = _navigate(t, 'WANDER', **{"Radius": 10.0 if flyer else 8.0,
                                    "Lin Speed": 3.0 if flyer else 1.2,
                                    "Reach Threshold": 0.8})
    if flyer:
        nav.movement = 'FLY'
    t.link(t.out(home, "Value"), nav, "Destination")
    t.link(t.step(), nav, "Flow")
    t.finish()
    t.bind(npcs)
    return t.tree, made


def _companion(context, target):
    npcs, made = _npcs(context, "Companion", (0.3, 0.6, 0.95))
    t = _Tree("Companion")
    t.section("Catch up: the farther behind, the faster (3 m: a walk, 12 m: a run)")
    me = t.out(t.node("LogicNativeGetOwner"), "Owner Object")
    gap = t.node("LogicNativeGetDistance", inputs={"Object A": me, "Object B": target})
    pace = t.node("LogicNativeMapRange", use_clamp=True,
                  inputs={"From Min": 3.0, "From Max": 12.0, "To Min": 2.5, "To Max": 7.0})
    t.link(t.out(gap, "Distance"), pace, "Value")
    t.section("Follow the target, stopping 2.5 m from it (Go To its Target Object)")
    nav = _navigate(t, 'GO_TO', **{"Target Object": target, "Reach Threshold": 2.5})
    t.link(t.out(pace, "Result"), nav, "Lin Speed")
    t.link(t.step(), nav, "Flow")
    t.finish()
    t.bind(npcs)
    return t.tree, made


def _cover(context, target):
    npcs, made = _npcs(context, "Soldier", (0.35, 0.45, 0.25))
    t = _Tree("Cover Soldier")
    t.section("Every second: the best place within 12 m hidden from the enemy, a wall close "
              "toward it (Navmesh Query: Find Spot)")
    here = t.position()
    spot = t.node("LogicNativeNavmeshQuery", query='FIND_SPOT', spot_visibility='HIDDEN',
                  inputs={"Radius": 12.0, "Look Object": target, "Cover": True})
    t.link(here, spot, "Position")
    t.link(here, spot, "From")
    found = t.node("LogicNativeBranch")
    t.link(t.step(interval=60, spread=True), found, "Flow")
    t.link(t.out(spot, "Found"), found, "Condition")
    t.section("Run there and stay (Navigate Go To, Until Arrived)")
    nav = _navigate(t, 'GO_TO', **{"Lin Speed": 4.0, "Reach Threshold": 0.4})
    nav.use_until_arrived = True
    t.link(t.out(spot, "Point"), nav, "Destination")
    t.link(t.out(found, "True"), nav, "Flow")
    t.finish()
    t.bind(npcs)
    return t.tree, made


def _ladder(context, height):
    cursor = context.scene.cursor.location.copy()
    rails = []
    for x in (-0.28, 0.28):
        rails.append(((x - 0.03, -0.03, 0.0), (x + 0.03, 0.03, height)))
    rungs = [((-0.28, -0.02, z - 0.02), (0.28, 0.02, z + 0.02))
             for z in [0.3 * (i + 1) for i in range(int(height / 0.3) - 1)]]
    verts, faces = [], []
    for lo, hi in rails + rungs:
        box = _box("_", lo, hi, (0.5, 0.35, 0.2))
        base = len(verts)
        verts.extend(tuple(v.co) for v in box.vertices)
        faces.extend(tuple(base + i for i in p.vertices) for p in box.polygons)
        bpy.data.meshes.remove(box)
    ladder = _object(context, "Ladder", _mesh("Ladder", verts, faces, (0.5, 0.35, 0.2)), cursor)
    # (Its foot in front of it on the floor, its top beyond it on the floor above: each end
    # finds its floor by itself, Physics > Navigation > Link.)
    foot = _empty(context, "Ladder Foot", cursor + Vector((0.0, -0.6, 0.0)), parent=ladder)
    top = _empty(context, "Ladder Top", cursor + Vector((0.0, 0.6, height)), parent=ladder)
    _link(foot, top, 'LADDER', duration=max(1.0, height * 0.6))
    return None, [ladder, foot, top]


def _door(context, height):
    cursor = context.scene.cursor.location.copy()
    leaf = _object(context, "Door", _box("Door", (-0.6, -0.05, 0.0), (0.6, 0.05, 2.2),
                                         (0.55, 0.32, 0.15)), cursor, physics='STATIC')
    front = _empty(context, "Door Link", cursor + Vector((0.0, -0.9, 0.0)), parent=leaf)
    back = _empty(context, "Door Link End", cursor + Vector((0.0, 0.9, 0.0)), parent=leaf)
    _link(front, back, 'DOOR', open_at_start=False, when_closed='WAIT', duration=1.0)
    _game_property(context, leaf, "open", 'BOOL', False)
    t = _Tree("Door")
    t.section("'open' changed: the door's link opens or closes, the leaf hides or shows")
    get = t.node("LogicNativeGetGamePropertyValue", variant='BOOLEAN', inputs={"Property": "open"})
    changed = t.node("LogicNativeOnValueChange")
    t.link(t.out(get, "Value"), changed, "Value")
    which = t.node("LogicNativeBranch")
    t.link(t.out(changed, "If Changed"), which, "Flow")
    t.link(t.out(get, "Value"), which, "Condition")
    for flow, state in (("True", True), ("False", False)):
        link = t.node("LogicNativeSetNavigationAttribute", attribute='LINK_ENABLED',
                      inputs={"Object": front, "Enabled": state})
        t.link(t.out(which, flow), link, "Flow")
        show = t.node("LogicNativeSetObjectAttribute", attribute_type='VISIBLE',
                      inputs={"Visible": not state})
        t.link(t.out(link, "Done"), show, "Flow")
    t.section("Sample: O opens and closes it (set 'open' from a switch, a key, a keeper)")
    key = t.node("LogicNativeKeyboardKey", input_type='TAP', inputs={"Key": "O"})
    flip = t.node("LogicNativeModifyGameProperty", variant='TOGGLE', inputs={"Property": "open"})
    t.link(t.out(key, "Out"), flip, "Flow")
    t.finish()
    t.bind([leaf])
    return t.tree, [leaf, front, back]


def _lift(context, height):
    cursor = context.scene.cursor.location.copy()
    # (Its floor's top at the cursor; the box round its origin, as Box bounds are.)
    cursor = cursor - Vector((0.0, 0.0, 0.125))
    cab = _object(context, "Lift Cab", _box("Lift Cab", (-1.3, -1.3, -0.125), (1.3, 1.3, 0.125),
                                            (0.85, 0.7, 0.2)), cursor, physics='KINEMATIC')
    cab.game.use_nav_vehicle = True
    cab.game.use_collision_bounds = True
    cab.game.collision_bounds_type = 'BOX'
    floor = cursor + Vector((0.0, 0.0, 0.125))
    made = [cab]
    # A link at each floor: from beside the shaft (west of the cab) onto the cab's floor (a
    # child of the cab), open while the cab stands there (the link's Reach).
    end = _empty(context, "Lift Cab Door", floor + Vector((-0.9, 0.0, 0.0)), parent=cab)
    made.append(end)
    for name, z in (("Lift Bottom Door", 0.0), ("Lift Top Door", height)):
        start = _empty(context, name, floor + Vector((-2.2, 0.0, z)))
        _link(start, end, 'DOOR', reach=1.5)
        made.append(start)
    # Up and down with stops (an action played in a loop): 4 s at each floor, 4 s a ride.
    cab.animation_data_create()
    keys = ((1, 0.0), (240, 0.0), (480, height), (720, height), (960, 0.0), (961, 0.0))
    for frame, z in keys:
        cab.location = cursor + Vector((0.0, 0.0, z))
        cab.keyframe_insert("location", frame=frame)
    cab.location = cursor
    action = cab.animation_data.action
    action.name = "Lift Ride"
    t = _Tree("Lift")
    t.section("Up and down with stops: 4 s at each floor, 4 s a ride (Play Action, Loop)")
    play = t.node("LogicNativePlayAction", play_mode='LOOP',
                  inputs={"Start Frame": 1.0, "End Frame": float(keys[-1][0]),
                          "Action Name": action.name})
    t.link(t.out(t.node("LogicNativeOnInit"), "Out"), play, "Flow")
    t.finish()
    t.bind([cab])
    return t.tree, made


_BUILDERS = {
    'PATROL': lambda c, target, h: _patrol(c, target),
    'CHASER': lambda c, target, h: _pursuer(c, target, False),
    'HORDE': lambda c, target, h: _pursuer(c, target, True),
    'WANDER': lambda c, target, h: _wanderer(c, target, False),
    'COMPANION': lambda c, target, h: _companion(c, target),
    'FLYER': lambda c, target, h: _wanderer(c, target, True),
    'COVER': lambda c, target, h: _cover(c, target),
    'LADDER': lambda c, target, h: _ladder(c, h),
    'DOOR': lambda c, target, h: _door(c, h),
    'LIFT': lambda c, target, h: _lift(c, h),
}


def scene_notes(scene):
    """What the scene's settings mean for the templates (said, never changed)."""
    notes = []
    gs = getattr(scene, "game_settings", None)
    rd = getattr(gs, "recast_data", None) if gs is not None else None
    if rd is not None and rd.build_mode == 'OFF':
        notes.append("Scene > Navigation Mesh > Build is Off: NPCs walk straight at their goals "
                     "(set it to Auto to walk round walls)")
    return notes


def add(context, template, *, target=None, height=4.0):
    """Builds `template` into the scene: returns (its tree or None, the objects it made)."""
    if template not in _BUILDERS:
        raise TemplateError("unknown navigation template %r" % template)
    if template in NEEDS_TARGET and target is None:
        raise TemplateError("this template needs a target object")
    tree, made = _BUILDERS[template](context, target, max(float(height), 1.0))
    for ob in made:
        ob.select_set(True)
    return tree, made
