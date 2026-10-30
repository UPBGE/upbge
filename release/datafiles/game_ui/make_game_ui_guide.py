# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Write the UI guide (game_ui_guide.html) from the Complete UI Example (game_ui_example.blend).

  blender --background --factory-startup --python make_game_ui_guide.py -- SHOTS_DIR [OUTPUT.html]

SHOTS_DIR holds the pictures guide_screenshots.py takes (world.png, inventory.png, ...). OUTPUT
defaults to game_ui_guide.html next to this script. Run it again whenever the example changes.

Everything the guide shows of the example comes from the file: the objects and their settings
(only those that are not as a new file and the Add menu make them), the node pictures, the
numbered marks on the screenshots. The words around them are here. The script stops with an error
when the example has an object, a logic frame or a material this guide does not explain, or a
setting a table or picture cannot show, so the guide cannot fall behind the example unnoticed.
"""

import base64
import collections
import hashlib
import html
import math
import os
import re
import sys

import bpy
import numpy
from mathutils import Matrix, Vector

from _bpy_internal.game_ui import recipe as R
from _bpy_internal.game_ui import templates as T

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
NODE_SOURCES = os.path.join(SOURCE_ROOT, "source", "blender", "nodes", "logic", "nodes")


def esc(text):
    return html.escape(str(text), quote=True)


# -----------------------------------------------------------------------------
# Numbers and values as a creator types them


def num(value):
    """A number as short as it reads: 1.2, 0.08, 100, -0.775 (a plain minus: it can be pasted)."""
    if isinstance(value, bool):
        return "on" if value else "off"
    if isinstance(value, int):
        return str(value)
    if abs(value - round(value)) < 1e-6:
        return str(int(round(value)))
    return f"{value:.4f}".rstrip("0").rstrip(".")


def vec(values):
    return "(" + ", ".join(num(v) for v in values) + ")"


def value_text(value):
    if value is None:
        return "empty"
    if isinstance(value, dict) and "id" in value:
        return value["name"]
    if isinstance(value, bool):
        return "on" if value else "off"
    if isinstance(value, (int, float)):
        return num(value)
    if isinstance(value, list):
        return vec(value[:3] if len(value) == 4 and abs(value[3] - 1.0) < 1e-6 else value)
    if value == "":
        return "empty"
    return "“" + value.replace("\n", "↵") + "”"


def line_break_note(how, target, value):
    """How to type a value with a line break: Blender's text fields take only one line."""
    return (f'<p class="note">↵ is a line break, which a text field can\'t take. {esc(how)} '
            f'type in the Python Console: <code>{esc(target)} = {esc(repr(value))}</code></p>')


# -----------------------------------------------------------------------------
# What the node sources say


def _statements(source, start):
    """The text of a C++ statement from `start` to its `;` (not one inside a string, nor inside
    the brackets of a lambda it passes)."""
    position = start
    in_string = False
    depth = 0
    while position < len(source):
        char = source[position]
        if in_string:
            if char == "\\":
                position += 1
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        elif char in "({[":
            depth += 1
        elif char in ")}]":
            depth -= 1
        elif char == ";" and depth <= 0:
            return source[start:position]
        position += 1
    return source[start:]


def node_idnames(source):
    """The logic node types a node source file registers."""
    return re.findall(r'logic_node_type_base\(\s*&\w+\s*,\s*"(LogicNative\w+)"_ustr', source)


def prop_label(struct_rna, identifier):
    prop = struct_rna.properties.get(identifier)
    return prop.name if prop is not None and prop.name else identifier.replace("_", " ").title()


def enum_label(struct_rna, identifier, value):
    prop = struct_rna.properties.get(identifier)
    if prop is None or prop.type != 'ENUM':
        return value
    for item in prop.enum_items:
        if item.identifier == value:
            return item.name
    return value


# -----------------------------------------------------------------------------
# Node pictures: each frame as the node editor shows it


SOCKET_COLORS = {
    "NodeSocketLogicExecution": "var(--s-exec)", "NodeSocketLogicObject": "var(--s-object)",
    "NodeSocketLogicString": "var(--s-string)", "NodeSocketLogicInt": "var(--s-int)",
    "NodeSocketLogicFloat": "var(--s-float)", "NodeSocketLogicBool": "var(--s-bool)",
    "NodeSocketLogicCondition": "var(--s-bool)", "NodeSocketLogicVector": "var(--s-vector)",
    "NodeSocketLogicList": "var(--s-list)", "NodeSocketLogicDictionary": "var(--s-dict)",
    "NodeSocketLogicCollection": "var(--s-collection)", "NodeSocketLogicGeneric": "var(--s-generic)",
}
HEADER_KINDS = (
    ("event", ("OnInit", "OnFixedUpdate", "OnUpdate", "OnCollision", "InputAction", "OnUIEvent",
               "OnUISound")),
    ("ui", ("OpenObjectMenu", "CloseObjectMenu", "ObjectGridView", "ScrollObjectView",
            "SetOverlayCollection", "RebindInputAction", "SetText", "PlaySound")),
    ("data", ("Inventory", "GetItemInfo", "GetGameProperty", "GetGamePropertyBool",
              "GetGamePropertyFloat", "GetObjectAttribute", "GetDistance", "DropItem")),
    ("object", ("ModifyProperty", "SetObjectAttribute", "ApplyMovement", "RemoveObject",
                "QuitGame")),
    ("flow", ("Branch", "SwitchFlowString", "AnyFlow")),
    ("math", ("VectorMath", "Compare", "FormattedString", "StringOperation")),
)
ROW = 22
HEADER = 26
# Longer input values are cut short in a picture, and given in full under it.
PICTURE_VALUE_LENGTH = 24
# The widest empty space between two columns of nodes in a picture.
COLUMN_GAP = 70
NODE_SKIP_PROPS = R.NODE_SKIP | {"panel_states"}


def header_kind(idname):
    short = idname.replace("LogicNative", "")
    for kind, names in HEADER_KINDS:
        if short in names:
            return kind
    return "flow"


def text_width(text):
    """About how wide a text is in a node picture (12 px), on the wide side."""
    width = 0.0
    for char in text:
        if char.isupper() or char in "mwMW@%":
            width += 8.2
        elif char.isdigit():
            width += 7.0
        elif char in " .,:;|!'()[]{}il":
            width += 3.8
        else:
            width += 6.6
    return width


def shorten(text, room):
    """The text cut with … to fit `room` (at least its first 3 characters)."""
    if text_width(text) <= room:
        return text
    for end in range(len(text) - 1, 2, -1):
        if text_width(text[:end] + "…") <= room:
            return text[:end].rstrip() + "…"
    return text[:3] + "…"


def fit_row(label, value, room):
    """A label and a value side by side in `room`: the label is cut first, as Blender does (to
    its first 4 characters), then the value. Returns (label, value, the value was cut)."""
    gap = 12
    if text_width(label) + text_width(value) + gap <= room:
        return label, value, False
    label = shorten(label, max(text_width(label[:4] + "…"), room - text_width(value) - gap))
    if text_width(label) + text_width(value) + gap <= room:
        return label, value, False
    short = shorten(value, room - text_width(label) - gap)
    return label, short, short != value


def _function_body(source, name):
    """The text of a C++ function's body, from its braces."""
    match = re.search(r"static void " + re.escape(name) + r"\(", source)
    if match is None:
        return None
    start = source.index("{", match.end())
    depth = 0
    for position in range(start, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position]
    return None


def node_draws():
    """How the node editor draws each logic node's own settings, read from the node sources:
    idname -> [(property, label: None for its own name, "" for none, expanded)], and the
    settings drawn inside an input's row: (idname, identifier) -> (row label, [properties])."""
    draws, in_sockets = {}, {}
    # A setting (layout.prop), or a data-block picker (template_id: Play Sound's sound) drawn
    # with no label, in the order the node draws them.
    prop_call = re.compile(r'\.prop\(\s*ptr\s*,\s*"(\w+)"\s*,\s*([^,]+),\s*'
                           r'(std::nullopt|IFACE_\("(?:[^"\\]|\\.)*"\)|"(?:[^"\\]|\\.)*")'
                           r'|template_id\(\s*&layout\s*,\s*\w+\s*,\s*ptr\s*,\s*"(\w+)"')
    for name in sorted(os.listdir(NODE_SOURCES)):
        if not name.endswith(".cc"):
            continue
        source = open(os.path.join(NODE_SOURCES, name), encoding="utf-8").read()
        idnames = node_idnames(source)
        props = []
        draw = re.search(r"draw_buttons\s*=\s*(\w+)\s*;", source)
        if draw is not None:
            body = _function_body(source, draw.group(1))
            if body is None:
                props = None
            else:
                for call in prop_call.finditer(body):
                    prop, flags, text, picker = call.groups()
                    if picker is not None:
                        props.append((picker, "", False))
                        continue
                    label = (None if text == "std::nullopt" else
                             re.search(r'"((?:[^"\\]|\\.)*)"', text).group(1))
                    props.append((prop, label, "EXPAND" in flags))
        # On the node (b) or a folded section's builder.
        for match in re.finditer(r'\b\w+\.add_input<[^>]+>\(\s*"([^"]*)"_ustr'
                                 r'(?:\s*,\s*"([^"]*)"_ustr)?\)', source):
            statement = _statements(source, match.end())
            if "custom_draw" not in statement:
                continue
            row_label = re.search(r'\.label\(\s*"((?:[^"\\]|\\.)*)"', statement)
            socket_props = re.findall(r'\.prop\(\s*&params\.node_ptr\s*,\s*"(\w+)"', statement)
            if not socket_props:
                # Drawn its own way (a search field), still showing the socket's value.
                continue
            for idname in idnames:
                in_sockets[(idname, match.group(2) or match.group(1))] = (
                    row_label.group(1) if row_label else None, socket_props)
        for idname in idnames:
            draws[idname] = props
    return draws, in_sockets


NODE_DRAWS, SOCKET_DRAWS = node_draws()


def node_panels():
    """The folded sections each logic node declares, read from the node sources: idname ->
    [(title, [input identifiers], the identifier of the checkbox in its header or None)], in the
    order the node's panel_states keeps them."""
    panels = {}
    panel_call = re.compile(r'PanelDeclarationBuilder\s*&\s*(\w+)\s*=\s*b\.add_panel\(\s*'
                            r'"([^"]*)"_ustr')
    for name in sorted(os.listdir(NODE_SOURCES)):
        if not name.endswith(".cc"):
            continue
        source = open(os.path.join(NODE_SOURCES, name), encoding="utf-8").read()
        found = []
        for match in panel_call.finditer(source):
            builder, title = match.groups()
            inputs, toggle = [], None
            for socket in re.finditer(r'\b' + re.escape(builder) + r'\.add_input<[^>]+>\(\s*'
                                      r'"([^"]*)"_ustr(?:\s*,\s*"([^"]*)"_ustr)?\)', source):
                identifier = socket.group(2) or socket.group(1)
                if ".panel_toggle()" in _statements(source, socket.end()):
                    toggle = identifier
                else:
                    inputs.append(identifier)
            found.append((title, inputs, toggle))
        if len(found) != source.count(".add_panel("):
            raise RuntimeError(f"the guide cannot read the folded sections of {name}")
        if found:
            for idname in node_idnames(source):
                panels[idname] = found
    return panels


NODE_PANELS = node_panels()


def socket_label(socket):
    """A socket's name as the node editor shows it (some change with the node's mode)."""
    return socket.label or socket.name


# Nodes whose header shows a setting's choice, not the node's name (their source sets a
# labelfunc): the setting.
NODE_TITLE_SETTINGS = {"LogicNativeInventory": "operation", "LogicNativeMath": "operation"}


def _titled_nodes():
    titled = set()
    for name in os.listdir(NODE_SOURCES):
        if name.endswith(".cc"):
            source = open(os.path.join(NODE_SOURCES, name), encoding="utf-8").read()
            if "labelfunc" in source:
                titled.update(node_idnames(source))
    unknown = titled - set(NODE_TITLE_SETTINGS)
    if unknown:
        raise RuntimeError(f"say what the header of {sorted(unknown)} shows (NODE_TITLE_SETTINGS)")
    return titled


TITLED_NODES = _titled_nodes()


def node_title(node):
    """A node's header as the node editor shows it."""
    if node.label:
        return node.label
    if node.bl_idname in TITLED_NODES:
        setting = NODE_TITLE_SETTINGS[node.bl_idname]
        return enum_label(node.bl_rna, setting, getattr(node, setting))
    return node.bl_label


def node_props(node):
    """The node's own settings as the editor draws them: (label or "", value, checkbox state,
    expanded enum items)."""
    draws = NODE_DRAWS.get(node.bl_idname)
    if draws is None:
        raise RuntimeError(f"the guide cannot read how {node.bl_idname} draws its settings")
    rows = []
    for identifier, label, expanded in draws:
        prop = node.bl_rna.properties[identifier]
        value = getattr(node, identifier)
        text = prop.name if label is None else label
        if prop.type == 'ENUM':
            items = [item.name for item in prop.enum_items] if expanded else None
            rows.append((text, enum_label(node.bl_rna, identifier, value), None, items))
        elif prop.type == 'POINTER':
            # A data-block picker shows the name of what it holds (Play Sound's sound).
            rows.append((text, value.name if value is not None else "", None, None))
        else:
            rows.append((text, None, bool(value), None))
    return rows


def socket_value(socket):
    if not hasattr(socket, "default_value") or socket.is_linked:
        return None
    value = socket.default_value
    if isinstance(value, bpy.types.ID):
        return value.name
    if value is None:
        return ""
    if isinstance(value, str):
        return value.replace("\n", "↵")
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return num(value)
    try:
        return vec(list(value))
    except TypeError:
        return str(value)


_NEW_NODE_INPUTS = {}


def new_node_inputs(idname):
    """A new node's input values (as socket_value gives them), by identifier."""
    if idname not in _NEW_NODE_INPUTS:
        tree = bpy.data.node_groups.new("__guide_new_node", "LogicNodeTree")
        try:
            node = tree.nodes.new(idname)
            _NEW_NODE_INPUTS[idname] = {socket.identifier: socket_value(socket)
                                        for socket in node.inputs}
        finally:
            bpy.data.node_groups.remove(tree)
    return _NEW_NODE_INPUTS[idname]


def node_layout(node):
    """Rows of a node, top to bottom: outputs, settings, inputs, then its folded sections (as
    Blender draws them). A section shows its inputs when it is open, or when one of them is linked
    or set: the picture shows every value (and the page says to open it). A section row is
    ("panel", (title, its header's checkbox input or None, shown open, closed in the file))."""
    def drawn(socket):
        return socket.enabled and not socket.hide
    rows = []
    for socket in node.outputs:
        if drawn(socket):
            rows.append(("out", socket))
    for row in node_props(node):
        rows.append(("prop", row))
    panels = NODE_PANELS.get(node.bl_idname, ())
    if len(panels) != len(node.panel_states):
        raise RuntimeError(f"{node.name}: the guide reads {len(panels)} folded sections, the node "
                           f"has {len(node.panel_states)}")
    inputs = {socket.identifier: socket for socket in node.inputs}
    folded = {identifier for _title, members, toggle in panels
              for identifier in members + ([toggle] if toggle else [])}
    for socket in node.inputs:
        if drawn(socket) and socket.identifier not in folded:
            rows.append(("in", socket))
    for state, (title, members, toggle) in zip(node.panel_states, panels):
        sockets = [inputs[identifier] for identifier in members if drawn(inputs[identifier])]
        header = inputs[toggle] if toggle and drawn(inputs[toggle]) else None
        if not sockets and header is None:
            # Blender draws no section with nothing in it.
            continue
        new = new_node_inputs(node.bl_idname)
        shown_open = not state.is_collapsed or any(
            socket.is_linked or socket_value(socket) != new[socket.identifier]
            for socket in sockets)
        rows.append(("panel", (title, header, shown_open, state.is_collapsed)))
        if shown_open:
            rows.extend(("in", socket) for socket in sockets)
    return rows


