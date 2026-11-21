# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Logic Nodes sidebar > Dashboard > Navigation (2026-11-20, Navigation_Ease_Of_Use_Plan.md item
9): drawn from the play report's "navigation" object (LN_Manager::WriteGameReport,
KX_Navigation::ReportJson).

The real panel draws with a layout that records every label and operator (no window needed),
from a report file written where the game writes it:

- without a report, or a report without navigation, it says to play the game once;
- setup problems come first under "Problems", each text with its count, an object's with a
  button selecting it; the moves of link ends (not setup errors) under "Notes";
- the navigation mesh built (Auto: built or not), the cell Auto Cells chose and why;
- the NPC sizes built with their counts and range, one sharing another size's navmesh said so;
- the NPCs by state and the cost a step;
- Scene > Navigation Mesh > Advanced shows the last play's Auto Cells cell from the same report.

    blender --background --factory-startup --python tests/python/logic_nodes_dashboard_navigation.py
"""

import json
import os
import sys

import bpy
from bl_ui import logic_nodes_dashboard as dashboard
from bl_ui import properties_game

PROBLEMS = []


def check(condition, text):
    if not condition:
        PROBLEMS.append(text)


class Recorder:
    """A layout that accepts every call, keeping each label's text and each operator."""

    def __init__(self, record):
        self.__dict__["record"] = record

    def __getattr__(self, name):
        def call(*args, **kwargs):
            if name == "label" and "text" in kwargs:
                self.record.append(("label", kwargs["text"]))
            elif name == "operator" and args:
                self.record.append(("operator", args[0]))
                return Operator(self.record)
            return Recorder(self.record)
        return call

    def __setattr__(self, name, value):
        pass


class Operator:
    def __init__(self, record):
        self.__dict__["record"] = record

    def __setattr__(self, name, value):
        self.record.append(("operator_property", "%s=%s" % (name, value)))


class FakeRegion:
    width = 2000


class FakeSystem:
    # (In the background the real preferences' ui_scale is 0.)
    ui_scale = 1.0


class FakePreferences:
    system = FakeSystem()


class FakeContext:
    def __init__(self):
        self.scene = bpy.context.scene
        self.region = FakeRegion()
        self.preferences = FakePreferences()
        self.space_data = None


def draw(cls, context):
    methods = {}
    for klass in reversed(cls.__mro__):
        for name, value in vars(klass).items():
            if callable(value) and not name.startswith("__"):
                methods[name] = value
    fake = type("FakePanel", (), methods)()
    record = []
    fake.layout = Recorder(record)
    cls.draw(fake, context)
    return record


def labels(record):
    return [text for kind, text in record if kind == "label"]


def report_path(scene):
    name = "".join(
        chr(b) if (b < 128 and (chr(b).isalnum() or chr(b) in "-_")) else "_"
        for b in scene.name.encode("utf-8"))
    return os.path.join(dashboard._report_folder(), "logic_nodes_report_" + name + ".json")


def write_report(scene, navigation, stamp):
    report = {"format": 1, "file": "", "scene": scene.name, "ended": "2026-11-20 12:00:00",
              "trees": []}
    if navigation is not None:
        report["navigation"] = navigation
    path = report_path(scene)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(report, handle)
    # (The panel reads a file again only when its time changed.)
    os.utime(path, (stamp, stamp))
    return path


NAVIGATION = {
    "build": "auto", "built": True, "cell_size": 0.15, "cell_height": 0.075,
    "auto_cells": True, "cell_reason": "0.15 m: for 'Guard', radius 0.30 m",
    "sizes": [
        {"radius": 0.3, "smallest": 0.3, "largest": 0.3, "npcs": 40, "own": True},
        {"radius": 0.6, "smallest": 0.5, "largest": 0.6, "npcs": 2, "own": False},
    ],
    "npcs": 42, "walking": 30, "waiting": 2, "arrived": 8, "failed": 2, "flying": 0,
    "climbing": 0, "other_navmesh": 0, "steps": 600, "step_ms": 0.42,
    "problems": [
        {"kind": "npc_rounded_up", "object": "Ogre",
         "text": "'Ogre' (radius 0.50 m) is treated as 0.60 m wide", "count": 1, "setup": True},
        {"kind": "link_end_moved", "object": "Ladder",
         "text": "link 'Ladder' -> 'Roof': its end moved down onto it", "count": 3,
         "setup": False},
    ],
}


def main():
    scene = bpy.context.scene
    context = FakeContext()
    panel = dashboard.NODE_PT_logic_nodes_navigation
    check(panel in dashboard.classes, "the Navigation panel is not registered")
    path = report_path(scene)
    if os.path.exists(path):
        os.remove(path)
    try:
        # No report: play once.
        drawn = labels(draw(panel, context))
        check(any("Play the game once" in text for text in drawn), "no report: %s" % drawn)
        # A report without navigation: the same.
        write_report(scene, None, 1_000_000_000)
        drawn = labels(draw(panel, context))
        check(any("Play the game once" in text for text in drawn),
              "no navigation in the report: %s" % drawn)

        bpy.data.objects.new("Ogre", None)
        write_report(scene, NAVIGATION, 1_000_000_100)
        record = draw(panel, context)
        drawn = labels(record)
        text = "\n".join(drawn)
        check(drawn.index("Problems") < drawn.index("Notes"), "Problems are not first")
        check("'Ogre' (radius 0.50 m) is treated as 0.60 m wide" in text, "the problem is missing")
        check("(3 times)" in text, "the note's count is missing")
        check(("operator", "object.logic_nodes_select_owner") in record and
              ("operator_property", "applied_object=Ogre") in record,
              "no button selects the problem's object")
        check(("operator_property", "applied_object=Ladder") not in record,
              "a button selects an object the file does not have")
        check("Navigation mesh: Auto (built)" in drawn, "the build is missing: %s" % drawn)
        check("Auto Cells: 0.15 m: for 'Guard', radius 0.30 m" in text, "the cell is missing")
        check("0.30 m: 40 NPCs (0.30 m)" in text, "the first size is missing:\n" + text)
        check("0.60 m: 2 NPCs (0.50 m to 0.60 m), sharing another size's navigation mesh" in text,
              "the shared size is missing:\n" + text)
        check("NPCs at the end: 42" in drawn, "the NPC count is missing")
        check("30 walking, 2 waiting, 8 arrived, 2 failing" in text, "the states are missing")
        check("Cost: 0.420 ms a step (600 steps)" in text, "the cost is missing")

        # Scene > Navigation Mesh > Advanced: the last play's cell.
        scene.game_settings.recast_data.use_auto_cells = True
        methods = {}
        for klass in reversed(properties_game.SCENE_PT_game_navmesh_advanced.__mro__):
            for name, value in vars(klass).items():
                if callable(value) and not name.startswith("__"):
                    methods[name] = value
        fake = type("FakePanel", (), methods)()
        record = []
        fake.layout = Recorder(record)
        properties_game.SCENE_PT_game_navmesh_advanced.draw(fake, bpy.context)
        check("Last play: 0.15 m: for 'Guard', radius 0.30 m" in labels(record),
              "Advanced does not show the last cell: %s" % labels(record))
    finally:
        if os.path.exists(path):
            os.remove(path)


main()
if PROBLEMS:
    for problem in PROBLEMS:
        print("PROBLEM:", problem)
    sys.exit(1)
print("OK: Dashboard > Navigation draws the play report's navigation")
