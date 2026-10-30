# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The UI guide is enough to rebuild the Complete UI Example.

Builds the example in a new file the way a creator following game_ui_guide.html does, from what
the page prints (read back from its HTML as a reader reads it): objects from the Add menu, placed
and parented (Ctrl P › Object (Without Inverse)) by the objects tables, with the settings their
rows and the lines above the tables give; materials from the Material tab's New button with the
inputs the materials tables give; the scene settings and the actions of the tables. The node trees
are built from the example once each picture is checked against its frame (nodes, values, links).
Then compares every setting that matters in the game with the example itself. It also runs every
check of the guide's generator (each object, logic frame and material of the example is explained,
nothing a picture cannot show).

  blender --background --factory-startup --python game_ui_guide_rebuild_test.py -- \\
      [--output-dir DIR]

With an output directory, the rebuilt file is saved there (game_ui_guide_rebuilt.blend), to play.
"""

import argparse
import html
import math
import os
import re
import sys
import traceback

import bpy
from mathutils import Euler, Matrix, Vector

SOURCE_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
GUIDE_DIR = os.path.join(SOURCE_ROOT, "release", "datafiles", "game_ui")
sys.path.insert(0, GUIDE_DIR)
# No __pycache__ in the source tree.
sys.dont_write_bytecode = True

import make_game_ui_guide as G  # noqa: E402
from _bpy_internal.game_ui import recipe as R  # noqa: E402

EXAMPLE = os.path.join(GUIDE_DIR, R.EXAMPLE_FILE)


# -----------------------------------------------------------------------------
# What the guide says


def read_guide():
    """What the page prints (as make_game_ui_guide.py writes it, after every check it makes),
    the example's recipe for the trees, and the example's fingerprint."""
    start = G.start_values()
    bpy.ops.wm.open_mainfile(filepath=EXAMPLE, load_ui=False)
    ex = G.Example("", start)
    page = G.page(ex)
    check_pictures(page, ex.recipe)
    guide = {
        "objects": printed_objects(page),
        "materials": printed_materials(page),
        "scene": printed_scene(page),
        "actions": printed_actions(page),
        "recipe": ex.recipe,
    }
    return guide, fingerprint(ex.scene)


NUMBERS = r"\(([-0-9.e, ]+)\)"
ID_KINDS = {"Object": "objects", "Collection": "collections", "Sound": "sounds",
            "Material": "materials", "NodeTree": "node_groups", "Image": "images",
            "VectorFont": "fonts", "World": "worlds", "Mesh": "meshes", "Text": "texts"}


def _numbers(text):
    return Vector(float(value) for value in text.split(","))


def _text(fragment):
    """A piece of the page as it reads: no tags, one space between words."""
    return " ".join(html.unescape(re.sub(r"<[^>]+>", " ", fragment)).split())


def _bold(fragment):
    return [html.unescape(text) for text in re.findall(r"<b>([^<]*)</b>", fragment)]


def _cells(row):
    return re.findall(r"<td[^>]*>(.*?)</td>", row, flags=re.S)


def split_items(fragment):
    """The items of a list the page writes with ", " between them (not one inside a bold value,
    a code block, brackets or quotes)."""
    items, depth, begin, index = [], 0, 0, 0
    while index < len(fragment):
        if fragment.startswith(("<b>", "<code"), index):
            depth += 1
            index = fragment.index(">", index) + 1
            continue
        if fragment.startswith(("</b>", "</code>"), index):
            depth -= 1
            index = fragment.index(">", index) + 1
            continue
        char = fragment[index]
        if char in "(“":
            depth += 1
        elif char in ")”":
            depth -= 1
        elif depth == 0 and fragment.startswith(", ", index):
            items.append(fragment[begin:index])
            begin = index + 2
            index += 2
            continue
        index += 1
    items.append(fragment[begin:])
    return [item.strip() for item in items if item.strip()]


def label_and_value(item):
    """“Label <b>value</b> (a note)” → (Label, value); “Label” → (Label, None)."""
    found = re.match(r"(.*?)\s*<b>([^<]*)</b>", item, flags=re.S)
    if found is None:
        return _text(item), None
    return _text(found.group(1)), html.unescape(found.group(2))


def parse_value(prop, text):
    """A value as the page prints it (enum names, “text”, on/off, (x, y, z), names), in the
    recipe's encoding, for the RNA property it is."""
    if prop.type == 'POINTER':
        if text == "empty":
            return None
        return {"id": ID_KINDS[prop.fixed_type.identifier], "name": text}
    if prop.type == 'BOOLEAN':
        return {"on": True, "off": False}[text]
    if prop.type == 'ENUM':
        for item in prop.enum_items:
            if item.name == text:
                return item.identifier
        raise RuntimeError(f"{prop.identifier}: no choice {text!r}")
    if prop.type == 'STRING':
        if text == "empty":
            return ""
        if not (text.startswith("“") and text.endswith("”")):
            raise RuntimeError(f"{prop.identifier}: {text!r} is not quoted")
        return text[1:-1].replace("↵", "\n")
    if prop.type in {'INT', 'FLOAT'}:
        if getattr(prop, "array_length", 0):
            values = [float(value) for value in text.strip("()").split(",")]
            if len(values) == prop.array_length - 1:
                # A color's alpha of 1 is not printed.
                values.append(1.0)
            return values
        return int(text) if prop.type == 'INT' else float(text)
    raise RuntimeError(f"{prop.identifier}: cannot read a {prop.type}")


def rna_named(struct_rna, label):
    """The property Blender shows as `label` (its name)."""
    found = [prop for prop in struct_rna.properties if prop.name == label]
    if len(found) != 1:
        raise RuntimeError(f"{struct_rna.identifier}: {len(found)} settings named {label!r}")
    return found[0]


# How Blender's Physics tab and Object tab name what the tables print (properties_game.py,
# properties_object.py): written here on their own, not taken from the generator.
PHYSICS_LABELS = {"Actor": "use_actor", "Ghost": "use_ghost",
                  "Collision Bounds › Bounds": "collision_bounds_type"}
