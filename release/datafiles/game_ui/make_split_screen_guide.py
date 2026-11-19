# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Write the Split-Screen Guide (split_screen_guide.html) from the Split-Screen FPS Example
(split_screen_example.blend).

  blender --background --factory-startup --python make_split_screen_guide.py -- \\
      [--check] [SHOTS_DIR] [OUTPUT.html]

SHOTS_DIR holds the pictures split_screen_screenshots.py takes (split.png, inventory.png, ...).
OUTPUT defaults to split_screen_guide.html next to this script. --check writes nothing: it only
stops with an error where the guide and the example differ. Run it again whenever the example
changes.

Everything the guide shows of the example comes from the file: the node pictures, the tables of
objects, materials, actions and settings. The words around them are here, and every name, node,
setting and value the words mark is checked against the file:
  <n>Title</n>   a node of the frame the words explain (as its header shows it)
  <s>Label</s>   a socket or setting of a node of that frame, or a UI Element, physics, camera
                 or scene setting (outside the frames: of any node too)
  <S>Label</S>   a socket or setting of any node of the frame's tree (another frame's)
  <v>value</v>   a value a node of that frame shows (a number, a text, a choice)
  <c>name</c>    a name in the example: an object, collection, tree, property, action, sound...
The script stops when the example has a logic frame, an object, a material, a collection, an
input action, an action, a sound or a picture this guide does not explain, so the guide cannot
fall behind the example unnoticed.
"""

import base64
import collections
import hashlib
import os
import re
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
# Leave no __pycache__ next to the installed data files.
sys.dont_write_bytecode = True

import make_game_ui_guide as G  # noqa: E402 (the UI guide's node pictures and tables)
from _bpy_internal.game_ui import recipe as R  # noqa: E402

EXAMPLE_FILE = "split_screen_example.blend"
GUIDE_FILE = "split_screen_guide.html"

esc = G.esc
num = G.num
vec = G.vec
enum_label = G.enum_label
prop_label = G.prop_label
setting_value = G.setting_value
node_title = G.node_title


# -----------------------------------------------------------------------------
# Reading the example


def read_recipe():
    """The example as the UI guide's recipe reads it, without the recipe's limits for what the
    templates can build: node materials and actions are only shown here, never built."""
    saved = R._only_ids, R._material, R._ID_COLLECTIONS
    R._only_ids = lambda *_args: None
    R._material = lambda _material, _fresh: {}
    R._ID_COLLECTIONS = R._ID_COLLECTIONS + ((bpy.types.Action, "actions"),)
    try:
        return R.extract(bpy.data, bpy.context.scene)
    finally:
        R._only_ids, R._material, R._ID_COLLECTIONS = saved


# Physics settings the tables name (the UI guide's, and the axis locks), as the Physics tab draws
# them.
G.GAME_SETTING_LABELS.update({
    "lock_rotation_x": "Lock Rotation X",
    "lock_rotation_y": "Lock Rotation Y",
    "lock_rotation_z": "Lock Rotation Z",
})
# The players' game properties have a table of their own (both players side by side).
G.PROPERTIES_IN_A_TABLE = {"Player", "Player 1"}
# The UI Element's Tooltip sub-panel (the UI example's tables have no tooltips).
TOOLTIP_KEYS = ("tooltip_object", "tooltip_text", "tooltip_placement", "tooltip_offset")
G.UI_GROUPS = G.UI_GROUPS + (("Tooltip", "Tooltip", TOOLTIP_KEYS),)
# The player's number feeds every frame of the Game tree: its stub sums the ends up.
G.STUB_ENDS_SHOWN = 3
# A node far below the others in the file is drawn closer.
G.ROW_GAP = 70
# Camera settings that only matter while the camera draws with Workbench.
WORKBENCH_ONLY = {"workbench_color", "workbench_lighting", "workbench_cavity",
                  "workbench_smooth_edges", "workbench_outline", "workbench_color_single"}

# Where the scene settings the example changes are (the generator stops on another).
SCENE_SETTING_PLACES = {
    "item_library": "Scene tab › Items and UI › Item Library",
    "use_input_gamepads_from_player_1": "Scene tab › Game Input Map › Settings › Gamepads Start "
                                        "at Player 1",
    "resolution_x": "Render tab › Game Resolution › Standalone Start X",
    "resolution_y": "Render tab › Game Resolution › Standalone Start Y",
    "show_fullscreen": "Render tab › Game Resolution › Fullscreen",
    "use_desktop": "Render tab › Game Resolution › Desktop",
}


class Demo(G.Example):
    """The example as the guide needs it (the UI guide's Example, read without its build
    rules)."""

    def __init__(self, shots_dir, start):
        self.scene = bpy.context.scene
        self.start = start
        self.recipe = read_recipe()
        self.view = G.guide_view(self.recipe, start)
        for name, entry in self.view["objects"].items():
            # The Tooltip panel greys its settings out without a Tooltip Object (and the offset
            # unless Placement is Beside): they do nothing there.
            ui = entry["ui"]
            if bpy.data.objects[name].game.ui.tooltip_object is None:
                for key in TOOLTIP_KEYS:
                    ui.pop(key, None)
            elif bpy.data.objects[name].game.ui.tooltip_placement != 'BESIDE':
                ui.pop("tooltip_offset", None)
        self.shots_dir = shots_dir
        self.homes = {}
        for entry in self.recipe["collections"]:
            for name in entry["objects"]:
                self.homes.setdefault(name, []).append(entry["name"] or "Scene Collection")
        self.shown_objects = set()
        self.shown_frames = set()
        self.shown_materials = set()
        self.shown_actions = set()
        self.noted_breaks = set()
        self.check_trees()
        self.words = Words(self)

    # Checks -------------------------------------------------------------

    def check_trees(self):
        """Stop when a node picture would not show all a reader needs: a muted or labelled node,
        a setting or a value the picture does not draw, a tree with nodes both in and outside
        frames."""
        for tree_name, entry in self.recipe["trees"].items():
            tree = bpy.data.node_groups[tree_name]
            framed = {spec["frame"] is not None for spec in entry["nodes"]}
            if len(framed) > 1:
                raise RuntimeError(f"tree {tree_name} has nodes in frames and outside them")
            for spec in entry["nodes"]:
                node = tree.nodes[spec["name"]]
                where = f"tree {tree_name}, node {node.name}"
                if spec["mute"]:
                    raise RuntimeError(f"{where} is muted")
                if node.label:
                    raise RuntimeError(f"{where} has a label")
                drawn = {prop for prop, _label, _expanded in G.node_draw_list(node) or ()}
                for (idname, _identifier), (_label, props) in G.SOCKET_DRAWS.items():
                    if idname == G.works_as(node):
                        drawn |= set(props)
                for key in spec["settings"]:
                    if key not in drawn:
                        raise RuntimeError(f"{where}: the picture does not show its {key}")
                for identifier in spec["inputs"]:
                    socket = next(s for s in node.inputs if s.identifier == identifier)
                    if socket.enabled and not socket.is_linked and socket.hide:
                        raise RuntimeError(f"{where}: input {identifier} is hidden but set")

    # Objects ----------------------------------------------------------------

    def what(self, name):
        """What an object is, in a few words: its kind and size (world size), or its text."""
        entry = self.view["objects"][name]
        ob = bpy.data.objects[name]
        if entry["type"] == 'MESH':
            low, high = self.bounds(name)
            size = high - low
            words = f"Mesh {num(size.x)} × {num(size.y)} × {num(size.z)}"
            if "verts" in entry.get("geometry", {}):
                count = len(entry["geometry"]["verts"])
                words = (f"Plane {num(max(size.x, size.y))} × "
                         f"{num(max(min(size.x, size.y), size.z))}" if count == 4 else
                         f"Box {num(size.x)} × {num(size.y)} × {num(size.z)}" if count == 8 else
                         words)
            return words + self.turned(name)
        if entry["type"] == 'FONT':
            text = dict(entry["text"])
            text.pop("body")
            words = ["Text"]
            if "size" in text:
                words.append(f"Size {num(text.pop('size'))}")
            for key, label in (("align_x", "Horizontal"), ("align_y", "Vertical")):
                if key in text:
                    words.append(f"{label} "
                                 f"{enum_label(bpy.types.TextCurve.bl_rna, key, text.pop(key))}")
            for key, value in sorted(text.items()):
                words.append(f"{prop_label(bpy.types.TextCurve.bl_rna, key)} "
                             f"{setting_value(bpy.types.TextCurve.bl_rna, key, value)}")
            return ", ".join(words) + self.turned(name)
        if entry["type"] == 'EMPTY':
            return "Empty" + self.turned(name)
        if entry["type"] == 'CAMERA':
            camera = ob.data
            words = (f"Camera, orthographic, Orthographic Scale {num(camera.ortho_scale)}"
                     if camera.type == 'ORTHO' else f"Camera, Focal Length {num(camera.lens)} mm")
            extra = {k: v for k, v in entry["data"].items()
                     if k not in {"type", "ortho_scale", "lens"} | R.CAMERA_DERIVED and
                     (camera.use_workbench or k not in WORKBENCH_ONLY)}
            if extra:
                words += ", " + ", ".join(
                    f"{prop_label(bpy.types.Camera.bl_rna, k)} "
                    f"{setting_value(bpy.types.Camera.bl_rna, k, v)}" for k, v in extra.items())
            return words + self.turned(name)
        if entry["type"] == 'LIGHT':
            words = f"{entry['light_type'].title()} light"
            if entry["data"]:
                words += ", " + ", ".join(
                    f"{prop_label(bpy.types.Light.bl_rna, k)} "
                    f"{setting_value(bpy.types.Light.bl_rna, k, v)}"
                    for k, v in entry["data"].items())
            return words
        return entry["type"].title()

    def turned(self, name):
        from mathutils import Matrix
        import math
        world = Matrix(self.recipe["objects"][name]["world"])
        words = []
        rotation = world.to_euler()
        if any(abs(angle) > 1e-4 for angle in rotation):
            words.append("turned " + vec([math.degrees(angle) for angle in rotation]) + "°")
        scale = world.to_scale()
        if any(abs(value - 1.0) > 1e-4 for value in scale):
            words.append("scale " + vec(scale))
        return (", " + ", ".join(words)) if words else ""

    def part_of(self, name):
        """What an object is to a UI Element, from the element's settings: its Highlight
        Object, a widget's part, a Tooltip Object."""
        target = bpy.data.objects[name]
        words = []
        for ob in bpy.data.objects:
            ui = ob.game.ui
            if not ui.use_element:
                continue
            if ui.highlight_object == target:
                words.append(f"shown while {ob.name} is highlighted (its Highlight Object)")
            if ui.widget != 'NONE' and ui.widget_part == target:
                part = G.WIDGET_PART_LABEL.get(ui.widget, "Part")
                words.append(f"the {part} of {ob.name}'s widget")
            if ui.tooltip_object == target:
                words.append(f"{ob.name}'s Tooltip Object")
        tooltips = [text for text in words if text.endswith("Tooltip Object")]
        if len(tooltips) > 1:
            owners = ", ".join(text[:-len("'s Tooltip Object")] for text in tooltips)
            words = [text for text in words if text not in tooltips]
            words.append(f"the Tooltip Object of {owners}")
        return words

    def settings_cell(self, name, shared, batch):
        # Every object is in the file already: never “made later”.
        return super().settings_cell(name, shared, set(self.recipe["objects"]))

    def objects_table(self, names, purposes=None, summary=None):
        """The objects, children under their parent, with what most of them share said once
        above, folded under `summary`."""
        purposes = purposes or {}
        names = list(names)
        wanted = set(names)
        for name in names:
            if name not in self.recipe["objects"]:
                raise RuntimeError(f"a table names {name}, which the example does not have")
            if name in self.shown_objects:
                raise RuntimeError(f"{name} is in two tables")
        for name in purposes:
            if name not in wanted:
                raise RuntimeError(f"a purpose for {name}, which is not in its table")
        shared = self._defaults(names)
        rows = []

        def walk(name, depth):
            self.shown_objects.add(name)
            entry = self.view["objects"][name]
            notes = [self.words.render(purposes[name])] if name in purposes else []
            if not notes:
                notes.extend(esc(text) for text in self.part_of(name))
            if depth == 0 and entry["parent"] and "parent" not in shared:
                notes.append(f"child of {esc(entry['parent'])}")
            if "homes" not in shared:
                notes.append("in " + esc(", ".join(self.homes.get(name, []))))
            note = "".join(f'<span class="what">{text}</span>' for text in notes)
            rows.append(
                f'<tr data-object="{esc(name)}">'
                f'<td class="ob" style="padding-left:{8 + 18 * depth}px">'
                f'{"└ " if depth else ""}<b>{esc(name)}</b>{note}</td>'
                f'<td>{esc(self.what(name))}</td>'
                f'<td>{self.settings_cell(name, shared, wanted)}</td></tr>')
            for child in sorted((child for child in names
                                 if self.recipe["objects"][child]["parent"] == name),
                                key=names.index):
                walk(child, depth + 1)

        for name in [name for name in names
                     if self.recipe["objects"][name]["parent"] not in wanted]:
            walk(name, 0)
        table = (self._shared_line(shared) +
                 '<div class="scroll"><table class="objects"><thead><tr><th>Object</th>'
                 '<th>What it is</th><th>Settings</th></tr></thead><tbody>' + "".join(rows) +
                 "</tbody></table></div>" + self.materials_for(names))
        if summary is None:
            summary = f"The {len(names)} objects"
        return f'<details class="objects"><summary>{summary}</summary>{table}</details>'

    # Materials --------------------------------------------------------------

    @staticmethod
    def _viewport_swatch(color):
        rgb = ",".join(str(int(round(max(0.0, min(1.0, c)) ** (1 / 2.2) * 255)))
                       for c in color[:3])
        return f'<span class="swatch" style="background:rgb({rgb})"></span>'

    @staticmethod
    def node_material_words(material):
        """A material's nodes as words: each link, with the values set on the nodes it passes."""
        tree = material.node_tree
        words = []
        for link in tree.links:
            if link.to_node.type == 'OUTPUT_MATERIAL':
                continue
            source = link.from_node
            what = source.bl_label
            if source.type == 'MATH':
                values = ", ".join(num(socket.default_value) for socket in source.inputs
                                   if not socket.is_linked and socket.enabled)
                what = f"Math ({enum_label(source.bl_rna, 'operation', source.operation)}, " \
                       f"{values})"
            elif source.type == 'TEX_IMAGE':
                what = f"Image Texture ({source.image.name if source.image else 'no image'})"
            words.append(f"{esc(what)} › {esc(link.from_socket.name)} → "
                         f"{esc(link.to_node.bl_label)} › {esc(link.to_socket.name)}")
        return "; ".join(words)

    def materials_for(self, names):
        """The materials these objects use that no earlier table showed: the Viewport Display
        color (what the Workbench cameras show: the menus, the HUD, the minimaps) and the
        surface the players' cameras (EEVEE) show."""
        rows = []
        for ob_name in names:
            for name in self.recipe["objects"][ob_name]["materials"]:
                if not name or name in self.shown_materials:
                    continue
                self.shown_materials.add(name)
                material = bpy.data.materials[name]
                kinds = sorted(node.type for node in material.node_tree.nodes)
                if kinds == ['BSDF_PRINCIPLED', 'OUTPUT_MATERIAL']:
                    labels = {socket.identifier: socket.name for socket in self._bsdf(name).inputs}
                    values = G.differs(G._bsdf_values(material), self.start["material"])
                    words = []
                    base, emission = values.get("Base Color"), values.get("Emission Color")
                    if base is not None and emission is not None and R._equal(base, emission):
                        words.append("Base Color = Emission Color "
                                     f"<b>{esc(G.value_text(base))}</b>")
                        values.pop("Base Color")
                        values.pop("Emission Color")
                    for key, value in values.items():
                        words.append(f"{esc(labels.get(key, key))} "
                                     f"<b>{esc(G.value_text(value))}</b>")
                    surface = "Principled BSDF: " + (", ".join(words) or "as a new material")
                else:
                    surface = "Nodes: " + self.node_material_words(material)
                settings = G.differs(R._settings(material, skip=G.MATERIAL_VIEWPORT_ONLY),
                                     self.start["material_settings"])
                # An animation data block without an action (left by playing an action in the
                # editor) changes nothing.
                if material.animation_data is None or material.animation_data.action is None:
                    settings = {k: v for k, v in settings.items()
                                if not k.startswith("animation_data")}
                extra = ", ".join(
                    f"{esc(prop_label(bpy.types.Material.bl_rna, key))} "
                    f"<b>{esc(setting_value(bpy.types.Material.bl_rna, key, value))}</b>"
                    for key, value in settings.items())
                if extra:
                    surface += "; " + extra
                color = tuple(material.diffuse_color)
                rows.append(f"<tr><td><b>{esc(name)}</b></td>"
                            f"<td>{self._viewport_swatch(color)}"
                            f"{esc(G.value_text(R.encode(color)))}</td>"
                            f"<td>{surface}</td></tr>")
        if not rows:
            return ""
        return ('<table class="mats"><thead><tr><th>Material</th><th>Viewport Display › Color '
                '(Workbench cameras)</th><th>Surface (the players’ cameras, EEVEE)</th></tr>'
                '</thead><tbody>' + "".join(rows) + "</tbody></table>")

    # Logic ------------------------------------------------------------------

    def frame_block(self, tree_name, title):
        """A frame's picture (title None: a tree without frames, drawn whole) and its words."""
        tree = bpy.data.node_groups[tree_name]
        frame = None
        if title is not None:
            frame = next(node for node in tree.nodes
                         if node.bl_idname == "NodeFrame" and node.label == title)
        if (tree_name, title) in self.shown_frames:
            raise RuntimeError(f"frame {tree_name} › {title} is shown twice")
        self.shown_frames.add((tree_name, title))
        nodes = [node for node in tree.nodes if node.parent == frame and
                 node.bl_idname != "NodeFrame"]
        picture, cut = G.frame_svg(tree, frame)
        notes = []
        if cut["names"]:
            notes.append('<p class="note">Full names: ' + "; ".join(
                f"{esc(shown)} is <b>{esc(full)}</b>" for shown, full in cut["names"].items()) +
                ".</p>")
        long_values = [f"{esc(node_title(node))} › {esc(name)}: <code>{esc(value)}</code>"
                       for node, name, value in cut["values"]]
        if long_values:
            notes.append('<p class="note">In full: ' + "; ".join(long_values) + ".</p>")
        to_open = [f"{esc(node_title(node))} › <b>{esc(name)}</b>" for node in nodes
                   for side, item in G.node_layout(node) if side == "panel"
                   for name, _header, shown_open, closed in [item] if shown_open and closed]
        if to_open:
            notes.append('<p class="note">Folded sections, drawn open here because something in '
                         'them is set (click the name to open one): ' + "; ".join(to_open) +
                         ".</p>")
        key = (tree_name, title)
        if key not in FRAMES:
            raise RuntimeError(f"no words for frame {tree_name} › {title!r}")
        bullets = "".join(f"<li>{self.words.render(line, tree, frame)}</li>"
                          for line in FRAMES[key])
        caption = esc(title) if title is not None else "(the whole tree: it has no frames)"
        users = [ob.name for ob in bpy.data.objects
                 for binding in ob.game.logic_node_bindings if binding.tree == tree]
        return (f'<figure class="frame"><figcaption><span class="tree">{esc(tree_name)}</span> '
                f'{caption}<span class="on">on {esc(", ".join(users))}</span></figcaption>'
                f'<div class="canvas">{picture}</div>'
                f'<ul class="how">{bullets}</ul>{"".join(notes)}</figure>')

    def frames(self, tree_name, *titles):
        return "".join(self.frame_block(tree_name, title) for title in titles)

    # Pictures ---------------------------------------------------------------

    def shot(self, name, caption, width=1280):
        """A screenshot of the game, scaled to `width`."""
        path = os.path.join(self.shots_dir, name + ".png") if self.shots_dir else ""
        if not path or not os.path.exists(path):
            return (f'<p class="missing">(Picture {esc(name)} not made: run '
                    'split_screen_screenshots.py)</p>')
        image = bpy.data.images.load(path)
        source_w, source_h = image.size
        scale = min(width / source_w, 1.0)
        image.scale(max(1, int(source_w * scale)), max(1, int(source_h * scale)))
        scaled_w, scaled_h = image.size
        jpeg = os.path.join(bpy.app.tempdir, "split_screen_guide_shot.jpg")
        image.filepath_raw = jpeg
        image.file_format = 'JPEG'
        image.save(filepath=jpeg, quality=86)
        bpy.data.images.remove(image)
        data = base64.b64encode(open(jpeg, "rb").read()).decode("ascii")
        os.remove(jpeg)
        return (f'<figure class="shot"><div class="shot-box"><img alt="{esc(caption)}" '
                f'src="data:image/jpeg;base64,{data}" width="{scaled_w}" height="{scaled_h}">'
                f'</div><figcaption>{self.words.render(caption)}</figcaption></figure>')

    def window_diagram(self):
        """The window as the Split Screen and Minimap trees lay it out, from their nodes."""
        split = node_of(bpy.data.node_groups["Split Screen"], "LogicNativeSetSplitScreen")
        viewport = node_of(bpy.data.node_groups["Minimap"], "LogicNativeSetCameraViewport")
        two = enum_label(split.bl_rna, "split_two_players", split.split_two_players)
        if two != "Side by Side":
            raise RuntimeError("the window picture draws two players side by side")
        area = enum_label(viewport.bl_rna, "viewport_area", viewport.viewport_area)
        if area != "Player's View":
            raise RuntimeError("the window picture draws the minimap in each player's view")
        value = lambda node, name: node.inputs[name].default_value
        divider = value(split, "Divider")
        left, top = value(viewport, "Left"), value(viewport, "Top")
        width, height = value(viewport, "Width"), value(viewport, "Height")
        res_x, res_y = self.scene.render.resolution_x, self.scene.render.resolution_y
        scale = 640 / res_x
        w, h = res_x * scale, res_y * scale
        half = (w - divider * scale * 4) / 2  # the divider drawn four times wider, to be seen
        gap = w - 2 * half
        parts = []
        for index, (x0, user) in enumerate(((0, 0), (half + gap, 1))):
            camera = bpy.data.objects[f"Minimap {user}"].data
            mx, my = x0 + left * half, top * h
            mw, mh = width * half, height * h
            if camera.viewport_shape == 'CIRCLE':
                # Circle: the oval filling the viewport's rectangle.
                shape = (f'<ellipse cx="{mx + mw / 2:.1f}" cy="{my + mh / 2:.1f}" '
                         f'rx="{mw / 2:.1f}" ry="{mh / 2:.1f}" class="d-mini"/>')
            else:
                shape = (f'<rect x="{mx:.1f}" y="{my:.1f}" width="{mw:.1f}" height="{mh:.1f}" '
                         'class="d-mini"/>')
            parts.append(
                f'<rect x="{x0:.1f}" y="0" width="{half:.1f}" height="{h:.1f}" class="d-view"/>'
                f'<text x="{x0 + half / 2:.1f}" y="{h / 2:.1f}" class="d-label">Player {user}'
                f'</text><text x="{x0 + half / 2:.1f}" y="{h / 2 + 22:.1f}" class="d-small">'
                f'Camera {user}</text>{shape}'
                f'<text x="{mx + mw / 2:.1f}" y="{my + mh + 16:.1f}" class="d-small">'
                f'Minimap {user}</text>'
                f'<text x="{x0 + half / 2:.1f}" y="{h / 2 - 40:.1f}" class="d-cross">+</text>')
        parts.append(f'<rect x="{half:.1f}" y="0" width="{gap:.1f}" height="{h:.1f}" '
                     'class="d-gap"/>')
        return (f'<figure class="diagram-box"><svg class="diagram" viewBox="-2 -2 {w + 4:.0f} '
                f'{h + 4:.0f}" role="img" aria-label="The window split between two players">'
                f'{"".join(parts)}</svg><figcaption>The window as the Split Screen and Minimap '
                f'trees lay it out ({res_x} × {res_y}; the black divider drawn wider than its '
                f'{num(divider)} pixels).</figcaption></figure>')

    # Tables of the scene ------------------------------------------------------

    def scene_settings(self):
        """The scene's settings that are not as in a new file, with where they are."""
        rna = bpy.types.SceneGameData.bl_rna
        game = G.differs(R._settings(self.scene.game_settings, skip=R.SCENE_GAME_SKIP),
                         self.start["game"])
        game.pop("input_actions", None)
        game.pop("inventory_rules", None)
        rows = []
        if self.scene.camera is not None:
            rows.append(f"<tr><td>Scene tab › Scene › Camera</td>"
                        f"<td><b>{esc(self.scene.camera.name)}</b></td></tr>")
        unknown = sorted(set(game) - set(SCENE_SETTING_PLACES))
        if unknown:
            raise RuntimeError(f"say where the scene settings {unknown} are "
                               "(SCENE_SETTING_PLACES)")
        for key, value in sorted(game.items()):
            rows.append(f"<tr><td>{esc(SCENE_SETTING_PLACES[key])}</td>"
                        f"<td><b>{esc(setting_value(rna, key, value))}</b></td></tr>")
        render = G.differs({name: R.encode(getattr(self.scene.render, name))
                            for name in G.RENDER_KEYS}, self.start["render"])
        render_rna = bpy.types.RenderSettings.bl_rna
        for key, value in render.items():
            rows.append(f"<tr><td>Output tab › Format › {esc(prop_label(render_rna, key))}</td>"
                        f"<td><b>{esc(G.value_text(value))}</b></td></tr>")
        return '<table class="data"><tbody>' + "".join(rows) + "</tbody></table>"

    def input_map(self):
        """Every action of the Game Input Map, and whether a new scene has it as it is."""
        rows = []
        key_rna = bpy.types.GameInputBinding.bl_rna
        for entry in self.recipe["scene"]["input_actions"]:
            settings = entry["settings"]
            name = settings["name"]
            start = self.start["actions"].get(name)
            if start is None:
                state = "added"
            elif R._equal(start, {"settings": settings, "bindings": entry["bindings"]}):
                state = "as in a new scene"
            else:
                raise RuntimeError(f"action {name} differs from a new scene's: say how")
            self._check_action(entry)
            keyboard, gamepad = [], []
            for binding in entry["bindings"]:
                device = binding["device"]
                if device == 'KEYBOARD':
                    text = enum_label(key_rna, "key", binding["key"])
                elif device == 'MOUSE':
                    text = enum_label(key_rna, "mouse_button", binding["mouse_button"])
                    if "Wheel" not in text and "Mouse" not in text:
                        text += " Mouse Button"
                else:
                    text = enum_label(key_rna, "gamepad_control", binding["gamepad_control"])
                mods = [m for m, on in (("Shift", binding.get("use_shift")),
                                        ("Ctrl", binding.get("use_ctrl")),
                                        ("Alt", binding.get("use_alt"))) if on]
                text = "+".join(mods + [text])
                if settings["type"] == 'VECTOR' and not G._is_stick(binding):
                    text += f" ({enum_label(key_rna, 'direction', binding['direction'])})"
                (gamepad if device == 'GAMEPAD' else keyboard).append(text)
            rows.append(
                f"<tr><td><b>{esc(name)}</b></td>"
                f"<td>{esc(enum_label(bpy.types.GameInputAction.bl_rna, 'type', settings['type']))}"
                f"</td><td>{esc(settings.get('context') or '')}</td>"
                f"<td>{esc(', '.join(keyboard))}</td><td>{esc(', '.join(gamepad))}</td>"
                f"<td>{esc(state)}</td></tr>")
        missing = set(self.start["actions"]) - {entry["settings"]["name"] for entry in
                                                self.recipe["scene"]["input_actions"]}
        if missing:
            raise RuntimeError(f"the example removed the actions {sorted(missing)}: say so")
        return ('<div class="scroll"><table class="data"><thead><tr><th>Action</th><th>Type</th>'
                '<th>Context</th><th>Keyboard and mouse</th><th>Gamepad</th><th></th></tr>'
                '</thead><tbody>' + "".join(rows) + "</tbody></table></div>")

    def inventory_rules(self):
        rows = []
        rna = bpy.types.GameInventoryRules.bl_rna
        for rules in self.scene.game_settings.inventory_rules:
            values = R._settings(rules)
            name = values.pop("name")
            fresh = bpy.data.scenes.new("__guide_rules")
            try:
                new = fresh.game_settings.inventory_rules.new(name="__new")
                defaults = R._settings(new)
            finally:
                bpy.data.scenes.remove(fresh)
            changed = G.differs(values, defaults)
            changed.pop("users", None)
            users = [f"{ob.name} › {prop.name}" for ob in bpy.data.objects
                     for prop in ob.game.properties
                     if prop.type in {'LIST', 'TABLE'} and prop.inventory_rules == name]
            rows.append(f"<tr><td><b>{esc(name)}</b></td><td>" + ", ".join(
                f"{esc(prop_label(rna, key))} <b>{esc(setting_value(rna, key, value))}</b>"
                for key, value in changed.items()) + f"</td><td>{esc(', '.join(users))}</td></tr>")
        return ('<table class="data"><thead><tr><th>Inventory Rules</th><th>Settings (the rest '
                'as new rules)</th><th>Lists that follow them</th></tr></thead><tbody>' +
                "".join(rows) + "</tbody></table>")

    def player_properties(self):
        """Both players' game properties side by side."""
        props = {name: {prop["name"]: prop for prop in self.view["objects"][name]["properties"]}
                 for name in ("Player", "Player 1")}
        order = [prop["name"] for prop in self.view["objects"]["Player"]["properties"]]
        if order != [prop["name"] for prop in self.view["objects"]["Player 1"]["properties"]]:
            raise RuntimeError("the players do not have the same game properties in order")
        missing = set(order) - set(PROPERTY_USES)
        if missing or set(PROPERTY_USES) - set(order):
            raise RuntimeError(f"say what uses the properties {sorted(missing)} "
                               "(PROPERTY_USES), and no others")
        rows = []
        for key in order:
            first, second = props["Player"][key], props["Player 1"][key]
            same = R._equal({k: v for k, v in first.items()},
                            {k: v for k, v in second.items()})
            other = ("same" if same else self.property_text(second))
            rows.append(f"<tr><td>{self.property_text(first)}</td><td>{other}</td>"
                        f"<td>{self.words.render(PROPERTY_USES[key])}</td></tr>")
        return ('<div class="scroll"><table class="data"><thead><tr><th>Player</th>'
                '<th>Player 1</th><th>Used by</th></tr></thead><tbody>' + "".join(rows) +
                "</tbody></table></div>")

    def items_table(self):
        names = [ob.name for ob in self.scene.game_settings.item_library.objects]
        rows = []
        for name in names:
            self.shown_objects.add(name)
            entry = self.view["objects"][name]
            props = {prop["name"]: prop for prop in entry["properties"]}
            if props["item"]["value"] != name:
                raise RuntimeError(f"library item {name}: its item property is not its name")
            known = {"name", "max_stack", "category", "item", "count", "description",
                     "restore", "durability"}
            if set(props) - known:
                raise RuntimeError(f"{name}: properties {sorted(set(props) - known)} the item "
                                   "table does not show")
            if entry["game"] != {"use_ghost": True} or entry["settings"] or entry["ui"]:
                raise RuntimeError(f"{name}: settings the item table does not show")
            cell = lambda key: (esc(G.value_text(props[key]["value"])).replace("\n", "↵")
                                if key in props else "")
            materials = [m for m in entry["materials"] if m]
            rows.append(f"<tr><td><b>{esc(name)}</b></td><td>{cell('name')}</td>"
                        f"<td>{cell('category')}</td><td>{cell('max_stack')}</td>"
                        f"<td>{cell('restore')}</td><td>{cell('durability')}</td>"
                        f"<td>{cell('description')}</td><td>{esc(', '.join(materials))}</td></tr>")
        return ('<div class="scroll"><table class="data"><thead><tr><th>Object (= item)</th>'
                '<th>name</th><th>category</th><th>max_stack</th><th>restore</th>'
                '<th>durability</th><th>description</th><th>Material</th></tr></thead><tbody>' +
                "".join(rows) + "</tbody></table></div>" + self.materials_for(names))

    def pickups_table(self):
        names = [name for name in self.recipe["objects"] if name.startswith("Pickup_")]
        counts = collections.Counter()
        for name in names:
            self.shown_objects.add(name)
            entry = self.view["objects"][name]
            props = {prop["name"]: prop["value"] for prop in entry["properties"]}
            if (set(props) != {"item", "count"} or entry["game"] != {"use_ghost": True} or
                    entry["settings"] or entry["ui"] or entry["bindings"] or
                    self.homes[name] != ["Scene Collection"]):
                raise RuntimeError(f"{name} is not a plain pickup")
            item = props["item"]
            if entry["materials"] != self.recipe["objects"][item]["materials"]:
                raise RuntimeError(f"{name} has not its item's material")
            counts[(item, props["count"])] += 1
        rows = "".join(f"<tr><td>{esc(item)}</td><td>{count}</td><td>{number}</td></tr>"
                       for (item, count), number in sorted(counts.items()))
        return len(names), ('<table class="data"><thead><tr><th>item</th><th>count</th>'
                            '<th>Pickups</th></tr></thead><tbody>' + rows + "</tbody></table>")

    @staticmethod
    def property_text(prop):
        """A game property as the UI guide's tables give it, and the Inventory Rules a list
        follows."""
        text = G.Example.property_text(prop)
        if prop.get("inventory_rules"):
            text += f", Inventory Rules <b>{esc(prop['inventory_rules'])}</b>"
        return text

    def sounds_table(self):
        rows = []
        for sound in bpy.data.sounds:
            if sound.users == 0:
                continue
            rows.append(f"<tr><td><b>{esc(sound.name)}</b></td><td>"
                        f"{'packed in the file' if sound.packed_file else esc(sound.filepath)}"
                        "</td></tr>")
        return ('<table class="data"><thead><tr><th>Sound</th><th>File</th></tr></thead>'
                '<tbody>' + "".join(rows) + "</tbody></table>")

    def action_table(self, names):
        """Actions (animations) with what they change."""
        rows = []
        for name in names:
            action = bpy.data.actions[name]
            self.shown_actions.add(name)
            curves = []
            for layer in action.layers:
                for strip in layer.strips:
                    for bag in strip.channelbags:
                        kind = bag.slot.target_id_type.title() if bag.slot else "?"
                        for curve in bag.fcurves:
                            keys = " → ".join(
                                f"frame {num(point.co[0])}: {num(point.co[1])}"
                                for point in curve.keyframe_points)
                            curves.append(f"{esc(kind)} {esc(curve.data_path)}"
                                          f"[{curve.array_index}]: {esc(keys)}")
            rows.append(f"<tr><td><b>{esc(name)}</b></td><td>{'<br>'.join(curves)}</td></tr>")
        return ('<table class="data"><thead><tr><th>Action</th><th>Keys</th></tr></thead>'
                '<tbody>' + "".join(rows) + "</tbody></table>")


