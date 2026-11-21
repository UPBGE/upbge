# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Scene > Navigation Mesh (2026-11-20, Navigation_Ease_Of_Use_Plan.md item 6): what most games
set in sight, everything else in Advanced.

The real panels draw with a layout that records every property and label (no window needed):

- the panel shows Build (with Build Radius and Remove Radius only for Around NPCs), Show (the
  navigation's Visualization switch), Jumps and Avoid Physics Objects, and nothing else;
- Advanced holds every other navigation setting of the scene: each RecastData property is drawn
  in one of the two panels, exactly once;
- Cell Size and Cell Height are greyed out while Auto Cells is on, the jump distances while Jumps
  is Off, Physics Min Size while Avoid Physics Objects is off, the Visualization rows while Show
  is off;
- a new scene builds Auto, with Auto Cells (the startup file and the RNA defaults agree).

    blender --background --factory-startup --python tests/python/game_navigation_scene_panel.py
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
                self.record.append(("prop", args[1], self.active, kwargs.get("text")))
            elif name == "label" and "text" in kwargs:
                self.record.append(("label", kwargs["text"], self.active, None))
            return Recorder(self.record, self.active)
        return call

    def __setattr__(self, name, value):
        if name == "active":
            self.__dict__["active"] = self.__dict__["active"] and bool(value)


def draw(cls):
    methods = {}
    for klass in reversed(cls.__mro__):
        for name, value in vars(klass).items():
            if callable(value) and not name.startswith("__"):
                methods[name] = value
    fake = type("FakePanel", (), methods)()
    record = []
    fake.layout = Recorder(record)
    cls.draw(fake, bpy.context)
    return record


def props(record):
    out = {}
    for kind, name, active, _text in record:
        if kind == "prop":
            out.setdefault(name, []).append(active)
    return out


def main():
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    gs = scene.game_settings
    rd = gs.recast_data
    main_panel = properties_game.SCENE_PT_game_navmesh
    advanced = properties_game.SCENE_PT_game_navmesh_advanced
    check(getattr(advanced, "bl_parent_id", "") == "SCENE_PT_game_navmesh",
          "Advanced is not a sub-panel of Navigation Mesh")
    check('DEFAULT_CLOSED' in getattr(advanced, "bl_options", set()), "Advanced is open by default")

    # A new scene: Auto, with Auto Cells.
    check(rd.build_mode == 'AUTO', "a new scene builds %s" % rd.build_mode)
    check(rd.use_auto_cells, "a new scene has Auto Cells off")
    rna = bpy.types.SceneGameRecastData.bl_rna.properties
    check(rna["build_mode"].default == 'AUTO', "Build's RNA default is %s" % rna["build_mode"].default)
    check(rna["use_auto_cells"].default is True, "Auto Cells' RNA default is off")

    # In sight: four things.
    rd.build_mode = 'AUTO'
    shown = props(draw(main_panel))
    check(set(shown) == {"build_mode", "show_obstacle_simulation", "jump_links",
                         "use_avoid_physics_objects"},
          "the panel shows %s" % sorted(shown))
    show_text = [text for kind, name, _a, text in draw(main_panel)
                 if kind == "prop" and name == "show_obstacle_simulation"]
    check(show_text == ["Show"], "the visualization switch reads %s" % show_text)
    rd.build_mode = 'AROUND_NPCS'
    shown = props(draw(main_panel))
    check("build_radius" in shown and "remove_radius" in shown,
          "Around NPCs: no Build Radius / Remove Radius")
    rd.build_mode = 'AUTO'

    # Every navigation setting drawn once, in one of the two panels.
    rd.partitioning = 'WATERSHED'
    everything = {}
    for panel in (main_panel, advanced):
        for name, actives in props(draw(panel)).items():
            everything.setdefault(name, []).extend(actives)
    rd.build_mode = 'AROUND_NPCS'
    for name, actives in props(draw(main_panel)).items():
        if name in {"build_radius", "remove_radius"}:
            everything.setdefault(name, []).extend(actives)
    rd.build_mode = 'AUTO'
    for prop in rna:
        if prop.identifier == "rna_type":
            continue
        count = len(everything.get(prop.identifier, []))
        check(count == 1, "%s is drawn %d times" % (prop.identifier, count))

    # Greyed while they do nothing.
    rd.use_auto_cells = True
    drawn = props(draw(advanced))
    check(drawn["cell_size"] == [False] and drawn["cell_height"] == [False],
          "Cell Size / Height not greyed with Auto Cells")
    rd.use_auto_cells = False
    drawn = props(draw(advanced))
    check(drawn["cell_size"] == [True] and drawn["cell_height"] == [True],
          "Cell Size / Height greyed without Auto Cells")
    rd.jump_links = 'OFF'
    drawn = props(draw(advanced))
    check(all(drawn[name] == [False] for name in ("jump_gap", "jump_up", "jump_drop",
                                                  "jump_spacing")),
          "the jump distances are not greyed with Jumps Off")
    rd.jump_links = 'AUTO'
    drawn = props(draw(advanced))
    check(all(drawn[name] == [True] for name in ("jump_gap", "jump_up", "jump_drop",
                                                 "jump_spacing")),
          "the jump distances are greyed with Jumps Auto")
    rd.use_avoid_physics_objects = False
    check(props(draw(advanced))["physics_min_size"] == [False],
          "Physics Min Size not greyed without Avoid Physics Objects")
    gs.show_obstacle_simulation = False
    check(props(draw(advanced))["vis_range"] == [False], "Visualization not greyed without Show")
    gs.show_obstacle_simulation = True
    check(props(draw(advanced))["vis_range"] == [True], "Visualization greyed with Show")


main()
if PROBLEMS:
    for problem in PROBLEMS:
        print("PROBLEM:", problem)
    sys.exit(1)
print("OK: Scene > Navigation Mesh shows four things, Advanced the rest, each once")