OBJECT_PLACES = {
    "Visibility › Show In › Renders": ("hide_render", True),
    "Visibility › Show In › Viewports": ("hide_viewport", True),
    "Visibility › Ray Visibility › Camera": ("visible_camera", False),
    "Visibility › Ray Visibility › Shadow": ("visible_shadow", False),
    "Visibility › Ray Visibility › Diffuse": ("visible_diffuse", False),
    "Visibility › Ray Visibility › Glossy": ("visible_glossy", False),
    "Visibility › Ray Visibility › Transmission": ("visible_transmission", False),
    "Visibility › Ray Visibility › Volume Scatter": ("visible_volume_scatter", False),
}
# The UI Element panel and its sub-panels, with the labels the Widget panel gives some settings.
UI_PANELS = {
    "UI": ("use_element", "action", "interactable", "pointer", "focus", "tab_order", "anchor",
           "player", "use_any_player"),
    "Drag": ("use_draggable", "follow_cursor", "drag_look", "use_click_pick_up",
             "drag_threshold", "use_drop_target", "accept_categories", "drop_group"),
    "Look": ("hover_scale", "press_scale", "look_time", "highlight_object"),
    "Widget": ("widget", "widget_object", "widget_property", "widget_min", "widget_max",
               "widget_step", "widget_part", "widget_max_property", "look_time"),
    "Nav": ("navigation_up", "navigation_down", "navigation_left", "navigation_right"),
    "Adv": ("priority", "padding"),
}
WIDGET_LABELS = {
    ("widget_part", 'SLIDER'): "Handle", ("widget_part", 'TOGGLE'): "Check Mark",
    ("widget_part", 'TEXT'): "Text", ("widget_part", 'BAR'): "Fill",
    ("widget_part", 'SELECTOR'): "Text", ("widget_max", 'TEXT'): "Max Length",
    ("look_time", 'BAR'): "Smoothing",
}


def ui_key(group, label, widget):
    ui_rna = bpy.types.GameObjectUISettings.bl_rna
    for key in UI_PANELS[group]:
        if key == "look_time" and (group == "Widget") != (widget == 'BAR'):
            continue
        if WIDGET_LABELS.get((key, widget), ui_rna.properties[key].name) == label:
            return key
    raise RuntimeError(f"no UI setting {label!r} in {group}")


def property_value(kind, text, item_type=None):
    """A game property's value, a list item or a table cell as the page prints it."""
    if kind == 'OBJECT':
        return None if text == "empty" else {"id": "objects", "name": text}
    if kind == 'BOOL':
        return {"on": True, "off": False}[text]
    if kind == 'INT':
        return int(text)
    if kind in {'FLOAT', 'TIMER'}:
        return float(text)
    if kind in {'VECTOR', 'COLOR'}:
        return [float(value) for value in text.strip("()").split(",")]
    if kind == 'STRING':
        if text == "empty":
            return ""
        return text[1:-1].replace("↵", "\n") if text.startswith("“") else text
    raise RuntimeError(f"cannot read a {kind} value {text!r}")


PROPERTY_TYPES = {item.name: item.identifier for item in
                  bpy.types.GameProperty.bl_rna.properties["type"].enum_items}


def printed_property(fragment):
    """A game property as property_text prints it: “<b>name</b> (Type) = value”, “… columns a
    (Type), … rows x | y; …”, “… of Type: a; b”, “… options A, B, value A”, “ (Save Game)”."""
    name = _bold(fragment)[0]
    rest = _text(fragment[fragment.index("</b>") + 4:])
    save = rest.endswith(" (Save Game)")
    if save:
        rest = rest[:-len(" (Save Game)")]
    found = re.match(r"\((\w+)\)(.*)$", rest)
    kind = PROPERTY_TYPES[found.group(1)]
    rest = found.group(2)
    entry = {"name": name, "type": kind, "save_game": save, "debug": False}
    if kind == 'TABLE':
        found = re.fullmatch(r" columns (.*?), (?:rows (.*)|no rows)", rest)
        entry["columns"] = []
        for column in found.group(1).split(", "):
            parts = re.fullmatch(r"(.*?) \((\w+)\)(?: default (.*))?", column)
            column_kind = PROPERTY_TYPES[parts.group(2)]
            default = parts.group(3)
            entry["columns"].append({
                "name": parts.group(1), "type": column_kind,
                "value": property_value(column_kind, default) if default else None})
        entry["rows"] = [
            [property_value(column["type"], cell) for column, cell in
             zip(entry["columns"], row.split(" | "))]
            for row in (found.group(2).split("; ") if found.group(2) else [])]
    elif kind == 'LIST':
        found = re.fullmatch(r" of (\w+)(?:: (.*)|, empty)", rest)
        entry["item_type"] = PROPERTY_TYPES[found.group(1)]
        entry["elements"] = [property_value(entry["item_type"], element)
                             for element in (found.group(2).split("; ") if found.group(2)
                                             else [])]
    elif kind == 'CHOICE':
        found = re.fullmatch(r" options (.*), value (.*)", rest)
        entry["options"] = found.group(1).split(", ")
        entry["value"] = found.group(2)
    else:
        found = re.fullmatch(r" = (.*)", rest)
        entry["value"] = property_value(kind, found.group(1))
    return entry