def node_of(tree, idname):
    nodes = [node for node in tree.nodes if node.bl_idname == idname]
    if len(nodes) != 1:
        raise RuntimeError(f"tree {tree.name} has {len(nodes)} nodes {idname}")
    return nodes[0]


# -----------------------------------------------------------------------------
# Checked words


class Words:
    """Renders the marks in the words (<n>, <s>, <v>, <c>) after checking them against the
    example."""

    MARK = re.compile(r"<(n|s|S|v|c)>(.*?)</\1>", re.S)

    def __init__(self, ex):
        self.names = set()
        data = bpy.data
        for collection in (data.objects, data.collections, data.node_groups, data.materials,
                           data.actions, data.sounds, data.images):
            self.names |= {item.name for item in collection}
        game = ex.scene.game_settings
        self.names |= {action.name for action in game.input_actions}
        self.names |= {rules.name for rules in game.inventory_rules}
        for ob in data.objects:
            ui = ob.game.ui
            for key in ("action", "accept_categories", "widget_property"):
                value = getattr(ui, key, "")
                if value:
                    self.names.add(value)
                    self.names |= {part.strip() for part in value.split(",")}
            if ob.type == 'FONT' and ob.data.body:
                self.names.add(ob.data.body)
                self.names |= set(re.findall(r"\{[^{}]*\}", ob.data.body))
            for prop in ob.game.properties:
                self.names.add(prop.name)
                if prop.type == 'TABLE':
                    self.names |= {column.name for column in prop.columns}
                    for row in prop.rows:
                        self.names |= {str(cell.value) for cell in row.cells}
                elif prop.type == 'LIST':
                    self.names |= {str(element.value) for element in prop.elements}
                elif prop.type == 'CHOICE':
                    self.names |= {option.name for option in prop.options}
                    self.names.add(prop.value)
                elif prop.type in {'STRING'}:
                    self.names.add(prop.value)
                    self.names |= {part.strip() for part in prop.value.split(",")}
        self.frame_marks = {}
        self.tree_labels = {}
        self.all_titles = set()
        # Every node's sockets and settings (the words outside the frames), and the labels of
        # the settings in the tabs (UI Element, physics, camera, scene: the words anywhere).
        self.all_labels = set()
        self.setting_labels = set()
        for tree in data.node_groups:
            if tree.bl_idname != "LogicNodeTree":
                continue
            for node in tree.nodes:
                if node.bl_idname == "NodeFrame":
                    continue
                titles, labels, values = self.node_marks(node)
                marks = self.frame_marks.setdefault((tree.name, node.parent), (set(), set(),
                                                                                set()))
                marks[0].update(titles)
                marks[1].update(labels)
                marks[2].update(values)
                self.tree_labels.setdefault(tree.name, set()).update(labels)
                self.all_titles |= titles
                self.all_labels |= labels
                for socket in node.inputs:
                    value = getattr(socket, "default_value", None)
                    if isinstance(value, str) and value:
                        self.names.add(value)
                        self.names |= {part.strip() for part in value.split(",")}
        for struct in (bpy.types.GameObjectUISettings, bpy.types.GameObjectSettings,
                       bpy.types.SceneGameData, bpy.types.Camera, bpy.types.GameInputAction,
                       bpy.types.GameInventoryRules, bpy.types.Object):
            for prop in struct.bl_rna.properties:
                self.setting_labels.add(prop.name)
                for item in getattr(prop, "enum_items", ()):
                    self.setting_labels.add(item.name)
        self.setting_labels |= {label for label, _inverted in G.OBJECT_SETTING_LABELS.values()}
        self.setting_labels |= set(G.GAME_SETTING_LABELS.values())
        self.setting_labels |= {G.ui_label(key, widget) for _short, _panel, keys in G.UI_GROUPS
                                for key in keys for widget in ("NONE", "SLIDER", "TOGGLE",
                                                               "TEXT", "BAR", "SELECTOR")}
        self.all_labels |= self.setting_labels

    @staticmethod
    def node_marks(node):
        titles = {node_title(node), node.bl_label}
        labels = set()
        values = set()
        for socket in list(node.inputs) + list(node.outputs):
            if socket.enabled and not socket.hide:
                labels.add(G.socket_label(socket))
        for socket in node.inputs:
            if not socket.enabled or socket.hide:
                continue
            value = G.socket_value(socket)
            if value is None:
                continue
            if isinstance(value, bool):
                values.add("on" if value else "off")
            else:
                values.add(str(value))
                if isinstance(value, str):
                    values |= {part.strip() for part in value.split(",")}
        for text, value, state, items in G.node_props(node):
            if text:
                labels.add(text)
            if value is not None:
                values.add(str(value))
            if state is not None:
                values.add("on" if state else "off")
            for item in items or ():
                values.add(item)
        # A merged node's option is also in its header's dropdown (Switch Flow's Integer).
        if hasattr(node, "variant"):
            values.add(enum_label(node.bl_rna, "variant", node.variant))
        return titles, labels, values

    def render(self, text, tree=None, frame=None):
        """The words with the marks checked and turned into HTML. In a frame's words, <n>, <s>
        and <v> must be of that frame (<s> may also be any node's or setting's label)."""
        marks = None
        if tree is not None:
            marks = self.frame_marks.get((tree.name, frame), (set(), set(), set()))

        def replace(match):
            kind, inner = match.group(1), match.group(2)
            plain = re.sub(r"<[^>]+>", "", inner)
            plain = plain.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
            where = f"{tree.name} › {frame.label if frame else '(no frame)'}" if tree else "text"
            if kind == "n":
                pool = marks[0] if marks is not None else self.all_titles
                if plain not in pool:
                    raise RuntimeError(f"{where}: no node {plain!r} there")
                return f"<b>{inner}</b>"
            if kind == "s":
                pool = ((marks[1] | self.setting_labels) if marks is not None else
                        self.all_labels)
                if plain not in pool:
                    raise RuntimeError(f"{where}: no socket or setting {plain!r}")
                return f"<i>{inner}</i>"
            if kind == "S":
                pool = (self.tree_labels.get(tree.name, set()) if tree is not None else
                        self.all_labels)
                if plain not in pool:
                    raise RuntimeError(f"{where}: no socket or setting {plain!r} in the tree")
                return f"<i>{inner}</i>"
            if kind == "v":
                if marks is None or plain not in marks[2]:
                    raise RuntimeError(f"{where}: no node there shows the value {plain!r}")
                return f'<code class="v">{inner}</code>'
            if plain not in self.names:
                raise RuntimeError(f"{where}: the example has no {plain!r}")
            return f"<code>{inner}</code>"

        return self.MARK.sub(replace, text)


