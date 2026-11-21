# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Add > Game Navigation Examples (2026-11-20, Navigation_Ease_Of_Use_Plan.md item 14): every
template built into a scene, in the background.

- The 3D view's Add menu has Game Navigation Examples, listing every template once (the startup
  module appends it: no Blender file lists it).
- Each NPC template makes its tree and binds it to the selected objects (a sample NPC at the 3D
  cursor when none is selected): the tree's links all valid, its nodes those the recipe names,
  their settings as it says; the selected object keeps its physics, its other bindings, its
  name.
- The object templates make their objects with Physics > Navigation set: the ladder's link kind
  Ladder, the door's Door link closed at start and Wait, the lift's cab a vehicle with a link at
  each floor, ending on the cab.
- Nothing else of the scene changes (its navigation settings included); a template asking for a
  target refuses to build without one.

    blender --background --factory-startup --python tests/python/navigation_templates.py
"""

import sys

import bpy
from mathutils import Vector
from _bpy_internal.navigation import templates

PROBLEMS = []


def check(condition, text):
    if not condition:
        PROBLEMS.append(text)


def works_as(node):
    return getattr(node, "variant_idname", "") or node.bl_idname


def nodes_of(tree, idname):
    """Nodes of an idname (a merged node's own or its option's former one)."""
    return [n for n in tree.nodes if idname in (works_as(n), n.bl_idname)]


def bound_trees(ob):
    return [b.tree for b in ob.game.logic_node_bindings if b.tree is not None]


def clear():
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    for tree in list(bpy.data.node_groups):
        if tree.bl_idname == "LogicNodeTree":
            bpy.data.node_groups.remove(tree)


def deselect(context):
    for ob in context.view_layer.objects:
        ob.select_set(False)


def scene_settings(scene):
    rd = scene.game_settings.recast_data
    return {p.identifier: getattr(rd, p.identifier) for p in rd.bl_rna.properties
            if p.identifier != "rna_type" and not p.is_readonly}


def main():
    context = bpy.context
    scene = context.scene

    # The menu.
    check(hasattr(bpy.types, "VIEW3D_MT_navigation_add"), "no Add > Game Navigation Examples menu")
    check(getattr(getattr(bpy.types, "VIEW3D_MT_navigation_add", None), "bl_label", "") ==
          "Game Navigation Examples", "the menu is not called Game Navigation Examples")
    check(hasattr(bpy.types, "OBJECT_OT_navigation_template_add") or
          "navigation_template_add" in dir(bpy.ops.object), "no template operator")
    drawn = []

    class Layout:
        operator_context = None

        def operator(self, idname, text="", icon='NONE'):
            class Props:
                pass
            props = Props()
            drawn.append((idname, text, props))
            return props

        def separator(self):
            pass

    class Menu:
        layout = Layout()
    bpy.types.VIEW3D_MT_navigation_add.draw(Menu(), context)
    listed = [getattr(props, "template", None) for _, _, props in drawn]
    check(sorted(listed) == sorted(templates.TEMPLATE_IDS),
          "the menu lists %s, the templates are %s" % (listed, templates.TEMPLATE_IDS))
    in_add = any(getattr(f, "__name__", "") == "_draw_add_menu"
                 for f in bpy.types.VIEW3D_MT_add._dyn_ui_initialize())
    check(in_add, "the Navigation menu is not in the 3D view's Add menu")

    # (Away from the origin: a part placed twice the cursor's offset away shows.)
    scene.cursor.location = (3.0, -2.0, 1.0)
    before_settings = scene_settings(scene)
    player = bpy.data.objects.new("Player", None)
    scene.collection.objects.link(player)

    # A target is asked for.
    for template in templates.NEEDS_TARGET:
        try:
            templates.add(context, template, target=None)
            PROBLEMS.append("%s built without a target" % template)
        except templates.TemplateError:
            pass

    expect_nodes = {
        'PATROL': ("LogicNativeFollowPath", "LogicNativeListFromItems"),
        'CHASER': ("LogicNativeNavigate",),
        'HORDE': ("LogicNativeNavigate",),
        'WANDER': ("LogicNativeNavigate", "LogicNativeSetTreeProperty"),
        'COMPANION': ("LogicNativeNavigate", "LogicNativeGetDistance", "LogicNativeMapRange"),
        'FLYER': ("LogicNativeNavigate",),
        'COVER': ("LogicNativeNavmeshQuery", "LogicNativeNavigate"),
        'DOOR': ("LogicNativeSetNavigationAttribute", "LogicNativeOnValueChange"),
        'LIFT': ("LogicNativePlayAction",),
    }
    for template in templates.TEMPLATE_IDS:
        for selected in (False, True):
            if selected and template in templates.MAKES_OBJECTS:
                continue
            what = "%s (%s)" % (template, "on a selected object" if selected else "a sample")
            deselect(context)
            mine = None
            if selected:
                mine = bpy.data.objects.new("My NPC", bpy.data.meshes.new("My NPC"))
                scene.collection.objects.link(mine)
                mine.game.physics_type = 'DYNAMIC'
                mine.select_set(True)
            existing = set(bpy.data.objects)
            target = player if template in templates.NEEDS_TARGET else None
            tree, made = templates.add(context, template, target=target, height=5.0)
            check(set(made) == set(bpy.data.objects) - existing,
                  "%s: made %s, new objects %s" % (what, [o.name for o in made],
                                                   [o.name for o in set(bpy.data.objects) -
                                                    existing]))
            if template in expect_nodes:
                check(tree is not None, "%s: no tree" % what)
                if tree is None:
                    continue
                check(all(link.is_valid for link in tree.links), "%s: an invalid link" % what)
                for idname in expect_nodes[template]:
                    check(nodes_of(tree, idname), "%s: no %s" % (what, idname))
                check(any(n.bl_idname == "NodeFrame" and n.label for n in tree.nodes),
                      "%s: no titled frame" % what)
                for n in tree.nodes:
                    if n.bl_idname != "NodeFrame":
                        check(n.parent is not None, "%s: %s outside every frame" % (what, n.name))
            runs = [o for o in bpy.data.objects if tree is not None and tree in bound_trees(o)]
            if template in templates.MAKES_OBJECTS:
                pass
            elif selected:
                check(runs == [mine], "%s: tree bound to %s" % (what, [o.name for o in runs]))
                check(mine.game.physics_type == 'DYNAMIC' and mine.name.startswith("My NPC"),
                      "%s: its physics or name changed" % what)
                check(all(o.name.startswith("Patrol Point") for o in made),
                      "%s: made %s" % (what, [o.name for o in made]))
            else:
                check(len(runs) == 1 and runs[0] in made, "%s: no sample running it" % what)
                if runs:
                    check(runs[0].location == scene.cursor.location or template == 'FLYER',
                          "%s: the sample not at the 3D cursor" % what)
            navs = nodes_of(tree, "LogicNativeNavigate") if tree is not None else []
            if template in templates.NEEDS_TARGET and template != 'COVER':
                check(navs and navs[0].inputs["Target Object"].default_value == player,
                      "%s: Navigate's Target Object not the target" % what)
            if template == 'CHASER':
                check(navs[0].mode == 'PURSUE' and navs[0].inputs["Avoidance"].default_value == 1,
                      "%s: not Pursue Polite" % what)
            if template == 'HORDE':
                check(navs[0].mode == 'PURSUE' and navs[0].inputs["Avoidance"].default_value == 2,
                      "%s: not Pursue Dense" % what)
            if template == 'FLYER':
                check(navs[0].mode == 'WANDER' and navs[0].movement == 'FLY',
                      "%s: not Wander, Fly" % what)
            if template == 'COVER':
                query = nodes_of(tree, "LogicNativeNavmeshQuery")[0]
                check(query.query == 'FIND_SPOT' and query.spot_visibility == 'HIDDEN' and
                      query.inputs["Look Object"].default_value == player,
                      "%s: Find Spot not hidden from the target" % what)
                check(navs[0].use_until_arrived, "%s: not Until Arrived" % what)
            if template == 'PATROL':
                points = [o for o in made if o.name.startswith("Patrol Point")]
                check(len(points) == 4, "%s: %d patrol points" % (what, len(points)))
                follow = nodes_of(tree, "LogicNativeFollowPath")[0]
                check(follow.way == 'NAVMESH' and follow.inputs["Loop"].default_value,
                      "%s: Follow Path not Navmesh, Loop" % what)
            # (Where they are in the world: parented parts kept their places.)
            context.view_layer.update()
            cursor = scene.cursor.location
            if template == 'LADDER':
                foot = next(o for o in made if o.name.startswith("Ladder Foot"))
                top = next(o for o in made if o.name.startswith("Ladder Top"))
                check((foot.matrix_world.translation - (cursor + Vector((0, -0.6, 0)))).length <
                      1e-4 and (top.matrix_world.translation -
                                (cursor + Vector((0, 0.6, 5.0)))).length < 1e-4,
                      "%s: the link's ends not beside the ladder (%s, %s)" %
                      (what, foot.matrix_world.translation, top.matrix_world.translation))
                check(foot.game.nav_link_end == top and foot.game.nav_link_kind == 'LADDER' and
                      foot.game.use_nav_link_both_ways,
                      "%s: the foot's link is not a ladder both ways to the top" % what)
                check(abs((top.location - foot.location).z - 5.0) < 1e-4 or
                      abs(top.matrix_world.translation.z - foot.matrix_world.translation.z -
                          5.0) < 1e-4, "%s: the top not 5 m up" % what)
            if template == 'DOOR':
                link = next(o for o in made if o.name == "Door Link")
                check((link.matrix_world.translation - (cursor + Vector((0, -0.9, 0)))).length <
                      1e-4, "%s: the link not in front of the door (%s)" %
                      (what, link.matrix_world.translation))
                check(link.game.nav_link_kind == 'DOOR' and not link.game.use_nav_link_open and
                      link.game.nav_link_when_closed == 'WAIT',
                      "%s: the door's link not a closed Wait door" % what)
                leaf = next(o for o in made if o.name == "Door")
                check("open" in leaf.game.properties and not leaf.game.properties["open"].value,
                      "%s: no 'open' property off" % what)
                setters = nodes_of(tree, "LogicNativeSetNavigationAttribute")
                check(len(setters) == 2 and all(s.attribute == 'LINK_ENABLED' and
                                                s.inputs["Object"].default_value == link
                                                for s in setters),
                      "%s: 'open' does not set the link" % what)
            if template == 'LIFT':
                cab = next(o for o in made if o.name == "Lift Cab")
                check(cab.game.use_nav_vehicle and cab.game.physics_type == 'KINEMATIC',
                      "%s: the cab not a Kinematic vehicle" % what)
                doors = [o for o in made if o.game.nav_link_end is not None]
                end = doors[0].game.nav_link_end if doors else None
                check(end is not None and
                      (end.matrix_world.translation - (cursor + Vector((-0.9, 0, 0)))).length <
                      1e-4, "%s: the cab's door not on the cab's floor (%s)" %
                      (what, end.matrix_world.translation if end else None))
                check(len(doors) == 2 and all(d.game.nav_link_end.parent == cab for d in doors),
                      "%s: not a link at each floor onto the cab" % what)
                heights = sorted(round(d.location.z - doors[0].location.z, 4) for d in doors)
                check(heights in ([0.0, 5.0], [-5.0, 0.0]), "%s: floors %s apart" % (what, heights))
                check(cab.animation_data and cab.animation_data.action,
                      "%s: the cab has no ride" % what)
    check(scene_settings(scene) == before_settings, "the scene's navigation settings changed")
    clear()


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback
        traceback.print_exc()
        PROBLEMS.append("an exception")
    if PROBLEMS:
        for p in PROBLEMS:
            print("PROBLEM:", p)
        sys.exit(1)
    print("navigation templates: all good")