def printed_settings(cell):
    """An object's Settings cell: a line per group (“Physics <b>Dynamic</b>, Actor”)."""
    out = {"object": {}, "game": {}, "ui": {}, "properties": [], "bindings": [],
           "physics": None, "materials": "shared"}
    for group in re.findall(r"<div>(.*?)</div>", cell, flags=re.S):
        found = re.match(r'<span class="grp">([^<]*)</span> ', group)
        label = found.group(1) if found else ""
        body = group[found.end():] if found else group
        items = split_items(body)
        if label == "":
            if "<b>no material</b>" in body:
                out["materials"] = []
            elif _text(body).startswith("material "):
                out["materials"] = _bold(body)[0].split(", ")
            else:
                raise RuntimeError(f"the test cannot read {_text(body)!r}")
        elif label == "Physics":
            game_rna = bpy.types.GameObjectSettings.bl_rna
            for item in items:
                name, value = label_and_value(item)
                if name == "":
                    out["physics"] = parse_value(game_rna.properties["physics_type"], value)
                    continue
                key = PHYSICS_LABELS[name]
                out["game"][key] = (True if value is None else
                                    parse_value(game_rna.properties[key], value))
                if key == "collision_bounds_type":
                    # Choosing Bounds turns Collision Bounds on (Jolt shows no checkbox).
                    out["game"]["use_collision_bounds"] = True
        elif label == "Object":
            for item in items:
                name, value = label_and_value(item)
                key, inverted = OBJECT_PLACES[name]
                on = {"on": True, "off": False}[value]
                out["object"][key] = (not on) if inverted else on
        elif label == "Body":
            code = re.search(r'<code class="body">(.*?)</code>', body, flags=re.S)
            out["body"] = html.unescape(code.group(1)) if code else ""
            if code is None and "<b>empty</b>" not in body:
                raise RuntimeError(f"the test cannot read the body {_text(body)!r}")
        elif label in UI_PANELS:
            ui_rna = bpy.types.GameObjectUISettings.bl_rna
            for index, item in enumerate(items):
                name, value = label_and_value(item)
                if name == "":
                    # The group's own setting, first: UI Element on, the Widget kind.
                    key = {"UI": "use_element", "Widget": "widget"}[label]
                else:
                    key = ui_key(label, name, out["ui"].get("widget", 'NONE'))
                out["ui"][key] = (True if value is None else
                                  parse_value(ui_rna.properties[key], value))
        elif label == "Property":
            out["properties"].append(printed_property(body))
        elif label == "Logic":
            out["bindings"] = [{"tree": tree, "enabled": True} for tree in _bold(body)]
        else:
            raise RuntimeError(f"the test cannot read the group {label!r}")
    return out


def printed_shape(place, kind):
    """What the Shape cell says of a text, a camera or a light (its data settings), and of a
    plane or a box its points and faces from the object's origin."""
    shape = place["shape"]
    if kind == 'FONT':
        parts = shape.split(", ")
        if parts[0] != "Text":
            raise RuntimeError(f"not a text: {shape!r}")
        text_rna = bpy.types.TextCurve.bl_rna
        labels = {"Size": "size", "Horizontal": "align_x", "Vertical": "align_y"}
        for part in parts[1:]:
            label, value = part.split(" ", 1)
            key = labels[label]
            place["text"][key] = (float(value) if key == "size" else
                                  parse_value(text_rna.properties[key], value))
        return None
    if kind == 'CAMERA':
        found = re.fullmatch(
            r"Camera, (?:orthographic, scale ([-0-9.]+)|([-0-9.]+) mm)(?:, (.+))?", shape)
        if found is None:
            raise RuntimeError(f"not a camera: {shape!r}")
        if found.group(1):
            place["data"].update({"type": 'ORTHO', "ortho_scale": float(found.group(1))})
        else:
            place["data"]["lens"] = float(found.group(2))
        # Its other settings, "Label value" each (a minimap's look: "Lighting Flat").
        camera_rna = bpy.types.Camera.bl_rna
        for part in found.group(3).split(", ") if found.group(3) else ():
            matches = [prop for prop in camera_rna.properties
                       if prop.name and part.startswith(prop.name + " ")]
            if not matches:
                raise RuntimeError(f"camera setting {part!r} is no Camera property")
            prop = max(matches, key=lambda prop: len(prop.name))
            place["data"][prop.identifier] = parse_value(prop, part[len(prop.name) + 1:])
        return None
    if kind == 'LIGHT':
        found = re.fullmatch(r"(\w+) light", shape)
        place["light_type"] = found.group(1).upper()
        return None
    if kind == 'EMPTY':
        if shape != "Empty":
            raise RuntimeError(f"not an empty: {shape!r}")
        return None
    found = re.match(r"(Plane|Upright plane|Box) ([-0-9.]+(?: × [-0-9.]+)+)$", shape)
    if not found:
        return None
    kind, size = found.group(1), [float(value) for value in found.group(2).split(" × ")]
    middle = place.get("mesh middle", Vector((0.0, 0.0, 0.0)))
    half = [value / 2.0 for value in size]
    if kind == "Plane":
        corners = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
        extent = (half[0], half[1], 0.0)
        faces = [(0, 1, 2, 3)]
    elif kind == "Upright plane":
        # A plane turned 90° on X: it faces -Y.
        corners = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
        extent = (half[0], 0.0, half[1])
        faces = [(0, 1, 2, 3)]
    else:
        # As Add > Mesh > Cube: every face faces out.
        corners = [(x, y, z) for z in (-1, 1) for y in (-1, 1) for x in (-1, 1)]
        extent = tuple(half)
        faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2),
                 (1, 3, 7, 5)]
    points = [Vector([middle[a] + corner[a] * extent[a] for a in range(3)]) for corner in corners]
    return points, faces


def _shared(paragraph):
    """What the line above a table says its rows share (unless a row says otherwise)."""
    shared = {}
    if _text(paragraph) == "Each row gives its own collection and settings.":
        return shared
    game_rna = bpy.types.GameObjectSettings.bl_rna
    for part in paragraph.split(" · "):
        words, bold = _text(part), _bold(part)
        if words.startswith("In collection"):
            shared["homes"] = bold
        elif words.startswith("rows without └ are children of"):
            shared["parent"] = bold[0]
        elif words.startswith("Physics"):
            shared["physics"] = parse_value(game_rna.properties["physics_type"], bold[0])
        elif words.startswith("meshes: material"):
            shared["MESH"] = bold[0]
        elif words.startswith("texts: material"):
            shared["FONT"] = bold[0]
        else:
            raise RuntimeError(f"the test cannot read {words!r} above a table")
    return shared