def frame_svg(tree, frame):
    """An SVG of one frame of a tree, nodes where they are in the file, and the values it cuts
    short: [(node, input, value)]."""
    nodes = [node for node in tree.nodes if node.parent == frame]
    # What the picture cuts short: values [(node, name, value)], names {shown: full}.
    cut = {"values": [], "names": {}}

    def short_name(text, room):
        shown = shorten(text, room)
        if shown != text:
            cut["names"][shown] = text
        return shown
    boxes = {}
    for node in nodes:
        x, y = node.location_absolute
        rows = node_layout(node)
        boxes[node.name] = [x, -y, node.width, HEADER + ROW * len(rows) + 8, rows]
    # Nodes drawn a little taller than Blender's never overlap the one below them.
    columns = sorted(boxes.values(), key=lambda box: (round(box[0] / 40), box[1]))
    for index, box in enumerate(columns):
        for other in columns[:index]:
            if abs(other[0] - box[0]) < min(other[2], box[2]) and \
                    other[1] <= box[1] < other[1] + other[3] + 12:
                box[1] = other[1] + other[3] + 12
    # Empty space wider than COLUMN_GAP between columns of nodes is left out: the picture keeps
    # their order, and fits the page better.
    reach = None
    shift = 0.0
    for box in sorted(boxes.values(), key=lambda box: box[0]):
        box[0] -= shift
        if reach is not None and box[0] - reach > COLUMN_GAP:
            extra = box[0] - reach - COLUMN_GAP
            box[0] -= extra
            shift += extra
        reach = box[0] + box[2] if reach is None else max(reach, box[0] + box[2])
    # Links to or from another frame: a dashed stub with the other end's name.
    incoming = [link for link in tree.links
                if link.to_node.parent == frame and link.from_node.parent != frame]
    outgoing = [link for link in tree.links
                if link.from_node.parent == frame and link.to_node.parent != frame]
    # One label per socket: the other ends of its links (Any Flow's input takes several).
    ends = {}
    for link, side in [(link, "in") for link in incoming] + [(link, "out") for link in outgoing]:
        if side == "in":
            key = (link.to_node.name, "in", link.to_socket.identifier)
            other, socket = link.from_node, link.from_socket
        else:
            key = (link.from_node.name, "out", link.from_socket.identifier)
            other, socket = link.to_node, link.to_socket
        where = (node_title(other),
                 other.parent.label.split(":")[0] if other.parent else "no frame")
        ends.setdefault(key, {}).setdefault(where, []).append(socket_label(socket))
    stub_names = {key: "; ".join(f"{node} › {', '.join(names)} ({frame_name})"
                                 for (node, frame_name), names in others.items())
                  for key, others in ends.items()}
    room_in = max((text_width(name) + 50 for key, name in stub_names.items() if key[1] == "in"),
                  default=0)
    room_out = max((text_width(name) + 50 for key, name in stub_names.items()
                    if key[1] == "out"), default=0)
    left = min(box[0] for box in boxes.values()) - 20 - room_in
    top = min(box[1] for box in boxes.values()) - 20
    right = max(box[0] + box[2] for box in boxes.values()) + 20 + room_out
    bottom = max(box[1] + box[3] for box in boxes.values()) + 20
    width, height = right - left, bottom - top
    sockets = {}
    parts = []
    for node in nodes:
        x, y, w, h, rows = boxes[node.name]
        x -= left
        y -= top
        kind = header_kind(node.bl_idname)
        parts.append(f'<rect x="{x:.0f}" y="{y:.0f}" width="{w:.0f}" height="{h:.0f}" rx="6" '
                     f'class="n-body"/>')
        parts.append(f'<path d="M{x:.0f},{y + HEADER:.0f} v-{HEADER - 6} a6,6 0 0 1 6,-6 '
                     f'h{w - 12:.0f} a6,6 0 0 1 6,6 v{HEADER - 6} z" class="h-{kind}"/>')
        parts.append(f'<text x="{x + 9:.0f}" y="{y + 17:.0f}" class="n-title">'
                     f'{esc(short_name(node_title(node), w - 16))}</text>')
        for index, (side, item) in enumerate(rows):
            row_y = y + HEADER + ROW * index + 15
            if side == "out":
                color = SOCKET_COLORS.get(item.bl_idname, "var(--s-generic)")
                cx, cy = x + w, row_y - 4
                sockets[(node.name, "out", item.identifier)] = (cx, cy, color)
                parts.append(f'<text x="{x + w - 11:.0f}" y="{row_y:.0f}" class="n-label" '
                             f'text-anchor="end">{esc(short_name(socket_label(item), w - 20))}'
                             '</text>')
                parts.append(_socket_mark(cx, cy, color, item.bl_idname))
            elif side == "prop":
                label, value, check, items = item
                if items is not None:
                    # An expanded choice: a row of buttons, the chosen one lit.
                    step = (w - 16) / len(items)
                    for option_index, option in enumerate(items):
                        button_x = x + 8 + step * option_index
                        lit = "n-check-on" if option == value else "n-field"
                        shown = shorten(option, step - 6)
                        if shown != option and option == value:
                            cut["values"].append((node, label or "choice", value))
                        parts.append(f'<rect x="{button_x:.0f}" y="{row_y - 13:.0f}" '
                                     f'width="{step - 1:.0f}" height="17" rx="3" class="{lit}"/>')
                        parts.append(f'<text x="{button_x + step / 2:.0f}" y="{row_y:.0f}" '
                                     f'class="n-label" text-anchor="middle">{esc(shown)}</text>')
                elif value is not None:
                    parts.append(f'<rect x="{x + 8:.0f}" y="{row_y - 13:.0f}" width="{w - 16:.0f}" '
                                 f'height="17" rx="3" class="n-field"/>')
                    if label:
                        shown_label, shown, short = fit_row(label, value, w - 28)
                        if short:
                            cut["values"].append((node, label, value))
                        parts.append(f'<text x="{x + 14:.0f}" y="{row_y:.0f}" class="n-dim">'
                                     f'{esc(shown_label)}</text>')
                        parts.append(f'<text x="{x + w - 14:.0f}" y="{row_y:.0f}" '
                                     f'class="n-value" text-anchor="end">{esc(shown)}</text>')
                    else:
                        # A dropdown with no label shows only its choice.
                        shown = shorten(value, w - 28)
                        if shown != value:
                            cut["values"].append((node, "choice", value))
                        parts.append(f'<text x="{x + 14:.0f}" y="{row_y:.0f}" '
                                     f'class="n-value">{esc(shown)}</text>')
                else:
                    parts.append(f'<rect x="{x + 10:.0f}" y="{row_y - 11:.0f}" width="12" '
                                 f'height="12" rx="2" class="{"n-check-on" if check else "n-check"}"/>')
                    parts.append(f'<text x="{x + 28:.0f}" y="{row_y:.0f}" class="n-label">'
                                 f'{esc(short_name(label, w - 36))}</text>')
            elif side == "panel":
                # A folded section's header: its arrow, its checkbox (a socket once linked), its
                # name.
                title, header, shown_open, _closed = item
                parts.append(f'<rect x="{x + 1:.0f}" y="{row_y - 15:.0f}" width="{w - 2:.0f}" '
                             f'height="{ROW - 2}" class="n-panel"/>')
                parts.append(f'<text x="{x + 9:.0f}" y="{row_y:.0f}" class="n-dim">'
                             f'{"▾" if shown_open else "▸"}</text>')
                title_x = x + 22
                if header is not None and header.is_linked:
                    color = SOCKET_COLORS.get(header.bl_idname, "var(--s-generic)")
                    cx, cy = x, row_y - 4
                    sockets[(node.name, "in", header.identifier)] = (cx, cy, color)
                    parts.append(_socket_mark(cx, cy, color, header.bl_idname))
                elif header is not None:
                    parts.append(f'<rect x="{x + 22:.0f}" y="{row_y - 11:.0f}" width="12" '
                                 f'height="12" rx="2" class="'
                                 f'{"n-check-on" if header.default_value else "n-check"}"/>')
                    title_x = x + 40
                parts.append(f'<text x="{title_x:.0f}" y="{row_y:.0f}" class="n-label n-section">'
                             f'{esc(short_name(title, x + w - title_x - 8))}</text>')
            else:
                color = SOCKET_COLORS.get(item.bl_idname, "var(--s-generic)")
                cx, cy = x, row_y - 4
                sockets[(node.name, "in", item.identifier)] = (cx, cy, color)
                parts.append(_socket_mark(cx, cy, color, item.bl_idname))
                drawn = SOCKET_DRAWS.get((node.bl_idname, item.identifier))
                if drawn is not None and not item.is_linked:
                    # Settings drawn in the input's row (X, Y, Z toggles) instead of its value.
                    row_label, props = drawn
                    parts.append(f'<text x="{x + 11:.0f}" y="{row_y:.0f}" class="n-label">'
                                 f'{esc(row_label or socket_label(item))}</text>')
                    for toggle_index, prop in enumerate(reversed(props)):
                        toggle_x = x + w - 12 - 22 * (toggle_index + 1)
                        on = getattr(node, prop)
                        parts.append(f'<rect x="{toggle_x:.0f}" y="{row_y - 13:.0f}" width="20" '
                                     f'height="17" rx="3" class="{"n-check-on" if on else "n-field"}"/>')
                        parts.append(f'<text x="{toggle_x + 10:.0f}" y="{row_y:.0f}" '
                                     f'class="n-label" text-anchor="middle">'
                                     f'{esc(node.bl_rna.properties[prop].name)}</text>')
                    continue
                name = socket_label(item)
                value = socket_value(item)
                if isinstance(value, bool):
                    name = shorten(name, w - 40)
                    parts.append(f'<rect x="{x + w - 24:.0f}" y="{row_y - 11:.0f}" width="12" '
                                 f'height="12" rx="2" class="{"n-check-on" if value else "n-check"}"/>')
                elif value is not None:
                    shown = (value if len(value) <= PICTURE_VALUE_LENGTH else
                             value[:PICTURE_VALUE_LENGTH - 2] + "…")
                    name, shown, short = fit_row(name, shown or "—", w - 22)
                    if shown != value and (short or len(value) > PICTURE_VALUE_LENGTH):
                        cut["values"].append((node, socket_label(item), value))
                    parts.append(f'<text x="{x + w - 11:.0f}" y="{row_y:.0f}" class="n-value" '
                                 f'text-anchor="end">{esc(shown)}</text>')
                else:
                    name = shorten(name, w - 22)
                parts.append(f'<text x="{x + 11:.0f}" y="{row_y:.0f}" class="n-label">'
                             f'{esc(name)}</text>')
    wires = []
    for link in tree.links:
        if link.from_node.parent != frame and link.to_node.parent != frame:
            continue
        start = sockets.get((link.from_node.name, "out", link.from_socket.identifier))
        end = sockets.get((link.to_node.name, "in", link.to_socket.identifier))
        if start is None or end is None:
            continue
        (x1, y1, color), (x2, y2, _color) = start, end
        bend = max(40.0, abs(x2 - x1) / 2.0)
        wires.append(f'<path d="M{x1:.0f},{y1:.0f} C{x1 + bend:.0f},{y1:.0f} {x2 - bend:.0f},'
                     f'{y2:.0f} {x2:.0f},{y2:.0f}" class="wire" style="stroke:{color}"/>')
    stubs = []
    for key, name in stub_names.items():
        if key not in sockets:
            raise RuntimeError(f"{tree.name}: the picture of {frame.label} has no socket for the "
                               f"link with {name}")
        x, y, color = sockets[key]
        side = key[1]
        end = x - 34 if side == "in" else x + 34
        wires.append(f'<path d="M{x:.0f},{y:.0f} H{end:.0f}" class="wire stub" '
                     f'style="stroke:{color}"/>')
        anchor = "end" if side == "in" else "start"
        text_x = end - 4 if side == "in" else end + 4
        stubs.append(f'<text x="{text_x:.0f}" y="{y + 4:.0f}" class="n-stub" '
                     f'text-anchor="{anchor}">{esc(name)}</text>')
    label = esc(frame.label)
    # A wide picture is made smaller to fit the page, down to 2/3, then scrolls.
    return (f'<svg viewBox="0 0 {width:.0f} {height:.0f}" width="{width:.0f}" '
            f'height="{height:.0f}" style="min-width:{width * 2 / 3:.0f}px" role="img" '
            f'aria-label="{label}">' + "".join(wires) + "".join(parts) + "".join(stubs) +
            "</svg>"), cut


def _socket_mark(cx, cy, color, idname):
    if idname == "NodeSocketLogicExecution":
        return (f'<polygon points="{cx - 5:.0f},{cy:.0f} {cx:.0f},{cy - 5:.0f} {cx + 5:.0f},'
                f'{cy:.0f} {cx:.0f},{cy + 5:.0f}" class="sock" style="fill:{color}"/>')
    return f'<circle cx="{cx:.0f}" cy="{cy:.0f}" r="4.6" class="sock" style="fill:{color}"/>'


# -----------------------------------------------------------------------------
# What the guide shows of the recipe

# Settings that do nothing where they are: the guide leaves them out (and the rebuild test checks
# that the example works from what the guide shows). A widget's numbers and parts only count on a
# widget, a UI Element's settings only on an element; how an object is drawn in the 3D view is
# the creator's own choice.
EDITOR_ONLY = {"show_name", "show_in_front", "empty_display_size", "empty_display_type",
               "display_type", "color", "hide_select"}
WIDGET_ONLY = {"widget_object", "widget_property", "widget_min", "widget_max", "widget_step",
               "widget_part", "widget_max_property"}
# A material's Viewport Display settings: only the 3D view's Solid shading uses them.
MATERIAL_VIEWPORT_ONLY = {"diffuse_color", "metallic", "roughness", "specular_intensity",
                          "specular_color"}
LIGHT_TYPES = ('POINT', 'SUN', 'SPOT', 'AREA')
# An object's place, data and links: the tables give them their own way.
OBJECT_NOT_SETTINGS = {
    "location", "rotation_euler", "rotation_quaternion", "rotation_axis_angle", "scale",
    "delta_location", "delta_rotation_euler", "delta_rotation_quaternion", "delta_scale",
    "dimensions", "matrix_world", "matrix_local", "matrix_basis", "matrix_parent_inverse",
    "parent", "data", "active_material", "active_material_index", "mode", "game", "type",
    "parent_type", "parent_bone", "parent_vertices", "bound_box", "active_shape_key_index",
}
# How a table names each object setting (the generator stops on another).
OBJECT_SETTING_LABELS = {
    # (where it is in the Object tab, drawn inverted), as properties_object.py draws them.
    "hide_render": ("Visibility › Show In › Renders", True),
    "hide_viewport": ("Visibility › Show In › Viewports", True),
    "visible_camera": ("Visibility › Ray Visibility › Camera", False),
    "visible_shadow": ("Visibility › Ray Visibility › Shadow", False),
    "visible_diffuse": ("Visibility › Ray Visibility › Diffuse", False),
    "visible_glossy": ("Visibility › Ray Visibility › Glossy", False),
    "visible_transmission": ("Visibility › Ray Visibility › Transmission", False),
    "visible_volume_scatter": ("Visibility › Ray Visibility › Volume Scatter", False),
}
# Where the scene's game settings the example changes are (the generator stops on another).
SCENE_SETTING_PLACES = {
    "item_library": "Scene tab › Items and UI › Item Library",
    "show_mouse": "Render tab › Game Debug › Mouse Cursor",
}
RENDER_KEYS = ("resolution_x", "resolution_y", "resolution_percentage")


def _object_values(ob):
    """An object's settings as the guide compares them (the recipe's encoding)."""
    values = {
        # Every setting of the object (not only the recipe's): none may be left out.
        "settings": R._settings(ob, skip=OBJECT_NOT_SETTINGS),
        "game": R._settings(ob.game, skip=R.GAME_SKIP),
        "ui": R._settings(ob.game.ui),
    }
    if ob.type == 'FONT':
        values["text"] = R._settings(ob.data, skip=R.TEXT_SKIP)
    elif ob.type in {'CAMERA', 'LIGHT'}:
        values["data"] = R._settings(ob.data)
    return values


def _kind(ob):
    return ob.type if ob.type != 'LIGHT' else "LIGHT:" + ob.data.type


def _action_values(action):
    return {"settings": R._settings(action, skip={"active_binding_index"}),
            "bindings": [R._settings(binding) for binding in action.bindings]}


def start_values():
    """What a creator starts from: the new file Blender opens with (File > New > General) and
    objects from the Add menu. The guide lists what differs from these, not from Blender's own
    defaults: a new file's resolution is 1920 x 1080, and the Add menu makes texts, empties,
    cameras and lights No Collision and a Sun's Strength 1. Run before the example is opened."""
    scene = bpy.context.scene
    start = {
        "game": R._settings(scene.game_settings, skip=R.SCENE_GAME_SKIP),
        "render": {name: R.encode(getattr(scene.render, name)) for name in RENDER_KEYS},
        "world": R._settings(scene.world) if scene.world else {},
        "actions": {action.name: _action_values(action)
                    for action in scene.game_settings.input_actions},
        "objects": {},
    }
    makers = {
        "MESH": lambda: bpy.ops.mesh.primitive_plane_add(),
        "FONT": lambda: bpy.ops.object.text_add(),
        "EMPTY": lambda: bpy.ops.object.empty_add(),
        "CAMERA": lambda: bpy.ops.object.camera_add(),
    }
    for light in LIGHT_TYPES:
        makers["LIGHT:" + light] = lambda light=light: bpy.ops.object.light_add(type=light)
    added = {}
    for kind, make in makers.items():
        make()
        ob = bpy.context.active_object
        if ob is None or _kind(ob) != kind:
            raise RuntimeError(f"could not add a {kind} object to compare with")
        start["objects"][kind] = _object_values(ob)
        added[kind] = ob
    # A new action and new bindings, for the settings the Input Map table does not show.
    actions = scene.game_settings.input_actions
    start["action_defaults"] = {}
    start["binding_defaults"] = {}
    for kind in ('BUTTON', 'VECTOR'):
        action = actions.new(name=f"__guide_{kind}", type=kind)
        start["action_defaults"][kind] = {
            k: v for k, v in R._settings(action, skip={"active_binding_index"}).items()
            if k not in {"name", "type", "context"}}
        for device in ('KEYBOARD', 'MOUSE', 'GAMEPAD'):
            start["binding_defaults"][device] = R._settings(action.bindings.new(device=device))
        actions.remove(action)
    # A material as the Material tab's New button makes it, on the plane.
    before = set(bpy.data.materials)
    with bpy.context.temp_override(object=added["MESH"], active_object=added["MESH"]):
        bpy.ops.material.new()
    material = (set(bpy.data.materials) - before).pop()
    start["material"] = _bsdf_values(material)
    start["material_settings"] = R._settings(material, skip=MATERIAL_VIEWPORT_ONLY)
    return start


