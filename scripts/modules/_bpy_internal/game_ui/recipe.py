# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Complete UI Example as a recipe: everything that makes it work, read from the example file
(`extract`), and built again in the open file, all of it or in parts (`Builder`).

The recipe keeps only what differs from what Blender gives a new object, node or setting, so the
UI guide can show exactly what to set, and nothing is left out: a file built from only what the
guide shows is the same example (tests/python/game_ui_guide_rebuild_test.py checks it). What the
recipe cannot write down (a material linked to the object, a nested frame, a mesh with loose
edges...) stops `extract` with a RecipeError rather than being left out.

Values are plain data (dict, list, str, number, bool). A data-block is `{"id": "objects",
"name": "Player"}`: the `bpy.data` collection it lives in and its name.
"""

import os

import bpy
from mathutils import Matrix

EXAMPLE_FOLDER = "game_ui"
EXAMPLE_FILE = "game_ui_example.blend"

# What a new object, node or setting has anyway, or what is not part of the example's design.
NODE_SKIP = {
    "name", "label", "location", "location_absolute", "width", "height", "dimensions", "parent",
    "select", "show_options", "show_preview", "hide", "mute", "show_texture", "color",
    "use_custom_color", "warning_propagation", "is_active_output", "color_tag",
}
OBJECT_KEEP = (
    "hide_render", "hide_viewport", "hide_select", "display_type", "show_name", "show_in_front",
    "color", "rotation_mode", "empty_display_type", "empty_display_size",
    # Object tab > Visibility > Ray Visibility (a name above a player casts no shadow).
    "visible_camera", "visible_diffuse", "visible_glossy", "visible_transmission",
    "visible_volume_scatter", "visible_shadow", "is_holdout", "is_shadow_catcher",
)
GAME_SKIP = {"ui", "custom_object"}
CAMERA_DERIVED = {"angle", "angle_x", "angle_y"}
SCENE_GAME_SKIP = {"active_input_action_index", "active_inventory_rules_index"}
TEXT_SKIP = {"body_format", "edit_format", "is_editmode", "texspace_location", "texspace_size",
             "use_auto_texspace"}
# A mesh with no more points than a box is kept as its points (planes, boxes, small markers);
# others (the items' shapes) are taken from the example file by name.
SMALL_MESH = 8

_ID_COLLECTIONS = (
    (bpy.types.Object, "objects"),
    (bpy.types.Collection, "collections"),
    (bpy.types.Material, "materials"),
    (bpy.types.Sound, "sounds"),
    (bpy.types.NodeTree, "node_groups"),
    (bpy.types.Mesh, "meshes"),
    (bpy.types.Text, "texts"),
    (bpy.types.Image, "images"),
    (bpy.types.VectorFont, "fonts"),
    (bpy.types.World, "worlds"),
    (bpy.types.Camera, "cameras"),
    (bpy.types.Light, "lights"),
)


class RecipeError(Exception):
    """The example uses something the recipe cannot write down."""


def example_path():
    """The example file installed with UPBGE, or an empty string when it is missing."""
    folder = bpy.utils.system_resource('DATAFILES', path=EXAMPLE_FOLDER)
    filepath = os.path.join(folder, EXAMPLE_FILE) if folder else ""
    return filepath if os.path.isfile(filepath) else ""


# -----------------------------------------------------------------------------
# Values


def _id_kind(value):
    for cls, kind in _ID_COLLECTIONS:
        if isinstance(value, cls):
            return kind
    raise RecipeError(f"the example refers to a {type(value).__name__} ({value.name!r})")


def encode(value):
    """A property value as plain data."""
    if value is None:
        return None
    if isinstance(value, bpy.types.ID):
        return {"id": _id_kind(value), "name": value.name}
    if isinstance(value, float):
        return round(value, 6)
    if isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, (set, frozenset)):
        return sorted(value)
    try:
        return [encode(item) for item in value]
    except TypeError:
        raise RecipeError(f"cannot write down the value {value!r}") from None


def _equal(a, b):
    if isinstance(a, float) or isinstance(b, float):
        return isinstance(a, (int, float)) and isinstance(b, (int, float)) and abs(a - b) <= 1e-5
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(_equal(x, y) for x, y in zip(a, b))
    return a == b


def _settings(struct, skip=(), prefix=""):
    """Every setting of a struct (nested settings as "outer.inner"), not its lists. Of a
    data-block, not what every data-block has (its name, preview, where it was read from)."""
    if isinstance(struct, bpy.types.ID):
        skip = set(skip) | {prop.identifier for prop in bpy.types.ID.bl_rna.properties}
    values = {}
    for prop in struct.bl_rna.properties:
        name = prop.identifier
        if name == "rna_type" or name in skip or prop.type == 'COLLECTION':
            continue
        if prop.type == 'POINTER':
            value = getattr(struct, name)
            if value is None or isinstance(value, bpy.types.ID):
                if not prop.is_readonly:
                    values[prefix + name] = encode(value)
            elif not isinstance(value, bpy.types.ID):
                values.update(_settings(value, skip=(), prefix=prefix + name + "."))
            continue
        if prop.is_readonly:
            continue
        values[prefix + name] = encode(getattr(struct, name))
    return values


def diff(struct, fresh, skip=()):
    """The settings of `struct` that are not as in `fresh` (a new one of the same kind)."""
    mine = _settings(struct, skip)
    theirs = _settings(fresh, skip)
    return {name: value for name, value in mine.items()
            if name not in theirs or not _equal(value, theirs[name])}


def _matrix(matrix):
    return [[round(value, 6) for value in row] for row in matrix]


def world_matrix(ob):
    """An object's world matrix from its own transforms (matrix_world is not kept up to date for
    objects in a collection disabled in the viewport, where the example's menus are)."""
    if ob.parent is None:
        return ob.matrix_basis.copy()
    return world_matrix(ob.parent) @ ob.matrix_parent_inverse @ ob.matrix_basis