# -----------------------------------------------------------------------------
# The words

def require(condition, what):
    """Stop when a fact the words state is not so in the example."""
    if not condition:
        raise RuntimeError(f"the words say {what}, the example differs")


def obj(name):
    return bpy.data.objects[name]


def node_in(tree_name, idname, **values):
    """The one node of a kind in a tree whose settings or inputs are these."""
    found = []
    for node in bpy.data.node_groups[tree_name].nodes:
        if node.bl_idname != idname:
            continue
        ok = True
        for key, value in values.items():
            if hasattr(node, key):
                have = getattr(node, key)
            else:
                have = node.inputs[key.replace("_", " ")].default_value
            if isinstance(have, bpy.types.ID):
                have = have.name
            if have != value:
                ok = False
        if ok:
            found.append(node)
    if len(found) != 1:
        raise RuntimeError(f"{tree_name}: {len(found)} nodes {idname} with {values}")
    return found[0]


PROPERTY_USES = {
    "stack_items": "Pick up (<s>Stack</s> of Add Item); the inventory's <c>StackToggle</c> "
                   "checkbox",
    "inventory": "the bag: the inventory's grid, the chest menu's bag, picking up, eating, "
                 "crafting, sorting",
    "recipes": "the CRAFT list; the Inventory Rules <c>Player bag</c> (on <c>inventory</c>) "
               "take their recipes from it: two items dropped on each other make a recipe's "
               "result",
    "walk_speed": "walking; the Settings slider <c>WalkSlider</c>; the HUD's <c>Value Text</c>",
    "sort_by": "sorting (<c>Name</c> or <c>Count</c>); the Settings choice <c>SortSelector</c>",
    "show_hints": "the Settings checkbox <c>HintsToggle</c>",
    "player_name": "the name tags, the HUD's <c>Value Text</c>, the computer's status; the "
                   "Settings field <c>NameField</c>",
    "weapon": "the inventory's WEAPON cell (a list of one slot)",
    "armor": "the inventory's ARMOR cell",
    "quick": "the inventory's QUICK cell and the HUD's quick bar",
    "stamina": "the Stamina frame and eating (Use frame); the stamina bars",
    "user": "the player's number: every <s>User</s> of the Game tree (Whose frame)",
    "map_mode": "the map: 0 live minimap, 1 picture, 2 off",
    "at_computer": "on while the player uses the computer: walking, stamina, the pause menu "
                   "and the crosshair wait",
    "camera": "the player's own camera: First person, and back from the computer",
    "head": "what Mouse Look and Gamepad Look tilt up and down",
}
# What each logic frame does, by (tree, frame title): filled from frame_words() by chapters().
FRAMES = {}