def _bsdf_values(material):
    bsdf = next(node for node in material.node_tree.nodes if node.type == 'BSDF_PRINCIPLED')
    return {socket.identifier: R.encode(socket.default_value) for socket in bsdf.inputs
            if hasattr(socket, "default_value") and not socket.is_linked}


WIDGET_NUMBERS = {"widget_min", "widget_max", "widget_step", "widget_max_property"}


def widget_numbers_used(ob):
    """The number settings a widget reads (LN_UIService): a Slider and a number Selector step
    between Min and Max, a Bar fills between Min and Max (or Max Property), a Text Field's Max is
    its length; a Toggle and a Selector of a Choice use none."""
    ui = ob.game.ui
    if ui.widget == 'SLIDER':
        return {"widget_min", "widget_max", "widget_step"}
    if ui.widget == 'BAR':
        return {"widget_min", "widget_max", "widget_max_property"}
    if ui.widget == 'TEXT':
        return {"widget_max"}
    if ui.widget == 'SELECTOR':
        holder = ui.widget_object
        if holder is None:
            holder = ob
            while holder is not None and ui.widget_property not in holder.game.properties:
                holder = holder.parent
        prop = holder.game.properties.get(ui.widget_property) if holder else None
        if prop is not None and prop.type == 'CHOICE':
            return set()
        return {"widget_min", "widget_max", "widget_step"}
    return set()


def _is_stick(binding):
    return binding["device"] == 'GAMEPAD' and binding["gamepad_control"] in {
        'LEFT_STICK', 'RIGHT_STICK'}


def differs(values, start):
    return {name: value for name, value in values.items()
            if name not in start or not R._equal(value, start[name])}


def guide_view(recipe, start):
    """The example as the guide shows it: each object's settings that are not as the Add menu
    makes them, without settings that do nothing where they are."""
    view = dict(recipe)
    view["objects"] = {}
    for name, entry in recipe["objects"].items():
        ob = bpy.data.objects[name]
        values = _object_values(ob)
        base = start["objects"][_kind(ob)]
        entry = dict(entry)
        entry["settings"] = {k: v for k, v in differs(values["settings"], base["settings"]).items()
                             if k not in EDITOR_ONLY}
        unknown = set(entry["settings"]) - set(OBJECT_SETTING_LABELS)
        if unknown:
            raise RuntimeError(f"{name}: say what the setting(s) {sorted(unknown)} are "
                               "(OBJECT_SETTING_LABELS), or that they only change the 3D view "
                               "(EDITOR_ONLY)")
        if not set(entry["settings"]) <= set(R.OBJECT_KEEP):
            raise RuntimeError(f"{name}: the templates' recipe does not keep "
                               f"{sorted(set(entry['settings']) - set(R.OBJECT_KEEP))}")
        entry["game"] = differs(values["game"], base["game"])
        entry["physics"] = values["game"]["physics_type"]
        ui = differs(values["ui"], base["ui"])
        if not ob.game.ui.use_element:
            ui = {}
        elif ob.game.ui.widget == 'NONE':
            ui = {k: v for k, v in ui.items() if k not in WIDGET_ONLY}
        if ob.game.ui.use_element and ob.game.ui.widget != 'NONE':
            unused = WIDGET_NUMBERS - widget_numbers_used(ob)
            ui = {k: v for k, v in ui.items() if k not in unused}
        entry["ui"] = ui
        if ob.type == 'FONT':
            entry["text"] = differs(values["text"], base["text"])
            entry["text"]["body"] = ob.data.body
            # The game starts a text's Text property from its body, and makes one when it sets the
            # text: a Text property equal to the body changes nothing.
            entry["properties"] = [prop for prop in entry["properties"]
                                   if not (prop["name"] == "Text" and prop["type"] == 'STRING'
                                           and prop["value"] in ("", ob.data.body))]
        elif ob.type in {'CAMERA', 'LIGHT'}:
            entry["data"] = differs(values["data"], base["data"])
        view["objects"][name] = entry
    return view


# How the tables name the UI Element settings: (short group name, its sub-panel in the Game
# Components tab, settings), as scripts/startup/bl_ui/properties_game.py draws them.
UI_GROUPS = (
    ("UI", "UI Element", ("use_element", "action", "interactable", "pointer", "focus",
                          "tab_order", "anchor", "player", "use_any_player")),
    ("Drag", "Drag and Drop", ("use_draggable", "follow_cursor", "drag_look",
                               "use_click_pick_up", "drag_threshold", "use_drop_target",
                               "accept_categories", "drop_group")),
    ("Look", "Look", ("hover_scale", "press_scale", "look_time", "highlight_object")),
    ("Widget", "Widget", ("widget", "widget_object", "widget_property", "widget_min",
                          "widget_max", "widget_step", "widget_part", "widget_max_property")),
    ("Nav", "Navigation", ("navigation_up", "navigation_down", "navigation_left",
                           "navigation_right")),
    ("Adv", "Advanced", ("priority", "padding")),
)
WIDGET_PART_LABEL = {"SLIDER": "Handle", "TOGGLE": "Check Mark", "TEXT": "Text", "BAR": "Fill",
                     "SELECTOR": "Text"}
# How the tables name the physics settings, with the Physics tab panel they are in (the panel's
# own label when it is not the setting's name; the generator stops on another).
GAME_SETTING_LABELS = {
    "use_actor": "Actor",
    "use_ghost": "Ghost",
    "collision_bounds_type": "Collision Bounds › Bounds",
}


def ui_label(identifier, widget):
    if identifier == "widget_part":
        return WIDGET_PART_LABEL.get(widget, "Part")
    if identifier == "widget_max" and widget == 'TEXT':
        return "Max Length"
    if identifier == "look_time" and widget == 'BAR':
        return "Smoothing"
    return prop_label(bpy.types.GameObjectUISettings.bl_rna, identifier)


def setting_value(struct_rna, identifier, value):
    prop = struct_rna.properties.get(identifier)
    if prop is not None and prop.type == 'ENUM' and isinstance(value, str):
        return enum_label(struct_rna, identifier, value)
    return value_text(value)


# What each object is for, in the tables (pieces named like *Glow, *Icon, *Count are described by
# their kind; the others may have no words).
PURPOSES = {
    "Player": "the player: walks, and holds every value the menus show",
    "NameTag": "the player's name, above it",
    "Stamina Bar.001": "the stamina bar above the player",
    "Hint": "the controls, on the ground",
    "Chest": "the chest in the world",
    "ChestLabel": "its label",
    "InvCamera": "the camera every menu and the HUD are seen through",
    "Panel": "the menu's background: every piece hangs from it",
    "InventoryTitle": "title",
    "Slot": "the first cell: the grid copies it",
    "ScrollBar": "the scrollbar",
    "ScrollHandle": "its handle",
    "PagePrev": "previous page",
    "PageNext": "next page",
    "PageText": "“Page 1 / 1”",
    "ItemName": "a line for messages",
    "DropHint": "the controls",
    "Tooltip": "the tooltip's background",
    "TooltipText": "the tooltip's text",
    "StackToggle": "a checkbox for stack_items",
    "StackCheck": "its check mark",
    "StackLabel": "its label",
    "EquipPlate": "the equipment column's background",
    "EquipTitle": "title",
    "WeaponSlot": "the weapon cell",
    "ArmorSlot": "the armor cell",
    "QuickSlot": "the quick cell",
    "WeaponSlotLabel": "the weapon cell's label",
    "ArmorSlotLabel": "the armor cell's label",
    "QuickSlotLabel": "the quick cell's label",
    "ChestRowText": "shows the item's name and count",
    "Pause Menu Title": "title",
    "Stamina Bar Fill": "the bar's fill",
    "Stamina Bar.001 Fill": "the bar's fill",
    "RefuseFrame": "red frame behind a cell that refuses a drop",
    "CraftPlate": "the recipe list's background",
    "CraftTitle": "title",
    "RecipeSlot": "the first recipe cell: the grid copies it",
    "CraftScrollBar": "the recipe list's scrollbar",
    "CraftScrollHandle": "its handle",
    "SettingsButton": "opens Settings",
    "ChestPanel": "the menu's background",
    "ChestTitle": "title",
    "BagTitle": "title",
    "ChestRow": "the first chest row: the grid copies it",
    "BagSlot": "the first bag cell: the grid copies it",
    "ChestHint": "the controls",
    "SettingsPanel": "the menu's background",
    "SettingsTitle": "title",
    "CloseButton": "closes Settings",
    "WalkSlider": "a slider for walk_speed",
    "WalkHandle": "its handle",
    "WalkValue": "shows walk_speed",
    "SortSelector": "a choice for sort_by",
    "SortValue": "shows the option",
    "SortArrows": "arrows around it",
    "HintsToggle": "a checkbox for show_hints",
    "HintsCheck": "its check mark",
    "NameField": "a text field for player_name",
    "NameText": "shows the name",
    "RebindButton": "rebinds Grab",
    "SettingsHint": "the controls",
    "Pause Menu Panel": "the menu's background",
    "Resume": "closes the menu",
    "Settings": "opens Settings",
    "Quit": "ends the game",
    "HUD Root": "holds the HUD pieces",
    "Stamina Bar": "the stamina bar",
    "Value Text": "name and speed",
    "Ground": "the ground",
    "Sun": "the light",
    "Camera_Parent": "holds the camera",
    "MainCamera": "the scene's camera",
}


def purpose(name, entry):
    if name in PURPOSES:
        return PURPOSES[name]
    if name.endswith("Glow"):
        return "shown while its parent is highlighted"
    if name.endswith("Icon"):
        return "shows the item's mesh"
    if name.endswith("Count"):
        return "shows the count"
    if entry["type"] == 'FONT' and entry["parent"] and (name.endswith("Text") or
                                                        name.endswith("Label")):
        return "its label"
    return ""


