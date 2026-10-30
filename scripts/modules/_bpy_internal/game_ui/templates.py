# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Add > Game UI > Templates: parts of the Complete UI Example built into the open scene.

Every template is built from the example's recipe (recipe.py): the same objects, settings, frames
and nodes as in the example and the UI guide, connected to the creator's scene: their player (the
example's Player stands for it), their UI camera (for the example's InvCamera), their Item Library
(the example's items when the scene has none) and their Game Input Map (only missing actions are
added). Menus are fitted to what the UI camera shows.
"""

import re

import bpy
from mathutils import Matrix, Vector

from . import recipe as R

# What stands for the creator's things in the example.
PLAYER = "Player"
CAMERA = "InvCamera"
ITEM_LIBRARY = "ItemLibrary"
# The mark a template's menu collection carries: which template made it (a Settings menu is found
# by it, to open from the inventory and the pause menu).
TEMPLATE_MARK = "game_ui_template"
# The mark of every object a template makes: its name in the example.
EXAMPLE_NAME_MARK = "game_ui_example_name"
# A template's menu collection keeps its pieces by their names in the example (a part added later
# finds the pieces it goes with by it, whatever they are called now, and not a copy of one).
PIECES_MARK = "game_ui_pieces"

INVENTORY_PARTS = {
    "base": {
        "objects": ("Panel", "InventoryTitle", "Slot", "ScrollBar", "PagePrev", "PageNext",
                    "PageText", "ItemName", "DropHint", "Tooltip", "StackToggle", "StackLabel"),
        "frames": (
            "Grid: 3 x 3 copies of Slot, scrollbar beside them",
            "Pages: < and > turn the page, Page / Page Count between",
            "Delete: left click a slot (or Enter / A) removes 1 item",
            "Highlight a cell: a tooltip with name and description",
            "Sort with the Sort action, then show the top",
            "Drop: drag a cell out of the menu to drop it in the world",
            "Drop with gamepad X or right click: lands in front of you",
            "Move or equip: from the Dropped Action list to the Action list",
        ),
        "properties": ("inventory", "recipes", "sort_by", "stack_items"),
    },
    "equipment": {
        "objects": ("EquipPlate", "EquipTitle", "WeaponSlot", "WeaponSlotLabel", "ArmorSlot",
                    "ArmorSlotLabel", "QuickSlot", "QuickSlotLabel", "RefuseFrame"),
        "frames": (
            "EQUIP: each cell shows its own list; Accept Categories",
            "Drop Enter: say if the cell takes it; red frame if not",
            "Drop Exit: clear the line and the red frame",
        ),
        "properties": ("weapon", "armor", "quick"),
    },
    "crafting": {
        "objects": ("CraftPlate", "CraftTitle", "RecipeSlot", "CraftScrollBar"),
        "frames": (
            "Craft: click or A on a recipe; the grid shows what it made",
            "Highlight a recipe: show what it takes",
        ),
        "properties": ("recipes",),
    },
    "settings": {
        "objects": ("SettingsButton",),
        "frames": ("Settings button: open Settings on top of the inventory",),
        "properties": (),
    },
}

PAUSE_PARTS = {
    "base": {
        "objects": ("Pause Menu Panel", "Pause Menu Title", "Resume", "Quit"),
        "frames": ("Resume: close the pause menu (the game goes on)", "Quit: end the game"),
        "properties": (),
    },
    "settings": {
        "objects": ("Settings",),
        "frames": ("Settings: open the Settings menu on top",),
        "properties": (),
    },
}

# A whole menu of the example: its collection, tree and panel; its part(s); what opens it (a frame
# of the example's Game tree, on the Player); the input actions and player properties it needs.
MENUS = {
    'INVENTORY': {
        "collection": "InventoryMenu", "tree": "Inventory", "panel": "Panel",
        "parts": INVENTORY_PARTS,
        "opener": "Open or close the inventory with I or gamepad Select",
        "actions": ("Inventory", "Sort"),
        "items": True,
        # Offsets in world units, from a highlighted or refusing cell: fitted with the menu.
        "menu_offsets": (("Vector Math.002", "B"), ("Vector Math.003", "B")),
    },
    'CHEST': {
        "collection": "ChestMenu", "tree": "Chest", "panel": "ChestPanel",
        "parts": None,
        "opener": "Open or close the chest with Use when next to it",
        "actions": ("Use",),
        "items": True,
        "properties": ("inventory",),
        # In the world, where the example has them next to the Player.
        "world": ("Chest", "ChestLabel"),
    },
    'SETTINGS': {
        "collection": "SettingsMenu", "tree": "Settings", "panel": "SettingsPanel",
        "parts": None,
        "opener": None,
        "actions": (),
        "items": False,
        "properties": ("walk_speed", "sort_by", "show_hints", "player_name"),
        "world": ("NameTag", "Hint"),
    },
    'PAUSE': {
        "collection": "Pause Menu", "tree": "Pause Menu", "panel": "Pause Menu Panel",
        "parts": PAUSE_PARTS,
        "opener": "Pause Menu: open with the Pause Menu action",
        "actions": ("Pause Menu",),
        "items": False,
    },
}

# Logic on the player only (frames of the example's Game tree), and objects next to it.
PLAYER_TEMPLATES = {
    'PICKUPS': {
        "frames": ("Pick up: add each touched item to the inventory, then remove it",),
        "actions": (),
        "properties": ("inventory", "stack_items"),
        "items": True,
        "world": tuple(f"Pickup_{index:02d}_{item}" for index, item in enumerate(
            ("Apple", "Sword", "Gem", "Potion", "Shield", "Key", "Coin", "Mushroom"))),
    },
    'WALKING': {
        "frames": ("Walk with the Move action at the Settings speed",),
        "actions": ("Move",),
        "properties": ("walk_speed",),
        "items": False,
        "world": (),
    },
    # The menus' sounds: On UI Sound and a Play Sound for each moment. A menu template adds it too
    # when the scene plays no menu sounds yet.
    'UI_SOUNDS': {
        "frames": ("UI Sounds: the menus' Move, Accept, Back and Denied sounds",),
        "actions": (),
        "properties": (),
        "items": False,
        "world": (),
    },
}

LABELS = {
    'INVENTORY': "Inventory", 'CHEST': "Chest", 'SETTINGS': "Settings Menu",
    'PAUSE': "Pause Menu", 'PICKUPS': "Pickups", 'WALKING': "Walking", 'UI_SOUNDS': "UI Sounds",
}


def ui_sounds_tree(scene):
    """A logic tree of the scene's objects that plays the menus' sounds (an On UI Sound), else
    None: one is enough, as it hears every player's."""
    for ob in scene.objects:
        for binding in ob.game.logic_node_bindings:
            tree = binding.tree
            if tree is not None and any(node.bl_idname == "LogicNativeOnUISound"
                                        for node in tree.nodes):
                return tree
    return None