def frame_words(ex):
    """What each logic frame does: short bullets beside its picture. Values the words give that
    no node shows are read from the example here."""
    game = ex.scene.game_settings
    steps = game.fps
    require(game.use_fixed_physics_timestep, "Logic + Physics steps come at a fixed rate")
    walk = obj("Player").game.properties["walk_speed"].value
    require(obj("Player 1").game.properties["walk_speed"].value == walk,
            "both players walk at the same speed")
    float_default = node_of(bpy.data.node_groups["Inventory"], "LogicNativeValueFloat")
    float_default = float_default.outputs[0].default_value
    look = node_in("Game", "LogicNativeGamepadLook")
    require(abs(look.inputs["Sensitivity"].default_value - 3.14159) < 1e-4,
            "the gamepad turns half a turn a second")
    glow = bpy.data.materials["GiftButtonMat"]
    math_node = next(node for node in glow.node_tree.nodes if node.type == 'MATH')
    low = math_node.inputs[2].default_value
    high = math_node.inputs[1].default_value + low
    for name in ("GiftButtonMat", "LightsButtonMat", "LeaveButtonMat"):
        tree = bpy.data.materials[name].node_tree
        require(any(link.from_node.type == 'OBJECT_INFO' and link.from_socket.name == "Alpha"
                    for link in tree.links), f"{name} reads the object's alpha")
    require(all(obj(name).color[3] == 0.0 for name in
                ("GiftButton", "LightsButton", "LeaveButton", "TerminalGift",
                 "TerminalTogether")), "the buttons start with alpha 0")
    return {
        # Split screen ----------------------------------------------------------
        ("Split Screen", None): (
            "When the game starts (<n>On Init</n>), the first <n>Set Camera</n> gives player "
            "<v>0</v> the camera <v>Camera 0</v>. <s>User</s> 0 sets the scene's active camera, "
            "as in a game without split screen.",
            "The second <n>Set Camera</n> gives player <v>1</v> the camera <v>Camera 1</v>. A "
            "<s>User</s> from 1 to 7 gives that player a view of their own: the window splits "
            "between the players that have a camera.",
            "<n>Set Split Screen</n> says how they share it. <s>2 Players</s> "
            "<v>Side by Side</v>: player 0 left, player 1 right. <s>3 Players</s> "
            "<v>Favor Top</v>: with a third player, player 0 would get the top half and the "
            "other two would share the bottom half. "
            "<s>View</s> <v>Crop</v>: a view keeps the height of the picture its camera has "
            "with the whole window, and shows its middle, so things keep their size.",
            "<s>Divider</s> <v>4</v>: 4 pixels of black between the views. "
            "<s>Keep Mouse in View</s> <v>on</v>: the mouse is player 0's and works only in "
            "their view, so the cursor cannot leave it. It is free while the game is paused, so "
            "the pause menu over the whole window can be used (on Linux with Wayland only a "
            "hidden cursor is kept in the view).",
            "<n>Set Cursor Visibility</n> <s>Show</s> <v>Only in Menus</v>: the system cursor "
            "shows while player 0 is in a menu that takes their input (the inventory, the "
            "chest, the pause menu), and is hidden while they play.",
            "The tree runs on the empty <c>Split Screen</c>. Any object would do: one object "
            "keeps the set-up in one place.",
        ),
        # The players -------------------------------------------------------------
        ("Game", "Whose: this player's number and object"): (
            "<c>Player</c> and <c>Player 1</c> run this one tree. Every node that needs to know "
            "whose input, menu or view it is reads the number in the object's own game property "
            "<v>user</v> (<n>Get Game Property</n> <v>Integer</v>): 0 on <c>Player</c>, 1 on "
            "<c>Player 1</c>.",
            "Its <s>Property Value</s> goes to every <S>User</S> input of the tree: the "
            "<b>Input Action</b>, <b>Open Object Menu</b>, <b>Set Camera</b>, "
            "<b>Set Player Pointer</b>, <b>Take UI Control</b>, <b>Show Message</b> and "
            "<b>Get Player Gamepad</b> nodes of the other frames.",
            "<n>Get Owner</n> gives the object the tree runs on, the player itself, to the nodes "
            "that need it as an object (distances, stamina, the inventory).",
            "So a second player needs no second tree: a copy of the player with another "
            "<c>user</c>.",
        ),
        ("Game", "Look: the mouse (player 1), the right stick (gamepads)"): (
            "The frame title counts players from 1: “player 1” is player 0 here.",
            "Every step (<n>On Logic + Physics Step</n>), <n>Switch Flow</n> <v>Integer</v> on "
            "the player's number runs <n>Mouse Look</n> only through <s>Then 0</s>: only player "
            "0 looks with the mouse (the mouse is theirs).",
            "<n>Mouse Look</n> turns the player left and right (<s>Body</s> empty: the tree's "
            "object) and the head up and down (<s>Head</s>: the object in the player's "
            "<c>head</c> property, <c>Head 0</c> or <c>Head 1</c>), at most "
            "<v>(-1.3963, 1.3963)</v> radians, 80° each way (<s>Cap Up / Down</s>). The camera "
            "hangs from the head, so it follows. <s>Center Mouse</s> keeps the cursor in the "
            "middle of player 0's view.",
            "<n>Gamepad Look</n> runs for every player, with the gamepad <n>Get Player Gamepad</n> "
            "gives them (<s>Index</s>). With the scene's Gamepads Start at Player 1, player 0 "
            "has none (−1: nothing happens) and player 1 has the first gamepad. "
            "<s>Sensitivity</s> <v>3.1416</v> radians a second (half a turn) at full stick; "
            "<s>Exponent</s> <v>2.3</v>: the stick's push is raised to this power, so small "
            "pushes turn slowly; pushes below <s>Threshold</s> <v>0.1</v> do nothing.",
            "Both wait by themselves while the player uses a menu or the computer.",
        ),
        ("Game", "Walk with the Move action at the Settings speed"): (
            "Every step, the <n>Branch</n> lets it through only on <s>False</s>: its "
            "<s>Condition</s> is the player's <c>at_computer</c> (read in the Computer frame). "
            "A player at the computer does not walk.",
            "<n>Input Action</n> <v>Move</v> (<v>While Held</v>) gives the direction of WASD or "
            "the left stick as its <s>Vector</s>: X right, Y forward, at most 1 long.",
            "<n>Vector Math</n> <v>Scale</v> multiplies it by the player's <v>walk_speed</v> "
            "(<n>Get Game Property</n>), which the Settings slider changes.",
            "<n>Apply Movement</n> moves the player by the result each step, along its own axes "
            f"(<s>Local</s> <v>on</v>): forward is where it faces. <c>walk_speed</c> {num(walk)} "
            f"m a step is {num(walk * steps)} m a second at the scene's {steps} steps a second.",
        ),
        ("Game", "Stamina: drains while walking, comes back while standing"): (
            "Every step, unless the player is at the computer (the first <n>Branch</n>, "
            "<s>False</s>):",
            "the second <n>Branch</n> asks <n>Input Action</n> <v>Move</v>'s <s>Active</s>: "
            "while the player walks, <n>Modify Property</n> <v>Subtract</v> takes <v>0.3</v> "
            "from <v>stamina</v>; while they stand, the other one adds <v>0.5</v>. "
            "<s>Clamp</s> keeps it between <v>0</v> and <v>100</v>.",
            "The stamina bars show it. Nothing else reads it: the player still walks at 0, and "
            "eating or drinking adds to it (the Use frame).",
        ),
        ("Game", "First person: hide your own body (not its shadow)"): (
            "When the game starts, <n>Set Camera Visibility</n> hides the player (<s>Object</s> "
            "empty: the tree's object) from its own camera (<s>Camera</s>: the object in the "
            "player's <v>camera</v> property). <s>Include Children</s> <v>on</v>: its name "
            "tags and stamina bar too. The other player's camera still draws all of them.",
            "<s>Keep Shadow</s> <v>on</v>: hidden, the body still casts its shadow in the "
            "player's own view.",
        ),
        # Controls, HUD, map ---------------------------------------------------------
        ("Game", "HUD: show it from the start"): (
            "When the game starts, <n>Open Object Menu</n> opens the collection <v>HUD</v> as "
            "this player's menu, seen through the camera <v>InvCamera</v>.",
            "<s>Area</s> <v>Player's View</v>: each player gets their own copy of the HUD, "
            "scaled into their part of the window. The pieces' UI Element › Screen Anchor keeps each "
            "at a side, a corner or the middle of that view.",
            "<s>Presentation</s> <v>Overlay</v> and <s>Routing</s> <v>UI + Game World</v>: the "
            "game shows and plays on under it. <s>Back</s> <v>Ignore</v>: Back never closes "
            "it. <s>Toggle</s> <v>off</v>.",
            "<s>Owner</s> empty: the HUD belongs to the object whose logic opens it, the player. "
            "Its texts (<c>{player_name}</c>) and bars read that player's properties.",
        ),
        ("Game", "Map: M or R3 cycles live map, picture, off"): (
            "<n>Input Action</n> <v>Map</v> (<v>Pressed</v>).",
            "<n>Switch Flow</n> <v>Integer</v> on the player's <v>map_mode</v>: from "
            "<s>Then 0</s> it sets <v>1</v>, from <s>Then 1</s> <v>2</v>, from <s>Default</s> "
            "(any other number) <v>0</v>. Each press goes live minimap (0) → picture (1) → off "
            "(2) → live minimap.",
            "The minimap camera and the HUD's map read <c>map_mode</c> (the Map chapter).",
        ),
        # Items ----------------------------------------------------------------
        ("Game", "Pick up: add each touched item to the inventory, remove it once"): (
            "Every step, the <n>Branch</n> goes on while <n>On Collision</n> says the player is "
            "<s>Colliding</s> with an object that has the game property <v>item</v> (its "
            "<s>Property</s> filter): a pickup.",
            "<n>Add Item</n> puts that object into the player's <v>inventory</v>: given an "
            "object as <s>Item</s>, it takes the object's <c>item</c> and <c>count</c> "
            "properties (<s>Amount</s> <v>0</v>: all it carries).",
            "Its <s>Override</s> is <v>on</v>, so it uses its own rules, not the list's "
            "Inventory Rules: <s>Stack</s> is the player's <c>stack_items</c> (the inventory's "
            "Stack pickups checkbox); each item stacks up to its Item Library object's "
            "<c>max_stack</c> (<s>Max Stack</s> <v>0</v> only means no limit for an item without "
            "one); <s>Capacity</s> <v>0</v> means no limit.",
            "<n>Show Message</n> adds a line to this player's HUD text <v>Messages</v> "
            "(<s>User</s>) for <v>3</v> seconds: <n>Formatted String</n> <v>Picked up {}</v> "
            "with the item's name from <n>Get Item Info</n>, and <s>Count</s> from "
            "<s>Added</s> (how many went in): “Picked up Gold Coin x5”.",
            "<n>Compare</n> asks whether the <s>Leftover</s> (a number through "
            "<n>Typecast Value</n>) is <v>0</v>: then all of it went in and "
            "<n>Remove Object</n> removes the pickup. Else the pickup keeps what did not fit "
            "(<n>Set Game Property</n> <v>count</v>) and the player reads <v>Bag full</v>. "
            "With <s>Capacity</s> 0 the bag never fills: in the example everything fits.",
            "<s>Collided Object</s> is the first object touching (<v>Any Object</v>), and "
            "<s>Colliding</s> goes on every step: pickups touched together go in one a step.",
        ),
        # Inventory -------------------------------------------------------------
        ("Game", "Open or close the inventory with I or gamepad Select"): (
            "<n>Input Action</n> <v>Inventory</v> (<v>Pressed</v>) runs once per press of the "
            "action's key or button, for this player (<s>User</s>).",
            "<n>Open Object Menu</n> opens the collection <v>InventoryMenu</v> through "
            "<v>InvCamera</v>. <s>Area</s> <v>Player's View</v>: it shows in this player's part "
            "of the window; the other player plays on.",
            "<s>Toggle</s> <v>on</v>: the same press closes it again. <s>Open Mode</s> "
            "<v>Replace Top</v>: opened while the chest is open, it takes the chest's place (a "
            "HUD is never replaced). <s>Presentation</s> <v>Screen</v>: the menu under it, the "
            "player's HUD, is hidden while it is open. <s>Back</s> <v>Close Menu</v>: Back (Esc, "
            "the gamepad's B) closes it too.",
            "<s>Routing</s> <v>UI Only</v>: while it is open, UI Elements in the world and in "
            "menus below it do not respond to the pointer or the keys. Game logic still gets the "
            "player's input: nothing in the Walk frame checks for an open menu (only looking "
            "waits by itself). <s>Input Context</s> "
            "<v>Inventory</v>: actions of that context (<c>Sort</c>) work only while it is the "
            "top menu.",
            "<s>Owner</s> empty: the player who opens it owns it, so the menu's grids show "
            "that player's lists.",
        ),
        ("Inventory", "Whose menu: its player and their character"): (
            "Each player's copy of the menu runs this tree. <n>Get Menu Player</n> gives that "
            "copy's player (<s>User</s>) and its owner (<s>Owner</s>: the player object that "
            "opened it).",
            "<s>User</s> goes to <b>Input Action</b>; <s>Owner</s> to the nodes that change the "
            "owner's lists and read its <c>sort_by</c>.",
        ),
        ("Inventory", "Grid: 3 x 3 copies of Slot; it moves, stacks, combines, equips "): (
            "<n>Object Grid View</n> shows a list game property in slot objects. <s>Object</s> "
            "empty: in a menu that is the menu's owner, so each player's copy shows their own "
            "<v>inventory</v>.",
            "<s>Slot Template</s> <v>Slot</v> with <s>Rows</s> <v>3</v> and <s>Columns</s> "
            "<v>3</v>: the grid copies <c>Slot</c> with its children into 3 × 3 cells, "
            "<s>Spacing</s> <v>(1.4, 1.4, 0)</v> apart, and shows each item's mesh (its Item "
            "Library object's) on its cell. <s>Show Counts</s>: the cell's text shows the "
            "count above 1. More items scroll with the wheel and the <s>Scrollbar</s> "
            "<v>ScrollBar</v>.",
            "<s>Handle Moves</s> <v>on</v>: the grid changes the list itself. A dragged item "
            "(with the mouse, or carried with <c>Grab</c>) moves to an empty place, stacks with "
            "the same item, swaps with another (<s>Swap</s>), or makes a recipe with the item "
            "it lands on; also into the equipment cells, which handle moves too. Holding "
            "<s>Split Action</s> <v>Split</v> while taking a stack takes half (<s>Split "
            "Amount</s> <v>0</v>).",
            "<s>Drop Into World</s> <v>on</v>: a stack let go over the player's game view drops "
            "into the world in front of the player (<s>Distance</s> <v>1.6</v>, "
            "<s>Hover Height</s> <v>0.5</v>) as a copy of its Item Library object. "
            "<s>Drop In Front</s> <v>on</v>: Secondary (right click, the gamepad's X) on a stack "
            "drops it the same way.",
            "<s>Hint Text</s> <v>ItemName</v> shows <s>Accept Hint</s> or <s>Refuse Hint</s> "
            "while an item is dragged over a cell. <s>Show Categories</s> and <s>Search</s> "
            "come from the Tabs and Find frame. <s>Use Action</s> <v>Use Item</v>: pressed on "
            "the highlighted cell, the item is used (the Game tree's Use frame).",
            "<s>On Scroll</s>, <s>On Rebind</s>, <s>Page</s> and <s>Page Count</s> go to the "
            "Pages frame.",
        ),
        ("Inventory", "Pages: < and > turn the page, Page / Page Count between"): (
            "When the grid scrolls or shows other items (<S>On Scroll</S>, <S>On Rebind</S>, "
            "through <n>Any Flow</n>), <n>Set Text</n> writes “Page 1 / 3” into "
            "<v>PageText</v>: <n>Formatted String</n> <v>Page {} / {}</v> of the grid's "
            "<S>Page</S> and <S>Page Count</S>.",
            "<n>On UI Event</n> <v>Click</v> on <v>page_prev</v> or <v>page_next</v> (the "
            "&lt; and &gt; buttons) runs <n>Scroll View To</n> <v>Previous Page</v> or "
            "<v>Next Page</v>. Its <s>Viewport Object</s> is empty: the tree's object, "
            "<c>Panel</c>, the bag grid's viewport.",
        ),
        ("Inventory", "Tabs and Find: show some categories, or items by name"): (
            "<n>On UI Event</n> <v>Click</v> on a <v>tab</v> (<c>TabAll</c>, <c>TabFood</c>, "
            "<c>TabGear</c>, <c>TabOther</c>): <n>Get Game Property</n> reads the tab's own "
            "<v>categories</v> and <n>Set Game Property</n> writes it on <c>Panel</c> "
            "(<n>Get Owner</n>: this copy's panel).",
            "The grid's <S>Show Categories</S> reads the panel's <v>categories</v>: empty shows "
            "every item, “food, potion” only those. Its <S>Search</S> reads the panel's "
            "<v>search</v>, which the Find field <c>SearchField</c> (a Text Field widget) "
            "writes as the player types (Text Fields take typing from player 0's keyboard "
            "only).",
            "<n>Set Object Attribute</n> <v>World Position</v> moves <v>TabMarker</v> to the "
            "clicked tab (<n>Get Object Attribute</n>).",
        ),
        ("Inventory", "Delete: left click a slot (or Enter / A) removes 1 item"): (
            "<n>On UI Event</n> <v>Click</v> on a cell of <v>inventory</v> (a click, or Accept "
            "on the selected cell): <n>Remove From Slot</n> takes <v>1</v> from that slot "
            "(<s>Slot</s>: the event's <s>Index</s>) of the owner's <v>inventory</v>.",
            "Its <s>Override</s> is off, so it follows the list's Inventory Rules "
            "(<c>Player bag</c>: Keep Places, an emptied slot stays a gap). Its own "
            "<s>Keep Places</s> would count only with <s>Override</s> on, or for a list without "
            "Inventory Rules.",
        ),
        ("Inventory", "Sort with the Sort action, then show the top"): (
            "<n>Input Action</n> <v>Sort</v> (<v>Pressed</v>) for the menu's player. The action's "
            "Input Context is Inventory: it works only while this menu is the top one.",
            "<n>Get Game Property</n> reads the owner's <v>sort_by</v> (the Settings choice); "
            "<n>String Operation</n> <v>Contains</v> <v>Count</v> turns <s>By Count</s> on for "
            "Count.",
            "<n>Sort Inventory</n> sorts the owner's <v>inventory</v> by item name or by count, "
            "then <n>Scroll View To</n> <v>Top</v> shows its start.",
        ),
        ("Inventory", "Settings button: open Settings on top of the inventory"): (
            "<n>On UI Event</n> <v>Click</v> on <v>settings</v> (<c>SettingsButton</c>): "
            "<n>Open Object Menu</n> opens <v>SettingsMenu</v> with <s>Open Mode</s> "
            "<v>Push</v>: the inventory stays open under it (hidden meanwhile: "
            "<s>Presentation</s> <v>Screen</v>) and shows again when Settings closes.",
            "Its <s>User</s> is <v>0</v>, but in a menu's own logic Open Object Menu opens for "
            "that menu's player, and the new menu belongs to the same owner: player 1's "
            "Settings opens in player 1's view and changes player 1's properties.",
        ),
        ("Inventory", "Cell look: Highlight and Refused play actions on the cell's mat"): (
            "<n>On UI Event</n> <v>Highlight Start</v> on a cell (<v>inventory</v>, "
            "<v>weapon</v>, <v>armor</v>, <v>quick</v>): <n>Play Action</n> plays "
            "<v>SlotHighlight</v> on it, frames <v>1</v> to <v>5</v>.",
            "<v>Highlight End</v>: <n>Animation Status</n> reads where that action is "
            "(<s>Action Frame</s>) and <n>Play Action</n> plays it from there back to frame "
            "<v>1</v>: a short hover fades back from where it got to.",
            "<v>Refused Start</v> and <v>Refused End</v> do the same with <v>SlotRefused</v> on "
            "<s>Layer</s> <v>1</v>, while a dragged item the cell refuses is over it.",
            "Both actions change the material's Viewport Display color (table below): the menus "
            "are seen through <c>InvCamera</c>, which draws with Workbench, and Workbench shows "
            "that color. Playing a material action gives the object its own copy of the "
            "material, so only that cell changes.",
        ),
        ("Inventory", "Durability: each cell's bar shows its item's own durability"): (
            "<n>On UI Event</n> <v>Item Shown</v> runs for a cell of <v>inventory</v> whenever it "
            "shows another item or its item's values change. Its <s>Item</s> is the slot (a row "
            "of the list): <n>Get Dictionary Key</n> reads its <v>durability</v>, or the "
            f"<n>Float</n> ({num(float_default)}) when it has none.",
            "<n>Set Game Property</n> writes it on the cell (its own <c>durability</c>), and "
            "<n>Set Object Attribute</n> shows the cell's <v>SlotBar</v> (<n>Get Child</n>) only "
            "above <v>0</v> (<n>Compare</n>).",
            "<c>SlotBar</c> is a Bar widget of <c>durability</c>, 0 to 100: it finds the "
            "property on its parent, the cell.",
        ),
        ("Game", "Use: Use Item on an item, or Quick, eats or drinks it"): (
            "<n>On UI Event</n> <v>Use</v> runs when a grid's slot is used: Use Item on a "
            "highlighted cell of the bag or the QUICK cell, or the Quick action for the HUD's "
            "quick bar. <s>Action</s> <v>inventory, quick</v>: the cells of those lists.",
            "Every player's Game tree hears every player's Use: <n>Compare</n> of the event's "
            "<s>User</s> with this player's number lets only that player's own tree go on.",
            "<n>Get Item Info</n> finds the item's object in the scene's Item Library and "
            "<n>Get Game Property</n> reads its <v>restore</v>. Above <v>0</v> (<n>Compare</n> "
            "<v>Greater Than</v>): <n>Modify Property</n> adds it to the player's "
            "<v>stamina</v>, at most <v>100</v>, and <n>Remove From Slot</n> takes <v>1</v> "
            "from the slot (<s>Slot</s>: the event's <s>Index</s>).",
            "Which list: the event's <s>Action</s>. The cells' Action names are the names of "
            "the lists they show (<c>inventory</c>, <c>quick</c>), so one node takes from either.",
            "<n>Show Message</n> writes “Apple: +15 stamina” in the HUD's <v>Messages</v> and the "
            "inventory's <v>ItemName</v> (when the inventory is open). An item without "
            "<c>restore</c> (a sword) only shows “Iron Sword can't be used”, in the "
            "inventory's <v>ItemName</v> alone.",
        ),
        # Equipment and crafting ---------------------------------------------------
        ("Inventory", "EQUIP: each cell shows its own list; Accept Categories"): (
            "Three more <n>Object Grid View</n> nodes, one cell each (<s>Rows</s> <v>1</v>), "
            "show the owner's lists <v>weapon</v>, <v>armor</v> and <v>quick</v> in "
            "<v>WeaponSlot</v>, <v>ArmorSlot</v> and <v>QuickSlot</v>, inside the "
            "<s>Viewport Object</s> <v>EquipPlate</v>.",
            "Each cell's UI Element › Accept Categories decides what it takes: "
            "<c>WeaponSlot</c> <c>weapon</c>, <c>ArmorSlot</c> <c>armor</c>, <c>QuickSlot</c> "
            "<c>food, potion</c> (an item's category is its Item Library object's "
            "<c>category</c>). A refused item goes back, and the cell plays its Refused look.",
            "They handle moves, and a stack dragged out over the game view drops into the world "
            "(<s>Drop Into World</s>). <s>Drop In Front</s> is off: Secondary does nothing on "
            "them. The quick cell's <s>Use Action</s> <v>Use Item</v> eats or drinks what it "
            "holds.",
        ),
        ("Inventory", "Craft: click or A on a recipe; the grid shows what it made"): (
            "Another <n>Object Grid View</n> lists the owner's <v>recipes</v> (a List of text "
            "lines) in copies of <v>RecipeSlot</v>, <s>Rows</s> <v>3</v> × <s>Columns</s> "
            "<v>1</v>, inside <s>Viewport Object</s> <v>CraftPlate</v>, with its own "
            "<s>Scrollbar</s> <v>CraftScrollBar</v>. <s>Priority</s> <v>1</v>: where grids use "
            "the same slot, or overlap under the pointer for the wheel, this one wins.",
            "<n>On UI Event</n> <v>Click</v> on a <v>recipe</v>: its <s>Item</s> is the recipe "
            "line. <n>Craft</n> makes it once (<s>Times</s> <v>1</v>) from the ingredients "
            "anywhere in the owner's <v>inventory</v>.",
            "<n>Set Text</n> writes what happened into <v>ItemName</v> (<n>Formatted String</n> "
            "<v>{}: {}</v>: the <s>Outcome</s>, such as Crafted or Missing, and the recipe), "
            "and <n>Scroll View To</n> <v>Show Item</v> scrolls the bag's grid to what it "
            "<s>Made</s>.",
        ),
        ("Quick Bar", "Quick bar: the quick slot; Quick uses it"): (
            "An <n>Object Grid View</n> in the HUD shows the owner's <v>quick</v> list in one "
            "<v>QuickBarSlot</v> (<s>Rows</s> <v>1</v>, <s>Columns</s> <v>1</v>).",
            "<s>Use Keys</s> <v>Quick</v>: the Quick action uses the item in the grid's first "
            "slot while the player has no menu open: the Game tree's Use frame eats it.",
            "<c>QuickBarSlot</c> takes no clicks nor focus (UI Element › Pointer Ignore, "
            "Focus None): only the key uses it.",
        ),
        # Chest -----------------------------------------------------------------
        ("Game", "Open or close the chest with Use when next to it"): (
            "<n>Input Action</n> <v>Use</v> (<v>Pressed</v>) for this player.",
            "<n>Distance Between</n> the player (<b>Get Owner</b>, Whose frame) and the object "
            "<v>Chest</v>; <n>Compare</n> <v>Less Than</v> <v>2.2</v>: within 2.2 m only.",
            "<n>Open Object Menu</n> opens <v>ChestMenu</v> for this player as the inventory "
            "opens: <v>Player's View</v>, <s>Toggle</s>, <v>Replace Top</v>, with "
            "<s>Input Context</s> <v>Chest</v>.",
            "The same Use also uses the computer and presses buttons: each of those checks its "
            "own distance.",
            "With a gamepad, Use is A, which the open menu takes as Accept (it presses the "
            "selected cell): a gamepad player closes the chest with Back (B). The keyboard's E "
            "toggles it as written.",
        ),
        ("Chest", "Chest list: the chest's items with their names"): (
            "<n>Object Grid View</n> shows the object <v>Chest</v>'s table <v>chest</v> "
            "(<s>Object</s> is set: not the player's) in rows: <s>Slot Template</s> "
            "<v>ChestRow</v>, <s>Columns</s> <v>1</v>, <s>Rows</s> <v>6</v>, <s>Spacing</s> "
            "<v>(0, 0.7, 0)</v>.",
            "<s>Show Names</s> and <s>Show Counts</s>: each row's first text shows the item's "
            "name (its Item Library object's <c>name</c>) with its count: “Blue Gem x2”.",
            "<s>Handle Moves</s> <v>on</v>: dragging between this list and the bag moves items "
            "from one object's list to the other's. Both players' copies show the same chest.",
        ),
        ("Chest", "Bag: your inventory as a grid"): (
            "A grid like the inventory's, without its scrollbar, hints, filters, Use and drops "
            "into the world: <s>Object</s> empty shows the menu owner's "
            "<v>inventory</v> in 3 × 3 copies of <v>BagSlot</v> (<s>Rows</s> <v>3</v>, "
            "<s>Columns</s> <v>3</v>).",
            "<s>Handle Moves</s> <v>on</v>: drag items between bag and chest. <s>Split Action</s> "
            "<v>Split</v> takes half a stack. <s>Drop Into World</s> is off: let go outside the "
            "two grids, a stack goes back.",
        ),
        # Settings --------------------------------------------------------------
        ("Settings", "Whose menu: its player and their character"): (
            "<n>Get Menu Player</n> gives the menu's player to the <s>User</s> of "
            "<b>Rebind Input Action</b> and <b>Close Object Menu</b>.",
        ),
        ("Settings", "Show hints toggle: show or hide the hint in the world"): (
            "<c>HintsToggle</c> is a Toggle widget of <c>show_hints</c>: a click flips the "
            "owner's (the player's) property by itself. <n>On UI Event</n> <v>Changed</v> on "
            "<v>hints</v> runs then, with the new value as <s>Item</s>.",
            "<n>Set Object Attribute</n> <v>Visible</v> shows or hides <v>Hint</v>, the controls "
            "text in the world. There is one Hint for both players: either player's checkbox "
            "shows or hides it for both.",
        ),
        ("Settings", "Grab key: the next key pressed becomes Grab"): (
            "<n>On UI Event</n> <v>Click</v> on <v>rebind</v> (<c>RebindButton</c>): "
            "<n>Rebind Input Action</n> waits up to <s>Timeout</s> <v>5</v> seconds for the "
            "menu player's next key or mouse button (<v>Keyboard & Mouse</v>) and gives it to "
            "<v>Grab</v>. Its <s>Direction</s> matters only for a vector action.",
            "A key or mouse button can be taken only from player 0 (the keyboard and mouse are "
            "theirs): in player 1's Settings the button waits its 5 seconds and changes nothing. "
            "The Game Input Map is one for all players, so player 0's new key is everyone's.",
            "<c>RebindButtonText</c> shows <c>{Grab:Keyboard}  /  {Grab:Gamepad}</c>: the "
            "keys it names change by themselves.",
        ),
        ("Settings", "Close: back to the menu below"): (
            "<n>On UI Event</n> <v>Click</v> on <v>close</v>: <n>Close Object Menu</n> "
            "<v>Top Menu</v> closes this player's top menu, Settings: the menu under it (the "
            "inventory or the pause menu) shows again. Back does the same.",
        ),
        # Pause -----------------------------------------------------------------
        ("Game", "Pause Menu: open with the Pause Menu action"): (
            "<n>Input Action</n> <v>Pause Menu</v> (<v>Pressed</v>): Esc, or the gamepad "
            "button the Input Map gives it.",
            "Unless the player is at the computer (<n>Branch</n>, <s>False</s>), "
            "<n>Open Object Menu</n> opens <v>Pause Menu</v> with <s>Area</s> <v>Window</v>: one "
            "menu over the whole window, as authored, and <s>Pause Game</s> <v>on</v>: the game "
            "stops for every player until it closes, however it closes.",
            "<s>Open Mode</s> <v>Push</v>: a menu already open stays under it. It is the menu of "
            "the player who opened it (<s>User</s>).",
            "<s>Toggle</s> is <v>on</v>, but while the game is paused this tree runs only its UI "
            "events: the Pause Menu action cannot close it again. The player closes it with Back "
            "(Esc, the gamepad's B) or Resume.",
        ),
        ("Pause Menu", "Whose menu: its player and their character"): (
            "<n>Get Menu Player</n> gives the menu's player to <b>Close Object Menu</b> and "
            "<b>Open Object Menu</b> (their <s>User</s>).",
        ),
        ("Pause Menu", "Resume: close the pause menu (the game goes on)"): (
            "<n>On UI Event</n> <v>Click</v> on <v>resume</v>: <n>Close Object Menu</n> "
            "<v>Top Menu</v> closes it, and the game goes on (it was opened with Pause Game).",
        ),
        ("Pause Menu", "Settings: open the Settings menu on top"): (
            "<n>On UI Event</n> <v>Click</v> on <v>settings</v>: <n>Open Object Menu</n> opens "
            "<v>SettingsMenu</v> with <s>Open Mode</s> <v>Push</v> and <s>Area</s> "
            "<v>Window</v>, over the whole window like the pause menu. Closing it shows the "
            "pause menu again.",
        ),
        ("Pause Menu", "Quit: end the game"): (
            "<n>On UI Event</n> <v>Click</v> on <v>quit</v>: <n>Quit Game</n>.",
        ),
        # Computer ----------------------------------------------------------------
        ("Game", "Computer: Use next to it to use it; Use, Back or Leave gives it"): (
            "<n>Input Action</n> <v>Use</v> within <v>2.2</v> of the <v>Computer</v> "
            "(<n>Distance Between</n>, <n>Compare</n>). <n>Branch</n> on <v>at_computer</v>: "
            "off, the player takes the computer; on, they leave it.",
            "Taking: <n>Take UI Control</n> (<s>Take</s> <v>on</v>) gives this player the "
            "<v>Computer</v>'s buttons alone. When another player has it, nothing happens (its "
            "<s>Failed</s> is not linked).",
            "<n>Set Player Pointer</n> <v>Surface</v> puts the pointer <v>Cursor</v> on the "
            "<v>Screen</v>: the player's <v>Move</v> action moves it at <s>Speed</s> <v>0.6</v> "
            "(screen widths a second at full tilt), player 0's mouse moves it directly, and "
            "Accept presses (<s>Press Action</s> empty: Enter, Space, the gamepad's A, a click).",
            "<n>Set Camera</n> gives this player the view of <v>Computer Camera</v>. Then the "
            "player's <v>at_computer</v> on; the computer's <v>status</v> “Hero is using it” "
            "(<n>Formatted String</n> <v>{} is using it</v> of their <v>player_name</v>); "
            "<v>Screen Buttons</v> shown with its children (<n>Set Object Attribute</n> "
            "<v>Visible</v>); its <v>message</v> <v>Pick a button, or Leave</v>.",
            "Leaving: Use again (the keyboard's E; a gamepad's Use is A, which the cursor takes "
            "as its press), Back (<n>Input Action</n> <v>Back</v>, only while at the "
            "computer), or a click on Leave (<n>On UI Event</n> <v>Click</v> on <v>exit</v>, by "
            "this player: <n>Compare</n> of the event's <s>User</s> with the player's number, "
            "both made numbers by <n>Typecast Value</n>). All three go into <n>Any Flow</n>.",
            "<n>Take UI Control</n> <s>Take</s> <v>off</v> gives the buttons back; "
            "<n>Set Player Pointer</n> <v>View Center</v> brings the aim back (as the Aim frame "
            "sets it); <n>Set Camera</n> gives back the camera in the player's <c>camera</c> "
            "property; then <v>at_computer</v> off, status <v>Free</v>, the buttons hidden, "
            "message <v>Locked. Stand at the desk and press Use</v>.",
            "When the game starts, <n>Set Camera Visibility</n> hides the player and its "
            "children from <v>Computer Camera</v>: the computer's view never shows a body in "
            "front of the screen.",
            "Walking, stamina, the pause menu and the crosshair read the same <c>at_computer</c> "
            "and wait while the player is at the computer.",
        ),
        ("Game", "Aim: the middle of the view presses buttons with Use"): (
            "When the game starts, <n>Set Player Pointer</n> <v>View Center</v> makes the middle "
            "of this player's view their pointer: a UI Element in the world (here the terminal's "
            "buttons; the computer's are hidden until a player takes it, and then pressed with "
            "its cursor) that the player looks at within <s>Reach</s> <v>2.5</v> of their "
            "camera is pressed with <v>Use</v> (<s>Press Action</s>), like a click.",
            "Every player aims at the same time. Use goes to the button only while the player "
            "aims at one; else the game gets it as usual (the chest, the computer). It waits "
            "while the player's menu is open and while the game is paused.",
        ),
        ("Computer", "Screen buttons: Gift drops a coin, Lights dims the sun (any pla"): (
            "<n>On UI Event</n> <v>Click</v> (<s>Action</s> empty: any UI Element); "
            "<n>Switch Flow</n> <v>String</v> on its <s>Action</s>: <v>gift</v> or "
            "<v>lights</v> (<c>GiftButton</c>, <c>LightsButton</c>).",
            "Gift: <n>Set Game Property</n> turns the computer's <v>gift</v> on. The Gift frame "
            "adds the coin in the next step.",
            "Lights: <n>Branch</n> on the computer's <v>dim</v>. Dimmed: "
            "<n>Set Light Attribute</n> <v>Power</v> <v>1</v> on <v>Sun</v>, <v>dim</v> off, "
            "<v>message</v> <v>Lights on</v>. Else power <v>0.3</v>, <v>dim</v> on, "
            "<v>Lights dimmed</v>. The sun is one light: both players see it change.",
            "The screen's text <c>ScreenMessage</c> is <c>{Computer.message}</c>: it shows the "
            "property as it changes.",
        ),
        ("Computer", "Gift: a coin appears next to the desk (in a Logic + Physics ste"): (
            "Every step, while the computer's <v>gift</v> is on: <n>Add Object</n> adds a copy "
            "of <v>Coin</v>, the Item Library's coin (Add Object copies objects of a hidden "
            "collection), placed as <v>Gift Spot</v> (<s>Copy Transform</s>). The copy has the "
            "coin's <c>item</c> and <c>count</c>: a pickup.",
            "Then <v>gift</v> off, and the <v>message</v> <v>A coin for you, next to the desk!</v>.",
        ),
        ("Computer", "Button glow: Highlight plays an action on the button"): (
            "<n>On UI Event</n> <v>Highlight Start</v> on the buttons <v>gift</v>, <v>lights</v>, "
            "<v>exit</v>, <v>terminal_gift</v>, <v>together</v> (the computer's and the "
            "terminal's): <n>Play Action</n> plays <v>ButtonGlow</v> on that button, frames "
            "<v>1</v> to <v>5</v>. <v>Highlight End</v> plays it back to frame <v>1</v> from "
            "where it is (<n>Animation Status</n>).",
            "<c>ButtonGlow</c> turns the button's Viewport Display color alpha from 0 to 1. The "
            "buttons' materials read it (Object Info › Alpha) into their Emission Strength, "
            f"{num(low)} to {num(high)}: the button glows. The players' cameras draw with EEVEE, "
            "so a material's nodes show there (a Workbench camera would show only its Viewport "
            "Display color).",
            "This tree runs on <c>Computer</c> but also lights the terminal's buttons: "
            "On UI Event hears every UI Element whose Action it names.",
        ),
        # Terminal ---------------------------------------------------------------
        ("Terminal", "Buttons: any player, also two at once"): (
            "<n>On UI Event</n> <v>Click</v> (<s>Action</s> empty); <n>Switch Flow</n> "
            "<v>String</v> on its <s>Action</s>: <v>terminal_gift</v> (<c>TerminalGift</c>) or "
            "<v>together</v> (<c>TerminalTogether</c>). Both buttons are Any Player UI Elements: "
            "every player presses them with the aim, two players also at the same moment.",
            "Gift: <n>Modify Property</n> <v>Add</v> <v>1</v> to the terminal's <v>gifts</v>, "
            "and the <v>message</v> <v>A coin for each press, next to the terminal</v>.",
        ),
        ("Terminal", "Gifts: a coin a step for each press"): (
            "Every step while the terminal's <v>gifts</v> is above <v>0</v>: "
            "<n>Add Object</n> adds a <v>Coin</v> placed as <v>Terminal Gift Spot</v>, and "
            "<n>Modify Property</n> <v>Subtract</v> takes <v>1</v> from <v>gifts</v>. Ten quick "
            "presses give ten coins, one a step.",
        ),
        ("Terminal", "Open Together: each press counts for 3 seconds"): (
            "<n>Switch Flow</n> <v>Integer</v> on the event's <S>User</S> (who pressed): player "
            "0's press goes to the first <n>Cooldown</n>, player 1's to the second. Other "
            "players' presses do nothing (no case for them).",
            "A press runs that player's Cooldown's <s>Reset</s>, then its <s>Flow</s>: the "
            "<s>Duration</s> of <v>3</v> seconds starts over and <s>Accepted</s> runs. For 3 "
            "seconds after the press its <s>Is Ready</s> is off: the press still counts.",
        ),
        ("Terminal", "Vault: opens if the other player's press still counts"): (
            "After player 0's press, the first <n>Branch</n> reads player 1's <S>Is Ready</S>. "
            "Ready: player 1 has not pressed in the last 3 seconds, and the terminal says "
            "<v>Player 1 ready: Player 2, press within 3 s!</v> (its texts count players from "
            "1). Not ready: both pressed within 3 s. The other <n>Branch</n> is the same for "
            "player 1's press.",
            "Both pressed: <n>Any Flow</n> → <n>Remove Object</n> <v>Vault</v>; the terminal's "
            "<v>vault_open</v> on; <v>TerminalTogether</v> hidden with its label "
            "(<n>Set Object Attribute</n> <v>Visible</v> <v>off</v>); "
            "<v>Together! The vault is open: take the Magic Sword</v>.",
        ),
        ("Terminal", "Too slow: a press ran out, the vault is still shut"): (
            "A Cooldown that ends by itself, 3 seconds after its last press (<S>Completed</S>, "
            "through <n>Any Flow</n>), while the vault is shut (<n>Branch</n> on "
            "<v>vault_open</v>, <s>False</s>): <v>Too slow: press within 3 s of each other</v>.",
        ),
        # HUD pieces ----------------------------------------------------------------
        ("Crosshair", None): (
            "Every frame (<n>On Frame Update</n>), <n>Set Object Attribute</n> <v>Visible</v> "
            "shows the tree's object, the HUD's “+” <c>Crosshair</c>, unless its owner is at "
            "the computer: <n>Get Menu Player</n>'s <s>Owner</s> is the HUD's player, "
            "<n>Get Game Property</n> reads their <v>at_computer</v>, <n>Boolean Combine</n> "
            "<v>Not</v> turns it around.",
        ),
        ("Map Picture", None): (
            "Every frame, <n>Set Object Attribute</n> <v>Visible</v> shows the HUD's <c>Map</c> "
            "with its children, the dots (<s>Include Children</s>), only while the owner's "
            "<v>map_mode</v> is <v>1</v> (<n>Compare</n> <v>Equal</v>).",
        ),
        ("Map Dot", None): (
            "Every step, each dot (<c>Map Dot 0</c>, <c>Map Dot 1</c>) reads the object in its "
            "own <v>track</v> property (<c>Player</c>, <c>Player 1</c>) and sets its "
            "<v>Local Position</v> to that object's world position with Z made 0 "
            "(<n>Vector Math</n> <v>Multiply</v> <v>(1, 1, 0)</v>), plus <v>(0, 0, 0.5)</v>.",
            "The dots hang from <c>Map</c>: they move, scale and hide with it.",
        ),
        ("Minimap", None): (
            "Every frame, <n>Set Camera Viewport</n> draws this camera (<n>Get Owner</n>: "
            "<c>Minimap 0</c> or <c>Minimap 1</c>) in the view of the player in its own "
            "<v>user</v> property (<s>User</s>, <s>Area</s> <v>Player's View</v>): from "
            "<s>Left</s> <v>0.69</v> and <s>Top</s> <v>0.03</v> of that view, <s>Width</s> and "
            "<s>Height</s> <v>0.29</v>, <s>On Top</s> of it.",
            "<s>Enabled</s> only while the player's <c>map_mode</c> is <v>0</v>: the camera's "
            "<v>player</v> property names the player.",
        ),
        # Sounds ------------------------------------------------------------------
        ("UI Sounds", "UI Sounds: the menus' Move, Accept, Back and Denied sounds"): (
            "<n>On UI Sound</n> runs at the menus' sound moments, for every player's UI: "
            "<s>Move</s> (the highlight moved), <s>Accept</s> (a click, a widget changed, a drop "
            "that moved an item), <s>Back</s> (Back closed a menu), <s>Denied</s> (a press that "
            "does nothing). Each runs a <n>Play Sound</n> of the packed sounds <v>UI Move</v>, "
            "<v>UI Accept</v>, <v>UI Back</v>, <v>UI Denied</v>, in the volume <s>Group</s> "
            "<v>UI</v>.",
            "The tree runs on <c>Player</c> only and plays every player's sounds. Back runs "
            "after the menu closed, so the node must not be in a menu's own logic.",
        ),
    }