# -----------------------------------------------------------------------------
# Reading the example


class _Fresh:
    """New objects, data, nodes and settings to compare with: what Blender gives anyway."""

    def __init__(self, data):
        self.data = data
        self._objects = {}
        self._nodes = {}
        self._tree = None

    def object(self, kind):
        if kind not in self._objects:
            data = self.data
            if kind == 'MESH':
                ob = data.objects.new("__fresh_mesh", data.meshes.new("__fresh_mesh"))
            elif kind == 'FONT':
                ob = data.objects.new("__fresh_font", data.curves.new("__fresh_font", 'FONT'))
            elif kind == 'CAMERA':
                ob = data.objects.new("__fresh_camera", data.cameras.new("__fresh_camera"))
            elif kind.startswith('LIGHT'):
                light_type = kind.split(":", 1)[1]
                ob = data.objects.new("__fresh_light", data.lights.new("__fresh_light", light_type))
            else:
                ob = data.objects.new("__fresh_empty", None)
            self._objects[kind] = ob
        return self._objects[kind]

    def node(self, idname, props):
        """A new node of a kind, with these settings (they decide which inputs it has)."""
        if self._tree is None:
            self._tree = self.data.node_groups.new("__fresh_nodes", "LogicNodeTree")
        node = self._tree.nodes.new(idname)
        apply_settings(node, props, lambda value: None)
        return node

    def frame(self):
        key = ("NodeFrame", ())
        if key not in self._nodes:
            if self._tree is None:
                self._tree = self.data.node_groups.new("__fresh_nodes", "LogicNodeTree")
            self._nodes[key] = self._tree.nodes.new("NodeFrame")
        return self._nodes[key]


def _geometry(ob):
    if ob.type != 'MESH':
        return None
    mesh = ob.data
    if len(mesh.vertices) <= SMALL_MESH:
        # Its points and faces are all it keeps: nothing else of it may matter.
        face_edges = {key for polygon in mesh.polygons for key in polygon.edge_keys}
        if any(edge.key not in face_edges for edge in mesh.edges):
            raise RecipeError(f"{ob.name}: a small mesh with loose edges cannot be written down")
        if len(ob.material_slots) > 1 and any(polygon.material_index for polygon in
                                              mesh.polygons):
            raise RecipeError(f"{ob.name}: faces with different materials cannot be written down")
        return {
            "verts": [encode(tuple(vertex.co)) for vertex in mesh.vertices],
            "faces": [list(polygon.vertices) for polygon in mesh.polygons],
        }
    corners = [vertex.co for vertex in mesh.vertices]
    return {
        "mesh": mesh.name,
        "min": [round(min(co[axis] for co in corners), 6) for axis in range(3)],
        "max": [round(max(co[axis] for co in corners), 6) for axis in range(3)],
    }


def _material(material, fresh_material):
    if material.node_tree is not None:
        kinds = sorted(node.type for node in material.node_tree.nodes)
        if kinds != ['BSDF_PRINCIPLED', 'OUTPUT_MATERIAL']:
            raise RecipeError(f"material {material.name!r} has nodes {kinds}: only a Principled "
                              "BSDF can be written down")
    entry = {"diffuse_color": encode(tuple(material.diffuse_color)), "bsdf": {}}
    if material.node_tree is not None:
        bsdf = next(node for node in material.node_tree.nodes if node.type == 'BSDF_PRINCIPLED')
        fresh = next(node for node in fresh_material.node_tree.nodes
                     if node.type == 'BSDF_PRINCIPLED')
        fresh_inputs = {socket.identifier: socket for socket in fresh.inputs}
        for socket in bsdf.inputs:
            if not hasattr(socket, "default_value") or socket.is_linked:
                continue
            value = encode(socket.default_value)
            other = fresh_inputs.get(socket.identifier)
            if other is None or not _equal(value, encode(other.default_value)):
                entry["bsdf"][socket.identifier] = value
    return entry