# The inventory's layout without a column (see _inventory_layout): the stack row is a Toggle and
# its label, 1.9 wide; it never comes nearer than 0.3 to the page buttons.
STACK_ROW_WIDTH = 1.9
PANEL_MARGIN = 0.4
COLUMN_GAP = 0.4


class TemplateError(Exception):
    """A template cannot be added as asked (a message for the creator). Raised only before the
    open file is changed."""


def find_player(scene, name=""):
    """The object a template works with: the one named, else the scene's Player (by name, or the
    one a template made, whatever it is called now); None when there is none yet."""
    if name:
        player = bpy.data.objects.get(name)
        if player is None:
            raise TemplateError(f"there is no object {name!r} to be the player")
        if scene not in player.users_scene:
            raise TemplateError(f"{name!r} is not in this scene")
        return player
    player = bpy.data.objects.get(PLAYER)
    if player is not None and scene in player.users_scene:
        return player
    for ob in scene.objects:
        if ob.get(EXAMPLE_NAME_MARK) == PLAYER:
            return ob
    return None


def _logic_tree(player):
    """The player's own logic tree: on, and on no other object (logic added to a tree another
    object uses would run on that object too). None when it has none."""
    for binding in player.game.logic_node_bindings:
        tree = binding.tree
        if tree is None or not binding.enabled:
            continue
        shared = any(other is not player and any(b.tree == tree
                                                 for b in other.game.logic_node_bindings)
                     for other in bpy.data.objects)
        if not shared:
            return tree
    return None


def _recipe_items(line):
    """The item names a recipe line names ("2 Wood + Stone = Axe": Wood, Stone, Axe)."""
    names = set()
    for part in re.split(r"[+=]", line):
        words = part.strip().split(" ", 1)
        if len(words) == 2 and words[0].isdigit():
            part = words[1]
        names.add(part.strip())
    return names


def _needs(template, settings_menu_too):
    """The player properties, input actions and items a template needs (with the Settings menu
    it adds when asked and the scene has none)."""
    if template in PLAYER_TEMPLATES:
        spec = PLAYER_TEMPLATES[template]
        return set(spec["properties"]), set(spec["actions"]), spec["items"]
    spec = MENUS[template]
    properties = set(spec.get("properties", ()))
    if spec["parts"] is not None:
        for part in spec["parts"].values():
            properties |= set(part["properties"])
    actions = set(spec["actions"])
    items = spec["items"]
    if settings_menu_too:
        more = _needs('SETTINGS', False)
        properties |= more[0]
        actions |= more[1]
    return properties, actions, items