def chapters(ex):
    """The guide, chapter by chapter: (id, title, HTML)."""
    FRAMES.clear()
    FRAMES.update(frame_words(ex))
    recipe = ex.recipe
    W = ex.words.render
    frames = ex.frames
    in_collection = lambda name: [ob for ob in recipe["objects"] if name in ex.homes.get(ob, [])]
    children = lambda name: [child for child, entry in recipe["objects"].items()
                             if entry["parent"] == name]

    def family(*names):
        """These objects with all their children, in the file's order."""
        out = []
        stack = list(names)
        while stack:
            name = stack.pop(0)
            out.append(name)
            stack[0:0] = children(name)
        return out

    game = ex.scene.game_settings
    out = []

    # 1 -------------------------------------------------------------------------
    require(game.use_input_gamepads_from_player_1, "player 0 has no gamepad")
    require(obj("Player").game.properties["user"].value == 0 and
            obj("Player 1").game.properties["user"].value == 1, "Player is 0, Player 1 is 1")
    out.append(("start", "Play the example", W(f"""
<p>Add › Game UI › <b>Open Split-Screen Example</b> opens a new, unsaved copy of the example.
Play it with Render tab › Game Resolution › Embedded Start (in the 3D view) or Standalone Start
(its own window).</p>
<ul class="rules">
<li><b>Player 0</b> (the object <c>Player</c>, left half) plays with the keyboard and mouse.</li>
<li><b>Player 1</b> (the object <c>Player 1</c>, right half) plays with the first gamepad:
the scene's Gamepads Start at Player 1 is on. Without a gamepad player 1 stands still.</li>
<li>Walk to the chest, the computer (left) and the public terminal, pick items up, open the
bag, eat, craft, equip, change the settings, pause.</li>
</ul>
<p class="callout"><b>Player numbers.</b> The nodes count players from 0: <s>User</s> 0 is
<c>Player</c>, 1 is <c>Player 1</c>. The texts on screen and a few frame titles count from 1:
there “Player 1” means player 0 and “Player 2” player 1. This guide always counts from 0.</p>
<p>Scene settings that are not as in a new file:</p>
{ex.scene_settings()}
""")))

    # 2 -------------------------------------------------------------------------
    out.append(("ideas", "How it works", W(f"""
<div class="scroll">{ex.window_diagram()}</div>
<ol class="ideas">
<li><b>One tree for every player.</b> <c>Player</c> and <c>Player 1</c> run the same logic tree,
<c>Game</c>. Each has a game property <c>user</c> (its number); every node that acts for a player
takes it as its <s>User</s>.</li>
<li><b>A camera for each player splits the window.</b> <b>Set Camera</b> with <s>User</s> 1 gives
player 1 a view of their own; <b>Set Split Screen</b> lays the views out.</li>
<li><b>Each player gets their own copy of a menu.</b> <b>Open Object Menu</b> with a <s>User</s>
and <s>Area</s> Player's View opens that player's copy, inside their view. The copy's logic runs
for them: <b>Get Menu Player</b> gives its player and its owner (the player object), and widgets,
texts and grids read the owner's properties.</li>
<li><b>Each camera can draw something else.</b> <b>Set Camera Visibility</b> hides a player's own
body from their own camera; <b>Set Camera Viewport</b> draws a minimap camera inside a player's
view.</li>
<li><b>Input belongs to a player.</b> <b>Input Action</b> reads an action of the Game Input Map
for its <s>User</s>. Player 0 has the keyboard and the mouse; the gamepads go to the players
after them. Players aim at buttons in the world with the middle of their view
(<b>Set Player Pointer</b> View Center).</li>
</ol>
<h3>The UI in short</h3>
<ul class="rules">
<li>A menu (and the HUD) is a <b>collection</b>: here inside the collection <c>UI</c>, which is
Disable in Viewports so its objects are not in the game world. <b>Open Object Menu</b> shows it
through a camera of its own, <c>InvCamera</c>.</li>
<li>A <b>UI Element</b> is an object with Game Components tab › UI Element on: it takes clicks,
focus and drags. <b>On UI Event</b> reports what happens to it, with its <s>Action</s> name.</li>
<li><b>Widgets</b> (Slider, Toggle, Selector, Text Field, Bar) show and change a game property by
themselves: the element's own or a parent's, else the menu owner's.</li>
<li><b>Live text</b>: braces in a text show values and keys, and update by themselves:
<c>{{player_name}}</c>, <c>{{Computer.message}}</c>, <c>{{Use}}</c>.</li>
<li>The menus here are built as in the <b>UI Guide</b> (Add › Game UI › Open UI Guide), which
explains each piece step by step.</li>
</ul>""")))

    # 3 -------------------------------------------------------------------------
    out.append(("split", "The split screen", W(f"""
<p>One small tree, <c>Split Screen</c>, sets up the two views when the game starts.</p>
{frames("Split Screen", None)}
{ex.objects_table(["Split Screen"], {"Split Screen": "runs the Split Screen tree"},
                  "The object")}
<p>The cameras <c>Camera 0</c> and <c>Camera 1</c> hang from the players' heads (next chapter).
The scene's camera is <c>{ex.scene.camera.name}</c>, player 0's.</p>""")))

    # 4 -------------------------------------------------------------------------
    players = family("Player", "Player 1")
    minimaps = ["Minimap 0", "Minimap 1"]
    players = [name for name in players if name not in minimaps]
    require(all(obj(name).game.properties["head"].value == obj(head) and
                obj(name).game.properties["camera"].value == obj(camera)
                for name, head, camera in (("Player", "Head 0", "Camera 0"),
                                           ("Player 1", "Head 1", "Camera 1"))),
            "each player's head and camera properties name its own")
    require(bpy.data.materials["Text One Side"].use_backface_culling,
            "the name tags' material shows only its front")
    require(not obj("NameTag").visible_shadow, "the name tags cast no shadow")
    out.append(("players", "The players", W(f"""
<p>The two players are the same object twice, with another number, camera and head. Each is a
Dynamic capsule that does not tip over (its rotation is locked); a head (an empty) sits at eye
height with the camera on it.</p>
{ex.objects_table(players, {
        "Player": "player 0: walks, and holds every value the menus show",
        "Player 1": "player 1: the same, with its own values",
        "Head 0": "turns up and down (Mouse Look's Head)",
        "Head 1": "the same for player 1",
        "Camera 0": "player 0's view",
        "Camera 1": "player 1's view",
        "NameTag": "the name over the player: <c>{player_name}</c> of its parent",
        "NameTag Back": "the same text facing back (one-sided texts: material "
                        "<c>Text One Side</c> has Backface Culling)",
        "Stamina Bar.001": "the stamina bar over the player (a Bar widget of <c>stamina</c>)",
        "Stamina Bar.002": "player 1's",
        "NameTag.001": "player 1's name over it",
        "NameTag.001 Back": "the same facing back",
    }, "The players' objects")}
<p>Their game properties, side by side:</p>
{ex.player_properties()}
{frames("Game", "Whose: this player's number and object",
        "Look: the mouse (player 1), the right stick (gamepads)",
        "Walk with the Move action at the Settings speed",
        "Stamina: drains while walking, comes back while standing",
        "First person: hide your own body (not its shadow)")}""")))

    # 5 -------------------------------------------------------------------------
    out.append(("input", "Controls", W(f"""
<p>Scene tab › Game Input Map. The UI controls a new scene has are kept; the game's actions are
added. <b>Input Action</b> reads an action for one player (<s>User</s>): player 0 uses the keyboard
and mouse, player 1 the first gamepad (Gamepads Start at Player 1; the second gamepad would be
player 2).</p>
{ex.input_map()}
<ul class="rules">
<li><b>Context</b>: <c>Sort</c> works only while the top menu's Input Context is
<c>Inventory</c>. The others work always.</li>
<li><b>Esc</b> is Back (the UI's), <c>Pause Menu</c> and the scene's Exit Key. Esc closes the
player's open menu (the UI uses it, and an Input Action ignores input the UI used). The HUD's Back
is Ignore, so with only the HUD open the game gets Esc: it opens the pause menu, and because an
action has it, it does not end the game.</li>
<li>Texts show an action's key by its name: <c>{{Use}}</c>, <c>{{Use:Keyboard}}</c>,
<c>{{Use:Gamepad}}</c>.</li>
</ul>""")))

    # 6 -------------------------------------------------------------------------
    hud = [name for name in in_collection("HUD") if name not in
           ("Map", "Map Dot 0", "Map Dot 1")]
    out.append(("hud", "The HUD", W(f"""
<p>The HUD is a menu too: the collection <c>HUD</c>, opened for each player when the game starts.
Each copy sits in its player's view and reads its player's properties.</p>
{frames("Game", "HUD: show it from the start")}
{ex.objects_table(hud + ["InvCamera"], {
        "HUD Root": "holds the HUD pieces",
        "Crosshair": "a “+” in the middle (Screen Anchor Center); runs the Crosshair tree",
        "Help Line": "the controls (Screen Anchor Bottom), keys by name: <c>{Use}</c>",
        "Messages": "pickup and eating messages (Show Message writes it)",
        "Value Text": "the owner's name and walk speed",
        "Stamina Bar": "a Bar widget of the owner's <c>stamina</c>",
        "QuickBar": "the quick bar's background; runs the Quick Bar tree",
        "QuickBarSlot": "the quick bar's one cell (the grid's template)",
        "QuickBarKey": "the Quick key's name",
        "InvCamera": "the camera every menu and the HUD are seen through (Use Workbench)",
    }, "The HUD's objects, and the menu camera")}
{frames("Crosshair", None)}
{frames("Quick Bar", "Quick bar: the quick slot; Quick uses it")}""")))

    # 7 -------------------------------------------------------------------------
    minimap_height = (obj("Minimap 0").matrix_world.translation.z -
                      obj("Player").matrix_world.translation.z)
    require(obj("Minimap 1").data.viewport_shape == 'CIRCLE' and
            obj("Minimap 0").data.viewport_shape == 'RECTANGLE', "player 1's minimap is round")
    require(all(obj(name).data.type == 'ORTHO' and obj(name).data.use_workbench and
                obj(name).parent == obj(player) for name, player in
                (("Minimap 0", "Player"), ("Minimap 1", "Player 1"))), "the minimaps")
    out.append(("map", "Map and minimap", W(f"""
<p>The Map action cycles the player's <c>map_mode</c>: 0 shows a live minimap (a camera over
the player), 1 a picture with dots, 2 nothing.</p>
{frames("Game", "Map: M or R3 cycles live map, picture, off")}
<p>The minimap cameras are children of the players, {num(minimap_height)} m above them, looking
down, orthographic, drawn with Workbench (cheap: no lights, no shadows). <c>Minimap 1</c>'s
Viewport Shape is Circle: player 1's minimap is an oval filling the same rectangle as player
0's, to show both (Width and Height are parts of the view's width and height: in a tall half
view the rectangle is taller than wide).</p>
{frames("Minimap", None)}
{frames("Map Picture", None)}
{frames("Map Dot", None)}
{ex.objects_table(minimaps + ["Map", "Map Dot 0", "Map Dot 1"], {
        "Minimap 0": "player 0's minimap camera; runs the Minimap tree",
        "Minimap 1": "player 1's",
        "Map": "the HUD's map picture (Screen Anchor Top Right); runs the Map Picture tree",
        "Map Dot 0": "player 0's dot; runs the Map Dot tree",
        "Map Dot 1": "player 1's dot",
    }, "The map's objects")}""")))

    # 8 -------------------------------------------------------------------------
    count, pickups_table = ex.pickups_table()
    sword = next(ob for ob in bpy.data.objects if ob.name.startswith("Pickup_") and
                 ob.game.properties["item"].value == "Magic Sword")
    require((sword.matrix_world.translation - obj("Vault").matrix_world.translation).length < 2.0,
            "the Magic Sword waits at the vault")
    out.append(("items", "Items and pickups", W(f"""
<p>An item is an object in the scene's Item Library (Scene tab › Items and UI): the collection
<c>ItemLibrary</c>, inside the hidden <c>UI</c>. The object's name is the item's name; its game
properties say how it shows and stacks (<c>name</c>, <c>max_stack</c>, <c>category</c>,
<c>description</c>) and what it does here (<c>restore</c>: stamina when eaten;
<c>durability</c>: fills the tooltip's “Durability {{durability}}”). The menus show its mesh as the
item's icon. The durability bar on a cell reads the slot's own <c>durability</c>, a column of the
list: only <c>Player</c>'s inventory has it (its starting Sword at 60); added and crafted items get
no value there, so their bar stays hidden.</p>
{ex.items_table()}
<p>A pickup is any object in the world with the game properties <c>item</c> (an Item Library
name) and <c>count</c>, a Ghost so the player walks through it. The example has {count}, of
these kinds (the Magic Sword lies at the vault):</p>
{pickups_table}
{frames("Game", "Pick up: add each touched item to the inventory, remove it once")}""")))

    # 9 -------------------------------------------------------------------------
    inventory = in_collection("InventoryMenu")
    out.append(("inventory", "The inventory", W(f"""
<p>The bag: the collection <c>InventoryMenu</c>, opened per player in their view. Its tree,
<c>Inventory</c>, runs on its panel <c>Panel</c> while the menu is open, once for each player's
copy.</p>
{frames("Game", "Open or close the inventory with I or gamepad Select")}
{ex.shot("inventory", "Player 0's inventory open in their view; player 1 plays on.")}
{frames("Inventory", "Whose menu: its player and their character",
        "Grid: 3 x 3 copies of Slot; it moves, stacks, combines, equips ",
        "Pages: < and > turn the page, Page / Page Count between",
        "Tabs and Find: show some categories, or items by name",
        "Delete: left click a slot (or Enter / A) removes 1 item",
        "Sort with the Sort action, then show the top",
        "Settings button: open Settings on top of the inventory",
        "Cell look: Highlight and Refused play actions on the cell's mat")}
{ex.action_table(["SlotHighlight", "SlotRefused"])}
{frames("Inventory", "Durability: each cell's bar shows its item's own durability")}
{frames("Game", "Use: Use Item on an item, or Quick, eats or drinks it")}
{ex.objects_table(inventory, {
        "Panel": "the menu's background: every piece hangs from it; runs the Inventory tree, "
                 "holds <c>categories</c> and <c>search</c>",
        "Slot": "the first cell: the grid copies it (with its children)",
        "SlotBar": "the durability bar (a Bar widget of the cell's <c>durability</c>)",
        "ScrollBar": "the bag's scrollbar",
        "PageText": "“Page 1 / 1”",
        "ItemName": "a line for hints and messages",
        "DropHint": "the controls",
        "TabMarker": "marks the chosen tab",
        "SearchField": "the Find field: a Text Field widget of the panel's <c>search</c>",
        "StackToggle": "the Stack pickups checkbox: a Toggle of the owner's <c>stack_items</c>",
        "EquipPlate": "the equipment column's background",
        "WeaponSlot": "the WEAPON cell",
        "ArmorSlot": "the ARMOR cell",
        "QuickSlot": "the QUICK cell",
        "CraftPlate": "the recipe list's background",
        "RecipeSlot": "the first recipe cell: the grid copies it",
        "CraftScrollBar": "the recipe list's scrollbar",
        "SettingsButton": "opens Settings",
        "Tooltip": "the cells' tooltip background (Show In Renders off: the UI shows it)",
    }, "The inventory's objects")}""")))

    # 10 ------------------------------------------------------------------------
    rules = game.inventory_rules["Player bag"]
    require(rules.recipes == "recipes" and rules.use_stack and rules.use_keep_places,
            "the Player bag rules")
    out.append(("equip", "Equipment, quick slot and crafting", W(f"""
<p>The equipment cells are lists of their own: a weapon is moved from <c>inventory</c> into
<c>weapon</c> by dragging it onto the WEAPON cell. The quick cell's list, <c>quick</c>, is also
on the HUD.</p>
{frames("Inventory", "EQUIP: each cell shows its own list; Accept Categories",
        "Craft: click or A on a recipe; the grid shows what it made")}
<p>Both players' bags follow the Inventory Rules <c>Player bag</c> (Scene tab › Items and UI ›
Inventory Rules, chosen on the list's game property): items stack, an emptied slot keeps its
place, and the recipes are the owner's <c>recipes</c>: dropping one ingredient on another makes
a two-item recipe's result. Nodes with <s>Override</s> on (Pick up) use their own settings
instead.</p>
{ex.inventory_rules()}""")))

    # 11 ------------------------------------------------------------------------
    chest_menu = in_collection("ChestMenu")
    out.append(("chest", "The chest", W(f"""
<p>The chest is an object in the world with a list, <c>chest</c>. Its menu, <c>ChestMenu</c>,
shows that list and the player's bag side by side.</p>
{frames("Game", "Open or close the chest with Use when next to it")}
{ex.shot("chest", "Player 1's chest menu in their view, while player 0 plays on.")}
{frames("Chest", "Chest list: the chest's items with their names",
        "Bag: your inventory as a grid")}
{ex.objects_table(["Chest", "ChestLabel"] + chest_menu, {
        "Chest": "the chest in the world; holds the <c>chest</c> table",
        "ChestLabel": "its label: <c>CHEST: {Use}</c>",
        "ChestPanel": "the menu's background; runs the Chest tree",
        "ChestRow": "the first chest row: the grid copies it",
        "BagSlot": "the first bag cell: the grid copies it",
        "ChestHint": "the controls",
        "ChestTooltip": "the bag cells' tooltip background",
    }, "The chest's objects")}""")))

    # 12 ------------------------------------------------------------------------
    settings_menu = in_collection("SettingsMenu")
    out.append(("settings", "Settings", W(f"""
<p>The Settings menu opens from the inventory (in the player's view) and from the pause menu
(over the window). Most of it needs no logic: its widgets change the owner's properties by
themselves.</p>
<ul class="rules">
<li><c>WalkSlider</c>: a Slider of <c>walk_speed</c>, from
{num(obj("WalkSlider").game.ui.widget_min)} to {num(obj("WalkSlider").game.ui.widget_max)} in
steps of {num(obj("WalkSlider").game.ui.widget_step)}; <c>WalkValue</c> shows
<c>{{walk_speed:2}}</c>.</li>
<li><c>SortSelector</c>: a Selector of the Choice property <c>sort_by</c> (Name, Count).</li>
<li><c>NameField</c>: a Text Field of <c>player_name</c> (typed with player 0's keyboard
only), at most
{num(obj("NameField").game.ui.widget_max)} letters.</li>
<li><c>HintsToggle</c>: a Toggle of <c>show_hints</c>; <c>RebindButton</c> rebinds Grab.</li>
</ul>
{frames("Settings", "Whose menu: its player and their character",
        "Show hints toggle: show or hide the hint in the world",
        "Grab key: the next key pressed becomes Grab", "Close: back to the menu below")}
{ex.objects_table(settings_menu, {
        "SettingsPanel": "the menu's background; runs the Settings tree",
        "CloseButton": "closes Settings",
        "WalkSlider": "a Slider of <c>walk_speed</c>",
        "WalkHandle": "its handle",
        "WalkValue": "shows <c>{walk_speed:2}</c>",
        "SortSelector": "a Selector of <c>sort_by</c>",
        "HintsToggle": "a Toggle of <c>show_hints</c>",
        "NameField": "a Text Field of <c>player_name</c>",
        "RebindButton": "rebinds Grab",
        "SettingsHint": "the controls",
    }, "The Settings menu's objects")}""")))

    # 13 ------------------------------------------------------------------------
    pause = in_collection("Pause Menu")
    out.append(("pause", "The pause menu", W(f"""
<p>Unlike the other menus, the pause menu covers the whole window and stops the game for both
players.</p>
{frames("Game", "Pause Menu: open with the Pause Menu action")}
{ex.shot("pause", "The pause menu over the whole window.")}
{frames("Pause Menu", "Whose menu: its player and their character",
        "Resume: close the pause menu (the game goes on)",
        "Settings: open the Settings menu on top", "Quit: end the game")}
{ex.objects_table(pause, {
        "Pause Menu Panel": "the menu's background; runs the Pause Menu tree",
        "Resume": "closes the menu",
        "Settings": "opens Settings",
        "Quit": "ends the game",
    }, "The pause menu's objects")}""")))

    # 14 ------------------------------------------------------------------------
    computer = family("Computer")
    out.append(("computer", "The computer: one player at a time", W(f"""
<p>A UI in the world that one player takes over: they see it through its own camera and move a
cursor on its screen. Its buttons are hidden until someone uses it.</p>
{frames("Game", "Computer: Use next to it to use it; Use, Back or Leave gives it",
        "Aim: the middle of the view presses buttons with Use")}
{frames("Computer", "Screen buttons: Gift drops a coin, Lights dims the sun (any pla",
        "Gift: a coin appears next to the desk (in a Logic + Physics ste",
        "Button glow: Highlight plays an action on the button")}
{ex.action_table(["ButtonGlow"])}
{ex.objects_table(computer + ["Computer Camera", "ComputerLabel", "Gift Spot"], {
        "Computer": "the desk; holds <c>status</c>, <c>message</c>, <c>dim</c>, <c>gift</c>; "
                    "runs the Computer tree",
        "Screen": "the Surface pointer moves on it",
        "Cursor": "the pointer on the screen (Show In Renders off: Set Player Pointer shows it)",
        "Screen Buttons": "holds the buttons: shown while a player uses the computer",
        "GiftButton": "Action <c>gift</c>",
        "LightsButton": "Action <c>lights</c>",
        "LeaveButton": "Action <c>exit</c>",
        "ScreenMessage": "<c>{Computer.message}</c>",
        "StatusText": "<c>{Computer.status}</c>",
        "Computer Camera": "the view of a player at the computer",
        "ComputerLabel": "<c>COMPUTER: {Use}</c>",
        "Gift Spot": "where the gift coin appears",
    }, "The computer's objects")}""")))

    # 15 ------------------------------------------------------------------------
    terminal = family("PublicTerminal")
    out.append(("terminal", "The public terminal: everyone at once", W(f"""
<p>The opposite of the computer: nobody takes it over, every player presses its buttons with
their aim, also two at the same moment. Open Together needs both players within 3 seconds.</p>
{frames("Terminal", "Buttons: any player, also two at once",
        "Gifts: a coin a step for each press",
        "Open Together: each press counts for 3 seconds",
        "Vault: opens if the other player's press still counts",
        "Too slow: a press ran out, the vault is still shut")}
{ex.objects_table(terminal + ["Terminal Gift Spot"] + family("Vault"), {
        "PublicTerminal": "holds <c>message</c>, <c>gifts</c>, <c>vault_open</c>; runs the "
                          "Terminal tree",
        "TerminalGift": "Action <c>terminal_gift</c>, Any Player",
        "TerminalTogether": "Action <c>together</c>, Any Player",
        "TerminalMessage": "<c>{PublicTerminal.message}</c>",
        "Terminal Gift Spot": "where the terminal's coins appear",
        "Vault": "removed when both press Open Together",
    }, "The terminal's objects")}""")))

    # 16 ------------------------------------------------------------------------
    out.append(("sounds", "Menu sounds", W(f"""
{frames("UI Sounds", "UI Sounds: the menus' Move, Accept, Back and Denied sounds")}
{ex.sounds_table()}""")))

    # 17 ------------------------------------------------------------------------
    require(not any(binding.tree for name in ("MainCamera", "Camera_Parent")
                    for binding in obj(name).game.logic_node_bindings),
            "MainCamera and Camera_Parent run no logic")
    for tree in bpy.data.node_groups:
        if tree.bl_idname == "LogicNodeTree":
            for node in tree.nodes:
                for socket in node.inputs:
                    value = getattr(socket, "default_value", None)
                    require(not (isinstance(value, bpy.types.Object) and
                                 value.name in ("MainCamera", "Camera_Parent")),
                            "no node uses MainCamera or Camera_Parent")
    require(ex.scene.camera != obj("MainCamera"), "MainCamera is not the scene's camera")
    world = ["Ground", "Sun", "Hint", "Help Board", "MainCamera", "Camera_Parent"]
    world = [name for name in recipe["objects"] if name in world]
    out.append(("world", "The rest of the world", W(f"""
{ex.objects_table(world, {
        "Ground": "the floor",
        "Sun": "the light the computer's Lights button dims",
        "Hint": "the controls, in the world: the Settings checkbox shows or hides it",
        "Help Board": "the board behind it",
        "MainCamera": "not used: the players' cameras draw the game",
        "Camera_Parent": "not used (MainCamera's parent)",
    }, "The world's objects")}""")))

    # 18 ------------------------------------------------------------------------
    split = node_of(bpy.data.node_groups["Split Screen"], "LogicNativeSetSplitScreen")
    three = enum_label(split.bl_rna, "split_three_players", split.split_three_players)
    out.append(("own", "Make your own", W(f"""
<h3>A split-screen game</h3>
<ol class="ideas">
<li>Make one player object with its logic tree, a game property <c>user</c> (Integer) and a
camera (on a head empty, for looking up and down). Link <c>user</c> into every <s>User</s>
input of the tree.</li>
<li>Copy the player (with its children) for each other player and give each copy its
<c>user</c>: 1, 2...</li>
<li>When the game starts, <b>Set Camera</b> with each player's <s>User</s> and camera, then
<b>Set Split Screen</b>.</li>
<li>Scene tab › Game Input Map › Settings › Gamepads Start at Player 1 for a keyboard player
and gamepad players; off, player 0 also has the first gamepad.</li>
<li>Open menus with <b>Open Object Menu</b>, <s>User</s> the player, <s>Area</s> Player's View;
in the menu's logic use <b>Get Menu Player</b>.</li>
<li><b>Set Camera Visibility</b> with Keep Shadow hides a first-person body from its own
camera.</li>
</ol>
<h3>A third player in the example</h3>
<ol class="ideas">
<li>Select <c>Player 1</c> with its children and copy them (Shift D): the copy's
<c>camera</c> and <c>head</c> and its minimap's <c>player</c> already name the copies. Set the
copy's <c>user</c> to 2, and the copied minimap camera's <c>user</c> to 2.</li>
<li>Add a <b>Set Camera</b> with <s>User</s> 2 and the new camera to the <c>Split Screen</c>
tree. <b>Set Split Screen</b>'s 3 Players is {esc(three)} already.</li>
<li>For the map picture, copy <c>Map Dot 1</c> and set its <c>track</c> to the new player.</li>
<li>The terminal's Open Together knows players 0 and 1 only (its Switch Flow cases).</li>
<li>Player 2 plays with the second gamepad.</li>
</ol>""")))
    return out


