# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Add > Game UI: ready-made pieces of object UI for the game engine.

A menu is a collection of objects in front of the scene's UI Camera, far from the world, opened
by an Open Object Menu node. These operators build the objects, their UI Element settings and the
logic nodes that make them work, so a creator only moves, recolours and wires what the game should
do.
"""

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, StringProperty
from bpy.types import Operator
from mathutils import Quaternion, Vector

# Where menus are built, away from the game world, and the new UI Camera's view of them:
# orthographic, 10.67 units wide, so 6 high at 16:9. A scene shows its menus with one camera, so a
# scene whose menus already have one keeps it and new menus go in front of it.
UI_CENTER = Vector((1000.0, 1000.0, 0.0))
UI_VIEW_WIDTH = 10.667
UI_CAMERA_NAME = "UI Camera"
# All of a scene's UI sits in one collection: the UI Camera and a child collection for each menu.
# Disable it in the viewport (the monitor icon) to edit the world without it: the game still
# starts the camera, so the menu keys work.
UI_COLLECTION_NAME = "Game UI"
UI_COLLECTION_MARK = "game_ui_collection"

MENU_MARK = "game_ui_menu"
BACKDROP_MARK = "game_ui_root"
TREE_MARK = "game_ui_tree"

# A complete small game made with these tools (inventory, equipment, crafting, a chest, settings,
# a pause menu and a HUD), installed with UPBGE in datafiles/game_ui.
EXAMPLE_FOLDER = "game_ui"
EXAMPLE_FILE = "game_ui_example.blend"
# How the example is built, step by step: a web page next to it.
GUIDE_FILE = "game_ui_guide.html"

# Curve resolution of texts that change in the game (values, grid counts): each change rebuilds
# the text's shape, about 3 times faster than at Blender's 12, and it looks the same at UI sizes.
# Titles and labels, which never change, keep 12.
CHANGING_TEXT_RESOLUTION = 3

COLORS = {
    "UI Panel": (0.035, 0.04, 0.05, 1.0),
    "UI Button": (0.16, 0.18, 0.21, 1.0),
    "UI Highlight": (0.85, 0.62, 0.18, 1.0),
    "UI Text": (0.92, 0.92, 0.92, 1.0),
    "UI Bar": (0.06, 0.06, 0.07, 1.0),
    "UI Bar Fill": (0.75, 0.12, 0.1, 1.0),
}


def _material(name):
    """A flat material: it looks the same whatever lights the menu has."""
    material = bpy.data.materials.get(name)
    if material is not None:
        return material
    material = bpy.data.materials.new(name)
    color = COLORS[name]
    material.diffuse_color = color
    material.use_nodes = True
    bsdf = material.node_tree.nodes.get("Principled BSDF")
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = color
        if "Emission Color" in bsdf.inputs:
            bsdf.inputs["Emission Color"].default_value = color
            bsdf.inputs["Emission Strength"].default_value = 1.0
    return material


def _world(ob):
    """An object's world matrix from its own transforms: matrix_world is not updated for objects
    in a collection disabled in the viewport, where menus are often kept."""
    if ob.parent is None:
        return ob.matrix_basis.copy()
    return _world(ob.parent) @ ob.matrix_parent_inverse @ ob.matrix_basis


def _link_object(ob, collection, location, parent=None):
    ob.location = location
    collection.objects.link(ob)
    ob.game.physics_type = 'NO_COLLISION'
    if parent is not None:
        # Parented where it is: its Location stays the world place it was given.
        ob.parent = parent
        ob.matrix_parent_inverse = _world(parent).inverted()
    return ob


def _plane(name, width, height, material, location, collection, parent=None, upright=False):
    mesh = bpy.data.meshes.new(name)
    w, h = width / 2.0, height / 2.0
    if upright:
        # Standing in the XZ plane, facing -Y (a bar above a character, seen from the front).
        verts = [(-w, 0.0, -h), (w, 0.0, -h), (w, 0.0, h), (-w, 0.0, h)]
    else:
        verts = [(-w, -h, 0.0), (w, -h, 0.0), (w, h, 0.0), (-w, h, 0.0)]
    mesh.from_pydata(verts, [], [(0, 1, 2, 3)])
    mesh.materials.append(_material(material))
    return _link_object(bpy.data.objects.new(name, mesh), collection, location, parent)


def _text(name, body, size, location, collection, align=('CENTER', 'CENTER'), parent=None,
          text_property=None, upright=False, changing=False):
    curve = bpy.data.curves.new(name, "FONT")
    curve.body = body
    curve.size = size
    if changing:
        curve.resolution_u = CHANGING_TEXT_RESOLUTION
    curve.align_x, curve.align_y = align
    curve.materials.append(_material("UI Text"))
    ob = _link_object(bpy.data.objects.new(name, curve), collection, location, parent)
    if upright:
        ob.rotation_euler = (1.5707964, 0.0, 0.0)
    if text_property is not None:
        _add_property(ob, "Text", 'STRING', text_property)
    return ob


def _add_property(ob, name, kind, value=None):
    with bpy.context.temp_override(object=ob, active_object=ob, selected_objects=[ob],
                                   selected_editable_objects=[ob]):
        bpy.ops.object.game_property_new(type=kind, name=name)
    prop = ob.game.properties[name]
    if value is not None:
        prop.value = value
    return prop


def _element(ob, action="", focus='ALL', pointer='BLOCK', highlight=None):
    ui = ob.game.ui
    ui.use_element = True
    ui.action = action
    ui.focus = focus
    ui.pointer = pointer
    if highlight is not None:
        ui.highlight_object = highlight


def _button(name, label, width, location, collection, action, parent=None):
    """A plate with its label and a frame shown while it is highlighted (mouse or selection)."""
    plate = _plane(name, width, 0.5, "UI Button", location, collection, parent)
    glow = _plane(name + " Glow", width + 0.12, 0.62, "UI Highlight",
                  location + Vector((0.0, 0.0, -0.01)), collection, plate)
    # Hidden in the game until highlighted (a menu's copy starts hidden, not shown for a frame).
    glow.hide_render = True
    _text(name + " Label", label, 0.24, location + Vector((0.0, 0.0, 0.02)), collection,
          parent=plate)
    _element(plate, action, highlight=glow)
    return plate


def _unique_name(base, collection):
    names = set(collection.keys())
    if base not in names:
        return base
    index = 1
    while f"{base}.{index:03d}" in names:
        index += 1
    return f"{base}.{index:03d}"


def _looks_down(camera):
    """A camera looking straight down, unturned: what the menu tools place menus for."""
    rotation = _world(camera).to_quaternion()
    return rotation.rotation_difference(Quaternion()).angle < 1e-3


def _menu_camera_in_use(scene):
    """The camera this scene's menus are already shown with (only one can show them): one of the
    scene, used by the logic of its objects, looking straight down."""
    trees = {binding.tree for ob in scene.objects for binding in ob.game.logic_node_bindings
             if binding.tree is not None}
    for tree in trees:
        if tree.bl_idname != "LogicNodeTree":
            continue
        for node in tree.nodes:
            if node.bl_idname in {"LogicNativeOpenObjectMenu", "LogicNativeSetOverlayCollection"}:
                camera = node.inputs["Camera"].default_value
                if (camera is not None and camera.type == 'CAMERA' and
                        scene in camera.users_scene and _looks_down(camera)):
                    return camera
    return None


def _scene_collections(scene):
    """The scene's collections at every level (not the Scene Collection itself)."""
    found = []
    stack = list(scene.collection.children)
    while stack:
        collection = stack.pop()
        found.append(collection)
        stack.extend(collection.children)
    return found