def printed_objects(page):
    """Every object as the page prints it, as a reader sees it. From the objects tables: its
    parent (the row above it, one step less indented, or the line above the table, or the row's
    “child of”), collections, shape, Location (from the parent), mesh middle, turn, scale and
    settings, each from the row or else from the line above the table. From the Player's
    properties table, its properties. From the items and pickups tables and their chapter's
    words, the items and pickups."""
    printed = {}
    shared, above = {}, []
    rows = re.compile(r'<p class="shared">(.*?)</p>|<tr data-object="([^"]*)">(.*?)</tr>', re.S)
    for match in rows.finditer(page):
        if match.group(1) is not None:
            shared, above = _shared(match.group(1)), []
            continue
        name, row = html.unescape(match.group(2)), match.group(3)
        cells = _cells(row)
        depth = (int(re.search(r"padding-left:(\d+)px", row).group(1)) - 8) // 18
        notes = [html.unescape(note)
                 for note in re.findall(r'<span class="what">([^<]*)</span>', cells[0])]
        del above[depth:]
        if depth:
            parent = above[depth - 1]
        else:
            child_of = [note[len("child of "):] for note in notes if note.startswith("child of ")]
            parent = child_of[0] if child_of else shared.get("parent")
        above.append(name)
        homes = [note[len("in "):].split(", ") for note in notes if note.startswith("in ")]
        lines = [_text(line) for line in cells[2].split("<br>")]
        place = {"parent": parent, "homes": homes[0] if homes else shared["homes"],
                 "shape": _text(cells[1]), "location": _numbers(lines[0].strip("()")),
                 "text": {}, "data": {}}
        for line in lines[1:]:
            key, numbers = re.fullmatch(r"(mesh middle|rotated|scale) " + NUMBERS + "°?",
                                        line).groups()
            place[key] = _numbers(numbers)
        place.update(printed_settings(cells[3]))
        place["shared"] = shared
        printed[name] = place
    # The Player's properties: the table after “The Player's game properties:”.
    table = re.search(r"The Player's game properties:</p>\s*(?:<div class=\"scroll\">)?"
                      r"<table.*?</table>", page, flags=re.S).group(0)
    printed["Player"]["properties"] = [printed_property(_cells(row)[0]) for row in
                                       re.findall(r"<tr>(.*?)</tr>", table, flags=re.S)
                                       if _cells(row)]
    # Items: their table, with the types in its heads; item (its own name) and count 1, Ghost
    # on, in the collection ItemLibrary (the chapter's steps).
    items = re.search(r"<table class=\"data\"><thead><tr><th>Object</th><th>name \((\w+)\)"
                      r".*?</table>", page, flags=re.S).group(0)
    heads = [_text(head) for head in re.findall(r"<th>(.*?)</th>", items)]
    columns = [re.fullmatch(r"(\w+) \((\w+)\)", head).groups() for head in heads[1:5]]
    steps = re.search(r"<code>item</code> \((\w+), its own name\) and <code>count</code>\s*"
                      r"\((\w+)\) 1", page).groups()
    for row in re.findall(r"<tr>(.*?)</tr>", items, flags=re.S):
        texts = [_text(cell) for cell in _cells(row)]
        if not texts:
            continue
        properties = [{"name": prop, "type": PROPERTY_TYPES[kind], "save_game": False,
                       "debug": False,
                       "value": property_value(PROPERTY_TYPES[kind],
                                               value.replace("↵", "\n"))}
                      for (prop, kind), value in zip(columns, texts[1:5])]
        properties += [
            {"name": "item", "type": PROPERTY_TYPES[steps[0]], "save_game": False,
             "debug": False, "value": texts[0]},
            {"name": "count", "type": PROPERTY_TYPES[steps[1]], "save_game": False,
             "debug": False, "value": 1}]
        printed[texts[0]] = {"parent": None, "homes": ["ItemLibrary"], "free": True,
                             "physics": None, "materials": [texts[5]], "object": {},
                             "game": {"use_ghost": True}, "ui": {}, "properties": properties,
                             "bindings": [], "text": {}, "data": {}}
    # Pickups: their table; Ghost on, in the Scene Collection (the chapter's words).
    pickups = re.search(r"<summary>All \d+ pickups</summary>.*?</table>", page,
                        flags=re.S).group(0)
    heads = [_text(head) for head in re.findall(r"<th>(.*?)</th>", pickups)]
    kinds = {prop: PROPERTY_TYPES[kind] for prop, kind in
             (re.fullmatch(r"(\w+) \((\w+)\)", head).groups() for head in heads[1:3])}
    for row in re.findall(r"<tr>(.*?)</tr>", pickups, flags=re.S):
        texts = [_text(cell) for cell in _cells(row)]
        if not texts:
            continue
        properties = [{"name": prop, "type": kinds[prop], "save_game": False, "debug": False,
                       "value": property_value(kinds[prop], value)}
                      for prop, value in (("item", texts[1]), ("count", texts[2]))]
        printed[texts[0]] = {"parent": None, "homes": ["Scene Collection"], "free": True,
                             "physics": None, "materials": [texts[6]],
                             "location": _numbers(texts[3].strip("()")), "object": {},
                             "game": {"use_ghost": True}, "ui": {}, "properties": properties,
                             "bindings": [], "text": {}, "data": {}}
    return printed


def printed_materials(page):
    """The materials tables: each material's Principled BSDF inputs and settings by name."""
    materials = {}
    for table in re.findall(r'<table class="mats">.*?</table>', page, flags=re.S):
        for row in re.findall(r"<tr>(.*?)</tr>", table, flags=re.S):
            cells = _cells(row)
            if not cells:
                continue
            name = _bold(cells[0])[0]
            values = {}
            if _text(cells[1]) != "as a new material":
                for item in split_items(cells[1]):
                    label, value = label_and_value(item)
                    if label == "Base Color = Emission Color":
                        values["Base Color"] = values["Emission Color"] = value
                    else:
                        values[label] = value
            materials[name] = values
    return materials


def printed_scene(page):
    """The scene settings table: (where, value) rows."""
    rows = []
    for row in re.findall(r"<tr>(.*?)</tr>", page, flags=re.S):
        cells = _cells(row)
        if len(cells) == 2 and " tab › " in _text(cells[0]):
            rows.append((_text(cells[0]), _bold(cells[1])[0]))
    return rows