CSS_EXTRA = """
.callout { background:var(--note-bg); color:var(--note-fg); border-radius:8px; padding:10px 14px; }
ol.ideas li { margin:6px 0; }
.frame figcaption .on { float:right; color:var(--muted); font-weight:normal; font-size:13px; }
details.objects { margin:14px 0; border:1px solid var(--rule); border-radius:8px;
  padding:6px 12px; background:var(--surface); }
details.objects > summary { font-weight:600; }
code.v { background:var(--accent-soft); }
.diagram-box { margin:14px 0; }
.diagram-box figcaption { color:var(--muted); font-size:13px; text-align:center; }
svg.diagram { width:100%; max-width:640px; height:auto; display:block; margin:0 auto; }
.n-stub { paint-order:stroke; stroke:var(--canvas); stroke-width:4px; stroke-linejoin:round; }
.d-view { fill:var(--accent-soft); stroke:var(--accent); stroke-width:1.5; }
.d-gap { fill:#000; }
.d-mini { fill:var(--surface); stroke:var(--accent); stroke-width:1.5; }
.d-label { font:600 18px var(--font); fill:var(--fg); text-anchor:middle; }
.d-small { font:12px var(--font); fill:var(--muted); text-anchor:middle; }
.d-cross { font:600 22px var(--font); fill:var(--fg); text-anchor:middle; }
"""


