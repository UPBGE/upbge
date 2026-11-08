# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import json
import os
import textwrap

import bpy
from bpy.types import Panel

from bl_operators.logic_nodes_bindings import (
    logic_nodes_binding_enabled_icon,
    logic_nodes_binding_status,
    logic_nodes_is_logic_editor_space,
    logic_nodes_object_has_tree,
    logic_nodes_selected_objects,
)


class NODE_PT_logic_nodes_administration(Panel):
    bl_label = "Administration"
    bl_space_type = 'NODE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Dashboard"

    @classmethod
    def poll(cls, context):
        return logic_nodes_is_logic_editor_space(context.space_data)

    def draw(self, context):
        layout = self.layout
        box = layout.box()
        col = box.column()
        col.scale_y = 1.4
        col.operator("object.logic_nodes_apply_tree", text="Apply To Selected", icon='PREFERENCES')
        col.operator("object.logic_nodes_binding_clear", text="Remove From Selected", icon='X')


class NODE_PT_logic_nodes_object_trees(Panel):
    bl_label = "Object Trees"
    bl_space_type = 'NODE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Dashboard"

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        if ob is None or not ob.select_get():
            return False
        return logic_nodes_is_logic_editor_space(context.space_data)

    def draw(self, context):
        layout = self.layout
        selected_objects = logic_nodes_selected_objects(context)
        active_tree_items = {}

        box_over = layout.box()
        title = box_over.row()
        for ob in selected_objects:
            for binding in ob.game.logic_node_bindings:
                tree_name = (binding.tree_name or "").strip()
                if not tree_name:
                    continue
                active_tree_items.setdefault(tree_name, []).append((ob, binding))

        tree_count = len(active_tree_items)
        if context.object:
            title.label(text=f"Trees applied to {context.object.name}: {tree_count}")

        if tree_count == 0:
            box_over.label(text="No logic trees applied", icon='INFO')
            return

        for tree_name in sorted(active_tree_items):
            entries = active_tree_items[tree_name]
            box = box_over.box()
            bindings = [binding for _ob, binding in entries]

            # One row, as in the Properties editor's Logic Nodes panel: Edit keeps its own size at
            # the right, with a small gap before the remove button.
            row = box.row(align=True)
            row.label(text="", icon=logic_nodes_binding_enabled_icon(bindings))
            row.label(text=tree_name, icon='NODETREE')
            buttons = row.row()
            buttons.alignment = 'RIGHT'
            buttons.operator("object.logic_nodes_find_tree", text="Edit", icon='NODETREE').tree_name = tree_name
            buttons.operator("object.logic_nodes_unapply_tree", text="", icon='X').tree_name = tree_name

            status_text, status_icon = logic_nodes_binding_status(context, bindings[0])
            if status_text:
                box.label(text=status_text, icon=status_icon)


class NODE_PT_logic_nodes_tree_applied_to(Panel):
    bl_label = "Tree applied to:"
    bl_space_type = 'NODE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Dashboard"

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return (
            logic_nodes_is_logic_editor_space(space)
            and getattr(space, "edit_tree", None) is not None
        )

    def draw_owner(self, layout, obj, binding, tree_name):
        box = layout.box()
        row = box.split(factor=0.65)
        name = row.row()
        name.alignment = 'LEFT'
        name.prop(binding, "enabled", text="")
        name.label(text=obj.name)

        # A small gap between selecting the object and removing the tree from it.
        buttons = row.row()
        buttons.alignment = 'RIGHT'
        buttons.operator(
            "object.logic_nodes_select_owner",
            text="",
            icon="RESTRICT_SELECT_OFF",
        ).applied_object = obj.name
        op = buttons.operator("object.logic_nodes_unapply_tree", text="", icon="X")
        op.tree_name = tree_name
        op.from_object = obj.name

        status_text, status_icon = logic_nodes_binding_status(bpy.context, binding)
        if status_text:
            box.label(text=status_text, icon=status_icon)

    def draw(self, context):
        layout = self.layout
        tree = context.space_data.edit_tree
        container = layout.column(align=True)
        found = False

        for obj in bpy.data.objects:
            if obj.name not in context.view_layer.objects:
                continue
            if not obj.game:
                continue
            if not logic_nodes_object_has_tree(obj, tree):
                continue
            for binding in obj.game.logic_node_bindings:
                if binding.tree == tree:
                    self.draw_owner(container, obj, binding, tree.name)
                    found = True
                    break

        if not found:
            container.label(text="Not applied to any object", icon='INFO')


# The play report the game writes when a scene ends (LN_Manager::WriteGameReport): one file per
# scene in Blender's base temp folder (blenderplayer writes there too). Read again only when the
# file changed.
_report_cache = {}


def _report_folder():
    session = os.path.normpath(bpy.app.tempdir)
    # The session folder is "<base>/blender_XXXXXX"; without one Blender uses the base itself.
    if os.path.basename(session).startswith("blender_"):
        return os.path.dirname(session)
    return session


def logic_nodes_report(scene):
    # Byte by byte as the engine names it (each byte of a non-ASCII letter becomes "_").
    name = "".join(
        chr(b) if (b < 128 and (chr(b).isalnum() or chr(b) in "-_")) else "_"
        for b in scene.name.encode("utf-8")
    )
    path = os.path.join(_report_folder(), "logic_nodes_report_" + name + ".json")
    try:
        stamp = os.path.getmtime(path)
    except OSError:
        return None
    cached = _report_cache.get(path)
    if cached is not None and cached[0] == stamp:
        return cached[1]
    try:
        with open(path, encoding="utf-8") as handle:
            report = json.load(handle)
    except (OSError, ValueError):
        return None
    _report_cache[path] = (stamp, report)
    return report