def printed_actions(page):
    """The Input Map table: each action to add with its bindings."""
    table = re.search(r"<table class=\"data\"><thead><tr><th>Action</th>.*?</table>", page,
                      flags=re.S).group(0)
    binding_rna = bpy.types.GameInputBinding.bl_rna
    action_rna = bpy.types.GameInputAction.bl_rna

    def choice(prop, label):
        return parse_value(binding_rna.properties[prop], label)

    actions = []
    for row in re.findall(r"<tr>(.*?)</tr>", table, flags=re.S):
        cells = _cells(row)
        if not cells:
            continue
        name = _bold(cells[0])[0]
        kind = parse_value(action_rna.properties["type"], _text(cells[1]))
        entry = {"settings": {"name": name, "type": kind, "context": _text(cells[2])},
                 "bindings": []}
        for column, device in ((cells[3], 'KEYBOARD'), (cells[4], 'GAMEPAD')):
            for text in split_items(html.unescape(column)) if _text(column) else []:
                binding = {"device": device}
                found = re.fullmatch(r"(.*?)(?: \((\w+)\))?", text)
                text, direction = found.groups()
                if direction:
                    binding["direction"] = choice("direction", direction)
                *mods, text = text.split("+")
                for mod in mods:
                    binding[{"Shift": "use_shift", "Ctrl": "use_ctrl", "Alt": "use_alt"}[mod]] = \
                        True
                if device == 'GAMEPAD':
                    binding["gamepad_control"] = choice("gamepad_control", text)
                elif text.endswith(" Mouse Button") or "Wheel" in text:
                    binding["device"] = 'MOUSE'
                    binding["mouse_button"] = choice("mouse_button",
                                                     text.removesuffix(" Mouse Button"))
                else:
                    binding["key"] = choice("key", text)
                entry["bindings"].append(binding)
        actions.append(entry)
    return actions


def check_pictures(page, recipe):
    """Each node picture against its frame in the file: a title per node (the name Blender's
    header shows, in full under the picture when cut), every input's value (in full under it when
    cut), a wire per link inside the frame and a stub per socket linked to another frame."""
    for figure in re.findall(r'<figure class="frame">.*?</figure>', page, flags=re.S):
        found = re.search(r'<span class="tree">([^<]*)</span> (.*?)</figcaption>', figure)
        tree = bpy.data.node_groups[html.unescape(found.group(1))]
        title = html.unescape(found.group(2))
        frame = next(node for node in tree.nodes
                     if node.bl_idname == "NodeFrame" and node.label == title)
        nodes = [node for node in tree.nodes if node.parent == frame]
        notes = _text(" ".join(re.findall(r'<p class="note">(.*?)</p>', figure, flags=re.S)))
        full = {shown.strip(): name.strip()
                for shown, name in re.findall(r"([^;:]+?…) is ([^;.]+)", notes)}
        shown = [full.get(html.unescape(text), html.unescape(text)) for text in
                 re.findall(r'class="n-title">([^<]*)</text>', figure)]
        expected = []
        for node in nodes:
            operation = getattr(node, "operation", None)
            if node.bl_idname in {"LogicNativeInventory", "LogicNativeMath"}:
                expected.append(G.enum_label(node.bl_rna, "operation", operation))
            else:
                expected.append(node.bl_label)
        if sorted(shown) != sorted(expected):
            raise RuntimeError(f"{tree.name} › {title}: the picture's nodes {sorted(shown)} are "
                               f"not the frame's {sorted(expected)}")
        values = [html.unescape(text) for text in
                  re.findall(r'class="n-value"[^>]*>([^<]*)</text>', figure)]
        for node in nodes:
            drawn = {item.identifier for side, item in G.node_layout(node) if side == "in"}
            headers = {item[1].identifier for side, item in G.node_layout(node)
                       if side == "panel" and item[1] is not None}
            for socket in node.inputs:
                if not socket.enabled or socket.hide or socket.is_linked:
                    continue
                if socket.identifier not in drawn | headers:
                    # In a folded section the picture leaves closed: a new node's value.
                    if G.socket_value(socket) != G.new_node_inputs(node.bl_idname)[
                            socket.identifier]:
                        raise RuntimeError(f"{tree.name} › {node.name}: {socket.identifier} is "
                                           "set, but its section is drawn closed")
                    continue
                value = G.socket_value(socket)
                if value is None or isinstance(value, bool):
                    continue
                if (node.bl_idname, socket.identifier) in G.SOCKET_DRAWS:
                    # Its row shows settings (X, Y, Z) instead: Blender shows no value to set,
                    # so it must be a new node's.
                    fresh = bpy.data.node_groups.new("__fresh", tree.bl_idname)
                    new = fresh.nodes.new(node.bl_idname)
                    same = G.socket_value(next(s for s in new.inputs
                                               if s.identifier == socket.identifier)) == value
                    bpy.data.node_groups.remove(fresh)
                    if not same:
                        raise RuntimeError(f"{tree.name} › {node.name}: {socket.identifier} is "
                                           "set, but its row shows no value")
                    continue
                value = value or "—"
                if value not in values and f"{G.socket_label(socket)}: {value}" not in notes:
                    raise RuntimeError(f"{tree.name} › {title} › {node.name}: "
                                       f"{socket.identifier} = {value!r} is not in the picture")
        inside = [link for link in tree.links
                  if link.from_node.parent == frame and link.to_node.parent == frame]
        ends = {("in", link.to_node.name, link.to_socket.identifier)
                if link.to_node.parent == frame
                else ("out", link.from_node.name, link.from_socket.identifier)
                for link in tree.links
                if (link.from_node.parent == frame) != (link.to_node.parent == frame)}
        if len(re.findall(r'class="wire"', figure)) != len(inside) or \
                len(re.findall(r'class="wire stub"', figure)) != len(ends):
            raise RuntimeError(f"{tree.name} › {title}: the picture's wires are not the links")


# -----------------------------------------------------------------------------
# Building it as a creator does