class Example:
    """The example as the guide needs it: its recipe (what to set), its file (pictures)."""

    def __init__(self, shots_dir, start):
        self.scene = bpy.context.scene
        self.start = start
        self.recipe = R.extract(bpy.data, self.scene)
        self.view = guide_view(self.recipe, start)
        self.shots_dir = shots_dir
        self.homes = {}
        for entry in self.recipe["collections"]:
            for name in entry["objects"]:
                self.homes.setdefault(name, []).append(entry["name"] or "Scene Collection")
        self.shown_objects = set()
        self.shown_frames = set()
        self.shown_materials = set()
        self.noted_breaks = set()
        self.check_trees()
        self.check_parents()
        self.text_bounds = self._measure_texts()

    def _measure_texts(self):
        """Each text's world bounds as the game shows it (a {name} shows a short value), for the
        marks on the pictures. Texts in a collection disabled in the viewport have no shape until
        it is shown: show it while measuring (the file is never saved)."""
        hidden = [c for c in bpy.data.collections if c.hide_viewport]
        bodies = {}
        for name, entry in self.recipe["objects"].items():
            if entry["type"] == 'FONT':
                curve = bpy.data.objects[name].data
                bodies[curve] = curve.body
                curve.body = re.sub(r"\{[^{}]*\}", "0000", curve.body)
        for collection in hidden:
            collection.hide_viewport = False
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        bounds = {}
        for name, entry in self.recipe["objects"].items():
            if entry["type"] != 'FONT':
                continue
            world = Matrix(entry["world"])
            evaluated = bpy.data.objects[name].evaluated_get(depsgraph)
            points = [world @ Vector(corner) for corner in evaluated.bound_box]
            bounds[name] = (Vector([min(p[a] for p in points) for a in range(3)]),
                            Vector([max(p[a] for p in points) for a in range(3)]))
        for collection in hidden:
            collection.hide_viewport = True
        for curve, body in bodies.items():
            curve.body = body
        bpy.context.view_layer.update()
        return bounds

    # Checks -------------------------------------------------------------

    def check_trees(self):
        """Stop when a node picture would not show all a creator needs: a node outside the
        frames, a muted or labelled node, a setting or a value the picture does not draw. (A link
        between two frames is drawn on both pictures as a named stub.)"""
        for tree_name, entry in self.recipe["trees"].items():
            tree = bpy.data.node_groups[tree_name]
            for spec in entry["nodes"]:
                node = tree.nodes[spec["name"]]
                where = f"tree {tree_name}, node {node.name}"
                if spec["frame"] is None:
                    raise RuntimeError(f"{where} is in no frame")
                if spec["mute"]:
                    raise RuntimeError(f"{where} is muted")
                if node.label:
                    raise RuntimeError(f"{where} has a label: the guide never says to set one")
                drawn = {prop for prop, _label, _expanded in NODE_DRAWS.get(node.bl_idname) or ()}
                for (idname, _identifier), (_label, props) in SOCKET_DRAWS.items():
                    if idname == node.bl_idname:
                        drawn |= set(props)
                for key in spec["settings"]:
                    if key not in drawn:
                        raise RuntimeError(f"{where}: the picture does not show its {key} (the "
                                           "node editor does not draw it)")
                for identifier in spec["inputs"]:
                    socket = next(s for s in node.inputs if s.identifier == identifier)
                    if socket.enabled and not socket.is_linked and socket.hide:
                        raise RuntimeError(f"{where}: input {identifier} is hidden but set")

    def check_parents(self):
        """A child's Location in the tables is its offset from its parent, which is so only for a
        parent that is not turned or scaled: stop on another."""
        objects = self.recipe["objects"]
        for name, entry in objects.items():
            if entry["parent"] is None:
                continue
            world = Matrix(objects[entry["parent"]]["world"])
            if (world.to_quaternion().angle > 1e-4 or
                    any(abs(value - 1.0) > 1e-4 for value in world.to_scale())):
                raise RuntimeError(f"{entry['parent']} is turned or scaled: a child's Location "
                                   "is not its offset")

    def check_like(self, names, model, shown):
        """Stop when an object differs from the model in more than a table shows: the guide says
        the others differ only in those values. Places are given by the table, or do not matter
        (library items are only copied)."""
        def key(name):
            entry = self.view["objects"][name]
            world = Matrix(entry["world"])
            props = [(prop["name"], prop["type"], None if prop["name"] in shown else
                      repr({k: v for k, v in prop.items() if k != "name"}))
                     for prop in entry["properties"]]
            middle = None if "middle" in shown else [round(v, 4) for v in self.middle(name)]
            return repr((entry["type"], entry["parent"], entry["settings"], entry["game"],
                         entry["ui"], props, [round(a, 4) for a in world.to_euler()],
                         [round(s, 4) for s in world.to_scale()], middle,
                         len([m for m in entry["materials"] if m]), entry["bindings"],
                         self.homes.get(name)))
        for name in names:
            if key(name) != key(model):
                raise RuntimeError(f"{name} differs from {model} in more than the guide's table "
                                   "shows")

    # Places and shapes ----------------------------------------------------

    def bounds(self, name):
        return T._bounds(self.recipe, name)

    def location(self, name):
        return Matrix(self.recipe["objects"][name]["world"]).translation

    def size(self, name):
        low, high = self.bounds(name)
        return high - low

    def middle(self, name):
        """Where a mesh's middle is, from its origin (zero when the mesh is centred)."""
        low, high = self.bounds(name)
        return (low + high) / 2.0 - self.location(name)

    def shape(self, name):
        """What an object is, in a few words, with its size."""
        entry = self.view["objects"][name]
        if entry["type"] == 'MESH':
            world = Matrix(entry["world"])
            # Sizes are world sizes, given for meshes that are not turned or scaled.
            if (world.to_quaternion().angle > 1e-4 or
                    any(abs(value - 1.0) > 1e-4 for value in world.to_scale())):
                raise RuntimeError(f"{name}: a turned or scaled mesh: say how to make it")
            low, high = self.bounds(name)
            size = high - low
            geometry = entry["geometry"]
            if "verts" in geometry:
                # A plane or a box is its bounds' corners (as Add > Mesh makes it, resized).
                for vert in geometry["verts"]:
                    point = world @ Vector(vert)
                    if any(min(abs(point[a] - low[a]), abs(point[a] - high[a])) > 1e-4
                           for a in range(3)):
                        raise RuntimeError(f"{name}: a small mesh that is not a plane or box")
                if len(geometry["verts"]) == 4:
                    corners = [Vector(geometry["verts"][index]) for index in geometry["faces"][0]]
                    normal = (corners[1] - corners[0]).cross(corners[2] - corners[0])
                    if size.z < 1e-6 and normal.z > 0.0:
                        return f"Plane {num(size.x)} × {num(size.y)}"
                    if size.y < 1e-6 and normal.y < 0.0:
                        # Add > Mesh > Plane turned 90° on X: it faces -Y, as the words say.
                        return f"Upright plane {num(size.x)} × {num(size.z)}"
                    raise RuntimeError(f"{name}: a plane facing another way: say how to make it")
                if len(geometry["verts"]) == 8:
                    # Add > Mesh > Cube: six faces, each facing out (a triangle mesh collides on
                    # its front side only).
                    centre = sum((Vector(v) for v in geometry["verts"]), Vector()) / 8.0
                    for face in geometry["faces"]:
                        corners = [Vector(geometry["verts"][index]) for index in face]
                        normal = (corners[1] - corners[0]).cross(corners[2] - corners[0])
                        middle = sum(corners, Vector()) / len(corners)
                        if normal.dot(middle - centre) <= 0.0:
                            raise RuntimeError(f"{name}: a box face facing in: say how to make it")
                    if len(geometry["faces"]) != 6:
                        raise RuntimeError(f"{name}: a box without six faces")
                    return f"Box {num(size.x)} × {num(size.y)} × {num(size.z)}"
                raise RuntimeError(f"{name}: a small mesh of {len(geometry['verts'])} points")
            return f"Mesh {num(size.x)} × {num(size.y)} × {num(size.z)} (any shape)"
        if entry["type"] == 'FONT':
            text = dict(entry["text"])
            text.pop("body")
            # As the Text tab names them: Font › Size, Paragraph › Alignment › Horizontal, Vertical.
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
            return ", ".join(words)
        if entry["type"] == 'EMPTY':
            return "Empty"
        if entry["type"] == 'CAMERA':
            ob = bpy.data.objects[name]
            words = (f"Camera, orthographic, scale {num(ob.data.ortho_scale)}"
                     if ob.data.type == 'ORTHO' else f"Camera, {num(ob.data.lens)} mm")
            extra = {k: v for k, v in entry["data"].items() if k not in {"type", "ortho_scale"}}
            if extra:
                words += ", " + ", ".join(
                    f"{prop_label(bpy.types.Camera.bl_rna, k)} "
                    f"{setting_value(bpy.types.Camera.bl_rna, k, v)}" for k, v in extra.items())
            return words
        if entry["type"] == 'LIGHT':
            words = f"{entry['light_type'].title()} light"
            if entry["data"]:
                words += ", " + ", ".join(
                    f"{prop_label(bpy.types.Light.bl_rna, k)} "
                    f"{setting_value(bpy.types.Light.bl_rna, k, v)}"
                    for k, v in entry["data"].items())
            return words
        return entry["type"].title()

    def location_cell(self, name):
        """Its Location (from the parent for a child), and the mesh's middle when it is not at
        the origin, and a turn or a scale."""
        entry = self.recipe["objects"][name]
        world = Matrix(entry["world"])
        place = world.translation
        if entry["parent"]:
            place = place - self.location(entry["parent"])
        lines = [vec(place)]
        if entry["type"] == 'MESH' and self.middle(name).length > 1e-4:
            lines.append(f"mesh middle {vec(self.middle(name))}")
        rotation = world.to_euler()
        if any(abs(angle) > 1e-4 for angle in rotation):
            lines.append("rotated " + vec([math.degrees(angle) for angle in rotation]) + "°")
        scale = world.to_scale()
        if any(abs(value - 1.0) > 1e-4 for value in scale):
            lines.append("scale " + vec(scale))
        return "<br>".join(esc(line) for line in lines)

    # The objects tables ---------------------------------------------------

    def _defaults(self, names):
        """What most objects of a table share, said once above it: the collection, the material
        of the planes, of the texts, the physics."""
        entries = {name: self.view["objects"][name] for name in names}
        shared = {}
        homes = {tuple(self.homes.get(name, [])) for name in names}
        if len(homes) == 1:
            shared["homes"] = homes.pop()
        for kind in ('MESH', 'FONT'):
            materials = collections.Counter(
                entry["materials"][0] for entry in entries.values()
                if entry["type"] == kind and entry["materials"] and entry["materials"][0])
            if materials:
                material, count = materials.most_common(1)[0]
                if count >= 2:
                    shared[kind] = material
        physics = collections.Counter(entry["physics"] for entry in entries.values())
        value, count = physics.most_common(1)[0]
        if count >= 2:
            shared["physics"] = value
        parents = {self.recipe["objects"][name]["parent"] for name in names
                   if self.recipe["objects"][name]["parent"] not in entries}
        if len(parents) == 1 and None not in parents:
            shared["parent"] = parents.pop()
        return shared

    def _shared_line(self, shared):
        game_rna = bpy.types.GameObjectSettings.bl_rna
        words = []
        if "homes" in shared:
            words.append("In collection " + ", ".join(f"<b>{esc(h)}</b>" for h in shared["homes"]))
        if "parent" in shared:
            words.append(f"rows without └ are children of <b>{esc(shared['parent'])}</b>")
        if "physics" in shared:
            words.append("Physics <b>" +
                         esc(enum_label(game_rna, "physics_type", shared["physics"])) + "</b>")
        if "MESH" in shared:
            words.append(f"meshes: material <b>{esc(shared['MESH'])}</b>")
        if "FONT" in shared:
            words.append(f"texts: material <b>{esc(shared['FONT'])}</b>")
        if not words:
            return '<p class="shared">Each row gives its own collection and settings.</p>'
        return '<p class="shared">' + " · ".join(words) + " (unless a row says otherwise).</p>"

    def settings_cell(self, name, shared, batch):
        """An object's settings, a line per group: “Physics <b>Dynamic</b>, Actor”. A group's
        first value with no name is the setting the group is named after (Physics Type, UI
        Element, Widget)."""
        entry = self.view["objects"][name]
        game_rna = bpy.types.GameObjectSettings.bl_rna
        groups = []

        def group(label, items):
            if items:
                groups.append((f'<span class="grp">{esc(label)}</span> ' if label else "") +
                              ", ".join(items))

        items = []
        # As the line above the table says, or else as the Add menu makes it.
        added = self.start["objects"][_kind(bpy.data.objects[name])]["game"]["physics_type"]
        if entry["physics"] != shared.get("physics", added):
            items.append("<b>" + esc(enum_label(game_rna, "physics_type", entry["physics"])) +
                         "</b>")
        jolt = self.scene.game_settings.physics_engine == 'JOLT'
        for key, value in entry["game"].items():
            if key == "physics_type":
                continue
            if (key == "use_collision_bounds" and value and jolt and
                    "collision_bounds_type" in entry["game"]):
                # Jolt draws no checkbox for it: choosing Bounds turns it on.
                continue
            if key not in GAME_SETTING_LABELS:
                raise RuntimeError(f"{name}: name the physics setting {key} "
                                   "(GAME_SETTING_LABELS)")
            shown = setting_value(game_rna, key, value)
            items.append(f"{esc(GAME_SETTING_LABELS[key])}" +
                         ("" if value is True else f" <b>{esc(shown)}</b>"))
        group("Physics", items)
        materials = [m for m in entry["materials"] if m]
        if len(materials) != len(entry["materials"]):
            raise RuntimeError(f"{name}: an empty material slot: say how to make it")
        if materials != ([shared[entry["type"]]] if entry["type"] in shared else []):
            group("", ["material <b>" + ", ".join(esc(m) for m in materials) + "</b>"
                       if materials else "<b>no material</b>"])
        items = []
        for key, value in entry["settings"].items():
            label, inverted = OBJECT_SETTING_LABELS[key]
            if key.startswith("visible_") and self.scene.render.engine != 'BLENDER_EEVEE':
                raise RuntimeError(f"{name}: the Object tab shows {key} only with EEVEE")
            shown = setting_value(bpy.types.Object.bl_rna, key,
                                  (not value) if inverted else value)
            note = " (starts hidden)" if key == "hide_render" and value else ""
            items.append(f"{esc(label)} <b>{esc(shown)}</b>{note}")
        group("Object", items)
        if entry["type"] == 'FONT':
            body = entry["text"]["body"]
            group("Body", [f'<code class="body">{esc(body)}</code>' if body else
                           "<b>empty</b> (delete “Text”: the game writes it)"])
        widget = bpy.data.objects[name].game.ui.widget
        shown_ui = set()
        for short, _panel, keys in UI_GROUPS:
            keys = list(keys)
            if widget == 'BAR':
                # A Bar's Look Time is its Smoothing, in the Widget sub-panel.
                if short == "Look":
                    keys.remove("look_time")
                elif short == "Widget":
                    keys.append("look_time")
            items = []
            for key in keys:
                if key not in entry["ui"]:
                    continue
                shown_ui.add(key)
                value = entry["ui"][key]
                rna = bpy.types.GameObjectUISettings.bl_rna
                if key in ("use_element", "widget"):
                    # The group's own setting: its value only.
                    items.append(f"<b>{esc(setting_value(rna, key, value))}</b>")
                    continue
                label = ui_label(key, widget)
                if value is True:
                    items.append(esc(label))
                    continue
                later = ""
                if (isinstance(value, dict) and value.get("id") == "objects" and
                        value["name"] not in self.shown_objects | batch):
                    later = " (made later: set it then)"
                items.append(f"{esc(label)} <b>{esc(setting_value(rna, key, value))}</b>{later}")
            group(short, items)
        if set(entry["ui"]) - shown_ui:
            raise RuntimeError(f"{name}: UI settings {sorted(set(entry['ui']) - shown_ui)} are "
                               "in no group (UI_GROUPS)")
        if entry["properties"] and name not in PROPERTIES_IN_A_TABLE:
            for prop in entry["properties"]:
                group("Property", [self.property_text(prop)])
                if isinstance(prop.get("value"), str) and "\n" in prop["value"]:
                    groups.append(self.break_note(
                        f"Select {name}, then",
                        f'C.object.game.properties["{prop["name"]}"].value', prop["value"]))
        for binding in entry["bindings"]:
            if not binding["enabled"]:
                raise RuntimeError(f"{name}: a turned-off logic tree: say so")
        if entry["bindings"]:
            group("Logic", [f"tree <b>{esc(binding['tree'])}</b>"
                            for binding in entry["bindings"]])
        return "".join(f"<div>{text}</div>" for text in groups if text)

    def objects_table(self, names, marks=()):
        """The objects to make, children under their parent, with what most of them share said
        once above. `marks`: the objects numbered on the picture before the table."""
        names = list(names)
        wanted = set(names)
        shared = self._defaults(names)
        numbers = {name: index + 1 for index, name in enumerate(marks)}
        # The numbered objects first, in their numbers' order.
        order = lambda name: (numbers.get(name, len(numbers) + 1), names.index(name))
        rows = []

        def walk(name, depth):
            self.shown_objects.add(name)
            entry = self.view["objects"][name]
            badge = (f'<span class="badge-n">{numbers[name]}</span>' if name in numbers else "")
            notes = [purpose(name, entry)] if purpose(name, entry) else []
            if depth == 0 and entry["parent"] and "parent" not in shared:
                notes.append(f"child of {entry['parent']}")
            if "homes" not in shared:
                notes.append("in " + ", ".join(self.homes.get(name, [])))
            note = "".join(f'<span class="what">{esc(text)}</span>' for text in notes)
            rows.append(
                f'<tr data-object="{esc(name)}">'
                f'<td class="ob" style="padding-left:{8 + 18 * depth}px">'
                f'{"└ " if depth else ""}{badge}<b>{esc(name)}</b>{note}</td>'
                f'<td>{esc(self.shape(name))}</td><td class="num">{self.location_cell(name)}</td>'
                f'<td>{self.settings_cell(name, shared, wanted)}</td></tr>')
            for child in sorted((child for child in names
                                 if self.recipe["objects"][child]["parent"] == name), key=order):
                walk(child, depth + 1)

        for name in sorted((name for name in names
                            if self.recipe["objects"][name]["parent"] not in wanted), key=order):
            walk(name, 0)
        missing = set(marks) - wanted
        if missing:
            raise RuntimeError(f"marks for objects not in the table: {sorted(missing)}")
        return (self._shared_line(shared) +
                '<div class="scroll"><table class="objects"><thead><tr><th>Object</th>'
                '<th>Shape</th><th>Location</th><th>Settings</th></tr></thead><tbody>' +
                "".join(rows) + "</tbody></table></div>" + self.materials_for(names))

    @staticmethod
    def property_text(prop):
        # The type names Blender's property type menu shows.
        kind = lambda value: enum_label(bpy.types.GameProperty.bl_rna, "type", value)
        if prop.get("debug"):
            raise RuntimeError(f"property {prop['name']} shows in the debug overlay: say so")
        text = f"<b>{esc(prop['name'])}</b> ({esc(kind(prop['type']))})"
        if prop["type"] == 'TABLE':
            columns = ", ".join(f"{esc(c['name'])} ({esc(kind(c['type']))})"
                                + ("" if c["value"] in (None, "", 0, 0.0, False) else
                                   f" default {esc(value_text(c['value']))}")
                                for c in prop["columns"])
            rows = "; ".join(" | ".join(esc(value_text(cell)) for cell in row)
                             for row in prop["rows"])
            text += f" columns {columns}" + (f", rows {rows}" if rows else ", no rows")
        elif prop["type"] == 'LIST':
            items = "; ".join(esc(value_text(item)) for item in prop["elements"])
            text += f" of {esc(kind(prop['item_type']))}" + (f": {items}" if items else
                                                               ", empty")
        elif prop["type"] == 'CHOICE':
            text += (" options " + ", ".join(esc(option) for option in prop["options"]) +
                     f", value {esc(prop['value'])}")
        else:
            text += f" = {esc(value_text(prop['value']))}"
        if prop["save_game"]:
            text += " (Save Game)"
        return text

    def break_note(self, how, target, value):
        """How to type a value with a line break, once per value."""
        key = (how, target, value)
        if key in self.noted_breaks:
            return ""
        self.noted_breaks.add(key)
        return line_break_note(how, target, value)

    # Materials ------------------------------------------------------------

    @staticmethod
    def _bsdf(name):
        material = bpy.data.materials[name]
        return next(node for node in material.node_tree.nodes if node.type == 'BSDF_PRINCIPLED')

    def swatch(self, name):
        color = self._bsdf(name).inputs["Base Color"].default_value
        rgb = ",".join(str(int(round(max(0.0, min(1.0, c)) ** (1 / 2.2) * 255))) for c in color[:3])
        return f'<span class="swatch" style="background:rgb({rgb})"></span>'

    def materials_for(self, names):
        """The materials these objects use that no earlier table showed: their Principled BSDF
        inputs that are not as in a new material (Material tab › New)."""
        rows = []
        for ob_name in names:
            for name in self.recipe["objects"][ob_name]["materials"]:
                if not name or name in self.shown_materials:
                    continue
                self.shown_materials.add(name)
                material = bpy.data.materials[name]
                if sorted(node.type for node in material.node_tree.nodes) != [
                        'BSDF_PRINCIPLED', 'OUTPUT_MATERIAL']:
                    raise RuntimeError(f"material {name} is not a plain Principled BSDF")
                labels = {socket.identifier: socket.name for socket in self._bsdf(name).inputs}
                values = differs(_bsdf_values(material), self.start["material"])
                words = []
                base, emission = values.get("Base Color"), values.get("Emission Color")
                if base is not None and emission is not None and R._equal(base, emission):
                    words.append(f"Base Color = Emission Color <b>{esc(value_text(base))}</b>")
                    values.pop("Base Color")
                    values.pop("Emission Color")
                for key, value in values.items():
                    words.append(f"{esc(labels.get(key, key))} <b>{esc(value_text(value))}</b>")
                settings = differs(R._settings(material, skip=MATERIAL_VIEWPORT_ONLY),
                                   self.start["material_settings"])
                for key, value in settings.items():
                    words.append(f"{esc(prop_label(bpy.types.Material.bl_rna, key))} "
                                 f"<b>{esc(setting_value(bpy.types.Material.bl_rna, key, value))}"
                                 "</b>")
                rows.append(f"<tr><td>{self.swatch(name)}<b>{esc(name)}</b></td>"
                            f"<td>{', '.join(words) or 'as a new material'}</td></tr>")
        if not rows:
            return ""
        return ('<table class="mats"><thead><tr><th>New material</th><th>Principled BSDF '
                '(the rest as a new material)</th></tr></thead><tbody>' + "".join(rows) +
                "</tbody></table>")

    # Logic ----------------------------------------------------------------

    def frame_block(self, tree_name, title):
        tree = bpy.data.node_groups[tree_name]
        frame = next(node for node in tree.nodes
                     if node.bl_idname == "NodeFrame" and node.label == title)
        self.shown_frames.add((tree_name, title))
        nodes = sorted((node for node in tree.nodes if node.parent == frame),
                       key=lambda node: (node.location_absolute[0], -node.location_absolute[1]))
        picture, cut = frame_svg(tree, frame)
        notes = []
        if cut["names"]:
            notes.append('<p class="note">Full names: ' + "; ".join(
                f"{esc(shown)} is <b>{esc(full)}</b>" for shown, full in cut["names"].items()) +
                ".</p>")
        long_values = [f"{esc(node_title(node))} › {esc(name)}: <code>{esc(value)}</code>"
                       for node, name, value in cut["values"]]
        for node in nodes:
            for socket in node.inputs:
                if not socket.enabled or socket.hide or socket.is_linked:
                    continue
                value = getattr(socket, "default_value", None)
                if isinstance(value, str) and "\n" in value:
                    notes.append(self.break_note(
                        f"Click the {node_title(node)} node, then",
                        f'bpy.data.node_groups["{tree_name}"].nodes.active'
                        f'.inputs["{socket.identifier}"].default_value', value))
        if long_values:
            notes.append('<p class="note">In full: ' + "; ".join(long_values) + ".</p>")
        # Sections a new node has folded, drawn open because something in them is set.
        to_open = [f"{esc(node_title(node))} › <b>{esc(title)}</b>" for node in nodes
                   for side, item in node_layout(node) if side == "panel"
                   for title, _header, shown_open, closed in [item] if shown_open and closed]
        if to_open:
            notes.append('<p class="note">Folded sections (click the name to open them): ' +
                         "; ".join(to_open) + ".</p>")
        bullets ="".join(f"<li>{line}</li>" for line in FRAMES[(tree_name, title)])
        return (f'<figure class="frame"><figcaption><span class="tree">{esc(tree_name)}</span> '
                f'{esc(title)}</figcaption>'
                f'<div class="canvas">{picture}</div>'
                f'<ul class="how">{bullets}</ul>{"".join(notes)}</figure>')

    # Pictures -------------------------------------------------------------

    def _project(self, camera, point):
        """Where a point is in the camera's picture, from its top left (0 to 1). As
        bpy_extras.object_utils.world_to_camera_view, with the camera's place from the recipe
        (matrix_world is not kept up to date in a collection disabled in the viewport)."""
        data = bpy.data.objects[camera].data
        local = Matrix(self.recipe["objects"][camera]["world"]).normalized().inverted() @ point
        depth = -local.z
        frame = list(data.view_frame(scene=self.scene))
        if data.type != 'ORTHO':
            if depth <= 0.0:
                raise RuntimeError(f"{camera} does not see the point {vec(point)}")
            frame = [-(corner / (corner.z / depth)) for corner in frame]
        x = (local.x - frame[2].x) / (frame[1].x - frame[2].x)
        y = (local.y - frame[1].y) / (frame[0].y - frame[1].y)
        return x, 1.0 - y

    def _extent(self, name, camera):
        """The screen box an object covers (its mesh's or text's corners, or its origin)."""
        entry = self.recipe["objects"][name]
        if entry["type"] == 'MESH':
            low, high = self.bounds(name)
        elif name in self.text_bounds:
            low, high = self.text_bounds[name]
        else:
            low = high = self.location(name)
        points = [Vector((x, y, z)) for x in (low.x, high.x) for y in (low.y, high.y)
                  for z in (low.z, high.z)]
        screen = [self._project(camera, point) for point in points]
        return (min(p[0] for p in screen), min(p[1] for p in screen),
                max(p[0] for p in screen), max(p[1] for p in screen))

    def shot(self, name, caption, marks=(), camera="InvCamera", crop_to=None, margin=0.05,
             width=1280, max_height=440):
        """A screenshot of the game (or the part around `crop_to` objects), with numbered marks
        just left of the objects in `marks`, in the order of the table that follows."""
        path = os.path.join(self.shots_dir, name + ".png") if self.shots_dir else ""
        if not path or not os.path.exists(path):
            return f'<p class="missing">(Picture {esc(name)} not made: run guide_screenshots.py)</p>'
        image = bpy.data.images.load(path)
        full_w, full_h = image.size
        box = (0.0, 0.0, 1.0, 1.0)
        if crop_to:
            extents = [self._extent(item, camera) for item in crop_to]
            box = (max(0.0, min(e[0] for e in extents) - margin),
                   max(0.0, min(e[1] for e in extents) - margin),
                   min(1.0, max(e[2] for e in extents) + margin),
                   min(1.0, max(e[3] for e in extents) + margin))
            pixels = numpy.empty(full_w * full_h * 4, dtype=numpy.float32)
            image.pixels.foreach_get(pixels)
            pixels = pixels.reshape(full_h, full_w, 4)
            x0, x1 = int(box[0] * full_w), int(box[2] * full_w)
            # Image rows go from the bottom up.
            y0, y1 = int((1.0 - box[3]) * full_h), int((1.0 - box[1]) * full_h)
            part = numpy.ascontiguousarray(pixels[y0:y1, x0:x1])
            cropped = bpy.data.images.new(f"guide_{name}_crop", x1 - x0, y1 - y0)
            cropped.pixels.foreach_set(part.ravel())
            bpy.data.images.remove(image)
            image = cropped
        source_w, source_h = image.size
        scale = min(width / source_w, 1.5)
        if crop_to:
            scale = min(scale, max_height / source_h)
        image.scale(max(1, int(source_w * scale)), max(1, int(source_h * scale)))
        scaled_w, scaled_h = image.size
        jpeg = os.path.join(bpy.app.tempdir, "guide_shot.jpg")
        # The screenshot's own colors (save_render would apply the view transform again).
        image.filepath_raw = jpeg
        image.file_format = 'JPEG'
        image.save(filepath=jpeg, quality=86)
        bpy.data.images.remove(image)
        data = base64.b64encode(open(jpeg, "rb").read()).decode("ascii")
        os.remove(jpeg)
        pins = []
        placed = []
        extents = {item: self._extent(item, camera) for item in marks if
                   item in self.recipe["objects"]}
        for number, item in enumerate(marks, 1):
            if item not in self.recipe["objects"]:
                raise RuntimeError(f"picture {name}: no object {item}")
            left, top, right, bottom = extents[item]
            to_x = lambda u: (u - box[0]) / (box[2] - box[0]) * scaled_w
            to_y = lambda v: (v - box[1]) / (box[3] - box[1]) * scaled_h
            x, y = to_x(left), to_y((top + bottom) / 2.0)
            if not (-5 <= x <= scaled_w and 0 <= y <= scaled_h):
                raise RuntimeError(f"picture {name}: {item} is outside it")
            holds = any(other != item and left <= e[0] and e[2] <= right and top <= e[1] and
                        e[3] <= bottom for other, e in extents.items())
            if holds:
                # A background with other marked pieces on it: inside its top left corner.
                x, y = max(x, 0) + 16, max(to_y(top), 0) + 16
            else:
                # Just left of the object; inside it when it starts at the picture's edge.
                x = x - 16 if x >= 32 else max(x, 0) + 16
            # Up (or down, at the top) from a mark already there.
            step = -28 if y > 40 else 28
            while any(abs(x - px) < 26 and abs(y - py) < 26 for px, py in placed):
                y += step
            placed.append((x, y))
            pins.append(f'<span class="pin" style="left:{x / scaled_w * 100:.2f}%;'
                        f'top:{y / scaled_h * 100:.2f}%">{number}</span>')
        names = ""
        if marks:
            names = " " + " · ".join(f"<b>{number}</b> {esc(item)}"
                                     for number, item in enumerate(marks, 1))
        return (f'<figure class="shot"><div class="shot-box"><img alt="{esc(caption)}" '
                f'src="data:image/jpeg;base64,{data}" width="{scaled_w}" height="{scaled_h}">'
                f'{"".join(pins)}</div><figcaption>{caption}{names}</figcaption></figure>')

    # Scene and input --------------------------------------------------------

    def _action_states(self):
        states = {}
        for entry in self.recipe["scene"]["input_actions"]:
            name = entry["settings"]["name"]
            start = self.start["actions"].get(name)
            if start is None:
                states[name] = "add"
            elif R._equal(start, {"settings": entry["settings"], "bindings": entry["bindings"]}):
                states[name] = "kept"
            else:
                states[name] = "changed"
        missing = set(self.start["actions"]) - set(states)
        if missing or "changed" in states.values():
            # The words say the example keeps the new scene's actions as they are.
            raise RuntimeError(f"the example removed {sorted(missing)} or changed "
                               f"{[n for n, s in states.items() if s == 'changed']}: "
                               "update the Input Map words")
        return states

    def _check_action(self, entry):
        """Stop when an action or a binding has a setting the Input Map table does not show
        (anything but these, as a new one has it)."""
        settings = {k: v for k, v in entry["settings"].items()
                    if k not in {"name", "type", "context"}}
        extra = differs(settings, self.start["action_defaults"][entry["settings"]["type"]])
        if extra:
            raise RuntimeError(f"action {entry['settings']['name']}: settings the guide does not "
                               f"show: {extra}")
        shown = {"device", "key", "mouse_button", "gamepad_control", "direction", "use_shift",
                 "use_ctrl", "use_alt"}
        for binding in entry["bindings"]:
            rest = {k: v for k, v in binding.items() if k not in shown}
            extra = differs(rest, self.start["binding_defaults"][binding["device"]])
            if extra:
                raise RuntimeError(f"action {entry['settings']['name']}: binding settings the "
                                   f"guide does not show: {extra}")

    def input_map(self, pause_chapter):
        """The actions to add (the UI controls every new scene has are left as they are)."""
        rows = []
        key_rna = bpy.types.GameInputBinding.bl_rna
        states = self._action_states()
        for entry in self.recipe["scene"]["input_actions"]:
            settings = entry["settings"]
            self._check_action(entry)
            if states[settings["name"]] == "kept":
                continue
            esc_key = any(binding["device"] == 'KEYBOARD' and binding["key"] == 'ESC'
                          for binding in entry["bindings"])
            when = ""
            if esc_key:
                if settings["name"] != "Pause Menu":
                    raise RuntimeError(f"action {settings['name']} uses Esc: say when to add it")
                when = f" <span class=\"what\">add it in chapter {pause_chapter}</span>"
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
                # A button of a vector action pushes one way; a stick is the whole vector.
                if settings["type"] == 'VECTOR' and not _is_stick(binding):
                    text += f" ({enum_label(key_rna, 'direction', binding['direction'])})"
                (gamepad if device == 'GAMEPAD' else keyboard).append(text)
            rows.append(
                f"<tr><td><b>{esc(settings['name'])}</b>{when}</td>"
                f"<td>{esc(enum_label(bpy.types.GameInputAction.bl_rna, 'type', settings['type']))}"
                f"</td><td>{esc(settings.get('context') or '')}</td>"
                f"<td>{esc(', '.join(keyboard))}</td><td>{esc(', '.join(gamepad))}</td></tr>")
        return ('<table class="data"><thead><tr><th>Action</th><th>Type</th><th>Context</th>'
                '<th>Keyboard and mouse</th><th>Gamepad</th></tr></thead><tbody>' +
                "".join(rows) + "</tbody></table>")

    def kept_actions(self):
        return [entry["settings"]["name"] for entry in self.recipe["scene"]["input_actions"]
                if self._action_states()[entry["settings"]["name"]] == "kept"]

    def scene_settings(self):
        """The scene's settings that are not as in a new file, with where they are."""
        rna = bpy.types.SceneGameData.bl_rna
        game = differs(R._settings(self.scene.game_settings, skip=R.SCENE_GAME_SKIP),
                       self.start["game"])
        rows = []
        # A new file's camera is deleted with its collection: the scene's is always set.
        if self.scene.camera is not None:
            rows.append(f"<tr><td>Scene tab › Scene › Camera</td>"
                        f"<td><b>{esc(self.scene.camera.name)}</b></td></tr>")
        for key, value in sorted(game.items()):
            place = SCENE_SETTING_PLACES.get(key)
            if place is None:
                raise RuntimeError(f"say where the scene setting {key} is (SCENE_SETTING_PLACES)")
            later = (f" (made in chapter {chapter('items')}: set it then)"
                     if key == "item_library" else "")
            rows.append(f"<tr><td>{esc(place)}</td>"
                        f"<td><b>{esc(setting_value(rna, key, value))}</b>{later}</td></tr>")
        render = differs({name: R.encode(getattr(self.scene.render, name))
                          for name in RENDER_KEYS}, self.start["render"])
        render_rna = bpy.types.RenderSettings.bl_rna
        for key, value in render.items():
            rows.append(f"<tr><td>Output tab › Format › {esc(prop_label(render_rna, key))}</td>"
                        f"<td><b>{esc(value_text(value))}</b></td></tr>")
        world = self.scene.world
        if world is not None:
            for key, value in differs(R._settings(world), self.start["world"]).items():
                rows.append(f"<tr><td>World tab › {esc(prop_label(bpy.types.World.bl_rna, key))}"
                            f"</td><td><b>{esc(value_text(value))}</b></td></tr>")
        return ('<table class="data"><tbody>' + "".join(rows) + "</tbody></table>")

    # Items and pickups ------------------------------------------------------

    def library(self):
        return [name for name in self.recipe["objects"]
                if T.ITEM_LIBRARY in self.homes.get(name, [])]

    def items_table(self):
        names = self.library()
        rows = []
        notes = []
        for name in names:
            self.shown_objects.add(name)
            entry = self.recipe["objects"][name]
            props = {prop["name"]: prop.get("value") for prop in entry["properties"]}
            materials = [m for m in entry["materials"] if m]
            low, high = self.bounds(name)
            rows.append(
                f"<tr><td><b>{esc(name)}</b></td><td>{esc(props.get('name', ''))}</td>"
                f"<td>{esc(props.get('category', ''))}</td><td>{esc(props.get('max_stack', ''))}</td>"
                f"<td>{esc(props.get('description', '')).replace(chr(10), '↵')}</td>"
                f"<td>{''.join(self.swatch(m) + esc(m) for m in materials)}</td>"
                f"<td class=\"num\">{esc(vec(high - low))}</td>"
                f"<td class=\"num\">{esc(vec(self.middle(name)))}</td></tr>")
            if "\n" in str(props.get("description", "")):
                notes.append(self.break_note(
                    f"Select {name}, then",
                    'C.object.game.properties["description"].value', props["description"]))
            if props.get("item") != name:
                raise RuntimeError(f"library item {name}: its item property is not its name")
        self.check_like(names, names[0],
                        {"name", "category", "max_stack", "description", "item", "middle"})
        model = self.view["objects"][names[0]]
        if [p["name"] for p in model["properties"]] != ["name", "max_stack", "category", "item",
                                                         "count", "description"]:
            raise RuntimeError("the items' properties are not those the words name")
        count = next(p for p in model["properties"] if p["name"] == "count")
        if (model["game"] != {"use_ghost": True} or model["settings"] or model["ui"] or
                count["value"] != 1 or model["physics"] != 'STATIC'):
            raise RuntimeError("the items' settings are not those the words name")
        self._check_unturned(names[0])
        self._check_plain_properties(names[0])
        head = "".join(f"<th>{esc(prop)} ({esc(self.item_kind(prop))})</th>"
                       for prop in ("name", "category", "max_stack", "description"))
        return ('<div class="scroll"><table class="data"><thead><tr><th>Object</th>' + head +
                '<th>Material</th><th>Size</th><th>Mesh middle</th></tr></thead><tbody>' +
                "".join(rows) +
                "</tbody></table></div>" + "".join(notes) + self.materials_for(names))

    def item_kind(self, prop):
        """The type of a library item's game property, as Blender's type menu names it."""
        model = self.recipe["objects"][self.library()[0]]
        kind = next(p["type"] for p in model["properties"] if p["name"] == prop)
        return enum_label(bpy.types.GameProperty.bl_rna, "type", kind)

    def _check_unturned(self, name):
        """The tables give no turn nor scale for items and pickups: stop on one that has."""
        world = Matrix(self.recipe["objects"][name]["world"])
        if (world.to_quaternion().angle > 1e-4 or
                any(abs(value - 1.0) > 1e-4 for value in world.to_scale())):
            raise RuntimeError(f"{name} is turned or scaled: the tables do not say so")

    def _check_plain_properties(self, name):
        """The item and pickup tables show no Save Game nor debug: stop on a property with one."""
        for prop in self.recipe["objects"][name]["properties"]:
            if prop["save_game"] or prop["debug"]:
                raise RuntimeError(f"{name}: {prop['name']} is Save Game or debug: say so")

    def pickups(self):
        return [name for name in self.recipe["objects"] if name.startswith("Pickup_")]

    def pickups_table(self):
        names = self.pickups()
        rows = []
        for name in names:
            self.shown_objects.add(name)
            entry = self.recipe["objects"][name]
            props = {prop["name"]: prop.get("value") for prop in entry["properties"]}
            rows.append(f"<tr><td>{esc(name)}</td><td>{esc(props.get('item'))}</td>"
                        f"<td>{esc(props.get('count'))}</td>"
                        f"<td class=\"num\">{esc(vec(self.location(name)))}</td>"
                        f"<td class=\"num\">{esc(vec(self.size(name)))}</td>"
                        f"<td class=\"num\">{esc(vec(self.middle(name)))}</td>"
                        f"<td>{esc(', '.join(m for m in entry['materials'] if m))}</td></tr>")
        self.check_like(names, names[0], {"item", "count", "middle"})
        model = self.view["objects"][names[0]]
        if ([p["name"] for p in model["properties"]] != ["item", "count"] or
                model["game"] != {"use_ghost": True} or model["settings"] or model["ui"] or
                model["physics"] != 'STATIC' or "Scene Collection" not in self.homes[names[0]]):
            raise RuntimeError("the pickups' settings are not those the words name")
        self._check_unturned(names[0])
        self._check_plain_properties(names[0])
        for name in names:
            item = next(p["value"] for p in self.recipe["objects"][name]["properties"]
                        if p["name"] == "item")
            if self.recipe["objects"][name]["materials"] != \
                    self.recipe["objects"][item]["materials"]:
                raise RuntimeError(f"{name} has not its item's material")
        kinds = {p["name"]: p["type"] for p in model["properties"]}
        kind = lambda prop: enum_label(bpy.types.GameProperty.bl_rna, "type", kinds[prop])
        return ('<details><summary>All ' + str(len(names)) + ' pickups</summary>'
                '<div class="scroll"><table class="data"><thead><tr><th>Object</th>'
                f'<th>item ({esc(kind("item"))})</th><th>count ({esc(kind("count"))})</th>'
                '<th>Location</th><th>Size</th><th>Mesh middle</th><th>Material</th>'
                '</tr></thead>'
                '<tbody>' + "".join(rows) + "</tbody></table></div></details>")

    def player_properties(self):
        rows = []
        for prop in self.view["objects"]["Player"]["properties"]:
            rows.append(f"<tr><td>{self.property_text(prop)}</td>"
                        f"<td>{PROPERTY_USES.get(prop['name'], '')}</td></tr>")
        return ('<table class="data"><thead><tr><th>Game property</th><th>Used by</th></tr>'
                '</thead><tbody>' + "".join(rows) + "</tbody></table>")