def check(context, recipe, template, player_name="", settings=True):
    """Everything that would stop a template, found before anything is added (so a refused
    template leaves the file as it was)."""
    scene = context.scene
    player = find_player(scene, player_name)
    if player is not None:
        if player.library is not None or player.override_library is not None:
            raise TemplateError(f"{player.name} comes from a library: make it local first")
        if not _in_view_layer(context.view_layer, player):
            raise TemplateError(f"{player.name} is in a collection excluded from the view layer")
    if template in MENUS:
        menu = find_menu(scene, template)
        if menu is not None:
            raise TemplateError(f"this scene has the {LABELS[template]} already ({menu.name})")
    elif template == 'UI_SOUNDS':
        tree = ui_sounds_tree(scene)
        if tree is not None:
            raise TemplateError(f"this scene plays the menus' sounds already (On UI Sound in "
                                f"tree {tree.name})")
    else:
        tree = _logic_tree(player) if player is not None else None
        titles = set(PLAYER_TEMPLATES[template]["frames"])
        if tree is not None and any(node.bl_idname == "NodeFrame" and node.label in titles
                                    for node in tree.nodes):
            raise TemplateError(f"{player.name} has the {LABELS[template]} logic already "
                                f"(tree {tree.name})")
    settings_menu_too = (template in {'INVENTORY', 'PAUSE'} and settings and
                         find_menu(scene, 'SETTINGS') is None)
    properties, actions, items = _needs(template, settings_menu_too)
    entries = {entry["name"]: entry for entry in recipe["objects"][PLAYER]["properties"]}
    for name in sorted(properties):
        prop = player.game.properties.get(name) if player is not None else None
        wanted = entries[name]["type"]
        if prop is not None and prop.type != wanted:
            raise TemplateError(f"{player.name} has a {prop.type.title()} property {name!r}; the "
                                f"template needs a {wanted.title()} one: rename it first")
    kinds = {entry["settings"]["name"]: entry["settings"]["type"]
             for entry in recipe["scene"]["input_actions"]}
    for action in scene.game_settings.input_actions:
        if action.name in actions and action.type != kinds[action.name]:
            raise TemplateError(f"the Game Input Map's {action.name} action is a "
                                f"{action.type.title()} action; the template needs a "
                                f"{kinds[action.name].title()} one")
    if items and scene.game_settings.item_library is None:
        names = [name for name, entry in recipe["objects"].items()
                 if any(name in collection["objects"] for collection in recipe["collections"]
                        if collection["name"] == ITEM_LIBRARY)]
        taken = [name for name in names if name in bpy.data.objects]
        if taken:
            raise TemplateError(
                "the example's items are objects named like the items, and this file has objects "
                f"named {', '.join(taken)} already: rename them, or set your own Item Library "
                "first (Scene tab > Items and UI)")


def _in_view_layer(view_layer, ob):
    """The object is in a collection the view layer does not exclude (read from the layer
    collections, which are current even when the view layer's object list is not yet)."""
    def walk(layer):
        if layer.exclude:
            return False
        if ob.name in layer.collection.objects:
            return True
        return any(walk(child) for child in layer.children)
    return walk(view_layer.layer_collection)


def find_menu(scene, template):
    """A menu a template made before, in this scene."""
    from bl_operators import game_ui as tools
    for collection in tools._scene_collections(scene):
        if collection.get(TEMPLATE_MARK) == template:
            return collection
    return None


def _descendants(recipe, roots):
    """The recipe objects in `roots` and every object parented below one of them, in recipe
    order."""
    objects = recipe["objects"]
    wanted = set(roots)
    changed = True
    while changed:
        changed = False
        for name, entry in objects.items():
            if name not in wanted and entry["parent"] in wanted:
                wanted.add(name)
                changed = True
    missing = set(roots) - set(objects)
    if missing:
        raise R.RecipeError(f"the example has no objects {sorted(missing)}")
    return [name for name in objects if name in wanted]


def _bounds(recipe, name):
    """World bounds (min, max) of a recipe mesh object, in the example."""
    entry = recipe["objects"][name]
    world = Matrix(entry["world"])
    geometry = entry["geometry"]
    if "verts" in geometry:
        points = [world @ Vector(v) for v in geometry["verts"]]
    else:
        corners = [Vector((x, y, z)) for x in (geometry["min"][0], geometry["max"][0])
                   for y in (geometry["min"][1], geometry["max"][1])
                   for z in (geometry["min"][2], geometry["max"][2])]
        points = [world @ corner for corner in corners]
    return (Vector([min(p[axis] for p in points) for axis in range(3)]),
            Vector([max(p[axis] for p in points) for axis in range(3)]))


def _location(recipe, name):
    return Matrix(recipe["objects"][name]["world"]).translation