def resolve(value):
    if value is None:
        return None
    return getattr(bpy.data, value["id"])[value["name"]]


def add_object(kind, place):
    """An object as the Add menu makes it."""
    if kind == 'MESH':
        bpy.ops.mesh.primitive_plane_add()
    elif kind == 'FONT':
        bpy.ops.object.text_add()
    elif kind == 'EMPTY':
        bpy.ops.object.empty_add()
    elif kind == 'CAMERA':
        bpy.ops.object.camera_add()
    elif kind == 'LIGHT':
        bpy.ops.object.light_add(type=place["light_type"])
    else:
        raise RuntimeError(f"no Add menu entry for a {kind}")
    return bpy.context.active_object


def build(guide):
    recipe = guide["recipe"]
    printed = guide["objects"]
    scene = bpy.context.scene

    # Before you start: a new file without its Cube, Light and Camera (and their collection).
    old = bpy.data.collections["Collection"]
    for ob in list(old.objects):
        bpy.data.objects.remove(ob)
    bpy.data.collections.remove(old)

    # A collection UI with every other collection in it (the chapters say so, and the generator's
    # check_collections that the example has them so).
    collections = {"Scene Collection": scene.collection}
    ui = bpy.data.collections.new("UI")
    scene.collection.children.link(ui)
    collections["UI"] = ui
    named = {home for place in printed.values() for home in place["homes"]}
    for name in sorted(named - {"Scene Collection", "UI"}):
        collection = bpy.data.collections.new(name)
        ui.children.link(collection)
        collections[name] = collection

    # The shapes the guide leaves free (any mesh, items, pickups): the example's, so the
    # comparison can see the rest.
    shapes = sorted({entry["geometry"]["mesh"] for entry in recipe["objects"].values()
                     if "mesh" in entry.get("geometry", {})})
    with bpy.data.libraries.load(EXAMPLE, link=False) as (data_from, data_to):
        # A copy: the loader puts the data-blocks in place of the names.
        data_to.meshes = list(shapes)
    shape_meshes = dict(zip(shapes, data_to.meshes))
    for mesh in shape_meshes.values():
        mesh.materials.clear()
    # The materials that came with them: the creator makes their own.
    for material in list(bpy.data.materials):
        if material.users == 0:
            bpy.data.materials.remove(material)

    objects = {}
    for name, place in printed.items():
        # What kind of object a row is: the reader sees it in its shape.
        entry = recipe["objects"].get(name)
        if entry is None:
            raise RuntimeError(f"the page prints {name}, which the example does not have")
        kind = entry["type"]
        shape = None if place.get("free") else printed_shape(place, kind)
        ob = add_object(kind, place)
        ob.name = name
        if kind == 'MESH':
            geometry = entry["geometry"]
            old_mesh = ob.data
            if shape is not None:
                points, faces = shape
                mesh = bpy.data.meshes.new(name)
                mesh.from_pydata([tuple(point) for point in points], [], faces)
                mesh.update()
            elif "mesh" in geometry and (place.get("free") or place["shape"].startswith("Mesh ")):
                mesh = shape_meshes[geometry["mesh"]]
            elif place.get("free"):
                mesh = bpy.data.meshes.new(name)
                mesh.from_pydata([tuple(v) for v in geometry["verts"]], [], geometry["faces"])
                mesh.update()
            else:
                raise RuntimeError(f"the page gives no shape for {name}: {place['shape']!r}")
            ob.data = mesh
            bpy.data.meshes.remove(old_mesh)
        if "location" in place:
            rotation = place.get("rotated", Vector((0.0, 0.0, 0.0)))
            scale = place.get("scale", Vector((1.0, 1.0, 1.0)))
            ob.matrix_basis = (
                Matrix.Translation(place["location"]) @
                Euler([math.radians(angle) for angle in rotation]).to_matrix().to_4x4() @
                Matrix.Diagonal(tuple(scale) + (1.0,)))
        else:
            # A library item: its place does not matter (the example's, to compare the rest).
            ob.matrix_basis = Matrix(entry["world"])
        homes = place["homes"]
        for home in homes:
            if ob.name not in collections[home].objects:
                collections[home].objects.link(ob)
        for collection in list(ob.users_collection):
            if collection.name not in homes and collection != collections.get(homes[0]):
                collection.objects.unlink(ob)
        objects[name] = ob

    materials = {}

    def material(name):
        if name in materials:
            return materials[name]
        holder = next(ob for ob in objects.values() if ob.type == 'MESH')
        before = set(bpy.data.materials)
        with bpy.context.temp_override(object=holder, active_object=holder):
            bpy.ops.material.new()
        made = (set(bpy.data.materials) - before).pop()
        made.name = name
        bsdf = next(node for node in made.node_tree.nodes if node.type == 'BSDF_PRINCIPLED')
        inputs = {socket.name: socket for socket in bsdf.inputs}
        for label, text in guide["materials"][name].items():
            if label in inputs:
                value = text.strip("()").split(",")
                value = [float(v) for v in value]
                if len(value) == 3 and len(inputs[label].default_value) == 4:
                    value.append(1.0)
                inputs[label].default_value = value if len(value) > 1 else value[0]
            else:
                prop = rna_named(bpy.types.Material.bl_rna, label)
                R.apply_settings(made, {prop.identifier: parse_value(prop, text)}, resolve)
        materials[name] = made
        return made

    for name, place in printed.items():
        ob = objects[name]
        if place["parent"]:
            # Ctrl P > Object (Without Inverse), then the Location typed in.
            location = ob.matrix_basis.copy()
            ob.parent = objects[place["parent"]]
            ob.matrix_parent_inverse = Matrix.Identity(4)
            ob.matrix_basis = location
        R.apply_settings(ob, place["object"], resolve)
        # The Physics Type the row (or the line above its table) prints, else the Add menu's.
        physics = place["physics"] or place.get("shared", {}).get("physics")
        if physics is not None:
            ob.game.physics_type = physics
        R.apply_settings(ob.game, place["game"], resolve)
        R.apply_settings(ob.game.ui, place["ui"], resolve)
        if ob.type == 'FONT':
            R.apply_settings(ob.data, place["text"], resolve)
            ob.data.body = place.get("body", "")
        elif ob.type in {'CAMERA', 'LIGHT'}:
            R.apply_settings(ob.data, place["data"], resolve)
        for prop in place["properties"]:
            R.add_game_property(ob, prop, resolve)
        names = place["materials"]
        if names == "shared":
            # The row names none: the material the line above the table gives its kind.
            shared = place["shared"]
            names = [shared[ob.type]] if ob.type in shared else []
        for material_name in names:
            ob.data.materials.append(material(material_name))

    # Scene settings: the table's rows, by where they are.
    game = scene.game_settings
    for where, value in guide["scene"]:
        if where == "Scene tab › Scene › Camera":
            scene.camera = objects[value]
        elif where == "Scene tab › Items and UI › Item Library":
            game.item_library = bpy.data.collections[value]
        elif where == "Render tab › Game Debug › Mouse Cursor":
            game.show_mouse = {"on": True, "off": False}[value]
        elif where.startswith("Output tab › Format › "):
            prop = rna_named(scene.render.bl_rna, where.rsplit(" › ", 1)[1])
            setattr(scene.render, prop.identifier, parse_value(prop, value))
        elif where.startswith("World tab › "):
            prop = rna_named(scene.world.bl_rna, where.rsplit(" › ", 1)[1])
            R.apply_settings(scene.world, {prop.identifier: parse_value(prop, value)}, resolve)
        else:
            raise RuntimeError(f"the test cannot set {where!r}")
    for entry in guide["actions"]:
        R.input_action(scene, entry)

    # The trees of the pictures (check_pictures compared them with the example's), applied to
    # the objects the rows name; the tones the Menu sounds chapter says to append come with them.
    remap = {(kind, item.name): item for kind in ("objects", "collections")
             for item in getattr(bpy.data, kind)}
    builder = R.Builder(recipe, remap=remap)
    for tree_name in recipe["trees"]:
        builder.tree(tree_name)
    if builder.missing:
        raise RuntimeError(f"the trees name what the guide never made: {builder.missing}")
    for name, place in printed.items():
        for binding in place["bindings"]:
            new = objects[name].game.logic_node_binding_new()
            new.tree = bpy.data.node_groups[binding["tree"]]
            new.enabled = binding["enabled"]

    # Finish: UI disabled in viewports.
    ui.hide_viewport = True


