# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Physics > Navigation (2026-11-20): every navigation setting of an object in one place.

The real panels draw with a layout that records every property and label (no window needed):

- the panel shows for every physics type Create Obstacle showed for, and for a Vehicle chassis
  (whose Obstacle row is hidden, as the old panel was);
- a Static vehicle is warned about (Characters ride only Kinematic or Dynamic vehicles);
- the Link sub-panel: To always; the other fields greyed out while To is empty; Kind's name only
  for Custom; Give Up After only for Wait;
- every RNA default equals the engine's (KX_NavObjectSettings) and a new object's value;
- the link's end survives what an object pointer must: a duplicate keeps it, deleting the end
  object empties it, an object cannot be its own end;
- the old Create Obstacle panel is gone.

    blender --background --factory-startup --python tests/python/game_physics_navigation_panel.py
"""

import sys

import bpy
from bl_ui import properties_game

PROBLEMS = []


def check(condition, text):
    if not condition:
        PROBLEMS.append(text)


class Recorder:
    """A layout that accepts every call, keeping each prop drawn (with whether its layout was
    active) and each label's text."""

    def __init__(self, record, active=True):
        self.__dict__["record"] = record
        self.__dict__["active"] = active

    def __getattr__(self, name):
        def call(*args, **kwargs):
            if name == "prop" and len(args) >= 2:
                self.record.append(("prop", args[1], self.active))
            elif name == "label" and "text" in kwargs:
                self.record.append(("label", kwargs["text"], self.active))
            return Recorder(self.record, self.active)
        return call

    def __setattr__(self, name, value):
        if name == "active":
            self.__dict__["active"] = self.__dict__["active"] and bool(value)


def draw(cls, ob):
    methods = {}
    for klass in reversed(cls.__mro__):
        for name, value in vars(klass).items():
            if callable(value) and not name.startswith("__"):
                methods[name] = value
    fake = type("FakePanel", (), methods)()
    record = []
    fake.layout = Recorder(record)
    with bpy.context.temp_override(object=ob, active_object=ob):
        cls.draw(fake, bpy.context)
    return record


def props(record):
    return {name: active for kind, name, active in record if kind == "prop"}


def labels(record):
    return [name for kind, name, _active in record if kind == "label"]


def polls(cls, ob):
    with bpy.context.temp_override(object=ob, active_object=ob):
        return cls.poll(bpy.context)