class Session:
    """One template added to the open scene: the creator's player, UI camera and collections, and
    what the example's names stand for there."""

    def __init__(self, context, recipe, player_name):
        from bl_operators import game_ui as tools
        self.tools = tools
        self.context = context
        self.scene = context.scene
        self.recipe = recipe
        self.notes = []
        self.player = self._player(player_name)
        self.remap = {("objects", PLAYER): self.player}
        # The menus' camera and collection, made when a template needs them (not for logic only).
        self._camera = None
        self._ui_center = None
        self._ui_collection = None

    @property
    def camera(self):
        if self._camera is None:
            self._camera, self._ui_center = self.tools._ui_space(self.scene)
            self.remap[("objects", CAMERA)] = self._camera
        return self._camera

    @property
    def ui_center(self):
        self.camera
        return self._ui_center

    @property
    def ui_collection(self):
        if self._ui_collection is None:
            self._ui_collection = self.tools._ui_collection(self.scene)
        return self._ui_collection

    # The player -------------------------------------------------------------

    def _player(self, name):
        player = find_player(self.scene, name)
        if player is not None:
            return player
        # None yet: the example's Player (a box that walks into items), at the 3D cursor; its
        # properties and logic come with the templates that use them.
        offset = self.scene.cursor.location - _location(self.recipe, PLAYER)
        body = dict(self.recipe)
        body["objects"] = dict(self.recipe["objects"])
        body["objects"][PLAYER] = dict(self.recipe["objects"][PLAYER], properties=[],
                                       bindings=[])
        builder = R.Builder(body, collection_for=lambda _name: self.scene.collection,
                            transform_for=lambda _name: Matrix.Translation(offset))
        builder.objects([PLAYER])
        player = builder.built["objects"][PLAYER]
        # Found again by the next template, even with another name (Player.001).
        _mark({PLAYER: player})
        self.notes.append(f"added a Player at the 3D cursor ({player.name})")
        return player

    def near_player(self):
        """Moves an example object next to the creator's player, where it is next to the
        example's Player."""
        return Matrix.Translation(R.world_matrix(self.player).translation -
                                  _location(self.recipe, PLAYER))

    def player_tree(self):
        """The player's own logic tree, else a new "Game" tree on it."""
        tree = _logic_tree(self.player)
        if tree is None:
            others = [binding.tree.name for binding in self.player.game.logic_node_bindings
                      if binding.tree is not None]
            tree = bpy.data.node_groups.new("Game", "LogicNodeTree")
            self.player.game.logic_node_binding_new().tree = tree
            if others:
                self.notes.append(f"added the logic to a new tree {tree.name} on "
                                  f"{self.player.name} (its tree {', '.join(others)} is off or "
                                  "also on other objects)")
        return tree

    # What the example needs ------------------------------------------------

    def ensure_items(self):
        """The scene's Item Library; the example's items when it has none."""
        game = self.scene.game_settings
        if game.item_library is not None:
            return game.item_library
        names = [name for name in self.recipe["objects"]
                 if ITEM_LIBRARY in self._homes(name)]
        collection = bpy.data.collections.new(ITEM_LIBRARY)
        self.ui_collection.children.link(collection)
        # Out of the game world (a library is copied from, never shown): as in the example.
        collection.hide_viewport = True
        builder = R.Builder(self.recipe, collection_for=lambda _name: collection)
        builder.objects(names)
        game.item_library = collection
        self.notes.append(f"added the example's {len(names)} items as the Item Library "
                          f"({collection.name})")
        return collection

    def library_items(self):
        """The item names the scene's Item Library has."""
        library = self.scene.game_settings.item_library
        return {ob.name for ob in library.all_objects} if library is not None else set()

    def for_library(self, entry, what):
        """An example object's entry whose item lists keep only the items the scene's Item
        Library has (the example's rows and recipes name the example's items)."""
        known = self.library_items()
        properties = []
        left_out = set()
        for prop in entry["properties"]:
            if prop["type"] == 'TABLE' and [c["name"] for c in prop["columns"]][:1] == ["item"]:
                rows = [row for row in prop["rows"] if row[0] in known]
                left_out |= {row[0] for row in prop["rows"]} - known
                prop = dict(prop, rows=rows)
            elif prop["type"] == 'LIST' and prop["name"] == "recipes":
                elements = [line for line in prop["elements"] if _recipe_items(line) <= known]
                for line in prop["elements"]:
                    left_out |= _recipe_items(line) - known
                prop = dict(prop, elements=elements)
            properties.append(prop)
        if left_out:
            self.notes.append(f"left {', '.join(sorted(left_out))} out of {what}: the Item "
                              "Library has no such items")
        return dict(entry, properties=properties)

    def ensure_ui_sounds(self):
        """A menu sounds like the example's: the UI Sounds frame in the player's logic, unless
        the scene plays menu sounds already."""
        if ui_sounds_tree(self.scene) is not None:
            return
        builder = R.Builder(self.recipe, remap=self.remap)
        tree = self.player_frames(PLAYER_TEMPLATES['UI_SOUNDS']["frames"], builder)
        _check('UI_SOUNDS', builder)
        self.notes.append(f"added the menus' sounds to {self.player.name}'s logic (tree "
                          f"{tree.name}, frame UI Sounds)")

    def ensure_actions(self, names):
        existing = {action.name for action in self.scene.game_settings.input_actions}
        entries = {entry["settings"]["name"]: entry
                   for entry in self.recipe["scene"]["input_actions"]}
        for name in names:
            if name not in existing:
                R.input_action(self.scene, entries[name])
                self.notes.append(f"added the {name} action to the Game Input Map")

    def ensure_properties(self, names):
        missing = [name for name in names if name not in self.player.game.properties]
        if not missing:
            return
        example = self.recipe["objects"][PLAYER]
        player = self.for_library(
            dict(example, properties=[prop for prop in example["properties"]
                                      if prop["name"] in missing]),
            f"{self.player.name}'s {', '.join(missing)}")
        entries = {entry["name"]: entry for entry in player["properties"]}
        for name in missing:
            R.add_game_property(self.player, entries[name], lambda _value: None)
        self.notes.append(f"added {', '.join(missing)} to {self.player.name}")

    # Building ---------------------------------------------------------------

    def _homes(self, name):
        return [entry["name"] for entry in self.recipe["collections"]
                if name in entry["objects"]]

    def player_frames(self, titles, builder):
        """Frames of the example's Game tree in the player's tree, below its nodes."""
        tree = self.player_tree()
        frames = [frame for frame in self.recipe["trees"]["Game"]["frames"]
                  if frame["label"] in titles]
        top = max(frame["location"][1] for frame in frames)
        left = min(frame["location"][0] for frame in frames)
        bottom = self.tools._next_frame_y(tree) if len(tree.nodes) else 0.0
        builder.tree("Game", frames=titles, into=tree, offset=(-left, bottom - top))
        return tree

    def world_objects(self, names, builder):
        """Objects that sit next to the player in the example, next to the creator's (their item
        lists with only the items the scene's Item Library has)."""
        move = self.near_player()
        builder.transform_for = lambda _name: move
        builder.collection_for = lambda _name: self.scene.collection
        overrides = {name: self.for_library(self.recipe["objects"][name], name)
                     for name in names}
        builder.objects(list(names), overrides=overrides)

    def rewrite_player_text(self, objects):
        """Text that shows a value of the example's Player ({Player.walk_speed}) shows the
        creator's player's."""
        if self.player.name == PLAYER:
            return
        for ob in objects:
            if ob.type == 'FONT' and "{" + PLAYER + "." in ob.data.body:
                ob.data.body = ob.data.body.replace("{" + PLAYER + ".", "{" + self.player.name + ".")
                if "Text" in ob.game.properties:
                    ob.game.properties["Text"].value = ob.data.body

    def find_menu(self, template):
        return find_menu(self.scene, template)

    def view_size(self, z):
        """Width and height the UI camera shows at height `z` (it looks straight down), and how
        far its Shift moves the middle of what it shows."""
        camera = self.camera
        data = camera.data
        render = self.scene.render
        width_px = max(1.0, render.resolution_x * render.pixel_aspect_x)
        height_px = max(1.0, render.resolution_y * render.pixel_aspect_y)
        if data.type == 'ORTHO':
            size = data.ortho_scale
        else:
            distance = max(0.1, R.world_matrix(camera).translation.z - z)
            sensor = data.sensor_height if data.sensor_fit == 'VERTICAL' else data.sensor_width
            size = distance * sensor / data.lens
        # The sensor (or Orthographic Scale) spans the width, the height (Vertical), or the
        # larger of the two (Auto, as Blender).
        fit = data.sensor_fit
        if fit == 'AUTO':
            fit = 'HORIZONTAL' if width_px >= height_px else 'VERTICAL'
        if fit == 'HORIZONTAL':
            width, height = size, size * height_px / width_px
        else:
            width, height = size * width_px / height_px, size
        return width, height, Vector((data.shift_x * size, data.shift_y * size, 0.0))


