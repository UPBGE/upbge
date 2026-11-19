# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Every logic node's mode is found by the Add menu's search (2026-11-14).

Typing an option's name in the Add menu's search lists "Node ▸ Option", which adds the node with
that option set (scripts/startup/bl_ui/node_add_menu_logic.py: `searchable_mode_of`,
`_node_operator_with_modes`). For every logic node in the Add menu:
- a node with a dropdown of options has a searchable mode, or is listed in
  LOGIC_NODES_WITHOUT_MODE (then a new node with several dropdowns must be given its mode in
  LOGIC_NODE_SEARCHABLE_MODES);
- the search entries are drawn with the menu's label, one per option, each setting its option;
- each option set on a new node really takes;
- each search word of LOGIC_NODE_SEARCH_ALIASES ("Toggle ▸ Flip-Flop") is listed once, in the
  search only, adding the plain node.

    blender --background --factory-startup --python tests/python/logic_nodes_searchable_modes.py
"""

import sys

import bpy
import bl_ui.node_add_menu_logic as menu

PROBLEMS = []


def check(condition, text):
    if not condition:
        PROBLEMS.append(text)


class FakeSettings(list):
    def add(self):
        setting = type("Setting", (), {})()
        self.append(setting)
        return setting


class FakeOperator:
    def __init__(self, idname, label):
        self.idname = idname
        self.label = label
        self.settings = FakeSettings()
        self.use_transform = False


class FakeMenu:
    """Records the entries `_node_operator_with_modes` draws."""
    layout = None
    use_transform = True

    def __init__(self):
        self.entries = []

    def node_operator(self, layout, idname, *, label=None, search_weight=0.0, translate=True):
        operator = FakeOperator(idname, label)
        self.entries.append(operator)
        return operator


search_context = type("Context", (), {"is_menu_search": True})()
tree = bpy.data.node_groups.new("SearchableModes", "LogicNodeTree")
items = {}
for path, layout_items in menu.LOGIC_MENU_LAYOUTS.items():
    for item in layout_items:
        if item.get("kind") == "node" and item["idname"].startswith("LogicNative") and \
                menu._is_visible_node_item(item):
            items.setdefault(item["idname"], item)

searchable = 0
entries = 0
for idname, item in sorted(items.items()):
    node_type = getattr(bpy.types, idname, None)
    check(node_type is not None, "%s is in the Add menu but no node type" % idname)
    if node_type is None:
        continue
    dropdowns = menu._own_mode_dropdowns(node_type)
    mode = menu.searchable_mode_of(item)
    if idname in menu.LOGIC_NODE_SEARCHABLE_INPUT_MODES:
        # A mode that is an input (2026-11-15): one entry per option, each setting the input's
        # value through the add operator's data path.
        identifier, options = menu.LOGIC_NODE_SEARCHABLE_INPUT_MODES[idname]
        check(mode == menu.searchable_input_mode_path(identifier),
              "%s: searchable mode %r, not the input's path" % (idname, mode))
        searchable += 1
        fake = FakeMenu()
        menu._node_operator_with_modes(fake, search_context, item, mode)
        entries += len(fake.entries) - 1
        check(len(fake.entries) == len(options) + 1, "%s: %d search entries for %d options" % (
            idname, len(fake.entries) - 1, len(options)))
        name = item.get("label") or node_type.bl_rna.name
        for operator, (option_name, value) in zip(fake.entries[1:], options):
            check(operator.label == "%s ▸ %s" % (name, option_name),
                  "%s: entry %r" % (idname, operator.label))
            check(len(operator.settings) == 1 and operator.settings[0].name == mode and
                  operator.settings[0].value == repr(value),
                  "%s: entry for %s sets %s" % (
                      idname, option_name, [(s.name, s.value) for s in operator.settings]))
            # As NODE_OT_add_node's apply_node_settings does it.
            node = tree.nodes.new(idname)
            path, attribute = mode.rsplit(".", 1)
            setattr(node.path_resolve(path), attribute, eval(operator.settings[0].value))
            check(node.inputs[identifier].default_value == value, "%s: %s gives %s" % (
                idname, option_name, node.inputs[identifier].default_value))
            tree.nodes.remove(node)
        continue
    if mode is None:
        check(not dropdowns or idname in menu.LOGIC_NODES_WITHOUT_MODE,
              "%s has dropdowns %s but no searchable mode: name it in LOGIC_NODE_SEARCHABLE_MODES "
              "(or LOGIC_NODES_WITHOUT_MODE)" % (idname, [p.identifier for p in dropdowns]))
        continue
    prop = node_type.bl_rna.properties.get(mode)
    check(prop is not None and prop.type == 'ENUM', "%s: searchable mode %r is no dropdown" % (
        idname, mode))
    if prop is None or prop.type != 'ENUM':
        continue
    options = [enum_item.identifier for enum_item in prop.enum_items_static]
    check(len(options) > 1, "%s: mode %r has %d options" % (idname, mode, len(options)))
    searchable += 1

    fake = FakeMenu()
    menu._node_operator_with_modes(fake, search_context, item, mode)
    entries += len(fake.entries) - 1
    check(len(fake.entries) == len(options) + 1,
          "%s: %d search entries for %d options" % (idname, len(fake.entries) - 1, len(options)))
    name = item.get("label") or node_type.bl_rna.name
    check(fake.entries[0].label == item.get("label") and not fake.entries[0].settings,
          "%s: the plain entry lost its label or carries a setting" % idname)
    for operator, option, enum_item in zip(fake.entries[1:], options, prop.enum_items_static):
        check(operator.label == "%s ▸ %s" % (name, enum_item.name),
              "%s: entry %r, not %r" % (idname, operator.label, "%s ▸ %s" % (name, enum_item.name)))
        check(len(operator.settings) == 1 and operator.settings[0].name == mode and
              operator.settings[0].value == repr(option),
              "%s: entry for %s sets %s" % (idname, option, [(s.name, s.value) for s in operator.settings]))
        check(operator.use_transform, "%s: entry for %s does not follow the menu's transform" % (
            idname, option))

    # What the entry's setting does when the node is added.
    for option in options:
        node = tree.nodes.new(idname)
        try:
            setattr(node, mode, option)
            check(getattr(node, mode) == option, "%s: setting %s=%s gives %s" % (
                idname, mode, option, getattr(node, mode)))
        except Exception as error:  # noqa: BLE001 (report every failure)
            check(False, "%s: setting %s=%s fails: %s" % (idname, mode, option, error))
        tree.nodes.remove(node)

# The plain menu (no search) draws the node alone.
item = items["LogicNativeGetObjectAttribute"]
fake = FakeMenu()
menu._node_operator_with_modes(fake, type("Context", (), {"is_menu_search": False})(), item,
                               menu.searchable_mode_of(item))
check(len(fake.entries) == 1, "outside the search the menu shows %d entries" % len(fake.entries))


# Search words (2026-11-16): "Toggle ▸ Flip-Flop" and "Toggle ▸ Flow Gate" add the plain node, in
# the search only. Drawn through the real menu pages, so a page that forgets them fails here.
class FakeLayout:
    def separator(self):
        pass


class FakePageMenu(FakeMenu):
    layout = FakeLayout()

    def draw_menu(self, layout, path):
        pass


def page_entries(context):
    fake = FakePageMenu()
    for path, layout_items in menu.LOGIC_MENU_LAYOUTS.items():
        menu._draw_menu_items(fake, context, layout_items)
    return fake.entries


searched = page_entries(search_context)
plain = page_entries(type("Context", (), {"is_menu_search": False})())
check(menu.LOGIC_NODE_SEARCH_ALIASES.get("LogicNativeFlipFlop") == ("Toggle",) and
      menu.LOGIC_NODE_SEARCH_ALIASES.get("LogicNativeFlowGate") == ("Toggle",),
      "Flip-Flop and Flow Gate lost their search word Toggle")
aliases = 0
for idname, words in sorted(menu.LOGIC_NODE_SEARCH_ALIASES.items()):
    check(idname in items, "%s has search words but is not in the Add menu" % idname)
    if idname not in items:
        continue
    name = items[idname].get("label") or getattr(bpy.types, idname).bl_rna.name
    for word in words:
        label = "%s ▸ %s" % (word, name)
        found = [entry for entry in searched if entry.label == label]
        check(len(found) == 1, "the search lists %r %d times" % (label, len(found)))
        for entry in found:
            aliases += 1
            check(entry.idname == idname and not entry.settings,
                  "%r adds %s with settings %s" % (label, entry.idname, entry.settings))
            check(entry.use_transform, "%r does not follow the menu's transform" % label)
        check(not any(entry.label == label for entry in plain),
              "%r shows outside the search" % label)
check(len(searched) == len(plain) + entries + aliases,
      "the search draws %d entries, the menu %d, the modes %d and the search words %d" % (
          len(searched), len(plain), entries, aliases))

print("%d logic nodes in the Add menu, %d with a searchable mode, %d search entries, "
      "%d search words" % (len(items), searchable, entries, aliases))
print("PROBLEMS (%d):" % len(PROBLEMS))
for problem in PROBLEMS:
    print("  " + problem)
sys.exit(1 if PROBLEMS else 0)