def _shown_collections():
    """The collections Open Object Menu and Set Overlay Collection nodes show."""
    shown = set()
    for tree in bpy.data.node_groups:
        if tree.bl_idname != "LogicNodeTree":
            continue
        for node in tree.nodes:
            if node.bl_idname in {"LogicNativeOpenObjectMenu", "LogicNativeSetOverlayCollection"}:
                collection = node.inputs["Collection"].default_value
                if collection is not None:
                    shown.add(collection)
    return shown


def _ui_collection(scene):
    """The collection a scene's UI goes in: the one Add > Game UI made (found by its mark, so it
    can be renamed), else the one the scene's menus already sit in (a creator's own "UI"), else a
    new "Game UI". Menus are its child collections, side by side: never one inside another,
    which the game cannot show."""
    collections = _scene_collections(scene)
    for collection in collections:
        if collection.get(UI_COLLECTION_MARK):
            return collection
    menus = {collection for collection in collections if collection.get(MENU_MARK)}
    menus |= _shown_collections() & set(collections)
    for collection in collections:
        if collection not in menus and any(child in menus for child in collection.children):
            return collection
    collection = bpy.data.collections.new(UI_COLLECTION_NAME)
    scene.collection.children.link(collection)
    collection[UI_COLLECTION_MARK] = True
    return collection


def _ui_space(scene):
    """The menu camera, and the middle of what it shows on the plane z = 0 (the camera looks
    straight down at it)."""
    camera = bpy.data.objects.get(UI_CAMERA_NAME)
    if camera is None or camera.type != 'CAMERA' or scene not in camera.users_scene:
        camera = _menu_camera_in_use(scene)
    if camera is None:
        data = bpy.data.cameras.new(UI_CAMERA_NAME)
        data.type = 'ORTHO'
        data.ortho_scale = UI_VIEW_WIDTH
        data.clip_start = 0.1
        data.clip_end = 100.0
        # Menus are lit by their own emission: their pass needs no shadows, which saves this
        # camera's shadow pool in GPU memory (Camera > Game Camera Settings).
        data.disable_shadows = True
        camera = bpy.data.objects.new(UI_CAMERA_NAME, data)
        camera.location = UI_CENTER + Vector((0.0, 0.0, 10.0))
        # With the UI, next to the menus (never inside one: the game would keep it asleep).
        _ui_collection(scene).objects.link(camera)
    return camera, Vector((camera.location.x, camera.location.y, 0.0))