# Objects whose game properties a table shows (with what uses each one) instead of their row.
PROPERTIES_IN_A_TABLE = {"Player"}

PROPERTY_USES = {
    "stack_items": "Pick up; the Stack pickups checkbox",
    "inventory": "the bag, the chest's bag, picking up, dropping, crafting",
    "recipes": "the CRAFT list; dragging one item onto another",
    "walk_speed": "walking; the Settings slider; the HUD",
    "sort_by": "sorting; the Settings choice",
    "show_hints": "the Settings checkbox",
    "player_name": "the name tag and the HUD; the Settings name field",
    "weapon": "the WEAPON cell (a list of one)",
    "armor": "the ARMOR cell",
    "quick": "the QUICK cell",
    "stamina": "the Stamina frame; both stamina bars",
}


# -----------------------------------------------------------------------------
# What each logic frame does: short bullets beside the picture

FRAMES = {
    ("Game", "Pick up: add each touched item to the inventory, then remove it"): (
        "Every physics step, <b>On Collision</b> checks whether the Player touches an object "
        "with an <code>item</code> property.",
        "If so, <b>Inventory › Add Item</b> adds that object's item and count to the Player's "
        "<code>inventory</code>. Amount 0 means all it carries.",
        "Its <i>Override</i> is on, so it uses its own settings: Stack comes from the Player's "
        "<code>stack_items</code>. (Off, it follows the list's Inventory Rules: Scene tab › "
        "Items and UI › Inventory Rules, picked on the list's game property.)",
        "Then <b>Remove Object</b> deletes the pickup.",
        "It uses <i>Colliding</i>, not <i>On Enter</i>: On Enter fires once, so a second "
        "touching item would be missed.",
    ),
    ("Game", "Open or close the inventory with I or gamepad Select"): (
        "<b>Input Action</b> <code>Inventory</code> (Pressed) fires once per press of I or "
        "Select.",
        "<b>Open Object Menu</b> shows <code>InventoryMenu</code> through "
        "<code>InvCamera</code>.",
        "<i>Replace Top</i>: it replaces the menu on top, such as the chest's.",
        "<i>Toggle</i>: the same key closes it. Back (Esc, B) closes it too.",
        "Input Context “Inventory”: actions with this context (Sort) work only while it is on "
        "top.",
        "Routing <i>UI Only</i>: UI below it ignores the mouse and keys. The game's actions "
        "still work.",
    ),
    ("Game", "Walk with the Move action at the Settings speed"): (
        "Every physics step, <b>Apply Movement</b> moves the Player by the <code>Move</code> "
        "action's Vector.",
        "<b>Vector Math</b> (Scale) multiplies it by the Player's <code>walk_speed</code>.",
        "Local is off, so W walks along the world's +Y.",
    ),
    ("Game", "Open or close the chest with Use when next to it"): (
        "<code>Use</code> (E, A) opens <code>ChestMenu</code> only when the Player is closer "
        "than 2.2 to the Chest.",
        "The menu settings are the inventory's, with Input Context “Chest”.",
    ),
    ("Game", "Stamina: drains while walking, comes back while standing"): (
        "Every physics step: while <code>Move</code> is Active, <code>stamina</code> drops by "
        "0.3; otherwise it rises by 0.5.",
        "<i>Clamp</i> keeps it between 0 and 100. Both stamina bars show it by themselves.",
    ),
    ("Game", "Pause Menu: open with the Pause Menu action"): (
        "<code>Pause Menu</code> (Esc, Start) opens the <code>Pause Menu</code> collection with "
        "<i>Push</i> and <i>Pause Game</i>.",
        "While it is open no physics step runs. Menu buttons still work. The game goes on when "
        "it closes.",
        "<i>Toggle</i>: Start closes it again, even while paused.",
        "That needs a straight link: Input Action into Open Object Menu, nothing linked after "
        "it.",
        "Esc closes it as Back. <i>Also When UI Uses It</i> is off, so that Esc does not reopen "
        "it.",
        "An action you add on Esc takes Esc from the Exit Key (Render tab › Game Debug). Esc no "
        "longer ends the game, so the menu has a Quit button.",
    ),
    ("Game", "UI Sounds: the menus' Move, Accept, Back and Denied sounds"): (
        "<b>On UI Sound</b> runs at every menu's sound moments: <i>Move</i> (the highlight moves "
        "to another cell or button), <i>Accept</i> (a click, Enter or A), <i>Back</i> (Back "
        "closes a menu), <i>Denied</i> (a press that does nothing, a drag that goes back).",
        "Each runs a <b>Play Sound</b> with its tone, in the volume group <i>UI</i>: a Set Volume "
        "node with group UI turns them all up or down.",
        "It is in the Game tree on the Player, outside every menu: the logic of a menu stops when "
        "Back closes it.",
        "A sound per button: a Switch Flow (String) on On UI Sound's Action, between Accept and "
        "Play Sound.",
    ),
    ("Game", "HUD: show it from the start"): (
        "<b>On Init</b> runs once. <b>Set Overlay Collection</b> shows the <code>HUD</code> "
        "collection through <code>InvCamera</code>.",
        "It is not a menu: Back and Close Object Menu leave it. It takes no keys.",
        "<code>HUD Root</code> is 1 below the menus, so an open menu covers the HUD.",
    ),
    ("Inventory", "Grid: 3 x 3 copies of Slot, scrollbar beside them"): (
        "<b>Get Object Property</b> reads the Player's <code>inventory</code> table.",
        "<b>Object Grid View</b> shows it in copies of <code>Slot</code>: 3 columns × 3 rows, "
        "1.4 apart.",
        "<i>Show Empty</i> keeps empty cells to drop on. <i>Show Counts</i> writes counts above "
        "1 into <code>SlotCount</code>.",
        "The scene's Item Library gives <code>SlotIcon</code> each item's mesh.",
        "<code>ScrollBar</code> is the scrollbar; its first mesh child is the handle. Both hide "
        "while all items fit.",
        "Viewport Object is empty, so the wheel scrolls the grid over the whole Panel.",
    ),
    ("Inventory", "Pages: < and > turn the page, Page / Page Count between"): (
        "A click on <code>PagePrev</code> or <code>PageNext</code> (Actions page_prev, "
        "page_next) turns the page.",
        "On Scroll and On Rebind (joined by <b>Any Flow</b>) write “Page {} / {}” into "
        "<code>PageText</code>.",
    ),
    ("Inventory", "Delete: left click a slot (or Enter / A) removes 1 item"): (
        "A click on a cell (Action <code>inventory</code>), or Return, Space or A on the selected "
        "one, fires <i>Click</i>.",
        "<b>Inventory › Remove From Slot</b> takes 1 from that slot (Index).",
    ),
    ("Inventory", "Highlight a cell: a tooltip with name and description"): (
        "<i>Highlight</i> fires when an element is pointed at or selected. When the highlight "
        "leaves, Object is empty.",
        "<b>Get Item Info</b> finds the item in the Item Library. Its name and description fill "
        "<code>TooltipText</code> (“{}↵{}”: a line break between).",
        "The <code>Tooltip</code> moves to the element plus (0.95, -1, 1.2). It shows only when "
        "an item was found.",
        "The tooltip and its text start hidden.",
    ),
    ("Inventory", "Sort with the Sort action, then show the top"): (
        "<code>Sort</code> (R, RT) has Context “Inventory”: it works only while this menu is on "
        "top.",
        "If <code>sort_by</code> contains “Count”, <b>Inventory › Sort Inventory</b> sorts by "
        "count, smallest stacks first.",
        "Otherwise it sorts A to Z by item.",
        "Then <b>Scroll Object View › Top</b> shows the first row.",
    ),
    ("Inventory", "Drop: drag a cell out of the menu to drop it in the world"): (
        "<i>Drop Outside</i>: a cell was let go over a world object, like the ground.",
        "<b>Remove From Slot</b> takes the whole stack from the list the cell's Action names.",
        "<b>Drop Item</b> puts a copy of the item's library object at that Point, with item "
        "and count. It appears at the next physics step.",
    ),
    ("Inventory", "Drop with gamepad X or right click: lands in front of you"): (
        "<i>Secondary Click</i>: right mouse on a bag cell, or X on the selected one.",
        "It takes the whole stack. <b>Drop Item</b> puts it at ground height, 1.6 ahead of the "
        "Player (+Y).",
    ),
    ("Inventory", "Move or equip: from the Dropped Action list to the Action list"): (
        "<i>Drop Received</i>: a cell was dropped on a cell. Each cell's Action is the name of "
        "the list it shows.",
        "<b>Move To Slot</b> takes from list <i>Dropped Action</i>, slot <i>Dropped Index</i> "
        "(From Slot).",
        "It puts into list <i>Action</i>, slot <i>Index</i> (To Slot).",
        "Empty cell: the stack moves to the end of that list.",
        "Same item: it stacks, up to max_stack. Other item: they swap.",
        "Its <i>Override</i> is on, for its own <i>Recipes</i> (the Player's "
        "<code>recipes</code>): an item dropped on another makes a recipe of just those two. Same "
        "list only.",
    ),
    ("Inventory", "EQUIP: each cell shows its own list; Accept Categories"): (
        "Three grids of one cell show the Player's <code>weapon</code>, <code>armor</code> and "
        "<code>quick</code> tables.",
        "Their Viewport Object is <code>EquipPlate</code> (the bag uses the Panel).",
        "Each cell's <i>Accept Categories</i> says which items it takes. A refused item goes "
        "back, with the Denied sound.",
        "An equipped item can go back only to an empty cell, or swap with an item its cell "
        "takes.",
    ),
    ("Inventory", "Drop Enter: say if the cell takes it; red frame if not"): (
        "<i>Drop Enter</i>: a dragged cell is over a drop target. <i>Accepted</i> says if it "
        "takes it.",
        "Taken: <code>ItemName</code> says “Put {name} here”.",
        "Refused: it says “{name} does not fit here”, and <code>RefuseFrame</code> moves just "
        "behind the cell and shows.",
    ),
    ("Inventory", "Drop Exit: clear the line and the red frame"): (
        "<i>Drop Exit</i>: the drag left the cell, ended or was cancelled.",
        "<code>ItemName</code> is cleared and <code>RefuseFrame</code> hides.",
    ),
    ("Inventory", "Craft: click or A on a recipe; the grid shows what it made"): (
        "A second grid shows the Player's <code>recipes</code> in copies of "
        "<code>RecipeSlot</code>: 1 column, 3 rows.",
        "Each recipe cell shows its result's icon, and its count above 1.",
        "<i>Advanced › Priority</i> 1: over <code>CraftPlate</code> the wheel scrolls this list, "
        "not the bag.",
        "A click on a recipe: <b>Inventory › Craft</b> makes it once (Times 1) from the Player's "
        "inventory.",
        "<code>ItemName</code> shows the Outcome and what was Made. The bag scrolls to the new "
        "item.",
    ),
    ("Inventory", "Highlight a recipe: show what it takes"): (
        "Pointing at a recipe (Action <code>recipe</code>) writes its text into "
        "<code>ItemName</code>.",
    ),
    ("Inventory", "Settings button: open Settings on top of the inventory"): (
        "A click on <code>SettingsButton</code> opens <code>SettingsMenu</code> with "
        "<i>Push</i>.",
        "The inventory stays open but hidden (Presentation <i>Screen</i>). Back or Close returns "
        "to it.",
    ),
    ("Pause Menu", "Resume: close the pause menu (the game goes on)"): (
        "<b>Close Object Menu</b> (Top Menu) closes the pause menu, and the game goes on.",
    ),
    ("Pause Menu", "Settings: open the Settings menu on top"): (
        "Opens <code>SettingsMenu</code> on top (Push). The game stays paused.",
    ),
    ("Pause Menu", "Quit: end the game"): (
        "<b>Quit Game</b> ends the game.",
    ),
    ("Settings", "Show hints toggle: show or hide the hint in the world"): (
        "<code>HintsToggle</code> (Action <code>hints</code>) fires <i>Changed</i> with its new "
        "value.",
        "<b>Set Object Attribute</b> shows or hides <code>Hint</code> with it.",
        "The other widgets need no logic: they write <code>walk_speed</code>, "
        "<code>sort_by</code> and <code>player_name</code> by themselves.",
    ),
    ("Settings", "Grab key: the next key pressed becomes Grab"): (
        "A click on <code>RebindButton</code>: <b>Rebind Input Action</b> waits up to 5 seconds "
        "for a key or mouse button.",
        "It replaces G, <code>Grab</code>'s keyboard and mouse control. Esc cancels.",
        "The button's text shows the new key by itself.",
    ),
    ("Settings", "Close: back to the menu below"): (
        "<b>Close Object Menu</b> (Top Menu) closes Settings. The menu below shows again.",
    ),
    ("Chest", "Chest list: the chest's items with their names"): (
        "Shows the Chest's <code>chest</code> table in copies of <code>ChestRow</code>: 1 column, "
        "6 rows, 0.7 apart.",
        "<i>Show Names</i> writes each item's name into <code>ChestRowText</code>, with “ x3” "
        "when there are more than one.",
    ),
    ("Chest", "Bag: your inventory as a grid"): (
        "The Player's <code>inventory</code> in copies of <code>BagSlot</code>: 3 × 3, 1.4 apart, "
        "no scrollbar.",
    ),
    ("Chest", "Drop on the chest: from the bag, or reorder the chest"): (
        "<i>Drop Received</i> on a chest row. <b>Switch Flow</b> checks where it came from "
        "(<i>Dropped Action</i>).",
        "From the bag: <b>Move To Slot</b> into the Chest's <code>chest</code> (To Object, To "
        "Property).",
        "From the chest: within the chest.",
    ),
    ("Chest", "Drop on the bag: from the chest, or reorder the bag"): (
        "The same for a bag cell: from the chest into the inventory, or within the bag.",
    ),
}


