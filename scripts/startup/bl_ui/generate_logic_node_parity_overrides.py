# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Generate logic_node_parity_socket_overrides.py (EXPECTED_ADDON_SOCKETS extensions).

  python3 scripts/startup/bl_ui/generate_logic_node_parity_overrides.py
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CATALOG = REPO / "scripts/startup/bl_ui/node_add_menu_logic_catalog.py"
OUT = Path(__file__).with_name("logic_node_parity_socket_overrides.py")


def sock(*names: str) -> dict[str, str]:
    return {n: "socket" for n in names}


def main() -> int:
    text = CATALOG.read_text(encoding="utf-8")
    entries: dict[str, str] = {}
    for m in re.finditer(
        r'_implemented\("([^"]+)",\s*"([^"]+)",',
        text,
    ):
        entries[m.group(1)] = m.group(2)

    SET_ATTR = (sock("Condition", "Object", "Value"), sock("Done"))
    SET_ATTR_COLOR = (sock("Condition", "Object", "Color"), sock("Done"))
    SET_VISIBILITY = (sock("Condition", "Object", "Visible", "Include Children"), sock("Done"))
    APPLY_M = (sock("Condition", "Object", "Vector"), sock("Done"))
    APPLY_IMPULSE = (sock("Condition", "Object", "Impulse", "Vector"), sock("Done"))
    ACTION_COND_DONE = (sock("Condition"), sock("Done"))
    ACTION_OBJ_DONE = (sock("Condition", "Object"), sock("Done"))
    SCENE_ACTION = (sock("Condition", "Scene"), sock("Done"))
    LIGHT_SET = (sock("Condition", "Light Object", "Value"), sock("Done"))

    overrides: dict[str, tuple[dict[str, str], dict[str, str]]] = {
        "LogicNativeValueBool": ({}, sock("Bool")),
        "LogicNativeValueInt": ({}, sock("Int")),
        "LogicNativeValueFloat": ({}, sock("Float")),
        "LogicNativeValueString": ({}, sock("String")),
        "LogicNativeValueVector": ({}, sock("Vector")),
        "LogicNativeColorRGB": ({}, sock("Color")),
        "LogicNativeColorRGBA": ({}, sock("Color")),
        "LogicNativeMath": (sock("A", "B"), sock("Result")),
        "LogicNativeVectorMath": (sock("Vector"), sock("Result")),
        "LogicNativeCompare": (sock("A", "B"), sock("Result")),
        "LogicNativeMapRange": (sock("Value", "From Min", "From Max", "To Min", "To Max"), sock("Result")),
        "LogicNativeRangedThreshold": (sock("Value", "Min", "Max"), sock("Result")),
        "LogicNativeLimitRange": (sock("Value", "Min", "Max"), sock("Result")),
        "LogicNativeWithinRange": (sock("Value", "Min", "Max"), sock("Result")),
        "LogicNativeRandomValue": (sock("Min", "Max"), sock("Value")),
        "LogicNativeStoreValue": (sock("Condition", "Value"), sock("Done", "Stored Value")),
        "LogicNativeValueSwitch": (sock("A if True, else B", "True", "False"), sock("Result")),
        "LogicNativeStringOperation": (sock("String"), sock("String")),
        "LogicNativeFormattedString": (sock("Format"), sock("String")),
        "LogicNativeInvert": (sock("Value"), sock("Result")),
        "LogicNativeCombineXYZ": (sock("X", "Y", "Z"), sock("Vector")),
        "LogicNativeSeparateXYZ": (sock("Vector"), sock("X", "Y", "Z")),
        "LogicNativeCombineXY": (sock("X", "Y"), sock("Vector")),
        "LogicNativeSeparateXY": (sock("Vector"), sock("X", "Y")),
        "LogicNativeEuler": (sock("X", "Y", "Z"), sock("Rotation")),
        "LogicNativeVectorToRotation": (sock("Direction", "Up"), sock("Rotation")),
        "LogicNativeModifyProperty": (sock("Condition", "Object", "Property", "Min", "Max"), sock("Done")),
        "LogicNativeModifyPropertyClamped": (sock("Condition", "Object", "Property", "Range"), sock("Done")),
        "LogicNativeCopyProperty": (sock("Condition", "Source", "Target", "Property"), sock("Done")),
        "LogicNativeGetTreeProperty": (sock("Property"), sock("Property")),
        "LogicNativeSetTreeProperty": (sock("Condition", "Name", "Value"), {}),
        "LogicNativeDelay": (sock("Condition", "Delay"), sock("Out")),
        "LogicNativePulsify": (sock("Condition", "Gap"), sock("Out")),
        "LogicNativeValueChangedTo": (sock("Value", "Target"), sock("Result")),
        "LogicNativeKeyboardKey": (sock("Key"), sock("If Pressed")),
        "LogicNativeMouseButton": (sock("Button"), sock("If Pressed")),
        "LogicNativeMouseWheel": ({}, sock("When Scrolled", "Difference")),
        "LogicNativeGamepadButton": (sock("Index", "Button"), sock("Pressed", "Strength")),
        "LogicNativeGamepadStick": (
            sock("Invert X", "Invert Y", "Index", "Sensitivity", "Threshold"),
            sock("X", "Y", "Vector"),
        ),
        "LogicNativeGamepadLook": (
            sock(
                "Condition",
            "Body",
            "Head",
                "Inverted",
                "Index",
                "Sensitivity",
                "Exponent",
                "Cap Left / Right",
                "Cap Up / Down",
                "Threshold",
            ),
            sock("Done"),
        ),
        "LogicNativeGamepadVibration": (
            sock("Condition", "Index", "Left", "Right", "Time"),
            sock("Done"),
        ),
        "LogicNativeCursorPosition": ({}, sock("Position", "X", "Y")),
        "LogicNativeCursorMovement": ({}, sock("Movement", "X", "Y")),
        "LogicNativeSetCursorPosition": (sock("Condition", "Screen X", "Screen Y"), sock("Done")),
        "LogicNativeSetCursorVisibility": (sock("Condition", "Visible"), sock("Done")),
        "LogicNativeGetChild": (sock("Parent", "Index"), sock("Child")),
        "LogicNativeGetCollisionGroup": ({}, sock("Bitmask")),
        "LogicNativeSetCollisionGroup": (sock("Condition", "Bitmask"), sock("Done")),
        "LogicNativeGetLightColor": (sock("Light Object"), sock("Color")),
        "LogicNativeGetLightPower": (sock("Light Object"), sock("Power")),
        "LogicNativeSetLightColor": LIGHT_SET,
        "LogicNativeSetLightPower": (sock("Condition", "Light Object", "Power"), sock("Done")),
        "LogicNativeSetLightShadow": (sock("Condition", "Object", "Use Shadow"), sock("Done")),
        "LogicNativeMakeLightUnique": (sock("Condition", "Light Object"), sock("Done")),
        "LogicNativeGetObjectAttribute": (
            sock("Object"),
            sock("Name", "Vector", "Visible", "Position", "Orientation", "Scale", "Color"),
        ),
        "LogicNativeSetObjectAttribute": (
            sock(
                "Condition",
                "XYZ",
                "Object",
                "Value",
                "Position",
                "Rotation",
                "Scale",
                "Color",
                "Visible",
                "Include Children",
            ),
            sock("Done"),
        ),
        "LogicNativeApplyRotation": (
            sock("Condition", "Object", "Rotation", "Local"),
            sock("Done"),
        ),
        "LogicNativeEvaluateCurve": (sock("Curve", "Factor"), sock("Vector")),
        "LogicNativeAddObject": (
            sock("Condition", "Object to Add", "Copy Transform", "Life", "Full Copy"),
            sock("Out"),
        ),
        "LogicNativeRemoveObject": ACTION_OBJ_DONE,
        "LogicNativeRemoveParent": (sock("Condition", "Child Object"), sock("Out")),
        "LogicNativeReplaceMesh": (sock("Condition", "Object", "Mesh Object"), sock("Done")),
        "LogicNativeMoveToward": (sock("Condition", "Object", "Target", "Speed"), sock("Done")),
        "LogicNativeRotateToward": (
            sock("Condition", "Object", "Target", "Factor", "Rot Axis", "Front"),
            sock("Done"),
        ),
        "LogicNativeSlowFollow": (sock("Condition", "Object", "Target", "Factor"), sock("Done")),
        "LogicNativeAlignAxisToVector": (sock("Condition", "Object", "Vector", "Axis"), sock("Done")),
        "LogicNativeRaycast": (
            sock(
                "Flow",
                "Caster",
                "Ignore Object",
                "Origin",
                "Destination",
                "Local",
                "Property",
                "X-Ray",
                "Mask",
                "Visualize",
            ),
            sock(
                "Done",
                "Has Result",
                "Hit Object",
                "Point",
                "Normal",
                "Direction",
                "Distance",
                "Face Index",
                "Has UV",
                "UV",
            ),
        ),
        "LogicNativeRaycastAll": (
            sock(
                "Flow",
                "Caster",
                "Ignore Object",
                "Origin",
                "Destination",
                "Local",
                "Property",
                "X-Ray",
                "Mask",
                "Visualize",
            ),
            sock(
                "Done",
                "Has Result",
                "Hit Count",
                "Hit Objects",
                "Points",
                "Normals",
                "Distances",
                "Face Indices",
                "Has UVs",
                "UVs",
            ),
        ),
        "LogicNativeCollision": (
            sock("Object", "Property", "Material"),
            sock("Colliding",
                 "Collided Object",
                 "Collided Objects",
                 "Contact Count",
                 "Point",
                 "Points",
                 "Normal",
                 "Normals"),
        ),
        "LogicNativeOnCollision": (
            sock("Object", "Property", "Material"),
            sock("On Enter",
                 "On Stay",
                 "On Exit",
                 "Colliding",
                 "Collided Object",
                 "Collided Objects",
                 "Contact Count",
                 "Point",
                 "Points",
                 "Normal",
                 "Normals"),
        ),
        "LogicNativePlayAction": (sock("Condition", "Object", "Animation Layer"), sock("Done")),
        "LogicNativeSetActionFrame": (sock("Condition", "Object", "Animation Layer", "Frame"), sock("Done")),
        "LogicNativePlaySound": (sock("Condition", "Volume", "Pitch", "Loop"), sock("Done")),
        "LogicNativePlaySound3D": (sock("Condition", "Speaker", "Volume", "Pitch", "Loop"), sock("Done")),
        "LogicNativeStopSound": ACTION_COND_DONE,
        "LogicNativePauseSound": ACTION_COND_DONE,
        "LogicNativeResumeSound": ACTION_COND_DONE,
        "LogicNativePrint": (sock("Condition", "Value"), sock("Done")),
        "LogicNativeSendEvent": (sock("Condition", "Subject", "Content", "Messenger", "Target"), sock("Done")),
        "LogicNativeReceiveEvent": (sock("Subject", "Target"), sock("Out", "Received", "Content", "Messenger")),
        "LogicNativeInstallLogicTree": (sock("Condition", "Object", "Tree"), sock("Done")),
        "LogicNativeRunLogicTree": (sock("Condition", "Object", "Tree Name"), sock("Done")),
        "LogicNativeLogicTreeStatus": (sock("Object", "Tree Name"), sock("Running", "Stopped")),
        "LogicNativeLoadScene": SCENE_ACTION,
        "LogicNativeSetScene": SCENE_ACTION,
        "LogicNativeSetGravity": (sock("Condition", "Gravity"), sock("Done")),
        "LogicNativeSetTimescale": (sock("Condition", "Timescale"), sock("Done")),
        "LogicNativeSetPhysics": (sock("Condition", "Object", "Active"), sock("Done")),
        "LogicNativeSetDynamics": (sock("Condition", "Object", "Enabled"), sock("Done")),
        "LogicNativeRebuildCollisionShape": (sock("Condition", "Object"), sock("Done")),
        "LogicNativeGetRigidBodyAttribute": (
            sock("Object"),
            sock(
                "Valid",
                "Value",
                "Linear Damping",
                "Angular Damping",
                "Enabled",
                "Allow Sleeping",
                "Lock Translation",
                "Lock Rotation",
            ),
        ),
        "LogicNativeSetRigidBodyAttribute": (
            sock(
                "Condition",
                "Object",
                "Value",
                "Linear Damping",
                "Angular Damping",
                "Min Linear",
                "Max Linear",
                "Min Angular",
                "Max Angular",
                "Enabled",
                "Allow Sleeping",
                "Wake",
                "Lock Translation",
                "Lock Rotation",
            ),
            sock("Done"),
        ),
        "LogicNativeSetMaterialSlot": (sock("Condition", "Object", "Material", "Slot"), sock("Done")),
        "LogicNativeSetMaterialParameter": (
            sock(
                "Condition",
                "Object",
                "Slot",
                "Material",
                "Node Name",
                "Socket",
                "Value",
                "Float Value",
                "Integer Value",
                "Boolean Value",
                "Vector Value",
                "Color Value",
            ),
            sock("Done"),
        ),
        "LogicNativeTime": ({}, sock("Time")),
        "LogicNativeDeltaFactor": ({}, sock("Factor")),
        "LogicNativeVehicleSetAttributes": (
            sock(
                "Condition",
            "Collider",
                "Wheels",
                "Suspension",
                "Suspension Value",
                "Stiffness",
                "Stiffness Value",
                "Damping",
                "Damping Value",
                "Friction",
                "Friction Value",
            ),
            sock("Done"),
        ),
        "LogicNativeEmptyList": (sock("Length"), sock("List")),
        "LogicNativeEmptyDict": ({}, sock("Dictionary")),
        "LogicNativeMakeList": (sock("Item A", "Item B", "Item C"), sock("List")),
        "LogicNativeListGetItem": (sock("List", "Index"), sock("Value")),
        "LogicNativeListExtend": (sock("List A", "List B"), sock("List")),
        "LogicNativeMakeDict": (sock("Key", "Value"), sock("Dictionary")),
        "LogicNativeDictGetKey": (sock("Dictionary", "Key"), sock("Value")),
        "LogicNativeDictGetKeys": (sock("Dictionary"), sock("Keys")),
        "LogicNativeLoop": (sock("Condition", "Count"), sock("Loop", "Index", "Value")),
        "LogicNativeLoopFromList": (sock("Condition", "List"), sock("Loop", "Index", "Value")),
    }

    for idname, label in entries.items():
        if idname in overrides:
            continue
        if idname.startswith("LogicNativeApply"):
            if idname == "LogicNativeApplyRotation":
                overrides[idname] = (
                    sock("Condition", "Object", "Rotation", "Local"),
                    sock("Done"),
                )
            else:
                overrides[idname] = APPLY_IMPULSE if "Impulse" in idname else APPLY_M
        elif idname == "LogicNativeGetBonePoseScale":
            overrides[idname] = (sock("Object", "Bone Name"), sock("Scale"))
        elif idname == "LogicNativeGetBonePoseTransform":
            overrides[idname] = (
                sock("Object", "Bone Name"),
                sock("Location", "Rotation", "Scale"),
            )
        elif idname.startswith("LogicNativeGetBone") and idname != "LogicNativeGetBoneLength":
            out = "Rotation" if "Orientation" in label or "Rotation" in label else "Position"
            overrides[idname] = (sock("Object", "Bone Name"), sock(out))
        elif idname == "LogicNativeSetBonePoseLocation":
            overrides[idname] = (sock("Condition", "Object", "Bone Name", "Location"), sock("Done"))
        elif idname == "LogicNativeSetBonePoseRotation":
            overrides[idname] = (sock("Condition", "Object", "Bone Name", "Rotation"), sock("Done"))
        elif idname == "LogicNativeSetBonePoseScale":
            overrides[idname] = (sock("Condition", "Object", "Bone Name", "Scale"), sock("Done"))
        elif idname == "LogicNativeSetBonePoseTransform":
            overrides[idname] = (
                sock("Condition", "Object", "Bone Name", "Location", "Rotation"),
                sock("Done"),
            )
        elif idname == "LogicNativeSetBoneConstraintInfluence":
            overrides[idname] = (
                sock("Condition", "Object", "Bone Name", "Constraint Name", "Influence"),
                sock("Done"),
            )
        elif idname.startswith("LogicNativeSetBone"):
            overrides[idname] = (sock("Condition", "Object", "Bone Name", "Vector"), sock("Done"))

    lines = [
        "# SPDX-FileCopyrightText: 2026 UPBGE Authors",
        "# SPDX-License-Identifier: GPL-2.0-or-later",
        "# Auto-generated by generate_logic_node_parity_overrides.py",
        "",
        "PARITY_SOCKET_OVERRIDES: dict[str, tuple[dict[str, str], dict[str, str]]] = {",
    ]
    for key in sorted(overrides):
        inp, out = overrides[key]
        lines.append(f'    "{key}": (')
        lines.append(f"        {inp!r},")
        lines.append(f"        {out!r},")
        lines.append("    ),")
    lines.append("}")
    lines.append("")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {len(overrides)} entries to {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