def _game_property(prop):
    entry = {"name": prop.name, "type": prop.type, "save_game": prop.use_save_game,
             "debug": prop.show_debug}
    if prop.type == 'CHOICE':
        entry["options"] = [option.name for option in prop.options]
        entry["value"] = prop.value
    elif prop.type == 'LIST':
        entry["item_type"] = prop.item_type
        entry["elements"] = [encode(element.value) for element in prop.elements]
    elif prop.type == 'TABLE':
        entry["columns"] = [{"name": column.name, "type": column.type,
                             "value": encode(column.value)} for column in prop.columns]
        entry["rows"] = [[encode(cell.value) for cell in row.cells] for row in prop.rows]
    else:
        entry["value"] = encode(prop.value)
    # The scene's Inventory Rules the list follows (the sets themselves are not carried).
    if prop.type in {'LIST', 'TABLE'} and prop.inventory_rules:
        entry["inventory_rules"] = prop.inventory_rules
    return entry


def _object(ob, fresh):
    kind = ob.type if ob.type != 'LIGHT' else "LIGHT:" + ob.data.type
    new = fresh.object(kind)
    if ob.game.sensors or ob.game.controllers or ob.game.actuators or ob.game.components:
        raise RecipeError(f"{ob.name} has logic bricks or Python components")
    if any(slot.link != 'DATA' for slot in ob.material_slots):
        raise RecipeError(f"{ob.name} has a material linked to the object, not to its data")
    if ob.parent is not None and ob.parent_type != 'OBJECT':
        raise RecipeError(f"{ob.name} is parented to a {ob.parent_type.lower()}, not an object")
    if ob.constraints:
        raise RecipeError(f"{ob.name} has constraints")
    entry = {
        "type": ob.type,
        "parent": ob.parent.name if ob.parent else None,
        "world": _matrix(world_matrix(ob)),
        "settings": {name: encode(getattr(ob, name)) for name in OBJECT_KEEP
                     if hasattr(ob, name) and
                     not _equal(encode(getattr(ob, name)), encode(getattr(new, name)))},
        "materials": [slot.material.name if slot.material else None
                      for slot in ob.material_slots],
        "game": diff(ob.game, new.game, skip=GAME_SKIP),
        "ui": diff(ob.game.ui, new.game.ui),
        "properties": [_game_property(prop) for prop in ob.game.properties],
        "bindings": [{"tree": binding.tree.name if binding.tree else binding.tree_name,
                      "enabled": binding.enabled}
                     for binding in ob.game.logic_node_bindings],
    }
    geometry = _geometry(ob)
    if geometry is not None:
        entry["geometry"] = geometry
    if ob.type == 'FONT':
        entry["text"] = diff(ob.data, new.data, skip=TEXT_SKIP)
        entry["text"]["body"] = ob.data.body
    elif ob.type in {'CAMERA', 'LIGHT'}:
        # A camera's angles follow from its lens and sensor: set, they would undo the sensor.
        entry["data"] = diff(ob.data, new.data, skip=CAMERA_DERIVED)
        if ob.type == 'LIGHT':
            # Compared with a new light of its own type: the type itself is never a difference.
            entry["light_type"] = ob.data.type
    elif ob.type not in {'MESH', 'EMPTY'}:
        raise RecipeError(f"{ob.name} is a {ob.type} object")
    for part in ("settings", "game", "ui", "text", "data"):
        _only_ids(f"{ob.name} {part}", entry.get(part, {}), BUILT_KINDS)
    return entry


# The data-blocks a Builder makes or finds (materials by an object's slots): a reference to
# anything else (a font, an image) could not be built.
BUILT_KINDS = {"objects", "collections", "node_groups"}
# A node may also play a packed sound of the example (Play Sound), which a tree loads first.
NODE_KINDS = BUILT_KINDS | {"sounds"}


def _only_ids(where, values, kinds):
    for key, value in values.items():
        if isinstance(value, dict) and "id" in value and value["id"] not in kinds:
            raise RecipeError(f"{where}: {key} refers to {value['id']} {value['name']!r}, which "
                              "the recipe cannot build")