def _inventory_layout(recipe, equipment, crafting):
    """How the inventory changes without its Equipment or Crafting column: the columns to its
    right close the gap, the panel ends after the last column, its centred lines stay centred,
    and the stack row keeps clear of the page buttons. With both, nothing changes."""
    panel_min, panel_max = _bounds(recipe, "Panel")
    equip_min, equip_max = _bounds(recipe, "EquipPlate")
    craft_min, craft_max = _bounds(recipe, "CraftPlate")
    scroll_min, scroll_max = _bounds(recipe, "ScrollBar")
    page_min, page_max = _bounds(recipe, "PageNext")
    shifts = {}
    cursor = equip_min.x
    right = scroll_max.x
    for part, (column_min, column_max) in (("equipment", (equip_min, equip_max)),
                                           ("crafting", (craft_min, craft_max))):
        included = equipment if part == "equipment" else crafting
        if not included:
            continue
        shifts[part] = cursor - column_min.x
        right = column_max.x + shifts[part]
        cursor = right + COLUMN_GAP
    right = right + PANEL_MARGIN
    stack_left = _location(recipe, "StackToggle").x - 0.2
    right = max(right, page_max.x + 0.3 + STACK_ROW_WIDTH + PANEL_MARGIN)
    shifts["stack"] = min(0.0, right - PANEL_MARGIN - STACK_ROW_WIDTH - stack_left)
    center_x = (panel_min.x + right) / 2.0
    shifts["centred"] = center_x - _location(recipe, "DropHint").x
    return {"shifts": shifts, "right": right, "bounds": (panel_min, Vector((right, panel_max.y,
                                                                            panel_max.z)))}