def INTRO(ex):
    return ex.words.render(f"""<header><h1>Split-Screen Guide</h1>
<p class="lede">How the Split-Screen FPS Example is made: two players in one window, each with
their own view, HUD, minimap and menus (an inventory with equipment and crafting, a chest,
settings), a pause menu for both, a computer one player takes over and a terminal everyone
uses. Open it with Add › Game UI › Open Split-Screen Example and follow along: every logic frame
of the example is pictured here with what it does.</p></header>
{ex.shot("split", "The game starts: player 0 left, player 1 right, each with a HUD and a minimap.")}""")


def page(ex):
    content = chapters(ex)
    trees = {tree.name: tree for tree in bpy.data.node_groups if tree.bl_idname == "LogicNodeTree"}
    all_frames = set()
    for name, tree in trees.items():
        frames = [node.label for node in tree.nodes if node.bl_idname == "NodeFrame"]
        all_frames |= {(name, label) for label in frames} if frames else {(name, None)}
    missing_frames = sorted(all_frames - ex.shown_frames, key=str)
    missing_objects = sorted(set(ex.recipe["objects"]) - ex.shown_objects)
    used_materials = {name for entry in ex.recipe["objects"].values()
                      for name in entry["materials"] if name}
    missing_materials = sorted(used_materials - ex.shown_materials)
    missing_actions = sorted({action.name for action in bpy.data.actions if action.users} -
                             ex.shown_actions)
    extra_frames = sorted(set(FRAMES) - all_frames, key=str)
    if missing_frames or missing_objects or missing_materials or missing_actions or extra_frames:
        raise RuntimeError("The guide and the example differ:\n"
                           f"  frames not explained: {missing_frames}\n"
                           f"  words for frames the example lacks: {extra_frames}\n"
                           f"  objects: {missing_objects}\n  materials: {missing_materials}\n"
                           f"  actions: {missing_actions}")
    toc = "".join(f'<a href="#{cid}">{index}. {esc(title)}</a>'
                  for index, (cid, title, _body) in enumerate(content, 1))
    sections = []
    for index, (cid, title, body) in enumerate(content, 1):
        sections.append(f'<section id="{cid}"><h2>{index}. {esc(title)}</h2>{body}</section>')
    sections = [re.sub(r'(?<!<div class="scroll">)(<table\b.*?</table>)',
                       r'<div class="scroll">\1</div>', section, flags=re.S)
                for section in sections]
    source = os.path.join(HERE, EXAMPLE_FILE)
    digest = hashlib.sha1(open(source, "rb").read()).hexdigest()[:12]
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Split-Screen Guide</title><style>{G.CSS}{CSS_EXTRA}</style></head>
<body><div class="wrap">
{INTRO(ex)}
<nav class="toc">{toc}</nav>
{"".join(sections)}
<footer>Made by make_split_screen_guide.py from {EXAMPLE_FILE} ({digest}).</footer>
</div></body></html>
"""


def without_pictures(text):
    """A page without its screenshots (and without the notes where a picture is missing)."""
    text = re.sub(r'<figure class="shot">.*?</figure>', "<shot/>", text, flags=re.S)
    return re.sub(r'<p class="missing">.*?</p>', "<shot/>", text, flags=re.S)


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    check = "--check" in args
    args = [arg for arg in args if arg != "--check"]
    shots = args[0] if args else ""
    output = args[1] if len(args) > 1 else os.path.join(HERE, GUIDE_FILE)
    if bpy.data.filepath:
        raise RuntimeError("start Blender with --factory-startup and no file: the guide compares "
                           "the example with a new file")
    start = G.start_values()
    bpy.ops.wm.open_mainfile(filepath=os.path.join(HERE, EXAMPLE_FILE), load_ui=False)
    demo = Demo(shots, start)
    text = page(demo)
    if check:
        # The shipped page must be this one (its pictures aside): a change of the example or the
        # words without a new page fails.
        with open(os.path.join(HERE, GUIDE_FILE), encoding="utf-8") as stream:
            shipped = stream.read()
        if without_pictures(shipped) != without_pictures(text):
            raise RuntimeError(f"{GUIDE_FILE} is not the page the example and the words make "
                               "now: write it again (see the top of this script)")
        print(f"GUIDE checked: the words and {GUIDE_FILE} match {EXAMPLE_FILE}")
        return
    with open(output, "w", encoding="utf-8") as stream:
        stream.write(text)
    print(f"GUIDE written: {output} ({len(text) // 1024} KB)")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback
        traceback.print_exc()
        sys.stdout.flush()
        os._exit(1)
    sys.stdout.flush()
    os._exit(0)