# -----------------------------------------------------------------------------
# Diagrams (the words in them are the example's own)

DIAGRAM_MENU = """
<svg class="diagram" viewBox="0 0 760 170" role="img" aria-label="How a menu opens">
<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7"
 orient="auto"><path d="M0,0 L10,5 L0,10 z" class="d-arrowhead"/></marker></defs>
<rect x="10" y="20" width="220" height="140" rx="10" class="d-box"/>
<text x="120" y="45" class="d-title">Player › Game tree</text>
<text x="120" y="72" class="d-text">Input Action “Inventory”</text>
<text x="120" y="94" class="d-text">→ Open Object Menu</text>
<text x="120" y="120" class="d-small">Collection: InventoryMenu</text>
<text x="120" y="140" class="d-small">Camera: InvCamera</text>
<path d="M232,90 H290" class="d-line" marker-end="url(#arrow)"/>
<rect x="292" y="20" width="220" height="140" rx="10" class="d-box"/>
<text x="402" y="45" class="d-title">InventoryMenu (collection)</text>
<text x="402" y="72" class="d-text">Panel, with tree Inventory</text>
<text x="402" y="94" class="d-text">└ every piece is its child</text>
<text x="402" y="130" class="d-small">asleep until the menu opens</text>
<path d="M514,90 H572" class="d-line" marker-end="url(#arrow)"/>
<rect x="574" y="20" width="176" height="140" rx="10" class="d-box"/>
<text x="662" y="45" class="d-title">While it is open</text>
<text x="662" y="72" class="d-text">the Inventory tree runs</text>
<text x="662" y="94" class="d-text">Back or Toggle</text>
<text x="662" y="116" class="d-text">closes it</text>
</svg>"""