def _part_of(parts, recipe, name):
    """The part an object of the menu belongs to: the part of the nearest piece above it (or of
    itself) that a part names. Every piece hangs from the panel, so "below the panel" is the whole
    menu: the nearest named piece decides."""
    roots = {root: part for part, spec in parts.items() for root in spec["objects"]}
    while name is not None:
        if name in roots:
            return roots[name]
        name = recipe["objects"][name]["parent"]
    return None


def _part_objects(parts, recipe, part):
    """The objects of one part of a menu, in recipe order."""
    roots = {root for spec in parts.values() for root in spec["objects"]}
    missing = roots - set(recipe["objects"])
    if missing:
        raise R.RecipeError(f"the example has no objects {sorted(missing)}")
    return [name for name in recipe["objects"] if _part_of(parts, recipe, name) == part]


def add(context, template, *, player="", equipment=True, crafting=True, settings=True):
    """Add a template to the context's scene. Returns what was added, for the creator. Raises
    TemplateError, with nothing changed, when it cannot be added."""
    if template not in MENUS and template not in PLAYER_TEMPLATES:
        raise TemplateError(f"unknown template {template!r}")
    try:
        recipe = R.read_example()
    except R.RecipeError as ex:
        raise TemplateError(str(ex)) from None
    check(context, recipe, template, player, settings)
    session = Session(context, recipe, player)
    if template in MENUS:
        made = _add_menu(session, template, equipment=equipment, crafting=crafting,
                         settings=settings)
    elif template in PLAYER_TEMPLATES:
        made = _add_player_logic(session, template)
    else:
        raise TemplateError(f"unknown template {template!r}")
    return made, session.notes


def _add_player_logic(session, template):
    spec = PLAYER_TEMPLATES[template]
    if spec["items"]:
        session.ensure_items()
    session.ensure_actions(spec["actions"])
    session.ensure_properties(spec["properties"])
    builder = R.Builder(session.recipe, remap=session.remap)
    if spec["world"]:
        # Only pickups of items the scene's Item Library has.
        known = session.library_items()
        item = lambda name: next(prop["value"] for prop in
                                 session.recipe["objects"][name]["properties"]
                                 if prop["name"] == "item")
        names = [name for name in spec["world"] if item(name) in known]
        if len(names) < len(spec["world"]):
            skipped = sorted({item(name) for name in spec["world"]} - known)
            session.notes.append(f"no pickups of {', '.join(skipped)}: the Item Library has no "
                                 "such items")
        if names:
            session.world_objects(names, builder)
            _mark(builder.built["objects"])
    session.player_frames(spec["frames"], builder)
    _check(template, builder)
    if template == 'PICKUPS' and session.player.game.physics_type not in {
            'DYNAMIC', 'RIGID_BODY', 'CHARACTER'}:
        session.notes.append(
            f"{session.player.name} is {session.player.game.physics_type.replace('_', ' ').title()}"
            ": it finds pickups by touching them, as the example's Dynamic Player does (Physics "
            "tab)")
    return session.player