def _menus_front(camera, z):
    """Just in front of the menus the camera already shows (towards it, as it looks down), so a
    menu opened over another one covers it; else `z`."""
    collections = set()
    for tree in bpy.data.node_groups:
        if tree.bl_idname != "LogicNodeTree":
            continue
        for node in tree.nodes:
            if (node.bl_idname == "LogicNativeOpenObjectMenu" and
                    node.inputs["Camera"].default_value in {camera, None} and
                    node.inputs["Collection"].default_value is not None):
                collections.add(node.inputs["Collection"].default_value)
    tops = [_world(ob).translation.z for collection in collections
            for ob in collection.all_objects]
    return max(z, max(tops) + 0.3) if tops else z


def _menu_of(ob):
    """The menu collection an object belongs to, else None."""
    if ob is None:
        return None
    for collection in ob.users_collection:
        if collection.get(MENU_MARK):
            return collection
    return None


def _root_of(collection):
    """A menu's panel: the parent of every piece added to it, so the whole menu moves, scales and
    hides with it."""
    root = collection.get(BACKDROP_MARK)
    return root if root is not None and collection in root.users_collection else None


def _new_tree(name, host):
    tree = bpy.data.node_groups.new(name, "LogicNodeTree")
    host.game.logic_node_binding_new().tree = tree
    return tree


def _frame(tree, title):
    """A titled frame at the origin: its nodes' locations are their places in the tree."""
    frame = tree.nodes.new("NodeFrame")
    frame.label = title[:63]
    frame.shrink = True
    return frame


def _node(tree, idname, frame, x, y, **inputs):
    node = tree.nodes.new(idname)
    node.parent = frame
    node.location = (x, y)
    for key, value in inputs.items():
        node.inputs[key].default_value = value
    return node


def _next_frame_y(tree):
    """Below every node of the tree, for the next frame (a node's place in the tree, also inside
    a frame, whose location is relative to it)."""
    bottom = 0.0
    for node in tree.nodes:
        if node.bl_idname != "NodeFrame":
            bottom = min(bottom, node.location_absolute.y - 400.0)
    return bottom


def _place(context, collection, size_x, size_y):
    """Where a new piece of a menu goes: below the selected piece (the whole piece the selection
    is part of: a grid rather than one of its cells, a button rather than its label); else at the
    top of the menu, under its title."""
    active = context.active_object
    root = collection.get(BACKDROP_MARK)
    bpy.context.view_layer.update()
    if active is not None and _menu_of(active) is collection and active != root:
        piece = active
        while (piece.parent is not None and piece.parent != root and
               _menu_of(piece.parent) is collection):
            piece = piece.parent
        top = _world(piece).translation
        height = piece.dimensions.y if piece.type in {'MESH', 'FONT'} else 0.5
        return Vector((top.x, top.y - height / 2.0 - size_y / 2.0 - 0.15, top.z))
    center = _world(root).translation if root is not None else UI_CENTER
    # Its top just under the menu's title (the title is 1.9 above the panel's middle).
    return center + Vector((0.0, 1.45 - size_y / 2.0, 0.05))


def _select_only(context, ob):
    for other in context.selected_objects:
        other.select_set(False)
    # Not in the view layer (its collection excluded): nothing to select.
    if ob.name in context.view_layer.objects:
        ob.select_set(True)
        context.view_layer.objects.active = ob


def _frame_view(context, ob):
    """Show the new menu in the 3D view: it is built away from the world."""
    if context.area is None or context.area.type != 'VIEW_3D':
        return
    region = next((r for r in context.area.regions if r.type == 'WINDOW'), None)
    try:
        with context.temp_override(area=context.area, region=region):
            bpy.ops.view3d.view_selected()
    except RuntimeError:
        pass


# The key and gamepad lists, made once: Blender needs the strings of a dynamic enum to live on.
_KEY_ITEMS = []
_PAD_ITEMS = []
# What a new menu opens with, first in the lists (a dynamic enum starts on its first item).
DEFAULT_KEY = "ESC"
DEFAULT_PAD = "START_OPTIONS"


def _key_items(_self, _context):
    if not _KEY_ITEMS:
        probe = bpy.types.GameInputBinding.bl_rna.properties["key"]
        for item in probe.enum_items:
            if item.identifier in {"NONE"} or item.identifier.startswith((
                    "NDOF", "TIMER", "ACTIONZONE", "XR_", "WINDOW_", "TEXTINPUT", "MOUSE",
                    "INBETWEEN", "TRACKPAD", "PEN", "ERASER", "WHEEL", "LEFTMOUSE", "MIDDLEMOUSE",
                    "RIGHTMOUSE", "BUTTON")):
                continue
            entry = (item.identifier, item.name, "")
            if item.identifier == DEFAULT_KEY:
                _KEY_ITEMS.insert(0, entry)
            else:
                _KEY_ITEMS.append(entry)
    return _KEY_ITEMS