def _encode_mark(value):
    """A custom property as plain data (a group's data-blocks too: the file read is let go)."""
    if hasattr(value, "to_dict"):
        value = value.to_dict()
    if isinstance(value, dict):
        return {key: _encode_mark(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_encode_mark(item) for item in value]
    return encode(value)


def _collection_entry(collection, parent_name):
    marks = {key: _encode_mark(collection[key]) for key in collection.keys()}
    return {
        "name": collection.name,
        "parent": parent_name,
        "hide_viewport": collection.hide_viewport,
        "hide_render": collection.hide_render,
        "hide_select": collection.hide_select,
        "marks": marks,
        "objects": [ob.name for ob in collection.objects],
    }


def _layer_states(layer_collection, out):
    for child in layer_collection.children:
        out[child.name] = {"exclude": child.exclude, "hide_viewport": child.hide_viewport}
        _layer_states(child, out)
    return out


def _tree(tree, fresh):
    frames = []
    nodes = []
    for node in tree.nodes:
        if node.bl_idname == "NodeFrame":
            if node.parent is not None:
                raise RecipeError(f"{tree.name}: frame {node.label or node.name!r} is inside "
                                  "another frame")
            frames.append({
                "name": node.name,
                "label": node.label,
                "location": encode(tuple(node.location)),
                "settings": diff(node, fresh.frame(), skip=NODE_SKIP),
            })
            continue
        props = diff(node, fresh.node(node.bl_idname, {}), skip=NODE_SKIP)
        new = fresh.node(node.bl_idname, props)
        for side in ("inputs", "outputs"):
            mine = {socket.identifier for socket in getattr(node, side)}
            theirs = {socket.identifier for socket in getattr(new, side)}
            if mine - theirs:
                raise RecipeError(f"{tree.name}: node {node.name!r} has {side} a new node of its "
                                  f"kind does not get: {sorted(mine - theirs)}")
        new_inputs = {socket.identifier: socket for socket in new.inputs}
        inputs = {}
        for socket in node.inputs:
            if not hasattr(socket, "default_value"):
                continue
            value = encode(socket.default_value)
            if not _equal(value, encode(new_inputs[socket.identifier].default_value)):
                inputs[socket.identifier] = value
        _only_ids(f"{tree.name}: node {node.name!r}", dict(props, **inputs), NODE_KINDS)
        nodes.append({
            "name": node.name,
            "idname": node.bl_idname,
            "label": node.label,
            "frame": node.parent.name if node.parent else None,
            "location": encode(tuple(node.location)),
            "width": round(node.width, 3),
            "hide": node.hide,
            "mute": node.mute,
            "settings": props,
            "inputs": inputs,
        })
    links = [{
        "from_node": link.from_node.name,
        "from_socket": link.from_socket.identifier,
        "to_node": link.to_node.name,
        "to_socket": link.to_socket.identifier,
    } for link in tree.links]
    return {"frames": frames, "nodes": nodes, "links": links}


def extract(data, scene):
    """The recipe of `scene` in `data` (bpy.data or a temporary BlendData)."""
    # The new things to compare with live in their own temporary data, dropped at the end (the
    # data read may be the open file's, which must not change).
    with bpy.data.temp_data() as fresh_data:
        return _extract(data, scene, fresh_data)


def _extract(data, scene, fresh_data):
    fresh = _Fresh(fresh_data)
    fresh_scene = fresh_data.scenes.new("Fresh")
    fresh_material = fresh_data.materials.new("Fresh")
    if fresh_material.node_tree is None:
        fresh_material.use_nodes = True
    fresh_world = fresh_data.worlds.new("Fresh")
    recipe = {"objects": {}, "collections": [], "trees": {}, "materials": {}}
    for ob in scene.objects:
        recipe["objects"][ob.name] = _object(ob, fresh)
    # Collections, parents first, with their objects in order.
    recipe["collections"].append(_collection_entry(scene.collection, None))
    recipe["collections"][0]["name"] = ""
    stack = [(child, "") for child in scene.collection.children]
    while stack:
        collection, parent = stack.pop(0)
        recipe["collections"].append(_collection_entry(collection, parent))
        stack.extend((child, collection.name) for child in collection.children)
    recipe["layer_collections"] = _layer_states(scene.view_layers[0].layer_collection, {})
    used_materials = {name for ob in recipe["objects"].values() for name in ob["materials"]
                      if name}
    for name in sorted(used_materials):
        recipe["materials"][name] = _material(data.materials[name], fresh_material)
    for tree in data.node_groups:
        if tree.bl_idname == "LogicNodeTree":
            recipe["trees"][tree.name] = _tree(tree, fresh)
    game = scene.game_settings
    recipe["scene"] = {
        "name": scene.name,
        "camera": scene.camera.name if scene.camera else None,
        "render": {name: encode(getattr(scene.render, name))
                   for name in ("resolution_x", "resolution_y", "resolution_percentage",
                                "engine")},
        "world": diff(scene.world, fresh_world) if scene.world else None,
        "game": diff(game, fresh_scene.game_settings, skip=SCENE_GAME_SKIP),
        "input_actions": [{
            "settings": _settings(action, skip={"active_binding_index"}),
            "bindings": [_settings(binding) for binding in action.bindings],
        } for action in game.input_actions],
    }
    return recipe


def read_example(filepath=None):
    """The recipe of the installed example, read without touching the open file."""
    filepath = filepath or example_path()
    if not filepath:
        raise RecipeError("the UI example file is not installed with this build")
    if bpy.data.filepath and os.path.samefile(bpy.data.filepath, filepath):
        raise RecipeError("the example file itself is open: add templates to another file")
    with bpy.data.temp_data() as data:
        with data.libraries.load(filepath) as (source, target):
            target.scenes = list(source.scenes)
        if len(data.scenes) != 1:
            raise RecipeError(f"the example has {len(data.scenes)} scenes, not one")
        recipe = extract(data, data.scenes[0])
    recipe["source"] = filepath
    return recipe


# -----------------------------------------------------------------------------
# Building it again


def apply_settings(struct, values, resolve):
    """Set recipe values on a struct, in the order the struct defines them (some settings
    decide what others mean, such as an object's physics type before its ghost option)."""
    pending = dict(values)
    for prop in struct.bl_rna.properties:
        name = prop.identifier
        nested = {key[len(name) + 1:]: value for key, value in pending.items()
                  if key.startswith(name + ".")}
        if nested:
            apply_settings(getattr(struct, name), nested, resolve)
            for key in nested:
                del pending[name + "." + key]
        if name not in pending:
            continue
        value = pending.pop(name)
        if prop.type == 'POINTER':
            value = resolve(value)
        elif prop.type == 'ENUM' and prop.is_enum_flag:
            value = set(value)
        elif isinstance(value, list):
            value = tuple(value)
        setattr(struct, name, value)
    if pending:
        raise RecipeError(f"{struct.bl_rna.identifier} has no settings {sorted(pending)}")


def _with_object(ob, function):
    with bpy.context.temp_override(object=ob, active_object=ob, selected_objects=[ob],
                                   selected_editable_objects=[ob]):
        return function()


def add_game_property(ob, entry, resolve):
    """A game property as the recipe writes it, with its options, items, columns and rows."""
    if entry["name"] in ob.game.properties:
        raise RecipeError(f"{ob.name} has a property {entry['name']!r} already")
    # The property operators do not edit an object disabled in viewports: enabled meanwhile.
    hidden = ob.hide_viewport
    ob.hide_viewport = False
    try:
        return _add_game_property(ob, entry, resolve)
    finally:
        ob.hide_viewport = hidden


def _cell_value(value, resolve):
    """A list item, cell or column default as the property takes it (an object, a vector)."""
    if isinstance(value, dict) and "id" in value:
        return resolve(value)
    return tuple(value) if isinstance(value, list) else value


def _add_game_property(ob, entry, resolve):
    _with_object(ob, lambda: bpy.ops.object.game_property_new(type=entry["type"],
                                                               name=entry["name"]))
    prop = ob.game.properties[entry["name"]]
    index = list(ob.game.properties).index(prop)
    prop.use_save_game = entry["save_game"]
    prop.show_debug = entry["debug"]

    def add_item(target):
        _with_object(ob, lambda: bpy.ops.object.game_property_item_add(index=index,
                                                                        target=target))

    if entry["type"] == 'CHOICE':
        # A new Choice starts with the options A and B: they become the recipe's (renamed out of
        # the way first, as option names are kept unique).
        options = entry["options"]
        for option_index, option in enumerate(prop.options):
            option.name = f"__option_{option_index}"
        for option_index, option in enumerate(options):
            if option_index >= len(prop.options):
                add_item('ITEM')
            prop.options[option_index].name = option
        while len(prop.options) > len(options):
            prop.active_option_index = len(prop.options) - 1
            _with_object(ob, lambda: bpy.ops.object.game_property_item_remove(index=index,
                                                                               target='ITEM'))
        prop.value = entry["value"]
    elif entry["type"] == 'LIST':
        prop.item_type = entry["item_type"]
        for element in entry["elements"]:
            add_item('ITEM')
            prop.elements[-1].value = _cell_value(element, resolve)
    elif entry["type"] == 'TABLE':
        for column in entry["columns"]:
            add_item('COLUMN')
            prop.columns[-1].name = column["name"]
            prop.columns[-1].type = column["type"]
            if column["value"] is not None:
                prop.columns[-1].value = _cell_value(column["value"], resolve)
        for row in entry["rows"]:
            add_item('ITEM')
            for cell, value in zip(prop.rows[-1].cells, row):
                cell.value = _cell_value(value, resolve)
    elif entry["type"] == 'OBJECT':
        prop.value = resolve(entry["value"])
    else:
        value = entry["value"]
        prop.value = tuple(value) if isinstance(value, list) else value
    if entry["type"] in {'LIST', 'TABLE'}:
        prop.inventory_rules = entry.get("inventory_rules", "")
    return prop


def input_action(scene, entry, replace=False):
    """An input action of the recipe in the scene's Game Input Map: added when the scene has no
    action of that name; else, with `replace`, given the recipe's bindings (the UI controls, which
    every scene has, cannot be removed), and left as the creator made it without."""
    actions = scene.game_settings.input_actions
    settings = dict(entry["settings"])
    name = settings.pop("name")
    kind = settings.pop("type")
    action = next((action for action in actions if action.name == name), None)
    if action is not None and not replace:
        return action
    if action is None:
        action = actions.new(name=name, type=kind)
    else:
        while len(action.bindings):
            action.bindings.remove(action.bindings[0])
        if action.type != kind:
            action.type = kind
    apply_settings(action, settings, lambda value: None)
    for binding_entry in entry["bindings"]:
        binding_settings = dict(binding_entry)
        binding = action.bindings.new(device=binding_settings.pop("device"))
        apply_settings(binding, binding_settings, lambda value: None)
    return action


class Builder:
    """Builds recipe objects, trees and settings in the open file.

    `remap` gives the example's names of data-blocks that are not built, but stand for one of the
    open file (the example's Player is the creator's player) or for nothing (None). A reference to
    anything else is written to `missing`, and left empty: the templates' tests make sure there is
    none.
    """

    def __init__(self, recipe, *, remap=None, collection_for=None, transform_for=None):
        self.recipe = recipe
        self.remap = dict(remap or {})
        self.collection_for = collection_for or (lambda name: None)
        self.transform_for = transform_for or (lambda name: Matrix.Identity(4))
        self.built = {"objects": {}, "node_groups": {}, "collections": {}, "materials": {},
                      "sounds": {}, "meshes": {}}
        self.missing = []
        self._new_inputs = None

    # References -----------------------------------------------------------

    def resolve(self, value):
        if value is None:
            return None
        if not isinstance(value, dict) or "id" not in value:
            raise RecipeError(f"not a data-block reference: {value!r}")
        kind, name = value["id"], value["name"]
        built = self.built.get(kind, {})
        if name in built:
            return built[name]
        if (kind, name) in self.remap:
            return self.remap[(kind, name)]
        if name in self.remap and kind == "objects":
            return self.remap[name]
        self.missing.append((kind, name))
        return None

    # Materials, meshes and sounds -----------------------------------------

    def material(self, name):
        if name is None:
            return None
        if name in self.built["materials"]:
            return self.built["materials"][name]
        entry = self.recipe["materials"][name]
        existing = bpy.data.materials.get(name)
        if existing is not None and self._same_material(existing, entry):
            material = existing
        else:
            material = bpy.data.materials.new(name)
            if material.node_tree is None:
                material.use_nodes = True
            material.diffuse_color = entry["diffuse_color"]
            bsdf = next(node for node in material.node_tree.nodes
                        if node.type == 'BSDF_PRINCIPLED')
            inputs = {socket.identifier: socket for socket in bsdf.inputs}
            for identifier, value in entry["bsdf"].items():
                inputs[identifier].default_value = (tuple(value) if isinstance(value, list)
                                                    else value)
        self.built["materials"][name] = material
        return material

    def _same_material(self, material, entry):
        """A material of the open file with the recipe's name and look (used, not copied): every
        input as the recipe's, or as a new material's where the recipe gives none."""
        if not _equal(encode(tuple(material.diffuse_color)), entry["diffuse_color"]):
            return False
        if material.node_tree is None:
            return False
        kinds = sorted(node.type for node in material.node_tree.nodes)
        if kinds != ['BSDF_PRINCIPLED', 'OUTPUT_MATERIAL']:
            return False
        bsdf = next(node for node in material.node_tree.nodes if node.type == 'BSDF_PRINCIPLED')
        if any(socket.is_linked for socket in bsdf.inputs):
            return False
        mine = {socket.identifier: encode(socket.default_value) for socket in bsdf.inputs
                if hasattr(socket, "default_value")}
        wanted = dict(self._new_material_inputs(), **entry["bsdf"])
        return all(_equal(mine.get(identifier), value) for identifier, value in wanted.items())

    def _new_material_inputs(self):
        if self._new_inputs is None:
            with bpy.data.temp_data() as data:
                material = data.materials.new("Fresh")
                if material.node_tree is None:
                    material.use_nodes = True
                bsdf = next(node for node in material.node_tree.nodes
                            if node.type == 'BSDF_PRINCIPLED')
                self._new_inputs = {socket.identifier: encode(socket.default_value)
                                    for socket in bsdf.inputs if hasattr(socket, "default_value")}
        return self._new_inputs

    def load_from_example(self, meshes=(), sounds=()):
        """Meshes (the items' shapes) and packed sounds, taken from the example file."""
        # Each once (a name twice in one load crashes Blender).
        meshes = [name for name in dict.fromkeys(meshes) if name not in self.built["meshes"]]
        sounds = [name for name in dict.fromkeys(sounds) if name not in self.built["sounds"]]
        if not meshes and not sounds:
            return
        source = self.recipe.get("source") or example_path()
        before = {kind: set(getattr(bpy.data, kind)) for kind in ("meshes", "sounds",
                                                                  "materials")}
        with bpy.data.libraries.load(source, link=False) as (data_from, data_to):
            data_to.meshes = list(meshes)
            data_to.sounds = list(sounds)
        for name, mesh in zip(meshes, data_to.meshes):
            self.built["meshes"][name] = mesh
            # Its own materials, as the recipe gives them (the object's slots are set after).
            mesh.materials.clear()
        for name, sound in zip(sounds, data_to.sounds):
            self.built["sounds"][name] = sound
        # Materials that came with the meshes: the recipe makes its own.
        for material in set(bpy.data.materials) - before["materials"]:
            if material.users == 0:
                bpy.data.materials.remove(material)

    # Objects --------------------------------------------------------------

    def objects(self, names, overrides=None):
        """Build these recipe objects, parented as in the example where both are built (else to
        what `remap` gives), each at its place in the example after `transform_for`. `overrides`:
        entries to build some of them from instead of the recipe's."""
        entries = dict(self.recipe["objects"], **(overrides or {}))
        self.load_from_example(meshes=[entries[name]["geometry"]["mesh"] for name in names
                                       if "mesh" in entries[name].get("geometry", {})])
        created = []
        for name in names:
            created.append(self._object(name, entries[name]))
        for name in names:
            self._parent(name, entries[name])
        for name in names:
            entry = entries[name]
            ob = self.built["objects"][name]
            apply_settings(ob.game, entry["game"], self.resolve)
            apply_settings(ob.game.ui, entry["ui"], self.resolve)
            for prop in entry["properties"]:
                add_game_property(ob, prop, self.resolve)
        return created

    def _object(self, name, entry):
        kind = entry["type"]
        data = None
        if kind == 'MESH':
            geometry = entry["geometry"]
            if "verts" in geometry:
                data = bpy.data.meshes.new(name)
                data.from_pydata([tuple(v) for v in geometry["verts"]], [], geometry["faces"])
                data.update()
            else:
                data = self.built["meshes"][geometry["mesh"]]
        elif kind == 'FONT':
            data = bpy.data.curves.new(name, 'FONT')
            text = dict(entry["text"])
            body = text.pop("body")
            apply_settings(data, text, self.resolve)
            data.body = body
        elif kind == 'CAMERA':
            data = bpy.data.cameras.new(name)
            apply_settings(data, entry["data"], self.resolve)
        elif kind == 'LIGHT':
            data = bpy.data.lights.new(name, entry["light_type"])
            apply_settings(data, entry["data"], self.resolve)
        ob = bpy.data.objects.new(name, data)
        # A shape taken from the example may serve several objects: its materials once.
        if kind in {'MESH', 'FONT'} and len(data.materials) == 0:
            for material_name in entry["materials"]:
                data.materials.append(self.material(material_name))
        collection = self.collection_for(name)
        if collection is None:
            collection = bpy.context.scene.collection
        collection.objects.link(ob)
        ob.matrix_basis = self.transform_for(name) @ Matrix(entry["world"])
        apply_settings(ob, entry["settings"], self.resolve)
        self.built["objects"][name] = ob
        return ob

    def _parent(self, name, entry):
        if entry["parent"] is None:
            return
        ob = self.built["objects"][name]
        parent = self.resolve({"id": "objects", "name": entry["parent"]})
        if parent is None:
            return
        # Kept where it is (Ctrl P > Object (Keep Transform)); a parent scaled to nothing has no
        # inverse, as in Blender.
        ob.parent = parent
        ob.matrix_parent_inverse = world_matrix(parent).inverted_safe()

    # Logic trees ----------------------------------------------------------

    def tree(self, name, frames=None, into=None, offset=(0.0, 0.0)):
        """Build a recipe tree (only the frames with these titles, when given; into an existing
        tree, moved by `offset`, when given). Returns the tree."""
        entry = self.recipe["trees"][name]
        tree = into or bpy.data.node_groups.new(name, "LogicNodeTree")
        if into is None:
            self.built["node_groups"][name] = tree
        wanted = None
        if frames is not None:
            wanted = {frame["name"] for frame in entry["frames"] if frame["label"] in frames}
            missing = set(frames) - {frame["label"] for frame in entry["frames"]}
            if missing:
                raise RecipeError(f"tree {name!r} has no frames {sorted(missing)}")
            # Checked before anything is built: a tree is never left half made.
            chosen = {spec["name"] for spec in entry["nodes"] if spec["frame"] in wanted}
            for link in entry["links"]:
                if (link["from_node"] in chosen) != (link["to_node"] in chosen):
                    raise RecipeError(f"tree {name!r}: a link leaves the chosen frames "
                                      f"({link['from_node']} -> {link['to_node']})")
        # The packed sounds its nodes play (Play Sound), taken from the example first.
        self.load_from_example(sounds=[
            value["name"] for spec in entry["nodes"]
            if wanted is None or spec["frame"] in wanted
            for value in spec["settings"].values()
            if isinstance(value, dict) and value.get("id") == "sounds"])
        made = {}
        for frame in entry["frames"]:
            if wanted is not None and frame["name"] not in wanted:
                continue
            node = tree.nodes.new("NodeFrame")
            node.name = frame["name"]
            node.label = frame["label"]
            apply_settings(node, frame["settings"], self.resolve)
            node.location = (frame["location"][0] + offset[0], frame["location"][1] + offset[1])
            made[frame["name"]] = node
        for spec in entry["nodes"]:
            if wanted is not None and spec["frame"] not in wanted:
                continue
            node = tree.nodes.new(spec["idname"])
            node.name = spec["name"]
            apply_settings(node, spec["settings"], self.resolve)
            for identifier, value in spec["inputs"].items():
                socket = next(s for s in node.inputs if s.identifier == identifier)
                if isinstance(value, dict):
                    socket.default_value = self.resolve(value)
                else:
                    socket.default_value = tuple(value) if isinstance(value, list) else value
            node.label = spec["label"]
            node.width = spec["width"]
            node.hide = spec["hide"]
            node.mute = spec["mute"]
            if spec["frame"] is not None:
                node.parent = made[spec["frame"]]
                node.location = spec["location"]
            else:
                node.location = (spec["location"][0] + offset[0],
                                 spec["location"][1] + offset[1])
            made[spec["name"]] = node
        for link in entry["links"]:
            if link["from_node"] not in made or link["to_node"] not in made:
                continue
            from_node, to_node = made[link["from_node"]], made[link["to_node"]]
            tree.links.new(
                next(s for s in from_node.outputs if s.identifier == link["from_socket"]),
                next(s for s in to_node.inputs if s.identifier == link["to_socket"]))
        return tree

    def bind(self, objects=None):
        """Logic tree bindings of built objects, as in the example."""
        for ob_name in objects or self.built["objects"]:
            ob = self.built["objects"][ob_name]
            for binding in self.recipe["objects"][ob_name]["bindings"]:
                tree = self.resolve({"id": "node_groups", "name": binding["tree"]})
                if tree is None:
                    continue
                new = ob.game.logic_node_binding_new()
                new.tree = tree
                new.enabled = binding["enabled"]

    # The whole scene ------------------------------------------------------

    def collections(self, scene):
        """Every collection of the example, in the example's places."""
        for entry in self.recipe["collections"]:
            if entry["name"] == "":
                self.built["collections"][""] = scene.collection
                continue
            collection = bpy.data.collections.new(entry["name"])
            self.built["collections"][entry["name"]] = collection
            self.built["collections"][entry["parent"]].children.link(collection)
            collection.hide_viewport = entry["hide_viewport"]
            collection.hide_render = entry["hide_render"]
            collection.hide_select = entry["hide_select"]

    def collection_marks(self):
        def decode(value):
            if isinstance(value, dict):
                if "id" in value and "name" in value:
                    return self.resolve(value)
                return {key: decode(item) for key, item in value.items()}
            if isinstance(value, list):
                return [decode(item) for item in value]
            return value

        for entry in self.recipe["collections"]:
            collection = self.built["collections"][entry["name"]]
            for key, value in entry["marks"].items():
                collection[key] = decode(value)

    def layer_collections(self, view_layer):
        # By the collections built (a name taken in the open file gives them another).
        names = {collection: name for name, collection in self.built["collections"].items()}

        def walk(layer):
            for child in layer.children:
                state = self.recipe["layer_collections"].get(names.get(child.collection))
                if state is not None:
                    child.exclude = state["exclude"]
                    child.hide_viewport = state["hide_viewport"]
                walk(child)
        walk(view_layer.layer_collection)

    def scene_settings(self, scene):
        entry = self.recipe["scene"]
        for name, value in entry["render"].items():
            setattr(scene.render, name, value)
        if entry["world"] is not None:
            if scene.world is None:
                scene.world = bpy.data.worlds.new("World")
            apply_settings(scene.world, entry["world"], self.resolve)
        actions = scene.game_settings.input_actions
        recipe_names = {action["settings"]["name"] for action in entry["input_actions"]}
        for action in [action for action in actions if action.name not in recipe_names]:
            actions.remove(action)
        for action_entry in entry["input_actions"]:
            input_action(scene, action_entry, replace=True)
        apply_settings(scene.game_settings, entry["game"], self.resolve)
        if entry["camera"] is not None:
            scene.camera = self.built["objects"].get(entry["camera"])

    def everything(self, scene):
        """The whole example in `scene` (an empty scene of a new file)."""
        self.collections(scene)
        recipe_objects = self.recipe["objects"]
        homes = {}
        for entry in self.recipe["collections"]:
            for name in entry["objects"]:
                homes.setdefault(name, []).append(entry["name"])
        self.collection_for = lambda name: self.built["collections"][homes[name][0]]
        names = list(recipe_objects)
        self.objects(names)
        for name in names:
            ob = self.built["objects"][name]
            for extra in homes[name][1:]:
                self.built["collections"][extra].objects.link(ob)
        for tree_name in self.recipe["trees"]:
            self.tree(tree_name)
        self.bind(names)
        self.collection_marks()
        self.layer_collections(scene.view_layers[0])
        self.scene_settings(scene)
        return self.built