def main():
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    ob = bpy.data.objects.new("Ladder", bpy.data.meshes.new("Ladder"))
    top = bpy.data.objects.new("Ladder Top", None)
    scene.collection.objects.link(ob)
    scene.collection.objects.link(top)
    bpy.context.view_layer.objects.active = ob
    nav = properties_game.PHYSICS_PT_game_navigation
    link = properties_game.PHYSICS_PT_game_navigation_link
    check(not hasattr(properties_game, "PHYSICS_PT_game_obstacles"),
          "the old Create Obstacle panel is still there")

    # Every physics type: the panel shows, the Obstacle row except on a vehicle chassis.
    for physics_type in ('SENSOR', 'STATIC', 'KINEMATIC', 'DYNAMIC', 'RIGID_BODY', 'SOFT_BODY',
                         'CHARACTER', 'NO_COLLISION', 'VEHICLE'):
        ob.game.physics_type = physics_type
        check(polls(nav, ob), "%s: no Navigation panel" % physics_type)
        check(polls(link, ob), "%s: no Link sub-panel" % physics_type)
        drawn = props(draw(nav, ob))
        for name in ("use_nav_vehicle", "nav_area", "nav_npc_radius"):
            check(name in drawn, "%s: %s not drawn" % (physics_type, name))
        has_obstacle = "use_obstacle_create" in drawn and "nav_keep_away" in drawn
        check("obstacle_radius" not in drawn, "%s: the old obstacle Radius drawn" % physics_type)
        check(has_obstacle == (physics_type != 'VEHICLE'),
              "%s: the Obstacle row drawn %s" % (physics_type, has_obstacle))

    # A Static vehicle is warned about, a Kinematic one is not.
    ob.game.use_nav_vehicle = True
    for physics_type, expected in (('STATIC', True), ('KINEMATIC', False), ('DYNAMIC', False)):
        ob.game.physics_type = physics_type
        warned = any("Characters ride only" in text for text in labels(draw(nav, ob)))
        check(warned == expected, "%s vehicle: warned %s" % (physics_type, warned))
    ob.game.use_nav_vehicle = False
    ob.game.physics_type = 'RIGID_BODY'

    # NPC Radius (2026-11-20): 0 shows the automatic value it replaces and where it comes from.
    scene.game_settings.physics_engine = 'JOLT'
    mesh_ob = bpy.data.objects.new("Guard", bpy.data.meshes.new("Guard"))
    mesh_ob.data.from_pydata([(-0.4, -0.25, 0), (0.4, -0.25, 0), (0.4, 0.25, 1.8),
                              (-0.4, 0.25, 1.8)], [], [(0, 1, 2, 3)])
    scene.collection.objects.link(mesh_ob)
    # (Its bounds are known once evaluated.)
    bpy.context.view_layer.update()
    for physics_type, bounds, expected, source in (
            ('CHARACTER', None, 0.4, "its collision shape"),
            ('RIGID_BODY', None, 0.4, "its collision shape"),
            ('DYNAMIC', None, 0.9, "its collision shape"),
            ('NO_COLLISION', None, 0.4, "its mesh")):
        mesh_ob.game.physics_type = physics_type
        radius, said = properties_game.nav_npc_auto_radius(mesh_ob, scene)
        check(abs(radius - expected) < 1e-4 and said == source,
              "%s: auto radius %.3f (%s), expected %.3f (%s)" % (
                  physics_type, radius, said, expected, source))
    mesh_ob.game.physics_type = 'CHARACTER'
    shown = [text for text in labels(draw(nav, mesh_ob)) if text.startswith("Auto:")]
    check(shown == ["Auto: 0.40 m (its collision shape)"], "the NPC Radius row shows %s" % shown)
    mesh_ob.game.nav_npc_radius = 0.7
    shown = [text for text in labels(draw(nav, mesh_ob)) if text.startswith("Auto:")]
    check(not shown, "an NPC Radius set still shows %s" % shown)
    empty = bpy.data.objects.new("Marker", None)
    scene.collection.objects.link(empty)
    empty.game.physics_type = 'NO_COLLISION'
    radius, said = properties_game.nav_npc_auto_radius(empty, scene)
    check(said == "the scene's fallback NPC size" and
          abs(radius - scene.game_settings.recast_data.agent_radius) < 1e-6,
          "an empty without a body: %.3f (%s)" % (radius, said))

    # The Link sub-panel: greyed out without an end, its kind's name and give-up time only when
    # they mean something.
    drawn = props(draw(link, ob))
    check(drawn.get("nav_link_end") is True, "To is not drawn active")
    for name in ("nav_link_kind", "use_nav_link_both_ways", "use_nav_link_open",
                 "nav_link_when_closed", "nav_link_radius", "nav_link_duration",
                 "nav_link_reach"):
        check(drawn.get(name) is False, "without an end %s is not greyed out (%s)" % (
            name, drawn.get(name)))
    check("nav_link_kind_name" not in drawn, "Kind's name drawn for a Jump")
    check("nav_link_wait_time" not in drawn, "Give Up After drawn for Go Round")
    ob.game.nav_link_end = top
    ob.game.nav_link_kind = 'CUSTOM'
    ob.game.nav_link_when_closed = 'WAIT'
    drawn = props(draw(link, ob))
    for name in ("nav_link_kind", "nav_link_kind_name", "nav_link_wait_time", "nav_link_reach"):
        check(drawn.get(name) is True, "with an end %s is not drawn active" % name)

    # The defaults: RNA's, a new object's and the engine's (KX_NavObjectSettings) agree.
    engine = {
        "use_nav_vehicle": False,
        "nav_area": "",
        "nav_link_kind": 'JUMP',
        "nav_link_kind_name": "",
        "use_nav_link_both_ways": True,
        "use_nav_link_open": True,
        "nav_link_when_closed": 'GO_ROUND',
        "nav_link_wait_time": 0.0,
        "nav_link_radius": 0.5,
        "nav_link_duration": 0.0,
        "nav_link_reach": 1.5,
        "nav_npc_radius": 0.0,
        "nav_keep_away": 0.0,
    }
    fresh = bpy.data.objects.new("Fresh", bpy.data.meshes.new("Fresh"))
    rna = bpy.types.GameObjectSettings.bl_rna.properties
    for name, value in engine.items():
        default = rna[name].default
        if isinstance(value, float):
            check(abs(default - value) < 1e-6, "%s: RNA default %r, engine %r" % (
                name, default, value))
            check(abs(getattr(fresh.game, name) - value) < 1e-6,
                  "%s: a new object has %r" % (name, getattr(fresh.game, name)))
        else:
            check(default == value, "%s: RNA default %r, engine %r" % (name, default, value))
            check(getattr(fresh.game, name) == value,
                  "%s: a new object has %r" % (name, getattr(fresh.game, name)))
    check(fresh.game.nav_link_end is None, "a new object has a link")

    # The end pointer: never itself, kept by a copy, emptied when the end goes.
    try:
        ob.game.nav_link_end = ob
    except TypeError:
        pass
    check(ob.game.nav_link_end != ob, "an object is its own link's end")
    ob.game.nav_link_end = top
    copy = ob.copy()
    scene.collection.objects.link(copy)
    check(copy.game.nav_link_end == top, "a copy lost its link's end")
    check(top.users == 1, "the link's end counts a user: %d" % top.users)
    bpy.data.objects.remove(top)
    check(ob.game.nav_link_end is None, "the deleted end object is still the link's end")
    check(copy.game.nav_link_end is None, "the copy keeps the deleted end object")


main()
if PROBLEMS:
    for problem in PROBLEMS:
        print("PROBLEM:", problem)
    sys.exit(1)
print("OK: Physics > Navigation draws, greys and defaults as the engine reads it")