def _pad_items(_self, _context):
    if not _PAD_ITEMS:
        _PAD_ITEMS.append(("NONE", "None", "No gamepad button"))
        probe = bpy.types.GameInputBinding.bl_rna.properties["gamepad_control"]
        for item in probe.enum_items:
            if "STICK" in item.identifier:
                continue
            entry = (item.identifier, item.name, "")
            if item.identifier == DEFAULT_PAD:
                _PAD_ITEMS.insert(0, entry)
            else:
                _PAD_ITEMS.append(entry)
    return _PAD_ITEMS


class OBJECT_OT_game_ui_menu_add(Operator):
    """Add a menu: a panel in front of the UI Camera with a title and a Close button, opened by a """ \
        """key or gamepad button (and closed by it, by Back or by Close)"""
    bl_idname = "object.game_ui_menu_add"
    bl_label = "Add Game UI Menu"
    bl_options = {'REGISTER', 'UNDO'}

    name: StringProperty(name="Name", default="Pause Menu",
                         description="The menu's name: its collection, title and input action")
    key: EnumProperty(name="Key", items=_key_items,
                      description="The key that opens the menu (and closes it with Toggle)")
    gamepad: EnumProperty(name="Gamepad", items=_pad_items,
                          description="The gamepad button that opens the menu")
    pause_game: BoolProperty(name="Pause Game", default=True,
                             description="The game pauses while the menu is open")
    toggle: BoolProperty(name="Toggle", default=True,
                         description="The same key or button closes the menu again")
    close_button: BoolProperty(name="Close Button", default=True,
                               description="A Close button in the top right corner")

    def invoke(self, context, _event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        scene = context.scene
        name = self.name.strip() or "Menu"
        camera, center = _ui_space(scene)
        center.z = _menus_front(camera, center.z)
        collection = bpy.data.collections.new(_unique_name(name, bpy.data.collections))
        _ui_collection(scene).children.link(collection)
        collection[MENU_MARK] = True

        # The backdrop takes every click while the menu is open (a click never reaches the game).
        backdrop = _plane(name + " Panel", 8.0, 4.8, "UI Panel", center, collection)
        _element(backdrop, "", focus='NONE')
        collection[BACKDROP_MARK] = backdrop
        # Every piece is a child of the panel: move, scale or hide the panel and the menu follows.
        _text(name + " Title", name.upper(), 0.38, center + Vector((0.0, 1.9, 0.05)),
              collection, parent=backdrop)

        tree = _new_tree(name, backdrop)
        collection[TREE_MARK] = tree
        if self.close_button:
            close_button = _button(name + " Close", "Close", 1.3,
                                   center + Vector((3.15, 1.9, 0.05)), collection, "close",
                                   parent=backdrop)
            # Last for Tab: the menu opens with its first own control selected, not Close.
            close_button.game.ui.tab_order = 100
            frame = _frame(tree, "Close button: close this menu")
            click = _node(tree, "LogicNativeOnUIEvent", frame, 0.0, 0.0, Action="close")
            click.ui_event = 'CLICK'
            close = _node(tree, "LogicNativeCloseObjectMenu", frame, 260.0, 0.0)
            close.menu_close_operation = 'TOP'
            tree.links.new(click.outputs["Out"], close.inputs["Flow"])

        # An input action (keyboard and gamepad), and the tree on the UI Camera that opens the
        # menu with it.
        # Its own action, named as its collection: a second menu of the same name gets its own key.
        actions = scene.game_settings.input_actions
        action = actions.new(name=_unique_name(collection.name, actions), type='BUTTON')
        action.bindings.new(device='KEYBOARD').key = self.key
        if self.gamepad != 'NONE':
            action.bindings.new(device='GAMEPAD').gamepad_control = self.gamepad
        opener = _new_tree("Open " + collection.name, camera)
        frame = _frame(opener, f"{collection.name}: open with the {action.name} action")
        pressed = _node(opener, "LogicNativeInputAction", frame, 0.0, 0.0, Action=action.name)
        pressed.input_type = 'TAP'
        open_menu = _node(opener, "LogicNativeOpenObjectMenu", frame, 260.0, 0.0,
                          Camera=camera, Collection=collection)
        open_menu.inputs["Input Context"].default_value = collection.name
        open_menu.use_pause_game = self.pause_game
        open_menu.use_toggle = self.toggle
        opener.links.new(pressed.outputs["Out"], open_menu.inputs["Flow"])

        _select_only(context, backdrop)
        _frame_view(context, backdrop)
        key = self.key.replace('_', ' ').title()
        message = f"Added menu '{collection.name}': press {key} in the game to open it"
        if self.key == scene.game_settings.exit_key:
            # A game action on the Exit Key takes it, as a Keyboard sensor on it does.
            message += f" ({key} no longer ends the game: add a Quit button)"
        if len(bpy.data.scenes) > 1:
            message += (f". Its key is in the Input Map of scene '{scene.name}': the game uses the "
                        "map of the scene it starts in")
        self.report({'INFO'}, message)
        return {'FINISHED'}


class GameUIPieceOperator:
    """Pieces go into the menu of the active object, as children of its panel; else above the
    selected world object, as its child; else at the 3D cursor."""
    bl_options = {'REGISTER', 'UNDO'}

    def target(self, context, size_x, size_y):
        """The collection, place and parent of a new piece, and whether it is above a world object
        (rather than in a menu)."""
        active = context.active_object
        collection = _menu_of(active)
        if collection is not None:
            return (collection, _place(context, collection, size_x, size_y), _root_of(collection),
                    False)
        # A world object is selected: above it, as its child (a bar or a name over each copy).
        if active is not None and active.select_get() and active.type != 'CAMERA':
            bpy.context.view_layer.update()
            world = _world(active)
            top = max((world @ Vector(corner)).z for corner in active.bound_box)
            location = world.translation.copy()
            location.z = top + 0.4
            return context.collection, location, active, True
        return context.collection, context.scene.cursor.location.copy(), None, False


class OBJECT_OT_game_ui_button_add(GameUIPieceOperator, Operator):
    """Add a button to the active object's menu: a click (or Accept on it) runs On UI Event """ \
        """Click with its Action, already in the menu's tree"""
    bl_idname = "object.game_ui_button_add"
    bl_label = "Add Game UI Button"

    label: StringProperty(name="Label", default="Button", description="The text on the button")
    action: StringProperty(name="Action", default="",
                           description="The Action name On UI Event reacts to (empty: the label "
                           "in lower case)")
    width: FloatProperty(name="Width", default=2.4, min=0.3, max=20.0)

    def execute(self, context):
        if _menu_of(context.active_object) is None:
            self.report({'ERROR'}, "Select a menu (or an object in it) first: Add > Game UI > Menu")
            return {'CANCELLED'}
        collection, location, parent, _in_world = self.target(context, self.width, 0.5)
        action = self.action.strip() or self.label.strip().lower().replace(" ", "_")
        name = _unique_name(self.label.strip() or "Button", bpy.data.objects)
        plate = _button(name, self.label, self.width, location, collection, action, parent)
        tree = collection.get(TREE_MARK)
        if tree is not None:
            y = _next_frame_y(tree)
            frame = _frame(tree, f"{self.label} button: what it does")
            click = _node(tree, "LogicNativeOnUIEvent", frame, 0.0, y, Action=action)
            click.ui_event = 'CLICK'
        _select_only(context, plate)
        return {'FINISHED'}


class OBJECT_OT_game_ui_bar_add(GameUIPieceOperator, Operator):
    """Add a bar that shows a number game property, such as health: in the active object's """ \
        """menu, else above the selected object (its own value), else at the 3D cursor"""
    bl_idname = "object.game_ui_bar_add"
    bl_label = "Add Game UI Bar"

    object_name: StringProperty(name="Object",
                                description="The object with the property; empty: the bar's "
                                "parent (the selected object), else the Player")
    property: StringProperty(name="Property", default="health")
    max_value: FloatProperty(name="Max", default=100.0)
    max_property: StringProperty(name="Max Property", default="",
                                 description="A property with the full value, such as max_health")
    width: FloatProperty(name="Width", default=3.0, min=0.2, max=20.0)
    smoothing: FloatProperty(name="Smoothing", default=0.25, min=0.0, max=5.0,
                             description="Seconds the fill takes to go from empty to full")

    def execute(self, context):
        collection, location, parent, in_world = self.target(context, self.width, 0.3)
        height = 0.3 if not in_world else 0.15
        width = self.width if not in_world else min(self.width, 1.2)
        bar = _plane(_unique_name(self.property.title() + " Bar", bpy.data.objects), width, height,
                     "UI Bar", location, collection, parent, upright=in_world)
        inset = height * 0.15
        fill_location = location + (Vector((0.0, -0.01, 0.0)) if in_world else
                                    Vector((0.0, 0.0, 0.01)))
        fill = _plane(bar.name + " Fill", width - 2 * inset, height - 2 * inset, "UI Bar Fill",
                      fill_location, collection, bar, upright=in_world)
        _element(bar, "", focus='NONE', pointer='IGNORE')
        ui = bar.game.ui
        ui.widget = 'BAR'
        ui.widget_property = self.property
        ui.widget_min = 0.0
        ui.widget_max = self.max_value
        ui.widget_max_property = self.max_property
        ui.widget_part = fill
        ui.look_time = self.smoothing
        owner = None
        if self.object_name:
            owner = bpy.data.objects.get(self.object_name)
        elif not in_world and _menu_of(bar) is not None:
            owner = bpy.data.objects.get("Player")
        if owner is not None and owner != bar:
            ui.widget_object = owner
        # The value the bar shows, on the object it reads, when it does not have it yet (not on a
        # menu's panel: there the bar reads the Player, or the object it is told to).
        reader = owner if owner is not None else (parent if in_world else None)
        if reader is not None and self.property and self.property not in reader.game.properties:
            _add_property(reader, self.property, 'FLOAT', self.max_value)
        if (reader is not None and self.max_property and
                self.max_property not in reader.game.properties):
            _add_property(reader, self.max_property, 'FLOAT', self.max_value)
        _select_only(context, bar)
        return {'FINISHED'}


class OBJECT_OT_game_ui_text_add(GameUIPieceOperator, Operator):
    """Add a text that shows game property values and updates by itself: {Player.gold} shows """ \
        """the Player's gold, {health} its own object's (or parent's), {Jump} the key of an action"""
    bl_idname = "object.game_ui_text_add"
    bl_label = "Add Game UI Value Text"

    text: StringProperty(name="Text", default="Gold: {Player.gold}",
                         description="Text with values in braces: {Object.property}, {property} "
                         "(of the text or its parent), :1 for one decimal ({Player.speed:1}), :0 "
                         "for none. Every change of the shown text rebuilds it: :0 keeps a value "
                         "that changes every frame cheap ({Player.stamina:0})")
    size: FloatProperty(name="Size", default=0.3, min=0.01, max=10.0)

    def execute(self, context):
        collection, location, parent, in_world = self.target(context, 0.0, self.size)
        text = self.text
        if in_world and text == "Gold: {Player.gold}":
            # Above a world object: its own name or value.
            text = "{health}"
        ob = _text(_unique_name("Value Text", bpy.data.objects), text, self.size, location,
                   collection, parent=parent, upright=in_world, changing=True)
        _select_only(context, ob)
        return {'FINISHED'}


class OBJECT_OT_game_ui_grid_add(GameUIPieceOperator, Operator):
    """Add an inventory grid to the active object's menu: cells made from one slot that """ \
        """show an object's inventory (a Table game property), with dragging between cells"""
    bl_idname = "object.game_ui_grid_add"
    bl_label = "Add Game UI Grid"

    object_name: StringProperty(name="Object", default="Player",
                                description="The object with the inventory")
    property: StringProperty(name="Property", default="inventory",
                             description="The inventory: a Table game property (item, count)")
    columns: IntProperty(name="Columns", default=4, min=1, max=32)
    rows: IntProperty(name="Rows", default=3, min=1, max=32)
    cell_size: FloatProperty(name="Cell Size", default=0.9, min=0.1, max=10.0)

    def execute(self, context):
        collection = _menu_of(context.active_object)
        if collection is None or collection.get(TREE_MARK) is None:
            self.report({'ERROR'}, "Select a menu (or an object in it) first: "
                        "Add > Game UI > Menu")
            return {'CANCELLED'}
        size = self.cell_size
        spacing = size * 1.12
        grid_width = spacing * (self.columns - 1) + size
        grid_height = spacing * (self.rows - 1) + size
        _collection, location, _parent, _in_world = self.target(context, grid_width, grid_height)
        # The grid's own viewport: the wheel over it scrolls it (not another grid of the menu). A
        # child of the menu's panel, with the cell a child of it: the cells made from it in the
        # game are its children too.
        plate = _plane(_unique_name(self.property.title() + " Grid", bpy.data.objects),
                       grid_width + 0.2, grid_height + 0.2, "UI Bar",
                       location + Vector((0.0, 0.0, -0.02)), collection, _root_of(collection))
        # The first cell's place: the grid grows right and down from it.
        start = location + Vector((-spacing * (self.columns - 1) / 2.0,
                                   spacing * (self.rows - 1) / 2.0, 0.0))
        name = _unique_name(self.property.title() + " Slot", bpy.data.objects)
        slot = _plane(name, size, size, "UI Button", start, collection, plate)
        glow = _plane(name + " Glow", size + 0.1, size + 0.1, "UI Highlight",
                      start + Vector((0.0, 0.0, -0.01)), collection, slot)
        glow.hide_render = True
        scene = context.scene
        library = scene.game_settings.item_library
        if library is not None:
            # The icon becomes the item's mesh from the Item Library; the text its count.
            icon_mesh = bpy.data.meshes.new(name + " Icon")
            s = size * 0.3
            icon_mesh.from_pydata([(-s, -s, 0.0), (s, -s, 0.0), (s, s, 0.0), (-s, s, 0.0)], [],
                                  [(0, 1, 2, 3)])
            icon_mesh.materials.append(_material("UI Text"))
            _link_object(bpy.data.objects.new(name + " Icon", icon_mesh), collection,
                         start + Vector((0.0, 0.0, 0.02)), slot)
            _text(name + " Count", "", size * 0.3, start + Vector((size * 0.42, -size * 0.42, 0.03)),
                  collection, ('RIGHT', 'BOTTOM'), slot, text_property="", changing=True)
        else:
            # No Item Library: the cell shows the item's name and count.
            _text(name + " Count", "", size * 0.17, start + Vector((0.0, 0.0, 0.03)), collection,
                  ('CENTER', 'CENTER'), slot, text_property="", changing=True)
        _element(slot, self.property, highlight=glow)
        slot.game.ui.use_draggable = True
        slot.game.ui.use_drop_target = True

        owner = bpy.data.objects.get(self.object_name)
        if owner is not None and self.property not in owner.game.properties:
            table = _add_property(owner, self.property, 'TABLE')
            index = list(owner.game.properties).index(table)
            for column_name, column_type in (("item", 'STRING'), ("count", 'INT')):
                with bpy.context.temp_override(object=owner, active_object=owner,
                                               selected_objects=[owner],
                                               selected_editable_objects=[owner]):
                    bpy.ops.object.game_property_item_add(index=index, target='COLUMN')
                table.columns[-1].name = column_name
                table.columns[-1].type = column_type

        tree = collection[TREE_MARK]
        y = _next_frame_y(tree)
        frame = _frame(tree, f"{self.property.title()} grid: {self.columns} x {self.rows} cells")
        # The grid shows the property and, with Moving Items' Handle Moves on, moves, stacks and
        # swaps what is dropped on its cells itself (by the list's Inventory Rules; also between
        # grids of other lists that handle moves).
        # Without an Object it is the list of whose menu it is.
        grid = _node(tree, "LogicNativeObjectGridView", frame, 0.0, y, Columns=self.columns,
                     Rows=self.rows, Property=self.property)
        if owner is not None:
            grid.inputs["Object"].default_value = owner
        grid.inputs["Viewport Object"].default_value = plate
        grid.inputs["Slot Template"].default_value = slot
        grid.inputs["Spacing"].default_value = (spacing, spacing, 0.0)
        grid.inputs["Show Empty"].default_value = True
        grid.inputs["Show Names"].default_value = library is None
        grid.inputs["Cull Outside"].default_value = False
        grid.inputs["Handle Moves"].default_value = True
        _select_only(context, slot)
        if library is None:
            self.report({'INFO'}, "Cells show item names; set Scene > Items and UI > Item Library "
                        "for icons")
        return {'FINISHED'}


def _example_path():
    """The installed example file, or an empty string when it is missing."""
    import os
    folder = bpy.utils.system_resource('DATAFILES', path=EXAMPLE_FOLDER)
    filepath = os.path.join(folder, EXAMPLE_FILE) if folder else ""
    return filepath if os.path.isfile(filepath) else ""


class WM_OT_game_ui_example_open(Operator):
    """Open a complete game made with object UI as a new file: an inventory with equipment and """ \
        """crafting, a chest, settings, a pause menu and a HUD, to play and take apart"""
    bl_idname = "wm.game_ui_example_open"
    bl_label = "Open Complete UI Example"

    @classmethod
    def poll(cls, _context):
        if _example_path():
            return True
        cls.poll_message_set("The UI example file is not installed with this build")
        return False

    def invoke(self, _context, _event):
        return self.open_example('INVOKE_DEFAULT')

    def execute(self, _context):
        return self.open_example('EXEC_DEFAULT')

    def open_example(self, execution_context):
        # Blender's own "new file from a start-up file": it first asks to save unsaved changes,
        # and the example opens untitled, so saving it never writes over the installed copy.
        try:
            return bpy.ops.wm.read_homefile(execution_context, filepath=_example_path())
        except RuntimeError as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}