def _wrapped(layout, context, text, icon='NONE'):
    """Labels of `text` that fit the sidebar (a label does not wrap)."""
    width = max(24, int(context.region.width / (7.0 * context.preferences.system.ui_scale)))
    for index, line in enumerate(textwrap.wrap(text, width) or [""]):
        layout.label(text=line, icon=icon if index == 0 else 'BLANK1' if icon != 'NONE' else 'NONE')


_EVENT_TEXT = {"steps": "On Logic + Physics Step", "frames": "On Frame Update"}
_KIND_TEXT = {
    "wide": ("wide column runs", 'CHECKMARK'),
    "wide_main": ("wide column runs, main thread only", 'CHECKMARK'),
    "pass": ("one tree at a time (no crowd speed-up)", 'ERROR'),
}


def _is_crowd(report, entry):
    """Whether a tree runs on enough objects for wide runs to matter (the report's
    "crowd_objects", set by the engine; 16 in reports written before it had one). A tree on fewer
    objects that runs one at a time costs little and is shown without a warning."""
    return entry.get("objects", 0) >= report.get("crowd_objects", 16)


class NODE_PT_logic_nodes_performance(Panel):
    bl_label = "Performance"
    bl_space_type = 'NODE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Dashboard"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return logic_nodes_is_logic_editor_space(context.space_data)

    def draw_tree(self, layout, context, report, entry):
        crowd = _is_crowd(report, entry)
        for event in entry.get("events", []):
            kind = event.get("kind")
            kind_text, icon = _KIND_TEXT.get(kind, (event.get("kind", ""), 'QUESTION'))
            if kind == "pass" and not crowd:
                # One at a time on a few objects: worth knowing, nothing to worry about.
                icon = 'INFO'
            _wrapped(layout, context, "{:s}: {:s}".format(_EVENT_TEXT.get(event.get("event"), ""),
                                                          kind_text), icon)
            reason = event.get("reason", "")
            if reason and kind != "wide":
                _wrapped(layout, context, "Why: " + reason)
            if kind == "pass" and not crowd:
                _wrapped(layout, context, "On few objects this costs little: wide runs pay for "
                         "trees on many objects.")
            elif event.get("hint"):
                _wrapped(layout, context, "Try: " + event["hint"], 'LIGHT')
        awake = entry.get("never_sleeps", [])
        if awake:
            _wrapped(layout, context,
                     "Never sleeps between its Every N Steps: these run every step: " +
                     ", ".join(awake) + ". If they only matter on the slowed route, put them "
                     "inside it (a Branch instead of a Boolean Edge).", 'TIME')
        for issue in entry.get("issues", []):
            node = issue.get("node", "")
            _wrapped(layout, context, (node + ": " if node else "") + issue.get("message", ""),
                     'CANCEL' if issue.get("severity") == "error" else 'ERROR')

    def draw(self, context):
        layout = self.layout
        report = logic_nodes_report(context.scene)
        if report is None:
            _wrapped(layout, context, "Play the game once: this panel then shows how each tree ran.",
                     'INFO')
            return
        # Start Game In Player plays a copy (wm.blenderplayer_start): "<file>~", or game.blend in
        # the session folder for an unsaved file.
        played = report.get("file", "")
        if bpy.data.is_saved:
            this_file = played in {bpy.data.filepath, bpy.data.filepath + "~"}
        else:
            this_file = played in {"", os.path.join(bpy.app.tempdir, "game.blend")}
        if not this_file:
            _wrapped(layout, context, "This report is of another file: play this one again.",
                     'ERROR')
        _wrapped(layout, context, "Last play: " + report.get("ended", ""))
        if "random_seed" in report:
            # Typed into Scene > Game > Random Numbers > Fixed, the same numbers come again.
            _wrapped(layout, context, "Random seed: {:d}{:s}".format(
                report["random_seed"], " (new each play)" if report.get("random_seed_new") else ""))
        trees = report.get("trees", [])
        tree = getattr(context.space_data, "edit_tree", None)
        if tree is not None:
            box = layout.box()
            box.label(text=tree.name, icon='NODETREE')
            entry = next((t for t in trees if t.get("tree") == tree.name), None)
            if entry is None:
                box.label(text="Not run in the last play", icon='INFO')
            else:
                box.label(text="On {:d} objects".format(entry.get("objects", 0)))
                self.draw_tree(box, context, report, entry)
        # Only what is worth acting on: crowds without the speed-up, trees that never sleep.
        slow = [t for t in trees if _is_crowd(report, t) and
                any(e.get("kind") == "pass" for e in t.get("events", []))]
        awake = [t for t in trees if t.get("never_sleeps")]
        if not slow and not awake:
            _wrapped(layout, context, "Nothing to speed up found in the last play.", 'CHECKMARK')
        for title, chosen in (("Crowd trees run one at a time", slow),
                              ("Trees that never sleep between their steps", awake)):
            if not chosen:
                continue
            box = layout.box()
            box.label(text=title)
            for entry in chosen[:12]:
                row = box.row()
                row.label(text="{:s} ({:d})".format(entry.get("tree", ""), entry.get("objects", 0)))
                if entry.get("tree") in bpy.data.node_groups:
                    row.operator("object.logic_nodes_find_tree", text="",
                                 icon='NODETREE').tree_name = entry["tree"]
            if len(chosen) > 12:
                box.label(text="... and {:d} more".format(len(chosen) - 12))


classes = (
    NODE_PT_logic_nodes_administration,
    NODE_PT_logic_nodes_object_trees,
    NODE_PT_logic_nodes_tree_applied_to,
    NODE_PT_logic_nodes_performance,
)

if __name__ == "__main__":
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
