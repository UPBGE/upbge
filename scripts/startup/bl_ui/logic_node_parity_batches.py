# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Batch groupings for native logic node parity audit.

See plan_native_logic_nodes_roadmap.md for tiered future implementation list.
"""

# Catalog `family` prefix or exact match; None means all implemented nodes.
PARITY_BATCHES: dict[int, dict] = {
    0: {"name": "tooling", "families": None},
    1: {
        "name": "events_time",
        "families": ("Events/Logic/Time", "Events", "Logic"),
        "exclude_idnames": (
            "LogicNativeSendEvent",
            "LogicNativeReceiveEvent",
            "LogicNativeCollision",
            "LogicNativeOnCollision",
        ),
    },
    2: {"name": "values", "families": ("Values/Math", "Values/Vector", "Values")},
    3: {"name": "math", "families": ("Values/Math", "Math")},
    4: {"name": "properties", "families": ("Properties/Object", "Properties", "Values/Properties")},
    5: {
        "name": "attributes",
        "families": (
            "Properties/Object/Transform",
            "Objects/Get Attribute",
            "Objects/Set Attribute",
        ),
    },
    6: {
        "name": "objects",
        "families": ("Properties/Object/Transform", "Objects", "Objects/Transformation", "Objects/Object Data"),
    },
    7: {"name": "input", "families": ("Input",)},
    8: {"name": "physics", "families": ("Physics/Jolt", "Physics/Character", "Physics/Vehicle", "Physics")},
    9: {
        "name": "animation",
        "families": ("Animation", "Animation/Get Rest Bone Data", "Animation/Get Pose Bone Data", "Animation/Set Pose Bone Data", "Animation/Advanced Bone Data", "Animation/Actions", "Animation/Bone Constraints"),
    },
    10: {"name": "sound", "families": ("Sound",)},
    11: {
        "name": "scene_render",
        "families": ("Scene/Camera/Render", "Scene/Camera", "Scene", "Render", "Lights", "Game"),
    },
    12: {
        "name": "data_logic",
        "families": ("Logic/Trees", "Logic/Loops", "Data/List", "Data/Dict", "Data", "Utility"),
        "include_idnames": (
            "LogicNativeSendEvent",
            "LogicNativeReceiveEvent",
            "LogicNativeCollision",
            "LogicNativeOnCollision",
            "LogicNativeCopyProperty",
        ),
    },
    13: {"name": "misc", "families": ("Nodes", "Objects/Curves", "Time", "Nodes/Materials")},
    14: {"name": "native_only", "families": None, "native_only_only": True},
}


def entry_in_batch(entry: dict, batch: int) -> bool:
    if batch not in PARITY_BATCHES:
        return True
    spec = PARITY_BATCHES[batch]
    if spec.get("native_only_only"):
        from logic_node_addon_reference import NATIVE_ONLY
        return entry["idname"] in NATIVE_ONLY

    idname = entry["idname"]
    exclude = spec.get("exclude_idnames")
    if exclude and idname in exclude:
        return False
    include = spec.get("include_idnames")
    if include and idname in include:
        return True

    families = spec.get("families")
    if families is None:
        return True
    family = entry.get("family", "")
    return any(family == f or family.startswith(f + "/") or f.startswith(family) for f in families)