# -----------------------------------------------------------------------------
# What matters in the game


def enc(value):
    if value is None:
        return None
    if isinstance(value, bpy.types.ID):
        return f"<{type(value).__name__} {value.name}>"
    if isinstance(value, float):
        return round(value, 4)
    if isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, (set, frozenset)):
        return sorted(value)
    try:
        return [enc(item) for item in value]
    except TypeError:
        return str(value)


def settings(struct, skip=()):
    values = {}
    for key, value in R._settings(struct, skip=skip).items():
        values[key] = enc(value) if not isinstance(value, dict) else f"<{value['name']}>"
    return values


def game_property(prop):
    entry = {"name": prop.name, "type": prop.type, "save": prop.use_save_game}
    if prop.type == 'CHOICE':
        entry["options"] = [option.name for option in prop.options]
        entry["value"] = prop.value
    elif prop.type == 'LIST':
        entry["item_type"] = prop.item_type
        entry["items"] = [enc(element.value) for element in prop.elements]
    elif prop.type == 'TABLE':
        entry["columns"] = [(column.name, column.type, enc(column.value))
                            for column in prop.columns]
        entry["rows"] = [[enc(cell.value) for cell in row.cells] for row in prop.rows]
    else:
        entry["value"] = enc(prop.value)
    return entry


# What the comparison leaves out, written down here on its own (not taken from the guide's
# generator, so a setting the generator wrongly hides is still compared).
# Object settings only the 3D view uses.
VIEW_ONLY_SETTINGS = {"show_name", "show_in_front", "empty_display_size", "empty_display_type",
                      "display_type", "color", "hide_select"}
# A material's Viewport Display settings.
MATERIAL_VIEW_ONLY = {"diffuse_color", "metallic", "roughness", "specular_intensity",
                      "specular_color"}
# An object's place, data and links (compared on their own).
NOT_SETTINGS = {
    "location", "rotation_euler", "rotation_quaternion", "rotation_axis_angle", "scale",
    "delta_location", "delta_rotation_euler", "delta_rotation_quaternion", "delta_scale",
    "dimensions", "matrix_world", "matrix_local", "matrix_basis", "matrix_parent_inverse",
    "parent", "data", "active_material", "active_material_index", "mode", "game", "type",
    "parent_type", "parent_bone", "parent_vertices", "bound_box", "active_shape_key_index",
}
WIDGET_SETTINGS = {"widget_object", "widget_property", "widget_min", "widget_max", "widget_step",
                   "widget_part", "widget_max_property"}
# The numbers each widget reads (LN_UIService): a Toggle none, a Selector of a Choice none.
WIDGET_NUMBERS_READ = {
    'SLIDER': {"widget_min", "widget_max", "widget_step"},
    'BAR': {"widget_min", "widget_max", "widget_max_property"},
    'TEXT': {"widget_max"},
    'TOGGLE': set(),
}


def ui_settings(ob):
    """The UI Element settings that do something."""
    ui = ob.game.ui
    if not ui.use_element:
        return {}
    values = settings(ui)
    numbers = {"widget_min", "widget_max", "widget_step", "widget_max_property"}
    if ui.widget == 'NONE':
        drop = WIDGET_SETTINGS
    elif ui.widget == 'SELECTOR':
        holder = ui.widget_object or ob
        while holder is not None and ui.widget_property not in holder.game.properties:
            holder = holder.parent
        prop = holder.game.properties.get(ui.widget_property) if holder else None
        drop = numbers if prop is not None and prop.type == 'CHOICE' else set()
    else:
        drop = numbers - WIDGET_NUMBERS_READ[ui.widget]
    return {key: value for key, value in values.items() if key not in drop}