DIAGRAM_GRID = """
<svg class="diagram" viewBox="0 0 760 190" role="img" aria-label="How a grid fills">
<defs><marker id="arrow2" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7"
 orient="auto"><path d="M0,0 L10,5 L0,10 z" class="d-arrowhead"/></marker></defs>
<rect x="10" y="20" width="200" height="150" rx="10" class="d-box"/>
<text x="110" y="45" class="d-title">Player.inventory</text>
<text x="110" y="78" class="d-text">Sword | 1</text>
<text x="110" y="103" class="d-text">Apple | 2</text>
<text x="110" y="128" class="d-text">Coin | 5</text>
<text x="110" y="155" class="d-small">a Table: item, count</text>
<rect x="250" y="20" width="230" height="150" rx="10" class="d-box"/>
<text x="365" y="45" class="d-title">Slot (the first cell)</text>
<text x="365" y="78" class="d-text">first other mesh → icon</text>
<text x="365" y="103" class="d-text">first text child → count</text>
<text x="365" y="128" class="d-text">Highlight Object → glow</text>
<text x="365" y="155" class="d-small">Object Grid View copies it</text>
<path d="M212,95 H248" class="d-line" marker-end="url(#arrow2)"/>
<path d="M482,95 H518" class="d-line" marker-end="url(#arrow2)"/>
<g class="d-grid">
<rect x="525" y="25" width="60" height="44" rx="5"/><rect x="595" y="25" width="60" height="44" rx="5"/>
<rect x="665" y="25" width="60" height="44" rx="5"/><rect x="525" y="77" width="60" height="44" rx="5"/>
<rect x="595" y="77" width="60" height="44" rx="5"/><rect x="665" y="77" width="60" height="44" rx="5"/>
<rect x="525" y="129" width="60" height="44" rx="5"/><rect x="595" y="129" width="60" height="44" rx="5"/>
<rect x="665" y="129" width="60" height="44" rx="5"/>
</g>
<text x="555" y="52" class="d-small">Sword</text><text x="625" y="52" class="d-small">Apple 2</text>
<text x="695" y="52" class="d-small">Coin 5</text>
<text x="625" y="186" class="d-small">3 × 3 copies, 1.4 apart</text>
</svg>"""

DIAGRAM_ITEMS = """
<svg class="diagram" viewBox="0 0 760 160" role="img" aria-label="Items and pickups">
<defs><marker id="arrow3" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7"
 orient="auto"><path d="M0,0 L10,5 L0,10 z" class="d-arrowhead"/></marker></defs>
<rect x="10" y="20" width="220" height="120" rx="10" class="d-box"/>
<text x="120" y="45" class="d-title">Pickup_00_Apple (world)</text>
<text x="120" y="75" class="d-text">item “Apple”, count 1</text>
<text x="120" y="100" class="d-small">the Player walks into it</text>
<path d="M232,80 H290" class="d-line" marker-end="url(#arrow3)"/>
<rect x="292" y="20" width="200" height="120" rx="10" class="d-box"/>
<text x="392" y="45" class="d-title">Player.inventory</text>
<text x="392" y="75" class="d-text">row: Apple | 1</text>
<text x="392" y="100" class="d-small">Inventory › Add Item</text>
<path d="M494,80 H552" class="d-line" marker-end="url(#arrow3)"/>
<rect x="554" y="20" width="196" height="120" rx="10" class="d-box"/>
<text x="652" y="45" class="d-title">ItemLibrary › Apple</text>
<text x="652" y="72" class="d-text">mesh → the icon</text>
<text x="652" y="96" class="d-small">name, category, max_stack,</text>
<text x="652" y="114" class="d-small">description</text>
</svg>"""


# -----------------------------------------------------------------------------
# The chapters

CHAPTERS = ("start", "player", "items", "inventory", "sounds", "equipment", "crafting", "chest",
            "settings", "pause", "hud", "finish")


def chapter(cid):
    return CHAPTERS.index(cid) + 1


def chapters(ex):
    """The guide, chapter by chapter: (id, title, template label or None, HTML)."""
    inventory = T.INVENTORY_PARTS
    recipe = ex.recipe
    parts = lambda part: T._part_objects(inventory, recipe, part)
    pause_parts = lambda part: T._part_objects(T.PAUSE_PARTS, recipe, part)
    in_collection = lambda name: [ob for ob in recipe["objects"] if name in ex.homes.get(ob, [])]
    frames = lambda tree, *titles: "".join(ex.frame_block(tree, title) for title in titles)
    all_frames = lambda tree: [frame["label"] for frame in recipe["trees"][tree]["frames"]]
    out = []

    world = ["Ground", "Sun", "Camera_Parent", "MainCamera"]
    out.append(("start", "Before you start", None, f"""
<p>The example starts from a new file (General) without its Cube, Light and Camera, and their
collection. Add the world first: the player needs ground to stand on.</p>
{ex.objects_table(world)}
<p>Scene settings the example changes:</p>
{ex.scene_settings()}
<p>A new scene's Game Input Map (Scene tab) has the UI controls already:
{esc(", ".join(ex.kept_actions()))}.</p>
<p>Add these actions:</p>
{ex.input_map(chapter("pause"))}"""))

    player = ["Player", "Stamina Bar.001", "Stamina Bar.001 Fill"]
    out.append(("player", "The player", "Walking", f"""
<p class="goal">A box that walks, and holds every value the menus show and change.</p>
<p class="tpl">The Walking template adds the Walk frame, the Move action and walk_speed. It adds
no stamina bar.</p>
{ex.shot("world", "The player and its stamina bar.", marks=("Player", "Stamina Bar.001"),
         camera="MainCamera", crop_to=("Player", "Stamina Bar.001"), margin=0.12, width=700)}
{ex.objects_table(player, marks=("Player", "Stamina Bar.001"))}
<p>The Player's game properties:</p>
{ex.player_properties()}
<p>Make a logic tree <b>Game</b> and apply it to the Player. Its other frames come in later
chapters.</p>
{frames("Game", "Walk with the Move action at the Settings speed",
        "Stamina: drains while walking, comes back while standing")}
<p class="try"><b>Try it:</b> walk with W A S D or the left stick. The bar drains, and refills
when you stop.</p>"""))

    library = ex.library()
    out.append(("items", "Items and pickups", "Pickups", f"""
<p class="goal">Items live in an Item Library. Pickups in the world add them to the inventory.</p>
<p class="tpl">The Pickups template adds the Item Library, the Pick up frame and 8 pickups next to
your player.</p>
<div class="scroll">{DIAGRAM_ITEMS}</div>
{ex.shot("world", "Pickups ahead of the player.", marks=("Pickup_00_Apple",), camera="MainCamera",
         crop_to=ex.pickups(), margin=0.03, width=1000)}
<ol class="steps">
<li>Make a collection <b>UI</b>, and in it a collection <b>ItemLibrary</b>. Set it as the Item
Library (Scene tab › Items and UI).</li>
<li>Add one object per item, named like the item. Turn on Ghost (Physics tab), so a dropped
copy does not block the player.</li>
<li>Give it the game properties <code>name</code>, <code>max_stack</code>,
<code>category</code> and <code>description</code>. The values are below. The example also gives
each item <code>item</code> ({esc(ex.item_kind("item"))}, its own name) and <code>count</code>
({esc(ex.item_kind("count"))}) 1.</li>
<li>Shape: any mesh. Put its origin at the bottom: the table gives the mesh's middle from the
origin. A dropped item is placed by its origin.</li>
<li>Material: give Emission Color the same color as Base Color, as below. With only Base Color
changed, the icon keeps the old Emission Color.</li>
</ol>
{ex.items_table()}
<p>A <b>pickup</b> looks like its item. It is in the Scene Collection, with the item's
material, Ghost on, and the properties <code>item</code> and <code>count</code>. The example has
{len(ex.pickups())} of them, in rows ahead of the player.</p>
{ex.pickups_table()}
{frames("Game", "Pick up: add each touched item to the inventory, then remove it")}
<p class="try"><b>Try it:</b> walk into items: they disappear into the inventory.</p>"""))
    ex.shown_objects.update(library)

    base = parts("base")
    # Not ScrollBar: it hides while all items fit, as in the picture.
    base_marks = ("Panel", "InventoryTitle", "Slot", "PagePrev", "PageText", "PageNext",
                  "StackToggle", "StackLabel", "DropHint")
    out.append(("inventory", "The inventory", "Inventory", f"""
<p class="goal">A 3 × 3 bag with pages, a scrollbar, a tooltip, sorting, deleting and
dropping.</p>
{ex.shot("inventory", "The inventory.", marks=base_marks)}
<p>Put <b>InvCamera</b> in the UI collection. Every menu and the HUD are seen through it.</p>
{ex.objects_table(["InvCamera"])}
<p>The collection <b>InventoryMenu</b> in UI. Every piece is a child of <code>Panel</code>. The
grid copies <code>Slot</code> with its children. (The equipment, crafting and Settings pieces
come in later chapters.)</p>
{ex.objects_table(base, marks=base_marks)}
<p>Make a tree <b>Inventory</b> and apply it to the Panel.</p>
{frames("Inventory", *inventory["base"]["frames"])}
<p>The Stack pickups checkbox needs no logic: it is a Toggle widget for the Player's
<code>stack_items</code>.</p>
<p>The Player opens the inventory:</p>
{frames("Game", "Open or close the inventory with I or gamepad Select")}
<p class="try"><b>Try it:</b> I or Select opens and closes it. Esc closes it too. Point at a cell
for the tooltip. Drag a cell onto another, click to delete one, drag one out to drop it. R sorts.
</p>"""))

    out.append(("sounds", "Menu sounds", "UI Sounds", f"""
<p class="goal">A short tone when the highlight moves, on a click, when Back closes a menu, and
when a press does nothing.</p>
<p class="tpl">The UI Sounds template adds this frame and the example's tones. A menu template adds
it too while your scene plays no menu sounds.</p>
<p>The tones are packed in the example. Append them (File › Append ›
<code>game_ui_example.blend</code>, next to this page › Sound) or use your own.</p>
{frames("Game", "UI Sounds: the menus' Move, Accept, Back and Denied sounds")}
<p class="try"><b>Try it:</b> open the inventory and point at the cells: each move ticks. Click a
cell, and close the inventory with Esc.</p>"""))

    equipment = parts("equipment")
    equipment_marks = ("EquipPlate", "EquipTitle", "WeaponSlot", "ArmorSlot", "QuickSlot")
    out.append(("equipment", "Equipment", "Inventory, with Equipment on", f"""
<p class="goal">Three cells that take only some kinds of items.</p>
{ex.shot("inventory", "The EQUIP column.", marks=equipment_marks, crop_to=equipment_marks,
         margin=0.03, width=520)}
{ex.objects_table(equipment, marks=equipment_marks)}
{frames("Inventory", *inventory["equipment"]["frames"])}
<p class="try"><b>Try it:</b> drag the sword onto WEAPON: it goes in. Drag an apple onto ARMOR:
a red frame shows, and it goes back.</p>"""))

    crafting = parts("crafting")
    crafting_marks = ("CraftPlate", "CraftTitle", "RecipeSlot", "CraftScrollBar")
    out.append(("crafting", "Crafting", "Inventory, with Crafting on", f"""
<p class="goal">A list of recipes. Click one to make it.</p>
{ex.shot("inventory", "The CRAFT column.", marks=crafting_marks, crop_to=crafting_marks,
         margin=0.03, width=560)}
<p>A recipe is a text such as “Apple + Mushroom = Potion”. A number asks for more than one:
“Sword + Gem + Potion + 2 Coin = Magic Sword”.</p>
{ex.objects_table(crafting, marks=crafting_marks)}
{frames("Inventory", *inventory["crafting"]["frames"])}
<p class="try"><b>Try it:</b> point at a recipe to see what it needs. Click it with the items in
the bag. Dragging an Apple onto a Mushroom makes a Potion too.</p>"""))

    chest_menu = in_collection("ChestMenu")
    chest_marks = ("ChestPanel", "ChestTitle", "ChestRow", "BagTitle", "BagSlot", "ChestHint")
    out.append(("chest", "The chest", "Chest", f"""
<p class="goal">A box in the world. Its menu shows the chest and the bag side by side.</p>
{ex.shot("world", "The chest.", marks=("Chest", "ChestLabel"), camera="MainCamera",
         crop_to=("Chest", "ChestLabel"), margin=0.06, width=520)}
{ex.objects_table(["Chest", "ChestLabel"], marks=("Chest", "ChestLabel"))}
{ex.shot("chest", "The chest's menu.", marks=chest_marks)}
<p>The collection <b>ChestMenu</b> in UI.</p>
{ex.objects_table(chest_menu, marks=chest_marks)}
<p>Make a tree <b>Chest</b> and apply it to the ChestPanel.</p>
{frames("Chest", *all_frames("Chest"))}
<p>The Player opens it:</p>
{frames("Game", "Open or close the chest with Use when next to it")}
<p class="try"><b>Try it:</b> walk to the chest and press E (A). Drag items both ways, or carry
them with G (Y).</p>"""))

    settings_menu = in_collection("SettingsMenu")
    settings_marks = ("SettingsPanel", "SettingsTitle", "CloseButton", "WalkSlider", "WalkValue",
                      "SortSelector", "HintsToggle", "NameField", "RebindButton", "SettingsHint")
    out.append(("settings", "Settings", "Settings Menu", f"""
<p class="goal">Widgets that change the Player's properties, and a key rebind.</p>
{ex.shot("settings", "The Settings menu.", marks=settings_marks)}
<p>Four widgets change the Player's properties: a Slider (<code>walk_speed</code>), a Selector
(<code>sort_by</code>), a Toggle (<code>show_hints</code>) and a Text Field
(<code>player_name</code>). A widget writes its property only when it is used.</p>
<p>The collection <b>SettingsMenu</b> in UI.</p>
{ex.objects_table(settings_menu, marks=settings_marks)}
<p>Make a tree <b>Settings</b> and apply it to the SettingsPanel.</p>
{frames("Settings", *all_frames("Settings"))}
<p>In the world, the name tag and the hint the checkbox hides:</p>
{ex.shot("world", "The name tag and the hint.", marks=("NameTag", "Hint"), camera="MainCamera",
         crop_to=("NameTag", "Hint", "Player"), margin=0.07, width=900)}
{ex.objects_table(["NameTag", "Hint"], marks=("NameTag", "Hint"))}
<p>A button in the inventory opens Settings (the pause menu has one too):</p>
{ex.shot("inventory", "The inventory's Settings button.", marks=("SettingsButton",),
         crop_to=("SettingsButton", "InventoryTitle"), margin=0.03, width=520)}
{ex.objects_table(parts("settings"), marks=("SettingsButton",))}
{frames("Inventory", *inventory["settings"]["frames"])}
<p class="try"><b>Try it:</b> open Settings from the inventory. Move the slider and walk faster.
Type a name: the name tag changes. Click Grab key and press a key.</p>"""))

    pause = pause_parts("base") + pause_parts("settings")
    pause_marks = ("Pause Menu Panel", "Pause Menu Title", "Resume", "Settings", "Quit")
    out.append(("pause", "The pause menu", "Pause Menu", f"""
<p class="goal">Pauses the game, with Resume, Settings and Quit.</p>
{ex.shot("pause", "The pause menu.", marks=pause_marks)}
<p>The collection <b>Pause Menu</b> in UI.</p>
{ex.objects_table(pause, marks=pause_marks)}
<p>Make a tree <b>Pause Menu</b> and apply it to the Pause Menu Panel.</p>
{frames("Pause Menu", T.PAUSE_PARTS["base"]["frames"][0], *T.PAUSE_PARTS["settings"]["frames"],
        *T.PAUSE_PARTS["base"]["frames"][1:])}
<p>Now add the <b>Pause Menu</b> action (chapter {chapter("start")}). The Player opens the menu
with it:</p>
{frames("Game", "Pause Menu: open with the Pause Menu action")}
<p class="try"><b>Try it:</b> Esc or Start pauses: the player stops. Resume, Esc or Start goes on.
Quit ends the game.</p>"""))

    hud = in_collection("HUD")
    out.append(("hud", "The HUD", None, f"""
<p class="goal">A stamina bar and a line of text, always on screen.</p>
{ex.shot("world", "The HUD.", marks=("Stamina Bar", "Value Text"),
         crop_to=("Stamina Bar", "Value Text"), margin=0.03, width=520)}
<p>The collection <b>HUD</b> in UI. Its pieces keep their place from the top left corner at any
window size (Screen Anchor).</p>
{ex.objects_table(hud, marks=("Stamina Bar", "Value Text"))}
{frames("Game", "HUD: show it from the start")}
<p class="try"><b>Try it:</b> the bar and the text show from the start. Walking drains the bar.</p>
"""))

    out.append(("finish", "Finish", None, f"""
<ol class="steps">
<li>Turn on <b>Disable in Viewports</b> for the UI collection. It is the screen icon in the
Outliner; show it with Outliner › Filter › Restriction Toggles.</li>
<li>The menus then get out of the way in the 3D view. The library's items stay out of the game
world.</li>
<li>Never exclude the UI collection: the game needs it.</li>
<li>Play and try every chapter's “Try it” with the mouse, the keyboard and a gamepad.</li>
</ol>
<h3>Common mistakes</h3>
<ul class="rules">
<li><b>Icon in the wrong color:</b> Emission Color must match Base Color.</li>
<li><b>Items not picked up:</b> a pickup needs an <code>item</code> property. On Collision looks
for it.</li>
<li><b>An item with no icon or name:</b> its <code>item</code> must name an Item Library
object.</li>
<li><b>One cell, or cells in the wrong place:</b> the Slot Template is the first cell. Rows above
0 make the copies, Spacing apart.</li>
<li><b>A piece left behind when the panel moves:</b> make it a child of the panel.</li>
<li><b>A menu never shows:</b> the UI collection must not be excluded. Open Object Menu needs its
Camera and Collection.</li>
<li><b>A text does not update:</b> braces go in the text's body.</li>
<li><b>Esc no longer quits:</b> an action you added on Esc takes it. Add a Quit button.</li>
<li><b>The pause key does not close the menu:</b> link the Input Action straight into Open Object
Menu, and nothing out of it.</li>
<li><b>Clicks go through to the game:</b> the panel needs UI Element on (Pointer stays
Block).</li>
<li><b>Two menus' logic in one tree:</b> keep one tree per menu, on its panel.</li>
</ul>"""))
    return out