def _add_menu(session, template, *, equipment, crafting, settings):
    recipe = session.recipe
    tools = session.tools
    spec = MENUS[template]
    parts = spec["parts"]
    chosen = {"base"}
    if template == 'INVENTORY':
        chosen |= {part for part, wanted in (("equipment", equipment), ("crafting", crafting))
                   if wanted}
    # The actions first: a Settings menu made for this menu names them in its hints.
    session.ensure_actions(spec["actions"])
    settings_menu = None
    if parts is not None and "settings" in parts and settings:
        settings_menu = session.find_menu('SETTINGS')
        if settings_menu is None:
            _add_menu(session, 'SETTINGS', equipment=True, crafting=True, settings=False)
            settings_menu = session.find_menu('SETTINGS')
        chosen.add("settings")

    # What is built, and what of the example stands for nothing.
    homes = {name: session._homes(name) for name in recipe["objects"]}
    if parts is None:
        names = [name for name in recipe["objects"] if spec["collection"] in homes[name]]
        frames = [frame["label"] for frame in recipe["trees"][spec["tree"]]["frames"]]
        properties = list(spec.get("properties", ()))
    else:
        names, frames, properties = [], [], []
        for part in parts:
            if part in chosen:
                names += _part_objects(parts, recipe, part)
                frames += parts[part]["frames"]
                properties += parts[part]["properties"]
            else:
                for name in _part_objects(parts, recipe, part):
                    session.remap[("objects", name)] = None
    order = list(recipe["objects"])
    names = sorted(set(names), key=order.index)

    if spec["items"]:
        session.ensure_items()
    session.ensure_ui_sounds()
    session.ensure_properties(dict.fromkeys(properties))

    # Where the menu goes: in front of the UI camera, in front of the menus it shows already,
    # small enough to fit what it shows.
    layout = None
    if template == 'INVENTORY':
        layout = _inventory_layout(recipe, equipment, crafting)
        low, high = layout["bounds"]
    else:
        low, high = _bounds(recipe, spec["panel"])
    if template == 'PAUSE' and "settings" not in chosen:
        quit_drop = _location(recipe, "Settings").y - _location(recipe, "Quit").y
    else:
        quit_drop = 0.0
    example_center = (low + high) / 2.0
    example_center.z = _location(recipe, spec["panel"]).z
    z = tools._menus_front(session.camera, session.ui_center.z)
    view_width, view_height, shift = session.view_size(z)
    scale = min(1.0, 0.92 * view_width / (high.x - low.x), 0.92 * view_height / (high.y - low.y))
    target = Vector((session.ui_center.x, session.ui_center.y, z)) + shift
    # Built at the example's size, then scaled by its panel (every piece hangs from it), as a
    # creator would: Grid View spacing is in the panel's units, so the grid shrinks with it.
    place = Matrix.Translation(target - example_center)

    def transform_for(name):
        shift = Vector((0.0, 0.0, 0.0))
        if layout is not None:
            part = _part_of(parts, recipe, name)
            if part in layout["shifts"]:
                shift.x += layout["shifts"][part]
            root = name
            while recipe["objects"][root]["parent"] not in {None, spec["panel"]}:
                root = recipe["objects"][root]["parent"]
            if root in {"StackToggle", "StackLabel"}:
                shift.x += layout["shifts"]["stack"]
            if root in {"DropHint", "ItemName"}:
                shift.x += layout["shifts"]["centred"]
        if quit_drop and _descendants(recipe, ["Quit"]).count(name):
            shift.y += quit_drop
        return place @ Matrix.Translation(shift)

    # Its objects in the world first (the menu's logic points at them), then the menu.
    world = {}
    if "world" in spec:
        world_builder = R.Builder(recipe, remap=dict(session.remap))
        session.world_objects([name for name in spec["world"] if name != "Hint"], world_builder)
        _check(template, world_builder)
        world.update(world_builder.built["objects"])
        if "Hint" in spec["world"]:
            world["Hint"] = _hint(session)
        _mark(world)

    collection = bpy.data.collections.new(spec["collection"])
    session.ui_collection.children.link(collection)
    collection[tools.MENU_MARK] = True
    collection[TEMPLATE_MARK] = template
    remap = dict(session.remap)
    remap.update({("objects", name): ob for name, ob in world.items()})
    if settings_menu is not None:
        remap[("collections", "SettingsMenu")] = settings_menu
    builder = R.Builder(recipe, remap=remap, collection_for=lambda _name: collection,
                        transform_for=transform_for)
    builder.built["collections"][spec["collection"]] = collection
    builder.objects(names)
    _mark(builder.built["objects"])
    collection[PIECES_MARK] = dict(builder.built["objects"])
    panel = builder.built["objects"][spec["panel"]]
    if layout is not None and abs(layout["right"] - _bounds(recipe, "Panel")[1].x) > 1e-4:
        _resize_panel(panel, recipe, layout["right"])
    if scale < 1.0:
        fit = (Matrix.Translation(target) @ Matrix.Scale(scale, 4) @
               Matrix.Translation(-target))
        for ob in builder.built["objects"].values():
            if ob.parent is None:
                ob.matrix_basis = fit @ ob.matrix_basis
    tree = builder.tree(spec["tree"], frames=frames)
    builder.bind([spec["panel"]])
    collection[tools.BACKDROP_MARK] = panel
    collection[tools.TREE_MARK] = tree
    for node_name, socket in spec.get("menu_offsets", ()):
        node = tree.nodes.get(node_name)
        if node is not None:
            node.inputs[socket].default_value = tuple(
                value * scale for value in node.inputs[socket].default_value)
    session.rewrite_player_text(builder.built["objects"].values())
    _fit_hints(session, template, builder.built["objects"], chosen)
    _check(template, builder)

    if spec["opener"] is not None:
        opener = R.Builder(recipe, remap=remap)
        opener.built["collections"][spec["collection"]] = collection
        session.player_frames([spec["opener"]], opener)
        _check(template, opener)
    if template == 'SETTINGS':
        _link_settings_buttons(session, collection)
    return panel


def _set_body(ob, body):
    ob.data.body = body
    if "Text" in ob.game.properties:
        ob.game.properties["Text"].value = body