def fingerprint(scene):
    objects = {}
    used_materials = set()
    for ob in scene.objects:
        world = R.world_matrix(ob)
        entry = {
            "type": ob.type,
            "parent": ob.parent.name if ob.parent else None,
            "collections": sorted(collection.name for collection in ob.users_collection),
            "world": [[round(value, 4) for value in row] for row in world],
            # Every object setting but those only the 3D view uses.
            "settings": {key: value for key, value in
                         settings(ob, skip=NOT_SETTINGS).items()
                         if key not in VIEW_ONLY_SETTINGS},
            "game": settings(ob.game, skip=R.GAME_SKIP),
            "ui": ui_settings(ob),
            # A text's Text property equal to its body changes nothing (the guide leaves it out).
            # In any order: the game finds them by name.
            "properties": sorted((game_property(prop) for prop in ob.game.properties
                                  if not (ob.type == 'FONT' and prop.name == "Text" and
                                          prop.type == 'STRING' and
                                          prop.value in ("", ob.data.body))),
                                 key=lambda entry: entry["name"]),
            "bindings": [(binding.tree.name if binding.tree else None, binding.enabled)
                         for binding in ob.game.logic_node_bindings],
            "materials": [slot.material.name if slot.material else None
                          for slot in ob.material_slots],
        }
        used_materials.update(slot.material.name for slot in ob.material_slots if slot.material)
        if ob.type == 'MESH':
            entry["points"] = sorted([round(value, 3) for value in world @ vertex.co]
                                     for vertex in ob.data.vertices)
            entry["faces"] = len(ob.data.polygons)
            # Which way each face faces (by where it is: a box's normals alone are the same
            # inside out): a triangle mesh collides on its front side only.
            turn = world.to_3x3()
            entry["normals"] = sorted(
                [[round(value, 3) for value in world @ polygon.center],
                 [round(value, 3) for value in (turn @ polygon.normal).normalized()]]
                for polygon in ob.data.polygons)
        elif ob.type == 'FONT':
            entry["text"] = settings(ob.data, skip=R.TEXT_SKIP)
            entry["body"] = ob.data.body
        elif ob.type in {'CAMERA', 'LIGHT'}:
            entry["data"] = settings(ob.data)
        objects[ob.name] = entry
    materials = {}
    for name in sorted(used_materials):
        material = bpy.data.materials[name]
        materials[name] = {"bsdf": {key: enc(value)
                                    for key, value in G._bsdf_values(material).items()},
                           "settings": settings(material, skip=MATERIAL_VIEW_ONLY)}
    trees = {}
    for tree in bpy.data.node_groups:
        if tree.bl_idname != "LogicNodeTree":
            continue
        nodes = {}
        for node in tree.nodes:
            if node.bl_idname == "NodeFrame":
                nodes[node.name] = {"frame": node.label}
                continue
            nodes[node.name] = {
                "idname": node.bl_idname,
                "frame": node.parent.label if node.parent else None,
                "settings": settings(node, skip=R.NODE_SKIP),
                "inputs": {socket.identifier: enc(getattr(socket, "default_value", None))
                           for socket in node.inputs if socket.enabled and not socket.is_linked},
                "mute": node.mute,
            }
        trees[tree.name] = {
            "nodes": nodes,
            "links": sorted((link.from_node.name, link.from_socket.identifier, link.to_node.name,
                             link.to_socket.identifier) for link in tree.links),
        }
    game = scene.game_settings
    layers = {}

    def walk(layer):
        for child in layer.children:
            layers[child.name] = (child.exclude, child.hide_viewport, child.holdout,
                                  child.indirect_only)
            walk(child)

    walk(scene.view_layers[0].layer_collection)
    return {
        "objects": objects,
        "materials": materials,
        "trees": trees,
        "scene": {
            "game": settings(game, skip=R.SCENE_GAME_SKIP),
            "camera": scene.camera.name if scene.camera else None,
            "actions": [(settings(action, skip={"active_binding_index"}),
                         [settings(binding) for binding in action.bindings])
                        for action in game.input_actions],
            "collections": sorted(
                (collection.name, collection.hide_viewport, collection.hide_render,
                 sorted(ob.name for ob in collection.objects),
                 sorted(child.name for child in collection.children))
                for collection in scene.collection.children_recursive),
            "layers": layers,
            "world": settings(scene.world) if scene.world else None,
            "render": [scene.render.resolution_x, scene.render.resolution_y,
                       scene.render.resolution_percentage, scene.render.engine],
        },
    }


def same(a, b):
    """Equal, numbers within the rounding of the recipe and the fingerprint."""
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and \
            not isinstance(a, bool) and not isinstance(b, bool):
        return abs(a - b) <= 1.5e-3
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return a == b


def compare(a, b, path="", out=None):
    out = [] if out is None else out
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b), key=str):
            if key not in a or key not in b:
                out.append(f"{path}/{key}: only in the {'example' if key in a else 'rebuild'}")
            else:
                compare(a[key], b[key], f"{path}/{key}", out)
    elif not same(a, b):
        out.append(f"{path}: example {a!r}, rebuild {b!r}")
    return out


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", default="")
    options = parser.parse_args(args)
    if bpy.data.filepath:
        raise RuntimeError("start Blender with --factory-startup and no file")
    guide, example = read_guide()
    bpy.ops.wm.read_homefile(use_factory_startup=True, load_ui=False)
    build(guide)
    rebuilt = fingerprint(bpy.context.scene)
    if options.output_dir:
        os.makedirs(options.output_dir, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(
            filepath=os.path.join(options.output_dir, "game_ui_guide_rebuilt.blend"))
    differences = compare(example, rebuilt)
    for line in differences[:200]:
        print("DIFFERENT", line)
    if differences:
        raise RuntimeError(f"{len(differences)} differences between the example and the file "
                           "built from the guide")
    print(f"GUIDE REBUILD OK: {len(rebuilt['objects'])} objects, {len(rebuilt['trees'])} trees, "
          f"{len(rebuilt['materials'])} materials")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
        sys.stdout.flush()
        os._exit(1)
    sys.stdout.flush()
    os._exit(0)