RULES = f"""
<section id="rules"><h2>How UPBGE object UI works</h2>
<div class="scroll">{DIAGRAM_MENU}</div>
<ul class="rules">
<li><b>A menu is a collection.</b> The example keeps them all in the UI collection. Keep every
piece a child of its panel, so it moves with it.</li>
<li><b>Build with the UI collection visible.</b> Turn on its Disable in Viewports at the end
(chapter {chapter("finish")}).</li>
<li><b>A grid copies its first cell</b> with the cell's children. The first mesh child (not the
glow) shows each item's icon. The first text child shows its count.</li>
<li><b>Open Object Menu</b> shows a menu through a camera of its own (InvCamera). The menu's tree
sits on its panel and runs only while the menu is open.</li>
<li><b>A UI Element</b> is any object with Game Components tab › UI Element on. It takes clicks,
focus and drags.</li>
<li><b>On UI Event</b> reports what happens to an element, with the element's Action name.</li>
<li><b>A tree runs on the object it is applied to.</b> Open the tree in the Logic Node Editor,
select the object, then Game Components tab › Logic Nodes › Apply To Selected.</li>
<li><b>Widgets</b> (Slider, Toggle, Selector, Text Field, Bar) change or show a game property by
themselves.</li>
<li><b>Live text:</b> braces in a text's body show values and keys: <code>{{Player.walk_speed:2}}</code>,
<code>{{player_name}}</code> (its own, else the nearest parent's), <code>{{Move}}</code>,
<code>{{Grab:Gamepad}}</code>.</li>
</ul>
<div class="scroll">{DIAGRAM_GRID}</div>
<h3>Reading the tables</h3>
<ul class="rules">
<li><b>Location</b> of a child is its offset from the parent. Parent it with Ctrl P › Object
(Without Inverse), then type the Location. Other objects use world positions.</li>
<li><b>Shape</b>: Add › Mesh › Plane or Cube, sized in Edit Mode so the object's Scale stays 1.
An upright plane is a plane turned 90° on X in Edit Mode. A text's Size is in the Data tab ›
Font › Transform, its Horizontal and Vertical in Paragraph › Alignment.</li>
<li><b>Mesh middle</b>: where the mesh sits from the origin, when not at it. Move the mesh by it in
Edit Mode.</li>
<li><b>Settings</b> lists only what differs from a new object or node. What most objects share is
said once above the table.</li>
<li><b>UI</b>, <b>Drag</b>, <b>Look</b>, <b>Widget</b>, <b>Nav</b>, <b>Adv</b>: Game Components tab
› UI Element and its sub-panels Drag and Drop, Look, Widget, Navigation, Advanced.</li>
<li>Numbers in circles match the screenshot above the table.</li>
<li>A chapter's badge names its template. A menu's template adds the whole chapter for your
player: objects, logic, actions and properties. Walking and Pickups add less; their chapters say
what.</li>
</ul>
</section>"""


CSS = """
:root { --bg:#f5f6f8; --fg:#1d2127; --muted:#5c6470; --rule:#d8dce2; --surface:#fff;
  --accent:#2f7d74; --accent-soft:#dcefeb; --note-bg:#f4ecdf; --note-fg:#6d4a1f;
  --mark:#ffd166; --canvas:#2c2e33; --node:#3d4047; --node-line:#202226; --node-fg:#e4e6ea;
  --node-dim:#a9aeb7; --node-value:#f2d38a; --field:#2a2c31;
  --h-event:#8c3a3a; --h-ui:#2c6e67; --h-data:#36588a; --h-object:#85532a; --h-flow:#565a66;
  --h-math:#4f4a7a;
  --s-exec:#f0f0f0; --s-object:#ee9d3f; --s-string:#6ec1f0; --s-generic:#a6a9b0; --s-list:#b48be0;
  --s-int:#5fae6e; --s-float:#9fa3a8; --s-bool:#e19fd0; --s-dict:#d9c26a; --s-collection:#f4f4f4;
  --s-vector:#7a6ee0;
  --font: "Segoe UI", system-ui, -apple-system, "Helvetica Neue", Arial, sans-serif;
  --mono: ui-monospace, "Cascadia Mono", Menlo, Consolas, monospace; }
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg:#17191d; --fg:#e3e6eb; --muted:#9aa2ae; --rule:#2e333b; --surface:#1f2228;
  --accent:#5cc2b4; --accent-soft:#1d3532; --note-bg:#3a2c1a; --note-fg:#e8c89a;
  --canvas:#1f2125; --node:#33363c; --node-line:#121316; color-scheme: dark; } }
:root[data-theme="dark"] { --bg:#17191d; --fg:#e3e6eb; --muted:#9aa2ae; --rule:#2e333b;
  --surface:#1f2228; --accent:#5cc2b4; --accent-soft:#1d3532; --note-bg:#3a2c1a;
  --note-fg:#e8c89a; --canvas:#1f2125; --node:#33363c; --node-line:#121316; color-scheme: dark; }
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--fg); font-family:var(--font); font-size:16px;
  line-height:1.55; }
.wrap { max-width:1320px; margin:0 auto; padding:32px 16px 80px; }
header h1 { font-size:clamp(30px, 5vw, 44px); margin:0 0 8px; line-height:1.1; }
.lede { color:var(--muted); max-width:70ch; }
nav.toc { background:var(--surface); border:1px solid var(--rule); border-radius:10px;
  padding:12px 18px; margin:22px 0; columns:3 200px; }
nav.toc a { display:block; color:var(--accent); text-decoration:none; padding:2px 0; }
section { margin-top:52px; }
h2 { font-size:28px; margin:0 0 10px; display:flex; flex-wrap:wrap; gap:10px;
  align-items:baseline; }
h3 { font-size:19px; margin:26px 0 8px; }
.badge { font-size:13px; font-weight:600; background:var(--accent-soft); color:var(--accent);
  border-radius:20px; padding:2px 10px; }
p, ul, ol { max-width:78ch; }
.goal { font-size:18px; }
code, kbd { font-family:var(--mono); font-size:.88em; background:var(--accent-soft);
  padding:1px 5px; border-radius:4px; }
code.body { display:block; white-space:pre-wrap; margin:3px 0; padding:4px 8px; }
ol.steps li, ul.rules li { margin:5px 0; }
.try { background:var(--accent-soft); border-radius:8px; padding:9px 14px; }
.tpl { color:var(--muted); }
.note { background:var(--note-bg); color:var(--note-fg); border-radius:8px; padding:8px 12px;
  font-size:14px; }
.note code { overflow-wrap:anywhere; }
.shared { color:var(--muted); font-size:14px; margin:14px 0 4px; }
.missing { color:var(--note-fg); }
.scroll { overflow-x:auto; }
table { border-collapse:collapse; background:var(--surface); font-size:14px; margin:10px 0; }
table.data, table.objects { width:100%; }
/* On a phone the objects table scrolls sideways instead of squeezing its Settings column. */
table.objects { min-width:760px; }
table.objects td:last-child { min-width:260px; }
th, td { border:1px solid var(--rule); padding:5px 8px; text-align:left; vertical-align:top; }
th { background:var(--accent-soft); font-weight:600; }
td.ob { white-space:nowrap; }
td.num { white-space:nowrap; font-family:var(--mono); font-size:13px; }
.what { display:block; color:var(--muted); font-size:12.5px; white-space:normal; }
.grp { font-size:11.5px; font-weight:700; color:var(--accent); text-transform:uppercase;
  letter-spacing:.03em; }
.badge-n { display:inline-block; min-width:20px; height:20px; border-radius:10px; background:var(--mark);
  color:#000; font-size:12px; font-weight:700; text-align:center; line-height:20px; margin-right:6px; }
table.mats td:first-child { white-space:nowrap; }
.swatch { display:inline-block; width:12px; height:12px; border-radius:3px; margin-right:6px;
  vertical-align:-1px; border:1px solid var(--rule); }
figure { margin:18px 0; }
figure.frame { background:var(--surface); border:1px solid var(--rule); border-radius:10px;
  padding:12px; }
figure.frame figcaption { font-weight:600; margin-bottom:8px; }
.tree { font-size:12px; font-weight:600; color:var(--accent); background:var(--accent-soft);
  border-radius:4px; padding:1px 6px; margin-right:6px; }
.canvas { background:var(--canvas); border-radius:8px; overflow-x:auto; padding:6px; }
.canvas svg { display:block; max-width:100%; height:auto; font-family:var(--font); }
ul.how { margin:10px 0 0; padding-left:20px; }
ul.how li { margin:3px 0; }
.n-body { fill:var(--node); stroke:var(--node-line); stroke-width:1.5; }
.h-event { fill:var(--h-event); } .h-ui { fill:var(--h-ui); } .h-data { fill:var(--h-data); }
.h-object { fill:var(--h-object); } .h-flow { fill:var(--h-flow); } .h-math { fill:var(--h-math); }
.n-title { fill:#fff; font-size:13px; font-weight:600; }
.n-label { fill:var(--node-fg); font-size:12px; }
.n-dim { fill:var(--node-dim); font-size:12px; }
.n-value { fill:var(--node-value); font-size:12px; }
.n-field { fill:var(--field); }
.n-check { fill:var(--field); stroke:#777; } .n-check-on { fill:#4772b3; stroke:#9bb8e8; }
.n-panel { fill:var(--node-line); opacity:.45; } .n-section { font-weight:600; }
.sock { stroke:#111; stroke-width:1; }
.wire { fill:none; stroke-width:2; opacity:.85; }
.wire.stub { stroke-dasharray:4 3; }
.n-stub { fill:var(--node-value); font-size:12px; font-style:italic; }
figure.shot { margin:14px 0; }
figure.shot img { max-width:100%; height:auto; display:block; border-radius:10px; }
/* As wide as the picture, so the marks' places (in % of it) stay on their objects. */
.shot-box { position:relative; display:inline-block; max-width:100%; vertical-align:top; }
/* A mark is smaller on a small screen. */
.pin { position:absolute; width:clamp(15px, 2.2vw, 24px); aspect-ratio:1; border-radius:50%;
  background:var(--mark); border:2px solid #000; color:#000; font-size:13px; font-weight:800;
  display:flex; align-items:center; justify-content:center; box-sizing:border-box;
  transform:translate(-50%, -50%); }
figure.shot figcaption { color:var(--muted); font-size:14px; margin-top:6px; max-width:80ch; }
svg.diagram { display:block; width:100%; min-width:560px; max-width:760px; height:auto;
  margin:14px 0; }
.d-box { fill:var(--surface); stroke:var(--accent); stroke-width:1.5; }
.d-title { fill:var(--fg); font-size:15px; font-weight:700; text-anchor:middle; }
.d-text { fill:var(--fg); font-size:14px; text-anchor:middle; }
.d-small { fill:var(--muted); font-size:12.5px; text-anchor:middle; }
.d-line { stroke:var(--accent); stroke-width:2; fill:none; }
.d-arrowhead { fill:var(--accent); }
.d-grid rect { fill:var(--accent-soft); stroke:var(--accent); }
details { margin:10px 0; }
details summary { cursor:pointer; color:var(--accent); }
footer { margin-top:60px; color:var(--muted); font-size:13px; }
@media (max-width:640px) { nav.toc { columns:1; } h2 { font-size:24px; }
  .pin { font-size:9px; border-width:1px; } }
"""


def check_collections(recipe):
    """The words say: a collection UI in the Scene Collection, every other collection inside it,
    none excluded nor hidden (UI gets Disable in Viewports at the end). Stop when the example
    differs."""
    for entry in recipe["collections"]:
        if entry["name"] == "":
            continue
        wanted_parent = "" if entry["name"] == "UI" else "UI"
        if (entry["parent"] != wanted_parent or entry["hide_render"] or entry["hide_select"] or
                entry["hide_viewport"] != (entry["name"] == "UI")):
            raise RuntimeError(f"collection {entry['name']} is not as the words say")
    for name, state in recipe["layer_collections"].items():
        if state["exclude"] or state["hide_viewport"]:
            raise RuntimeError(f"collection {name} is excluded or hidden in the view layer")


def page(ex):
    check_collections(ex.recipe)
    content = chapters(ex)
    if [cid for cid, *_rest in content] != list(CHAPTERS):
        raise RuntimeError("the chapters are not in CHAPTERS' order")
    missing_frames = sorted(
        (tree, frame["label"]) for tree, entry in ex.recipe["trees"].items()
        for frame in entry["frames"] if (tree, frame["label"]) not in ex.shown_frames)
    missing_objects = sorted(set(ex.recipe["objects"]) - ex.shown_objects)
    missing_materials = sorted(set(ex.recipe["materials"]) - ex.shown_materials)
    if missing_frames or missing_objects or missing_materials:
        raise RuntimeError("The guide does not explain everything in the example:\n"
                           f"  frames: {missing_frames}\n  objects: {missing_objects}\n"
                           f"  materials: {missing_materials}")
    toc = '<a href="#rules">How it works</a>' + "".join(
        f'<a href="#{cid}">{index}. {esc(title)}</a>'
        for index, (cid, title, _t, _h) in enumerate(content, 1))
    sections = []
    for index, (cid, title, template, body) in enumerate(content, 1):
        badge = (f'<span class="badge">Template: Add › Game UI › Templates › {esc(template)}</span>'
                 if template else "")
        sections.append(f'<section id="{cid}"><h2>{index}. {esc(title)} {badge}</h2>{body}'
                        '</section>')
    source = bpy.data.filepath
    digest = hashlib.sha1(open(source, "rb").read()).hexdigest()[:12] if source else "?"
    # Every table scrolls sideways on a narrow screen, not the page.
    sections = [re.sub(r'(?<!<div class="scroll">)(<table\b.*?</table>)',
                       r'<div class="scroll">\1</div>', section, flags=re.S)
                for section in sections]
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Game UI Guide</title><style>{CSS}</style></head>
<body><div class="wrap">
<header><h1>Game UI Guide</h1>
<p class="lede">How the Complete UI Example is made: an inventory with equipment and crafting, a
chest, settings, a pause menu and a HUD. Open the finished game with Add › Game UI › Open
Complete UI Example. Add a part to your game with Add › Game UI › Templates. A chapter's badge
names its template.</p></header>
{ex.shot("world", "The example's world, with the HUD at the top left.", camera="MainCamera")}
<nav class="toc">{toc}</nav>
{RULES}
{"".join(sections)}
<footer>Made by make_game_ui_guide.py from game_ui_example.blend ({digest}).</footer>
</div></body></html>
"""


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    shots = args[0] if args else ""
    output = args[1] if len(args) > 1 else os.path.join(HERE, "game_ui_guide.html")
    if bpy.data.filepath:
        raise RuntimeError("start Blender with --factory-startup and no file: the guide compares "
                           "the example with a new file")
    start = start_values()
    bpy.ops.wm.open_mainfile(filepath=os.path.join(HERE, R.EXAMPLE_FILE), load_ui=False)
    example = Example(shots, start)
    text = page(example)
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