def _fit_hints(session, template, built, chosen):
    """Hint lines say only what this menu does and name only actions this scene has (an action
    it does not have would show as written, {Use:Keyboard})."""
    actions = {action.name for action in session.scene.game_settings.input_actions}
    if template == 'INVENTORY' and "DropHint" in built:
        uses = ["move", "stack"]
        if "crafting" in chosen:
            uses.append("combine")
        if "equipment" in chosen:
            uses.append("equip")
        what = ", ".join(uses[:-1]) + " or " + uses[-1]
        body = built["DropHint"].data.body
        first, rest = body.split("\n", 1)
        _set_body(built["DropHint"],
                  first.replace("move, stack, combine or equip", what) + "\n" + rest)
    if template == 'SETTINGS' and "SettingsHint" in built:
        lines = built["SettingsHint"].data.body.split("\n")
        keys = [f"{name}: {{{name}:Keyboard}} / {{{name}:Gamepad}}"
                for name in ("Move", "Inventory", "Use") if name in actions]
        _set_body(built["SettingsHint"], "\n".join(lines[:2] + (["   ".join(keys)] if keys else [])))


def _check(template, builder):
    """Every reference of what was built stands for something of the open file (or for nothing,
    on purpose)."""
    if builder.missing:
        raise R.RecipeError(f"{LABELS[template]}: the example's {builder.missing} stand for "
                            "nothing in this scene")


def _mark(objects):
    for name, ob in objects.items():
        ob[EXAMPLE_NAME_MARK] = name


def _resize_panel(panel, recipe, right):
    """The panel's right edge at `right` (in the example's coordinates, before the fit)."""
    world = Matrix(recipe["objects"]["Panel"]["world"])
    old_right = _bounds(recipe, "Panel")[1].x
    local_old = (world.inverted() @ Vector((old_right, 0.0, 0.0))).x
    local_new = (world.inverted() @ Vector((right, 0.0, 0.0))).x
    for vertex in panel.data.vertices:
        if abs(vertex.co.x - local_old) < 1e-4:
            vertex.co.x = local_new
    panel.data.update()


def _hint(session):
    """The text the Settings menu's Show hints toggle shows and hides: below the player, as in the
    example, naming the actions this scene has."""
    actions = {action.name for action in session.scene.game_settings.input_actions}
    parts = [f"{{{name}}} {what}" for name, what in (
        ("Move", "walk"), ("Inventory", "inventory"), ("Use", "open the chest"),
        ("Pause Menu", "pause")) if name in actions]
    builder = R.Builder(session.recipe, remap=dict(session.remap))
    session.world_objects(["Hint"], builder)
    hint = builder.built["objects"]["Hint"]
    hint.data.body = "    ".join(parts) if parts else "Hints are shown"
    return hint


def _set_world(ob, matrix):
    """Place an object (parented or not) at a world matrix."""
    if ob.parent is None:
        ob.matrix_basis = matrix
    else:
        ob.matrix_basis = (R.world_matrix(ob.parent) @ ob.matrix_parent_inverse).inverted() @ matrix


def _link_settings_buttons(session, settings_menu):
    """A new Settings menu: the inventory and the pause menu made without one get their Settings
    button, as in the example, and open it."""
    recipe = session.recipe
    tools = session.tools
    for template, part in (('INVENTORY', INVENTORY_PARTS["settings"]),
                           ('PAUSE', PAUSE_PARTS["settings"])):
        menu = session.find_menu(template)
        if menu is None:
            continue
        tree = menu.get(tools.TREE_MARK)
        panel = menu.get(tools.BACKDROP_MARK)
        if tree is None or panel is None or any(
                node.bl_idname == "NodeFrame" and node.label in part["frames"]
                for node in tree.nodes):
            continue
        spec = MENUS[template]
        pieces = {name: ob for name, ob in dict(menu.get(PIECES_MARK, {})).items()
                  if ob is not None and menu in ob.users_collection}
        # Where the example's menu is in the scene: from its panel, which stays where the
        # template put it (the creator may have moved the whole menu since).
        place = R.world_matrix(panel) @ Matrix(recipe["objects"][spec["panel"]]["world"]).inverted()
        if template == 'PAUSE' and "Quit" in pieces:
            # Quit went up to the Settings button's place: back to the example's, under it.
            _set_world(pieces["Quit"], place @ Matrix(recipe["objects"]["Quit"]["world"]))
        remap = dict(session.remap)
        remap.update({("objects", name): ob for name, ob in pieces.items()})
        remap[("collections", "SettingsMenu")] = settings_menu
        builder = R.Builder(recipe, remap=remap, collection_for=lambda _name: menu,
                            transform_for=lambda _name: place)
        new_names = _part_objects(spec["parts"], recipe, "settings")
        builder.objects(new_names)
        _mark(builder.built["objects"])
        menu[PIECES_MARK] = dict(pieces, **builder.built["objects"])
        builder.tree(spec["tree"], frames=part["frames"], into=tree)
        _check(template, builder)
        # Pieces that point at the button (the grid's cell moves up to it).
        for name, ob in pieces.items():
            for key, value in recipe["objects"][name]["ui"].items():
                if isinstance(value, dict) and value.get("name") in new_names:
                    setattr(ob.game.ui, key, builder.built["objects"][value["name"]])
        session.notes.append(f"the {LABELS[template]} opens the Settings menu now")