def _guide_path():
    """The installed UI guide, or an empty string when it is missing."""
    import os
    folder = bpy.utils.system_resource('DATAFILES', path=EXAMPLE_FOLDER)
    filepath = os.path.join(folder, GUIDE_FILE) if folder else ""
    return filepath if os.path.isfile(filepath) else ""


class WM_OT_game_ui_guide_open(Operator):
    """Open the UI guide in the web browser: how the Complete UI Example is built, step by """ \
        """step, to build it (or your own) yourself"""
    bl_idname = "wm.game_ui_guide_open"
    bl_label = "Open UI Guide"

    @classmethod
    def poll(cls, _context):
        if _guide_path():
            return True
        cls.poll_message_set("The UI guide is not installed with this build")
        return False

    def execute(self, _context):
        try:
            return bpy.ops.wm.path_open(filepath=_guide_path())
        except RuntimeError as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}


# Parts of the Complete UI Example (the UI guide has a chapter on each).
TEMPLATE_ITEMS = (
    ('INVENTORY', "Inventory",
     "An inventory menu: a grid with a scrollbar and pages, a tooltip, sorting, deleting, "
     "dropping into the world, with an Equipment and a Crafting column"),
    ('CHEST', "Chest",
     "A chest next to the player and its menu: the chest's items and the player's bag side by "
     "side, dragged between"),
    ('SETTINGS', "Settings Menu",
     "A settings menu: a slider, a choice, a toggle, a text field and a key rebind, bound to the "
     "player's properties"),
    ('PAUSE', "Pause Menu",
     "A pause menu: Resume, Settings and Quit, opened (and closed) with Esc or Start"),
    ('PICKUPS', "Pickups",
     "The player picks up the items it touches into its inventory, and 8 items to pick up next to "
     "it"),
    ('WALKING', "Walking",
     "The player walks with the Move action (WASD or the left stick) at its walk_speed"),
    ('UI_SOUNDS', "UI Sounds",
     "The menus' sounds: On UI Sound plays a short tone when the highlight moves, on a click, "
     "when Back closes a menu and when a press does nothing (Play Sound, group UI)"),
)


def _player_default(context):
    """The object a template works with when the creator names none: their Player (or the one a
    template made), else the selected object (not a camera, nor a piece of a menu)."""
    from _bpy_internal.game_ui import templates
    player = templates.find_player(context.scene)
    if player is not None:
        return player.name
    active = context.active_object
    if (active is not None and active.select_get() and active.type in {'MESH', 'EMPTY'} and
            _menu_of(active) is None):
        return active.name
    return ""


class OBJECT_OT_game_ui_template_add(Operator):
    """Add a part of the Complete UI Example to this scene, working with its player, UI camera, """ \
        """Item Library and Game Input Map"""
    bl_idname = "object.game_ui_template_add"
    bl_label = "Add Game UI Template"
    bl_options = {'REGISTER', 'UNDO'}

    template: EnumProperty(name="Template", items=TEMPLATE_ITEMS)
    # Not remembered: each time the scene's own player (an old name may be gone or another
    # object's).
    player: StringProperty(
        name="Player",
        description="The object the template works with: its properties, logic tree and place. "
        "Empty: the object named Player, else a new Player at the 3D cursor",
        options={'SKIP_SAVE'})
    equipment: BoolProperty(
        name="Equipment", default=True,
        description="Weapon, armor and quick slots that take only items of their category")
    crafting: BoolProperty(
        name="Crafting", default=True,
        description="A list of recipes: click one to make it from the inventory's items")
    settings: BoolProperty(
        name="Settings Menu", default=True,
        description="A Settings button that opens the scene's Settings menu (added too when the "
        "scene has none)")

    @classmethod
    def description(cls, _context, properties):
        for identifier, _label, text in TEMPLATE_ITEMS:
            if identifier == properties.template:
                return text
        return cls.__doc__

    @classmethod
    def poll(cls, context):
        if context.mode != 'OBJECT':
            cls.poll_message_set("Templates are added in Object Mode")
            return False
        if not _example_path():
            cls.poll_message_set("The UI example file is not installed with this build")
            return False
        return True

    def invoke(self, context, _event):
        if not self.player:
            self.player = _player_default(context)
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, _context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.prop_search(self, "player", bpy.data, "objects")
        if self.template == 'INVENTORY':
            layout.prop(self, "equipment")
            layout.prop(self, "crafting")
        if self.template in {'INVENTORY', 'PAUSE'}:
            layout.prop(self, "settings")

    def execute(self, context):
        from _bpy_internal.game_ui import recipe, templates
        label = next(label for identifier, label, _text in TEMPLATE_ITEMS
                     if identifier == self.template)
        try:
            made, notes = templates.add(context, self.template, player=self.player,
                                        equipment=self.equipment, crafting=self.crafting,
                                        settings=self.settings)
        except templates.TemplateError as ex:
            # Refused before anything was added.
            self.report({'ERROR'}, f"{label}: {ex}")
            return {'CANCELLED'}
        except Exception as ex:
            # Failed part-way: an undo step all the same, so Undo removes what was added.
            if not isinstance(ex, recipe.RecipeError):
                import traceback
                traceback.print_exc()
            self.report({'ERROR'}, f"{label} failed part-way ({ex}); Undo removes what it added")
            return {'FINISHED'}
        _select_only(context, made)
        if self.template in templates.MENUS:
            _frame_view(context, made)
        message = f"Added the {label} template"
        if notes:
            message += ": " + "; ".join(notes)
        self.report({'INFO'}, message)
        return {'FINISHED'}


classes = (
    OBJECT_OT_game_ui_menu_add,
    OBJECT_OT_game_ui_button_add,
    OBJECT_OT_game_ui_bar_add,
    OBJECT_OT_game_ui_text_add,
    OBJECT_OT_game_ui_grid_add,
    OBJECT_OT_game_ui_template_add,
    WM_OT_game_ui_example_open,
    WM_OT_game_ui_guide_open,
)
